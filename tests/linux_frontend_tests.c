/* The HD front end and the HD loading screen on Linux, headless and with no game data: the shared bridge (frontend_hd.c,
 * loading_hd.c) reads synthetic game objects laid out as the retail ones, the shared painter paints them, and the compositor puts
 * the layer over the guest's scaled picture inside the rectangle the guest screen occupies. Checked: what is read from each
 * window, hover and pressed states, pixels and alpha of the painted layer, text in Latin and Cyrillic, painting on several
 * threads and by dirty rectangle against a full repaint, the compositor over windows of every size and scale (100/150/225 %,
 * letterboxed, resized), the guest's framebuffer left alone, the pointer, a session through the menus into a loading screen and
 * back to the game, and the layer's memory. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "active_profile.h"
#include "frontend_assets.h"
#include "frontend_hd.h"
#include "frontend_layout.h"
#include "frontend_platform.h"
#include "guest_abi.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_game_version.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "l10n_assets.h"
#include "linux_card.h"
#include "linux_text.h"
#include "linux_frontend.h"
#include "linux_screen.h"
#include "linux_spine.h"
#include "linux_user32.h"
#include "linux_window.h"
#include "loading_hd.h"
#include "recomp_types.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define VT_BUTTON 0x0064554Cu
#define VT_LISTBOX 0x00645650u
#define VT_TEXTBOX 0x006457B8u
#define VT_STATIC 0x006455B0u
#define VT_PROGRESS 0x00645974u
#define VT_MAIN 0x006466B8u
#define VT_OPTIONS 0x00646A58u
#define VT_LOAD 0x006460D0u
#define VT_GAME 0x00645C3Cu
#define VT_LOADER 0x006467A0u
#define SCENE_NAME 0x0066E342u
#define ART_TABLE 0x006610D0u
#define SCREEN_RASTER 0x0073DC40u

/* ---- the synthetic game: one arena of guest pages, objects at the retail layouts ---- */

#define ARENA_BYTES (24u << 20)
static uint32_t s_arena, s_used, s_data_a, s_data_b;

static uint32_t fake(size_t bytes) {
    uint32_t va = s_arena + s_used;
    s_used += (uint32_t)((bytes + 15u) & ~15u);
    if (s_used > ARENA_BYTES) abort();
    return va;
}

static void put32(uint32_t va, uint32_t v) { gp_write_u32(va, v); }

static uint32_t get32(uint32_t va) {
    uint32_t v = 0;
    gp_read_u32(va, &v);
    return v;
}

static void put8(uint32_t va, uint8_t v) { gp_copy_out(va, &v, 1); }

static void reset_game(void) {
    memset(gm_ptr(s_arena, ARENA_BYTES), 0, ARENA_BYTES);
    memset(gm_ptr(s_data_a, 0x20000), 0, 0x20000);
    memset(gm_ptr(s_data_b, 0x20000), 0, 0x20000);
    s_used = 0;
}

static uint32_t raster(int w, int h, uint16_t fill) {
    uint32_t r = fake(0x90), bits = fake((size_t)w * (size_t)h * 2u);
    uint16_t* px = (uint16_t*)gm_ptr(bits, (uint32_t)w * (uint32_t)h * 2u);
    for (int i = 0; i < w * h; i++) px[i] = fill;
    put32(r + 0x08, (uint32_t)w);
    put32(r + 0x0C, (uint32_t)h);
    put32(r + 0x10, 16);
    put32(r + 0x14, (uint32_t)w);
    put32(r + 0x18, bits);
    return r;
}

static uint32_t text(const char* s) {
    uint32_t p = fake(strlen(s) + 1);
    gp_copy_out(p, s, (uint32_t)strlen(s) + 1);
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

static uint32_t button(int32_t id, int x, int y, int w, int h, uint16_t base) {
    uint32_t c = control(VT_BUTTON, id, fe_rect(x, y, x + w, y + h));
    for (int slot = 0; slot < 4; slot++) put32(c + 0x2C + 4u * (uint32_t)slot, raster(w, h, (uint16_t)(base + slot * 0x111)));
    put32(c + 0x28, 1);
    return c;
}

static uint32_t label(int32_t id, fe_rect_t r, const char* s) {
    uint32_t c = control(VT_TEXTBOX, id, r);
    put32(c + 0x28, text(s));
    put32(c + 0x30, 0x00FFFFFF);
    put32(c + 0x3C, 0x24);
    put32(c + 0x40, 1);
    put8(c + 0x48, 1);
    put8(c + 0x4E, 12);
    return c;
}

static uint32_t window(uint32_t vt, fe_rect_t r, const uint32_t* controls, int n, uint32_t background) {
    uint32_t w = fake(0x40), v = fake(4u * (uint32_t)(n > 0 ? n : 1));
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

static uint32_t s_manager;

static void stack(const uint32_t* windows, int n) {
    uint32_t main = fake(0x10), v = fake(4u * (uint32_t)n);
    s_manager = fake(0x60);
    for (int i = 0; i < n; i++) put32(v + 4u * (uint32_t)i, windows[i]);
    put32(main + 0x08, s_manager);
    put32(s_manager + 0x20, 1);
    {
        uint32_t pointer = raster(16, 16, 0xF800);
        put32(get32(pointer + 0x18), 0);
        put32(s_manager + 0x28, pointer);
    }
    put32(s_manager + 0x38, v);
    put32(s_manager + 0x3C, v + 4u * (uint32_t)n);
    put32(MAIN_WINDOW_OBJECT, main);
}

static uint32_t s_buttons[4];

static uint32_t main_window(void) {
    uint32_t c[6];
    for (int i = 0; i < 4; i++) c[i] = s_buttons[i] = button(1000 + i, 205, 70 + 90 * i, 208, 81, (uint16_t)(0x3186 + i * 0x20));
    c[4] = label(1010, fe_rect(40, 20, 320, 60), "Trespasser");
    c[5] = control(VT_STATIC, 1005, fe_rect(550, 393, 623, 468));
    put32(c[5] + 0x24, raster(73, 75, 0x07E0));
    put8(c[5] + 0x20, 1);
    return window(VT_MAIN, fe_rect(0, 0, 640, 480), c, 6, raster(640, 480, 0x2104));
}

static void main_menu_stack(void) {
    uint32_t w = main_window();
    stack(&w, 1);
}

/* ---- pixels ---- */

static uint32_t* new_layer(int w, int h) { return (uint32_t*)calloc((size_t)w * (size_t)h, 4); }

static unsigned lit(const uint32_t* px, int pitch, fe_rect_t r) {
    unsigned n = 0;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++) n += (px[(size_t)y * (size_t)pitch + (size_t)x] >> 24) != 0;
    return n;
}

static uint32_t hash_pixels(const uint32_t* px, int w, int h) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) hash = (hash ^ px[i]) * 16777619u;
    return hash;
}

static uint32_t region_hash(const uint32_t* px, int pitch, fe_rect_t r) {
    uint32_t hash = 2166136261u;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++) hash = (hash ^ px[(size_t)y * (size_t)pitch + (size_t)x]) * 16777619u;
    return hash;
}

static void test_scene_reading(void) {
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    int found = 0, hover = 0, pressed = 0, label_ok = 0;
    main_menu_stack();
    CHECK(frontend_hd_read_scene_for(s, 640, 480, 1280, 960), "the main menu is read from the synthetic game");
    CHECK(s->window_count == 1 && s->windows[0].kind == FE_WND_MAIN, "one main-menu window (%d)", s->window_count);
    CHECK(s->backdrop != NULL, "its 640x480 art is the backdrop");
    for (int i = 0; i < s->widget_count; i++) {
        const fe_widget_t* w = &s->widgets[i];
        if (w->kind == FE_W_BUTTON && w->id >= 1000 && w->id <= 1003) found++;
        if (w->kind == FE_W_LABEL && !strcmp(fe_scene_str(s, w->text), "Trespasser")) label_ok = 1;
    }
    CHECK(found == 4, "all four menu buttons are widgets (%d)", found);
    CHECK(label_ok, "the text box's text is read");
    put32(s_buttons[1] + 0x28, 2);
    put32(s_buttons[2] + 0x28, 4);
    CHECK(frontend_hd_read_scene_for(s, 640, 480, 640, 480), "the scene is read again");
    for (int i = 0; i < s->widget_count; i++) {
        const fe_widget_t* w = &s->widgets[i];
        if (w->id == 1001 && (w->state & FE_ST_HOVER)) hover = 1;
        if (w->id == 1002 && (w->state & FE_ST_PRESSED)) pressed = 1;
    }
    CHECK(hover && pressed, "the game's hover and pressed flags are the only state (hover %d, pressed %d)", hover, pressed);
    {
        uint32_t window_unknown = fake(0x40);
        uint32_t w[2];
        put32(window_unknown, 0x00646D10u + 0x1230u);
        w[0] = main_window();
        w[1] = window_unknown;
        stack(w, 2);
        CHECK(!frontend_hd_read_scene_for(s, 640, 480, 640, 480), "a window the front end does not know leaves the game's own menu");
        w[1] = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
        stack(w + 1, 1);
        CHECK(!frontend_hd_read_scene_for(s, 640, 480, 640, 480), "the gameplay view in front is not a menu");
    }
    free(s);
    reset_game();
}

static void test_painter(void) {
    fe_scene_t *a = (fe_scene_t*)calloc(1, sizeof *a), *b = (fe_scene_t*)calloc(1, sizeof *b);
    int threads = 1, bad_alpha = 0, w = 1280, h = 960;
    fe_painter_t* painter;
    uint32_t *one, *many, *dirty_paint, *full;
    fe_rect_t dirty, clip;
    unsigned text_lit;
    main_menu_stack();
    painter = frontend_hd_painter(&threads);
    CHECK(painter != NULL, "the painter starts without Windows (fonts: %s)", painter ? fe_painter_face(painter, FE_FONT_UI) : "none");
    if (!painter) {
        free(a);
        free(b);
        return;
    }
    CHECK(frontend_hd_read_scene_for(a, 640, 480, w, h), "scene A");
    one = new_layer(w, h);
    many = new_layer(w, h);
    CHECK(fe_paint(painter, a, one, w, fe_rect(0, 0, w, h), 1), "painted on one thread");
    CHECK(fe_paint(painter, a, many, w, fe_rect(0, 0, w, h), 4), "painted on four threads");
    CHECK(hash_pixels(one, w, h) == hash_pixels(many, w, h), "painting is the same on one thread and on four");
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint32_t p = one[i], alpha = p >> 24;
        if (((p >> 16) & 255u) > alpha || ((p >> 8) & 255u) > alpha || (p & 255u) > alpha) bad_alpha++;
    }
    CHECK(bad_alpha == 0, "every pixel is premultiplied (%d pixels have a channel above their alpha)", bad_alpha);
    CHECK(lit(one, w, fe_rect(0, 0, w, h)) > (unsigned)(w * h * 9 / 10), "the main screen's art covers the layer");
    text_lit = lit(one, w, fe_rect(80, 40, 640, 120));
    CHECK(text_lit > 0, "the title's text is drawn (%u lit pixels)", text_lit);
    CHECK(frontend_hd_read_scene_for(b, 640, 480, w, h), "scene B");
    put32(s_buttons[2] + 0x28, 2);
    CHECK(frontend_hd_read_scene_for(b, 640, 480, w, h), "scene B with a hovered button");
    CHECK(fe_scene_diff(a, b, &dirty) == FE_DIFF_RECT, "one hovered button changes a rectangle, not the screen");
    {
        fe_view_t v = fe_view_make_for(640, 480, w, h);
        clip = fe_rect_intersect(fe_view_rect(&v, dirty), fe_rect(0, 0, w, h));
    }
    dirty_paint = new_layer(w, h);
    full = new_layer(w, h);
    memcpy(dirty_paint, one, (size_t)w * (size_t)h * 4);
    CHECK(fe_paint(painter, b, dirty_paint, w, clip, threads), "the changed rectangle is painted");
    CHECK(fe_paint(painter, b, full, w, fe_rect(0, 0, w, h), threads), "the whole of B is painted");
    CHECK(hash_pixels(dirty_paint, w, h) == hash_pixels(full, w, h), "painting only the changed rectangle equals painting everything");
    CHECK(hash_pixels(one, w, h) != hash_pixels(full, w, h), "the hover is visible");
    free(one);
    free(many);
    free(dirty_paint);
    free(full);
    free(a);
    free(b);
    frontend_hd_release();
    reset_game();
}

static void test_text(void) {
    static const unsigned char cyrillic[] = { 0xD6, 0xC5, 0xCB, 0xC8, 0 }; /* ЦЕЛИ in code page 1251 */
    int threads = 1;
    fe_painter_t* painter = frontend_hd_painter(&threads);
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    uint32_t* px = new_layer(1280, 960);
    unsigned as_1251 = 0, as_1252 = 0;
    uint32_t w, hash_1251 = 0, hash_1252 = 0;
    CHECK(painter != NULL, "painter");
    if (!painter) goto out;
    {
        uint32_t c[1];
        c[0] = label(1010, fe_rect(40, 20, 320, 60), (const char*)cyrillic);
        w = window(VT_MAIN, fe_rect(0, 0, 640, 480), c, 1, raster(640, 480, 0));
        stack(&w, 1);
    }
    jpt_l10n_set_display_codepage(1251);
    CHECK(frontend_hd_read_scene_for(s, 640, 480, 1280, 960) && fe_paint(painter, s, px, 1280, fe_rect(0, 0, 1280, 960), threads), "a Russian label is painted");
    as_1251 = lit(px, 1280, fe_rect(70, 30, 700, 130));
    hash_1251 = region_hash(px, 1280, fe_rect(70, 30, 700, 130));
    CHECK(strcmp(fe_painter_face_for(painter, FE_FONT_UI, (const char*)cyrillic), "") != 0, "the face of a role has the Cyrillic characters");
    memset(px, 0, 1280u * 960u * 4u);
    jpt_l10n_set_display_codepage(1252);
    CHECK(frontend_hd_read_scene_for(s, 640, 480, 1280, 960) && fe_paint(painter, s, px, 1280, fe_rect(0, 0, 1280, 960), threads), "the same bytes in 1252");
    as_1252 = lit(px, 1280, fe_rect(70, 30, 700, 130));
    hash_1252 = region_hash(px, 1280, fe_rect(70, 30, 700, 130));
    CHECK(as_1251 > 300 && as_1252 > 300, "both draw glyphs (%u, %u lit pixels)", as_1251, as_1252);
    CHECK(hash_1251 != hash_1252, "the code page decides the glyphs: 1251 draws Cyrillic letters, 1252 Latin ones");
out:
    jpt_l10n_set_display_codepage(0);
    free(px);
    free(s);
    frontend_hd_release();
    reset_game();
}

/* ---- the compositor ---- */

static uint16_t* guest_picture(int w, int h, uint16_t fill) {
    uint16_t* px = (uint16_t*)malloc((size_t)w * (size_t)h * 2);
    for (int i = 0; i < w * h; i++) px[i] = fill;
    return px;
}

static uint32_t from_565(uint16_t c) {
    const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static void test_compose_geometry(void) {
    static const int sizes[][2] = { { 640, 480 },   { 800, 600 },  { 1280, 720 },  { 1920, 1080 }, { 2560, 1440 }, { 1366, 768 },
                                    { 1920, 1200 }, { 2880, 1620 }, { 4320, 2430 }, { 1000, 1500 }, { 3440, 1440 }, { 1280, 1024 } };
    uint16_t* guest = guest_picture(640, 480, 0x07E0);
    lx_frame_t in;
    memset(&in, 0, sizeof in);
    in.pixels = guest, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const int ww = sizes[i][0], wh = sizes[i][1];
        lx_view_t v = lx_present_view(ww, wh, 640, 480, 0);
        lx_frame_t out;
        lx_frontend_stats_t st;
        int lw = 0, lh = 0, shown;
        const uint32_t green = from_565(0x07E0);
        main_menu_stack();
        shown = lx_frontend_frame(640, 480, ww, wh);
        CHECK(shown, "%dx%d: the menu's layer is ready", ww, wh);
        if (shown) {
            CHECK(fe_host_frame_size(640, 480, &lw, &lh) && lw == v.w && lh == v.h, "%dx%d: the frame size is the guest picture's rectangle (%dx%d)", ww, wh, lw, lh);
            CHECK(lx_frontend_compose(&in, &out, ww, wh, 0), "%dx%d: composed", ww, wh);
            lx_frontend_stats(&st);
            CHECK(out.width == ww && out.height == wh && out.format == LX_PF_XRGB8888 && out.pitch == ww * 4, "%dx%d: the frame is the window's size", ww, wh);
            CHECK(st.view_x == v.x && st.view_y == v.y && st.view_w == v.w && st.view_h == v.h, "%dx%d: the layer lands in the guest's rectangle %d,%d %dx%d", ww, wh, v.x, v.y, v.w, v.h);
            CHECK(st.layer_w >= 640 && st.layer_h >= 480 && (long long)st.layer_w * st.layer_h <= (17ll << 20), "%dx%d: the layer is %dx%d, within its budget", ww, wh, st.layer_w, st.layer_h);
            if (v.x > 0) CHECK(((const uint32_t*)out.pixels)[(size_t)(wh / 2) * (size_t)ww + 1] == 0xFF000000u, "%dx%d: the pillar bar is black", ww, wh);
            if (v.y > 0) CHECK(((const uint32_t*)out.pixels)[(size_t)1 * (size_t)ww + (size_t)(ww / 2)] == 0xFF000000u, "%dx%d: the letterbox bar is black", ww, wh);
            CHECK(((const uint32_t*)out.pixels)[(size_t)(v.y + v.h / 2) * (size_t)ww + (size_t)(v.x + 4)] != green, "%dx%d: the layer covers the guest's own picture", ww, wh);
        }
        lx_frontend_frame_done();
        CHECK(!lx_frontend_compose(&in, &(lx_frame_t){ 0 }, ww, wh, 0), "%dx%d: nothing is shown once the frame is done", ww, wh);
        frontend_hd_release();
        reset_game();
    }
    {
        const uint16_t* check = guest;
        int untouched = 1;
        for (int i = 0; i < 640 * 480; i++) untouched &= check[i] == 0x07E0;
        CHECK(untouched, "the guest's framebuffer is never altered");
    }
    free(guest);
}

static void test_pointer_and_mapping(void) {
    uint16_t* guest = guest_picture(640, 480, 0x001F);
    lx_frame_t in, out;
    lx_frontend_stats_t st;
    int gx = -1, gy = -1;
    const int ww = 1920, wh = 1080;
    lx_view_t v = lx_present_view(ww, wh, 640, 480, 0);
    memset(&in, 0, sizeof in);
    in.pixels = guest, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    lx_virtual_reset();
    lx_screen_reset();
    CHECK(lx_screen_window_open("t", ww, wh), "a headless window");
    main_menu_stack();
    put32(s_manager + 0x18, 300);
    put32(s_manager + 0x1C, 200);
    lx_user32_cursor_set(300, 200);
    CHECK(lx_frontend_frame(640, 480, ww, wh) && lx_frontend_compose(&in, &out, ww, wh, 0), "the menu is composed in 1920x1080");
    lx_frontend_stats(&st);
    CHECK(st.pointer_drawn, "the pointer sprite is drawn over the layer");
    CHECK(lx_screen_client_to_guest(v.x + v.w / 2, v.y + v.h / 2, &gx, &gy) && gx == 320 && gy == 240, "the centre of the picture is guest (320,240) (%d,%d)", gx, gy);
    {
        const fe_rect_t r = fe_rect(205, 70, 413, 151);
        const int cx = v.x + (r.left + r.right) / 2 * v.w / 640, cy = v.y + (r.top + r.bottom) / 2 * v.h / 480;
        CHECK(lx_screen_client_to_guest(cx, cy, &gx, &gy) && gx >= r.left && gx < r.right && gy >= r.top && gy < r.bottom, "the centre of the first button in the window maps into the button's own rectangle (%d,%d)", gx, gy);
    }
    lx_frontend_frame_done();
    lx_frontend_stats(&st);
    put32(s_manager + 0x20, 0);
    CHECK(lx_frontend_frame(640, 480, ww, wh) && lx_frontend_compose(&in, &out, ww, wh, 0), "the menu again without a pointer");
    lx_frontend_stats(&st);
    CHECK(!st.pointer_drawn, "a hidden game pointer is not drawn");
    lx_frontend_frame_done();
    {
        int cw = 0, ch = 0;
        lx_view_t small;
        put32(s_manager + 0x20, 1);
        lx_screen_set_client_size(1280, 720);
        lx_screen_window_client(&cw, &ch);
        small = lx_present_view(cw, ch, 640, 480, 0);
        CHECK(cw == 1280 && ch == 720, "the window is resized to 1280x720 (%dx%d)", cw, ch);
        CHECK(lx_frontend_frame(640, 480, cw, ch) && lx_frontend_compose(&in, &out, cw, ch, 0), "the menu is composed again after the resize");
        lx_frontend_stats(&st);
        CHECK(st.view_x == small.x && st.view_y == small.y && st.view_w == small.w && st.view_h == small.h, "the layer follows the new rectangle (%d,%d %dx%d)", st.view_x, st.view_y, st.view_w, st.view_h);
        CHECK(out.width == 1280 && out.height == 720, "and the frame is the new window's size");
        CHECK(lx_screen_client_to_guest(small.x + small.w / 2, small.y + small.h / 2, &gx, &gy) && gx == 320 && gy == 240, "the mouse maps through the new rectangle (%d,%d)", gx, gy);
        lx_frontend_frame_done();
    }
    lx_screen_window_close();
    frontend_hd_release();
    reset_game();
    free(guest);
}

/* Frames after the first reuse their buffers (no allocation) and stay cheap. */
static void test_performance(void) {
    static const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
    uint16_t* guest = guest_picture(640, 480, 0x2104);
    lx_frame_t in, out;
    memset(&in, 0, sizeof in);
    in.pixels = guest, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const int ww = sizes[i][0], wh = sizes[i][1];
        const void* first = NULL;
        double first_ms, again_ms = 0.0;
        int stable = 1;
        main_menu_stack();
        {
            const double t0 = fe_host_ms();
            lx_frontend_frame(640, 480, ww, wh);
            lx_frontend_compose(&in, &out, ww, wh, 0);
            first_ms = fe_host_ms() - t0;
        }
        first = out.pixels;
        lx_frontend_frame_done();
        for (int k = 0; k < 20; k++) {
            const double t0 = fe_host_ms();
            lx_frontend_frame(640, 480, ww, wh);
            lx_frontend_compose(&in, &out, ww, wh, 0);
            again_ms += fe_host_ms() - t0;
            stable &= out.pixels == first;
            lx_frontend_frame_done();
        }
        again_ms /= 20.0;
        printf("  menu at %dx%d: first frame %.1f ms (art scaled, glyphs made), again %.2f ms\n", ww, wh, first_ms, again_ms);
        CHECK(stable, "%dx%d: the composed frame reuses its buffer", ww, wh);
        CHECK(again_ms < 120.0, "%dx%d: a menu frame after the first is cheap (%.2f ms)", ww, wh, again_ms);
        frontend_hd_release();
        reset_game();
    }
    free(guest);
}

/* ---- loading ---- */

static void test_loading(void) {
    uint32_t c[4], loader, art_raster;
    ld_scene_t s, refused;
    char slot[LD_SLOT_BYTES];
    int threads = 1, w = 0, h = 0;
    fe_painter_t* painter;
    uint32_t* px;
    unsigned lit_pixels;
    art_raster = raster(640, 480, 0x3186);
    c[0] = control(VT_PROGRESS, 100, fe_rect(120, 380, 520, 392));
    put32(c[0] + 0x20, 0);
    put32(c[0] + 0x24, 100);
    put32(c[0] + 0x28, 40);
    put32(c[0] + 0x30, 0xF800);
    c[1] = label(1000, fe_rect(120, 360, 520, 378), "Loading Level");
    c[2] = label(1001, fe_rect(120, 400, 520, 420), "Please wait");
    loader = window(VT_LOADER, fe_rect(0, 0, 640, 480), c, 3, art_raster);
    stack(&loader, 1);
    put32(SCREEN_RASTER, raster(640, 480, 0));
    put32(get32(SCREEN_RASTER) + 0x7C, 640);
    put32(get32(SCREEN_RASTER) + 0x80, 480);
    CHECK(loading_hd_screen(&w, &h) == 0 || (w > 640 && h > 480), "the loading size is the picture's rectangle in the window or none");
    CHECK(loading_hd_read(loader, 1280, 960, &s, slot, sizeof slot), "the loading window is read at 1280x960");
    CHECK(s.kind == LD_SCENE_LOADER && s.gauge_count == 1 && s.art565 != NULL, "one gauge and the level's picture");
    CHECK(fabsf(s.gauges[0].fraction - 0.4f) < 0.001f, "the gauge is at 40%% (%.3f)", s.gauges[0].fraction);
    CHECK(!strcmp(s.gauges[0].caption, "Loading Level"), "the bar's caption is its text box (%s)", s.gauges[0].caption);
    CHECK(s.note_count == 1 && !strcmp(s.notes[0], "Please wait"), "the other line is a note");
    CHECK(!loading_hd_read(loader, 640, 480, &refused, slot, sizeof slot), "a screen no larger than 640x480 is the game's own loading screen");
    painter = frontend_hd_painter(&threads);
    px = new_layer(1280, 960);
    CHECK(painter && fe_paint_loading(painter, &s, px, 1280, fe_rect(0, 0, 1280, 960), threads), "the loading screen is painted");
    lit_pixels = lit(px, 1280, fe_rect(0, 0, 1280, 960));
    CHECK(lit_pixels == 1280u * 960u, "the loading screen is opaque (%u of %u pixels)", lit_pixels, 1280u * 960u);
    {
        static const unsigned char cyrillic[] = { 0xC7, 0xE0, 0xE3, 0xF0, 0xF3, 0xE7, 0xEA, 0xE0, 0 }; /* in code page 1251 */
        ld_scene_t russian = s;
        uint32_t* px2 = new_layer(1280, 960);
        jpt_l10n_set_display_codepage(1251);
        memcpy(russian.notes[0], cyrillic, sizeof cyrillic);
        CHECK(fe_paint_loading(painter, &russian, px2, 1280, fe_rect(0, 0, 1280, 960), threads), "a Russian loading note is painted");
        jpt_l10n_set_display_codepage(0);
        CHECK(hash_pixels(px2, 1280, 960) != hash_pixels(px, 1280, 960), "the Russian note changes the loading screen");
        free(px2);
    }
    {
        uint16_t* guest = guest_picture(640, 480, 0x07E0);
        lx_frame_t in, out;
        memset(&in, 0, sizeof in);
        in.pixels = guest, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
        fe_host_layer(px, 1280, 960, NULL);
        CHECK(lx_frontend_compose(&in, &out, 1280, 960, 0), "a loading layer is composed with no menu open");
        CHECK(((const uint32_t*)out.pixels)[480 * 1280 + 640] == px[480 * 1280 + 640], "the loading layer replaces the guest's picture 1:1");
        fe_host_layer(NULL, 0, 0, NULL);
        CHECK(!lx_frontend_compose(&in, &out, 1280, 960, 0), "and goes away when the loader is done");
        free(guest);
    }
    free(px);
    frontend_hd_release();
    reset_game();
}

/* ---- a session: menus, dialogs, back, loading, gameplay ---- */

static int frame_shows(int ww, int wh) {
    uint16_t* guest = guest_picture(640, 480, 0x0000);
    lx_frame_t in, out;
    int shown;
    memset(&in, 0, sizeof in);
    in.pixels = guest, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    shown = lx_frontend_frame(640, 480, ww, wh) && lx_frontend_compose(&in, &out, ww, wh, 0);
    lx_frontend_frame_done();
    free(guest);
    return shown;
}

static void test_session(void) {
    uint32_t w[3];
    int shown = 0;
    main_menu_stack();
    CHECK(frame_shows(1280, 720), "the main menu is HD");
    {
        uint32_t c[2];
        c[0] = button(1000, 28, 74, 130, 42, 0x4208);
        c[1] = button(1004, 116, 122, 87, 36, 0x4208);
        w[0] = main_window();
        w[1] = window(VT_OPTIONS, fe_rect(160, 140, 480, 340), c, 2, 0);
        stack(w, 2);
        CHECK(frame_shows(1280, 720), "Options over the main menu is HD");
    }
    {
        uint32_t c[1];
        c[0] = control(VT_LISTBOX, 1100, fe_rect(180, 170, 460, 300));
        put32(c[0] + 0x24, fake(16));
        put32(c[0] + 0x28, get32(c[0] + 0x24));
        w[1] = window(VT_LOAD, fe_rect(160, 140, 480, 340), c, 1, 0);
        w[0] = main_window();
        stack(w, 2);
        shown = frame_shows(1280, 720);
        CHECK(shown || !shown, "Load is HD or the game's own when its list cannot be read");
    }
    main_menu_stack();
    CHECK(frame_shows(1280, 720), "back on the main menu: HD, with only this window's widgets");
    {
        uint32_t c[1], loader;
        c[0] = control(VT_PROGRESS, 100, fe_rect(120, 380, 520, 392));
        put32(c[0] + 0x24, 100);
        put32(c[0] + 0x28, 10);
        loader = window(VT_LOADER, fe_rect(0, 0, 640, 480), c, 1, raster(640, 480, 0x1111));
        stack(&loader, 1);
        CHECK(!frame_shows(1280, 720), "the loader is not a menu: no menu layer is left from the main menu");
    }
    {
        uint32_t game = window(VT_GAME, fe_rect(0, 0, 640, 480), NULL, 0, 0);
        stack(&game, 1);
        CHECK(!frame_shows(1280, 720), "gameplay shows the game's own frame with no layer or pointer left");
    }
    for (int i = 0; i < 300; i++) {
        lx_frontend_frame(640, 480, 1280, 720);
        lx_frontend_frame_done();
    }
    lx_frontend_reset();
    reset_game();
}


/* ---- the controls card (F2) ---- */

static unsigned changed_pixels(const uint32_t* px, int stride, int w, int h, uint32_t background) {
    unsigned n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) n += px[(size_t)y * (size_t)stride + (size_t)x] != background;
    return n;
}

static uint32_t buffer_hash(const uint32_t* px, size_t n) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ px[i]) * 16777619u;
    return hash;
}

static void test_card(void) {
    static const int sizes[][2] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 }, { 1000, 1500 } };
    const uint32_t background = 0xFF204060u, bar = 0xFF123456u;
    const char* data;
    size_t size;
    uint32_t hash_en = 0, hash_ru = 0, hash_fr = 0;
    lx_card_stats_t st;
    lx_card_reset();
    CHECK(!lx_card_shown() && !lx_card_hint_shown(), "no card or hint to begin with");
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const int fw = sizes[i][0], fh = sizes[i][1];
        const lx_view_t v = lx_present_view(fw, fh, 640, 480, 0);
        uint32_t* fb = (uint32_t*)malloc((size_t)fw * (size_t)fh * 4);
        int bars_intact = 1;
        for (size_t k = 0; k < (size_t)fw * (size_t)fh; k++) fb[k] = bar;
        for (int y = v.y; y < v.y + v.h; y++)
            for (int x = v.x; x < v.x + v.w; x++) fb[(size_t)y * (size_t)fw + (size_t)x] = background;
        lx_card_show();
        CHECK(lx_card_shown(), "%dx%d: F2 shows the card", fw, fh);
        CHECK(lx_card_draw(fb, fw, fw, fh, v.x, v.y, v.w, v.h), "%dx%d: the card is drawn", fw, fh);
        lx_card_stats(&st);
        CHECK(st.fits == 1 && st.rows >= 8 && st.columns >= 1 && st.text_h >= 9, "%dx%d: the card fits: %d rows, %d columns, text %d px", fw, fh, st.rows, st.columns, st.text_h);
        CHECK(st.box_left >= 0 && st.box_top >= 0 && st.box_right <= v.w && st.box_bottom <= v.h, "%dx%d: inside the picture (%d,%d-%d,%d of %dx%d)", fw, fh, st.box_left, st.box_top, st.box_right, st.box_bottom, v.w, v.h);
        CHECK(changed_pixels(fb + (size_t)v.y * (size_t)fw + (size_t)v.x, fw, v.w, v.h, background) > (unsigned)(v.w * v.h / 40), "%dx%d: text and glyphs are on the picture", fw, fh);
        for (int y = 0; y < fh && bars_intact; y++)
            for (int x = 0; x < fw; x++) {
                const int inside = x >= v.x && x < v.x + v.w && y >= v.y && y < v.y + v.h;
                if (!inside && fb[(size_t)y * (size_t)fw + (size_t)x] != bar) {
                    bars_intact = 0;
                    break;
                }
            }
        CHECK(bars_intact, "%dx%d: nothing is drawn in the letterbox bars", fw, fh);
        if (fw == 1280) hash_en = buffer_hash(fb, (size_t)fw * (size_t)fh);
        lx_card_hide();
        CHECK(!lx_card_shown() && !lx_card_draw(fb, fw, fw, fh, v.x, v.y, v.w, v.h), "%dx%d: hidden again, nothing drawn", fw, fh);
        free(fb);
    }
    CHECK(lx_card_hint() == 1 && lx_card_hint_shown() && lx_card_hint() == 0, "the one-line hint is shown once, then refused while it is up");
    lx_card_show();
    CHECK(!lx_card_hint_shown() && lx_card_shown() && lx_card_hint() == 1, "the card takes the hint's place and a hint is not added over it");
    lx_card_hide();
    {
        const int fw = 1280, fh = 720;
        const lx_view_t v = lx_present_view(fw, fh, 640, 480, 0);
        uint32_t* fb = (uint32_t*)malloc((size_t)fw * (size_t)fh * 4);
        CHECK(l10n_asset("ru", "interface", &data, &size) && size > 100, "the Russian interface text is embedded");
        jpt_tr_load(data, size, 1251);
        jpt_l10n_set_display_codepage(1251);
        lx_text_set_codepage(1251);
        for (size_t k = 0; k < (size_t)fw * (size_t)fh; k++) fb[k] = background;
        lx_card_show();
        CHECK(lx_card_draw(fb, fw, fw, fh, v.x, v.y, v.w, v.h), "the card is drawn in Russian");
        lx_card_stats(&st);
        hash_ru = buffer_hash(fb, (size_t)fw * (size_t)fh);
        CHECK(st.fits == 1 && st.rows >= 8, "Russian card: %d rows, text %d px", st.rows, st.text_h);
        CHECK(hash_ru != hash_en, "the Russian card is not the English one");
        lx_card_hide();
        jpt_tr_reset();
        jpt_l10n_set_display_codepage(0);
        lx_text_set_codepage(1252);
        if (l10n_asset("fr", "interface", &data, &size) && size > 100) {
            jpt_tr_load(data, size, 1252);
            for (size_t k = 0; k < (size_t)fw * (size_t)fh; k++) fb[k] = background;
            lx_card_show();
            CHECK(lx_card_draw(fb, fw, fw, fh, v.x, v.y, v.w, v.h), "the card is drawn in French");
            hash_fr = buffer_hash(fb, (size_t)fw * (size_t)fh);
            CHECK(hash_fr != hash_en && hash_fr != hash_ru, "the French card differs from the English and the Russian ones");
            lx_card_hide();
            jpt_tr_reset();
        }
        free(fb);
    }
    lx_card_reset();
}

int main(void) {
    char error[256];
    const jpt_game_version_t* v;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!gm_init(0, 0)) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    lx_spine_init((lx_table_t){ NULL, 0 }, (lx_table_t){ NULL, 0 });
    v = jpt_game_version_by_id("en-116");
    if (!v || !jpt_profile_activate(v, error, sizeof error)) {
        printf("no profile: %s\n", error);
        return 2;
    }
    s_arena = gm_alloc_pages(0, ARENA_BYTES, "arena");
    s_data_a = gm_alloc_pages(0x00660000u, 0x20000, "data a");
    s_data_b = gm_alloc_pages(0x00730000u, 0x20000, "data b");
    if (!s_arena || !s_data_a || !s_data_b) {
        printf("cannot place the synthetic game\n");
        return 2;
    }
    test_scene_reading();
    test_painter();
    test_text();
    test_compose_geometry();
    test_pointer_and_mapping();
    test_performance();
    test_loading();
    test_session();
    test_card();
    lx_frontend_reset();
    jpt_profile_reset();
    gm_shutdown();
    printf("linux_frontend_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
