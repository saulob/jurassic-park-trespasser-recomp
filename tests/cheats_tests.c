/* The F10 cheats' policy, headless: multipliers, health, the camera modes' math and when values may be in the game. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats_policy.h"
#include "jpt_cheats.h"
#include "jpt_config.h"

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

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_multipliers(void) {
    CHECK(ch_multiplier(CH_MULTIPLIER_DEFAULT) == 1.0f && !strcmp(ch_multiplier_text(CH_MULTIPLIER_DEFAULT), "1.0x"),
          "the default is 1.0x");
    CHECK(ch_multiplier(0) == 0.5f && ch_multiplier(CH_MULTIPLIER_COUNT - 1) == 5.0f, "0.5x to 5.0x");
    CHECK(ch_multiplier_clamp(-3) == 0 && ch_multiplier_clamp(99) == CH_MULTIPLIER_COUNT - 1, "indices clamp");
    CHECK(ch_jump_speed_scale(CH_MULTIPLIER_DEFAULT) == 1.0f, "1.0x leaves the jump exactly as it was");
    for (int i = 0; i < CH_MULTIPLIER_COUNT; i++) {
        float v = ch_jump_speed_scale(i);
        CHECK(near(v * v, ch_multiplier(i)), "the jump speed scale squared is the height multiplier (%s)",
              ch_multiplier_text(i));
    }
}

static void test_speed_table(void) {
    static const float want[CH_SPEED_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.5f, 10.0f };
    static const char* const text[CH_SPEED_COUNT] = { "0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x", "7.5x", "10.0x" };
    CHECK(CH_SPEED_COUNT == 8 && ch_speed(CH_MULTIPLIER_DEFAULT) == 1.0f, "eight speeds, 1.0x the default");
    for (int i = 0; i < CH_SPEED_COUNT; i++) {
        CHECK(ch_speed(i) == want[i] && !strcmp(ch_speed_text(i), text[i]), "Movement Speed step %d is %s", i, text[i]);
        CHECK(isfinite(ch_speed(i)) && ch_speed(i) > 0.0f && (i == 0 || ch_speed(i) > ch_speed(i - 1)), "increasing and finite");
    }
    CHECK(ch_speed(CH_SPEED_COUNT - 1) == 10.0f && ch_speed(CH_SPEED_COUNT + 5) == 10.0f && ch_speed(99) == 10.0f,
          "10.0x is the most, whatever the index");
    CHECK(ch_speed_clamp(-1) == 0 && ch_speed_clamp(99) == CH_SPEED_COUNT - 1, "speed indices clamp");
    CHECK(CH_MULTIPLIER_COUNT == 6 && ch_multiplier(CH_MULTIPLIER_COUNT - 1) == 5.0f, "Jump Height keeps its 0.5x to 5.0x");
    for (int i = 0; i < CH_SPEED_COUNT; i++) {
        char line[64];
        CHECK(ch_format_change(line, sizeof line, DM_ACT_SPEED, 0, i) && strstr(line, text[i]), "the change line names %s", text[i]);
    }
}

static void test_state_and_reset(void) {
    ch_state_t s;
    jpt_cheat_session_t session;
    ch_init(&s);
    CHECK(s.speed == CH_MULTIPLIER_DEFAULT && s.jump == CH_MULTIPLIER_DEFAULT && !s.super_health &&
              s.camera == CH_CAMERA_NORMAL && !ch_any(&s),
          "every cheat starts off");
    s.speed = 3;
    s.jump = 4;
    s.super_health = 1;
    s.camera = CH_CAMERA_FREE;
    ch_set_level(&s, "BE");
    CHECK(ch_any(&s) && !ch_level_seen(&s, DM_PLACE_LEVEL, "BE") && s.speed == 3 && s.camera == CH_CAMERA_FREE,
          "the same level keeps them");
    CHECK(!ch_level_seen(&s, DM_PLACE_NONE, "") && s.camera == CH_CAMERA_FREE, "a dialog or a load keeps them");
    CHECK(ch_level_seen(&s, DM_PLACE_LEVEL, "IT") && s.camera == CH_CAMERA_NORMAL && s.speed == 3 && s.jump == 4 &&
              s.super_health,
          "another level takes the free camera back; the session's other cheats stay");
    s.camera = CH_CAMERA_FREE;
    CHECK(!ch_level_seen(&s, DM_PLACE_LEVEL, "IT") && s.camera == CH_CAMERA_FREE, "the level is noted: no second report");
    CHECK(ch_level_seen(&s, DM_PLACE_MAIN_MENU, "") && s.camera == CH_CAMERA_NORMAL && s.speed == 3,
          "the main menu takes the free camera back too");
    CHECK(!ch_level_seen(&s, DM_PLACE_MAIN_MENU, ""), "nothing to leave: nothing reported");
    jpt_cheat_session_start(&session, NULL);
    jpt_cheat_set(&session, JPT_CHEAT_SPEED, 6);
    jpt_cheat_set(&session, JPT_CHEAT_SUPER_HEALTH, 1);
    ch_from_session(&s, &session);
    CHECK(s.speed == 6 && s.jump == CH_MULTIPLIER_DEFAULT && s.super_health && s.camera == CH_CAMERA_NORMAL,
          "the state follows the session");
}

static void test_when_applied(void) {
    CHECK(ch_should_apply(DM_PLACE_LEVEL, 0, 0, 1), "a running level holds the cheats");
    CHECK(!ch_should_apply(DM_PLACE_NONE, 0, 0, 1), "a menu or dialog over the level does not: saves never keep them");
    CHECK(!ch_should_apply(DM_PLACE_LEVEL, 0, 1, 1), "nor a save the game is about to write");
    CHECK(!ch_should_apply(DM_PLACE_LEVEL, 1, 0, 1), "nor a load the game is about to run");
    CHECK(!ch_should_apply(DM_PLACE_MAIN_MENU, 0, 0, 1) && !ch_should_apply(DM_PLACE_LEVEL, 0, 0, 0),
          "nor the main menu or a level without its player");
}

static void test_health(void) {
    float hp = 350.0f, max = 400.0f, out = 0.0f;
    CHECK(ch_super_max(100.0f) == 100.0f * CH_SUPER_HEALTH_FACTOR && ch_super_max(100.0f) > 100.0f,
          "Super Health is above the normal maximum");
    ch_super_off(100.0f, &hp, &max);
    CHECK(max == 100.0f && hp == 100.0f, "Super Health off clamps to the normal maximum");
    hp = 40.0f;
    max = 400.0f;
    ch_super_off(100.0f, &hp, &max);
    CHECK(max == 100.0f && hp == 40.0f, "and never takes health below it");
    hp = 0.5f;
    ch_super_off(100.0f, &hp, &max);
    CHECK(hp == 0.5f, "nor kills");
    CHECK(ch_heal(25.0f, 100.0f, &out) && out == 100.0f, "Heal fills to the maximum");
    out = -1.0f;
    CHECK(!ch_heal(0.0f, 100.0f, &out) && !ch_heal(-12.0f, 100.0f, &out) && out == -1.0f, "the dead stay dead");
    CHECK(!ch_heal(50.0f, 0.0f, &out), "no maximum: no heal");
}

/* 0x004CD090's rotation of a vector by a CRotate3 (scalar first). */
static void rotate(const float q[4], const float v[3], float out[3]) {
    float a = q[0], b = q[1], c = q[2], d = q[3], k = a * a - 0.5f;
    out[0] = 2.0f * ((b * b + k) * v[0] + (c * b - d * a) * v[1] + (c * a + d * b) * v[2]);
    out[1] = 2.0f * ((d * a + b * c) * v[0] + (c * c + k) * v[1] + (d * c - b * a) * v[2]);
    out[2] = 2.0f * ((d * b - c * a) * v[0] + (b * a + c * d) * v[1] + (d * d + k) * v[2]);
}

static void test_cameras(void) {
    static const float forward[3] = { 0.0f, 1.0f, 0.0f };
    dm_placement_t p, back;
    ch_freecam_t c, again;
    ch_freecam_input_t in;
    float f[3];
    memset(&c, 0, sizeof c);
    c.pos[0] = 1.0f;
    c.pos[1] = 2.0f;
    c.pos[2] = 3.0f;
    c.yaw = 0.7f;
    c.pitch = -0.3f;
    ch_freecam_placement(&c, &p);
    rotate(p.rot, forward, f);
    CHECK(near(f[2], sinf(-0.3f)) && near(atan2f(-f[0], f[1]), 0.7f), "the camera looks where yaw and pitch say");
    CHECK(near(dm_yaw(p.rot), 0.7f), "its yaw is the one the coordinates show");
    ch_freecam_from(&p, &again);
    CHECK(near(again.yaw, 0.7f) && near(again.pitch, -0.3f) && again.pos[2] == 3.0f, "a placement reads back");
    memset(&c, 0, sizeof c);
    memset(&in, 0, sizeof in);
    in.move[1] = 1.0f;
    ch_freecam_step(&c, &in, 0.5f);
    CHECK(near(c.pos[1], CH_FREE_CAMERA_SPEED * 0.5f) && near(c.pos[0], 0.0f) && c.pos[2] == 0.0f,
          "forward moves along the facing axis");
    memset(&c, 0, sizeof c);
    c.yaw = 1.5707963f;
    ch_freecam_step(&c, &in, 1.0f);
    CHECK(near(c.pos[0], -CH_FREE_CAMERA_SPEED) && near(c.pos[1], 0.0f), "turned left, forward is -X");
    memset(&c, 0, sizeof c);
    in.move[0] = 1.0f;
    in.move[1] = 0.0f;
    ch_freecam_step(&c, &in, 1.0f);
    CHECK(near(c.pos[0], CH_FREE_CAMERA_SPEED) && near(c.pos[1], 0.0f), "right strafes to +X facing +Y");
    memset(&c, 0, sizeof c);
    in.move[0] = 1.0f;
    in.move[1] = 1.0f;
    ch_freecam_step(&c, &in, 1.0f);
    CHECK(near(sqrtf(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1]), CH_FREE_CAMERA_SPEED), "diagonals are not faster");
    memset(&c, 0, sizeof c);
    memset(&in, 0, sizeof in);
    in.up = 1;
    in.fast = 1;
    ch_freecam_step(&c, &in, 1.0f);
    CHECK(near(c.pos[2], CH_FREE_CAMERA_SPEED * CH_FREE_CAMERA_FAST), "up, fast");
    in.up = 0;
    in.down = 1;
    in.fast = 0;
    ch_freecam_step(&c, &in, 1.0f);
    CHECK(near(c.pos[2], CH_FREE_CAMERA_SPEED * (CH_FREE_CAMERA_FAST - 1.0f)), "down");
    memset(&c, 0, sizeof c);
    memset(&in, 0, sizeof in);
    in.rotate[0] = 0.2f;
    in.rotate[1] = 5.0f;
    ch_freecam_step(&c, &in, 0.0f);
    CHECK(near(c.yaw, -0.2f) && c.pitch == CH_FREE_CAMERA_PITCH_MAX, "mouse right turns right; pitch is limited");
    CHECK(c.pos[0] == 0.0f && c.pos[1] == 0.0f && c.pos[2] == 0.0f, "no time, no movement");
    ch_freecam_placement(&c, &back);
    CHECK(near(back.rot[0] * back.rot[0] + back.rot[1] * back.rot[1] + back.rot[2] * back.rot[2] +
                   back.rot[3] * back.rot[3],
               1.0f),
          "the rotation is a unit quaternion");
}

/* The saved launcher values, as the host reads them. */
static void saved_from(int saved[JPT_CHEAT_COUNT], const char* const* settings) {
    jpt_config_t c;
    jpt_config_defaults(&c);
    for (int i = 0; settings && settings[i]; i += 2) jpt_config_apply(&c, settings[i], settings[i + 1]);
    memcpy(saved, c.cheat, sizeof c.cheat);
}

static void test_central_list(void) {
    static const struct {
        jpt_cheat_t id;
        const char* label;
    } order[JPT_CHEAT_COUNT] = {
        { JPT_CHEAT_SPEED, "Movement Speed" },         { JPT_CHEAT_JUMP, "Jump Height" },
        { JPT_CHEAT_INVULNERABLE, "Invulnerability" }, { JPT_CHEAT_AMMO, "Infinite Ammo" },
        { JPT_CHEAT_FREEZE_AI, "Freeze Dinosaurs" },   { JPT_CHEAT_HEAL, "Heal" },
        { JPT_CHEAT_SUPER_HEALTH, "Super Health" },    { JPT_CHEAT_TELEPORT, "Teleport" },
        { JPT_CHEAT_LEVEL_SELECT, "Level Select" },    { JPT_CHEAT_FREE_CAMERA, "Fly / Free Camera" },
    };
    int saved_count = 0;
    for (int i = 0; i < JPT_CHEAT_COUNT; i++) {
        const jpt_cheat_info_t* info = &jpt_cheats[i];
        CHECK(info->id == (jpt_cheat_t)i && info->id == order[i].id && !strcmp(info->label, order[i].label),
              "list entry %d is %s, F10's order", i, order[i].label);
        CHECK(info->help && info->help[0], "%s has a help text", info->label);
        CHECK(info->kind != JPT_CHEAT_ACTION || (!info->key && !jpt_cheat_is_saved(info)), "%s: an action is never saved",
              info->label);
        CHECK(jpt_cheat_is_saved(info) == (info->key != NULL), "%s: saved when it has a key", info->label);
        saved_count += jpt_cheat_is_saved(info);
        for (int j = 0; j < i; j++) {
            CHECK(strcmp(info->label, jpt_cheats[j].label), "label %s is not repeated", info->label);
            CHECK(!info->key || !jpt_cheats[j].key || strcmp(info->key, jpt_cheats[j].key), "key %s is not repeated",
                  info->key);
        }
        if (info->kind == JPT_CHEAT_CHOICE) {
            CHECK(info->choice_count > 1 && info->choices, "%s lists its choices", info->label);
            CHECK(jpt_cheat_valid_value(info, info->default_value), "%s: the default is one of them", info->label);
            for (int c = 0; c < info->choice_count; c++) {
                int parsed = -1;
                CHECK(jpt_cheat_parse(info, info->choices[c].saved, &parsed) && parsed == c, "%s: \"%s\" is choice %d",
                      info->label, info->choices[c].saved, c);
                for (int d = 0; d < c; d++)
                    CHECK(strcmp(info->choices[c].saved, info->choices[d].saved) &&
                              strcmp(info->choices[c].label, info->choices[d].label) &&
                              info->choices[c].value != info->choices[d].value,
                          "%s: choices %d and %d differ", info->label, c, d);
            }
        } else if (info->kind == JPT_CHEAT_TOGGLE) {
            CHECK(info->default_value == 0 && !info->choices, "%s starts Off", info->label);
        }
    }
    CHECK(saved_count == JPT_CHEAT_PERSISTENT_COUNT, "six cheats are saved");
    CHECK(jpt_cheats[JPT_CHEAT_FREE_CAMERA].key == NULL && !jpt_cheat_is_saved(&jpt_cheats[JPT_CHEAT_FREE_CAMERA]),
          "Fly / Free Camera is a mode of the running level: F10 only");
}

static void test_choice_lists(void) {
    static const char* const speed[] = { "0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x", "7.5x", "10.0x" };
    static const char* const jump[] = { "0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x" };
    const jpt_cheat_info_t* sp = &jpt_cheats[JPT_CHEAT_SPEED];
    const jpt_cheat_info_t* ju = &jpt_cheats[JPT_CHEAT_JUMP];
    CHECK(sp->choice_count == CH_SPEED_COUNT && ju->choice_count == CH_MULTIPLIER_COUNT, "F10 uses the lists' sizes");
    for (int i = 0; i < CH_SPEED_COUNT; i++)
        CHECK(!strcmp(sp->choices[i].label, speed[i]) && ch_speed(i) == sp->choices[i].value &&
                  !strcmp(ch_speed_text(i), sp->choices[i].label),
              "Movement Speed %s is the same in F10 and the launcher", speed[i]);
    for (int i = 0; i < CH_MULTIPLIER_COUNT; i++)
        CHECK(!strcmp(ju->choices[i].label, jump[i]) && ch_multiplier(i) == ju->choices[i].value &&
                  !strcmp(ch_multiplier_text(i), ju->choices[i].label),
              "Jump Height %s is the same in F10 and the launcher", jump[i]);
    CHECK(sp->default_value == CH_MULTIPLIER_DEFAULT && ju->default_value == CH_MULTIPLIER_DEFAULT &&
              ch_speed(sp->default_value) == 1.0f && ch_multiplier(ju->default_value) == 1.0f,
          "both start at 1.0x, the original");
}

static void test_f10_uses_the_list(void) {
    dm_state_t s;
    dm_world_t w;
    dm_row_t rows[DM_ROWS_MAX];
    int n, at = 0, shown = 0;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_LEVEL;
    snprintf(w.scene, sizeof w.scene, "be.scn");
    snprintf(w.level, sizeof w.level, "BE");
    w.player = 1;
    w.ai = 0;
    w.hp = w.hp_max = 100.0f;
    w.camera_head = 1;
    w.speed = w.jump = CH_MULTIPLIER_DEFAULT;
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind != DM_ROW_ITEM) continue;
        if (at < JPT_CHEAT_COUNT)
            CHECK(!strcmp(rows[i].text, jpt_cheats[at].label), "F10 row %d is %s, not %s", at, jpt_cheats[at].label,
                  rows[i].text);
        else
            CHECK(rows[i].item == DM_CLOSE, "Close ends the list");
        at++;
    }
    CHECK(at == JPT_CHEAT_COUNT + 1, "F10 lists every cheat of the list and Close: %d rows", at);
    for (int i = 0; i < n; i++) {
        CHECK(rows[i].item != DM_SHOW_COORDS, "Show Coordinates is not in Cheats");
        CHECK(strcmp(rows[i].text, "Show Coordinates") && strcmp(rows[i].text, "Third Person"),
              "no Show Coordinates or Third Person row: %s", rows[i].text);
    }
    for (int d = 0; d < DM_ITEM_COUNT; d++) CHECK(strcmp(dm_item_label(DM_MENU_CHEATS, (dm_item_t)d), "Third Person"), "no item");
    dm_close(&s);
    dm_toggle(&s, &w, DM_MENU_DEBUG);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) shown |= rows[i].item == DM_SHOW_COORDS && rows[i].kind == DM_ROW_ITEM;
    CHECK(shown, "Show Coordinates stays in the Debug Menu");
}

static void test_saved_and_session(void) {
    static const char* const on[] = { JPT_KEY_CHEAT_MOVEMENT_SPEED, "5", JPT_KEY_CHEAT_JUMP_HEIGHT, "2",
                                      JPT_KEY_CHEAT_INVULNERABILITY, "1", JPT_KEY_CHEAT_INFINITE_AMMO, "on",
                                      JPT_KEY_CHEAT_FREEZE_DINOSAURS, "0", JPT_KEY_CHEAT_SUPER_HEALTH, "1", NULL };
    int saved[JPT_CHEAT_COUNT];
    jpt_cheat_session_t s;
    char line[256];
    saved_from(saved, NULL);
    jpt_cheat_session_start(&s, saved);
    for (int i = 0; i < JPT_CHEAT_COUNT; i++)
        CHECK(jpt_cheat_effective(&s, (jpt_cheat_t)i) == jpt_cheats[i].default_value, "%s: no cheat is on by default",
              jpt_cheats[i].label);
    CHECK(jpt_cheat_format_saved(line, sizeof line, &s) &&
              !strcmp(line, "Movement Speed 1.0x, Jump Height 1.0x, Invulnerability Off, Infinite Ammo Off, Freeze "
                            "Dinosaurs Off, Super Health Off"),
          "the log line: %s", line);

    saved_from(saved, on);
    jpt_cheat_session_start(&s, saved);
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_SPEED) == 5 && jpt_cheat_effective(&s, JPT_CHEAT_JUMP) == 3,
          "saved choices are in effect from the start");
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_INVULNERABLE) == 1 && jpt_cheat_effective(&s, JPT_CHEAT_AMMO) == 1 &&
              jpt_cheat_effective(&s, JPT_CHEAT_SUPER_HEALTH) == 1,
          "saved On shows On");
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_FREEZE_AI) == 0, "saved Off shows Off");
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_FREE_CAMERA) == 0, "the free camera never starts On");
    CHECK(jpt_cheat_format_saved(line, sizeof line, &s) && strstr(line, "Movement Speed 5.0x") &&
              strstr(line, "Jump Height 2.0x") && strstr(line, "Invulnerability On") && strstr(line, "Freeze Dinosaurs Off"),
          "the log line: %s", line);

    CHECK(jpt_cheat_set(&s, JPT_CHEAT_INVULNERABLE, 0) == 0 && jpt_cheat_effective(&s, JPT_CHEAT_INVULNERABLE) == 0 &&
              s.saved[JPT_CHEAT_INVULNERABLE] == 1,
          "F10 On -> Off: this session only, the saved value stays On");
    CHECK(jpt_cheat_set(&s, JPT_CHEAT_FREEZE_AI, 1) == 1 && jpt_cheat_effective(&s, JPT_CHEAT_FREEZE_AI) == 1 &&
              s.saved[JPT_CHEAT_FREEZE_AI] == 0,
          "F10 Off -> On: this session only, the saved value stays Off");
    CHECK(jpt_cheat_set(&s, JPT_CHEAT_INVULNERABLE, 1) == 1 && s.changed[JPT_CHEAT_INVULNERABLE] < 0,
          "back to the saved value: no difference kept");
    CHECK(jpt_cheat_cycle(&s, JPT_CHEAT_SPEED) == 6 && s.saved[JPT_CHEAT_SPEED] == 5 && s.changed[JPT_CHEAT_SPEED] == 6,
          "a choice steps for the session only");
    for (int i = 0; i < CH_SPEED_COUNT - 1; i++) jpt_cheat_cycle(&s, JPT_CHEAT_SPEED);
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_SPEED) == 5 && s.changed[JPT_CHEAT_SPEED] < 0,
          "cycled all the way round: it follows the saved value again");
    CHECK(jpt_cheat_set(&s, JPT_CHEAT_SPEED, 99) == 5 && jpt_cheat_set(&s, JPT_CHEAT_SPEED, -1) == 5,
          "a value the list does not have changes nothing");
    jpt_cheat_set(&s, JPT_CHEAT_INVULNERABLE, 0);
    jpt_cheat_set(&s, JPT_CHEAT_FREEZE_AI, 1);
    jpt_cheat_set(&s, JPT_CHEAT_FREE_CAMERA, 1);
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_FREE_CAMERA) == 1, "the free camera is a session change");
    jpt_cheat_session_start(&s, saved);
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_INVULNERABLE) == 1 && jpt_cheat_effective(&s, JPT_CHEAT_FREEZE_AI) == 0 &&
              jpt_cheat_effective(&s, JPT_CHEAT_FREE_CAMERA) == 0 && jpt_cheat_effective(&s, JPT_CHEAT_SPEED) == 5,
          "the next start: the saved values win again");
    jpt_cheat_set(&s, JPT_CHEAT_JUMP, 0);
    jpt_cheat_clear_changes(&s);
    CHECK(jpt_cheat_effective(&s, JPT_CHEAT_JUMP) == 3, "clearing the changes follows the saved values");
}

static void test_saved_validation(void) {
    static const char* const bad[] = { JPT_KEY_CHEAT_MOVEMENT_SPEED, "4", JPT_KEY_CHEAT_JUMP_HEIGHT, "fast",
                                       JPT_KEY_CHEAT_INVULNERABILITY, "banana", JPT_KEY_CHEAT_INFINITE_AMMO, "2",
                                       JPT_KEY_CHEAT_SUPER_HEALTH, "", JPT_KEY_CHEAT_FREEZE_DINOSAURS, "nan", NULL };
    static const char* const spelled[] = { JPT_KEY_CHEAT_MOVEMENT_SPEED, "2.0", JPT_KEY_CHEAT_JUMP_HEIGHT, "0.50",
                                           JPT_KEY_CHEAT_INVULNERABILITY, "YES", NULL };
    static const char* const unsaved[] = { "cheat_heal", "cheat_teleport", "cheat_level_select", "cheat_free_camera",
                                           "cheat_fly_free_camera", "cheat_show_coordinates", "cheat_third_person" };
    int saved[JPT_CHEAT_COUNT], wild[JPT_CHEAT_COUNT];
    jpt_config_t c;
    jpt_cheat_session_t s;
    saved_from(saved, bad);
    for (int i = 0; i < JPT_CHEAT_COUNT; i++)
        CHECK(saved[i] == jpt_cheats[i].default_value, "an invalid or empty value is the default (%s)", jpt_cheats[i].label);
    saved_from(saved, spelled);
    CHECK(saved[JPT_CHEAT_SPEED] == 3 && saved[JPT_CHEAT_JUMP] == 0 && saved[JPT_CHEAT_INVULNERABLE] == 1,
          "another spelling of a listed value is that value");
    jpt_config_defaults(&c);
    for (size_t i = 0; i < sizeof unsaved / sizeof unsaved[0]; i++)
        CHECK(!jpt_config_apply(&c, unsaved[i], "1") && !jpt_default_value(unsaved[i]), "%s is not a setting", unsaved[i]);
    for (int i = 0; i < JPT_SETTING_COUNT; i++) {
        const jpt_cheat_info_t* info = NULL;
        for (int k = 0; k < JPT_CHEAT_COUNT; k++)
            if (jpt_cheats[k].key && !strcmp(jpt_cheats[k].key, jpt_settings[i].key)) info = &jpt_cheats[k];
        if (info)
            CHECK(!strcmp(jpt_settings[i].default_value, jpt_cheat_saved_text(info, info->default_value)),
                  "%s: the settings default is the list's", info->key);
        CHECK(strncmp(jpt_settings[i].key, "cheat_", 6) || info, "%s is a cheat of the list", jpt_settings[i].key);
    }
    for (int k = 0; k < JPT_CHEAT_COUNT; k++)
        if (jpt_cheat_is_saved(&jpt_cheats[k]))
            CHECK(jpt_default_value(jpt_cheats[k].key) != NULL, "%s has a settings default", jpt_cheats[k].key);
    for (int i = 0; i < JPT_CHEAT_COUNT; i++) wild[i] = 77;
    jpt_cheat_session_start(&s, wild);
    for (int i = 0; i < JPT_CHEAT_COUNT; i++)
        CHECK(jpt_cheat_effective(&s, (jpt_cheat_t)i) == jpt_cheats[i].default_value, "a value outside the list: default");
}

void cheats_tests(int* checks, int* failures) {
    test_multipliers();
    test_speed_table();
    test_state_and_reset();
    test_when_applied();
    test_health();
    test_cameras();
    test_central_list();
    test_choice_lists();
    test_f10_uses_the_list();
    test_saved_and_session();
    test_saved_validation();
    *checks += g_checks;
    *failures += g_failures;
}
