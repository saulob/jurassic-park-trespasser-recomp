#include "linux_gdi32.h"

#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "linux_log.h"
#include "linux_screen.h"
#include "linux_text.h"
#include "linux_user32.h"
#include "linux_window.h"

#define MAX_OBJECTS 512
#define MAX_SURFACES 8
#define STOCK_COUNT 21
#define GEN_MAX 0xFFFFFu

enum { K_NONE = 0, K_DC, K_BITMAP, K_BRUSH, K_PEN, K_FONT, K_REGION, K_PALETTE };
enum { SRCCOPY = 0x00CC0020, BLACKNESS = 0x00000042, WHITENESS = 0x00FF0062, PATCOPY = 0x00F00021 };
enum { BI_RGB = 0, BI_BITFIELDS = 3 };
enum { F565, F555, F8888, F24, F8 };

typedef struct {
    int fmt, w, h;
    uint8_t* base; /* the top row */
    int pitch;     /* bytes from a row to the next, negative for a bottom-up DIB */
    const uint32_t* palette;
} surf_t;

typedef struct {
    uint32_t guest_bits;
    int w, h, bpp, fmt, bottom_up, pitch, external;
    uint32_t palette[256];
} bitmap_t;

typedef struct {
    int memory;
    uint32_t hwnd;
    int surface;
    uint32_t bitmap, brush, pen, font, region;
    uint32_t text_color, bk_color;
    int bk_mode, map_mode, stretch_mode, align, org_x, org_y, pos_x, pos_y;
} dc_t;

typedef struct {
    uint32_t color;
    int style;
} brush_t;

typedef struct {
    float scale;
    int bold;
} font_t;

typedef struct {
    int used, kind, stock;
    uint32_t generation;
    union {
        dc_t dc;
        bitmap_t bitmap;
        brush_t brush;
        font_t font;
        int32_t rect[4];
    } u;
} object_t;

typedef struct {
    int used;
    uint32_t hwnd;
    int w, h;
    uint16_t* pixels;
} surface_t;

static object_t s_obj[MAX_OBJECTS];
static surface_t s_surf[MAX_SURFACES];
static uint32_t s_generation = 1;
static int s_warned_rop;
static unsigned s_blits;

static uint32_t token_of(int slot) { return (s_obj[slot].generation << 12) | (uint32_t)(slot + 1); }

static object_t* object_of(uint32_t token, int kind) {
    uint32_t slot = token & 0xFFFu;
    object_t* o;
    if (!slot || slot > MAX_OBJECTS) return NULL;
    o = &s_obj[slot - 1];
    return o->used && o->generation == (token >> 12) && (!kind || o->kind == kind) ? o : NULL;
}

static object_t* alloc_object(int kind, uint32_t* token) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (s_obj[i].used) continue;
        memset(&s_obj[i], 0, sizeof s_obj[i]);
        s_obj[i].used = 1;
        s_obj[i].kind = kind;
        s_obj[i].generation = s_generation++ & GEN_MAX;
        if (!s_obj[i].generation) s_obj[i].generation = s_generation++;
        *token = token_of(i);
        return &s_obj[i];
    }
    return NULL;
}

static uint32_t s_stock[STOCK_COUNT];

void hle_gdi32_init(void) {
    static const uint32_t gray[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000, 0 };
    memset(s_obj, 0, sizeof s_obj);
    memset(s_surf, 0, sizeof s_surf);
    memset(s_stock, 0, sizeof s_stock);
    s_generation = 1;
    s_warned_rop = 0;
    s_blits = 0;
    for (int i = 0; i < STOCK_COUNT; i++) {
        uint32_t t;
        int kind = i <= 5 ? K_BRUSH : i <= 8 ? K_PEN : i == 15 ? K_PALETTE : K_FONT;
        object_t* o = alloc_object(kind, &t);
        if (!o) continue;
        o->stock = 1;
        if (kind == K_BRUSH) o->u.brush.color = gray[i], o->u.brush.style = i == 5 ? 1 : 0;
        if (kind == K_PEN) o->u.brush.color = i == 6 ? 0xFFFFFF : 0, o->u.brush.style = i == 8 ? 5 : 0;
        if (kind == K_FONT) o->u.font.scale = lx_text_scale(16);
        s_stock[i] = t;
    }
}

void hle_gdi32_shutdown(void) {
    for (int i = 0; i < MAX_SURFACES; i++) {
        free(s_surf[i].pixels);
        s_surf[i].pixels = NULL;
        s_surf[i].used = 0;
    }
}

unsigned lx_gdi_blit_count(void) { return s_blits; }

unsigned lx_gdi_object_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_OBJECTS; i++) n += (unsigned)(s_obj[i].used && !s_obj[i].stock);
    return n;
}

/* ---- pixels ---- */

static uint32_t colorref_to_xrgb(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

static uint32_t to_xrgb(const surf_t* s, const uint8_t* p) {
    switch (s->fmt) {
    case F565: {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        uint32_t r = (uint32_t)(v >> 11) & 31, g = (uint32_t)(v >> 5) & 63, b = (uint32_t)v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    case F555: {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        uint32_t r = (uint32_t)(v >> 10) & 31, g = (uint32_t)(v >> 5) & 31, b = (uint32_t)v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
    case F24: return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    case F8: return s->palette ? s->palette[p[0]] : (uint32_t)p[0] * 0x010101u;
    default: return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    }
}

static void from_xrgb(const surf_t* s, uint8_t* p, uint32_t c) {
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    switch (s->fmt) {
    case F565: {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
        break;
    }
    case F555: {
        uint16_t v = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
        break;
    }
    case F24: p[0] = (uint8_t)b, p[1] = (uint8_t)g, p[2] = (uint8_t)r; break;
    case F8: p[0] = (uint8_t)((r * 30 + g * 59 + b * 11) / 100); break;
    default: p[0] = (uint8_t)b, p[1] = (uint8_t)g, p[2] = (uint8_t)r, p[3] = 0;
    }
}

static int bytes_per_pixel(int fmt) { return fmt == F8888 ? 4 : fmt == F24 ? 3 : fmt == F8 ? 1 : 2; }

static int dc_surface(const dc_t* dc, surf_t* s) {
    memset(s, 0, sizeof *s);
    if (dc->memory) {
        const object_t* bo = object_of(dc->bitmap, K_BITMAP);
        const bitmap_t* b;
        uint8_t* mem;
        if (!bo) return 0;
        b = &bo->u.bitmap;
        mem = (uint8_t*)gm_ptr_checked(b->guest_bits, (uint32_t)(b->pitch * b->h), 1);
        if (!mem) return 0;
        s->fmt = b->fmt, s->w = b->w, s->h = b->h;
        s->pitch = b->bottom_up ? -b->pitch : b->pitch;
        s->base = b->bottom_up ? mem + (size_t)(b->h - 1) * (size_t)b->pitch : mem;
        s->palette = b->bpp == 8 ? b->palette : NULL;
        return 1;
    }
    if (dc->surface < 0 || !s_surf[dc->surface].used) return 0;
    s->fmt = F565, s->w = s_surf[dc->surface].w, s->h = s_surf[dc->surface].h;
    s->base = (uint8_t*)s_surf[dc->surface].pixels, s->pitch = s->w * 2;
    return 1;
}

static dc_t* dc_of(uint32_t hdc) {
    object_t* o = object_of(hdc, K_DC);
    return o ? &o->u.dc : NULL;
}

/* ---- window device contexts ---- */

static int surface_for(uint32_t hwnd, int w, int h) {
    int free_slot = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (s_surf[i].used && s_surf[i].hwnd == hwnd) {
            if (s_surf[i].w != w || s_surf[i].h != h) {
                uint16_t* p = (uint16_t*)calloc((size_t)w * (size_t)h, 2);
                if (!p) return -1;
                free(s_surf[i].pixels);
                s_surf[i].pixels = p, s_surf[i].w = w, s_surf[i].h = h;
            }
            return i;
        }
        if (!s_surf[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || w <= 0 || h <= 0) return -1;
    s_surf[free_slot].pixels = (uint16_t*)calloc((size_t)w * (size_t)h, 2);
    if (!s_surf[free_slot].pixels) return -1;
    s_surf[free_slot].used = 1, s_surf[free_slot].hwnd = hwnd, s_surf[free_slot].w = w, s_surf[free_slot].h = h;
    return free_slot;
}

static void init_dc(dc_t* dc) {
    dc->brush = s_stock[0];
    dc->pen = s_stock[7];
    dc->font = s_stock[13];
    dc->text_color = 0, dc->bk_color = 0xFFFFFF, dc->bk_mode = 2, dc->map_mode = 1, dc->stretch_mode = 1;
    dc->surface = -1;
}

uint32_t lx_gdi_window_dc(uint32_t hwnd, int client_w, int client_h) {
    uint32_t t;
    object_t* o;
    int s = surface_for(hwnd, client_w, client_h);
    if (s < 0) return 0;
    o = alloc_object(K_DC, &t);
    if (!o) return 0;
    init_dc(&o->u.dc);
    o->u.dc.hwnd = hwnd;
    o->u.dc.surface = s;
    return t;
}

int lx_gdi_release_dc(uint32_t hdc) {
    object_t* o = object_of(hdc, K_DC);
    if (!o) return 0;
    o->used = 0;
    return 1;
}

void lx_gdi_window_gone(uint32_t hwnd) {
    for (int i = 0; i < MAX_SURFACES; i++)
        if (s_surf[i].used && s_surf[i].hwnd == hwnd) {
            free(s_surf[i].pixels);
            memset(&s_surf[i], 0, sizeof s_surf[i]);
        }
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (s_obj[i].used && s_obj[i].kind == K_DC && !s_obj[i].u.dc.memory && s_obj[i].u.dc.hwnd == hwnd) s_obj[i].used = 0;
}

void lx_gdi_present_window(uint32_t hwnd) {
    lx_frame_t f;
    if (hwnd != lx_user32_main_window()) return;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!s_surf[i].used || s_surf[i].hwnd != hwnd) continue;
        f.pixels = s_surf[i].pixels, f.width = s_surf[i].w, f.height = s_surf[i].h, f.pitch = s_surf[i].w * 2;
        f.format = LX_PF_RGB565, f.generation = 0;
        lx_screen_present(&f);
        return;
    }
}

uint32_t lx_gdi_surface_dc(uint32_t guest_bits, int width, int height, int pitch, int bpp, uint32_t green_mask) {
    uint32_t bitmap_token, dc_token;
    object_t* bo;
    object_t* dc;
    if (!guest_bits || width <= 0 || height <= 0 || pitch <= 0 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return 0;
    bo = alloc_object(K_BITMAP, &bitmap_token);
    if (!bo) return 0;
    bo->u.bitmap.guest_bits = guest_bits, bo->u.bitmap.w = width, bo->u.bitmap.h = height, bo->u.bitmap.bpp = bpp;
    bo->u.bitmap.pitch = pitch, bo->u.bitmap.external = 1;
    bo->u.bitmap.fmt = bpp == 16 ? (green_mask == 0x07E0 ? F565 : F555) : bpp == 32 ? F8888 : bpp == 24 ? F24 : F8;
    for (uint32_t i = 0; i < 256; i++) bo->u.bitmap.palette[i] = i * 0x010101u;
    dc = alloc_object(K_DC, &dc_token);
    if (!dc) {
        bo->used = 0;
        return 0;
    }
    init_dc(&dc->u.dc);
    dc->u.dc.memory = 1;
    dc->u.dc.bitmap = bitmap_token;
    return dc_token;
}

int lx_gdi_release_surface_dc(uint32_t hdc) {
    object_t* dc = object_of(hdc, K_DC);
    object_t* bo;
    if (!dc || !dc->u.dc.memory) return 0;
    bo = object_of(dc->u.dc.bitmap, K_BITMAP);
    if (bo && bo->u.bitmap.external) bo->used = 0;
    dc->used = 0;
    return 1;
}

/* ---- drawing ---- */

static uint32_t brush_color(uint32_t brush) {
    const object_t* o = object_of(brush, K_BRUSH);
    if (o) return o->u.brush.color;
    switch (brush > 0 ? brush - 1 : 0) {
    case 5: return 0xFFFFFF;
    case 15: return 0xC0C0C0;
    default: return 0;
    }
}

static void fill_surface(const surf_t* s, int x0, int y0, int x1, int y1, uint32_t xrgb) {
    uint8_t first[4];
    int bpp = bytes_per_pixel(s->fmt);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x0 >= x1 || y0 >= y1) return;
    from_xrgb(s, first, xrgb);
    for (int y = y0; y < y1; y++) {
        uint8_t* row = s->base + (ptrdiff_t)y * s->pitch + (ptrdiff_t)x0 * bpp;
        for (int x = x0; x < x1; x++, row += bpp) memcpy(row, first, (size_t)bpp);
    }
}

int lx_gdi_fill_rect(uint32_t hdc, const int32_t* rect, uint32_t brush) {
    dc_t* dc = dc_of(hdc);
    surf_t s;
    if (!dc || !dc_surface(dc, &s)) return 0;
    fill_surface(&s, rect[0], rect[1], rect[2], rect[3], colorref_to_xrgb(brush_color(brush)));
    return 1;
}

static int blit(dc_t* dst, int dx, int dy, int dw, int dh, const dc_t* src, int sx, int sy, int sw, int sh, uint32_t rop) {
    surf_t d, s;
    if (!dc_surface(dst, &d)) return 0;
    if (rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY) {
        uint32_t c = rop == BLACKNESS ? 0 : rop == WHITENESS ? 0xFFFFFF : colorref_to_xrgb(brush_color(dst->brush));
        fill_surface(&d, dx, dy, dx + dw, dy + dh, c);
        return 1;
    }
    if (rop != SRCCOPY && !s_warned_rop) {
        s_warned_rop = 1;
        lx_log("Linux GDI", "raster operation 0x%08X drawn as SRCCOPY", rop);
    }
    if (!src || !dc_surface(src, &s)) return 0;
    for (int y = 0; y < dh; y++) {
        int ty = dy + y, fy = sy + (sh == dh ? y : y * sh / dh);
        if (ty < 0 || ty >= d.h || fy < 0 || fy >= s.h) continue;
        if (sw == dw && s.fmt == d.fmt) {
            int x0 = dx < 0 ? -dx : 0, x1 = dx + dw > d.w ? d.w - dx : dw;
            int bpp = bytes_per_pixel(d.fmt);
            if (sx + x0 < 0) x0 = -sx;
            if (sx + x1 > s.w) x1 = s.w - sx;
            if (x0 < x1) memcpy(d.base + (ptrdiff_t)ty * d.pitch + (ptrdiff_t)(dx + x0) * bpp,
                                 s.base + (ptrdiff_t)fy * s.pitch + (ptrdiff_t)(sx + x0) * bpp, (size_t)(x1 - x0) * (size_t)bpp);
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int tx = dx + x, fx = sx + (sw == dw ? x : x * sw / dw);
            if (tx < 0 || tx >= d.w || fx < 0 || fx >= s.w) continue;
            from_xrgb(&d, d.base + (ptrdiff_t)ty * d.pitch + (ptrdiff_t)tx * bytes_per_pixel(d.fmt),
                      to_xrgb(&s, s.base + (ptrdiff_t)fy * s.pitch + (ptrdiff_t)fx * bytes_per_pixel(s.fmt)));
        }
    }
    return 1;
}

static void window_drawn(const dc_t* dc) {
    s_blits++;
    if (!dc->memory) lx_gdi_present_window(dc->hwnd);
}

static uint64_t g_BitBlt(const uint32_t* a, void* u) {
    dc_t* dst = dc_of(a[0]);
    const dc_t* src = a[5] ? dc_of(a[5]) : NULL;
    int w = (int32_t)a[3], h = (int32_t)a[4];
    (void)u;
    if (!dst || w <= 0 || h <= 0) return 0;
    if (!blit(dst, (int32_t)a[1] - dst->org_x, (int32_t)a[2] - dst->org_y, w, h, src, (int32_t)a[6], (int32_t)a[7], w, h, a[8])) return 0;
    window_drawn(dst);
    return 1;
}

static uint64_t g_StretchBlt(const uint32_t* a, void* u) {
    dc_t* dst = dc_of(a[0]);
    const dc_t* src = a[5] ? dc_of(a[5]) : NULL;
    int dw = (int32_t)a[3], dh = (int32_t)a[4], sw = (int32_t)a[8], sh = (int32_t)a[9];
    (void)u;
    if (!dst || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return 0;
    if (!blit(dst, (int32_t)a[1] - dst->org_x, (int32_t)a[2] - dst->org_y, dw, dh, src, (int32_t)a[6], (int32_t)a[7], sw, sh, a[10])) return 0;
    window_drawn(dst);
    return 1;
}

/* ---- objects ---- */

static uint64_t g_GetStockObject(const uint32_t* a, void* u) {
    (void)u;
    return a[0] < STOCK_COUNT ? s_stock[a[0]] : 0;
}

static uint64_t g_CreateSolidBrush(const uint32_t* a, void* u) {
    uint32_t t;
    object_t* o = alloc_object(K_BRUSH, &t);
    (void)u;
    if (!o) return 0;
    o->u.brush.color = a[0];
    return t;
}

static uint64_t g_CreatePen(const uint32_t* a, void* u) {
    uint32_t t;
    object_t* o = alloc_object(K_PEN, &t);
    (void)u;
    if (!o) return 0;
    o->u.brush.style = (int)a[0], o->u.brush.color = a[2];
    return t;
}

static uint64_t make_font(int height, int weight) {
    uint32_t t;
    object_t* o = alloc_object(K_FONT, &t);
    if (!o) return 0;
    o->u.font.scale = lx_text_scale(height);
    o->u.font.bold = weight >= 600;
    return t;
}

static uint64_t g_CreateFontA(const uint32_t* a, void* u) {
    (void)u;
    return make_font((int32_t)a[0], (int32_t)a[4]);
}

static uint64_t g_CreateFontIndirectA(const uint32_t* a, void* u) {
    uint32_t lf[6];
    (void)u;
    if (!gp_copy_in(lf, a[0], sizeof lf)) return 0;
    return make_font((int32_t)lf[0], (int32_t)lf[4]);
}

static uint64_t g_CreateRectRgn(const uint32_t* a, void* u) {
    uint32_t t;
    object_t* o = alloc_object(K_REGION, &t);
    (void)u;
    if (!o) return 0;
    for (int i = 0; i < 4; i++) o->u.rect[i] = (int32_t)a[i];
    return t;
}

static uint64_t g_DeleteObject(const uint32_t* a, void* u) {
    object_t* o = object_of(a[0], 0);
    (void)u;
    if (!o) return 0;
    if (o->stock) return 1;
    if (o->kind == K_BITMAP && o->u.bitmap.guest_bits && !o->u.bitmap.external) gm_heap_free(o->u.bitmap.guest_bits);
    o->used = 0;
    return 1;
}

static uint64_t g_CreateCompatibleDC(const uint32_t* a, void* u) {
    uint32_t t;
    object_t* o;
    (void)a;
    (void)u;
    o = alloc_object(K_DC, &t);
    if (!o) return 0;
    init_dc(&o->u.dc);
    o->u.dc.memory = 1;
    return t;
}

static uint64_t g_DeleteDC(const uint32_t* a, void* u) {
    object_t* o = object_of(a[0], K_DC);
    (void)u;
    if (!o) return 0;
    o->used = 0;
    return 1;
}

static uint64_t g_SelectObject(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    const object_t* o = object_of(a[1], 0);
    uint32_t old = 0;
    (void)u;
    if (!dc || !o) return 0;
    switch (o->kind) {
    case K_BITMAP: old = dc->bitmap, dc->bitmap = a[1]; break;
    case K_BRUSH: old = dc->brush, dc->brush = a[1]; break;
    case K_PEN: old = dc->pen, dc->pen = a[1]; break;
    case K_FONT: old = dc->font, dc->font = a[1]; break;
    case K_REGION: old = dc->region, dc->region = a[1]; return o ? 2 : 0;
    default: return 0;
    }
    return old;
}

static uint64_t g_SelectClipRgn(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    (void)u;
    if (!dc) return 0;
    dc->region = a[1];
    return a[1] ? 2 : 1;
}

static uint64_t g_CreateDIBSection(const uint32_t* a, void* u) {
    uint32_t bih[10], t, bits;
    uint32_t masks[3] = { 0, 0, 0 };
    bitmap_t* b;
    object_t* o;
    int w, h, bpp, pitch;
    (void)u;
    if (!gp_copy_in(bih, a[1], sizeof bih)) return 0;
    w = (int32_t)bih[1], h = (int32_t)bih[2], bpp = (int)(bih[3] >> 16);
    if (w <= 0 || h == 0 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return 0;
    pitch = ((w * bpp + 31) / 32) * 4;
    bits = gm_heap_alloc((uint32_t)pitch * (uint32_t)(h < 0 ? -h : h), 1);
    if (!bits) return 0;
    o = alloc_object(K_BITMAP, &t);
    if (!o) {
        gm_heap_free(bits);
        return 0;
    }
    b = &o->u.bitmap;
    b->guest_bits = bits, b->w = w, b->h = h < 0 ? -h : h, b->bpp = bpp, b->pitch = pitch, b->bottom_up = h > 0;
    if (bpp == 16) {
        b->fmt = F555;
        if (bih[4] == BI_BITFIELDS && gp_copy_in(masks, a[1] + 40, sizeof masks) && masks[1] == 0x07E0) b->fmt = F565;
    } else b->fmt = bpp == 32 ? F8888 : bpp == 24 ? F24 : F8;
    if (bpp == 8) {
        uint32_t entries = bih[8] ? bih[8] : 256, quad;
        for (uint32_t i = 0; i < 256; i++) b->palette[i] = i * 0x010101u;
        for (uint32_t i = 0; i < entries && i < 256; i++)
            if (gp_read_u32(a[1] + bih[0] + 4 * i, &quad)) b->palette[i] = quad & 0xFFFFFFu;
    }
    if (a[3]) gp_write_u32(a[3], bits);
    lx_log("Linux GDI", "framebuffer DIB section %dx%d %d bpp (%s) at guest 0x%08X", b->w, b->h, bpp,
           b->fmt == F565 ? "RGB565" : b->fmt == F555 ? "RGB555" : "other", bits);
    return t;
}

/* ---- device context state ---- */

#define DC_SETTER(name, field, type) \
    static uint64_t g_##name(const uint32_t* a, void* u) { \
        dc_t* dc = dc_of(a[0]); \
        type old; \
        (void)u; \
        if (!dc) return 0; \
        old = (type)dc->field; \
        dc->field = (type)a[1]; \
        return (uint64_t)(uint32_t)old; \
    }

DC_SETTER(SetBkMode, bk_mode, int)
DC_SETTER(SetTextColor, text_color, uint32_t)
DC_SETTER(SetBkColor, bk_color, uint32_t)
DC_SETTER(SetMapMode, map_mode, int)
DC_SETTER(SetStretchBltMode, stretch_mode, int)
DC_SETTER(SetTextAlign, align, int)

static uint64_t g_GetTextColor(const uint32_t* a, void* u) {
    const dc_t* dc = dc_of(a[0]);
    (void)u;
    return dc ? dc->text_color : 0xFFFFFFFFu;
}

static uint64_t g_SetSystemPaletteUse(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 1;
}

static uint64_t g_OffsetWindowOrgEx(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    int32_t old[2];
    (void)u;
    if (!dc) return 0;
    old[0] = dc->org_x, old[1] = dc->org_y;
    dc->org_x += (int32_t)a[1], dc->org_y += (int32_t)a[2];
    if (a[3]) gp_copy_out(a[3], old, sizeof old);
    return 1;
}

static uint64_t g_MoveToEx(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    int32_t old[2];
    (void)u;
    if (!dc) return 0;
    old[0] = dc->pos_x, old[1] = dc->pos_y;
    dc->pos_x = (int32_t)a[1], dc->pos_y = (int32_t)a[2];
    if (a[3]) gp_copy_out(a[3], old, sizeof old);
    return 1;
}

static uint64_t g_GetDeviceCaps(const uint32_t* a, void* u) {
    lx_display_mode_t m = lx_screen_mode();
    (void)u;
    switch (a[1]) {
    case 2: return 1;
    case 4: return (uint32_t)(m.width * 254 / 960);
    case 6: return (uint32_t)(m.height * 254 / 960);
    case 8: return (uint32_t)m.width;
    case 10: return (uint32_t)m.height;
    case 12: return (uint32_t)m.bpp;
    case 14: return 1;
    case 24: return m.bpp > 8 ? 0xFFFFFFFFu : (uint32_t)(1 << m.bpp);
    case 36: return 1;
    case 38: return 0x2889;
    case 88: case 90: return 96;
    case 104: return m.bpp > 8 ? 0 : (uint32_t)(1 << m.bpp);
    default: return 0;
    }
}

/* ---- text ---- */

static const font_t* dc_font(const dc_t* dc) {
    const object_t* o = object_of(dc->font, K_FONT);
    static font_t def;
    if (o) return &o->u.font;
    def.scale = lx_text_scale(16);
    return &def;
}

static uint64_t g_GetTextMetricsA(const uint32_t* a, void* u) {
    const dc_t* dc = dc_of(a[0]);
    lx_text_metrics_t m;
    uint32_t tm[14];
    (void)u;
    if (!dc) return 0;
    lx_text_metrics(dc_font(dc)->scale, &m);
    memset(tm, 0, sizeof tm);
    tm[0] = (uint32_t)m.height, tm[1] = (uint32_t)m.ascent, tm[2] = (uint32_t)m.descent;
    tm[5] = (uint32_t)m.avg_width, tm[6] = (uint32_t)m.max_width;
    tm[7] = dc_font(dc)->bold ? 700 : 400;
    tm[9] = tm[10] = 96;
    tm[11] = 0x20u | (0xFFu << 8) | (0x3Fu << 16) | (0x20u << 24);
    return gp_copy_out(a[1], tm, sizeof tm);
}

static const char* text_arg(uint32_t va, int count) {
    if (count < 0) return NULL;
    return count ? (const char*)gm_ptr_checked(va, (uint32_t)count, 0) : "";
}

static uint64_t g_GetTextExtentPointA(const uint32_t* a, void* u) {
    const dc_t* dc = dc_of(a[0]);
    const font_t* f;
    lx_text_metrics_t m;
    const char* s = text_arg(a[1], (int32_t)a[2]);
    int32_t size[2];
    (void)u;
    if (!dc || !s) return 0;
    f = dc_font(dc);
    lx_text_metrics(f->scale, &m);
    size[0] = lx_text_width(f->scale, s, (int)a[2], f->bold), size[1] = m.height;
    return gp_copy_out(a[3], size, sizeof size);
}

typedef struct {
    surf_t s;
    uint32_t fg;
    int clip[4];
} text_ctx_t;

static void text_pixel(void* c, int x, int y, int coverage) {
    text_ctx_t* t = (text_ctx_t*)c;
    uint8_t* p;
    uint32_t bg, out = 0;
    if (x < t->clip[0] || x >= t->clip[2] || y < t->clip[1] || y >= t->clip[3]) return;
    p = t->s.base + (ptrdiff_t)y * t->s.pitch + (ptrdiff_t)x * bytes_per_pixel(t->s.fmt);
    bg = to_xrgb(&t->s, p);
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t f = (t->fg >> shift) & 255, b = (bg >> shift) & 255;
        out |= ((f * (uint32_t)coverage + b * (uint32_t)(255 - coverage) + 127) / 255) << shift;
    }
    from_xrgb(&t->s, p, out);
}

static void text_clip(const surf_t* s, const int32_t* rect, int* clip) {
    clip[0] = rect && rect[0] > 0 ? rect[0] : 0;
    clip[1] = rect && rect[1] > 0 ? rect[1] : 0;
    clip[2] = rect && rect[2] < s->w ? rect[2] : s->w;
    clip[3] = rect && rect[3] < s->h ? rect[3] : s->h;
}

/* One line with its top-left corner at (x, y) in device space. */
static void draw_run(const dc_t* dc, text_ctx_t* t, int x, int y, const char* s, int n) {
    const font_t* f = dc_font(dc);
    lx_text_metrics_t m;
    int pen = x;
    lx_text_metrics(f->scale, &m);
    if (dc->bk_mode == 2) fill_surface(&t->s, x, y, x + lx_text_width(f->scale, s, n, f->bold), y + m.height, colorref_to_xrgb(dc->bk_color));
    for (int i = 0; i < n; i++) {
        lx_text_glyph(f->scale, (uint8_t)s[i], f->bold, pen, y + m.ascent, text_pixel, t);
        pen += lx_text_advance(f->scale, (uint8_t)s[i], f->bold);
    }
}

static uint64_t g_TextOutA(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    text_ctx_t t;
    lx_text_metrics_t m;
    const font_t* f;
    const char* s = text_arg(a[3], (int32_t)a[4]);
    int x = (int32_t)a[1], y = (int32_t)a[2], n = (int32_t)a[4], w;
    (void)u;
    if (!dc || !s || !dc_surface(dc, &t.s)) return 0;
    f = dc_font(dc);
    lx_text_metrics(f->scale, &m);
    w = lx_text_width(f->scale, s, n, f->bold);
    if (dc->align & 1) x = dc->pos_x, y = dc->pos_y;
    if ((dc->align & 6) == 6) x -= w / 2;
    else if (dc->align & 2) x -= w;
    if ((dc->align & 24) == 24) y -= m.ascent;
    else if (dc->align & 8) y -= m.height;
    t.fg = colorref_to_xrgb(dc->text_color);
    text_clip(&t.s, NULL, t.clip);
    draw_run(dc, &t, x - dc->org_x, y - dc->org_y, s, n);
    if (dc->align & 1) dc->pos_x = x + w;
    window_drawn(dc);
    return 1;
}

enum {
    DT_CENTER = 0x1, DT_RIGHT = 0x2, DT_VCENTER = 0x4, DT_BOTTOM = 0x8, DT_WORDBREAK = 0x10, DT_SINGLELINE = 0x20, DT_EXPANDTABS = 0x40,
    DT_NOCLIP = 0x100, DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800, DT_END_ELLIPSIS = 0x8000
};

enum { LINE_CHARS = 1024, MAX_LINES = 64 };

typedef struct {
    char text[LINE_CHARS];
    int len, width;
} line_t;

static int prepare_text(const char* src, int n, unsigned flags, char* out, int cap) {
    int len = 0;
    for (int i = 0; i < n && len < cap - 8; i++) {
        char c = src[i];
        if (!(flags & DT_NOPREFIX) && c == '&') {
            if (i + 1 < n && src[i + 1] == '&') out[len++] = '&', i++;
            continue;
        }
        if (c == '\t' && (flags & DT_EXPANDTABS)) {
            do out[len++] = ' ';
            while (len % 8 && len < cap - 8);
            continue;
        }
        out[len++] = c;
    }
    return len;
}

/* Breaks at line feeds, and at the last space (or inside a word that alone is too wide) once a wrapped line no longer fits. */
static int layout_lines(const font_t* f, const char* s, int n, unsigned flags, int avail, line_t* lines, int max_lines) {
    int count = 0, start = 0;
    int wrap = (flags & DT_WORDBREAK) && !(flags & DT_SINGLELINE);
    while (count < max_lines) {
        int end = start, last_space = -1, next;
        for (; end < n; end++) {
            if (s[end] == '\n' && !(flags & DT_SINGLELINE)) break;
            if (s[end] == ' ') last_space = end;
            if (wrap && end > start && lx_text_width(f->scale, s + start, end - start + 1, f->bold) > avail) {
                if (last_space > start) end = last_space;
                break;
            }
        }
        next = end < n && (s[end] == ' ' || s[end] == '\n') ? end + 1 : end;
        lines[count].len = 0;
        for (int i = start; i < end; i++)
            if (s[i] != '\r' && s[i] != '\n' && lines[count].len < LINE_CHARS) lines[count].text[lines[count].len++] = s[i];
        lines[count].width = lx_text_width(f->scale, lines[count].text, lines[count].len, f->bold);
        count++;
        if (next >= n) break;
        start = next;
    }
    return count;
}

static void end_ellipsis(const font_t* f, line_t* l, int avail) {
    int dots = lx_text_width(f->scale, "...", 3, f->bold);
    if (l->width <= avail) return;
    while (l->len > 0 && lx_text_width(f->scale, l->text, l->len, f->bold) + dots > avail) l->len--;
    memcpy(l->text + l->len, "...", 3);
    l->len += 3;
    l->width = lx_text_width(f->scale, l->text, l->len, f->bold);
}

int lx_gdi_draw_text(uint32_t hdc, uint32_t text_va, int count, int32_t* rect, uint32_t flags, const int32_t* margins) {
    static line_t lines[MAX_LINES];
    static char prepared[LINE_CHARS];
    dc_t* dc = dc_of(hdc);
    text_ctx_t t;
    lx_text_metrics_t m;
    const font_t* f;
    const char* src;
    int n, nlines, total, avail, widest = 0, y, ml = margins ? margins[0] : 0, mr = margins ? margins[1] : 0;
    if (!dc || !rect || !dc_surface(dc, &t.s)) return 0;
    if (count < 0) {
        src = gp_string(text_va, LINE_CHARS);
        if (!src) return 0;
        count = (int)strlen(src);
    } else if (!(src = text_arg(text_va, count))) {
        return 0;
    }
    f = dc_font(dc);
    lx_text_metrics(f->scale, &m);
    n = prepare_text(src, count, flags, prepared, LINE_CHARS);
    avail = rect[2] - rect[0] - ml - mr;
    nlines = layout_lines(f, prepared, n, flags, avail, lines, MAX_LINES);
    for (int i = 0; i < nlines; i++) {
        if ((flags & DT_END_ELLIPSIS) && !(flags & DT_WORDBREAK)) end_ellipsis(f, &lines[i], avail);
        if (lines[i].width > widest) widest = lines[i].width;
    }
    total = nlines * m.height;
    if (flags & DT_CALCRECT) {
        rect[2] = rect[0] + ml + mr + widest;
        rect[3] = rect[1] + total;
        return total;
    }
    y = rect[1];
    if (flags & DT_SINGLELINE) {
        if (flags & DT_VCENTER) y += (rect[3] - rect[1] - total) / 2;
        else if (flags & DT_BOTTOM) y = rect[3] - total;
    }
    t.fg = colorref_to_xrgb(dc->text_color);
    {
        int32_t dev[4] = { rect[0] - dc->org_x, rect[1] - dc->org_y, rect[2] - dc->org_x, rect[3] - dc->org_y };
        text_clip(&t.s, (flags & DT_NOCLIP) ? NULL : dev, t.clip);
    }
    for (int i = 0; i < nlines; i++) {
        int x = rect[0] + ml;
        if (flags & DT_CENTER) x += (avail - lines[i].width) / 2;
        else if (flags & DT_RIGHT) x += avail - lines[i].width;
        draw_run(dc, &t, x - dc->org_x, y + i * m.height - dc->org_y, lines[i].text, lines[i].len);
    }
    window_drawn(dc);
    return total;
}

enum { BDR_RAISEDOUTER = 1, BDR_SUNKENOUTER = 2, BDR_RAISEDINNER = 4, BDR_SUNKENINNER = 8, BF_MIDDLE = 0x800, BF_ADJUST = 0x2000 };

static void edge_frame(const surf_t* s, int32_t* r, uint32_t top_left, uint32_t bottom_right) {
    fill_surface(s, r[0], r[1], r[2] - 1, r[1] + 1, top_left);
    fill_surface(s, r[0], r[1] + 1, r[0] + 1, r[3] - 1, top_left);
    fill_surface(s, r[0], r[3] - 1, r[2], r[3], bottom_right);
    fill_surface(s, r[2] - 1, r[1], r[2], r[3] - 1, bottom_right);
    r[0]++, r[1]++, r[2]--, r[3]--;
}

int lx_gdi_draw_edge(uint32_t hdc, int32_t* rect, uint32_t edge, uint32_t flags) {
    enum { WHITE = 0xFFFFFF, LIGHT = 0xDFDFDF, FACE = 0xC0C0C0, SHADOW = 0x808080, DARK = 0x000000 };
    dc_t* dc = dc_of(hdc);
    surf_t s;
    int32_t r[4];
    if (!dc || !rect || !dc_surface(dc, &s)) return 0;
    for (int i = 0; i < 4; i++) r[i] = rect[i] - ((i & 1) ? dc->org_y : dc->org_x);
    if (flags & BF_MIDDLE) fill_surface(&s, r[0] + 1, r[1] + 1, r[2] - 1, r[3] - 1, FACE);
    if (edge & BDR_RAISEDOUTER) edge_frame(&s, r, WHITE, DARK);
    else if (edge & BDR_SUNKENOUTER) edge_frame(&s, r, SHADOW, WHITE);
    if (edge & BDR_RAISEDINNER) edge_frame(&s, r, LIGHT, SHADOW);
    else if (edge & BDR_SUNKENINNER) edge_frame(&s, r, DARK, LIGHT);
    if (flags & BF_ADJUST)
        for (int i = 0; i < 4; i++) rect[i] = r[i] + ((i & 1) ? dc->org_y : dc->org_x);
    window_drawn(dc);
    return 1;
}

static uint64_t g_Ellipse(const uint32_t* a, void* u) {
    (void)u;
    return dc_of(a[0]) != NULL;
}

static uint64_t g_LineTo(const uint32_t* a, void* u) {
    dc_t* dc = dc_of(a[0]);
    (void)u;
    if (!dc) return 0;
    dc->pos_x = (int32_t)a[1], dc->pos_y = (int32_t)a[2];
    return 1;
}

#define G32(fn, argc) { "gdi32.dll", #fn, g_##fn, argc, HT_STDCALL }

static const hle_entry_t k_table[] = {
    G32(BitBlt, 9), G32(StretchBlt, 11), G32(GetStockObject, 1), G32(CreateSolidBrush, 1), G32(CreatePen, 3), G32(CreateFontA, 14),
    G32(CreateFontIndirectA, 1), G32(CreateRectRgn, 4), G32(DeleteObject, 1), G32(CreateCompatibleDC, 1), G32(DeleteDC, 1),
    G32(SelectObject, 2), G32(SelectClipRgn, 2), G32(CreateDIBSection, 6), G32(SetBkMode, 2), G32(SetTextColor, 2),
    G32(SetBkColor, 2), G32(SetMapMode, 2), G32(SetStretchBltMode, 2), G32(SetTextAlign, 2), G32(GetTextColor, 1),
    G32(SetSystemPaletteUse, 2), G32(OffsetWindowOrgEx, 4), G32(MoveToEx, 4), G32(GetDeviceCaps, 2), G32(GetTextMetricsA, 2),
    G32(GetTextExtentPointA, 4), G32(TextOutA, 5), G32(Ellipse, 5), G32(LineTo, 3),
};

const hle_entry_t* hle_gdi32_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
