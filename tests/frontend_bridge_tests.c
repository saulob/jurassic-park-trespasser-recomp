/* The HD frontend's bridge (frontend_hd.c) against fake game objects laid out as the retail ones: what it reads from
 * each window, the Options and Audio edits, and the fallbacks. A 32-bit build, so host pointers are guest addresses. */
#include "frontend_hd.h"
#include "host.h"
#include "save_preview.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define VT_BUTTON 0x0064554Cu
#define VT_LISTBOX 0x00645650u
#define VT_CHECKBOX 0x00645758u
#define VT_TEXTBOX 0x006457B8u
#define VT_SLIDER 0x00645864u
#define VT_STATIC 0x006455B0u
#define VT_PROGRESS 0x00645974u
#define VT_HOTSPOT 0x006459CCu
#define VT_MAIN 0x006466B8u
#define VT_OPTIONS 0x00646A58u
#define VT_AUDIO 0x00646C28u
#define VT_CONTROLS 0x00645F38u
#define VT_LOAD 0x006460D0u
#define VT_YESNO 0x00645D34u
#define VT_OK 0x00646390u
#define VT_GAME 0x00645C3Cu
#define VT_VIDEO 0x00646FC8u

static int* g_checks;
static int* g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        (*g_checks)++;                                                                                                  \
        if (!(cond)) {                                                                                                  \
            (*g_failures)++;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

/* ---- fake guest memory: only allocated blocks are readable ---- */

#define MAX_BLOCKS 512
static struct {
    uint8_t* p;
    size_t n;
} g_blocks[MAX_BLOCKS];
static int g_block_count;
static uint32_t g_main;

static uint32_t fake(size_t bytes) {
    uint8_t* p = (uint8_t*)calloc(1, bytes);
    if (!p || g_block_count >= MAX_BLOCKS) abort();
    g_blocks[g_block_count].p = p;
    g_blocks[g_block_count++].n = bytes;
    return (uint32_t)(uintptr_t)p;
}

static void free_fakes(void) {
    for (int i = 0; i < g_block_count; i++) free(g_blocks[i].p);
    g_block_count = 0;
    g_main = 0;
}

static int inside(uint32_t va, uint32_t bytes) {
    for (int i = 0; i < g_block_count; i++) {
        uintptr_t lo = (uintptr_t)g_blocks[i].p, hi = lo + g_blocks[i].n;
        if (va >= lo && (uintptr_t)va + bytes <= hi) return 1;
    }
    return 0;
}

static void put32(uint32_t va, uint32_t v) { *(uint32_t*)(uintptr_t)va = v; }
static uint32_t get32(uint32_t va) { return *(const uint32_t*)(uintptr_t)va; }

int guest_read32(uint32_t va, uint32_t* out) {
    if (va == MAIN_WINDOW_OBJECT && g_main) {
        *out = g_main;
        return 1;
    }
    if (!inside(va, 4)) return 0;
    *out = get32(va);
    return 1;
}

int guest_readable(uint32_t va, uint32_t bytes) { return bytes == 0 || inside(va, bytes); }

int guest_read_string(uint32_t va, char* out, int cap) {
    int n = 0;
    if (cap <= 0) return 0;
    for (; n < cap - 1; n++) {
        if (!inside(va + (uint32_t)n, 1)) return 0;
        out[n] = *(const char*)(uintptr_t)(va + (uint32_t)n);
        if (!out[n]) return 1;
    }
    out[n] = 0;
    return 1;
}

int guest_write32(uint32_t va, uint32_t value) {
    if (!inside(va, 4)) return 0;
    put32(va, value);
    return 1;
}

const char* guest_ui_class_name(uint32_t vtable) { return vtable == VT_VIDEO ? "CVideoWnd" : "a window of the game"; }

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

static struct {
    const uint32_t* pixels;
    RECT dirty;
    int has_dirty, calls;
} g_layer;

void d3dr_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty) {
    (void)w;
    (void)h;
    if (!pixels) return;
    g_layer.calls++;
    g_layer.pixels = pixels;
    g_layer.has_dirty = dirty != NULL;
    if (dirty) g_layer.dirty = *dirty;
}

void d3dr_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    (void)sprite;
    (void)size;
    (void)generation;
    (void)x;
    (void)y;
}

int d3dr_frontend_failed(void) { return 0; }

static char g_user_dir[MAX_PATH];
static uint16_t g_frozen[480][640];

const char* io_trace_user_folder(void) { return g_user_dir; }

int ui_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch) {
    *bits = &g_frozen[0][0];
    *w = 640;
    *h = 480;
    *pitch = 640;
    return 1;
}
void d3dr_frontend_release(void) {}
int window_host_pointer(int* x, int* y) {
    *x = *y = 0;
    return 0;
}

/* ---- game objects ---- */

static uint32_t raster(int w, int h, uint16_t fill) {
    uint32_t r = fake(0x74), bits = fake((size_t)w * h * 2);
    for (int i = 0; i < w * h; i++) ((uint16_t*)(uintptr_t)bits)[i] = fill;
    put32(r + 0x08, (uint32_t)w);
    put32(r + 0x0C, (uint32_t)h);
    put32(r + 0x10, 16);
    put32(r + 0x14, (uint32_t)w);
    put32(r + 0x18, bits);
    return r;
}

static uint32_t text(const char* s) {
    uint32_t p = fake(strlen(s) + 1);
    memcpy((void*)(uintptr_t)p, s, strlen(s) + 1);
    return p;
}

static uint32_t control(uint32_t vt, int32_t id, fe_rect_t r) {
    uint32_t c = fake(0xA0);
    put32(c, vt);
    put32(c + 0x08, 4 | 2);
    put32(c + 0x0C, (uint32_t)id);
    put32(c + 0x10, (uint32_t)r.left);
    put32(c + 0x14, (uint32_t)r.top);
    put32(c + 0x18, (uint32_t)r.right);
    put32(c + 0x1C, (uint32_t)r.bottom);
    return c;
}

static fe_rect_t rect_of(uint32_t c) {
    return fe_rect((int32_t)get32(c + 0x10), (int32_t)get32(c + 0x14), (int32_t)get32(c + 0x18), (int32_t)get32(c + 0x1C));
}

static int visible(uint32_t c) { return (get32(c + 0x08) & 4) != 0; }

static uint32_t button(int32_t id, int x, int y, int w, int h) {
    uint32_t c = control(VT_BUTTON, id, fe_rect(x, y, x + w, y + h));
    put32(c + 0x2C, raster(w, h, 0x1234));
    return c;
}

static uint32_t label(int32_t id, fe_rect_t r, const char* s, int border) {
    uint32_t c = control(VT_TEXTBOX, id, r);
    put32(c + 0x28, text(s));
    put32(c + 0x30, 0x00FFFFFF);
    put32(c + 0x3C, border ? 0x24 : 0x0);
    put32(c + 0x40, 1);
    put32(c + 0x44, (uint32_t)border);
    *(uint8_t*)(uintptr_t)(c + 0x48) = 1;
    *(uint8_t*)(uintptr_t)(c + 0x4E) = 12;
    return c;
}

static uint32_t window(uint32_t vt, fe_rect_t r, const uint32_t* controls, int n, uint32_t background) {
    uint32_t w = fake(0x40), v = fake(4 * (size_t)(n > 0 ? n : 1));
    put32(w, vt);
    put32(w + 0x10, (uint32_t)r.left);
    put32(w + 0x14, (uint32_t)r.top);
    put32(w + 0x18, (uint32_t)r.right);
    put32(w + 0x1C, (uint32_t)r.bottom);
    for (int i = 0; i < n; i++) put32(v + 4u * (uint32_t)i, controls[i]);
    put32(w + 0x20, v);
    put32(w + 0x24, v + 4u * (uint32_t)n);
    put32(w + 0x2C, background);
    return w;
}

static uint32_t g_manager;

static void stack(const uint32_t* windows, int n) {
    uint32_t main = fake(0x10), v = fake(4 * (size_t)n);
    g_manager = fake(0x60);
    for (int i = 0; i < n; i++) put32(v + 4u * (uint32_t)i, windows[i]);
    put32(main + 0x08, g_manager);
    put32(g_manager + 0x18, 0);
    put32(g_manager + 0x1C, 0);
    put32(g_manager + 0x20, 1);
    put32(g_manager + 0x28, raster(32, 32, 0));
    put32(g_manager + 0x38, v);
    put32(g_manager + 0x3C, v + 4u * (uint32_t)n);
    g_main = main;
}

static uint32_t main_window(int with_buttons) {
    uint32_t c[6];
    int n = 0;
    if (with_buttons) {
        for (int i = 0; i < 4; i++) c[n++] = button(1000 + i, 205, 70 + 90 * i, 208, 81);
        c[n++] = control(VT_BUTTON, 1004, fe_rect(0, 430, 100, 479));
        c[n] = control(VT_STATIC, 1005, fe_rect(550, 393, 623, 468));
        put32(c[n] + 0x24, raster(73, 75, 0x07E0));
        *(uint8_t*)(uintptr_t)(c[n] + 0x20) = 1;
        n++;
    }
    return window(VT_MAIN, fe_rect(0, 0, 640, 480), c, n, raster(640, 480, 0x2104));
}

static fe_scene_t g_scene;

static const fe_widget_t* find(const fe_scene_t* s, int kind, int32_t id) {
    for (int i = 0; i < s->widget_count; i++)
        if (s->widgets[i].kind == kind && s->widgets[i].id == id) return &s->widgets[i];
    return NULL;
}

static int has_text(const fe_scene_t* s, const char* t) {
    for (int i = 0; i < s->widget_count; i++)
        if (!strcmp(fe_scene_str(s, s->widgets[i].text), t)) return 1;
    return 0;
}

static fe_rect_t at(fe_rect_t w, int l, int t, int r, int b) { return fe_rect(w.left + l, w.top + t, w.left + r, w.top + b); }

static void test_options(void) {
    fe_rect_t wr = fe_rect(160, 140, 480, 340), before[5];
    uint32_t c[6], w;
    const int32_t ids[5] = { 1000, 1001, 1002, 1003, 1004 };
    c[0] = control(VT_STATIC, -1, at(wr, 0, 0, 320, 200));
    put32(c[0] + 0x24, raster(320, 200, 0x4208));
    c[1] = button(1000, wr.left + 28, wr.top + 74, 130, 42);
    c[2] = button(1001, wr.left + 162, wr.top + 33, 87, 36);
    c[3] = button(1002, wr.left + 71, wr.top + 33, 87, 36);
    c[4] = button(1003, wr.left + 162, wr.top + 75, 115, 42);
    c[5] = button(1004, wr.left + 116, wr.top + 122, 87, 36);
    w = window(VT_OPTIONS, wr, c, 6, 0);
    frontend_hd_window_centred(w);
    CHECK(!visible(c[2]) && !(get32(c[2] + 0x08) & 2), "the Video entry is still visible or enabled");
    for (int i = 0; i < 5; i++) before[i] = rect_of(c[i + 1]);
    for (int i = 1; i < 6; i++) {
        fe_rect_t r = rect_of(c[i]);
        if (i == 2) continue;
        CHECK(r.left >= wr.left + 8 && r.right <= wr.right - 8, "options entry %d leaves the window: %d..%d", ids[i - 1],
              r.left, r.right);
        CHECK(r.top >= wr.top + 8 && r.bottom <= wr.bottom - 8, "options entry %d leaves the window", ids[i - 1]);
        for (int k = 1; k < i; k++)
            if (k != 2) CHECK(fe_rect_empty(fe_rect_intersect(r, rect_of(c[k]))), "options entries overlap");
    }
    CHECK(rect_of(c[5]).top > rect_of(c[4]).bottom && rect_of(c[5]).top > rect_of(c[1]).bottom, "Close is not last");
    CHECK(rect_of(c[3]).top < rect_of(c[1]).top && rect_of(c[3]).top < rect_of(c[4]).top &&
              rect_of(c[1]).right <= rect_of(c[4]).left,
          "the entries are not Audio high, Controls left and Credits right");
    frontend_hd_window_centred(w);
    for (int i = 0; i < 5; i++) CHECK(fe_rect_equal(before[i], rect_of(c[i + 1])), "a second centring moved entry %d", ids[i]);
    {
        uint32_t ws[2];
        ws[0] = main_window(0);
        ws[1] = w;
        stack(ws, 2);
    }
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080), "the Options scene is refused");
    CHECK(g_scene.window_count == 2 && g_scene.windows[0].kind == FE_WND_MAIN && g_scene.windows[1].kind == FE_WND_OPTIONS,
          "Options scene windows");
    CHECK(g_scene.windows[0].count == 0, "the main screen's buttons are drawn under a dialog");
    CHECK(g_scene.backdrop != NULL, "the main screen's art is missing");
    CHECK(!find(&g_scene, FE_W_BUTTON, 1001) && !has_text(&g_scene, "VIDEO"), "Video is in the frontend");
    CHECK(has_text(&g_scene, "AUDIO") && has_text(&g_scene, "CONTROLS") && has_text(&g_scene, "CREDITS") &&
              has_text(&g_scene, "CLOSE"),
          "an Options entry is missing");
    CHECK(g_scene.windows[1].count == 4, "Options shows %d widgets", g_scene.windows[1].count);
    free_fakes();
}

static void test_options_in_game(void) {
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    uint32_t c[4], w, ws[2];
    c[0] = button(1000, wr.left + 100, wr.top + 74, 130, 42);
    c[1] = button(1001, wr.left + 162, wr.top + 33, 87, 36);
    c[2] = button(1002, wr.left + 71, wr.top + 33, 87, 36);
    c[3] = button(1004, wr.left + 116, wr.top + 122, 87, 36);
    w = window(VT_OPTIONS, wr, c, 4, 0);
    frontend_hd_window_centred(w);
    CHECK(!visible(c[1]), "the in-game Video entry is still visible");
    CHECK(rect_of(c[2]).top < rect_of(c[0]).top && rect_of(c[0]).bottom < rect_of(c[3]).top &&
              rect_of(c[2]).bottom < rect_of(c[3]).top,
          "in-game Options order");
    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1440), "the in-game Options scene is refused");
    CHECK(g_scene.windows[0].kind == FE_WND_GAME && !g_scene.backdrop, "the game view under in-game Options");
    CHECK(!has_text(&g_scene, "VIDEO") && g_scene.windows[1].count == 3, "in-game Options entries");
    free_fakes();
}

static void test_audio(void) {
    static const char* names[] = { "Play Sound Effects", "Play Ambient Sounds", "Play Voice Overs", "Play Background Music",
                                   "Show Subtitles", "3D Audio Accel. Enabled" };
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    uint32_t c[32], checks[6], labels[6], hotspots[6], w, ws[2];
    int n = 0, tops[5];
    c[n++] = button(1000, wr.left + 185, wr.top + 155, 118, 42);
    c[n++] = button(1001, wr.left + 22, wr.top + 155, 62, 42);
    c[n] = control(VT_SLIDER, 1004, at(wr, 144, 20, 290, 43));
    put32(c[n] + 0x2C, 101);
    put32(c[n] + 0x30, 80);
    n++;
    for (int k = 0; k < 6; k++) {
        int y = 65 + 15 * k;
        checks[k] = c[n++] = control(VT_CHECKBOX, 1020 + k, at(wr, 80, y, 92, y + 12));
        put32(checks[k] + 0x28, k < 4 ? 2 : 0);
        labels[k] = c[n++] = label(-1, at(wr, 120, y - 2, 300, y + 15), names[k], 0);
        hotspots[k] = c[n++] = control(VT_HOTSPOT, 1100 + k, at(wr, 80, y, 300, y + 15));
    }
    w = window(VT_AUDIO, wr, c, n, 0);
    frontend_hd_window_centred(w);
    CHECK(!visible(checks[5]) && !visible(labels[5]) && !visible(hotspots[5]), "the 3D audio row is still visible");
    fe_layout_rows(5, wr.top + 65, wr.top + 155, 15, tops);
    for (int k = 0; k < 5; k++) {
        fe_rect_t rc = rect_of(checks[k]), rl = rect_of(labels[k]), rh = rect_of(hotspots[k]);
        CHECK(visible(checks[k]) && visible(labels[k]) && visible(hotspots[k]), "audio row %d hidden", k);
        CHECK(rc.top == tops[k], "audio row %d at %d, not %d", k, rc.top, tops[k]);
        CHECK(rl.top == rc.top - 2 && rh.top == rc.top, "audio row %d came apart (label %d, hotspot %d, box %d)", k, rl.top,
              rh.top, rc.top);
        if (k) CHECK(rc.top - rect_of(checks[k - 1]).top >= 15, "audio rows %d and %d overlap", k - 1, k);
    }
    frontend_hd_window_centred(w);
    for (int k = 0; k < 5; k++) CHECK(rect_of(checks[k]).top == tops[k], "a second centring moved audio row %d", k);
    ws[0] = main_window(0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 3440, 1440), "the Audio scene is refused");
    CHECK(!has_text(&g_scene, "3D Audio Accel. Enabled") && !find(&g_scene, FE_W_CHECK, 1025), "3D audio is in the frontend");
    for (int k = 0; k < 5; k++) {
        const fe_widget_t* x = find(&g_scene, FE_W_CHECK, 1020 + k);
        CHECK(x && ((x->state & FE_ST_CHECKED) != 0) == (k < 4), "audio check box %d state", k);
        CHECK(has_text(&g_scene, names[k]), "audio label %d missing", k);
    }
    {
        const fe_widget_t* s = find(&g_scene, FE_W_SLIDER, 1004);
        CHECK(s && s->range == 101 && s->value == fe_slider_thumb_x(s->rect, 10, 80, 101), "the volume slider's thumb");
        CHECK(has_text(&g_scene, "Master Volume"), "the volume slider has no caption");
    }
    free_fakes();
}

static void test_main_and_frame(void) {
    uint32_t ws[1], b1;
    fe_widget_t* hovered;
    ws[0] = main_window(1);
    stack(ws, 1);
    CHECK(frontend_hd_read_scene(&g_scene, 1024, 768), "the main screen is refused");
    for (int i = 0; i < 4; i++) {
        const fe_widget_t* x = find(&g_scene, FE_W_BUTTON, 1000 + i);
        CHECK(x && x->style == FE_BUTTON_LARGE, "main menu button %d", 1000 + i);
    }
    CHECK(has_text(&g_scene, "NEW GAME") && has_text(&g_scene, "LOAD") && has_text(&g_scene, "OPTIONS") &&
              has_text(&g_scene, "EXIT"),
          "main menu labels");
    CHECK(!find(&g_scene, FE_W_BUTTON, 1004), "the main screen's pictureless button is drawn");
    {
        static const int lengths[4] = { 8, 4, 7, 4 };
        fe_rect_t planned[4];
        uint32_t controls = get32(get32(get32(get32(g_main + 8) + 0x38)) + 0x20);
        fe_layout_main_menu(lengths, 4, planned);
        for (int i = 0; i < 4; i++) {
            const fe_widget_t* x = find(&g_scene, FE_W_BUTTON, 1000 + i);
            CHECK(fe_rect_equal(rect_of(get32(controls + 4u * (uint32_t)i)), planned[i]), "main menu button %d not moved", i);
            CHECK(x && fe_rect_equal(x->rect, planned[i]), "main menu plaque %d drawn away from its button", i);
        }
    }
    CHECK(find(&g_scene, FE_W_IMAGE, 1005) != NULL, "the main screen's logo is missing");
    memset(&g_layer, 0, sizeof g_layer);
    CHECK(frontend_hd_frame(1024, 768) == 1, "the main screen frame falls back");
    CHECK(g_layer.pixels && g_layer.has_dirty && g_layer.dirty.right - g_layer.dirty.left == 1024 &&
              g_layer.dirty.bottom - g_layer.dirty.top == 768,
          "the first frame is not painted whole");
    frontend_hd_frame_done();
    CHECK(frontend_hd_frame(1024, 768) == 1 && !g_layer.has_dirty, "an unchanged frame is painted again");
    frontend_hd_frame_done();
    b1 = get32(get32(get32(g_main + 8) + 0x38));
    b1 = get32(get32(b1 + 0x20) + 4);
    put32(b1 + 0x28, 2);
    CHECK(frontend_hd_frame(1024, 768) == 1 && g_layer.has_dirty, "a hovered button is not painted");
    hovered = (fe_widget_t*)find(&g_scene, FE_W_BUTTON, 1001);
    if (hovered) {
        fe_view_t v = fe_view_make(1024, 768);
        fe_rect_t r = fe_view_rect(&v, hovered->rect), d = fe_rect(g_layer.dirty.left, g_layer.dirty.top, g_layer.dirty.right,
                                                                   g_layer.dirty.bottom);
        CHECK(fe_rect_equal(fe_rect_intersect(r, d), r) && (d.bottom - d.top) < 768 / 2,
              "the repaint of a hovered button is not just around it");
    }
    frontend_hd_frame_done();
    for (int i = 0; i < 200; i++) frontend_hd_idle();
    free_fakes();
}

static void test_lists_fields_messages(void) {
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    uint32_t c[8], items, list, up, down, w, ws[2];
    char name[32];
    int n = 0;
    list = control(VT_LISTBOX, 1002, at(wr, 124, 14, 300, 150));
    items = fake(12 * 12);
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof name, "Save %02d", i);
        put32(items + 12u * (uint32_t)i, text(name));
    }
    put32(list + 0x24, items);
    put32(list + 0x28, items + 12 * 12);
    up = button(0, wr.left + 284, wr.top + 14, 16, 16);
    down = button(0, wr.left + 284, wr.top + 134, 16, 16);
    put32(list + 0x30, up);
    put32(list + 0x34, down);
    put32(list + 0x38, 0x4 | 0x2);
    put32(list + 0x3C, 4);
    put32(list + 0x40, 2);
    put32(list + 0x44, 9);
    put32(list + 0x48, 16);
    put32(list + 0x4C, 15);
    put32(list + 0x68, 0x00FFFFFF);
    *(uint8_t*)(uintptr_t)(list + 0x6C) = 12;
    c[n++] = list;
    c[n++] = up;
    c[n++] = down;
    c[n++] = button(1001, wr.left + 15, wr.top + 159, 62, 42);
    c[n++] = button(1004, wr.left + 82, wr.top + 159, 104, 42);
    put32(c[n - 1] + 0x08, 4);
    c[n++] = button(1000, wr.left + 190, wr.top + 159, 118, 42);
    w = window(VT_LOAD, wr, c, n, 0);
    ws[0] = main_window(0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1200), "the Load scene is refused");
    {
        const fe_widget_t* l = find(&g_scene, FE_W_LIST, 1002);
        CHECK(l && l->item_count == 12 && l->top == 2 && l->selected == 4 && l->rows == 9 && l->row_h == 15,
              "the save list's geometry");
        CHECK(l && fe_list_shown(l) == 9 && !strcmp(fe_scene_str(&g_scene, g_scene.items[l->first_item]), "Save 02"),
              "the save list's shown items");
        CHECK(l && !fe_rect_empty(l->track), "the save list's scroll bar is missing");
    }
    {
        int arrows = 0;
        for (int i = 0; i < g_scene.widget_count; i++)
            if (g_scene.widgets[i].kind == FE_W_BUTTON &&
                (g_scene.widgets[i].style == FE_BUTTON_ARROW_UP || g_scene.widgets[i].style == FE_BUTTON_ARROW_DOWN))
                arrows++;
        CHECK(arrows == 2, "the list's arrows are %d", arrows);
        CHECK(find(&g_scene, FE_W_BUTTON, 1004) && (find(&g_scene, FE_W_BUTTON, 1004)->state & FE_ST_DISABLED),
              "the disabled Delete");
    }
    free_fakes();

    n = 0;
    c[n] = label(1031, at(wr, 94, 60, 164, 74), "Shift", 1);
    *(uint8_t*)(uintptr_t)(c[n] + 0x48) = 0;
    n++;
    c[n++] = label(-1, at(wr, 10, 60, 90, 74), "Run Forward", 0);
    c[n++] = control(VT_HOTSPOT, 1011, at(wr, 10, 60, 164, 74));
    c[n++] = button(1001, wr.left + 4, wr.top + 157, 62, 42);
    w = window(VT_CONTROLS, wr, c, n, 0);
    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    ws[1] = w;
    stack(ws, 2);
    put32(g_manager + 0x18, (uint32_t)(wr.left + 40));
    put32(g_manager + 0x1C, (uint32_t)(wr.top + 66));
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1080), "the Controls scene is refused");
    {
        const fe_widget_t* f = find(&g_scene, FE_W_FIELD, 1031);
        CHECK(f && (f->state & FE_ST_FOCUS), "the key being bound is not marked");
        CHECK(f && (f->state & FE_ST_ROW_HOVER), "the row under the pointer is not marked");
    }
    free_fakes();

    c[0] = button(1000, wr.left + 80, wr.top + 160, 80, 42);
    c[1] = button(1001, wr.left + 165, wr.top + 160, 65, 42);
    c[2] = label(1002, at(wr, 10, 10, 320, 155), "Quit the game?", 0);
    ws[0] = main_window(0);
    ws[1] = window(VT_YESNO, wr, c, 3, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1280, 720), "the Yes/No box is refused");
    CHECK(has_text(&g_scene, "YES") && has_text(&g_scene, "NO") && has_text(&g_scene, "Quit the game?"), "Yes/No box text");
    free_fakes();

    c[0] = button(1000, wr.left + 205, wr.top + 190, 62, 42);
    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    ws[1] = window(VT_OK, wr, c, 1, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1280, 720) && has_text(&g_scene, "OK"), "the OK box");
    CHECK(g_scene.windows[1].panel.bottom >= wr.top + 232, "the OK box's plate does not reach its button");
    free_fakes();
}


/* Load's entries: 84 bytes each, the save's file name at +0x28, in list order. */
static uint32_t entries_of(const char* const* names, int n) {
    uint32_t v = fake(84 * (size_t)n);
    for (int i = 0; i < n; i++) strcpy((char*)(uintptr_t)(v + 84u * (uint32_t)i + 0x28), names[i]);
    return v;
}

static uint32_t save_list(fe_rect_t r, int selected, int n) {
    uint32_t list = control(VT_LISTBOX, 1002, r), items = fake(12 * (size_t)n);
    for (int i = 0; i < n; i++) put32(items + 12u * (uint32_t)i, text("a save"));
    put32(list + 0x24, items);
    put32(list + 0x28, items + 12u * (uint32_t)n);
    put32(list + 0x3C, (uint32_t)selected);
    put32(list + 0x44, 9);
    put32(list + 0x48, 16);
    put32(list + 0x4C, 15);
    *(uint8_t*)(uintptr_t)(list + 0x6C) = 12;
    return list;
}

static const fe_widget_t* photo(void) {
    for (int i = 0; i < g_scene.widget_count; i++)
        if (g_scene.widgets[i].kind == FE_W_IMAGE && g_scene.widgets[i].style == FE_IMAGE_PHOTO) return &g_scene.widgets[i];
    return NULL;
}

static void test_save_pictures(void) {
    static const char* names[] = { "SAVEGAME.000", "SAVEGAME.002" };
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    char path[MAX_PATH], picture[MAX_PATH];
    uint32_t c[3], w, ws[2], thumb;
    GetTempPathA(sizeof g_user_dir, g_user_dir);
    _snprintf(g_user_dir + strlen(g_user_dir), sizeof g_user_dir - strlen(g_user_dir) - 1, "jpt_bridge_%lu",
              GetCurrentProcessId());
    CreateDirectoryA(g_user_dir, NULL);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) g_frozen[y][x] = (uint16_t)(x < 320 ? 0xF800 : 0x07E0);
    _snprintf(path, sizeof path - 1, "%s\\SAVEGAME.002", g_user_dir);
    save_preview_path(path, picture, sizeof picture);

    frontend_hd_save_opened(path, g_user_dir);
    CHECK(GetFileAttributesA(picture) != INVALID_FILE_ATTRIBUTES, "writing a save leaves no HD picture");
    frontend_hd_save_opened("C:\\elsewhere\\SAVEGAME.002", g_user_dir);
    frontend_hd_save_opened(path, "C:\\elsewhere");

    c[0] = save_list(at(wr, 124, 14, 300, 150), 1, 2);
    c[1] = control(VT_STATIC, 1003, at(wr, 13, 10, 113, 85));
    thumb = raster(100, 75, 0x1234);
    put32(c[1] + 0x24, thumb);
    c[2] = button(1000, wr.left + 190, wr.top + 159, 118, 42);
    w = window(VT_LOAD, wr, c, 3, 0);
    put32(w + 0x34, c[0]);
    put32(w + 0x38, entries_of(names, 2));
    put32(w + 0x3C, get32(w + 0x38) + 2 * 84);
    ws[0] = main_window(0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1440), "Load with save pictures is refused");
    CHECK(photo() && photo()->image32 && photo()->image_w == save_preview_size(640, 480).width,
          "the selected save's HD picture is not shown");
    CHECK(photo() && fe_rect_equal(photo()->rect, fe_layout_photo(FE_WND_LOAD, wr)), "the picture frame is misplaced");
    CHECK(!find(&g_scene, FE_W_IMAGE, 1003) || find(&g_scene, FE_W_IMAGE, 1003)->style == FE_IMAGE_PHOTO,
          "the game's small thumbnail is drawn too");
    CHECK(g_scene.windows[1].panel.left == wr.left - FE_PANEL_MARGIN, "the picture frame widens the plate");

    put32(c[0] + 0x3C, 0);
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1440) && photo() && !photo()->image32 &&
              photo()->image == (const uint16_t*)(uintptr_t)get32(thumb + 0x18),
          "a save without an HD picture does not fall back to its thumbnail");

    frontend_hd_save_deleted(path, g_user_dir);
    CHECK(GetFileAttributesA(picture) == INVALID_FILE_ATTRIBUTES, "deleting a save leaves its HD picture");
    put32(c[0] + 0x3C, 1);
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1440) && photo() && !photo()->image32 && photo()->image,
          "a deleted HD picture is still shown");
    free_fakes();

    c[0] = save_list(at(wr, 125, 41, 297, 155), -1, 0);
    c[1] = button(1000, wr.left + 190, wr.top + 159, 118, 42);
    w = window(0x006462A8u, wr, c, 2, 0);
    put32(w + 0x38, c[0]);
    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 2560, 1440) && photo() && photo()->image32,
          "Save with nothing selected does not show the picture a new save gets");
    free_fakes();
    {
        char previews[MAX_PATH];
        _snprintf(previews, sizeof previews - 1, "%s\\previews", g_user_dir);
        RemoveDirectoryA(previews);
        RemoveDirectoryA(g_user_dir);
    }
}

static void test_fallbacks(void) {
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    uint32_t c[2], ws[2];
    ws[0] = main_window(0);
    ws[1] = window(VT_VIDEO, wr, NULL, 0, 0);
    stack(ws, 2);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "a video window is drawn by the frontend");
    CHECK(frontend_hd_frame(1920, 1080) == 0, "the frame does not fall back for a video window");
    free_fakes();

    c[0] = control(VT_PROGRESS, 100, at(wr, 29, 81, 288, 84));
    ws[0] = main_window(0);
    ws[1] = window(VT_OPTIONS, wr, c, 1, 0);
    stack(ws, 2);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "a progress bar is drawn by the frontend");
    free_fakes();

    c[0] = control(0x00651234u, 7, at(wr, 10, 10, 40, 40));
    ws[0] = main_window(0);
    ws[1] = window(VT_AUDIO, wr, c, 1, 0);
    stack(ws, 2);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "an unknown control is drawn by the frontend");
    free_fakes();

    c[0] = button(1999, wr.left + 10, wr.top + 10, 80, 40);
    ws[0] = main_window(0);
    ws[1] = window(VT_OPTIONS, wr, c, 1, 0);
    stack(ws, 2);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "a button without a known label is drawn by the frontend");
    free_fakes();

    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    stack(ws, 1);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "the gameplay view alone is drawn by the frontend");
    free_fakes();

    ws[0] = window(VT_MAIN, fe_rect(0, 0, 640, 480), NULL, 0, raster(320, 240, 0));
    stack(ws, 1);
    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "a main screen without its 640x480 art is drawn");
    free_fakes();

    CHECK(!frontend_hd_read_scene(&g_scene, 1920, 1080), "a scene is read without a window stack");
}

void frontend_bridge_tests(int* checks, int* failures) {
    g_checks = checks;
    g_failures = failures;
    test_options();
    test_options_in_game();
    test_audio();
    test_main_and_frame();
    test_lists_fields_messages();
    test_save_pictures();
    test_fallbacks();
}
