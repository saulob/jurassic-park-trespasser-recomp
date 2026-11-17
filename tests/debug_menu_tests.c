/* Headless checks of the F5 Debug / Test Menu: its policy (input, items, checkpoint, formatting, key points), the
 * retail sites it reuses (game/trespass.exe 1.0.116) and its panel, compass included, on a WARP device. */
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
    w.key_count = 4;
    w.key_index = -1;
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
    w.key_index = -1;
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

static void test_f5(void) {
    dm_state_t s;
    dm_world_t none, menu = main_menu_world(), level = level_world("be.scn");
    memset(&none, 0, sizeof none);
    dm_init(&s);
    CHECK(!dm_f5(&s, &none) && !s.open, "F5 with no level and no main menu does nothing");
    CHECK(dm_f5(&s, &menu) && s.open, "F5 in the main menu opens");
    CHECK(s.cursor == DM_LEVEL_SELECT, "the main menu starts on Level Select");
    CHECK(dm_f5(&s, &menu) && !s.open, "F5 again closes");
    CHECK(dm_f5(&s, &level) && s.open && s.cursor == DM_TELEPORT, "F5 in a level opens on Teleport");
    CHECK(s.field[0] == 10.0f && s.field[1] == 20.0f && s.field[2] == 30.0f, "the fields start at the player");
    CHECK(dm_f5(&s, &level) && !s.open, "F5 closes in a level");
    level.player = 0;
    CHECK(dm_f5(&s, &level) && s.cursor == DM_LEVEL_SELECT && s.fill_fields,
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
    dm_f5(&s, &w);
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
    dm_f5(&s, &w);
    key(&s, &w, VK_RETURN, DOWN);
    dm_f5(&s, &w);
    CHECK(!dm_key(&s, &w, VK_RETURN, DOWN, 0, 0, 0, 0), "a fresh press after a lost key-up is the game's");
    CHECK(!key(&s, &w, VK_RETURN, UP), "with its key-up");
}

static void test_navigation_and_pad(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_action_t a;
    dm_init(&s);
    dm_f5(&s, &w);
    press(&s, &w, VK_UP);
    CHECK(s.cursor == DM_FIELD_Z, "Up moves to the item above");
    w.player = 0;
    dm_world_seen(&s, &w);
    CHECK(s.cursor == DM_LEVEL_SELECT, "a disabled item gives the cursor to the first enabled one");
    for (int i = 0; i < 6; i++) press(&s, &w, VK_DOWN);
    CHECK(dm_item_enabled(&s, &w, (dm_item_t)s.cursor), "Down skips disabled items (%d)", s.cursor);
    w.player = 1;
    dm_pad(&s, &w, PAD(DPAD_DOWN), 0);
    dm_pad(&s, &w, PAD(EAST), 0);
    CHECK(!s.open, "B closes the main page");
    dm_f5(&s, &w);
    dm_pad(&s, &w, PAD(SOUTH), 0);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_TELEPORT, "A uses the item");
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
    dm_f5(&s, &menu);
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
    dm_f5(&s, &w);
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
    dm_toggle(&s, &menu, DM_MENU_DEBUG);
    press(&s, &menu, VK_RETURN);
    n = dm_rows(&s, &menu, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++)
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == 0)
            CHECK(!strcmp(rows[i].text, "Beach") && !rows[i].value[0], "from the main menu the list reads the same");
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_DOWN);
    press(&s, &menu, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "ij.scn"),
          "the Debug Menu sends the same file");
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    for (int i = 0; i < 7; i++) press(&s, &w, VK_DOWN);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_LOAD_LEVEL && !strcmp(a.scene, "alpha.scn"),
          "a custom scene is loaded by its own file name");
}

static void test_restart_reload(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_init(&s);
    CHECK(!dm_item_enabled(&s, &menu, DM_RESTART) && !dm_item_enabled(&s, &menu, DM_RELOAD), "no level: no restart");
    dm_f5(&s, &w);
    go_to(&s, &w, DM_RESTART);
    press(&s, &w, VK_RETURN);
    CHECK(!s.open && dm_take(&s, &a) && a.kind == DM_ACT_RESTART, "Restart Level is routed and closes the menu");
    dm_f5(&s, &w);
    go_to(&s, &w, DM_RELOAD);
    press(&s, &w, VK_RETURN);
    CHECK(!s.open && dm_take(&s, &a) && a.kind == DM_ACT_RELOAD && !strcmp(a.scene, "be.scn"),
          "Reload Current Level loads the current scene");
    w.scene[0] = 0;
    CHECK(!dm_item_enabled(&s, &w, DM_RELOAD) && dm_item_enabled(&s, &w, DM_RESTART), "no scene name: no reload");
}

static void test_teleport(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    dm_action_t a;
    float nan_pos[3] = { 0.0f, (float)NAN, 0.0f }, far_pos[3] = { 0.0f, 0.0f, 200000.0f };
    float ok_pos[3] = { -5.5f, 1e4f, 0.0f };
    CHECK(!dm_teleport_valid(nan_pos) && !dm_teleport_valid(far_pos) && dm_teleport_valid(ok_pos), "teleport targets");
    dm_init(&s);
    dm_f5(&s, &w);
    go_to(&s, &w, DM_FIELD_X);
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

static void test_checkpoint(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), other = level_world("it.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_init(&s);
    CHECK(!dm_item_enabled(&s, &w, DM_RETURN_CHECKPOINT), "no checkpoint: Return disabled");
    dm_f5(&s, &w);
    go_to(&s, &w, DM_SET_CHECKPOINT);
    w.at.rot[0] = 0.0f;
    w.at.rot[3] = 1.0f;
    press(&s, &w, VK_RETURN);
    CHECK(s.checkpoint.set && !strcmp(s.checkpoint.level, "BE") && s.checkpoint.at.pos[1] == 20.0f &&
              s.checkpoint.at.rot[3] == 1.0f,
          "Set Checkpoint keeps the level, position and orientation");
    CHECK(!strcmp(dm_feedback_text(&s, 1000), "Checkpoint set") && !dm_feedback_text(&s, 1000 + DM_FEEDBACK_MS),
          "with short feedback");
    w.at.pos[1] = 99.0f;
    go_to(&s, &w, DM_RETURN_CHECKPOINT);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_RETURN_CHECKPOINT && a.to.pos[1] == 20.0f && a.to.rot[3] == 1.0f,
          "Return to Checkpoint asks for the saved placement");
    dm_world_seen(&s, &w);
    CHECK(s.checkpoint.set, "the same level (a restart) keeps it");
    CHECK(!dm_item_enabled(&s, &other, DM_RETURN_CHECKPOINT), "never restored in another level");
    dm_world_seen(&s, &other);
    CHECK(!s.checkpoint.set && !dm_item_enabled(&s, &w, DM_RETURN_CHECKPOINT), "a level change clears it");
    s.checkpoint.set = 1;
    strcpy(s.checkpoint.level, "BE");
    dm_world_seen(&s, &menu);
    CHECK(!s.checkpoint.set, "so does the main menu");
}

static void test_coordinates(void) {
    dm_placement_t p;
    char out[128];
    double half = 1.570 / 2.0;
    memset(&p, 0, sizeof p);
    p.pos[0] = 123.456f;
    p.pos[1] = 78.9f;
    p.pos[2] = -45.12f;
    p.rot[0] = (float)cos(half);
    p.rot[3] = (float)sin(half);
    CHECK(dm_format_coords(out, sizeof out, "BE", &p) &&
              !strcmp(out, "Level=BE X=123.456 Y=78.900 Z=-45.120 Yaw=1.570"),
          "clipboard text: %s", out);
    memset(&p, 0, sizeof p);
    p.pos[0] = -0.0001f;
    p.rot[0] = 1.0f;
    CHECK(dm_format_coords(out, sizeof out, "", &p) && !strcmp(out, "Level=- X=0.000 Y=0.000 Z=0.000 Yaw=0.000"),
          "no level, no negative zero: %s", out);
    CHECK(!dm_format_coords(out, 10, "BE", &p), "a short buffer is refused");
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

/* A std::list<CTeleport*> in a fake guest memory: sentinel at 0x1000, nodes at 0x1100 + 0x10 * i. */
static uint32_t g_mem[0x200];

static int fake_read(uint32_t va, uint32_t* out) {
    if (va < 0x1000 || va >= 0x1000 + sizeof g_mem || (va & 3)) return 0;
    *out = g_mem[(va - 0x1000) / 4];
    return 1;
}

static void fake_put(uint32_t va, uint32_t v) { g_mem[(va - 0x1000) / 4] = v; }

static uint32_t node(int i) { return 0x1100u + 0x10u * (uint32_t)i; }

static void fake_list(int n) {
    memset(g_mem, 0, sizeof g_mem);
    fake_put(0x1000, n ? node(0) : 0x1000);
    fake_put(0x1004, n ? node(n - 1) : 0x1000);
    for (int i = 0; i < n; i++) {
        fake_put(node(i), i + 1 < n ? node(i + 1) : 0x1000);
        fake_put(node(i) + 4, i ? node(i - 1) : 0x1000);
        fake_put(node(i) + 8, 0xC0DE0000u + (uint32_t)i);
    }
}

/* CTeleport::NextTeleport (0x0060C990): wraps at the end, answers the iterator's node and advances it. */
static int fake_tnext(uint32_t* iterator) {
    uint32_t at = *iterator, next;
    if (at == 0x1000) fake_read(0x1000, &at);
    if (at == 0x1000) return -1;
    fake_read(at, &next);
    *iterator = next;
    return (int)((at - 0x1100u) / 0x10u);
}

static void test_key_points(void) {
    dm_keys_t k;
    uint32_t it;
    int reached[8], ok = 1;
    fake_list(4);
    CHECK(dm_keys_walk(fake_read, 0x1000, node(0), &k) && k.count == 4 && k.index == -1 && k.prev_target == node(3),
          "before any TNEXT: 4 key points, none reached, Previous goes to the last");
    CHECK(dm_keys_walk(fake_read, 0x1000, node(2), &k) && k.index == 1 && k.prev_target == node(0),
          "after reaching the second, Previous goes to the first");
    CHECK(dm_keys_walk(fake_read, 0x1000, 0x1000, &k) && k.index == 3 && k.prev_target == node(2),
          "after the last, Previous goes to the third");
    CHECK(dm_keys_walk(fake_read, 0x1000, node(1), &k) && k.index == 0 && k.prev_target == node(3),
          "Previous from the first wraps to the last");
    it = node(0);
    for (int i = 0; i < 6; i++) reached[i] = fake_tnext(&it);
    CHECK(reached[0] == 0 && reached[3] == 3 && reached[4] == 0 && reached[5] == 1, "TNEXT walks and wraps");
    for (int step = 0; step < 6; step++) {
        int before;
        dm_keys_walk(fake_read, 0x1000, it, &k);
        before = k.index;
        it = k.prev_target;
        if (fake_tnext(&it) != dm_prev_key_index(4, before)) ok = 0;
        dm_keys_walk(fake_read, 0x1000, it, &k);
        if (k.index != dm_prev_key_index(4, before)) ok = 0;
    }
    CHECK(ok, "Previous = the iterator on the node before, then TNEXT itself");
    CHECK(dm_prev_key_index(0, -1) == -1 && dm_prev_key_index(1, 0) == 0 && dm_prev_key_index(5, 3) == 2, "indices");
    fake_list(0);
    CHECK(dm_keys_walk(fake_read, 0x1000, 0x1000, &k) && k.count == 0 && k.index == -1, "an empty list");
    fake_list(4);
    CHECK(!dm_keys_walk(fake_read, 0x1000, 0x1234, &k) && k.count == 0, "an iterator outside the list is refused");
    fake_put(node(2) + 4, node(0));
    CHECK(!dm_keys_walk(fake_read, 0x1000, node(0), &k), "a broken back link is refused");
    fake_list(4);
    fake_put(node(3), node(1));
    CHECK(!dm_keys_walk(fake_read, 0x1000, node(0), &k), "a cycle is refused");
    CHECK(!dm_keys_walk(fake_read, 0, 0, &k), "no list");
    {
        dm_state_t s;
        dm_world_t w = level_world("be.scn");
        dm_action_t a;
        dm_init(&s);
        dm_f5(&s, &w);
        go_to(&s, &w, DM_NEXT_KEY);
        press(&s, &w, VK_RETURN);
        CHECK(dm_take(&s, &a) && a.kind == DM_ACT_NEXT_KEY && s.open, "Next Key Point is routed");
        go_to(&s, &w, DM_PREV_KEY);
        press(&s, &w, VK_RETURN);
        CHECK(dm_take(&s, &a) && a.kind == DM_ACT_PREV_KEY, "Previous Key Point is routed");
        w.key_count = 0;
        CHECK(!dm_item_enabled(&s, &w, DM_NEXT_KEY) && !dm_item_enabled(&s, &w, DM_PREV_KEY),
              "no key points: disabled");
    }
}

static void test_toggles(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_init(&s);
    CHECK(!s.show_coords, "Show Coordinates starts off");
    dm_f5(&s, &w);
    go_to(&s, &w, DM_SHOW_COORDS);
    press(&s, &w, VK_RETURN);
    CHECK(s.show_coords && s.open, "Show Coordinates toggles on");
    press(&s, &w, VK_RETURN);
    CHECK(!s.show_coords, "and off");
    go_to(&s, &w, DM_COPY_COORDS);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_COPY, "Copy Coordinates is routed");
    go_to(&s, &w, DM_FREEZE_AI);
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI && a.on == 1, "Freeze AI on");
    w.ai = 1;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FREEZE_AI && a.on == 0, "Freeze AI off follows the game's flag");
    w.ai = -1;
    CHECK(!dm_item_enabled(&s, &w, DM_FREEZE_AI) && !dm_item_enabled(&s, &menu, DM_FREEZE_AI),
          "no AI system: disabled");
    CHECK(!dm_item_enabled(&s, &menu, DM_COPY_COORDS) && dm_item_enabled(&s, &menu, DM_SHOW_COORDS),
          "no player: nothing to copy");
}

static void test_stale_state(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), none;
    dm_row_t rows[DM_ROWS_MAX];
    int n, disabled_return = 0, titled = 0;
    memset(&none, 0, sizeof none);
    dm_init(&s);
    dm_f5(&s, &w);
    go_to(&s, &w, DM_FIELD_Y);
    dm_char(&s, &w, '5');
    dm_world_seen(&s, &none);
    CHECK(!s.open && !s.editing && s.page == DM_PAGE_MAIN, "a load or a dialog in front closes the menu");
    w.player = 0;
    dm_f5(&s, &w);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == DM_RETURN_CHECKPOINT && !rows[i].enabled)
            disabled_return = 1;
        if (rows[i].kind == DM_ROW_TITLE && !strcmp(rows[i].text, "Debug / Test Menu")) titled = 1;
        if (rows[i].kind == DM_ROW_INFO && !strcmp(rows[i].text, "Player"))
            CHECK(!strcmp(rows[i].value, "unavailable"), "coordinates unavailable without a player");
    }
    CHECK(titled && disabled_return && n > 15 && n < DM_ROWS_MAX, "rows: title, disabled items (%d rows)", n);
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
    CHECK(dm_toggle(&s, &w, DM_MENU_DEBUG) && s.open && s.menu == DM_MENU_DEBUG && s.page == DM_PAGE_MAIN,
          "F5 over Cheats replaces it: one menu at a time");
    CHECK(dm_toggle(&s, &w, DM_MENU_CHEATS) && s.open && s.menu == DM_MENU_CHEATS,
          "F10 over the Debug Menu replaces it");
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
          "Freeze Dinosaurs asks for the Debug Menu's Freeze AI action");
    w.ai = 1;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = row_of(rows, n, DM_FREEZE_AI);
    CHECK(r && !strcmp(r->text, "Freeze Dinosaurs") && !strcmp(r->value, "On"), "Cheats shows the DINOS flag");
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = row_of(rows, n, DM_FREEZE_AI);
    CHECK(r && !strcmp(r->text, "Freeze AI") && !strcmp(r->value, "On"), "and so does the Debug Menu: one flag");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
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
          "and the Debug Menu's Teleport action");
    press(&s, &w, VK_ESCAPE);
    CHECK(s.open && s.page == DM_PAGE_MAIN && s.cursor == DM_TELEPORT_PAGE, "Esc goes back to Cheats");
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    CHECK(s.field[0] == 10.0f, "each opening starts the fields at the player");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    go_to(&s, &w, DM_LEVEL_SELECT);
    press(&s, &w, VK_RETURN);
    CHECK(s.page == DM_PAGE_LEVELS, "Level Select: the same list");
    press(&s, &w, VK_ESCAPE);
    CHECK(s.page == DM_PAGE_MAIN && s.cursor == DM_LEVEL_SELECT, "and back");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!row_of(rows, n, DM_SHOW_COORDS), "Show Coordinates is not a cheat");
    for (int i = 0; i < n; i++)
        CHECK(strcmp(rows[i].text, "Show Coordinates") && strcmp(rows[i].text, "Third Person"),
              "no Show Coordinates or Third Person row: %s", rows[i].text);
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
    CHECK(!row_of(rows, n, DM_RESTART) && !row_of(rows, n, DM_COPY_COORDS), "only the Cheats items");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    dm_toggle(&s, &menu, DM_MENU_CHEATS);
    CHECK(s.open && s.cursor == DM_LEVEL_SELECT, "in the main menu Cheats offers Level Select");
    for (int i = 0; i < DM_ITEM_COUNT; i++)
        if (i != DM_LEVEL_SELECT && i != DM_SHOW_COORDS && i != DM_AMBIENT_OCCLUSION && i != DM_WATER_REFRACTION &&
            i != DM_ENHANCED_SKY && i != DM_BACK && i != DM_CLOSE)
            CHECK(!dm_item_enabled(&s, &menu, (dm_item_t)i), "no level: %s disabled",
                  dm_item_label(DM_MENU_CHEATS, (dm_item_t)i));
}

static void test_ambient_occlusion(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_row_t rows[DM_ROWS_MAX];
    int n, graphics = -1, at = -1;
    dm_init(&s);
    w.ambient_occlusion = 0;
    dm_f5(&s, &w);
    go_to(&s, &w, DM_AMBIENT_OCCLUSION);
    CHECK(s.cursor == DM_AMBIENT_OCCLUSION, "the Debug Menu reaches Ambient Occlusion");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == DM_ROW_SECTION && !strcmp(rows[i].text, "Graphics")) graphics = i;
        if (rows[i].kind == DM_ROW_ITEM && rows[i].item == DM_AMBIENT_OCCLUSION) at = i;
    }
    CHECK(graphics >= 0 && at == graphics + 2 && rows[graphics + 1].item == DM_FIELD_OF_VIEW &&
              row_says(rows, n, DM_AMBIENT_OCCLUSION, "Off") && rows[at].selected &&
              !strcmp(rows[at].text, "Ambient Occlusion"),
          "a Graphics section holds Field of View, then Ambient Occlusion: Off");
    CHECK(rows[n - 1].item == DM_CLOSE, "Close stays last");
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_AMBIENT_OCCLUSION && a.value == 1, "Enter: Off -> Low");
    w.ambient_occlusion = 4;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.value == 0, "Enter wraps Ultra -> Off");
    press(&s, &w, VK_RIGHT);
    CHECK(!dm_take(&s, &a), "Right stops at Ultra");
    press(&s, &w, VK_LEFT);
    CHECK(dm_take(&s, &a) && a.value == 3, "Left: Ultra -> High");
    w.ambient_occlusion = 0;
    press(&s, &w, VK_LEFT);
    CHECK(!dm_take(&s, &a), "Left stops at Off");
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_DPAD_RIGHT), 1000);
    CHECK(dm_take(&s, &a) && a.value == 1, "D-pad Right: Off -> Low");
    w.ambient_occlusion = 3;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(row_says(rows, n, DM_AMBIENT_OCCLUSION, "High"), "the row shows the renderer's level");
    w.ambient_occlusion = -1;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!dm_item_enabled(&s, &w, DM_AMBIENT_OCCLUSION) && row_says(rows, n, DM_AMBIENT_OCCLUSION, "Unavailable"),
          "software renderer or no feature level 11.0: Unavailable");
    menu.ambient_occlusion = 2;
    CHECK(dm_item_enabled(&s, &menu, DM_AMBIENT_OCCLUSION), "it can be changed from the main menu too");
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!row_of(rows, n, DM_AMBIENT_OCCLUSION), "not in Cheats");
}

/* Field of View: the session's value, stepped by 1 (Shift, LB/RB: 10) inside the setting's range; Enter is Default. */
static void test_field_of_view(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu = main_menu_world();
    dm_action_t a;
    dm_row_t rows[DM_ROWS_MAX];
    const dm_row_t* r;
    int n;
    dm_init(&s);
    w.fov = JPT_FOV_DEFAULT;
    w.ambient_occlusion = 2;
    dm_f5(&s, &w);
    go_to(&s, &w, DM_FIELD_OF_VIEW);
    CHECK(s.cursor == DM_FIELD_OF_VIEW, "the Debug Menu reaches Field of View");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    r = row_of(rows, n, DM_FIELD_OF_VIEW);
    CHECK(r && !strcmp(r->text, "Field of View") && !strcmp(r->value, "75 (Default)") && r->selected,
          "Field of View 75 (Default)");
    CHECK(r && fabsf(r->bar - 0.3f) < 1e-6f, "its bar sits at 75 of 60..110");
    for (int i = 0; i < n; i++) CHECK(rows[i].item == DM_FIELD_OF_VIEW || rows[i].bar < 0.0f, "only Field of View has a bar");
    press(&s, &w, VK_RIGHT);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FIELD_OF_VIEW && a.value == 76, "Right: 76");
    press(&s, &w, VK_LEFT);
    CHECK(dm_take(&s, &a) && a.value == 74, "Left: 74");
    dm_key(&s, &w, VK_RIGHT, 1, 0, 1, 0, 1000);
    dm_key(&s, &w, VK_RIGHT, 0, 0, 1, 0, 1000);
    CHECK(dm_take(&s, &a) && a.value == 85, "Shift+Right: 85");
    dm_key(&s, &w, VK_LEFT, 1, 0, 0, 1, 1000);
    dm_key(&s, &w, VK_LEFT, 0, 0, 0, 1, 1000);
    CHECK(dm_take(&s, &a) && a.value == 74, "Ctrl+Left still moves a whole degree");
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER), 1000);
    CHECK(dm_take(&s, &a) && a.value == 85, "RB: +10");
    w.fov = 105;
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER), 2000);
    CHECK(dm_take(&s, &a) && a.value == JPT_FOV_MAX, "RB stops at the maximum");
    w.fov = JPT_FOV_MAX;
    press(&s, &w, VK_RIGHT);
    CHECK(!dm_take(&s, &a), "Right at the maximum: nothing");
    w.fov = JPT_FOV_MIN;
    press(&s, &w, VK_LEFT);
    CHECK(!dm_take(&s, &a), "Left at the minimum: nothing");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(row_says(rows, n, DM_FIELD_OF_VIEW, "60") && fabsf(row_of(rows, n, DM_FIELD_OF_VIEW)->bar) < 1e-6f,
          "the row follows the session's value");
    w.fov = 98;
    press(&s, &w, VK_RETURN);
    CHECK(dm_take(&s, &a) && a.kind == DM_ACT_FIELD_OF_VIEW && a.value == JPT_FOV_DEFAULT, "Enter: Default 75");
    w.fov = 0;
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!dm_item_enabled(&s, &w, DM_FIELD_OF_VIEW) && row_says(rows, n, DM_FIELD_OF_VIEW, "-") &&
              row_of(rows, n, DM_FIELD_OF_VIEW)->bar < 0.0f,
          "no value: disabled");
    menu.fov = 90;
    CHECK(dm_item_enabled(&s, &menu, DM_FIELD_OF_VIEW), "it can be changed from the main menu too");
    w.fov = 90;
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(!row_of(rows, n, DM_FIELD_OF_VIEW), "not in Cheats");
}

/* Water Refraction and Enhanced Sky: the running state, switched for the session only (the renderer keeps the setting). */
static void test_effect_switches(void) {
    static const struct {
        dm_item_t item;
        dm_action_kind_t kind;
        const char* label;
    } k[] = { { DM_WATER_REFRACTION, DM_ACT_WATER_REFRACTION, "Water Refraction" },
              { DM_ENHANCED_SKY, DM_ACT_ENHANCED_SKY, "Enhanced Sky" } };
    for (int e = 0; e < 2; e++) {
        dm_state_t s;
        dm_world_t w = level_world("be.scn"), menu = main_menu_world();
        dm_action_t a;
        dm_row_t rows[DM_ROWS_MAX];
        int n, graphics = -1, ao = -1, at = -1, close = -1;
        int* state = e ? &w.enhanced_sky : &w.water_refraction;
        dm_init(&s);
        w.ambient_occlusion = 2;
        w.water_refraction = w.enhanced_sky = 1;
        dm_f5(&s, &w);
        go_to(&s, &w, k[e].item);
        CHECK(s.cursor == (int)k[e].item, "the Debug Menu reaches %s", k[e].label);
        n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
        for (int i = 0; i < n; i++) {
            if (rows[i].kind == DM_ROW_SECTION && !strcmp(rows[i].text, "Graphics")) graphics = i;
            if (rows[i].kind == DM_ROW_ITEM && rows[i].item == DM_AMBIENT_OCCLUSION) ao = i;
            if (rows[i].kind == DM_ROW_ITEM && rows[i].item == (int)k[e].item) at = i;
            if (rows[i].kind == DM_ROW_ITEM && rows[i].item == DM_CLOSE) close = i;
        }
        CHECK(graphics >= 0 && ao == graphics + 2 && at == ao + 1 + e && close == n - 1 && close > at,
              "Graphics: Field of View, Ambient Occlusion, Water Refraction, Enhanced Sky, then Close");
        CHECK(!strcmp(rows[at].text, k[e].label) && row_says(rows, n, k[e].item, "On") && rows[at].selected,
              "%s shows the state in use: On", k[e].label);
        press(&s, &w, VK_RETURN);
        CHECK(dm_take(&s, &a) && a.kind == k[e].kind && a.on == 0, "Enter: On -> Off");
        *state = 0;
        n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
        CHECK(row_says(rows, n, k[e].item, "Off"), "the row follows the running state: Off");
        press(&s, &w, VK_RETURN);
        CHECK(dm_take(&s, &a) && a.kind == k[e].kind && a.on == 1, "Enter: Off -> On");
        press(&s, &w, VK_LEFT);
        CHECK(!dm_take(&s, &a), "Left on Off: nothing");
        press(&s, &w, VK_RIGHT);
        CHECK(dm_take(&s, &a) && a.on == 1, "Right: On");
        *state = 1;
        press(&s, &w, VK_RIGHT);
        CHECK(!dm_take(&s, &a), "Right on On: nothing");
        press(&s, &w, VK_LEFT);
        CHECK(dm_take(&s, &a) && a.on == 0, "Left: Off");
        for (int i = 0; i < 6; i++) {
            *state = i & 1;
            press(&s, &w, VK_RETURN);
            CHECK(dm_take(&s, &a) && a.kind == k[e].kind && a.on == !(i & 1), "repeated switches alternate");
        }
        *state = -1;
        n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
        CHECK(!dm_item_enabled(&s, &w, k[e].item) && row_says(rows, n, k[e].item, "Unavailable"),
              "the Software renderer: Unavailable");
        menu.water_refraction = menu.enhanced_sky = 0;
        CHECK(dm_item_enabled(&s, &menu, k[e].item), "available from the main menu too");
        dm_toggle(&s, &w, DM_MENU_CHEATS);
        n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
        CHECK(!row_of(rows, n, k[e].item), "not in Cheats");
    }
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
    CHECK(calls(0x0040BF0Cu, 0x0040BCC0u) && bytes_at(0x0040BF44u, "B938E36600") && calls(0x0040BF49u, 0x00414480u) &&
              bytes_at(0x0040BF4Eu, "6A01") && calls(0x0040BF52u, 0x0040BD70u),
          "death Restart: pause, Restart Level 0x00414480 on 0x0066E338, resume(1)");
    CHECK(bytes_at(0x004165AFu, "B938E36600") && calls(0x004165B4u, 0x00414480u), "options Restart: the same call");
    CHECK(bytes_at(0x004144A4u, "8D7E0A") && calls(0x004144DCu, 0x00606630u),
          "Restart reloads the scene named at 0x0066E342 in place (bLoadScene)");
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
    int lit[3] = { 0, 0, 0 }, low[3] = { 0, 0, 0 }, amber = 0;
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
    dm_f5(&s, &w);
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
    for (int pass = 0; pass < 3; pass++) {
        f.menu = pass == 0;
        f.rows = rows;
        f.row_count = f.menu ? dm_rows(&s, &w, rows, DM_ROWS_MAX) : 0;
        f.feedback = "Checkpoint set";
        f.help = "help";
        f.coord_lines = pass < 2 ? 2 : 0;
        f.hud = pass == 2 ? &hud : NULL;
        strcpy(f.coords[0], "Level BE");
        strcpy(f.coords[1], "X 10.000");
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
                        amber += pass == 2 && r > 200 && g > 150 && b < 110;
                    }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
        }
    }
    CHECK(lit[0] > 4 * lit[1] && lit[1] > 50, "the menu and the coordinates line are drawn (%d, %d samples)", lit[0],
          lit[1]);
    CHECK(lit[2] > 100 && low[2] > 0 && low[2] < lit[2] / 2,
          "the compass, objective line and notice along the top, the world marker and trail in the picture (%d, %d below)",
          lit[2], low[2]);
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
        g.row_count = g.coord_lines = 0;
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
void objective_tests(int* checks, int* failures);
void overlay_tests(int* checks, int* failures);
void physics_watch_tests(int* checks, int* failures);
void menu_l10n_tests(int* checks, int* failures);
void menu_layout_tests(int* checks, int* failures);

int main(void) {
    test_profile_activate();
    test_f5();
    test_input_capture();
    test_navigation_and_pad();
    test_levels();
    test_level_names();
    test_restart_reload();
    test_teleport();
    test_checkpoint();
    test_coordinates();
    test_yaw();
    test_key_points();
    test_toggles();
    test_stale_state();
    test_cheats_menu();
    test_ambient_occlusion();
    test_field_of_view();
    test_effect_switches();
    cheats_tests(&g_checks, &g_failures);
    compass_tests(&g_checks, &g_failures);
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
