/* The HD frontend's bridge (frontend_hd.c) against fake game objects laid out as the retail ones: what it reads from
 * each window, the Options and Audio edits, and the fallbacks. A 32-bit build, so host pointers are guest addresses. */
#include "frontend_hd.h"
#include "gamepad.h"
#include "host.h"
#include "l10n.h"
#include "level_session.h"
#include "loading_hd.h"
#include "save_preview.h"

#include <math.h>
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
static uint32_t g_main, g_scene_name, g_art_table, g_screen_raster;

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
    g_scene_name = g_art_table = g_screen_raster = 0;
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

/* The game's data at fixed addresses: the loading window's scene name and picture table. */
#define SCENE_NAME 0x0066E342u
#define ART_TABLE 0x006610D0u
#define ART_TABLE_BYTES (10u * 0x28u)
#define SCREEN_RASTER 0x0073DC40u
#define SCREEN_RASTER 0x0073DC40u

static uint32_t fixed(uint32_t va) {
    if (g_scene_name && va >= SCENE_NAME && va < SCENE_NAME + 64u) return g_scene_name + (va - SCENE_NAME);
    if (g_art_table && va >= ART_TABLE && va < ART_TABLE + ART_TABLE_BYTES) return g_art_table + (va - ART_TABLE);
    if (g_screen_raster && va == SCREEN_RASTER) return g_screen_raster;
    return va;
}

int guest_read32(uint32_t va, uint32_t* out) {
    if (va == MAIN_WINDOW_OBJECT && g_main) {
        *out = g_main;
        return 1;
    }
    va = fixed(va);
    if (!inside(va, 4)) return 0;
    *out = get32(va);
    return 1;
}

int guest_readable(uint32_t va, uint32_t bytes) { return bytes == 0 || inside(fixed(va), bytes); }

int guest_read_string(uint32_t va, char* out, int cap) {
    int n = 0;
    va = fixed(va);
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
    int has_dirty, calls, w, h;
} g_layer;

void d3dr_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty) {
    if (!pixels) return;
    g_layer.w = w;
    g_layer.h = h;
    g_layer.calls++;
    g_layer.pixels = pixels;
    g_layer.has_dirty = dirty != NULL;
    if (dirty) g_layer.dirty = *dirty;
}

/* The language's menu labels as the frontend and the loading screen ask for them: none until a test sets some. */
static const char* s_label_from;
static const char* s_label_to;

int l10n_release_language(void) { return 0; }
const char* l10n_label(const char* english) { return s_label_from && !strcmp(english, s_label_from) ? s_label_to : NULL; }

void d3dr_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    (void)sprite;
    (void)size;
    (void)generation;
    (void)x;
    (void)y;
}

int d3dr_frontend_failed(void) { return 0; }
int d3dr_frame_size(int guest_w, int guest_h, int* w, int* h) {
    *w = guest_w * 3;
    *h = guest_h * 3;
    return 1;
}

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
void window_host_level_load_done(void) {}
void d3dr_texture_polygon_reset(void) {}
void d3dr_texture_polygon_warm(void) {}
void guest_threads_trace(const char* event) { (void)event; }

int window_host_pointer(int* x, int* y) {
    *x = *y = 0;
    return 0;
}

/* loading_hd.c's hooks are never entered here: only its reader runs. */
uint32_t g_eax, g_ecx, g_esp;
int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    (void)va;
    (void)replacement;
    (void)lifted;
    return 0;
}
static int g_hardware = 1;
int d3dr_hardware(void) { return g_hardware; }
int guest_display_built(void) { return 1; }
int guest_ui_probe(guest_ui_view_t* out) { return (void)out, 0; }
int guest_host_path(const char* guest_path, char* out, size_t cap) { return snprintf(out, cap, "%s", guest_path) > 0; }
jpt_level_source_t level_session_source(const char* scene) { return (void)scene, JPT_SOURCE_RETAIL; }
void debug_menu_notice(const char* text) { (void)text; }
int custom_level_file(const char* path, char* out, size_t cap) {
    (void)path;
    (void)out;
    (void)cap;
    return 0;
}
int guest_load_string(unsigned id, char* out, int cap) {
    (void)id;
    (void)out;
    (void)cap;
    return 0;
}

static int g_pad_in_use;
static jpt_gamepad_settings_t g_pad_settings;
int gamepad_menu_focus(void) { return 0; }
void gamepad_prompts(gamepad_prompts_t* out) {
    out->gamepad = g_pad_in_use;
    out->family = JPT_FAMILY_PLAYSTATION;
    out->settings = &g_pad_settings;
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
    for (int slot = 0; slot < 4; slot++) put32(c + 0x2C + 4u * (uint32_t)slot, raster(w, h, (uint16_t)(0x1234 + slot * 0x111)));
    put32(c + 0x28, 1);
    return c;
}

static uint32_t checkbox(int32_t id, fe_rect_t r, int checked) {
    uint32_t c = control(VT_CHECKBOX, id, r);
    for (int i = 0; i < 4; i++) put32(c + 0x2C + 4u * (uint32_t)i, raster(12, 12, (uint16_t)(0x2000 + i * 0x20)));
    put32(c + 0x28, checked ? 2 : 0);
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
    CHECK(!find(&g_scene, FE_W_BUTTON, 1001), "Video is in the frontend");
    for (int k = 0; k < 5; k++) {
        const fe_widget_t* x = find(&g_scene, FE_W_BUTTON, ids[k]);
        if (ids[k] != 1001) CHECK(x && x->image && x->image_w > 0, "Options entry %d has no picture", ids[k]);
    }
    CHECK(find(&g_scene, FE_W_IMAGE, -1) != NULL, "the Options plate picture is missing");
    CHECK(g_scene.windows[1].count == 5, "Options shows %d widgets", g_scene.windows[1].count);
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
    CHECK(!find(&g_scene, FE_W_BUTTON, 1001) && g_scene.windows[1].count == 3, "in-game Options entries");
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
        checks[k] = c[n++] = checkbox(1020 + k, at(wr, 80, y, 92, y + 12), k < 4);
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
    s_label_from = "Play Voice Overs";
    s_label_to = "Reproduzir vozes";
    CHECK(frontend_hd_read_scene(&g_scene, 3440, 1440) && has_text(&g_scene, s_label_to) && !has_text(&g_scene, "Play Voice Overs") &&
              has_text(&g_scene, "Play Ambient Sounds"),
          "a label of the game's own text boxes is read translated, the others as they are");
    s_label_from = s_label_to = NULL;
    CHECK(frontend_hd_read_scene(&g_scene, 3440, 1440), "the Audio scene is read again");
    for (int k = 0; k < 5; k++) {
        const fe_widget_t* x = find(&g_scene, FE_W_CHECK, 1020 + k);
        CHECK(x && ((x->state & FE_ST_CHECKED) != 0) == (k < 4), "audio check box %d state", k);
        CHECK(x && x->image && x->image == (const uint16_t*)(uintptr_t)get32(get32(checks[k] + 0x2C + 4u * (k < 4 ? 2u : 0u)) + 0x18),
              "audio check box %d shows the wrong picture", k);
        CHECK(has_text(&g_scene, names[k]), "audio label %d missing", k);
    }
    {
        const fe_widget_t* s = find(&g_scene, FE_W_SLIDER, 1004);
        CHECK(s && s->range == 101 && s->value == fe_slider_thumb_x(s->rect, 10, 80, 101), "the volume slider's thumb");
        CHECK(!has_text(&g_scene, "Master Volume"), "a caption the game does not have");
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
        CHECK(x && x->image && x->image_w == 208 && x->image_h == 81, "main menu button %d shows the game's picture", 1000 + i);
    }
    CHECK(!find(&g_scene, FE_W_BUTTON, 1004), "the main screen's pictureless button is drawn");
    {
        uint32_t controls = get32(get32(get32(get32(g_main + 8) + 0x38)) + 0x20);
        for (int i = 0; i < 4; i++) {
            fe_rect_t original = fe_rect(205, 70 + 90 * i, 205 + 208, 70 + 90 * i + 81);
            const fe_widget_t* x = find(&g_scene, FE_W_BUTTON, 1000 + i);
            CHECK(fe_rect_equal(rect_of(get32(controls + 4u * (uint32_t)i)), original), "main menu button %d was moved", i);
            CHECK(x && fe_rect_equal(x->rect, original), "main menu plaque %d is not at its original place", i);
        }
    }
    CHECK(find(&g_scene, FE_W_IMAGE, 1005) != NULL, "the main screen's logo is missing");
    memset(&g_layer, 0, sizeof g_layer);
    CHECK(frontend_hd_frame(1024, 768) == 1, "the main screen frame falls back");
    CHECK(g_layer.pixels && g_layer.w == 3072 && g_layer.h == 2304, "the layer is the renderer's frame size, %dx%d", g_layer.w, g_layer.h);
    CHECK(g_layer.pixels && g_layer.has_dirty && g_layer.dirty.right - g_layer.dirty.left == 3072 &&
              g_layer.dirty.bottom - g_layer.dirty.top == 2304,
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
        fe_view_t v = fe_view_make_for(1024, 768, 3072, 2304);
        fe_rect_t r = fe_view_rect(&v, hovered->rect), d = fe_rect(g_layer.dirty.left, g_layer.dirty.top, g_layer.dirty.right,
                                                                   g_layer.dirty.bottom);
        CHECK(fe_rect_equal(fe_rect_intersect(r, d), r) && (d.bottom - d.top) < 2304 / 2,
              "the repaint of a hovered button is not just around it");
    }
    frontend_hd_frame_done();
    {
        /* the hovered plaque carries its normal picture, so the painter can lift its red lettering */
        static fe_scene_t now;
        const fe_widget_t *h, *other;
        CHECK(frontend_hd_read_scene(&now, 1024, 768), "the hovered main screen is refused");
        h = find(&now, FE_W_BUTTON, 1001);
        other = find(&now, FE_W_BUTTON, 1000);
        CHECK(h && (h->state & FE_ST_HOVER) && h->normal_image &&
                  h->normal_image == (const uint16_t*)(uintptr_t)get32(get32(b1 + 0x2C) + 0x18) &&
                  h->normal_pitch == h->image_w,
              "a hovered plaque does not carry its normal picture for the red lift");
        CHECK(other && !other->normal_image, "a plaque that is not hovered carries a normal picture");
    }
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
        CHECK(f && f->filled, "the box that paints its own background");
        CHECK(f && f->border, "the field's sunken edge");
    }
    free_fakes();

    c[0] = button(1000, wr.left + 80, wr.top + 160, 80, 42);
    c[1] = button(1001, wr.left + 165, wr.top + 160, 65, 42);
    c[2] = label(1002, at(wr, 10, 10, 320, 155), "Quit the game?", 0);
    ws[0] = main_window(0);
    ws[1] = window(VT_YESNO, wr, c, 3, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1280, 720), "the Yes/No box is refused");
    CHECK(find(&g_scene, FE_W_BUTTON, 1000) && find(&g_scene, FE_W_BUTTON, 1001) && has_text(&g_scene, "Quit the game?"),
          "Yes/No box pictures and text");
    CHECK(find(&g_scene, FE_W_BUTTON, 1000)->image && find(&g_scene, FE_W_LABEL, 1002)->weight == 0, "Yes/No box widgets");
    free_fakes();

    c[0] = button(1000, wr.left + 205, wr.top + 190, 62, 42);
    ws[0] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
    ws[1] = window(VT_OK, wr, c, 1, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1280, 720) && find(&g_scene, FE_W_BUTTON, 1000), "the OK box");
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
    CHECK(photo() && fe_rect_equal(photo()->rect, at(wr, 13, 10, 113, 85)), "the picture is not where the game's thumbnail is");
    CHECK(!find(&g_scene, FE_W_IMAGE, 1003) || find(&g_scene, FE_W_IMAGE, 1003)->style == FE_IMAGE_PHOTO,
          "the game's small thumbnail is drawn too");

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
    c[2] = control(VT_STATIC, 1003, at(wr, 17, 41, 117, 116));
    put32(c[2] + 0x24, raster(100, 75, 0x1234));
    w = window(0x006462A8u, wr, c, 3, 0);
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
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080) && g_scene.windows[1].count == 0,
          "an unknown control takes its whole window down to the game's own menu");
    free_fakes();

    c[0] = button(1999, wr.left + 10, wr.top + 10, 80, 40);
    c[1] = button(1998, wr.left + 110, wr.top + 10, 80, 40);
    put32(get32(c[1] + 0x2C) + 0x10, 8);
    ws[0] = main_window(0);
    ws[1] = window(VT_OPTIONS, wr, c, 2, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080), "a button the frontend has no name for is refused");
    CHECK(find(&g_scene, FE_W_BUTTON, 1999) && find(&g_scene, FE_W_BUTTON, 1999)->image, "an unnamed button has its picture");
    CHECK(find(&g_scene, FE_W_BUTTON, 1998) && !find(&g_scene, FE_W_BUTTON, 1998)->image,
          "a button with an unreadable picture is dropped alone");
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

/* ---- the loading window (CLoaderWnd from menu\loader.ddf, centred on the screen) ---- */

#define VT_LOADER 0x006467A0u
#define VT_DIRECT_LOAD 0x00646888u

static void game_tables(const char* scene) {
    g_art_table = fake(ART_TABLE_BYTES);
    for (int i = 0; i < LD_TABLE_ROWS; i++) {
        strcpy((char*)(uintptr_t)(g_art_table + 0x28u * (uint32_t)i), ld_retail_table[i].scene);
        strcpy((char*)(uintptr_t)(g_art_table + 0x28u * (uint32_t)i + 0x14u), ld_retail_table[i].image);
    }
    g_scene_name = fake(64);
    strcpy((char*)(uintptr_t)g_scene_name, scene);
}

/* loader.ddf's "progress 1 0 ...": enabled, not visible (the game draws its bars regardless). */
static uint32_t progress(int32_t id, fe_rect_t r, int32_t lo, int32_t hi, int32_t pos) {
    uint32_t c = control(VT_PROGRESS, id, r);
    put32(c + 0x08, 2);
    put32(c + 0x20, (uint32_t)lo);
    put32(c + 0x24, (uint32_t)hi);
    put32(c + 0x28, (uint32_t)pos);
    *(uint16_t*)(uintptr_t)(c + 0x30) = (uint16_t)((31u << 11) | (16u << 5));
    return c;
}

enum { L_RETAIL, L_BUTTON, L_PLEASE_WAIT, L_EMPTY_TEXT, L_HIDDEN_TEXT, L_THREE_TEXTS, L_CAPTIONS_MOVED, L_FIVE_BARS };

/* loader.ddf: two captions, two bar frames (statics) and two progress bars in a 320x200 box centred on the screen. */
static uint32_t loader(int sw, int sh, int extra, uint32_t background) {
    fe_rect_t b = fe_rect(sw / 2 - 160, sh / 2 - 100, sw / 2 + 160, sh / 2 + 100);
    uint32_t c[16];
    int n = 0, moved = extra == L_CAPTIONS_MOVED;
    c[n++] = label(1000, moved ? at(b, 0, 150, 320, 165) : at(b, 0, 45, 320, 60), "World Initialization", 0);
    c[n++] = control(VT_STATIC, -1, at(b, 23, 67, 297, 91));
    c[n++] = progress(100, at(b, 29, 81, 288, 84), 0, 99, 50);
    c[n++] = label(1001, moved ? at(b, 0, 5, 320, 20) : at(b, 0, 95, 320, 110), "Level File Copy", 0);
    c[n++] = control(VT_STATIC, -1, at(b, 23, 110, 297, 134));
    c[n++] = progress(101, at(b, 29, 116, 288, 119), 0, 99, 0);
    if (extra == L_BUTTON) c[n++] = control(VT_BUTTON, 1000, at(b, 120, 170, 200, 190));
    if (extra == L_PLEASE_WAIT || extra == L_THREE_TEXTS) c[n++] = label(102, at(b, 0, 15, 320, 40), "Please Wait", 0);
    if (extra == L_THREE_TEXTS) {
        c[n++] = label(103, at(b, 0, 150, 320, 165), "Second line", 0);
        c[n++] = label(104, at(b, 0, 170, 320, 185), "Third line", 0);
    }
    if (extra == L_EMPTY_TEXT) c[n++] = label(102, at(b, 0, 15, 320, 40), "", 0);
    if (extra == L_HIDDEN_TEXT) {
        c[n] = label(102, at(b, 0, 15, 320, 40), "Hidden", 0);
        put32(c[n++] + 0x08, 2);
    }
    if (extra == L_FIVE_BARS)
        for (int i = 0; i < 3; i++) c[n++] = progress(102 + i, at(b, 29, 140 + 10 * i, 288, 143 + 10 * i), 0, 9, i);
    return window(VT_LOADER, fe_rect(0, 0, sw, sh), c, n, background);
}

static int read_loader(int extra, ld_scene_t* s, char* slot) {
    uint32_t w;
    game_tables("BE.SCN");
    w = loader(2560, 1920, extra, raster(2560, 1920, 0));
    return loading_hd_read(w, 2560, 1920, s, slot, LD_SLOT_BYTES);
}

static int retail_gauges(const ld_scene_t* s) {
    return s->gauge_count == 2 && !strcmp(s->gauges[0].caption, "World Initialization") &&
           !strcmp(s->gauges[1].caption, "Level File Copy");
}

static void test_loading_window(void) {
    ld_scene_t s;
    char slot[LD_SLOT_BYTES];
    uint32_t background = raster(1920, 1080, 0x0000), w;
    ((uint16_t*)(uintptr_t)get32(background + 0x18))[300 * 1920 + 640] = 0xF800;
    game_tables("BE.SCN");
    w = loader(1920, 1080, L_RETAIL, background);
    CHECK(loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "the retail loading window is read");
    CHECK(s.kind == LD_SCENE_LOADER && s.screen_w == 1920 && s.screen_h == 1080, "loading scene kind/size");
    CHECK(s.art565 && s.art565[0] == 0xF800 && s.art_w == 640 && s.art_h == 480 && s.art_pitch == 1920,
          "the picture is the 640x480 the game centred in its background");
    CHECK(!strcmp(slot, "li_be"), "New Game's BE.SCN shows li_be (got %s)", slot);
    CHECK(retail_gauges(&s) && s.note_count == 0, "two captioned gauges, no other text (%d gauges, %d notes)", s.gauge_count,
          s.note_count);
    CHECK(fabsf(s.gauges[0].fraction - 50.0f / 99.0f) < 1e-4f && s.gauges[1].fraction == 0.0f,
          "World Initialization at 50 of 0..99, Level File Copy empty (%.3f)", s.gauges[0].fraction);
    CHECK(s.gauges[0].color == 0xFF4000 && s.gauges[0].text_color == 0xFFFFFF, "the bar keeps the game's colour %06X",
          s.gauges[0].color);
    put32(get32(get32(w + 0x20) + 8) + 0x28, 120);
    CHECK(loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot) && s.gauges[0].fraction == 1.0f,
          "a bar past its range is full, as the game clips it");
    free_fakes();

    background = raster(2560, 1440, 0);
    game_tables("TestScene.scn");
    w = loader(2560, 1440, L_RETAIL, background);
    CHECK(loading_hd_read(w, 2560, 1440, &s, slot, sizeof slot) && !strcmp(slot, "li_other"),
          "a scene not in the table shows li_other (got %s)", slot);
    free_fakes();

    background = raster(2560, 1440, 0);
    w = loader(2560, 1440, L_RETAIL, background);
    CHECK(loading_hd_read(w, 2560, 1440, &s, slot, sizeof slot) && !slot[0],
          "without the game's table the picture is still shown, with no HD art slot");
    free_fakes();
}

/* Log 042: loader.ddf's bars are not visible by their flags, yet drawn; the captions must still find them. */
static void test_loading_042(void) {
    ld_scene_t s;
    char slot[LD_SLOT_BYTES];
    CHECK(read_loader(L_RETAIL, &s, slot), "042: the retail loading window (bars without the visible bit) is HD");
    CHECK(retail_gauges(&s) && s.note_count == 0, "042: both captions belong to their bars (%d gauges, %d notes)", s.gauge_count,
          s.note_count);
    CHECK(!strcmp(slot, "li_be"), "042: Beach shows li_be");
    free_fakes();
}

static void test_loading_texts(void) {
    ld_scene_t s;
    char slot[LD_SLOT_BYTES];
    CHECK(read_loader(L_PLEASE_WAIT, &s, slot) && retail_gauges(&s) && s.note_count == 1 && !strcmp(s.notes[0], "Please Wait"),
          "a text of its own (the DDF's commented Please Wait) is a line on the plate, not a fallback");
    free_fakes();
    CHECK(read_loader(L_EMPTY_TEXT, &s, slot) && retail_gauges(&s) && s.note_count == 0, "an empty text is ignored");
    free_fakes();
    CHECK(read_loader(L_HIDDEN_TEXT, &s, slot) && retail_gauges(&s) && s.note_count == 0,
          "a text the game does not draw (visible bit clear) is not drawn either");
    free_fakes();
    CHECK(read_loader(L_THREE_TEXTS, &s, slot) && retail_gauges(&s) && s.note_count == LD_MAX_NOTES &&
              !strcmp(s.notes[0], "Please Wait") && !strcmp(s.notes[1], "Second line"),
          "texts past the plate's room are left out alone, top first (%d notes)", s.note_count);
    free_fakes();
    CHECK(read_loader(L_CAPTIONS_MOVED, &s, slot) && retail_gauges(&s) && s.note_count == 0,
          "captions find their bars by id wherever they are placed");
    free_fakes();
    CHECK(read_loader(L_BUTTON, &s, slot) && retail_gauges(&s), "an unknown control is left out, the screen stays HD");
    free_fakes();
    CHECK(read_loader(L_FIVE_BARS, &s, slot) && s.gauge_count == LD_MAX_GAUGES && !strcmp(s.gauges[0].caption, "World Initialization"),
          "bars past the fourth are left out, the screen stays HD (%d)", s.gauge_count);
    free_fakes();
}

/* The loading screen reads its labels from the game's own controls: a translated label replaces the English one there. */
static void test_loading_labels_translated(void) {
    ld_scene_t s;
    char slot[LD_SLOT_BYTES];
    s_label_from = "World Initialization";
    s_label_to = "Inicialização do mundo";
    CHECK(read_loader(L_RETAIL, &s, slot) && s.gauge_count == 2 && !strcmp(s.gauges[0].caption, s_label_to) &&
              !strcmp(s.gauges[1].caption, "Level File Copy"),
          "World Initialization is drawn translated, the other label as the game has it (%s | %s)", s.gauges[0].caption,
          s.gauges[1].caption);
    free_fakes();
    s_label_from = "Level File Copy";
    s_label_to = "Cópia dos arquivos da fase";
    CHECK(read_loader(L_RETAIL, &s, slot) && !strcmp(s.gauges[0].caption, "World Initialization") &&
              !strcmp(s.gauges[1].caption, s_label_to),
          "Level File Copy is drawn translated (%s)", s.gauges[1].caption);
    free_fakes();
    s_label_from = s_label_to = NULL;
}

static void test_loading_fallbacks(void) {
    ld_scene_t s;
    char slot[LD_SLOT_BYTES];
    uint32_t w, screen;
    int sw = 0, sh = 0;
    game_tables("jr.scn");
    w = loader(1920, 1080, L_RETAIL, raster(320, 200, 0));
    CHECK(!loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "a background without the 640x480 picture is refused");
    free_fakes();

    game_tables("jr.scn");
    w = loader(1920, 1080, L_RETAIL, 0);
    CHECK(!loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "a loading window without its background is refused");
    free_fakes();

    game_tables("jr.scn");
    w = loader(1920, 1080, L_RETAIL, raster(1920, 1080, 0));
    put32(w + 0x24, get32(w + 0x20) - 4);
    CHECK(!loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "a control list that cannot be read is refused");
    free_fakes();

    game_tables("jr.scn");
    w = loader(640, 480, L_RETAIL, raster(640, 480, 0));
    CHECK(!loading_hd_read(w, 640, 480, &s, slot, sizeof slot), "a 640x480 screen keeps the game's own loading screen");
    free_fakes();

    w = window(VT_VIDEO, fe_rect(0, 0, 1920, 1080), NULL, 0, raster(1920, 1080, 0));
    CHECK(!loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "a video window is never read as a loading screen");
    w = window(VT_DIRECT_LOAD, fe_rect(0, 0, 1920, 1080), NULL, 0, raster(1920, 1080, 0));
    CHECK(!loading_hd_read(w, 1920, 1080, &s, slot, sizeof slot), "the direct-load list is not a loading screen");
    CHECK(!loading_hd_read(0, 1920, 1080, &s, slot, sizeof slot), "no window, no loading screen");
    free_fakes();

    screen = fake(0x90);
    put32(screen + 0x7C, 2560);
    put32(screen + 0x80, 1920);
    g_screen_raster = fake(4);
    put32(g_screen_raster, screen);
    CHECK(loading_hd_screen(&sw, &sh) && sw == 2560 && sh == 1920, "Direct3D 11 paints at the game's screen size (%dx%d)", sw, sh);
    g_hardware = 0;
    CHECK(!loading_hd_screen(&sw, &sh), "the software renderer keeps the game's own loading screen");
    g_hardware = 1;
    free_fakes();
}


/* ---- JPT-UIHD2B9: the menus at the game's 640x480 screen, Direct Load, button pictures by state ---- */

static const fe_widget_t* widget_with_id(int32_t id) {
    for (int i = 0; i < g_scene.widget_count; i++)
        if (g_scene.widgets[i].id == id && g_scene.widgets[i].kind != FE_W_IMAGE) return &g_scene.widgets[i];
    return NULL;
}

static uint16_t first_pixel(uint32_t raster_va) { return *(const uint16_t*)(uintptr_t)get32(raster_va + 0x18); }

static const uint16_t* bits_of(uint32_t raster_va) { return (const uint16_t*)(uintptr_t)get32(raster_va + 0x18); }

static void test_small_screen(void) {
    uint32_t ws[2], opts[1];
    ws[0] = main_window(1);
    stack(ws, 1);
    memset(&g_layer, 0, sizeof g_layer);
    CHECK(frontend_hd_frame(640, 480) == 1, "the main menu is not recognised on the game's 640x480 screen");
    CHECK(g_layer.w == 1920 && g_layer.h == 1440, "the 640x480 menu layer is the frame's size: %dx%d", g_layer.w, g_layer.h);
    CHECK(frontend_hd_read_scene(&g_scene, 640, 480) && g_scene.guest_w == 640 && g_scene.screen_w == 640,
          "a layer as large as the game's screen");
    CHECK(frontend_hd_read_scene_for(&g_scene, 640, 480, 2880, 2160) && g_scene.guest_w == 640 && g_scene.screen_w == 2880,
          "a layer larger than the game's screen");
    frontend_hd_frame_done();
    for (int i = 0; i < 200; i++) frontend_hd_idle();
    free_fakes();

    opts[0] = button(1004, 116, 122, 87, 36);
    ws[0] = main_window(0);
    ws[1] = window(VT_OPTIONS, fe_rect(160, 140, 480, 340), opts, 1, 0);
    stack(ws, 2);
    CHECK(frontend_hd_frame(640, 480) == 1, "Options over the main menu is not shown at 640x480");
    frontend_hd_frame_done();
    for (int i = 0; i < 200; i++) frontend_hd_idle();
    free_fakes();
}

static void test_layer_size(void) {
    int w, h;
    frontend_hd_layer_size(640, 480, 2880, 2160, &w, &h);
    CHECK(w == 2880 && h == 2160, "a 4:3 frame is the layer: %dx%d", w, h);
    frontend_hd_layer_size(5424, 1520, 7680, 2160, &w, &h);
    CHECK(w * h <= (17 << 20) && w >= 5424 && h >= 1520 && abs(w * 2160 - h * 7680) < 7680, "a 32:9 frame within the budget: %dx%d", w, h);
    frontend_hd_layer_size(1920, 1080, 0, 0, &w, &h);
    CHECK(w == 1920 && h == 1080, "no renderer: the game's screen");
    frontend_hd_layer_size(1920, 1080, 1280, 720, &w, &h);
    CHECK(w == 1920 && h == 1080, "a frame smaller than the game's screen never shrinks the layer");
    frontend_hd_layer_size(3840, 2160, 16000, 9000, &w, &h);
    CHECK(w * h <= (17 << 20) && w >= 3840, "a huge frame is brought within the budget: %dx%d", w, h);
}

static void test_button_pictures(void) {
    uint32_t ws[2], c[4], hover, pressed, disabled, normal;
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    c[0] = normal = button(1000, wr.left + 10, wr.top + 10, 80, 40);
    c[1] = hover = button(1001, wr.left + 100, wr.top + 10, 80, 40);
    put32(hover + 0x28, 1 | 2);
    c[2] = pressed = button(1002, wr.left + 10, wr.top + 60, 80, 40);
    put32(pressed + 0x28, 1 | 2 | 4);
    c[3] = disabled = button(1003, wr.left + 100, wr.top + 60, 80, 40);
    put32(disabled + 0x08, 4);
    ws[0] = main_window(0);
    ws[1] = window(VT_OPTIONS, wr, c, 4, 0);
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080), "buttons in every state are refused");
    CHECK(widget_with_id(1000) && widget_with_id(1000)->image == bits_of(get32(normal + 0x2C)),
          "a resting button shows its first picture");
    CHECK(widget_with_id(1001) && widget_with_id(1001)->image == bits_of(get32(hover + 0x30)) &&
              (widget_with_id(1001)->state & FE_ST_HOVER),
          "a hovered button shows its second picture");
    CHECK(widget_with_id(1002) && widget_with_id(1002)->image == bits_of(get32(pressed + 0x34)) &&
              (widget_with_id(1002)->state & FE_ST_PRESSED),
          "a pressed button shows its third picture");
    CHECK(widget_with_id(1003) && widget_with_id(1003)->image == bits_of(get32(disabled + 0x38)) &&
              (widget_with_id(1003)->state & FE_ST_DISABLED),
          "a disabled button shows its fourth picture");
    CHECK(widget_with_id(1000)->image_keyed && widget_with_id(1000)->image_key == first_pixel(get32(normal + 0x2C)),
          "a button picture is keyed by its top-left pixel");
    put32(disabled + 0x38, 0);
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080) && !widget_with_id(1003),
          "a disabled button without a fourth picture is not drawn");
    free_fakes();
}

static void test_direct_load(void) {
    fe_rect_t wr = fe_rect(160, 120, 480, 360);
    uint32_t c[4], list, items, w, ws[2], names;
    char* name;
    int dx;
    list = control(VT_LISTBOX, 1002, at(wr, 10, 10, 300, 180));
    items = fake(12 * 3);
    names = fake(64);
    name = (char*)(uintptr_t)names;
    strcpy(name, "as.scn");
    strcpy(name + 16, "lab.scn");
    strcpy(name + 32, "TestScene.scn");
    for (int i = 0; i < 3; i++) put32(items + 12u * (uint32_t)i, names + 16u * (uint32_t)i);
    put32(list + 0x24, items);
    put32(list + 0x28, items + 36);
    put32(list + 0x3C, 1);
    put32(list + 0x44, 12);
    put32(list + 0x48, 16);
    put32(list + 0x4C, 14);
    *(uint8_t*)(uintptr_t)(list + 0x6C) = 12;
    c[0] = button(1000, wr.left + 120, wr.top + 190, 118, 42);
    c[1] = button(1001, wr.left + 230, wr.top + 190, 62, 42);
    c[2] = list;
    w = window(VT_DIRECT_LOAD, wr, c, 3, 0);
    frontend_hd_window_centred(w);
    dx = fe_layout_direct_dx(wr);
    CHECK(dx < 0 && fe_rect_equal(rect_of(c[0]), fe_rect(wr.left + 120 + dx, wr.top + 190, wr.left + 238 + dx, wr.top + 232)),
          "Direct Load's controls did not move with its window");
    CHECK(rect_of(list).left == wr.left + 10 + dx, "Direct Load's list did not move");
    CHECK((int32_t)get32(w + 0x10) == wr.left + dx, "Direct Load's window rectangle did not move");
    put32(w + 0x10, (uint32_t)wr.left);
    put32(w + 0x18, (uint32_t)wr.right);
    ws[0] = main_window(0);
    ws[1] = w;
    stack(ws, 2);
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1080), "Direct Load is refused");
    CHECK(g_scene.windows[1].kind == FE_WND_DIRECT_LOAD, "Direct Load is not recognised");
    {
        const fe_widget_t* l = find(&g_scene, FE_W_LIST, 1002);
        const fe_widget_t* photo_w = NULL;
        for (int i = 0; i < g_scene.widget_count; i++)
            if (g_scene.widgets[i].kind == FE_W_IMAGE && g_scene.widgets[i].style == FE_IMAGE_PHOTO) photo_w = &g_scene.widgets[i];
        CHECK(l && l->item_count == 3 && l->selected == 1, "Direct Load's list");
        CHECK(l && !strcmp(fe_scene_str(&g_scene, g_scene.items[l->first_item + 1]), "lab.scn"), "Direct Load's level names");
        CHECK(photo_w && fe_rect_equal(photo_w->rect, fe_layout_direct_photo(wr)), "the level's picture frame");
    }
    CHECK(find(&g_scene, FE_W_BUTTON, 1000) && find(&g_scene, FE_W_BUTTON, 1001), "Direct Load's buttons");
    free_fakes();
}

/* The Controls window's binding fields follow the device used last: the game's key names, or the gamepad's glyphs. */
static void test_controls_prompts(void) {
    fe_rect_t wr = fe_rect(160, 140, 480, 340);
    uint32_t c[4], ws[2];
    const fe_widget_t *walk, *jump, *hand;
    c[0] = label(1030, at(wr, 94, 46, 164, 60), "S", 1);
    c[1] = label(1035, at(wr, 94, 116, 164, 130), "Q", 1);
    c[2] = label(1039, at(wr, 164, 88, 246, 102), "Left Mouse", 1);
    c[3] = label(-1, at(wr, 10, 46, 90, 60), "Walk Forward", 0);
    ws[0] = main_window(0);
    ws[1] = window(VT_CONTROLS, wr, c, 4, 0);
    stack(ws, 2);
    jpt_gamepad_defaults(&g_pad_settings);
    g_pad_in_use = 0;
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1440), "the Controls scene is refused");
    frontend_hd_prompts(&g_scene);
    CHECK(g_scene.legend_count == 0 && find(&g_scene, FE_W_FIELD, 1035) && !find(&g_scene, FE_W_FIELD, 1035)->prompt.kind,
          "keyboard and mouse keep the game's own key names");
    g_pad_in_use = 1;
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1440), "the Controls scene is refused");
    frontend_hd_prompts(&g_scene);
    walk = find(&g_scene, FE_W_FIELD, 1030);
    jump = find(&g_scene, FE_W_FIELD, 1035);
    hand = find(&g_scene, FE_W_FIELD, 1039);
    CHECK(jump && jump->prompt.kind == FE_PROMPT_PAD && jump->prompt.control == JPT_PAD_SOUTH &&
              jump->prompt.family == JPT_FAMILY_PLAYSTATION, "Jump shows the bound face button");
    CHECK(walk && walk->prompt.control == JPT_PAD_LEFT_STICK_MOVE && walk->prompt.dir == JPT_DIR_UP && walk->prompt.tilt == 1,
          "Walk Forward shows the left stick half up");
    CHECK(hand && hand->prompt.control == JPT_PAD_LEFT_TRIGGER, "Move Hand shows the left trigger");
    CHECK(g_scene.legend_count == 2 && g_scene.legend[0].control == JPT_PAD_SOUTH && g_scene.legend[1].control == JPT_PAD_EAST,
          "the menu legend shows select and back");
    g_pad_settings.bind[JPT_ACT_JUMP] = JPT_PAD_NONE;
    CHECK(frontend_hd_read_scene(&g_scene, 1920, 1440), "the Controls scene is refused");
    frontend_hd_prompts(&g_scene);
    jump = find(&g_scene, FE_W_FIELD, 1035);
    CHECK(jump && !jump->prompt.kind && !strcmp(fe_scene_str(&g_scene, jump->text), "-"), "an unbound action shows a dash");
    g_pad_in_use = 0;
    free_fakes();
}

void frontend_bridge_tests(int* checks, int* failures) {
    g_checks = checks;
    g_failures = failures;
    test_loading_window();
    test_loading_042();
    test_loading_texts();
    test_loading_labels_translated();
    test_loading_fallbacks();
    test_options();
    test_options_in_game();
    test_audio();
    test_main_and_frame();
    test_lists_fields_messages();
    test_save_pictures();
    test_fallbacks();
    test_small_screen();
    test_layer_size();
    test_button_pictures();
    test_direct_load();
    test_controls_prompts();
}
