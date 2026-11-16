/* The F5 / F10 panel's geometry and pointer, headless: where the header, close button, rows and footer go at each screen
 * size, the columns of Level Select, which row a click lands on, and what the menus show without coordinates. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats_policy.h"
#include "debug_menu_layout.h"
#include "debug_menu_policy.h"
#include "jpt_gamepad.h"
#include "jpt_tr.h"

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

static const struct {
    float w, h;
} k_views[] = { { 640, 480 }, { 800, 600 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
static const float k_scales[] = { 1.0f, 1.5f, 2.25f };

/* A body of n rows at the default font's pitch (13 px line, 4 px spacing, scaled). */
static float rows_height(int n, float scale) { return (float)n * (13.0f + 4.0f) * scale; }

/* The footer is the fixed help area alone (DMP_HELP_LINES lines), whatever is selected. */
static void check_layout(const char* what, float vw, float vh, float scale, float content_chars, int rows) {
    const float em = 13.0f * scale, pad = 8.0f * scale, char_w = em * 0.55f;
    const float title_w = 6.0f * char_w, content_w = content_chars * char_w;
    dml_params_t p;
    dml_layout_t l;
    const dml_rect_t view = { 0.0f, 0.0f, vw, vh };
    p.view_w = vw;
    p.view_h = vh;
    p.em = em;
    p.pad = pad;
    p.panel_w = dml_panel_width(vw, vh, em, pad, em * 24.0f, content_w, title_w);
    p.body_h = rows_height(rows, scale);
    p.footer_h = 2.0f * (em + 4.0f * scale);
    dml_layout(&p, &l);
    CHECK(dml_inside(&l.panel, &view), "%s %.0fx%.0f x%.1f: the panel is inside the screen", what, vw, vh, scale);
    CHECK(l.panel.w <= vw - 2.0f * dml_margin(vw, vh, em) + 0.01f, "%s %.0fx%.0f: the panel keeps its margins", what, vw, vh);
    CHECK(dml_inside(&l.header, &l.panel) && l.header.h >= 30.0f, "%s %.0fx%.0f: the header is in the panel", what, vw, vh);
    CHECK(dml_inside(&l.close, &l.header), "%s %.0fx%.0f x%.1f: the close button is inside the header", what, vw, vh, scale);
    CHECK(l.close.w >= 24.0f && l.close.h >= 24.0f && l.close.w == l.close.h, "%s: the close button is a comfortable square (%.1f)",
          what, l.close.w);
    CHECK(l.close.x >= l.panel.x + l.panel.w * 0.5f, "%s: the close button is on the right", what);
    CHECK(dml_inside(&l.title, &l.header) && l.title.x + l.title.w <= l.close.x, "%s: the title area ends before the button", what);
    CHECK(title_w <= l.title.w + 0.01f, "%s %.0fx%.0f x%.1f: the title fits beside the button (%.1f of %.1f)", what, vw, vh,
          scale, title_w, l.title.w);
    CHECK(dml_inside(&l.body, &l.panel) && l.body.y >= l.header.y + l.header.h - 0.01f, "%s: the rows are below the header", what);
    CHECK(dml_inside(&l.footer, &l.panel) && l.footer.y >= l.body.y + l.body.h - 0.01f,
          "%s %.0fx%.0f x%.1f: the footer is inside the panel, below the rows", what, vw, vh, scale);
    CHECK(l.footer.y + l.footer.h <= l.panel.y + l.panel.h + 0.01f, "%s: the footer is not cut off", what);
    CHECK(l.panel.h <= vh - 2.0f * dml_margin(vw, vh, em) + 0.01f || l.body.h <= 0.0f, "%s: the panel height fits the screen", what);
    CHECK(l.scrolls == (rows_height(rows, scale) > l.body.h + 0.01f), "%s: scrolling only when the rows do not fit", what);
}

static void test_panel_layout(void) {
    for (size_t v = 0; v < sizeof k_views / sizeof k_views[0]; v++)
        for (size_t s = 0; s < sizeof k_scales / sizeof k_scales[0]; s++) {
            /* F10: eleven rows, the widest about 30 characters */
            check_layout("main", k_views[v].w, k_views[v].h, k_scales[s], 30.0f, 11);
            /* Level Select: the current level, its title, the ten names and Back */
            check_layout("levels", k_views[v].w, k_views[v].h, k_scales[s], 24.0f, 13);
            check_layout("teleport", k_views[v].w, k_views[v].h, k_scales[s], 48.0f, 8);
            /* more rows than any screen holds: the body scrolls, the footer and the header stay */
            check_layout("tall", k_views[v].w, k_views[v].h, k_scales[s], 30.0f, 80);
        }
}

static void test_widths(void) {
    const float em = 13.0f, pad = 8.0f;
    CHECK(dml_panel_width(1920, 1080, em, pad, em * 24.0f, 30.0f * em * 0.55f, 6.0f * em * 0.55f) == em * 24.0f,
          "the menus keep their minimum width: the values they show never widen them");
    CHECK(dml_panel_width(640, 480, em, pad, em * 24.0f, 5000.0f, 50.0f) <= 640.0f - 2.0f * dml_margin(640, 480, em) + 0.01f,
          "a very wide row never makes the panel wider than the screen");
    CHECK(dml_panel_width(640, 480, em, pad, em * 24.0f, 10.0f, 400.0f) >= 400.0f + 2.0f * pad + 30.0f - 0.01f,
          "a long title keeps its room beside the button");
    for (size_t v = 0; v < sizeof k_views / sizeof k_views[0]; v++)
        for (size_t s = 0; s < sizeof k_scales / sizeof k_scales[0]; s++) {
            const float e = 13.0f * k_scales[s], p = 8.0f * k_scales[s], name = 16.0f * e * 0.55f;
            const float w = dml_panel_width(k_views[v].w, k_views[v].h, e, p, e * 24.0f, name, 6.0f * e * 0.55f);
            CHECK(name <= w - 2.0f * p, "%.0fx%.0f x%.2f: Test Scene Night fits whole (%.0f of %.0f)", k_views[v].w, k_views[v].h,
                  k_scales[s], name, w - 2.0f * p);
        }
}

static void test_columns(void) {
    const float widths[3] = { 100.0f, 80.0f, 40.0f };
    float x[3];
    CHECK(dml_columns(widths, 3, 400.0f, 20.0f, 8.0f, x) && x[0] == 0.0f && x[1] == 120.0f && x[2] == 220.0f,
          "roomy columns keep their gap");
    CHECK(dml_columns(widths, 3, 250.0f, 20.0f, 8.0f, x) && x[1] >= 108.0f && x[2] + 40.0f <= 250.01f,
          "short columns close up and still do not touch (%.1f %.1f)", x[1], x[2]);
    CHECK(!dml_columns(widths, 3, 200.0f, 20.0f, 8.0f, x), "columns that cannot fit say so");
    CHECK(!dml_columns(widths, 0, 200.0f, 20.0f, 8.0f, x) && dml_columns(widths, 1, 200.0f, 20.0f, 8.0f, x),
          "one column always fits, none is refused");
}

static dm_world_t world(const char* scene, const char* level) {
    dm_world_t w;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_LEVEL;
    snprintf(w.scene, sizeof w.scene, "%s", scene);
    snprintf(w.level, sizeof w.level, "%s", level);
    w.player = 1;
    w.at.pos[0] = 12.5f;
    w.at.pos[1] = -3.25f;
    w.at.pos[2] = 7.0f;
    w.at.rot[0] = 1.0f;
    w.ai = 0;
    w.invulnerable = w.ammo = 0;
    w.hp = 90.0f;
    w.hp_max = 100.0f;
    w.camera_head = 1;
    w.speed = w.jump = CH_MULTIPLIER_DEFAULT;
    w.key_index = -1;
    return w;
}

static const dm_row_t* find(const dm_row_t* rows, int n, int item) {
    for (int i = 0; i < n; i++)
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == item) return &rows[i];
    return NULL;
}

static const dm_row_t* find_text(const dm_row_t* rows, int n, const char* text) {
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].text, text)) return &rows[i];
    return NULL;
}

static int has_coordinates(const dm_row_t* rows, int n) {
    for (int i = 0; i < n; i++)
        if (strstr(rows[i].value, "Yaw") || strstr(rows[i].text, "Yaw") || !strcmp(rows[i].text, "Player")) return 1;
    return 0;
}

static dm_levels_t levels(void) {
    static const char* const names[] = { "be.scn", "jr.scn", "ij.scn", "it.scn", "lab.scn", "as.scn", "as2.scn", "sum.scn",
                                         "testscene.scn", "testscnnght.scn" };
    dm_levels_t l;
    memset(&l, 0, sizeof l);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) dm_levels_add(&l, names[i]);
    return l;
}

static void test_level_names(void) {
    static const struct {
        const char *scene, *id, *name;
    } k[] = { { "be.scn", "BE", "Beach" }, { "jr.scn", "JR", "Jungle Road" }, { "ij.scn", "IJ", "Industrial Jungle" },
              { "it.scn", "IT", "InGen Town" }, { "lab.scn", "LAB", "InGen Lab" }, { "as.scn", "AS", "Ascent, Part 1" },
              { "as2.scn", "AS2", "Ascent, Part 2" }, { "sum.scn", "SUM", "Summit" },
              { "testscene.scn", "TESTSCENE", "Test Scene" }, { "testscnnght.scn", "TESTSCNNGHT", "Test Scene Night" } };
    char text[DM_TEXT_MAX];
    jpt_tr_reset();
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        dm_world_t w = world(k[i].scene, k[i].id);
        CHECK(dm_level_display(&w, text, sizeof text) && !strcmp(text, k[i].name), "%s is %s, not %s", k[i].id, k[i].name, text);
        memset(w.scene, 0, sizeof w.scene);
        CHECK(dm_level_display(&w, text, sizeof text) && !strcmp(text, k[i].name), "%s from the id alone: %s", k[i].id, text);
    }
    {
        dm_world_t w = world("mystery.scn", "MYSTERY");
        CHECK(dm_level_display(&w, text, sizeof text) && !strcmp(text, "MYSTERY"), "an unknown level keeps its id: %s", text);
        w = world("", "");
        CHECK(dm_level_display(&w, text, sizeof text) && !strcmp(text, "?"), "no level at all: %s", text);
        w = world("", "Beach");
        CHECK(dm_level_display(&w, text, sizeof text) && !strcmp(text, "Beach"), "a name already spelled out is not doubled: %s",
              text);
        CHECK(!dm_level_display(&w, text, 3), "a short buffer is refused, not overrun");
    }
}

static void test_main_menu_rows(void) {
    dm_state_t s;
    dm_world_t w = world("be.scn", "BE");
    dm_row_t rows[DM_ROWS_MAX];
    const dm_row_t* r;
    int n;
    jpt_tr_reset();
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!find_text(rows, n, "Level") && !find_text(rows, n, "Current level"), "the main menu shows no level");
    CHECK(!has_coordinates(rows, n), "no coordinates in the Cheats menu");
    {
        int titles = 0, items = 0, others = 0;
        for (int i = 0; i < n; i++) {
            titles += !strcmp(rows[i].text, "Cheats");
            if (rows[i].kind == DM_ROW_ITEM) {
                CHECK(items < JPT_CHEAT_COUNT ? !strcmp(rows[i].text, jpt_cheats[items].label) : rows[i].item == DM_CLOSE,
                      "row %d is %s", items, rows[i].text);
                items++;
            } else {
                others += rows[i].kind != DM_ROW_TITLE;
            }
        }
        CHECK(titles == 1 && rows[0].kind == DM_ROW_TITLE, "one Cheats title, the header's: %d", titles);
        CHECK(items == JPT_CHEAT_COUNT + 1 && others == 0, "only the cheats and Close below it (%d rows, %d others)", items,
              others);
    }
    for (int i = 0; i < n; i++)
        CHECK(!strstr(rows[i].value, "X ") && strcmp(rows[i].text, "Show Coordinates") && strcmp(rows[i].text, "Third Person"),
              "no X/Y/Z, Show Coordinates or Third Person: %s", rows[i].text);
    CHECK(find(rows, n, DM_CLOSE) && !find(rows, n, DM_BACK), "the main menu has Close and no Back");
    s.cursor = DM_LEVEL_SELECT;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    w = world("as2.scn", "AS2");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = find_text(rows, n, "Current level");
    CHECK(r && !strcmp(r->value, "Ascent, Part 2"), "AS2 is Ascent, Part 2: %s", r ? r->value : "none");
    w = world("zz.scn", "ZZ");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = find_text(rows, n, "Current level");
    CHECK(r && !strcmp(r->value, "ZZ"), "an unknown level shows its id: %s", r ? r->value : "none");
    dm_close(&s);
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(has_coordinates(rows, n), "the Debug Menu keeps its coordinates line");
}

static void test_heal_row(void) {
    dm_state_t s;
    dm_world_t w = world("be.scn", "BE");
    dm_row_t rows[DM_ROWS_MAX];
    dm_action_t a;
    const dm_row_t* r;
    float out = 0.0f;
    int n;
    jpt_tr_reset();
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = find(rows, n, DM_HEAL);
    CHECK(r && !strcmp(r->value, "90 / 100") && !strchr(r->value, '>'), "Heal shows the health without a submenu marker: %s",
          r ? r->value : "none");
    w.hp = 100.0f;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = find(rows, n, DM_HEAL);
    CHECK(r && !strcmp(r->value, "100 / 100"), "full health reads 100 / 100: %s", r ? r->value : "none");
    w.hp = 90.0f;
    s.cursor = DM_HEAL;
    CHECK(dm_help_text(&s, &w) && !strcmp(dm_help_text(&s, &w), "Fill health to maximum"), "Heal explains itself");
    s.cursor = DM_MOVE_SPEED;
    CHECK(!dm_help_text(&s, &w), "other rows add no help line");
    s.cursor = DM_HEAL;
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    s.cursor = DM_FREEZE_AI;
    CHECK(!dm_help_text(&s, &w), "the Debug Menu adds none");
    dm_close(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_HEAL;
    {
        dm_state_t t = s;
        dm_pad(&t, &w, 1u << JPT_PAD_SOUTH, 0);
        CHECK(dm_take(&t, &a) && a.kind == DM_ACT_HEAL && t.open, "A on Heal runs it and leaves the menu open");
    }
    CHECK(ch_heal(90.0f, 100.0f, &out) && out == 100.0f, "Heal takes 90 / 100 to 100 / 100");
    CHECK(ch_heal(100.0f, 100.0f, &out) && out == 100.0f, "Heal at 100 / 100 stays 100 / 100");
    CHECK(ch_heal(10.0f, 400.0f, &out) && out == 400.0f, "Heal fills to the maximum in force, Super Health's included");
    CHECK(!ch_heal(0.0f, 100.0f, &out), "the dead stay dead");
    {
        char line[64];
        CHECK(ch_format_change(line, sizeof line, DM_ACT_HEAL, 1, 0) && !strcmp(line, "Heal"), "the log names it: %s", line);
    }
}

static void test_level_select_page(void) {
    dm_state_t s;
    dm_world_t w = world("testscnnght.scn", "TESTSCNNGHT");
    dm_row_t rows[DM_ROWS_MAX];
    dm_levels_t l = levels();
    const dm_row_t* r;
    int n, back = -1;
    jpt_tr_reset();
    dm_init(&s);
    dm_set_levels(&s, &l);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_LEVEL_SELECT;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    dm_key(&s, &w, 13, 0, 0, 0, 0, 0);
    CHECK(s.page == DM_PAGE_LEVELS && s.open, "Enter opens Level Select");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!has_coordinates(rows, n), "Level Select shows no coordinates");
    r = find_text(rows, n, "Current level");
    CHECK(r && r->kind == DM_ROW_INFO && !strcmp(r->value, "Test Scene Night"), "Current level: Test Scene Night (%s)",
          r ? r->value : "none");
    CHECK(!find_text(rows, n, "Level"), "not Level:");
    for (int i = 0; i < n; i++)
        if (rows[i].kind == DM_ROW_ITEM && !strcmp(rows[i].text, "< Back")) back = i;
    CHECK(back == n - 1 && rows[back].item == l.count, "< Back ends the list");
    {
        static const char* const names[] = { "Beach", "Jungle Road", "Industrial Jungle", "InGen Town", "InGen Lab",
                                             "Ascent, Part 1", "Ascent, Part 2", "Summit", "Test Scene", "Test Scene Night" };
        static const char* const codes[] = { "BE", "JR", "IJ", "IT", "LAB", "AS", "AS2", "SUM", "TESTSCENE", "TESTSCNNGHT" };
        int listed = 0;
        for (int i = 0; i < n; i++) {
            if (rows[i].kind != DM_ROW_ITEM || rows[i].item == l.count) continue;
            CHECK(listed < 10 && !strcmp(rows[i].text, names[listed]), "level %d reads %s: %s", listed,
                  listed < 10 ? names[listed] : "-", rows[i].text);
            CHECK(!rows[i].value[0], "level %d has no second column: %s", listed, rows[i].value);
            CHECK(!strstr(rows[i].text, ".scn") && !strstr(rows[i].text, " - "), "no file or code: %s", rows[i].text);
            for (int c = 0; c < 10; c++)
                CHECK(strncmp(rows[i].text, codes[c], strlen(codes[c])) || !strcmp(rows[i].text, names[listed]) ||
                          rows[i].text[strlen(codes[c])] != ' ',
                      "no code before the name: %s", rows[i].text);
            CHECK(strcmp(rows[i].value, "current"), "no current marker");
            listed++;
        }
        CHECK(listed == 10, "the ten levels (%d)", listed);
        CHECK(s.level_cursor == 9, "the current level is selected when the list opens");
    }
    CHECK(!find(rows, n, DM_CLOSE), "no Close row in the submenu");
    s.level_cursor = l.count - 1;
    dm_key(&s, &w, 40, 1, 0, 0, 0, 0);
    CHECK(s.level_cursor == l.count, "Down from the last level reaches Back");
    dm_key(&s, &w, 40, 1, 0, 0, 0, 0);
    CHECK(s.level_cursor == 0, "and wraps round");
    dm_key(&s, &w, 38, 1, 0, 0, 0, 0);
    CHECK(s.level_cursor == l.count, "Up from the first goes to Back");
    {
        dm_action_t a;
        dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
        CHECK(s.open && s.page == DM_PAGE_MAIN && s.menu == DM_MENU_CHEATS && !dm_take(&s, &a), "Enter on Back returns to Cheats");
        CHECK(s.cursor == DM_LEVEL_SELECT, "on Level Select");
    }
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    s.level_cursor = l.count;
    dm_pad(&s, &w, 1u << JPT_PAD_SOUTH, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "A on Back returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    dm_key(&s, &w, 27, 1, 0, 0, 0, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "Esc leaves Level Select for Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    dm_pad(&s, &w, 1u << JPT_PAD_EAST, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "B leaves Level Select for Cheats");
    dm_key(&s, &w, 27, 1, 0, 0, 0, 0);
    CHECK(!s.open, "Esc in the main menu closes it");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_LEVEL_SELECT;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    CHECK(s.page == DM_PAGE_LEVELS, "Level Select again");
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && !s.open && s.page == DM_PAGE_MAIN, "F10 closes the menu from Level Select");
    {
        dm_state_t empty;
        dm_levels_t none;
        dm_init(&empty);
        memset(&none, 0, sizeof none);
        dm_set_levels(&empty, &none);
        dm_toggle(&empty, &w, DM_MENU_CHEATS);
        empty.cursor = DM_LEVEL_SELECT;
        dm_key(&empty, &w, 13, 1, 0, 0, 0, 0);
        n = dm_rows(&empty, &w, rows, DM_ROWS_MAX);
        CHECK(empty.page == DM_PAGE_LEVELS && find(rows, n, 0) && !strcmp(find(rows, n, 0)->text, "< Back"),
              "with no scene files Back is still there");
        dm_key(&empty, &w, 13, 1, 0, 0, 0, 0);
        CHECK(empty.page == DM_PAGE_MAIN, "and works");
    }
}

static void test_teleport_page(void) {
    dm_state_t s;
    dm_world_t w = world("be.scn", "BE");
    dm_row_t rows[DM_ROWS_MAX];
    dm_action_t a;
    const dm_row_t* r;
    int n;
    jpt_tr_reset();
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_TELEPORT_PAGE;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    CHECK(s.page == DM_PAGE_TELEPORT, "Enter opens Teleport");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(find(rows, n, DM_FIELD_X) && find(rows, n, DM_FIELD_Y) && find(rows, n, DM_FIELD_Z) && find(rows, n, DM_TELEPORT),
          "Teleport keeps X, Y, Z and the action");
    r = find_text(rows, n, "Player");
    CHECK(r && strstr(r->value, "X 12.500") && strstr(r->value, "Yaw"), "and the player's coordinates");
    r = find(rows, n, DM_BACK);
    CHECK(r && !strcmp(r->text, "< Back") && r->enabled, "Teleport has < Back");
    CHECK(!find(rows, n, DM_CLOSE), "and no Close");
    s.cursor = DM_BACK;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN && s.cursor == DM_TELEPORT_PAGE && !dm_take(&s, &a), "Enter on Back returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    s.cursor = DM_BACK;
    dm_pad(&s, &w, 1u << JPT_PAD_SOUTH, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "A on Back returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    dm_key(&s, &w, 27, 1, 0, 0, 0, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "Esc returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    dm_pad(&s, &w, 1u << JPT_PAD_EAST, 0);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "B returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && !s.open, "F10 closes the menu from Teleport");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_TELEPORT_PAGE;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    s.cursor = DM_FIELD_X;
    dm_char(&s, &w, '5');
    CHECK(s.editing, "a coordinate is being typed");
    s.cursor = DM_BACK;
    s.editing = 1;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    CHECK(s.page == DM_PAGE_MAIN && !s.editing, "Back also ends an edit");
}

/* What the panel reports: the rows in a column, the close button in the corner. */
static dml_hits_t hits_for(const dm_state_t* s, const dm_row_t* rows, int n) {
    dml_hits_t h;
    float y = 40.0f;
    memset(&h, 0, sizeof h);
    h.valid = 1;
    h.menu = (int)s->menu;
    h.page = (int)s->page;
    h.has_close = 1;
    h.panel = (dml_rect_t){ 100.0f, 10.0f, 400.0f, 600.0f };
    h.close = (dml_rect_t){ 470.0f, 14.0f, 26.0f, 26.0f };
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == DM_ROW_ITEM && rows[i].enabled && h.count < DML_HITS_MAX) {
            h.hit[h.count].rect = (dml_rect_t){ 100.0f, y, 400.0f, 20.0f };
            h.hit[h.count].item = rows[i].item;
            h.count++;
        }
        y += 20.0f;
    }
    return h;
}

static float row_y(const dm_row_t* rows, int n, int item) {
    float y = 40.0f;
    for (int i = 0; i < n; i++, y += 20.0f)
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == item) return y + 10.0f;
    return -1.0f;
}

static void test_pointer(void) {
    dm_state_t s;
    dm_world_t w = world("be.scn", "BE");
    dm_row_t rows[DM_ROWS_MAX];
    dm_action_t a;
    dml_hits_t h;
    dm_levels_t l = levels();
    int n;
    jpt_tr_reset();
    dm_init(&s);
    dm_set_levels(&s, &l);
    CHECK(!dm_mouse_move(&s, &w, NULL, 10, 10) && !dm_mouse_click(&s, &w, NULL, 10, 10, 0), "a closed menu takes no pointer");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);

    CHECK(dml_hit_find(&h, 300.0f, row_y(rows, n, DM_HEAL)) >= 0 && h.hit[dml_hit_find(&h, 300.0f, row_y(rows, n, DM_HEAL))].item == DM_HEAL,
          "a row's whole width is its target");
    CHECK(dml_hit_find(&h, 90.0f, 100.0f) < 0 && dml_hit_find(&h, 300.0f, 5.0f) < 0, "outside the rows is nothing");

    CHECK(dm_mouse_move(&s, &w, &h, 300.0f, row_y(rows, n, DM_INFINITE_AMMO)) && s.cursor == DM_INFINITE_AMMO,
          "the pointer over a row selects it");
    CHECK(dm_mouse_move(&s, &w, &h, 300.0f, row_y(rows, n, DM_CLOSE)) && s.cursor == DM_CLOSE, "and over Close");
    CHECK(dm_mouse_move(&s, &w, &h, 300.0f, 5.0f) && s.cursor == DM_CLOSE, "over nothing it leaves the selection");

    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_INVULNERABLE), 0) && dm_take(&s, &a) &&
              a.kind == DM_ACT_INVULNERABLE && a.on == 1 && s.cursor == DM_INVULNERABLE && s.open,
          "a click on a toggle runs it");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_MOVE_SPEED), 0) && dm_take(&s, &a) && a.kind == DM_ACT_SPEED &&
              a.value == CH_MULTIPLIER_DEFAULT + 1,
          "a click on Movement Speed steps it like Enter");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_JUMP_HEIGHT), 0) && dm_take(&s, &a) && a.kind == DM_ACT_JUMP,
          "and Jump Height");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_HEAL), 0) && dm_take(&s, &a) && a.kind == DM_ACT_HEAL && s.open,
          "a click on Heal runs it and leaves the menu open");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_FREEZE_AI), 0) && dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI,
          "Freeze Dinosaurs");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_SUPER_HEALTH), 0) && dm_take(&s, &a) &&
              a.kind == DM_ACT_SUPER_HEALTH,
          "Super Health");
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_FREE_CAMERA), 0) && dm_take(&s, &a) && a.kind == DM_ACT_CAMERA,
          "Free Camera");

    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_TELEPORT_PAGE), 0) && s.page == DM_PAGE_TELEPORT && s.open,
          "a click on Teleport opens it");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_BACK), 0) && s.page == DM_PAGE_MAIN && s.open,
          "a click on < Back returns to Cheats");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_TELEPORT_PAGE), 0);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 480.0f, 20.0f, 0) && !s.open && s.page == DM_PAGE_MAIN,
          "the close button closes the whole menu from Teleport");
    dm_toggle(&s, &w, DM_MENU_CHEATS);

    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_LEVEL_SELECT), 0) && s.page == DM_PAGE_LEVELS,
          "a click on Level Select opens it");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, 2), 0) && s.level_cursor == 2 && s.open == 0 && dm_take(&s, &a) &&
              a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, l.scene[2]),
          "a click on a level loads it");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    s.cursor = DM_LEVEL_SELECT;
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, l.count), 0) && s.open && s.page == DM_PAGE_MAIN,
          "a click on < Back in Level Select returns to Cheats");
    dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 480.0f, 20.0f, 0) && !s.open, "the close button closes the whole menu from Level Select");

    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    h = hits_for(&s, rows, n);
    CHECK(dm_mouse_click(&s, &w, &h, 480.0f, 20.0f, 0) && !s.open, "the close button closes the main menu");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_CLOSE), 0) && !s.open, "the Close row closes it");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    CHECK(dm_mouse_click(&s, &w, &h, 700.0f, 300.0f, 0) && s.open && !dm_take(&s, &a), "a click outside the panel does nothing");
    h.page = DM_PAGE_LEVELS;
    CHECK(dm_mouse_click(&s, &w, &h, 300.0f, row_y(rows, n, DM_HEAL), 0) && !dm_take(&s, &a),
          "a hit table of another page is not trusted");
    h.valid = 0;
    CHECK(dm_mouse_click(&s, &w, &h, 480.0f, 20.0f, 0) && s.open, "nor one that was not drawn");

    {
        dm_world_t off = world("be.scn", "BE");
        dm_state_t t;
        off.hp = 0.0f;
        dm_init(&t);
        dm_toggle(&t, &off, DM_MENU_CHEATS);
        n = dm_rows(&t, &off, rows, DM_ROWS_MAX);
        h = hits_for(&t, rows, n);
        CHECK(dml_hit_find(&h, 300.0f, row_y(rows, n, DM_HEAL)) < 0, "a disabled row is no target");
    }
}

static void test_close_everywhere(void) {
    dm_state_t s;
    dm_world_t w = world("be.scn", "BE");
    dm_levels_t l = levels();
    static const dm_item_t pages[] = { DM_LEVEL_SELECT, DM_TELEPORT_PAGE };
    jpt_tr_reset();
    dm_init(&s);
    dm_set_levels(&s, &l);
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && dm_toggle(&s, &w, DM_MENU_CHEATS) && !s.open, "F10 closes the main menu");
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        dm_toggle(&s, &w, DM_MENU_CHEATS);
        s.cursor = pages[i];
        dm_key(&s, &w, 13, 1, 0, 0, 0, 0);
        CHECK(s.page != DM_PAGE_MAIN, "page %d opened", (int)i);
        CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && !s.open && s.page == DM_PAGE_MAIN, "F10 closes page %d and resets it", (int)i);
        CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && s.page == DM_PAGE_MAIN, "the next opening starts on the main page");
        dm_close(&s);
    }
}

static char* read_source(const char* name) {
    char path[512];
    FILE* f;
    long n;
    char* text = NULL;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, name);
    if (!(f = fopen(path, "rb"))) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) text[n] = 0;
    fclose(f);
    return text;
}

/* A change shows in its own row: the menu gets no line repeating it, and the Cheats panel no key instructions. */
static void test_no_echo(void) {
    char *cheats = read_source("src/host/cheats.c"), *menu = read_source("src/host/debug_menu.c");
    const char* changed = cheats ? strstr(cheats, "static const char* changed(") : NULL;
    const char* end = changed ? strstr(changed, "\n}") : NULL;
    CHECK(changed && end, "cheats.c has its change logger");
    if (changed && end) {
        char body[512];
        size_t len = (size_t)(end - changed) < sizeof body - 1 ? (size_t)(end - changed) : sizeof body - 1;
        memcpy(body, changed, len);
        body[len] = 0;
        CHECK(strstr(body, "host_log") && strstr(body, "return NULL;") && !strstr(body, "return C.text"),
              "a cheat that changed is logged, not echoed under its row");
    }
    CHECK(menu && !strstr(menu, "Teleported to") && !strstr(menu, "F10 close") && !strstr(menu, "Free Camera ("),
          "no teleport echo, no key instructions, no Free Camera instructions in the menu");
    CHECK(menu && strstr(menu, "if (D.state.menu != DM_MENU_CHEATS)"), "the key help is the Debug Menu's alone");
    free(cheats);
    free(menu);
}

void menu_layout_tests(int* checks, int* failures) {
    g_checks = checks;
    g_failures = failures;
    test_panel_layout();
    test_widths();
    test_columns();
    test_level_names();
    test_main_menu_rows();
    test_heal_row();
    test_level_select_page();
    test_teleport_page();
    test_pointer();
    test_close_everywhere();
    test_no_echo();
}
