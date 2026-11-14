/* The F10 cheats' policy, headless: multipliers, health, the camera modes' math and when values may be in the game. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats_policy.h"

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
    ch_init(&s);
    CHECK(s.speed == CH_MULTIPLIER_DEFAULT && s.jump == CH_MULTIPLIER_DEFAULT && !s.super_health &&
              s.camera == CH_CAMERA_NORMAL && !ch_any(&s),
          "every cheat starts off");
    s.speed = 3;
    s.jump = 4;
    s.super_health = 1;
    s.camera = CH_CAMERA_FREE;
    ch_set_level(&s, "BE");
    CHECK(ch_any(&s) && !ch_level_seen(&s, DM_PLACE_LEVEL, "BE") && s.speed == 3, "the same level keeps them");
    CHECK(!ch_level_seen(&s, DM_PLACE_NONE, "") && s.camera == CH_CAMERA_FREE, "a dialog or a load keeps them");
    CHECK(ch_level_seen(&s, DM_PLACE_LEVEL, "IT") && !ch_any(&s) && s.speed == CH_MULTIPLIER_DEFAULT &&
              s.jump == CH_MULTIPLIER_DEFAULT && s.camera == CH_CAMERA_NORMAL && !s.super_health,
          "another level resets speed, jump, Super Health and the camera");
    s.camera = CH_CAMERA_THIRD;
    ch_set_level(&s, "IT");
    CHECK(ch_level_seen(&s, DM_PLACE_MAIN_MENU, "") && s.camera == CH_CAMERA_NORMAL, "the main menu resets them");
    CHECK(!ch_level_seen(&s, DM_PLACE_MAIN_MENU, ""), "nothing to reset: nothing reported");
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
    ch_third_person_relative(&p);
    CHECK(p.pos[1] < -1.0f && p.pos[2] > 0.0f && p.pos[0] == 0.0f && p.rot[0] == 1.0f && !p.rot[1] && !p.rot[3],
          "Third Person: behind and above the head, looking the head's way");
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

void cheats_tests(int* checks, int* failures) {
    test_multipliers();
    test_speed_table();
    test_state_and_reset();
    test_when_applied();
    test_health();
    test_cameras();
    *checks += g_checks;
    *failures += g_failures;
}
