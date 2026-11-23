#include "linux_frontend.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "frontend_hd.h"
#include "frontend_platform.h"
#include "gamepad.h"
#include "guest_abi.h"
#include "guest_va.h"
#include "host_input.h"
#include "linux_log.h"
#include "linux_user32.h"
#include "loading_hd.h"

#define RASTER_W 0x08u
#define RASTER_H 0x0Cu
#define RASTER_BPP 0x10u
#define RASTER_PITCH 0x14u
#define RASTER_BITS 0x18u
#define PICTURE_MAX 16384

static struct {
    int enabled, hd;
    const uint32_t* layer;
    int layer_w, layer_h;
    const uint32_t* sprite;
    int sprite_size, sprite_x, sprite_y;
    int window_w, window_h;
    uint32_t* out;
    size_t out_capacity;
    int* column;
    size_t column_capacity;
    lx_frontend_stats_t stats;
    recomp_func_t centre_lifted;
} S = { .enabled = 1 };

/* ---- what the shared front end asks of the host ---- */

void fe_host_layer(const uint32_t* pixels, int w, int h, const fe_rect_t* dirty) {
    (void)dirty;
    S.layer = pixels;
    S.layer_w = pixels ? w : 0;
    S.layer_h = pixels ? h : 0;
}

void fe_host_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    (void)generation;
    S.sprite = sprite;
    S.sprite_size = sprite ? size : 0;
    S.sprite_x = x;
    S.sprite_y = y;
}

int fe_host_layer_failed(void) { return 0; }

void fe_host_layer_release(void) {
    S.layer = NULL;
    S.layer_w = S.layer_h = 0;
    S.sprite = NULL;
}

int fe_host_frame_size(int guest_w, int guest_h, int* w, int* h) {
    lx_view_t v;
    if (S.window_w <= 0 || S.window_h <= 0) return 0;
    v = lx_present_view(S.window_w, S.window_h, guest_w, guest_h, 0);
    if (v.w <= 0 || v.h <= 0) return 0;
    *w = v.w;
    *h = v.h;
    return 1;
}

int fe_host_loading_size(int guest_w, int guest_h, int* w, int* h) { return fe_host_frame_size(guest_w, guest_h, w, h); }

int fe_host_layer_available(void) { return S.enabled; }

int fe_host_pointer_position(int* x, int* y) { return host_game_cursor(x, y); }

int fe_host_pad_focus(void) { return gamepad_menu_focus(); }

#define FROZEN_PICTURE GVA(0x0066E338)

int fe_host_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch) {
    uint32_t still = 0, sw = 0, sh = 0, bpp = 0, sp = 0, sb = 0;
    if (!guest_read32(FROZEN_PICTURE, &still) || !still || !guest_read32(still + RASTER_W, &sw) || !guest_read32(still + RASTER_H, &sh) ||
        !guest_read32(still + RASTER_BPP, &bpp) || !guest_read32(still + RASTER_PITCH, &sp) || !guest_read32(still + RASTER_BITS, &sb))
        return 0;
    if (bpp != 16 || !sb || !sw || !sh || sw > PICTURE_MAX || sh > PICTURE_MAX || sp < sw || !guest_readable(sb, (sp * (sh - 1) + sw) * 2u))
        return 0;
    *bits = (const uint16_t*)ADDR(sb);
    *w = (int)sw;
    *h = (int)sh;
    *pitch = (long)sp;
    return 1;
}

double fe_host_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

uint32_t fe_host_tick(void) { return (uint32_t)fe_host_ms(); }

int fe_host_cpu_count(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n > 64 ? 64 : (int)n;
}

int fe_host_file_stamp(const char* path, uint64_t* stamp, uint64_t* size) {
    char host[MAX_PATH];
    struct stat st;
    if (!guest_host_path(path, host, sizeof host)) snprintf(host, sizeof host, "%s", path);
    if (stat(host, &st) != 0) return 0;
    *stamp = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    *size = (uint64_t)st.st_size;
    return 1;
}

const char* fe_host_user_folder(void) { return "D:\\"; }

int fe_host_exe_dir(char* out, size_t cap) {
    char path[MAX_PATH];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    char* slash;
    if (n <= 0) return 0;
    path[n] = 0;
    slash = strrchr(path, '/');
    if (!slash) return 0;
    slash[1] = 0;
    if (strlen(path) >= cap) return 0;
    strcpy(out, path);
    return 1;
}

int fe_host_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

void fe_host_level_load_begin(void) {}
void fe_host_level_loaded(void) {}
void fe_host_level_load_done(void) {}
int fe_host_display_built(void) { return 1; }

/* ---- window centring (the Options and Audio windows lose the entries that have no meaning here) ---- */

static void centre_window(void) {
    const uint32_t window = MEM32(g_esp + 4);
    if (g_alias_entry) {
        S.centre_lifted();
        return;
    }
    S.centre_lifted();
    frontend_hd_window_centred(window);
}

int lx_frontend_install(void) {
    loading_hd_install();
    return guest_override_install(GVA(0x00411EA0), centre_window, &S.centre_lifted);
}

void lx_frontend_reset(void) {
    const int enabled = S.enabled;
    frontend_hd_release();
    free(S.out);
    free(S.column);
    memset(&S, 0, sizeof S);
    S.enabled = enabled;
}

void lx_frontend_enable(int enabled) { S.enabled = enabled != 0; }
int lx_frontend_enabled(void) { return S.enabled; }

void lx_frontend_ui_frame(void) { loading_hd_ui_frame(); }

void lx_frontend_file_created(const char* guest_path) {
    if (S.enabled) frontend_hd_save_opened(guest_path, fe_host_user_folder());
}

void lx_frontend_file_deleted(const char* guest_path) {
    if (S.enabled) frontend_hd_save_deleted(guest_path, fe_host_user_folder());
}

void lx_frontend_stats(lx_frontend_stats_t* out) { *out = S.stats; }

/* ---- the frame ---- */

static int menu_open(void) {
    guest_ui_view_t v;
    if (!guest_ui_probe(&v)) return 0;
    return (v.ui == GUEST_UI_MENU || v.ui == GUEST_UI_DIALOG) && v.kind != GUEST_WND_LOADER && v.kind != GUEST_WND_VIDEO;
}

int lx_frontend_frame(int guest_w, int guest_h, int window_w, int window_h) {
    S.window_w = window_w;
    S.window_h = window_h;
    if (!S.enabled) return 0;
    if (guest_w == FE_CANVAS_W && guest_h == FE_CANVAS_H && menu_open()) S.hd = frontend_hd_frame(guest_w, guest_h);
    else frontend_hd_idle();
    return S.layer != NULL;
}

void lx_frontend_frame_done(void) {
    if (S.hd) frontend_hd_frame_done();
    S.hd = 0;
}

static int grow(size_t pixels) {
    if (S.out_capacity < pixels) {
        uint32_t* grown = (uint32_t*)realloc(S.out, pixels * 4u);
        if (!grown) return 0;
        S.out = grown;
        S.out_capacity = pixels;
    }
    return 1;
}

static int grow_columns(size_t n) {
    if (S.column_capacity < n) {
        int* grown = (int*)realloc(S.column, n * 2u * sizeof(int));
        if (!grown) return 0;
        S.column = grown;
        S.column_capacity = n;
    }
    return 1;
}

static uint32_t from_565(uint16_t c) {
    const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

/* The guest's picture, nearest-neighbour, into the view's rectangle; the rest of the window is black. */
static void draw_guest(const lx_frame_t* in, lx_view_t v, int window_w, int window_h) {
    for (size_t i = 0, n = (size_t)window_w * (size_t)window_h; i < n; i++) S.out[i] = 0xFF000000u;
    for (int x = 0; x < v.w; x++) S.column[x] = (int)(((long long)x * 2 + 1) * in->width / (2 * (long long)v.w));
    for (int y = 0; y < v.h; y++) {
        const int sy = (int)(((long long)y * 2 + 1) * in->height / (2 * (long long)v.h));
        const uint8_t* row = (const uint8_t*)in->pixels + (size_t)sy * (size_t)in->pitch;
        uint32_t* dst = S.out + (size_t)(v.y + y) * (size_t)window_w + (size_t)v.x;
        if (in->format == LX_PF_RGB565)
            for (int x = 0; x < v.w; x++) dst[x] = from_565(((const uint16_t*)row)[S.column[x]]);
        else
            for (int x = 0; x < v.w; x++) dst[x] = 0xFF000000u | ((const uint32_t*)row)[S.column[x]];
    }
}

static uint32_t over(uint32_t top, uint32_t under) {
    const uint32_t inv = 255u - (top >> 24);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t c = ((top >> shift) & 255u) + (((under >> shift) & 255u) * inv + 127u) / 255u;
        out |= (c > 255u ? 255u : c) << shift;
    }
    return out | 0xFF000000u;
}

static uint32_t lerp4(uint32_t a, uint32_t b, unsigned t) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) out |= ((((a >> shift) & 255u) * (256u - t) + ((b >> shift) & 255u) * t) >> 8) << shift;
    return out;
}

/* The layer (premultiplied BGRA) over the view's rectangle: copied when the sizes match, bilinear otherwise. */
static void draw_layer(lx_view_t v, int window_w) {
    if (S.layer_w == v.w && S.layer_h == v.h) {
        for (int y = 0; y < v.h; y++) {
            const uint32_t* src = S.layer + (size_t)y * (size_t)v.w;
            uint32_t* dst = S.out + (size_t)(v.y + y) * (size_t)window_w + (size_t)v.x;
            for (int x = 0; x < v.w; x++) dst[x] = over(src[x], dst[x]);
        }
        return;
    }
    for (int x = 0; x < v.w; x++) {
        const long long fx = ((long long)x * 2 + 1) * S.layer_w * 128 / v.w - 128;
        const int x0 = (int)(fx >> 8);
        S.column[2 * x] = x0 < 0 ? 0 : x0 >= S.layer_w ? S.layer_w - 1 : x0;
        S.column[2 * x + 1] = (int)(fx < 0 ? 0 : fx & 255);
    }
    for (int y = 0; y < v.h; y++) {
        const long long fy = ((long long)y * 2 + 1) * S.layer_h * 128 / v.h - 128;
        const int y0 = (int)(fy >> 8), y1 = y0 + 1;
        const unsigned wy = (unsigned)(fy < 0 ? 0 : fy & 255);
        const uint32_t *r0 = S.layer + (size_t)(y0 < 0 ? 0 : y0 >= S.layer_h ? S.layer_h - 1 : y0) * (size_t)S.layer_w,
                       *r1 = S.layer + (size_t)(y1 < 0 ? 0 : y1 >= S.layer_h ? S.layer_h - 1 : y1) * (size_t)S.layer_w;
        uint32_t* dst = S.out + (size_t)(v.y + y) * (size_t)window_w + (size_t)v.x;
        for (int x = 0; x < v.w; x++) {
            const int a = S.column[2 * x], b = a + 1 < S.layer_w ? a + 1 : a;
            const unsigned wx = (unsigned)S.column[2 * x + 1];
            const uint32_t top = lerp4(lerp4(r0[a], r0[b], wx), lerp4(r1[a], r1[b], wx), wy);
            dst[x] = over(top, dst[x]);
        }
    }
}

static int draw_pointer(lx_view_t v, int window_w) {
    int drawn = 0;
    if (!S.sprite || S.sprite_size <= 0 || S.layer_w <= 0 || S.layer_h <= 0) return 0;
    for (int sy = 0; sy < S.sprite_size; sy++) {
        const int ly = S.sprite_y + sy, y = v.y + (int)((long long)ly * v.h / S.layer_h);
        if (ly < 0 || ly >= S.layer_h) continue;
        for (int sx = 0; sx < S.sprite_size; sx++) {
            const int lx = S.sprite_x + sx, x = v.x + (int)((long long)lx * v.w / S.layer_w);
            const uint32_t px = S.sprite[(size_t)sy * (size_t)S.sprite_size + (size_t)sx];
            if (lx < 0 || lx >= S.layer_w || !(px >> 24)) continue;
            S.out[(size_t)y * (size_t)window_w + (size_t)x] = over(px, S.out[(size_t)y * (size_t)window_w + (size_t)x]);
            drawn = 1;
        }
    }
    return drawn;
}

int lx_frontend_compose(const lx_frame_t* in, lx_frame_t* out, int window_w, int window_h, int stretch) {
    lx_view_t v;
    if (!S.enabled || !S.layer || !in->pixels || in->width <= 0 || in->height <= 0 || window_w <= 0 || window_h <= 0) return 0;
    v = lx_present_view(window_w, window_h, in->width, in->height, stretch);
    if (v.w <= 0 || v.h <= 0 || v.x < 0 || v.y < 0 || v.x + v.w > window_w || v.y + v.h > window_h) return 0;
    if (!grow((size_t)window_w * (size_t)window_h) || !grow_columns((size_t)(v.w > v.h ? v.w : v.h))) return 0;
    draw_guest(in, v, window_w, window_h);
    draw_layer(v, window_w);
    S.stats.pointer_drawn = draw_pointer(v, window_w);
    S.stats.frames_with_layer++;
    S.stats.composed_frames++;
    S.stats.layer_w = S.layer_w, S.stats.layer_h = S.layer_h;
    S.stats.view_x = v.x, S.stats.view_y = v.y, S.stats.view_w = v.w, S.stats.view_h = v.h;
    *out = *in;
    out->pixels = S.out;
    out->width = window_w;
    out->height = window_h;
    out->pitch = window_w * 4;
    out->format = LX_PF_XRGB8888;
    return 1;
}
