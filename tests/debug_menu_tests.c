/* Headless checks of the F10 Cheats and F3 Objectives menus: their policy (input, items, teleport, level select), the
 * retail sites they reuse (game/trespass.exe 1.0.116) and their panel, compass included, on a WARP device. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "test_profile.h"

#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats_policy.h"
#include "debug_menu_panel.h"
#include "debug_menu_policy.h"
#include "jpt_config.h"
#include "jpt_gamepad.h"

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

#define DOWN 1
#define UP 0
#define PAD(b) JPT_PAD_BIT(JPT_PAD_##b)

static dm_world_t level_world(const char* scene) {
    dm_world_t w;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_LEVEL;
    snprintf(w.scene, sizeof w.scene, "%s", scene);
    dm_level_id(w.scene, w.level, sizeof w.level);
    w.player = 1;
    w.at.pos[0] = 10.0f;
    w.at.pos[1] = 20.0f;
    w.at.pos[2] = 30.0f;
    w.at.rot[0] = 1.0f;
    w.ai = 0;
    w.hp = 80.0f;
    w.hp_max = 100.0f;
    w.camera_head = 1;
    w.speed = w.jump = CH_MULTIPLIER_DEFAULT;
    return w;
}

static dm_world_t main_menu_world(void) {
    dm_world_t w;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_MAIN_MENU;
    w.ai = w.invulnerable = w.ammo = -1;
    w.speed = w.jump = CH_MULTIPLIER_DEFAULT;
    return w;
}

static int key(dm_state_t* s, const dm_world_t* w, unsigned vk, int down) {
    return dm_key(s, w, vk, down, 0, 0, 0, 1000);
}

static void press(dm_state_t* s, const dm_world_t* w, unsigned vk) {
    key(s, w, vk, DOWN);
    key(s, w, vk, UP);
}

static void go_to(dm_state_t* s, const dm_world_t* w, dm_item_t item) {
    for (int i = 0; i < DM_ITEM_COUNT * 2 && s->cursor != (int)item; i++) press(s, w, VK_DOWN);
}

static int open_cheats(dm_state_t* s, const dm_world_t* w) { return dm_toggle(s, w, DM_MENU_CHEATS); }

static void open_teleport(dm_state_t* s, const dm_world_t* w) {
    open_cheats(s, w);
    go_to(s, w, DM_TELEPORT_PAGE);
    press(s, w, VK_RETURN);
}

static void test_open_close(void) {
    dm_state_t s;
    dm_world_t none, menu = main_menu_world(), level = level_world("be.scn");
    memset(&none, 0, sizeof none);
    dm_init(&s);
    CHECK(!open_cheats(&s, &none) && !s.open, "F10 with no level and no main menu does nothing");
    CHECK(open_cheats(&s, &menu) && s.open, "F10 in the main menu opens");
    CHECK(s.cursor == DM_LEVEL_SELECT, "the main menu starts on Level Select");
    CHECK(open_cheats(&s, &menu) && !s.open, "F10 again closes");
    CHECK(open_cheats(&s, &level) && s.open && s.cursor == DM_MOVE_SPEED, "F10 in a level opens on Movement Speed");
    CHECK(s.field[0] == 10.0f && s.field[1] == 20.0f && s.field[2] == 30.0f, "the fields start at the player");
    CHECK(open_cheats(&s, &level) && !s.open, "F10 closes in a level");
    level.player = 0;
    CHECK(open_cheats(&s, &level) && s.cursor != DM_MOVE_SPEED && dm_item_enabled(&s, &level, (dm_item_t)s.cursor) &&
              s.fill_fields,
          "no player: the first enabled item, the fields wait");
    level.player = 1;
    dm_world_seen(&s, &level);
    CHECK(!s.fill_fields && s.field[2] == 30.0f, "the fields fill once the player is read");
}

static void test_input_capture(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_init(&s);
    CHECK(!key(&s, &w, 'W', DOWN), "a closed menu takes no key");
    open_cheats(&s, &w);
    CHECK(!key(&s, &w, 'W', UP), "the key-up of a key the game saw goes to the game");
    CHECK(key(&s, &w, 'W', DOWN) && key(&s, &w, VK_F1, DOWN) && key(&s, &w, VK_SPACE, DOWN), "an open menu takes keys");
    CHECK(key(&s, &w, 'W', UP) && key(&s, &w, VK_F1, UP) && key(&s, &w, VK_SPACE, UP), "and their key-ups");
    CHECK(dm_char(&s, &w, 'q') && dm_char(&s, &w, '\r'), "and characters");
    CHECK(key(&s, &w, VK_ESCAPE, DOWN) && !s.open, "Esc closes the menu first");
    CHECK(dm_char(&s, &w, 0x1B), "the Esc character after closing is the menu's");
    CHECK(dm_key(&s, &w, VK_ESCAPE, DOWN, 1, 0, 0, 0), "so are its repeats");
    CHECK(key(&s, &w, VK_ESCAPE, UP), "and its key-up: no window of the game sees a lone Esc key-up");
    CHECK(!key(&s, &w, VK_ESCAPE, DOWN) && !key(&s, &w, VK_ESCAPE, UP), "the next Esc is the game's again");
    CHECK(!dm_char(&s, &w, 'a'), "characters are the game's again");
    open_cheats(&s, &w);
    key(&s, &w, VK_RETURN, DOWN);
    open_cheats(&s, &w);
    CHECK(!dm_key(&s, &w, VK_RETURN, DOWN, 0, 0, 0, 0), "a fresh press after a lost key-up is the game's");
    CHECK(!key(&s, &w, VK_RETURN, UP), "with its key-up");
}

static void test_navigation_and_pad(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_action_t a;
    dm_init(&s);
    open_cheats(&s, &w);
    press(&s, &w, VK_UP);
    CHECK(s.cursor == DM_CLOSE, "Up from the first item wraps to Close");
    press(&s, &w, VK_DOWN);
    CHECK(s.cursor == DM_MOVE_SPEED, "Down wraps back to the first item");
    w.player = 0;
    dm_world_seen(&s, &w);
    CHECK(s.cursor != DM_MOVE_SPEED && dm_item_enabled(&s, &w, (dm_item_t)s.cursor),
          "a disabled item gives the cursor to an enabled one");
    for (int i = 0; i < 6; i++) press(&s, &w, VK_DOWN);
    CHECK(dm_item_enabled(&s, &w, (dm_item_t)s.cursor), "Down skips disabled items (%d)", s.cursor);
    w.player = 1;
    dm_pad(&s, &w, PAD(DPAD_DOWN), 0);
    dm_pad(&s, &w, PAD(EAST), 0);
    CHECK(!s.open, "B closes the main page");
    open_cheats(&s, &w);
    dm_pad(&s, &w, PAD(SOUTH), 0);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SPEED, "A uses the item");
    CHECK(!dm_take(&s, &a), "an action is taken once");
    dm_pad(&s, &w, PAD(START), 0);
    CHECK(!s.open, "Start closes");
    {
        dm_repeat_t r = { 0, 0 };
        CHECK(!dm_pad_repeat(&r, PAD(DPAD_DOWN), PAD(DPAD_DOWN), 0), "a press is not a repeat");
        CHECK(!dm_pad_repeat(&r, PAD(DPAD_DOWN), 0, 399), "no repeat before 400 ms");
        CHECK(dm_pad_repeat(&r, PAD(DPAD_DOWN), 0, 400) == PAD(DPAD_DOWN), "a repeat at 400 ms");
        CHECK(!dm_pad_repeat(&r, PAD(DPAD_DOWN), 0, 470) && dm_pad_repeat(&r, PAD(DPAD_DOWN), 0, 480), "then every 80");
        CHECK(!dm_pad_repeat(&r, 0, 0, 2000) && !dm_pad_repeat(&r, PAD(DPAD_DOWN), 0, 3000), "released: no repeat");
        CHECK(!dm_pad_repeat(&r, PAD(SOUTH), PAD(SOUTH), 0) && !dm_pad_repeat(&r, PAD(SOUTH), 0, 9000),
              "only the D-pad repeats");
    }
}

static void test_levels(void) {
    dm_levels_t l;
    dm_state_t s;
    dm_world_t menu = main_menu_world(), w = level_world("it.scn");
    dm_action_t a;
    char id[DM_LEVEL_ID_MAX];
    static const char* const bad[] = { "", "be", ".scn", "be.scx", "..\\be.scn", "a b.scn", "c:be.scn",
                                       "averyveryverylongname.scn", "be.scn.bak" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!dm_scene_valid(bad[i]), "\"%s\" is no scene", bad[i]);
    CHECK(dm_scene_valid("BE.SCN") && dm_scene_valid("as2.scn") && dm_scene_valid("lab.scn"), "scene names");
    CHECK(dm_level_id("be.scn", id, sizeof id) && !strcmp(id, "BE"), "be.scn is BE");
    CHECK(dm_level_id("As2.Scn", id, sizeof id) && !strcmp(id, "AS2"), "As2.Scn is AS2");
    CHECK(!dm_level_id("menu.tpa", id, sizeof id) && !id[0], "menu.tpa is no level");
    memset(&l, 0, sizeof l);
    CHECK(dm_levels_add(&l, "sum.scn") && dm_levels_add(&l, "BE.SCN") && dm_levels_add(&l, "it.scn"), "levels add");
    CHECK(!dm_levels_add(&l, "be.scn") && !dm_levels_add(&l, "readme.txt"), "a name once, scenes only");
    CHECK(l.count == 3 && !strcmp(l.scene[0], "BE.SCN") && !strcmp(l.scene[1], "it.scn") &&
              !strcmp(l.scene[2], "sum.scn"),
          "the campaign in play order");
    dm_init(&s);
    dm_set_levels(&s, &l);
    open_cheats(&s, &menu);
    press(&s, &menu, VK_RETURN);
    CHECK(s.page == DM_PAGE_LEVELS && s.level_cursor == 0, "Level Select lists the levels");
    press(&s, &menu, VK_ESCAPE);
    CHECK(s.open && s.page == DM_PAGE_MAIN, "Esc goes back to the main page");
    press(&s, &menu, VK_RETURN);
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_RETURN);
    CHECK(!s.open && dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "sum.scn"),
          "a level is loaded by its file name and the menu closes");
    open_cheats(&s, &w);
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    CHECK(s.level_cursor == 1, "in a level the list starts on the current one");
    CHECK(dm_item_enabled(&s, &menu, DM_LEVEL_SELECT) && dm_item_enabled(&s, &w, DM_LEVEL_SELECT), "Level Select");
    menu.place = DM_PLACE_NONE;
    CHECK(!dm_item_enabled(&s, &menu, DM_LEVEL_SELECT), "nowhere to load from: disabled");
}

static void test_level_names(void) {
    static const struct {
        const char* file;
        const char* title;
    } retail[] = {
        { "be.scn", "BE - Beach" },
        { "jr.scn", "JR - Jungle Road" },
        { "ij.scn", "IJ - Industrial Jungle" },
        { "IT.scn", "IT - InGen Town" },
        { "lab.scn", "LAB - InGen Lab" },
        { "as.scn", "AS - Ascent, Part 1" },
        { "as2.scn", "AS2 - Ascent, Part 2" },
        { "sum.scn", "SUM - Summit" },
        { "TestScene.scn", "TESTSCENE - Test Scene" },
        { "TestScnNght.scn", "TESTSCNNGHT - Test Scene Night" },
    };
    static const char* const shuffled[] = { "TestScnNght.scn", "sum.scn", "zz_custom.scn", "lab.scn", "be.scn",
                                            "as2.scn", "TestScene.scn", "IT.scn", "alpha.scn", "ij.scn",
                                            "as.scn", "jr.scn" };
    dm_levels_t l;
    dm_state_t s;
    dm_world_t w = level_world("IT.scn"), menu = main_menu_world();
    dm_row_t rows[DM_ROWS_MAX];
    dm_action_t a;
    char title[DM_TEXT_MAX];
    int n, rows_seen = 0;
    for (size_t i = 0; i < sizeof retail / sizeof retail[0]; i++)
        CHECK(dm_level_title(retail[i].file, title, sizeof title) && !strcmp(title, retail[i].title),
              "%s is shown as \"%s\" (got \"%s\")", retail[i].file, retail[i].title, title);
    CHECK(!strcmp(dm_level_name("be.scn"), "Beach") && !dm_level_name("custom.scn") && !dm_level_name("be.txt"),
          "a name for retail levels only");
    CHECK(dm_level_title("MyLevel.scn", title, sizeof title) && !strcmp(title, "MYLEVEL"),
          "an unknown scene keeps its id");
    CHECK(!dm_level_title("menu.tpa", title, sizeof title), "no title for a file that is no scene");
    memset(&l, 0, sizeof l);
    for (size_t i = 0; i < sizeof shuffled / sizeof shuffled[0]; i++) dm_levels_add(&l, shuffled[i]);
    CHECK(l.count == 12, "every scene is listed (%d)", l.count);
    for (size_t i = 0; i < sizeof retail / sizeof retail[0]; i++)
        CHECK(i < (size_t)l.count && !_stricmp(l.scene[i], retail[i].file), "slot %d is %s (got %s)", (int)i,
              retail[i].file, i < (size_t)l.count ? l.scene[i] : "-");
    CHECK(!strcmp(l.scene[10], "alpha.scn") && !strcmp(l.scene[11], "zz_custom.scn"),
          "other scenes follow the test scenes, by name");
    CHECK(!dm_levels_add(&l, "BE.SCN") && !dm_levels_add(&l, "ALPHA.scn"), "a scene is listed once, in any case");
    dm_init(&s);
    dm_set_levels(&s, &l);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    CHECK(s.page == DM_PAGE_LEVELS && s.level_cursor == 3, "Level Select starts on the current level");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind != DM_ROW_ITEM || rows[i].item == l.count) continue;
        CHECK(rows[i].item == rows_seen && !strcmp(rows[i].text, rows_seen < 10 ? strstr(retail[rows_seen].title, " - ") + 3
                                                                                : rows_seen == 10 ? "ALPHA"
                                                                                                  : "ZZ_CUSTOM"),
              "row %d: \"%s\"", rows_seen, rows[i].text);
        CHECK(!rows[i].value[0], "row %d has no second column: \"%s\"", rows_seen, rows[i].value);
        rows_seen++;
    }
    CHECK(rows_seen == 12, "all twelve rows");
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "lab.scn"),
          "choosing \"LAB - InGen Lab\" sends lab.scn, the file name the backend loads");
    dm_toggle(&s, &menu, DM_MENU_CHEATS);
    press(&s, &menu, VK_RETURN);
    n = dm_rows(&s, &menu, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++)
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == 0)
            CHECK(!strcmp(rows[i].text, "Beach") && !rows[i].value[0], "from the main menu the list reads the same");
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "ij.scn"),
          "the main menu sends the same file");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    for (int i = 0; i < 7; i++) press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "alpha.scn"),
          "a custom scene is loaded by its own file name");
}

static void test_teleport(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_action_t a;
    float nan_pos[3] = { 0.0f, (float)NAN, 0.0f }, far_pos[3] = { 0.0f, 0.0f, 200000.0f };
    float ok_pos[3] = { -5.5f, 1e4f, 0.0f };
    CHECK(!dm_teleport_valid(nan_pos) && !dm_teleport_valid(far_pos) && dm_teleport_valid(ok_pos), "teleport targets");
    dm_init(&s);
    open_teleport(&s, &w);
    CHECK(s.page == DM_PAGE_TELEPORT && s.cursor == DM_FIELD_X, "Teleport opens its page on X");
    dm_char(&s, &w, '-');
    dm_char(&s, &w, '1');
    dm_char(&s, &w, '2');
    dm_char(&s, &w, 'x');
    dm_char(&s, &w, '.');
    dm_char(&s, &w, '5');
    CHECK(s.editing && !strcmp(s.edit, "-12.5"), "digits, sign and point edit a field (%s)", s.edit);
    press(&s, &w, VK_BACK);
    CHECK(!strcmp(s.edit, "-12."), "Backspace");
    press(&s, &w, VK_RETURN);
    CHECK(!s.editing && s.field[0] == -12.0f, "Enter commits");
    dm_char(&s, &w, '9');
    press(&s, &w, VK_ESCAPE);
    CHECK(s.open && !s.editing && s.field[0] == -12.0f, "Esc cancels an edit before closing anything");
    dm_char(&s, &w, '7');
    press(&s, &w, VK_DOWN);
    CHECK(s.field[0] == 7.0f && s.cursor == DM_FIELD_Y, "moving away commits");
    press(&s, &w, VK_RIGHT);
    CHECK(s.field[1] == 21.0f, "Right adds 1");
    dm_key(&s, &w, VK_LEFT, DOWN, 0, 1, 0, 0);
    CHECK(s.field[1] == 11.0f, "Shift+Left takes 10");
    dm_key(&s, &w, VK_RIGHT, DOWN, 0, 0, 1, 0);
    CHECK(fabsf(s.field[1] - 11.1f) < 1e-4f, "Ctrl+Right adds 0.1");
    dm_pad(&s, &w, PAD(RIGHT_SHOULDER), 0);
    dm_pad(&s, &w, PAD(DPAD_LEFT), 0);
    CHECK(fabsf(s.field[1] - 20.1f) < 1e-4f, "RB adds 10, D-pad left takes 1");
    dm_char(&s, &w, '1');
    dm_char(&s, &w, '-');
    dm_char(&s, &w, '9');
    press(&s, &w, VK_RETURN);
    CHECK(fabsf(s.field[1] - 20.1f) < 1e-4f, "a value that is not a number leaves the field");
    go_to(&s, &w, DM_TELEPORT);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_TELEPORT && a.to.pos[0] == 7.0f && fabsf(a.to.pos[1] - 20.1f) < 1e-4f &&
              a.to.pos[2] == 30.0f && s.open,
          "Teleport sends the fields and stays open");
    w.player = 0;
    CHECK(!dm_item_enabled(&s, &w, DM_TELEPORT) && !dm_item_enabled(&s, &w, DM_FIELD_X), "no player: no teleport");
}

/* 0x004CD090, vector by CRotate3 (LOC's Facing line), term by term as its x87 code computes it. */
static void retail_rotate(const float q[4], const float v[3], float out[3]) {
    float a = q[0], b = q[1], c = q[2], d = q[3], k = a * a - 0.5f;
    out[0] = 2.0f * ((b * b + k) * v[0] + (c * b - d * a) * v[1] + (c * a + d * b) * v[2]);
    out[1] = 2.0f * ((d * a + b * c) * v[0] + (c * c + k) * v[1] + (d * c - b * a) * v[2]);
    out[2] = 2.0f * ((d * b - c * a) * v[0] + (b * a + c * d) * v[1] + (d * d + k) * v[2]);
}

static void test_yaw(void) {
    static const float forward[3] = { 0.0f, 1.0f, 0.0f };
    int worst = 0;
    srand(7);
    for (int i = 0; i < 2000; i++) {
        float q[4], f[3], n = 0.0f, yaw, want;
        for (int j = 0; j < 4; j++) {
            q[j] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
            n += q[j] * q[j];
        }
        n = sqrtf(n);
        for (int j = 0; j < 4; j++) q[j] /= n;
        retail_rotate(q, forward, f);
        if (f[0] * f[0] + f[1] * f[1] < 1e-4f) continue;
        want = atan2f(-f[0], f[1]);
        yaw = dm_yaw(q);
        if (fabsf(remainderf(yaw - want, 6.2831853f)) > 1e-4f) worst++;
    }
    CHECK(!worst, "yaw is the heading of LOC's facing vector (%d misses)", worst);
    {
        float q[4] = { (float)cos(0.25), 0.0f, 0.0f, (float)sin(0.25) };
        CHECK(fabsf(dm_yaw(q) - 0.5f) < 1e-5f, "a turn of 0.5 rad about Z is yaw 0.5 (counter-clockwise)");
    }
}

static void test_toggles(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_init(&s);
    open_cheats(&s, &w);
    go_to(&s, &w, DM_FREEZE_AI);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI && a.on == 1, "Freeze AI on");
    w.ai = 1;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI && a.on == 0, "Freeze AI off follows the game's flag");
    w.ai = -1;
    CHECK(!dm_item_enabled(&s, &w, DM_FREEZE_AI) && !dm_item_enabled(&s, &menu, DM_FREEZE_AI),
          "no AI system: disabled");
}

static void test_stale_state(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), none;
    dm_row_t rows[DM_ROWS_MAX];
    int n, disabled_teleport = 0, titled = 0;
    memset(&none, 0, sizeof none);
    dm_init(&s);
    open_teleport(&s, &w);
    go_to(&s, &w, DM_FIELD_Y);
    dm_char(&s, &w, '5');
    dm_world_seen(&s, &none);
    CHECK(!s.open && !s.editing && s.page == DM_PAGE_MAIN, "a load or a dialog in front closes the menu");
    w.player = 0;
    open_cheats(&s, &w);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == DM_TELEPORT_PAGE && !rows[i].enabled) disabled_teleport = 1;
        if (rows[i].kind == DM_ROW_TITLE && !strcmp(rows[i].text, "Cheats")) titled = 1;
    }
    CHECK(titled && disabled_teleport && n > 8 && n < DM_ROWS_MAX, "rows: title, disabled items (%d rows)", n);
}

static const dm_row_t* row_of(const dm_row_t* rows, int n, int item) {
    for (int i = 0; i < n; i++)
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == item) return &rows[i];
    return NULL;
}

static int row_says(const dm_row_t* rows, int n, int item, const char* value) {
    const dm_row_t* r = row_of(rows, n, item);
    return r && !strcmp(r->value, value);
}

static void test_cheats_menu(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), dead, menu = main_menu_world(), none;
    dm_action_t a;
    dm_row_t rows[DM_ROWS_MAX];
    const dm_row_t* r;
    int n;
    memset(&none, 0, sizeof none);
    dm_init(&s);
    CHECK(!dm_toggle(&s, &none, DM_MENU_CHEATS) && !s.open, "F10 needs a level or the main menu");
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && s.open && s.menu == DM_MENU_CHEATS && s.cursor == DM_MOVE_SPEED,
          "F10 opens Cheats on Movement Speed");
    CHECK(dm_toggle(&s, &w, DM_MENU_OBJECTIVES) && s.open && s.menu == DM_MENU_OBJECTIVES && s.page == DM_PAGE_MAIN,
          "F3 over Cheats replaces it: one menu at a time");
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && s.open && s.menu == DM_MENU_CHEATS, "F10 over Objectives replaces it");
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && !s.open, "F10 again closes");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    CHECK(key(&s, &w, 'W', DOWN) && key(&s, &w, 'W', UP), "Cheats takes the keys");
    press(&s, &w, VK_ESCAPE);
    CHECK(!s.open, "Esc closes Cheats first");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    press(&s, &w, VK_RIGHT);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SPEED && a.value == CH_MULTIPLIER_DEFAULT + 1, "Right: faster");
    w.speed = 0;
    press(&s, &w, VK_LEFT);
    CHECK(!dm_take(&s, &a), "no slower than 0.5x");
    w.speed = CH_SPEED_COUNT - 1;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SPEED && a.value == 0, "Enter cycles round from 10.0x to 0.5x");
    press(&s, &w, VK_RIGHT);
    CHECK(!dm_take(&s, &a), "Right stops at 10.0x");
    for (int i = 0; i < CH_SPEED_COUNT; i++) {
        w.speed = i;
        press(&s, &w, VK_RETURN);
        CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SPEED && a.value == (i + 1) % CH_SPEED_COUNT, "Enter steps %d", i);
    }
    w.speed = CH_MULTIPLIER_DEFAULT;
    press(&s, &w, VK_DOWN);
    dm_pad(&s, &w, PAD(DPAD_RIGHT), 0);
    CHECK(s.cursor == DM_JUMP_HEIGHT && dm_take(&s, &a) && a.kind == DM_ACT_JUMP && a.value == 2, "Jump Height");
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_INVULNERABLE && a.on == 1, "Invulnerability on");
    w.invulnerable = 1;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_INVULNERABLE && a.on == 0, "and off: the game's flag decides");
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_AMMO && a.on == 1, "Infinite Ammo (WOO)");
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(s.cursor == DM_FREEZE_AI && dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI && a.on == 1,
          "Freeze Dinosaurs asks for the Freeze AI action");
    w.ai = 1;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = row_of(rows, n, DM_FREEZE_AI);
    CHECK(r && !strcmp(r->text, "Freeze Dinosaurs") && !strcmp(r->value, "On"), "Cheats shows the DINOS flag");
    go_to(&s, &w, DM_HEAL);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_HEAL, "Heal");
    dead = w;
    dead.hp = -5.0f;
    CHECK(!dm_item_enabled(&s, &dead, DM_HEAL) && !dm_item_enabled(&s, &dead, DM_SUPER_HEALTH),
          "nothing heals the dead");
    go_to(&s, &w, DM_SUPER_HEALTH);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SUPER_HEALTH && a.on == 1, "Super Health on");
    w.super_health = 1;
    dead.super_health = 1;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_SUPER_HEALTH && a.on == 0 &&
              dm_item_enabled(&s, &dead, DM_SUPER_HEALTH),
          "and off, even after death");
    w.super_health = 0;
    go_to(&s, &w, DM_TELEPORT_PAGE);
    press(&s, &w, VK_RETURN);
    CHECK(s.page == DM_PAGE_TELEPORT && s.cursor == DM_FIELD_X && s.field[1] == 20.0f, "Teleport: the same fields");
    dm_char(&s, &w, '5');
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_TELEPORT && a.to.pos[0] == 5.0f && a.to.pos[2] == 30.0f,
          "and the Teleport action");
    press(&s, &w, VK_ESCAPE);
    CHECK(s.open && s.page == DM_PAGE_MAIN && s.cursor == DM_TELEPORT_PAGE, "Esc goes back to Cheats");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    CHECK(s.field[0] == 10.0f, "each opening starts the fields at the player");
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    CHECK(s.page == DM_PAGE_LEVELS, "Level Select: the same list");
    press(&s, &w, VK_ESCAPE);
    CHECK(s.page == DM_PAGE_MAIN && s.cursor == DM_LEVEL_SELECT, "and back");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++)
        CHECK(strcmp(rows[i].text, "Show Coordinates") && strcmp(rows[i].text, "Third Person") &&
                  strcmp(rows[i].text, "Debug / Test Menu"),
              "no Show Coordinates, Third Person or Debug row: %s", rows[i].text);
    go_to(&s, &w, DM_FREE_CAMERA);
    CHECK(s.cursor == DM_FREE_CAMERA, "Fly / Free Camera is in the list");
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_CAMERA && a.value == CH_CAMERA_FREE, "Free Camera on");
    w.camera = CH_CAMERA_FREE;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(row_says(rows, n, DM_FREE_CAMERA, "On"), "Free Camera shows On");
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_CAMERA && a.value == CH_CAMERA_NORMAL, "Free Camera off");
    w.camera_head = 0;
    w.camera = CH_CAMERA_NORMAL;
    CHECK(!dm_item_enabled(&s, &w, DM_FREE_CAMERA), "no camera on Anne: no free camera");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(rows[0].kind == DM_ROW_TITLE && !strcmp(rows[0].text, "Cheats") && row_says(rows, n, DM_MOVE_SPEED, "1.0x") &&
              row_says(rows, n, DM_TELEPORT_PAGE, ">") && row_of(rows, n, DM_CLOSE),
          "Cheats rows");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    dm_toggle(&s, &menu, DM_MENU_CHEATS);
    CHECK(s.open && s.cursor == DM_LEVEL_SELECT, "in the main menu Cheats offers Level Select");
    for (int i = 0; i < DM_ITEM_COUNT; i++)
        if (i != DM_LEVEL_SELECT && i != DM_BACK && i != DM_CLOSE)
            CHECK(!dm_item_enabled(&s, &menu, (dm_item_t)i), "no level: %s disabled",
                  dm_item_label(DM_MENU_CHEATS, (dm_item_t)i));
}

/* ---- retail sites (game/trespass.exe 1.0.116) ---- */

typedef struct {
    uint32_t va, size, raw;
} section_t;

static uint8_t* g_exe;
static size_t g_exe_size;
static section_t g_sections[16];
static int g_section_count;

static const uint8_t* exe_at(uint32_t va, uint32_t bytes) {
    for (int i = 0; i < g_section_count; i++) {
        uint32_t rva = va - 0x00400000u;
        if (rva >= g_sections[i].va && rva + bytes <= g_sections[i].va + g_sections[i].size &&
            g_sections[i].raw + (rva - g_sections[i].va) + bytes <= g_exe_size)
            return g_exe + g_sections[i].raw + (rva - g_sections[i].va);
    }
    return NULL;
}

static int load_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    uint32_t pe;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    g_exe_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_exe = (uint8_t*)malloc(g_exe_size);
    if (!g_exe || fread(g_exe, 1, g_exe_size, f) != g_exe_size || g_exe_size < 0x400) {
        fclose(f);
        return 0;
    }
    fclose(f);
    pe = *(const uint32_t*)(g_exe + 0x3C);
    for (int i = 0; i < *(const uint16_t*)(g_exe + pe + 6) && i < 16; i++) {
        const uint8_t* s = g_exe + pe + 24 + *(const uint16_t*)(g_exe + pe + 20) + i * 40;
        g_sections[i].va = *(const uint32_t*)(s + 12);
        g_sections[i].size = *(const uint32_t*)(s + 16);
        g_sections[i].raw = *(const uint32_t*)(s + 20);
        g_section_count = i + 1;
    }
    return 1;
}

static int bytes_at(uint32_t va, const char* hex) {
    size_t n = strlen(hex) / 2;
    const uint8_t* p = exe_at(va, (uint32_t)n);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1 || p[i] != b) return 0;
    }
    return 1;
}

static int calls(uint32_t va, uint32_t target) {
    const uint8_t* p = exe_at(va, 5);
    return p && p[0] == 0xE8 && va + 5 + *(const int32_t*)(p + 1) == target;
}

static uint32_t u32_at(uint32_t va) {
    const uint8_t* p = exe_at(va, 4);
    return p ? *(const uint32_t*)p : 0;
}

static int string_at(uint32_t va, const char* text) {
    const uint8_t* p = exe_at(va, (uint32_t)strlen(text) + 1);
    return p && !strcmp((const char*)p, text);
}

static char* read_text(const char* root, const char* relative) {
    char path[MAX_PATH];
    FILE* f;
    long n;
    char* text;
    snprintf(path, sizeof path, "%s/%s", root, relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)n + 1, 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

/* The text of a source function: from its signature to the next line that starts at column 0 with a brace. */
static int function_has(const char* text, const char* signature, const char* needle) {
    const char* start = strstr(text, signature);
    const char* end;
    char* body;
    int found;
    if (!start) return 0;
    end = strstr(start, "\n}");
    if (!end) return 0;
    body = (char*)calloc((size_t)(end - start) + 1, 1);
    memcpy(body, start, (size_t)(end - start));
    found = strstr(body, needle) != NULL;
    free(body);
    return found;
}

/* Stealth Mode: every sighting of an object goes through CInfluenceList::See -> Add, whose callers are all inside the two
 * functions the host already owns; the wrappers only mark Anne's AI infos and call the retail body. */
static void test_perception(const char* root) {
    char* ai = read_text(root, "src/host/active_ai.c");
    char* cheats = read_text(root, "src/host/cheats.c");
    char* list = read_text(root, "src/host/guest_override_list.h");
    char* fixes = read_text(root, "src/host/physics_fixes.c");
    CHECK(bytes_at(0x00424AB0u, "56") && bytes_at(0x00424B1Cu, "E80F000000") && calls(0x00424B1Cu, 0x00424B30u),
          "CInfluenceList::See hands an object to Add (0x00424B30)");
    CHECK(calls(0x004215BFu, 0x00424AB0u) && calls(0x00421788u, 0x00424AB0u) && calls(0x00421803u, 0x00424AB0u) &&
              calls(0x00419D93u, 0x00424AB0u),
          "See is called from CBrain::Influences (the world query, Anne's parts, the other animals' parts) and SeenByNearest");
    CHECK(bytes_at(0x004213F0u, "83EC58") && bytes_at(0x00419CE0u, "83EC18"),
          "0x004213F0 (Influences) and 0x00419CE0 (SeenByNearest) are the entries that hold every one of those calls");
    CHECK(bytes_at(0x00424B3Fu, "8B4528568B7010F6460C0257"),
          "Add: object + 0x28 (info), info + 0x10 (CAIInfo), flags + 0x0C bit 2: the object never enters the influences");
    CHECK(bytes_at(0x0042176Au, "BEAC000000") && bytes_at(0x0042177Bu, "A164EF6600") && bytes_at(0x00421790u, "81FECC0000007CD7"),
          "Influences sights gpPlayer's eight parts at + 0x0AC..+0x0CC");
    CHECK(bytes_at(0x00424AD4u, "8B56288B421085C074438B4808") && bytes_at(0x00424AFDu, "8B471033C905AC000000"),
          "See reads the same CAIInfo and skips an animal's own parts (+ 0x0AC)");
    CHECK(bytes_at(0x0060A469u, "8DBEAC000000") && bytes_at(0x0060A474u, "C744242C08000000"),
          "CAnimate moves eight parts at + 0x0AC: the same array");
    CHECK(ai && list, "the sources are readable");
    if (ai && list) {
        CHECK(strstr(list, "GUEST_OVERRIDE(004213F0, REQUIRED)") && strstr(list, "GUEST_OVERRIDE(00419CE0, REQUIRED)"),
              "both perception entries have a stable entry in every lift");
        CHECK(function_has(ai, "static void stealth_influences(void) {", "A.influences();") &&
                  function_has(ai, "static void stealth_influences(void) {", "cheats_stealth_begin") &&
                  function_has(ai, "static void stealth_seen_by_nearest(void) {", "A.seen();"),
              "the Original wrappers run the retail bodies: the AI keeps updating");
        CHECK(function_has(ai, "static void ai_influences(void) {", "cheats_stealth_begin") &&
                  function_has(ai, "static void ai_seen_by_nearest(void) {", "cheats_stealth_end"),
              "the Enhanced entries hide Anne the same way");
        CHECK(function_has(ai, "void active_ai_install(void) {", "stealth_install();"),
              "Stealth Mode works without the Active AI hooks");
    }
    if (cheats) {
        CHECK(!function_has(cheats, "void cheats_stealth_begin(", "AI_SYSTEM") &&
                  !function_has(cheats, "void cheats_stealth_begin(", "AI_FLAGS") &&
                  !function_has(cheats, "void cheats_stealth_begin(", "freeze") &&
                  !function_has(cheats, "static const char* run_stealth(", "cheats_freeze_ai") &&
                  !function_has(cheats, "static const char* run_stealth(", "AI_BORING"),
              "Stealth Mode never touches CAISystem's flags: Freeze Dinosaurs is not involved");
        CHECK(function_has(cheats, "void cheats_stealth_begin(", "if (!C.want.stealth") &&
                  function_has(cheats, "void cheats_stealth_end(", "original"),
              "off: nothing is marked; on: every flag written is put back");
        CHECK(function_has(cheats, "static const char* run_stealth(", "remember(JPT_CHEAT_STEALTH") &&
                  function_has(cheats, "static const char* run_stealth(", "log_toggle(\"Stealth Mode\""),
              "the toggle is one session value and one log line");
        CHECK(!strstr(cheats, "debug_menu_move_player") && !strstr(cheats, "noclip_commit") && !strstr(cheats, "CH_CAMERA_NOCLIP"),
              "Noclip never moves Anne or the camera: it keeps no position of its own");
        CHECK(function_has(cheats, "static const char* run_noclip(", "remember(JPT_CHEAT_NOCLIP") &&
                  !function_has(cheats, "static const char* run_noclip(", "FREE_CAMERA") &&
                  !function_has(cheats, "static const char* run_noclip(", "suspend"),
              "the Noclip toggle is one session value and one log line: no Fly, no camera");
    }
    if (fixes) {
        CHECK(function_has(fixes, "static void noclip_before(", "nc_vertical_response") &&
                  function_has(fixes, "static void noclip_before(", "cheats_noclip()") &&
                  function_has(fixes, "static void noclip_after(", "nc_next_velocity"),
              "Noclip works on the physics step's contact responses, in Pelvic_IDOF's wrapper");
        CHECK(!strstr(fixes, "ATTACHED") && !strstr(fixes, "camera") && !strstr(fixes, "PURGE"),
              "and touches neither the camera link nor a teleport");
        CHECK(function_has(fixes, "static void noclip_before(", "wrf(tag, response[0]);") &&
                  function_has(fixes, "static void noclip_before(", "wrf(tag + 4, response[1]);") &&
                  !function_has(fixes, "static void noclip_before(", "wrf(tag + 8"),
              "only the x and y of each part's BioTag are rewritten, never its z");
    }
    free(ai);
    free(cheats);
    free(list);
    free(fixes);
}

static void test_retail(const char* root) {
    static const struct {
        const char* name;
        uint32_t id, handler;
    } cheats[] = { { "LOC", 2, 0x0040B721u }, { "TELE", 3, 0x0040B8CFu }, { "TNEXT", 4, 0x0040B73Bu },
                   { "DINOS", 11, 0x0040B8BFu } };
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/game/trespass.exe", root);
    if (!load_exe(path)) {
        printf("debug_menu_tests: %s not installed, retail sites skipped\n", path);
        return;
    }
    CHECK(u32_at(0x006601E0u) == 12, "the cheat table has 12 entries");
    for (size_t i = 0; i < sizeof cheats / sizeof cheats[0]; i++) {
        int found = 0;
        for (uint32_t e = 0x00660180u; e < 0x006601E0u; e += 8)
            if (string_at(u32_at(e), cheats[i].name) && u32_at(e + 4) == cheats[i].id) found = 1;
        CHECK(found, "cheat %s is case %u", cheats[i].name, cheats[i].id);
        CHECK(u32_at(0x0040B8DCu + 4 * cheats[i].id) == cheats[i].handler, "cheat %s jumps to 0x%08X", cheats[i].name,
              cheats[i].handler);
    }
    CHECK(bytes_at(0x0040B8CFu, "5F5E5DB001"), "TELE's case returns without acting: compiled out of retail");
    CHECK(bytes_at(0x0040B721u, "A050DE6600"), "LOC toggles the byte at 0x0066DE50");
    CHECK(bytes_at(0x0040C805u, "A050DE6600") && bytes_at(0x0040C812u, "A144F473008B4814") &&
              bytes_at(0x0040C824u, "FF5210"),
          "LOC draws the active camera's presence (CWorld + 0x14, vtable + 0x10)");
    CHECK(calls(0x0040C8FCu, 0x004CD090u) && bytes_at(0x004CD090u, "83EC10D901"), "LOC's facing uses 0x004CD090");
    CHECK(calls(0x0040B73Bu, 0x0060CB70u), "TNEXT calls PlayerTeleportToNextLocation 0x0060CB70");
    CHECK(bytes_at(0x0060CB70u, "A144F47300") && bytes_at(0x0060CB79u, "8B7014") && calls(0x0060CB7Du, 0x0060C990u) &&
              calls(0x0060CB8Cu, 0x00548150u) && bytes_at(0x0060CB91u, "8B4650") && bytes_at(0x0060CBA8u, "FF5218") &&
              bytes_at(0x0060CBAEu, "FF5338") && bytes_at(0x0060CBB1u, "6A02") && calls(0x0060CBB3u, 0x006264D0u),
          "TNEXT: camera, NextTeleport, PurgeRenderCaches, attached mover, p3Placement, Move, SetNoPageFrames(2)");
    CHECK(bytes_at(0x0060C990u, "A114F673008B0D00F67300") && bytes_at(0x0060C9B5u, "8B4008"),
          "NextTeleport: iterator 0x0073F614, list head 0x0073F600, node value + 8");
    CHECK(bytes_at(0x0040B8BFu, "A17CE76600") && bytes_at(0x0040B8C4u, "8A4852"),
          "DINOS toggles CAISystem + 0x52 (bBoring) behind 0x0066E77C");
    CHECK(bytes_at(0x00607E90u, "518B54240853") && bytes_at(0x00607EA1u, "C6463801"),
          "CWorld::DeferredLoad 0x00607E90 sets its pending load");
    CHECK(bytes_at(0x00607FD0u, "8A4138") && calls(0x0040C4B3u, 0x00607FD0u) && calls(0x0040C4C6u, 0x00607FE0u) &&
              calls(0x0040C500u, 0x00413040u) && calls(0x0040C563u, 0x00414100u),
          "CGameWnd's frame turns a pending load into Loading Level and the full load 0x00414100");
    CHECK(calls(0x004702B1u, 0x00607E90u), "the game's own level-change action uses DeferredLoad");
    CHECK(calls(0x0041418Eu, 0x004142C0u) && bytes_at(0x00414337u, "8D550A"),
          "the full load keeps the scene name at 0x0066E338 + 0x0A");
    CHECK(bytes_at(0x0040BEEAu, "8B0D64EF6600") && bytes_at(0x0040BEF2u, "FF90FC010000"),
          "gpPlayer 0x0066EF64, dead = vtable + 0x1FC");
    CHECK(bytes_at(0x00415563u, "8B3D84546400") && bytes_at(0x0041555Cu, "837C240C57") &&
              calls(0x0041559Eu, 0x004157D0u),
          "Ctrl+Shift+Q+W in the main menu opens Direct Load 0x004157D0");
    CHECK(bytes_at(0x00415FAAu, "BF00136600") && string_at(0x00661300u, "*.scn") &&
              bytes_at(0x0041412Du, "BF800B6600") && string_at(0x00660B80u, "data\\"),
          "Direct Load lists data\\*.scn, the folder the load reads");
    CHECK(calls(0x004160D6u, 0x00414D80u) && calls(0x004160E5u, 0x00414100u) && calls(0x004160EFu, 0x00413160u) &&
              bytes_at(0x004157F3u, "8B166A02"),
          "Direct Load's OK: menu sounds off, full load (error box when < 0), the main menu ends with 2");
    CHECK(calls(0x0041566Cu, 0x00414D80u) && calls(0x00415696u, 0x00414100u) && bytes_at(0x004156A1u, "6A02"),
          "New Game runs the same sequence");
    {
        int woo = 0, invul = 0;
        for (uint32_t e = 0x00660180u; e < 0x006601E0u; e += 8) {
            woo |= string_at(u32_at(e), "WOO") && u32_at(e + 4) == 9;
            invul |= string_at(u32_at(e), "INVUL") && u32_at(e + 4) == 1;
        }
        CHECK(woo && u32_at(0x0040B8DCu + 4 * 9) == 0x0040B86Bu && bytes_at(0x0040B86Bu, "A0C1EC6600") &&
                  bytes_at(0x0043FA5Bu, "381DC1EC6600"),
              "WOO toggles g_bUnlimitedAmmo 0x0066ECC1, which CGun reads");
        CHECK(invul && u32_at(0x0040B8DCu + 4 * 1) == 0x0040B8CFu, "INVUL's console case is compiled out");
    }
    CHECK(bytes_at(0x00444671u, "C682DD00000000") && bytes_at(0x0044467Eu, "C680DE00000001") &&
              bytes_at(0x004569D7u, "8A82DD000000") && bytes_at(0x004569DDu, "85C0"),
          "CPlayer::bInvulnerable at + 0xDD, still tested before collision damage");
    CHECK(bytes_at(0x0060971Cu, "B80000C842") && bytes_at(0x00609727u, "894650894654") &&
              bytes_at(0x0060A847u, "D84350") && bytes_at(0x0060A84Du, "D85B54"),
          "CAnimate hit points + 0x50, maximum + 0x54 (100 each), regeneration clamped to the maximum");
    CHECK(bytes_at(0x004F0B27u, "D94370D84E14DC0DB0A86400") && bytes_at(0x0064A8B0u, "0000000000004E40"),
          "walk speed = rSpeedMultiplier (info + 0x70) x request x 60");
    CHECK(bytes_at(0x004444C2u, "6A78") && bytes_at(0x004D8436u, "C70744D66400") &&
              u32_at(0x0064D644u + 12) == 0x004F3B30u && calls(0x004F3B46u, 0x004F0960u) &&
              bytes_at(0x006149E3u, "8B4E28") && bytes_at(0x00611CEFu, "89510C"),
          "the player's own CPhysicsInfoPlayer at [[player + 0x28] + 0x0C] uses that handler");
    CHECK(bytes_at(0x004F0BF6u, "C705A8AF67000000803F") && bytes_at(0x004F0C06u, "C6055DAF670001") &&
              bytes_at(0x004E78E0u, "A05DAF6700") && bytes_at(0x004E7931u, "D905A8AF6700") &&
              calls(0x004F3FD2u, 0x004E2470u),
          "a jump: Pelvis_Jump.z = 1 and voluntary, read by Foot_IDOF inside Pelvic_IDOF");
    CHECK(bytes_at(0x00617F29u, "8B4B50") && bytes_at(0x00617F77u, "8B4354") && bytes_at(0x00618093u, "8D7358") &&
              calls(0x0061820Du, 0x00614B10u) && bytes_at(0x00614BD3u, "C20800"),
          "CEntityAttached::UpdateFromAttached: attached + 0x50, animate + 0x54, relative + 0x58, CInstance::Move");
    CHECK(bytes_at(0x00618250u, "8D7358"), "a save keeps the camera's relative placement: cheats leave it before saves");
    test_perception(root);
    CHECK(bytes_at(0x00608130u, "8A4144"), "CWorld's pending save flag + 0x44");
    CHECK(bytes_at(0x0041789Au, "8B3D60536400") && bytes_at(0x004178B5u, "FFD7"),
          "the modal UI loop reads its cursor (GetCursorPos) each pass: where the menu's actions run");
    CHECK(bytes_at(0x0040BECDu, "8D41E53DA2000000") && bytes_at(0x0040C220u + 0x72u - 0x1Bu, "07") &&
              u32_at(0x0040C200u + 7u * 4u) == 0x0040C1F6u && bytes_at(0x0040C220u + 0x70u - 0x1Bu, "02") &&
              u32_at(0x0040C200u + 2u * 4u) == 0x0040C002u,
          "CGameWnd::OnKey gives F3 no action (slot 7, its exit) and F1 the Hint (slot 2): F3 is free for Objectives");
    CHECK(bytes_at(0x0040C220u + 0x78u - 0x1Bu, "07"), "F9 has no action in CGameWnd::OnKey either");
    CHECK(calls(0x0040C00Au, 0x00416BF0u) && calls(0x0040C02Eu, 0x004175C0u) && bytes_at(0x00416C75u, "8B151CF06600") &&
              bytes_at(0x00416C85u, "81C250460000") && bytes_at(0x00416C8Du, "A15CE06600"),
          "F1: CHintWnd, DoModal; its text is LoadString(g_hInst, 18000 + iCurrentHint at 0x0066F01C)");
    CHECK(bytes_at(0x00607C10u, "568BF18B4E148B01FF904C010000") && calls(0x00607C30u, 0x00607B50u) &&
              bytes_at(0x005F6100u, "A1D8D77300"),
          "CWorld::pinsFindInstance 0x00607C10 (active camera, then the handle map) and u4Hash 0x005F6100");
    CHECK(bytes_at(0x00614B18u, "8D4630"), "CInstance::Move compares against the presence at + 0x30");
}

/* ---- the panel on WARP ---- */

static void test_panel(void) {
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0 };
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_row_t rows[DM_ROWS_MAX];
    dmp_frame_t f;
    cp_hud_t hud;
    int lit[2] = { 0, 0 }, low[2] = { 0, 0 }, amber = 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 3, D3D11_SDK_VERSION, &dev, NULL,
                                 &ctx))) {
        printf("debug_menu_tests: no WARP device, panel skipped\n");
        return;
    }
    memset(&d, 0, sizeof d);
    d.Width = 1280;
    d.Height = 960;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)tex, NULL, &rtv)))
        goto done;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging))) goto done;
    dm_init(&s);
    open_cheats(&s, &w);
    memset(&f, 0, sizeof f);
    f.view_w = 960.0f;
    f.view_h = 720.0f;
    f.dpi_scale = 1.5f;
    memset(&hud, 0, sizeof hud);
    hud.compass = hud.marker = hud.view = 1;
    hud.alpha = 1.0f;
    hud.heading = 30.0f;
    hud.relative = -150.0f;
    hud.camera.rot[0] = 1.0f;
    hud.camera.tan_half_h = 0.5f;
    hud.target[1] = 40.0f;
    hud.trail_count = 1;
    hud.trail[0][1] = 20.0f;
    strcpy(hud.distance, "184 m");
    strcpy(hud.objective, "Reach the monorail");
    hud.objective_alpha = 1.0f;
    strcpy(hud.notice, "Objective updated");
    for (int pass = 0; pass < 2; pass++) {
        f.menu = pass == 0;
        f.rows = rows;
        f.row_count = f.menu ? dm_rows(&s, &w, rows, DM_ROWS_MAX) : 0;
        f.feedback = "Heal";
        f.help = "help";
        f.hud = pass == 1 ? &hud : NULL;
        for (int frame = 0; frame < 3; frame++) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
            dmp_draw(dev, ctx, rtv, 1280, 960, &f);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < 960; y += 4)
                for (int x = 0; x < 1280; x += 4)
                    {
                        uint32_t px = ((const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x];
                        uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                        if (px & 0xFFFFFFu) {
                            lit[pass]++;
                            low[pass] += y > 960 / 5;
                        }
                        amber += pass == 1 && r > 200 && g > 150 && b < 110;
                    }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
        }
    }
    CHECK(lit[0] > 50, "the menu is drawn (%d samples)", lit[0]);
    CHECK(lit[1] > 100 && low[1] > 0 && low[1] < lit[1] / 2,
          "the compass, objective line and notice along the top, the world marker and trail in the picture (%d, %d below)",
          lit[1], low[1]);
    CHECK(amber > 10, "objective text and markers are amber (%d samples)", amber);
    {
        /* the title alone over a bright sky: amber letters inside one thin dark outline, every pixel read */
        static const float sky[4] = { 0.62f, 0.78f, 1.0f, 1.0f };
        cp_hud_t title;
        dmp_frame_t g;
        static unsigned char amber_map[320][1280], dark_map[320][1280];
        int amber_px = 0, dark_px = 0, reach = 0, distant = 0;
        memset(&title, 0, sizeof title);
        title.alpha = title.objective_alpha = 1.0f;
        strcpy(title.objective, "Reach the monorail");
        g = f;
        g.menu = 0;
        g.rows = NULL;
        g.row_count = 0;
        g.hud = &title;
        for (int frame = 0; frame < 3; frame++) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, sky);
            dmp_draw(dev, ctx, rtv, 1280, 960, &g);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < 320; y++) {
                const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                for (int x = 0; x < 1280; x++) {
                    uint32_t r = (row[x] >> 16) & 0xFF, gr = (row[x] >> 8) & 0xFF, b = row[x] & 0xFF;
                    amber_map[y][x] = (unsigned char)(r > 110 && gr > 70 && r > b + 50);
                    dark_map[y][x] = (unsigned char)(r < 80 && gr < 90 && b < 110);
                    amber_px += r > 200 && gr > 150 && b < 110;
                    dark_px += dark_map[y][x];
                }
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
            /* how far the dark reaches from the letters: an outline hugs them, a shadow or a doubled copy does not */
            for (int y = 0; y < 320; y++)
                for (int x = 0; x < 1280; x++) {
                    int d = 0;
                    if (!dark_map[y][x]) continue;
                    for (int k = 1; k <= 6 && !d; k++)
                        for (int dy = -k; dy <= k && !d; dy++)
                            for (int dx = -k; dx <= k; dx++) {
                                int ax = x + dx, ay = y + dy;
                                if ((dx > -k && dx < k && dy > -k && dy < k) || ax < 0 || ay < 0 || ax >= 1280 || ay >= 320)
                                    continue;
                                if (amber_map[ay][ax]) {
                                    d = k;
                                    break;
                                }
                            }
                    if (!d) d = 7;
                    distant += d > 3;
                    if (d > reach) reach = d;
                }
        }
        CHECK(amber_px > 80 && dark_px > 80, "the title is amber with a dark outline (%d, %d pixels)", amber_px, dark_px);
        CHECK(reach >= 1 && distant * 50 <= dark_px, "the outline hugs the letters: %d of %d dark pixels are more than 3 px from them", distant, dark_px);
    }
    {
        /* the Cheats panel as drawn: its click targets are the rows, inside the panel, with the close button in the header */
        dm_state_t c;
        dm_levels_t scenes;
        dm_world_t cw = level_world("testscnnght.scn");
        const dml_hits_t* hits;
        float main_w = 0.0f;
        memset(&scenes, 0, sizeof scenes);
        for (int i = 0; i < 12; i++) {
            static const char* const names[] = { "be.scn", "jr.scn", "ij.scn", "it.scn", "lab.scn", "as.scn", "as2.scn",
                                                 "sum.scn", "testscene.scn", "testscnnght.scn", "alpha.scn", "zz_custom.scn" };
            dm_levels_add(&scenes, names[i]);
        }
        cw.hp = 90.0f;
        cw.hp_max = 100.0f;
        for (int page = 0; page < 3; page++) {
            int enabled = 0;
            dm_init(&c);
            dm_set_levels(&c, &scenes);
            dm_toggle(&c, &cw, DM_MENU_CHEATS);
            if (page == 1) {
                c.cursor = DM_LEVEL_SELECT;
                dm_key(&c, &cw, VK_RETURN, 1, 0, 0, 0, 0);
            } else if (page == 2) {
                c.cursor = DM_TELEPORT_PAGE;
                dm_key(&c, &cw, VK_RETURN, 1, 0, 0, 0, 0);
            }
            memset(&f, 0, sizeof f);
            f.view_w = 960.0f;
            f.view_h = 720.0f;
            f.dpi_scale = 1.5f;
            f.menu = 1;
            f.rows = rows;
            f.row_count = dm_rows(&c, &cw, rows, DM_ROWS_MAX);
            f.title = dm_menu_title(DM_MENU_CHEATS);
            f.help_lines = DMP_HELP_LINES;
            f.context_help = dm_help_text(&c, &cw);
            f.menu_id = DM_MENU_CHEATS;
            f.page = c.page;
            for (int i = 0; i < f.row_count; i++) enabled += rows[i].kind == DM_ROW_ITEM && rows[i].enabled;
            for (int frame = 0; frame < 3; frame++) {
                ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
                dmp_draw(dev, ctx, rtv, 1280, 960, &f);
            }
            hits = dmp_hits();
            CHECK(hits->valid && hits->menu == DM_MENU_CHEATS && hits->page == c.page && hits->has_close,
                  "page %d: the panel reports its targets", page);
            CHECK(hits->count == enabled, "page %d: every enabled row is a target (%d of %d)", page, hits->count, enabled);
            CHECK(hits->panel.x >= 0.0f && hits->panel.y >= 0.0f && hits->panel.x + hits->panel.w <= 960.0f &&
                      hits->panel.y + hits->panel.h <= 720.0f,
                  "page %d: the panel is on the screen (%.0f,%.0f %.0fx%.0f)", page, hits->panel.x, hits->panel.y, hits->panel.w,
                  hits->panel.h);
            CHECK(dml_inside(&hits->close, &hits->panel) && hits->close.y < hits->panel.y + 60.0f &&
                      hits->close.x > hits->panel.x + hits->panel.w * 0.5f && hits->close.w >= 24.0f,
                  "page %d: the close button is at the panel's top right", page);
            for (int i = 0; i < hits->count; i++) {
                CHECK(dml_inside(&hits->hit[i].rect, &hits->panel), "page %d: target %d is inside the panel", page, i);
                CHECK(!dml_contains(&hits->hit[i].rect, hits->close.x + 1.0f, hits->close.y + 1.0f),
                      "page %d: no row under the close button", page);
                for (int j = 0; j < i; j++)
                    CHECK(hits->hit[j].rect.y + hits->hit[j].rect.h <= hits->hit[i].rect.y + 0.5f ||
                              hits->hit[i].rect.y + hits->hit[i].rect.h <= hits->hit[j].rect.y + 0.5f,
                          "page %d: targets %d and %d do not overlap", page, j, i);
            }
            CHECK(dml_inside(&hits->help, &hits->panel) && hits->help.h > 0.0f, "page %d: the help area is inside the panel", page);
            if (page == 0) {
                /* every row selected in turn, with and without a help line or a message: the panel never moves */
                const dml_rect_t first = hits->panel;
                int moved = 0;
                for (int row = 0; row < f.row_count; row++) {
                    if (rows[row].kind != DM_ROW_ITEM || !rows[row].enabled) continue;
                    c.cursor = rows[row].item;
                    f.row_count = dm_rows(&c, &cw, rows, DM_ROWS_MAX);
                    f.context_help = dm_help_text(&c, &cw);
                    f.feedback = row % 3 == 0 ? "Not available" : NULL;
                    cw.hp = row % 2 ? 100.0f : 90.0f;
                    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
                    dmp_draw(dev, ctx, rtv, 1280, 960, &f);
                    hits = dmp_hits();
                    moved += hits->panel.x != first.x || hits->panel.y != first.y || hits->panel.w != first.w ||
                             hits->panel.h != first.h;
                }
                CHECK(moved == 0, "the main panel keeps its size and place on every row (%d changes)", moved);
                c.cursor = DM_HEAL;
                CHECK(dm_help_text(&c, &cw) != NULL, "Heal has its help line in the reserved area");
                f.feedback = NULL;
                main_w = hits->panel.w;
            }
            if (page == 1) {
                CHECK(hits->panel.w <= main_w + 0.5f, "Level Select is no wider than the main menu (%.0f against %.0f)",
                      hits->panel.w, main_w);
                CHECK(hits->count == scenes.count + 1, "every level and Back are targets (%d)", hits->count);
                CHECK(dm_mouse_click(&c, &cw, hits, hits->hit[hits->count - 1].rect.x + 5.0f,
                                     hits->hit[hits->count - 1].rect.y + hits->hit[hits->count - 1].rect.h * 0.5f, 0) &&
                          c.page == DM_PAGE_MAIN,
                      "a click on the drawn < Back returns to Cheats");
            }
            if (page == 0)
                CHECK(dm_mouse_click(&c, &cw, hits, hits->close.x + hits->close.w * 0.5f, hits->close.y + hits->close.h * 0.5f, 0) &&
                          !c.open,
                      "a click on the drawn close button closes the menu");
        }
        f.menu = 0;
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
        dmp_draw(dev, ctx, rtv, 1280, 960, &f);
        CHECK(!dmp_hits()->valid, "no menu, no targets");
        CHECK(dmp_imgui_errors() == 0,
              "Dear ImGui reported no layout error drawing the main page (every row, with and without help), Teleport, "
              "Level Select and the Debug Menu (%d)", dmp_imgui_errors());
    }
    {
        /* the drawn minimap: bottom-left, the player marker fixed, the map sliding under it as the player walks */
        static uint32_t shot[2][960][1280];
        const minimap_bounds_t* mb = minimap_bounds("BE");
        const minimap_level_map_t* mm = minimap_map("BE");
        const minimap_run_t* best = NULL;
        cp_hud_t mh;
        dmp_frame_t g;
        minimap_frame_t expect;
        minimap_rect_t pic = { 0.0f, 0.0f, 1280.0f, 960.0f };
        float x0, s0, x1, n0, px, py;
        for (int i = 0; mm && i < mm->count; i++)
            if (mm->runs[i].kind == MM_KIND_LAND && (!best || mm->runs[i].len > best->len)) best = &mm->runs[i];
        if (mb && mm && best && minimap_run_world(mb, mm, best, &x0, &s0, &x1, &n0)) {
            px = (x0 + x1) * 0.5f;
            py = (s0 + n0) * 0.5f;
            memset(&g, 0, sizeof g);
            g.view_w = 1280.0f;
            g.view_h = 960.0f;
            g.dpi_scale = 1.0f;
            g.hud = &mh;
            for (int shot_i = 0; shot_i < 2; shot_i++) {
                memset(&mh, 0, sizeof mh);
                mh.minimap = 1;
                strcpy(mh.minimap_level, "BE");
                mh.player_pos[0] = px + 40.0f * (float)shot_i;
                mh.player_pos[1] = py;
                minimap_place(&pic, mh.player_pos[0], mh.player_pos[1], MM_ZOOM, &expect);
                for (int frame = 0; frame < 3; frame++) {
                    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
                    dmp_draw(dev, ctx, rtv, 1280, 960, &g);
                }
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
                    for (int y = 0; y < 960; y++)
                        memcpy(shot[shot_i][y], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, 1280 * 4);
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
                }
            }
            {
                int outside = 0, inside = 0, moved = 0, white[2] = { 0, 0 };
                const int cx = (int)expect.cx, cy = (int)expect.cy, r = (int)expect.radius;
                for (int y = 0; y < 960; y++)
                    for (int x = 0; x < 1280; x++) {
                        const int in_box = x >= cx - r - 4 && x <= cx + r + 4 && y >= cy - r - 4 && y <= cy + r + 4;
                        if ((shot[0][y][x] & 0xFFFFFFu) && !in_box) outside++;
                        if ((shot[0][y][x] & 0xFFFFFFu) && in_box) inside++;
                        if (in_box && shot[0][y][x] != shot[1][y][x]) moved++;
                    }
                for (int k = 0; k < 2; k++) {
                    const uint32_t px_ = shot[k][cy][cx];
                    white[k] = ((px_ >> 16) & 0xFF) > 200 && ((px_ >> 8) & 0xFF) > 200 && (px_ & 0xFF) > 200;
                }
                CHECK(cx < 640 && cy > 480 && expect.cx - expect.radius < 40.0f && 960.0f - (expect.cy + expect.radius) < 40.0f,
                      "the drawn minimap sits in the bottom-left corner");
                CHECK(inside > 2000 && outside == 0, "only the bottom-left circle is drawn (%d inside, %d outside)", inside,
                      outside);
                CHECK(white[0] && white[1], "the white player marker is at the circle centre before and after walking");
                CHECK(moved > 300, "the drawn map changes when the player walks (%d pixels)", moved);
            }
        }
    }
    dmp_release();
done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (tex) ID3D11Texture2D_Release(tex);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
}

void cheats_tests(int* checks, int* failures);
void compass_tests(int* checks, int* failures);
void minimap_tests(int* checks, int* failures);
void objective_tests(int* checks, int* failures);
void overlay_tests(int* checks, int* failures);
void physics_watch_tests(int* checks, int* failures);
void menu_l10n_tests(int* checks, int* failures);
void menu_layout_tests(int* checks, int* failures);

int main(void) {
    test_profile_activate();
    test_open_close();
    test_input_capture();
    test_navigation_and_pad();
    test_levels();
    test_level_names();
    test_teleport();
    test_yaw();
    test_toggles();
    test_stale_state();
    test_cheats_menu();
    cheats_tests(&g_checks, &g_failures);
    compass_tests(&g_checks, &g_failures);
    minimap_tests(&g_checks, &g_failures);
    objective_tests(&g_checks, &g_failures);
    overlay_tests(&g_checks, &g_failures);
    physics_watch_tests(&g_checks, &g_failures);
    menu_l10n_tests(&g_checks, &g_failures);
    menu_layout_tests(&g_checks, &g_failures);
    test_retail(JPT_SOURCE_DIR);
    test_panel();
    printf("debug_menu_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
