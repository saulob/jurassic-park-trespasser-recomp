/* The host's menus and HUD on Linux: the F10 Cheats panel, the F3 Objectives window, the compass and the minimap, drawn by the same Dear
 * ImGui code the Windows host draws with (debug_menu_panel.cpp) through the software backend into a frame, and driven by the shared
 * model (debug_menu.c, debug_menu_policy.c): open and close by key, pointer and click on the hit table, the layout the Windows
 * host uses, the minimap's disc at the place and size minimap_place_facing gives, the player's arrow at its centre. Headless. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compass_policy.h"
#include "debug_menu.h"
#include "debug_menu_panel.h"
#include "guest_abi.h"
#include "jpt_config.h"
#include "linux_guest_ui.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "l10n_assets.h"
#include "linux_panel.h"
#include "linux_text.h"
#include "minimap.h"

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

#define W 640
#define H 480

static uint32_t s_pixels[W * H];

static void clear_frame(void) { memset(s_pixels, 0, sizeof s_pixels); }

static unsigned lit_in(float x, float y, float w, float h) {
    unsigned n = 0;
    int x0 = (int)x, y0 = (int)y, x1 = (int)(x + w), y1 = (int)(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) n += s_pixels[j * W + i] != 0;
    return n;
}

static int gameplay_probe(guest_ui_view_t* v) {
    memset(v, 0, sizeof *v);
    v->ui = GUEST_UI_GAMEPLAY;
    v->kind = GUEST_WND_GAME;
    v->top = 0x00A00000u;
    v->depth = 1;
    return 1;
}

static int menu_probe(guest_ui_view_t* v) {
    memset(v, 0, sizeof *v);
    v->ui = GUEST_UI_MENU;
    v->kind = GUEST_WND_MAIN_MENU;
    v->top = 0x00A00000u;
    v->depth = 1;
    return 1;
}

static unsigned channel(uint32_t px, int shift) { return (px >> shift) & 0xFFu; }

static void test_minimap(void) {
    dmp_frame_t f;
    cp_hud_t hud;
    minimap_frame_t fr;
    const minimap_rect_t pic = { 0.0f, 0.0f, (float)W, (float)H };
    float heading = 0.0f;
    unsigned colours[64], distinct = 0;
    memset(&f, 0, sizeof f);
    memset(&hud, 0, sizeof hud);
    const minimap_bounds_t* bounds = minimap_bounds("BE");
    CHECK(bounds != NULL, "the beach level has its minimap bounds");
    snprintf(hud.minimap_level, sizeof hud.minimap_level, "BE");
    hud.minimap = 1;
    if (bounds) {
        hud.player_pos[0] = (bounds->min_x + bounds->max_x) * 0.5f;
        hud.player_pos[1] = (bounds->min_y + bounds->max_y) * 0.5f;
    }
    hud.heading = heading;
    CHECK(minimap_place_facing(&pic, hud.player_pos[0], hud.player_pos[1], heading, MM_ZOOM, &fr), "the minimap has a place in the picture");
    f.view_w = (float)W, f.view_h = (float)H, f.dpi_scale = 1.0f, f.hud = &hud;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    {
        const uint32_t centre = s_pixels[(int)fr.cy * W + (int)fr.cx], corner = s_pixels[3 * W + 3];
        const uint32_t ring = s_pixels[(int)fr.cy * W + (int)(fr.cx + fr.radius - 1.0f)];
        CHECK(channel(centre, 16) > 200 && channel(centre, 8) > 200 && channel(centre, 0) > 200,
              "the player's arrow is white at the minimap's centre (%06X)", centre);
        CHECK(corner == 0, "nothing is drawn outside the disc (%06X)", corner);
        CHECK(ring != 0, "the disc has its rim at the radius the shared geometry gives (%06X)", ring);
        CHECK(s_pixels[(int)fr.cy * W + (int)(fr.cx + fr.radius + 4.0f)] == 0, "and nothing beyond it");
    }
    for (int y = (int)(fr.cy - fr.radius); y < (int)(fr.cy + fr.radius); y += 3)
        for (int x = (int)(fr.cx - fr.radius); x < (int)(fr.cx + fr.radius); x += 3) {
            const uint32_t px = s_pixels[y * W + x];
            int seen = 0;
            for (unsigned i = 0; i < distinct; i++) seen |= colours[i] == px;
            if (!seen && distinct < 64) colours[distinct++] = px;
        }
    CHECK(distinct >= 4, "the disc holds the land, the water and the landmarks of the level (%u colours)", distinct);
}

static void test_compass(void) {
    dmp_frame_t f;
    cp_hud_t hud;
    cp_layout_t l;
    memset(&f, 0, sizeof f);
    memset(&hud, 0, sizeof hud);
    hud.compass = 1;
    hud.alpha = 1.0f;
    hud.heading = 0.0f;
    f.view_w = (float)W, f.view_h = (float)H, f.dpi_scale = 1.0f, f.hud = &hud;
    cp_layout_in(0.0f, 0.0f, (float)W, (float)H, &l);
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(l.left, l.top, l.right - l.left, l.height) > 200, "the compass strip is drawn along the top of the picture (%u pixels)",
          lit_in(l.left, l.top, l.right - l.left, l.height));
    CHECK(lit_in(0, (float)H / 2, (float)W, (float)H / 2) == 0, "and nothing below the HUD");
}

static void hud_target_ahead(cp_hud_t* hud) {
    memset(hud, 0, sizeof *hud);
    hud->alpha = 1.0f;
    hud->view = 1;
    hud->camera.rot[0] = 1.0f;
    hud->camera.tan_half_h = 0.75f;
    hud->target[1] = 50.0f;
}

static void test_hud_visibility(void) {
    dmp_frame_t f;
    cp_hud_t hud;
    cp_layout_t l;
    static uint32_t with_waypoint[W * H];
    const float centre_x = (float)W / 2 - 24.0f, centre_y = (float)H / 2 - 24.0f;
    const minimap_bounds_t* bounds = minimap_bounds("BE");
    memset(&f, 0, sizeof f);
    f.view_w = (float)W, f.view_h = (float)H, f.dpi_scale = 1.0f, f.hud = &hud;
    cp_layout_in(0.0f, 0.0f, (float)W, (float)H, &l);

    hud_target_ahead(&hud);
    hud.marker = 1;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(centre_x, centre_y, 48.0f, 48.0f) > 20 && lit_in(l.left, l.top, l.right - l.left, l.height) == 0,
          "compass off, waypoint on: the 3D diamond is drawn and the strip is not");

    hud.compass = 1;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(centre_x, centre_y, 48.0f, 48.0f) > 20 && lit_in(l.left, l.top, l.right - l.left, l.height) > 200,
          "compass on, waypoint on: both are drawn");

    hud.marker = 0;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(centre_x, centre_y, 48.0f, 48.0f) == 0 && lit_in(l.left, l.top, l.right - l.left, l.height) > 200,
          "compass on, waypoint off: the strip is drawn and no marker is");

    hud.compass = 0;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(0.0f, 0.0f, (float)W, (float)H) == 0, "compass and waypoint off: nothing is drawn");

    hud_target_ahead(&hud);
    snprintf(hud.minimap_level, sizeof hud.minimap_level, "BE");
    hud.minimap = 1;
    CHECK(bounds != NULL, "the beach level has its minimap bounds");
    if (!bounds) return;
    hud.player_pos[0] = (bounds->min_x + bounds->max_x) * 0.5f;
    hud.player_pos[1] = (bounds->min_y + bounds->max_y) * 0.5f;
    hud.waypoint = 1;
    hud.waypoint_pos[0] = hud.player_pos[0] + 30.0f;
    hud.waypoint_pos[1] = hud.player_pos[1] + 30.0f;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    memcpy(with_waypoint, s_pixels, sizeof with_waypoint);
    hud.waypoint = 0;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    CHECK(lit_in(0.0f, (float)H / 2, (float)W, (float)H / 2) > 1000 && memcmp(with_waypoint, s_pixels, sizeof with_waypoint) != 0,
          "minimap on, waypoint off: the map stays and only the objective marker leaves it");
}

static void test_cheats_panel(void) {
    const dml_hits_t* hits;
    unsigned lit, again;
    lx_guest_ui_set_probe(gameplay_probe);
    debug_menu_set_hooks(NULL, lx_panel_hits);
    CHECK(!debug_menu_visible(), "no menu is open to start with");
    debug_menu_f10();
    CHECK(debug_menu_visible() && debug_menu_open_menu() == 1, "F10 opens Cheats over a running level");
    clear_frame();
    CHECK(lx_panel_wanted(0) == 1, "an open menu is something to draw");
    CHECK(lx_panel_draw(s_pixels, W, H, W, NULL), "the panel is drawn");
    hits = lx_panel_hits();
    CHECK(hits && hits->valid && hits->has_close, "the draw leaves a hit table with its close button");
    if (!hits || !hits->valid) return;
    CHECK(hits->panel.x >= 0 && hits->panel.y >= 0 && hits->panel.x + hits->panel.w <= (float)W && hits->panel.y + hits->panel.h <= (float)H,
          "the panel sits inside the picture (%.0f,%.0f %.0fx%.0f)", (double)hits->panel.x, (double)hits->panel.y, (double)hits->panel.w, (double)hits->panel.h);
    CHECK(hits->panel.x + hits->panel.w > (float)W * 0.9f && hits->panel.y < (float)H * 0.2f, "docked in the top right corner, as on Windows (%.0f,%.0f)", (double)(hits->panel.x + hits->panel.w), (double)hits->panel.y);
    lit = lit_in(hits->panel.x, hits->panel.y, hits->panel.w, hits->panel.h);
    CHECK(lit > (unsigned)(hits->panel.w * hits->panel.h * 0.5f), "the panel's body is painted (%u of %.0f pixels)", lit, (double)(hits->panel.w * hits->panel.h));
    CHECK(lit_in(0, 0, hits->panel.x - 2.0f, (float)H) == 0, "and nothing is drawn to its left");
    CHECK(hits->count > 0, "the rows are on the hit table (%d)", hits->count);
    clear_frame();
    lx_panel_draw(s_pixels, W, H, W, NULL);
    again = lit_in(hits->panel.x, hits->panel.y, hits->panel.w, hits->panel.h);
    CHECK(again == lit, "the same frame draws the same pixels (%u, %u)", lit, again);
    CHECK(debug_menu_pointer(0, (int)(hits->close.x + 2), (int)(hits->close.y + 2)) == 1, "the pointer over the open menu is taken by it");
    CHECK(debug_menu_pointer(1, (int)(hits->close.x + hits->close.w / 2), (int)(hits->close.y + hits->close.h / 2)) == 1 && !debug_menu_visible(),
          "a click on the close button closes it");
    debug_menu_f10();
    CHECK(debug_menu_visible(), "F10 opens it again");
    CHECK(debug_menu_key_event(0x1B, 1, 0, 0, 0) == 1 && !debug_menu_visible(), "Escape closes it and the game does not get the key");
    CHECK(debug_menu_pointer(1, 10, 10) == 0, "with it closed a click is the game's");
}

static void test_objectives_window(void) {
    const dml_hits_t* hits;
    lx_guest_ui_set_probe(gameplay_probe);
    debug_menu_f3();
    CHECK(debug_menu_visible() && debug_menu_open_menu() == 2, "F3 opens Objectives over a running level");
    clear_frame();
    CHECK(lx_panel_draw(s_pixels, W, H, W, NULL), "the window is drawn");
    hits = lx_panel_hits();
    CHECK(lit_in((float)W * 0.25f, (float)H * 0.25f, (float)W * 0.5f, (float)H * 0.5f) > 1000, "Objectives is a window in the middle of the picture");
    CHECK(hits != NULL, "it has a hit table");
    debug_menu_close();
    CHECK(!debug_menu_visible(), "and closes");
}

static void test_main_menu_refuses_objectives(void) {
    lx_guest_ui_set_probe(menu_probe);
    debug_menu_f3();
    CHECK(!debug_menu_visible(), "F3 does nothing with the main menu in front: Objectives belongs to a level");
    lx_guest_ui_set_probe(NULL);
}

static void test_counter(void) {
    clear_frame();
    CHECK(lx_panel_wanted(1) == 1, "the frame counter alone is something to draw");
    CHECK(lx_panel_draw(s_pixels, W, H, W, "FPS: 59.9") == 1, "it is drawn by the same code");
    CHECK(lit_in((float)W * 0.7f, 0, (float)W * 0.3f, 40) > 100 && lit_in(0, (float)H / 2, (float)W, (float)H / 2) == 0,
          "at the top right corner, as on Windows");
}

static unsigned coverage_sum;

static void count_pixel(void* ctx, int x, int y, int coverage) {
    (void)ctx, (void)x, (void)y;
    coverage_sum += (unsigned)coverage;
}

/* The project's own Russian text, as the game gets it (code page 1251) and as the panels get it (UTF-8): every letter of the alphabet
 * has the same advance and a drawn glyph through both paths. */
static void test_russian_text(void) {
    const char* data;
    size_t size;
    const float scale = lx_text_scale(-16);
    int letters = 0, mismatched = 0, blank = 0;
    CHECK(l10n_asset("ru", "interface", &data, &size) && size > 100, "the Russian interface text is embedded");
    lx_text_set_codepage(1251);
    CHECK(lx_text_codepage() == 1251, "the guest's strings are drawn in code page 1251");
    for (int b = 0xC0; b <= 0xFF; b++) {
        const int unicode = 0x410 + (b - 0xC0);
        letters++;
        mismatched += lx_text_advance(scale, (uint8_t)b, 0) != lx_text_unicode_advance(scale, unicode);
        coverage_sum = 0;
        lx_text_glyph(scale, (uint8_t)b, 0, 0, 20, count_pixel, NULL);
        blank += coverage_sum == 0;
    }
    CHECK(letters == 64 && !mismatched && !blank, "all 64 Russian letters: the byte and the Unicode path agree and every glyph is drawn (%d mismatched, %d blank)", mismatched, blank);
    {
        char cp[256];
        const char* line = strstr(data, "НОВАЯ ИГРА");
        size_t n = line ? jpt_utf8_to_cp(1251, "НОВАЯ ИГРА", cp, sizeof cp) : (size_t)-1;
        CHECK(line && n == 10 && (uint8_t)cp[0] == 0xCD && (uint8_t)cp[1] == 0xCE, "NOVAYA IGRA converts to the bytes the game draws (%02X %02X)", (uint8_t)cp[0], (uint8_t)cp[1]);
        CHECK(lx_text_width(scale, cp, (int)n, 0) > 60, "and has a Cyrillic width, not the Latin-1 one");
    }
    lx_text_set_codepage(1252);
    CHECK(lx_text_advance(scale, 0xCD, 0) != lx_text_unicode_advance(scale, 0x41D), "the same byte is a Latin letter in code page 1252");
}

static void test_russian_panel(void) {
    dmp_frame_t f;
    dm_row_t rows[2];
    memset(&f, 0, sizeof f);
    memset(rows, 0, sizeof rows);
    rows[0].kind = DM_ROW_SECTION;
    snprintf(rows[0].text, sizeof rows[0].text, "НОВАЯ ИГРА НАСТРОЙКИ");
    f.view_w = (float)W, f.view_h = (float)H, f.dpi_scale = 1.0f;
    f.menu = 1, f.title = "ЦЕЛИ", f.rows = rows, f.row_count = 1, f.help_lines = DMP_HELP_LINES;
    clear_frame();
    dmp_draw_software(s_pixels, W, H, W, &f);
    {
        const dml_hits_t* hits = lx_panel_hits();
        CHECK(hits && hits->valid, "a panel with Russian title and row draws");
        if (hits && hits->valid) {
            const unsigned lit = lit_in(hits->panel.x, hits->panel.y + 2, hits->panel.w, 60);
            CHECK(lit > 400, "its header and first row carry Cyrillic glyphs, not missing-glyph boxes (%u lit pixels)", lit);
        }
    }
}

/* Level Select numbers the retail levels from the one table, in the game's order, on this host as on Windows. */
static void test_level_select_numbers(void) {
    static const struct { const char* scene; const char* row; int number; } k[] = {
        { "be.scn", "1 - Beach", 1 }, { "jr.scn", "2 - Jungle Road", 2 }, { "ij.scn", "3 - Industrial Jungle", 3 },
        { "it.scn", "4 - InGen Town", 4 }, { "lab.scn", "5 - InGen Lab", 5 }, { "as.scn", "6 - Ascent, Part 1", 6 },
        { "as2.scn", "7 - Ascent, Part 2", 7 }, { "sum.scn", "8 - Summit", 8 },
        { "TestScene.scn", "Test Scene", 0 }, { "TestScnNght.scn", "Test Scene Night", 0 },
    };
    char row[DM_TEXT_MAX];
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        CHECK(dm_level_row_text(k[i].scene, row, sizeof row) && !strcmp(row, k[i].row) && dm_level_number(k[i].scene) == k[i].number,
              "Level Select formats %s as \"%s\" (got \"%s\")", k[i].scene, k[i].row, row);
    CHECK(dm_level_number("custom.scn") == 0 && dm_level_row_text("custom.scn", row, sizeof row) && !strcmp(row, "CUSTOM"),
          "a scene the game did not ship carries no number");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_level_select_numbers();
    test_minimap();
    test_compass();
    test_hud_visibility();
    test_cheats_panel();
    test_objectives_window();
    test_main_menu_refuses_objectives();
    test_counter();
    test_russian_text();
    test_russian_panel();
    lx_panel_release();
    printf("linux_panel_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
