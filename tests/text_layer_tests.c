/* The game's text as a layer of its own (text_layer.c), headless: what both renderers capture, that the pixels around a
 * line never come back, that nothing outlives its frame, and that the two renderers share one draw order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_layer.h"

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

enum { W = 320, H = 240, WHITE = 0xFFFF, GREEN = 0x07E0, BLUE = 0x001F };

static uint16_t s_surface[H][W], s_before[H][W];
static uint32_t s_table[65536];
static uint32_t s_out[H * W];

static void clear_surface(uint16_t background) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) s_surface[y][x] = background;
    memcpy(s_before, s_surface, sizeof s_surface);
}

/* A glyph-like run: every other pixel of a one-pixel-high row, as aliased text leaves it between letters. */
static void draw_text(int x0, int y, int count, uint16_t colour) {
    for (int i = 0; i < count; i++) s_surface[y][x0 + i * 2] = colour;
}

static int layer_pixel(const tl_view_t* v, int x, int y) {
    tl_rect_t one = { x, y, x + 1, y + 1 };
    uint32_t px = 0;
    if (x < v->dirty.left || x >= v->dirty.right || y < v->dirty.top || y >= v->dirty.bottom) return 0;
    tl_expand(&one, s_table, &px, 1);
    return px != 0;
}

static int layer_count(const tl_view_t* v) {
    int n = 0;
    for (int y = v->dirty.top; y < v->dirty.bottom; y++)
        for (int x = v->dirty.left; x < v->dirty.right; x++) n += layer_pixel(v, x, y);
    return n;
}

/* One software text draw: announce the box, draw, release the DC. */
static void soft_draw(tl_rect_t box, int x0, int y, int count, uint16_t colour) {
    CHECK(tl_claim((uint8_t*)s_surface, W * 2, W, H, &box), "the box is claimed");
    draw_text(x0, y, count, colour);
    tl_settle((const uint8_t*)s_surface);
}

/* What the presentation shows: the surface, the minimap over it where it is, then the text layer over both. */
static uint32_t compose(int x, int y, int in_minimap, const tl_view_t* layer) {
    uint32_t px = s_table[s_surface[y][x]];
    if (in_minimap) px = 0xFFFF0000u;
    if (layer && layer_pixel(layer, x, y)) {
        tl_rect_t one = { x, y, x + 1, y + 1 };
        tl_expand(&one, s_table, &px, 1);
    }
    return px;
}

static void test_software_capture(void) {
    tl_view_t v;
    const tl_rect_t line = { 60, 100, 200, 120 };
    clear_surface(GREEN);
    soft_draw(line, 70, 108, 20, WHITE);
    CHECK(memcmp(s_surface[100], s_before[100], sizeof s_surface[100]) == 0, "the rows around the line are untouched");
    for (int x = 60; x < 200; x++)
        CHECK(s_surface[108][x] == ((x >= 70 && x < 110 && (x - 70) % 2 == 0) ? WHITE : GREEN),
              "the frame keeps the text and the picture it had (x %d)", x);
    CHECK(tl_current(&v) && layer_count(&v) == 20, "only the 20 text pixels are in the layer (%d)", layer_count(&v));
    CHECK(!layer_pixel(&v, 71, 108) && !layer_pixel(&v, 65, 105), "the pixels between and around the letters are not text");
    tl_frame_end();
}

/* The text crosses the minimap's edge: the part over the minimap wins, and the rest of the box shows the minimap, not the
 * frame it was drawn over. */
static void test_over_minimap(void) {
    tl_view_t v;
    const tl_rect_t line = { 60, 100, 200, 120 };
    int over_text = 0, over_gap = 0, outside_text = 0;
    clear_surface(GREEN);
    soft_draw(line, 70, 108, 40, WHITE);
    CHECK(tl_current(&v), "the line is there to be presented");
    for (int x = 60; x < 200; x++) {
        const int minimap = x < 130;
        const uint32_t px = compose(x, 108, minimap, &v);
        const int text = x >= 70 && x < 150 && (x - 70) % 2 == 0;
        over_text += minimap && text && px == (s_table[WHITE] | 0xFF000000u);
        over_gap += minimap && !text && px == 0xFFFF0000u;
        outside_text += !minimap && text && px == (s_table[WHITE] | 0xFF000000u);
    }
    CHECK(over_text == 30, "the letters over the minimap are in front of it (%d)", over_text);
    CHECK(over_gap == 40, "between the letters the minimap shows through, not a rectangle of the old frame (%d)", over_gap);
    CHECK(outside_text == 10, "the letters off the minimap are the same (%d)", outside_text);
    for (int y = 100; y < 120; y++)
        if (y != 108)
            for (int x = 60; x < 200; x++)
                CHECK(compose(x, y, x < 130, &v) == (x < 130 ? 0xFFFF0000u : s_table[GREEN]),
                      "the rest of the box is the minimap or the picture (%d, %d)", x, y);
    tl_frame_end();
}

static void test_frames(void) {
    tl_view_t v;
    const tl_rect_t first = { 20, 40, 120, 60 }, second = { 150, 180, 250, 200 };
    clear_surface(GREEN);
    soft_draw(first, 30, 50, 10, WHITE);
    CHECK(tl_current(&v) && v.dirty.left == 20 && v.dirty.right == 120, "the first caption is the layer");
    tl_frame_end();
    CHECK(!tl_current(&v), "nothing is shown once the frame is over and no text was drawn");
    tl_frame_end();
    clear_surface(GREEN);
    soft_draw(second, 160, 190, 10, BLUE);
    CHECK(tl_current(&v) && layer_count(&v) == 10 && !layer_pixel(&v, 30, 50) && v.dirty.left == 150 && v.dirty.top == 180,
          "the new caption replaces the old one: none of its pixels are left (%d)", layer_count(&v));
    tl_frame_end();
    CHECK(!tl_current(&v), "and it ends with its frame");
}

static void test_lines(void) {
    tl_view_t v;
    clear_surface(GREEN);
    /* three lines, each drawn as the game does: a shadow pass offset by one pixel, then the colour pass over it */
    for (int line = 0; line < 3; line++) {
        const int y = 140 + line * 20;
        const tl_rect_t box = { 40, y - 2, 240, y + 18 };
        CHECK(tl_claim((uint8_t*)s_surface, W * 2, W, H, &box), "line %d is claimed", line);
        draw_text(41, y + 1, 30, BLUE);
        CHECK(tl_claim((uint8_t*)s_surface, W * 2, W, H, &box), "line %d is claimed again for the colour pass", line);
        draw_text(40, y, 30, WHITE);
        tl_settle((const uint8_t*)s_surface);
    }
    CHECK(tl_current(&v) && v.dirty.top == 138 && v.dirty.bottom == 198, "three lines span their boxes (%ld..%ld)", v.dirty.top,
          v.dirty.bottom);
    CHECK(layer_count(&v) == 3 * 30 * 2, "every pixel of every pass is kept, none twice (%d)", layer_count(&v));
    for (int line = 0; line < 3; line++) CHECK(layer_pixel(&v, 40, 140 + line * 20) && layer_pixel(&v, 41, 141 + line * 20), "line %d", line);
    for (int y = 138; y < 198; y++)
        for (int x = 40; x < 240; x++) {
            const int text = layer_pixel(&v, x, y);
            CHECK(text || s_surface[y][x] == GREEN, "no colour of the frame is lost where there is no text (%d, %d)", x, y);
        }
    tl_frame_end();
    /* a caption wider than the frame is cut at its edge */
    clear_surface(GREEN);
    {
        const tl_rect_t wide = { -30, 20, W + 30, 40 };
        CHECK(tl_claim((uint8_t*)s_surface, W * 2, W, H, &wide), "a box past both edges is claimed within the frame");
        draw_text(0, 30, W / 2, WHITE);
        tl_settle((const uint8_t*)s_surface);
        CHECK(tl_current(&v) && v.dirty.left == 0 && v.dirty.right == W && layer_count(&v) == W / 2, "and kept within it");
    }
    tl_frame_end();
    /* a box over a quarter of the frame is not text: nothing is claimed or marked */
    clear_surface(GREEN);
    {
        const tl_rect_t huge = { 0, 0, W, H };
        CHECK(!tl_claim((uint8_t*)s_surface, W * 2, W, H, &huge) && !memcmp(s_surface, s_before, sizeof s_surface),
              "a whole-frame box is refused and the frame is untouched");
    }
}

/* The GPU renderer claims tiles itself (the box is marker-filled before the draw); the layer it gets is the same. */
static void test_same_layer(void) {
    static uint16_t gpu[H][W];
    tl_view_t soft, hard;
    const tl_rect_t line = { 60, 100, 200, 120 };
    int differ = 0;
    clear_surface(GREEN);
    soft_draw(line, 70, 108, 20, WHITE);
    CHECK(tl_current(&soft), "software layer");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) gpu[y][x] = layer_pixel(&soft, x, y) ? WHITE : GREEN;
    tl_frame_end();
    for (int y = line.top; y < line.bottom; y++)
        for (int x = line.left; x < line.right; x++) gpu[y][x] = TL_MARKER;
    for (int i = 0; i < 20; i++) gpu[108][70 + i * 2] = WHITE;
    tl_note(W, H, &line);
    tl_scan((const uint8_t*)gpu, W * 2, W, H);
    CHECK(tl_current(&hard), "GPU layer");
    for (int y = 100; y < 120; y++)
        for (int x = 60; x < 200; x++) differ += layer_pixel(&hard, x, y) != (y == 108 && x >= 70 && x < 110 && !((x - 70) & 1));
    CHECK(differ == 0 && layer_count(&hard) == 20, "the GPU renderer's capture is the software renderer's (%d differ)", differ);
    tl_frame_end();
    /* it is the other renderer's frame boundary that ends it: a stale layer is never shown */
    CHECK(!tl_current(&hard), "no layer after the frame");
}

static void test_resize(void) {
    static uint16_t big[480][640];
    tl_view_t v;
    const tl_rect_t line = { 60, 100, 200, 120 }, other = { 300, 400, 500, 420 };
    clear_surface(GREEN);
    soft_draw(line, 70, 108, 10, WHITE);
    tl_frame_end();
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) big[y][x] = GREEN;
    CHECK(tl_claim((uint8_t*)big, 640 * 2, 640, 480, &other), "a larger frame claims in its own size");
    for (int i = 0; i < 10; i++) big[410][310 + i * 2] = BLUE;
    tl_settle((const uint8_t*)big);
    CHECK(tl_current(&v) && v.w == 640 && v.h == 480 && layer_count(&v) == 10 && !layer_pixel(&v, 70, 108),
          "after a resize the layer follows the new frame and keeps nothing of the old one");
    tl_frame_end();
}

/* An unreleased claim never leaves the marker in the picture. */
static void test_unreleased(void) {
    tl_view_t v;
    const tl_rect_t line = { 60, 100, 200, 120 };
    clear_surface(GREEN);
    CHECK(tl_claim((uint8_t*)s_surface, W * 2, W, H, &line) && s_surface[110][100] == TL_MARKER, "the box is marked while drawn");
    tl_frame_end();
    CHECK(!memcmp(s_surface, s_before, sizeof s_surface) && !tl_current(&v), "ending the frame puts the picture back");
}

/* A working copy may have CRLF line endings (core.autocrlf); the source checks below look for LF. */
static char* lf_only(char* text) {
    char* out = text;
    for (const char* in = text; text && *in; in++)
        if (!(in[0] == '\r' && in[1] == '\n')) *out++ = *in;
    if (text) *out = 0;
    return text;
}

static void test_line_endings(void) {
    char crlf[] = "minimap(f);\r\n        text_layer(\r\n", lf[] = "minimap(f);\n        text_layer(\n", lone[] = "a\rb\r";
    CHECK(!strcmp(lf_only(crlf), "minimap(f);\n        text_layer(\n") && !strcmp(lf_only(lf), "minimap(f);\n        text_layer(\n"),
          "a source reads the same with CRLF and LF line endings");
    CHECK(!strcmp(lf_only(lone), "a\rb\r") && lf_only(NULL) == NULL, "a lone carriage return is kept");
}

static char* read_file(const char* root, const char* path) {
    char name[512];
    FILE* f;
    long size;
    char* text;
    snprintf(name, sizeof name, "%s/%s", root, path);
    f = fopen(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) text[size] = 0;
    else {
        free(text);
        text = NULL;
    }
    fclose(f);
    return lf_only(text);
}

/* One draw order for both renderers: HUD and minimap, the game's text, the frame counter, then the windows; both present
 * paths reach it through debug_menu_overlay, and both feed the same layer. */
static void test_shared_order(const char* root) {
    char* panel = read_file(root, "src/host/debug_menu_panel.cpp");
    char* present = read_file(root, "src/host/ddraw/d3d11_present.c");
    char* menu = read_file(root, "src/host/debug_menu_win.c");
    char* renderer = read_file(root, "src/host/ddraw/d3d11_renderer.c");
    char* dd_surface = read_file(root, "src/host/ddraw/dd_surface.c");
    const char *hud, *text, *counter, *window;
    CHECK(panel && present && menu && renderer && dd_surface, "the sources are readable");
    if (!panel || !present || !menu || !renderer || !dd_surface) goto done;
    hud = strstr(panel, "minimap(f);\n        text_layer(");
    text = strstr(panel, "text_layer(ImGui::GetBackgroundDrawList(), f);");
    counter = strstr(panel, "frame_counter(ImGui::GetBackgroundDrawList(), f);");
    window = strstr(panel, "if (f->menu) menu(f);");
    CHECK(hud && text && counter && window && hud < counter && text < counter && counter < window,
          "minimap, then the game's text, then the counter, then the windows");
    CHECK(!strstr(panel, "GetForegroundDrawList"), "nothing is drawn over the windows");
    CHECK(strstr(present, "debug_menu_overlay(s_p.device, s_p.context, view,") && strstr(present, "debug_menu_overlay(s_p.device, s_p.context, s_p.back_rtv,"),
          "the software and the hardware present both draw the same overlay");
    CHECK(strstr(menu, "present_take_text_layer(&text.texture") && strstr(present, "int present_take_text_layer("),
          "one place hands the layer to the overlay");
    CHECK(strstr(renderer, "tl_scan(") && strstr(renderer, "tl_claim(") && strstr(renderer, "tl_settle(") && strstr(renderer, "tl_frame_end()") &&
              strstr(dd_surface, "tl_frame_end()"),
          "both renderers feed the layer and end its frame");
done:
    free(panel);
    free(present);
    free(menu);
    free(renderer);
    free(dd_surface);
}

void text_layer_tests(int* checks, int* failures, const char* root) {
    g_checks = checks;
    g_failures = failures;
    for (uint32_t v = 0; v < 65536; v++) s_table[v] = 0xFF000000u | (v * 2654435761u >> 8);
    test_software_capture();
    test_over_minimap();
    test_frames();
    test_lines();
    test_same_layer();
    test_resize();
    test_unreleased();
    test_line_endings();
    test_shared_order(root);
    (void)s_out;
}
