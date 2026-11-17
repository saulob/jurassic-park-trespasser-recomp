/*
 * The HD loading screen. The level load (0x004142C0) opens the game's loading window, CLoaderWnd, which draws itself and
 * Blts the screen to the front at each step of progress (0x00415AD0). Around that draw the renderer is handed a layer
 * painted from the window's own state (the level's picture, its bars and their captions), and the game's present shows
 * it. The "Loading Level" line the game writes on the front before a level change (0x00413040) gets a plaque the same
 * way. Whatever the layer cannot show is left to the game's own screen.
 */
#include "guest_va.h"
#include "host.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend_assets.h"
#include "frontend_hd.h"
#include "jpt_l10n.h"
#include "l10n.h"
#include "loading_hd.h"
#include "save_preview.h"

#define LEVEL_LOAD_VA GVA(0x004142C0)
#define LOADER_DRAW_VA GVA(0x00415AD0)
#define SCREEN_MESSAGE_VA GVA(0x00413040)
#define LOADING_LEVEL_STRING 37u
#define LOADER_VTABLE GVA(0x006467A0)
#define SCENE_NAME GVA(0x0066E342)
#define ART_TABLE GVA(0x006610D0)
#define ART_ROW 0x28u
#define ART_IMAGE 0x14u
#define GAME_INSTANCE GVA(0x0066E05C)
#define SCREEN_RASTER GVA(0x0073DC40)
#define RASTER_DISPLAY_W 0x7Cu
#define RASTER_DISPLAY_H 0x80u
#define WND_CONTROLS_BEGIN 0x20u
#define WND_CONTROLS_END 0x24u
#define WND_BACKGROUND 0x2Cu
#define CTRL_FLAGS 0x08u
#define CTRL_VISIBLE 4u
#define CTRL_ID 0x0Cu
#define CTRL_RECT 0x10u
#define VT_TEXTBOX GVA(0x006457B8)
#define VT_STATIC GVA(0x006455B0)
#define VT_PROGRESS GVA(0x00645974)
#define TEXT_STRING 0x28u
#define TEXT_FG 0x30u
#define BAR_MIN 0x20u
#define BAR_MAX 0x24u
#define BAR_POS 0x28u
#define BAR_COLOR 0x30u
#define RASTER_W 0x08u
#define RASTER_H 0x0Cu
#define RASTER_BPP 0x10u
#define RASTER_PITCH 0x14u
#define RASTER_BITS 0x18u
#define ART_W 640
#define ART_H 480
#define MAX_CONTROLS 32
#define CAPTION_REACH 40
#define SCENE_CAP 64
#define LOG_LIMIT 24

static struct {
    recomp_func_t level_lifted, draw_lifted, message_lifted;
    load_hold_t hold;
    int loading, drawing, messaging;
    ld_scene_t previous;
    int have_previous;
    uint32_t* layer;
    int layer_w, layer_h;
    sp_image_t art;
    char art_slot[LD_SLOT_BYTES], art_path[MAX_PATH];
    int art_tried;
    char scene[SCENE_CAP], slot[LD_SLOT_BYTES], refusal[96];
    unsigned updates, held;
    int shown, painted_logged, loads_logged, held_logged, omitted;
} L;

static int read32(uint32_t va, uint32_t* out) { return guest_read32(va, out); }

static uint32_t peek32(uint32_t va) {
    uint32_t v = 0;
    return read32(va, &v) ? v : 0;
}

static int read_rect(uint32_t va, fe_rect_t* r) {
    uint32_t v[4];
    for (int i = 0; i < 4; i++)
        if (!read32(va + 4u * (uint32_t)i, &v[i])) return 0;
    *r = fe_rect((int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]);
    return 1;
}

static uint32_t rgb_of_565(uint32_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

static uint32_t rgb_of_colorref(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

/* The level's picture as CLoaderWnd laid it: 640x480, centred in its screen-sized background raster. */
static int read_art(uint32_t loader, ld_scene_t* s) {
    uint32_t raster = peek32(loader + WND_BACKGROUND), w, h, bpp, pitch, bits;
    if (!raster || !read32(raster + RASTER_W, &w) || !read32(raster + RASTER_H, &h) || !read32(raster + RASTER_BPP, &bpp) ||
        !read32(raster + RASTER_PITCH, &pitch) || !read32(raster + RASTER_BITS, &bits))
        return 0;
    if (bpp != 16 || !bits || w < ART_W || h < ART_H || w > 16384 || h > 16384 || pitch < w ||
        !guest_readable(bits, (pitch * (h - 1) + w) * 2u))
        return 0;
    s->art565 = (const uint16_t*)(uintptr_t)bits + (size_t)((int)h / 2 - ART_H / 2) * pitch + ((int)w / 2 - ART_W / 2);
    s->art_w = ART_W;
    s->art_h = ART_H;
    s->art_pitch = (int)pitch;
    s->art_key = fe_hash_pixels(s->art565, ART_W, ART_H, (int)pitch, 7);
    return 1;
}

/* The picture's slot, by the game's own rule over its own table. */
static void read_slot(char* slot, int cap, char* scene, int scene_cap) {
    char names[LD_TABLE_ROWS][24], images[LD_TABLE_ROWS][24];
    const char* scenes[LD_TABLE_ROWS];
    int row;
    slot[0] = 0;
    if (!guest_read_string(SCENE_NAME, scene, scene_cap)) scene[0] = 0;
    for (int i = 0; i < LD_TABLE_ROWS; i++) {
        uint32_t at = ART_TABLE + ART_ROW * (uint32_t)i;
        if (!guest_read_string(at, names[i], ART_IMAGE + 1) || !guest_read_string(at + ART_IMAGE, images[i], ART_IMAGE + 1)) return;
        scenes[i] = names[i];
    }
    row = ld_select(scene, scenes, LD_TABLE_ROWS);
    if (row >= 0) ld_slot_name(images[row], slot, cap);
}

typedef struct {
    fe_rect_t rect;
    int32_t id;
    char text[LD_TEXT_BYTES];
    uint32_t color;
    int used;
} caption_t;

typedef struct {
    fe_rect_t rect;
    int32_t id, lo, hi, pos;
    uint32_t color;
} bar_t;

static int refuse(const char* why) {
    if (strcmp(L.refusal, why)) {
        strncpy(L.refusal, why, sizeof L.refusal - 1);
        if (L.loads_logged < LOG_LIMIT) host_log("loading", "the game's own loading screen is shown: %s", why);
    }
    return 0;
}

enum { OMIT_CONTROL = 1, OMIT_BAR = 2, OMIT_TEXT = 4 };

/* One part of the window the HD screen cannot show: left out, the rest of the screen stays HD. */
static void omit(int kind, const char* what) {
    if (L.omitted & kind) return;
    L.omitted |= kind;
    if (L.loads_logged < LOG_LIMIT) host_log("loading", "warning: left out of the HD loading screen: %s", what);
}

/* As the game draws them: a text box only while visible, a progress bar always (CUIProgressBar's draw, 0x00407BC0,
 * never reads the visible bit, and loader.ddf's bars have it clear). */
static int drawn_by_game(uint32_t vt, uint32_t flags) { return vt == VT_PROGRESS || (flags & CTRL_VISIBLE); }

static void sort_by_top(void* items, int count, size_t size, size_t rect_at) {
    char tmp[sizeof(caption_t) > sizeof(bar_t) ? sizeof(caption_t) : sizeof(bar_t)];
    char* base = (char*)items;
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0; j--) {
            char *a = base + (size_t)(j - 1) * size, *b = base + (size_t)j * size;
            if (((const fe_rect_t*)(b + rect_at))->top >= ((const fe_rect_t*)(a + rect_at))->top) break;
            memcpy(tmp, a, size);
            memcpy(a, b, size);
            memcpy(b, tmp, size);
        }
}

/* loader.ddf names each bar by id: text 1000 is bar 100's caption, text 1001 bar 101's; any other bar takes the
 * nearest unused text just above it. */
#define CAPTION_ID_OFFSET 900

static int caption_of(caption_t* captions, int count, const bar_t* bar) {
    int best = -1;
    for (int i = 0; i < count; i++)
        if (!captions[i].used && captions[i].id == bar->id + CAPTION_ID_OFFSET) return i;
    for (int i = 0; i < count; i++) {
        fe_rect_t r = captions[i].rect;
        if (captions[i].used || r.bottom > bar->rect.top || bar->rect.top - r.bottom > CAPTION_REACH || r.right <= bar->rect.left ||
            r.left >= bar->rect.right)
            continue;
        if (best < 0 || r.bottom > captions[best].rect.bottom) best = i;
    }
    return best;
}

static void read_caption(uint32_t c, int32_t id, fe_rect_t r, caption_t* captions, int* count) {
    caption_t* t = &captions[*count];
    t->rect = r;
    t->id = id;
    t->used = 0;
    t->color = rgb_of_colorref(peek32(c + TEXT_FG));
    if (!guest_read_string(peek32(c + TEXT_STRING), t->text, sizeof t->text)) t->text[0] = 0;
    if (t->text[0]) {
        const char* label = l10n_label(t->text);
        if (label) snprintf(t->text, sizeof t->text, "%s", label);
        (*count)++;
    }
}

static void read_bar(uint32_t c, int32_t id, fe_rect_t r, bar_t* bars, int* count) {
    bar_t* b = &bars[(*count)++];
    b->rect = r;
    b->id = id;
    b->lo = (int32_t)peek32(c + BAR_MIN);
    b->hi = (int32_t)peek32(c + BAR_MAX);
    b->pos = (int32_t)peek32(c + BAR_POS);
    b->color = guest_readable(c + BAR_COLOR, 2) ? rgb_of_565(*(const volatile uint16_t*)(uintptr_t)(c + BAR_COLOR)) : 0xFF4100;
}

static void make_gauge(ld_gauge_t* g, const bar_t* bar, caption_t* caption) {
    double span = (double)bar->hi - bar->lo;
    g->fraction = span > 0 ? (float)(((double)bar->pos - bar->lo) / span) : 0.0f;
    if (!(g->fraction > 0.0f)) g->fraction = 0.0f;
    if (g->fraction > 1.0f) g->fraction = 1.0f;
    g->color = bar->color;
    g->text_color = caption ? caption->color : 0xFFFFFF;
    g->caption[0] = 0;
    if (!caption) return;
    caption->used = 1;
    strcpy(g->caption, caption->text);
}

static int read_controls(uint32_t loader, ld_scene_t* s) {
    caption_t captions[MAX_CONTROLS];
    bar_t bars[MAX_CONTROLS];
    int caption_count = 0, bar_count = 0;
    uint32_t begin = peek32(loader + WND_CONTROLS_BEGIN), end = peek32(loader + WND_CONTROLS_END);
    if (end < begin || (end - begin) % 4 || (end - begin) / 4 > MAX_CONTROLS || !guest_readable(begin, end - begin))
        return refuse("its controls cannot be read");
    for (uint32_t at = begin; at < end; at += 4) {
        uint32_t c = peek32(at), vt, flags, id;
        fe_rect_t r;
        if (!c || !read32(c, &vt) || !read32(c + CTRL_FLAGS, &flags) || !read32(c + CTRL_ID, &id) || !read_rect(c + CTRL_RECT, &r)) {
            omit(OMIT_CONTROL, "a control that cannot be read");
            continue;
        }
        if (vt == VT_STATIC || !drawn_by_game(vt, flags)) continue;
        if (vt == VT_TEXTBOX) read_caption(c, (int32_t)id, r, captions, &caption_count);
        else if (vt == VT_PROGRESS) read_bar(c, (int32_t)id, r, bars, &bar_count);
        else omit(OMIT_CONTROL, "a control it does not know");
    }
    sort_by_top(bars, bar_count, sizeof bars[0], offsetof(bar_t, rect));
    sort_by_top(captions, caption_count, sizeof captions[0], offsetof(caption_t, rect));
    if (bar_count > LD_MAX_GAUGES) {
        omit(OMIT_BAR, "a progress bar past the fourth");
        bar_count = LD_MAX_GAUGES;
    }
    for (int i = 0; i < bar_count; i++) {
        int k = caption_of(captions, caption_count, &bars[i]);
        make_gauge(&s->gauges[i], &bars[i], k >= 0 ? &captions[k] : NULL);
    }
    s->gauge_count = bar_count;
    for (int i = 0; i < caption_count; i++) {
        if (captions[i].used) continue;
        if (s->note_count == LD_MAX_NOTES) {
            omit(OMIT_TEXT, "a line of text past the second free one");
            break;
        }
        strcpy(s->notes[s->note_count], captions[i].text);
        s->note_colors[s->note_count++] = captions[i].color;
    }
    return 1;
}

int loading_hd_read(uint32_t loader, int screen_w, int screen_h, ld_scene_t* s, char* slot, int slot_cap) {
    char scene[SCENE_CAP];
    memset(s, 0, sizeof *s);
    slot[0] = 0;
    if (peek32(loader) != LOADER_VTABLE) return refuse("the window in front is not the loading window");
    if (screen_w <= ART_W || screen_h <= ART_H) return refuse("the screen is no larger than the game's 640x480");
    if (!read_art(loader, s)) return refuse("its picture cannot be read");
    if (!read_controls(loader, s)) return 0;
    read_slot(slot, slot_cap, scene, sizeof scene);
    s->kind = LD_SCENE_LOADER;
    s->screen_w = screen_w;
    s->screen_h = screen_h;
    return 1;
}

static void art_folder(char* out, size_t cap) {
    char* slash;
    out[0] = 0;
    if (!GetModuleFileNameA(NULL, out, (DWORD)cap)) return;
    slash = strrchr(out, '\\');
    if (!slash) {
        out[0] = 0;
        return;
    }
    slash[1] = 0;
    strncat(out, "assets\\frontend\\loading_hd\\", cap - strlen(out) - 1);
}

/* The slot's HD picture, if one was drawn: <exe>\assets\frontend\loading_hd\<slot>.png or .jpg, decoded once a load at
 * about the size the picture has on this screen. */
static void load_hd_art(int screen_w, int screen_h, const char* slot) {
    static const char* const k_types[] = { ".png", ".jpg" };
    char folder[MAX_PATH];
    ld_layout_t l;
    fe_rect_t box;
    L.art_tried = 1;
    strncpy(L.art_slot, slot, sizeof L.art_slot - 1);
    art_folder(folder, sizeof folder);
    ld_layout_make(&l, screen_w, screen_h, 0, 0);
    box = fe_view_rect(&l.view, l.photo);
    for (int i = 0; folder[0] && i < 2; i++) {
        if (_snprintf(L.art_path, sizeof L.art_path - 1, "%s%s%s", folder, slot, k_types[i]) < 0) continue;
        L.art_path[sizeof L.art_path - 1] = 0;
        if (GetFileAttributesA(L.art_path) == INVALID_FILE_ATTRIBUTES) continue;
        if (sp_image_load(L.art_path, (box.right - box.left) * 2, box.bottom - box.top, &L.art)) return;
        host_log("loading", "warning: HD art %s could not be read: the game's picture is shown", L.art_path);
    }
    L.art_path[0] = 0;
}

static void use_hd_art(ld_scene_t* s, const char* slot) {
    if (slot[0] && !L.art_tried) load_hd_art(s->screen_w, s->screen_h, slot);
    if (!L.art.pixels || strcmp(L.art_slot, slot)) return;
    s->art32 = L.art.pixels;
    s->art565 = NULL;
    s->art_w = L.art.width;
    s->art_h = L.art.height;
    s->art_key = fe_hash(fe_hash(0, slot, strlen(slot)), &L.art.pixels, sizeof L.art.pixels);
}

static int ensure_layer(int w, int h) {
    if (L.layer && L.layer_w == w && L.layer_h == h) return 1;
    free(L.layer);
    L.layer = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
    L.layer_w = L.layer ? w : 0;
    L.layer_h = L.layer ? h : 0;
    L.have_previous = 0;
    return L.layer != NULL;
}

static void log_first_paint(const ld_scene_t* s, double ms) {
    if (L.painted_logged || L.loads_logged >= LOG_LIMIT) return;
    L.painted_logged = 1;
    if (s->kind == LD_SCENE_MESSAGE)
        host_log("loading", "\"%s\" shown on a plaque at %dx%d (painted in %.1f ms)", s->message, s->screen_w, s->screen_h, ms);
    else if (s->art32)
        host_log("loading", "level \"%s\": HD loading screen at %dx%d using %s (%dx%d, painted in %.1f ms)", L.scene, s->screen_w,
                 s->screen_h, strrchr(L.art_path, '\\') ? strrchr(L.art_path, '\\') + 1 : L.art_path, s->art_w, s->art_h, ms);
    else
        host_log("loading", "level \"%s\": HD loading screen at %dx%d using %s_0.tga (painted in %.1f ms)", L.scene, s->screen_w,
                 s->screen_h, L.slot[0] ? L.slot : "the game's picture", ms);
}

/* Paints what changed since the last draw and hands the layer to the renderer for the game's next present. */
static int show(const ld_scene_t* now) {
    int threads = 1, diff;
    fe_painter_t* painter = frontend_hd_painter(&threads);
    ld_layout_t l;
    fe_rect_t dirty, clip;
    RECT box;
    LARGE_INTEGER t0, t1, freq;
    if (d3dr_frontend_failed()) return refuse("the renderer could not show the HD layer");
    if (!painter) return refuse("the HD painter could not start");
    if (!ensure_layer(now->screen_w, now->screen_h)) return refuse("no memory for the HD layer");
    ld_layout_scene(&l, now);
    diff = ld_scene_diff(L.have_previous ? &L.previous : NULL, now, &l, &dirty);
    clip = diff == LD_DIFF_ALL ? fe_rect(0, 0, now->screen_w, now->screen_h) : dirty;
    QueryPerformanceCounter(&t0);
    if (!fe_rect_empty(clip) && !fe_paint_loading(painter, now, L.layer, now->screen_w, clip, threads)) {
        L.have_previous = 0;
        return refuse("the HD layer could not be painted");
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (diff == LD_DIFF_ALL) log_first_paint(now, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    L.previous = *now;
    L.have_previous = 1;
    L.refusal[0] = 0;
    box.left = clip.left;
    box.top = clip.top;
    box.right = clip.right;
    box.bottom = clip.bottom;
    d3dr_set_frontend(L.layer, now->screen_w, now->screen_h, fe_rect_empty(clip) ? NULL : &box);
    return 1;
}

int loading_hd_screen(int* w, int* h) {
    uint32_t raster;
    if (!d3dr_hardware() || !guest_display_built() || !read32(SCREEN_RASTER, &raster) || !raster) return 0;
    *w = (int)peek32(raster + RASTER_DISPLAY_W);
    *h = (int)peek32(raster + RASTER_DISPLAY_H);
    return *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384;
}

static void release(void) {
    int threads;
    fe_painter_t* painter = L.layer || L.art.pixels ? frontend_hd_painter(&threads) : NULL;
    if (painter) fe_painter_trim(painter);
    save_preview_free(&L.art);
    free(L.layer);
    L.layer = NULL;
    L.layer_w = L.layer_h = 0;
    L.have_previous = 0;
    L.art_tried = 0;
    L.painted_logged = 0;
    L.art_slot[0] = L.art_path[0] = 0;
    d3dr_frontend_release();
}

static void loader_draw(void) {
    ld_scene_t now;
    char slot[LD_SLOT_BYTES];
    int w, h, hd = 0;
    uint32_t loader = g_ecx;
    if (g_alias_entry) {
        L.draw_lifted();
        return;
    }
    if (loading_hd_screen(&w, &h) && loading_hd_read(loader, w, h, &now, slot, sizeof slot)) {
        if (!L.scene[0] && !guest_read_string(SCENE_NAME, L.scene, sizeof L.scene)) L.scene[0] = 0;
        if (!L.slot[0]) strncpy(L.slot, slot, sizeof L.slot - 1);
        use_hd_art(&now, slot);
        hd = show(&now);
    }
    if (hd) {
        L.updates++;
        L.shown = 1;
    }
    L.drawing = 1;
    L.draw_lifted();
    L.drawing = 0;
    if (hd) d3dr_set_frontend(NULL, 0, 0, NULL);
}

static void screen_message(void) {
    ld_scene_t now;
    int w, h, hd = 0;
    if (g_alias_entry) {
        L.message_lifted();
        return;
    }
    if (MEM32(g_esp + 4) == LOADING_LEVEL_STRING && loading_hd_screen(&w, &h) && w > ART_W && h > ART_H) {
        const char* translated = jpt_l10n_text_or(LOADING_LEVEL_STRING, NULL);
        memset(&now, 0, sizeof now);
        now.kind = LD_SCENE_MESSAGE;
        now.screen_w = w;
        now.screen_h = h;
        if (translated) snprintf(now.message, sizeof now.message, "%s", translated);
        if (translated ||
            LoadStringA((HINSTANCE)(uintptr_t)peek32(GAME_INSTANCE), LOADING_LEVEL_STRING, now.message, sizeof now.message) > 0) {
            frontend_hd_release();
            hd = show(&now);
        }
    }
    L.messaging = 1;
    L.message_lifted();
    L.messaging = 0;
    if (hd) d3dr_set_frontend(NULL, 0, 0, NULL);
}

static void log_load(void) {
    char bars[160];
    int n = 0;
    if (L.loads_logged >= LOG_LIMIT) return;
    if (++L.loads_logged == LOG_LIMIT) host_log("loading", "loading log limit reached");
    if (!L.shown) return;
    bars[0] = 0;
    for (int i = 0; i < L.previous.gauge_count && n < (int)sizeof bars - 1; i++)
        n += _snprintf(bars + n, sizeof bars - 1 - n, "%s%s %.0f%%", i ? ", " : "", L.previous.gauges[i].caption,
                       L.previous.gauges[i].fraction * 100.0f);
    bars[sizeof bars - 1] = 0;
    host_log("loading", "level \"%s\" loaded: %u HD loading screen updates (%s); %u blank presents held so far", L.scene,
             L.updates, bars[0] ? bars : "no bars", L.held);
}

static void level_load(void) {
    if (g_alias_entry) {
        L.level_lifted();
        return;
    }
    if (d3dr_hardware()) {
        frontend_hd_release();
        load_hold_arm(&L.hold, GetTickCount());
    }
    L.loading = 1;
    L.updates = 0;
    L.shown = 0;
    L.omitted = 0;
    L.scene[0] = L.slot[0] = L.refusal[0] = 0;
    L.level_lifted();
    L.loading = 0;
    window_host_level_load_done();
    log_load();
    release();
    if (d3dr_hardware()) load_hold_arm(&L.hold, GetTickCount());
    else load_hold_release(&L.hold);
}

int loading_hd_level_loading(void) { return L.loading; }

int loading_hd_present_held(void) {
    int held = load_hold_present(&L.hold, L.drawing || L.messaging, GetTickCount());
    if (held) {
        L.held++;
        if (!L.held_logged++) host_log("loading", "a blank present around a level load was held: the loading picture stays up");
    }
    return held;
}

void loading_hd_ui_frame(void) {
    load_hold_release(&L.hold);
    if (!L.loading && L.layer) release();
}

void loading_hd_install(void) {
    const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { SCREEN_MESSAGE_VA, screen_message, &L.message_lifted },
        { LEVEL_LOAD_VA, level_load, &L.level_lifted },
        { LOADER_DRAW_VA, loader_draw, &L.draw_lifted },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) guest_override_install(k[i].va, k[i].replacement, k[i].lifted);
}
