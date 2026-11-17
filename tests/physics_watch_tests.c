/* DEBUG_SAULO: the temporary player telemetry, headless: the stuck detector, sanity checks, snapshot lines, the cheat
 * log lines and the speed/jump combinations it reports. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats_policy.h"
#include "jpt_gamepad.h"
#include "physics_watch_policy.h"

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

/* Ticks of 1/60 s; returns how many fired. pos advances by speed m/s along x. */
static int run(pw_stuck_t* s, double* t, float* x, int ticks, int watching, float move, float speed) {
    int fired = 0;
    for (int i = 0; i < ticks; i++) {
        pw_tick_t k;
        float seconds, moved;
        *t += 1.0 / 60.0;
        *x += speed / 60.0f;
        memset(&k, 0, sizeof k);
        k.t = *t;
        k.watching = watching;
        k.move = move;
        k.pos[0] = *x;
        k.pos[1] = 50.0f;
        fired += pw_stuck_step(s, &k, &seconds, &moved);
    }
    return fired;
}

static void test_stuck(void) {
    pw_stuck_t s;
    double t = 0.0;
    float x = 0.0f;
    pw_stuck_init(&s);
    CHECK(!run(&s, &t, &x, 120, 1, 1.0f, 3.0f), "moving normally: never");
    CHECK(!run(&s, &t, &x, 120, 1, 0.0f, 0.0f), "standing still without input: never");
    CHECK(!run(&s, &t, &x, 120, 0, 1.0f, 0.0f), "an interface up, the free camera or Anne dead (not watching): never");
    CHECK(!run(&s, &t, &x, 25, 1, 1.0f, 0.0f), "input without movement for less than 0.5 s: not yet");
    CHECK(run(&s, &t, &x, 10, 1, 1.0f, 0.0f) == 1, "input without movement for 0.5 s: once");
    CHECK(!run(&s, &t, &x, 240, 1, 1.0f, 0.0f), "still stuck: not again");
    CHECK(!run(&s, &t, &x, 10, 1, 1.0f, 3.0f), "moving again, less than a metre: not armed yet");
    CHECK(!run(&s, &t, &x, 30, 1, 1.0f, 3.0f), "moving again past a metre: armed");
    CHECK(run(&s, &t, &x, 40, 1, 1.0f, 0.0f) == 1, "stuck again: once more");
    CHECK(!run(&s, &t, &x, 120, 1, 0.1f, 0.0f), "a stick barely pushed is not trying to move");
}

static pw_snapshot_t sample(void) {
    pw_snapshot_t s;
    memset(&s, 0, sizeof s);
    s.reason = "PLAYER STUCK";
    s.t = 82.5;
    snprintf(s.level, sizeof s.level, "BE");
    s.hint = 101;
    s.speed = "5.0x";
    s.jump = "2.0x";
    s.camera = "Normal";
    s.speed_real = 5.0f;
    s.player_ok = 1;
    s.player_pos[0] = -1790.5f;
    s.player_pos[1] = -1001.25f;
    s.player_pos[2] = 14.0f;
    s.pelvis = 0;
    s.pelvis_pos[0] = -1790.5f;
    s.pelvis_pos[1] = -1001.25f;
    s.pelvis_pos[2] = 13.0f;
    s.pelvis_vel[0] = 3.0f;
    s.pelvis_vel[1] = 4.0f;
    s.alive = 1.0f;
    s.hp = 100.0f;
    s.hp_max = 100.0f;
    s.hand_pos[0] = -1790.0f;
    s.hand_pos[1] = -1001.25f;
    s.hand_pos[2] = 13.0f;
    s.move_y = 1.0f;
    s.state = JPT_CMD_HAND | JPT_CMD_RUN;
    s.lt = s.hand_seen = 1;
    s.stick_x = 0.5f;
    s.camera_ok = 1;
    s.camera_pos[0] = -1790.5f;
    s.camera_pos[1] = -1001.25f;
    s.camera_pos[2] = 14.6f;
    s.stuck_seconds = 0.52f;
    s.stuck_moved = 0.01f;
    return s;
}

static int has_line(const pw_snapshot_t* s, const char* text) {
    char line[PW_LINE_MAX];
    for (int i = 0; i < PW_LINES; i++)
        if (pw_format(s, i, line, sizeof line) && strstr(line, text)) return 1;
    return 0;
}

static void test_snapshot(void) {
    pw_snapshot_t s = sample();
    CHECK(has_line(&s, "PLAYER STUCK at 82.500 s, level BE, hint 101"), "reason, time, level, hint");
    CHECK(has_line(&s, "location BE X=-1790.500 Y=-1001.250 Z=14.000"), "exact position");
    CHECK(has_line(&s, "Movement Speed 5.0x, Jump Height 2.0x, camera Normal; player rSpeedMultiplier 5.000"),
          "multipliers and the guest's own value");
    CHECK(has_line(&s, "(5.00 m/s)"), "pelvis speed");
    CHECK(has_line(&s, "hand 1, wrist 0, arm 0") && has_line(&s, "LT 1, Move Hand seen by the game 1"), "hand input");
    CHECK(has_line(&s, "pelvis->hand 0.500"), "hand distance");
    CHECK(has_line(&s, "camera Normal at"), "camera");
    CHECK(has_line(&s, "trying to move for 0.52 s, moved 0.010 m"), "stuck figures");
    CHECK(!pw_invalid(&s) && !has_line(&s, "out of range"), "a sane state");
    s.stuck_seconds = 0.0f;
    CHECK(!has_line(&s, "stuck:"), "a manual snapshot has no stuck line");
    CHECK(!has_line(&s, "max relative drift"), "no drift line before the parts were read");
    s.drift_ok = 1;
    s.drift[PW_PART_HAND] = 0.612f;
    s.drift[PW_PART_RIGHT_FOOT] = 0.905f;
    CHECK(has_line(&s, "max relative drift (from the pelvis, last 1-2 s): hand=0.612 m, right_foot=0.905 m, "
                       "left_foot=unused"),
          "drift line, the human's unused left foot named");
    s.left_foot_moves = 1;
    s.drift[PW_PART_LEFT_FOOT] = 0.4f;
    CHECK(has_line(&s, "left_foot=0.400 m"), "a moving left foot measured");
    s.pelvis = -1;
    CHECK(has_line(&s, "no human pelvis"), "no pelvis said plainly");
}

static void test_sanity(void) {
    pw_snapshot_t s = sample();
    s.pelvis_vel[2] = NAN;
    CHECK(pw_invalid(&s) && !strcmp(pw_invalid(&s), "pelvis velocity"), "NaN");
    s = sample();
    s.hand_pos[1] = INFINITY;
    CHECK(pw_invalid(&s) && !strcmp(pw_invalid(&s), "hand position"), "Inf");
    s = sample();
    s.player_pos[0] = 3.0e6f;
    CHECK(pw_invalid(&s) && !strcmp(pw_invalid(&s), "player position") && has_line(&s, "out of range: player position"),
          "an absurd position");
    s = sample();
    s.pelvis_vel[0] = 500.0f;
    CHECK(pw_invalid(&s) && !strcmp(pw_invalid(&s), "pelvis velocity"), "an absurd speed");
    s = sample();
    s.speed_real = 0.0f;
    CHECK(pw_invalid(&s) && !strcmp(pw_invalid(&s), "rSpeedMultiplier"), "a zero multiplier");
    s.speed_real = -1.0f;
    CHECK(!pw_invalid(&s), "an unknown multiplier is not an error");
}

static void test_cheat_lines(void) {
    char line[160];
    ch_state_t c;
    CHECK(ch_format_change(line, sizeof line, DM_ACT_SPEED, 1, 5) && !strcmp(line, "Movement Speed = 5.0x"), "%s", line);
    CHECK(ch_format_change(line, sizeof line, DM_ACT_JUMP, 1, 3) && !strcmp(line, "Jump Height = 2.0x"), "%s", line);
    CHECK(ch_format_change(line, sizeof line, DM_ACT_INVULNERABLE, 1, 0) && !strcmp(line, "Invulnerability = On"), "%s",
          line);
    CHECK(ch_format_change(line, sizeof line, DM_ACT_AMMO, 0, 0) && !strcmp(line, "Infinite Ammo = Off"), "%s", line);
    CHECK(ch_format_change(line, sizeof line, DM_ACT_CAMERA, 1, CH_CAMERA_FREE) && !strcmp(line, "Camera = Free Camera"),
          "%s", line);
    CHECK(ch_format_change(line, sizeof line, DM_ACT_FREEZE_AI, 1, 0) && !strcmp(line, "Freeze Dinosaurs = On"), "%s", line);
    CHECK(!ch_format_change(line, sizeof line, DM_ACT_TELEPORT, 1, 0), "not a cheat");
    ch_init(&c);
    c.speed = 5;
    c.jump = 3;
    CHECK(ch_format_active(line, sizeof line, &c, 0, -1) &&
              !strcmp(line, "movement=5.0x jump=2.0x super health=Off stealth=Off noclip=Off camera=Normal invulnerability=Off infinite ammo=?"),
          "%s", line);
}

/* The four the user plays with: what the game is given (the walk request x rSpeedMultiplier, the jump's start speed). */
static void test_combinations(void) {
    static const struct {
        int speed, jump;
        float walk, jump_scale;
    } combos[] = { { 1, 1, 1.0f, 1.0f },  { 5, 1, 5.0f, 1.0f },          { 1, 3, 1.0f, 1.41421356f },
                   { 5, 3, 5.0f, 1.41421356f }, { 6, 3, 7.5f, 1.41421356f }, { 7, 5, 10.0f, 2.23606798f } };
    for (int i = 0; i < 6; i++) {
        float walk = ch_speed(combos[i].speed), scale = ch_jump_speed_scale(combos[i].jump);
        CHECK(fabsf(walk - combos[i].walk) < 1e-6f && fabsf(scale - combos[i].jump_scale) < 1e-5f,
              "speed %s jump %s: walk x%.2f, jump start speed x%.4f (height x%.2f)", ch_speed_text(combos[i].speed),
              ch_multiplier_text(combos[i].jump), walk, scale, scale * scale);
        CHECK(fabsf(scale * scale - ch_multiplier(combos[i].jump)) < 1e-5f, "the height multiplier is the one asked for");
    }
}

static void test_hand_far(void) {
    pw_snapshot_t s = sample();
    float limit = pw_hand_limit(0.1f, 0.0f, 0.0f, 0.45f, 0.778f), peaks[PW_PARTS];
    const float slow[PW_PARTS] = { 2.0f, 3.0f, 3.0f, 2.5f }, fast[PW_PARTS] = { 18.0f, 40.0f, 30.0f, 120.0f };
    pw_peaks_t p;
    int armed = 1;
    CHECK(limit > 1.6f && limit < 1.8f, "shoulder reach plus 1.5 arm lengths (%.3f m)", limit);
    CHECK(pw_hand_limit(0.0f, 0.0f, 0.0f, 0.0f, 0.0f) == PW_HAND_FAR_M && pw_hand_limit(NAN, 0, 0, 0, 0.778f) == PW_HAND_FAR_M,
          "no arm data: the fixed threshold");
    CHECK(!pw_hand_far_step(&armed, 0.44f, limit) && armed, "the 1x hand (0.44 m)");
    CHECK(!pw_hand_far_step(&armed, 1.6f, limit) && armed, "1.6 m is still within this arm's reach");
    CHECK(pw_hand_far_step(&armed, limit + 0.05f, limit) && !armed, "past the game's own drop distance: once");
    CHECK(!pw_hand_far_step(&armed, limit + 1.0f, limit), "still far: not again");
    CHECK(!pw_hand_far_step(&armed, 0.7f, limit) && armed, "back within reach: re-armed");
    CHECK(!pw_hand_far_step(&armed, NAN, limit) && armed, "NaN is the sanity check's, not this one's");
    s.drive[1] = 18.4f;
    s.arm_urgency = 0.75f;
    s.hand_limit = limit;
    CHECK(has_line(&s, "pelvis drive (Kontrol) move 0.00 18.40") && has_line(&s, "urgency 0.75"), "drive and hand target");
    s.arm_urgency = 0.0f;
    s.arm_target[1] = -7.4e37f;
    CHECK(has_line(&s, "hand target (ControlArm): unused (rest pose)") && !has_line(&s, "e+37") && !has_line(&s, "-74"),
          "urgency 0: the target is not read by the game, so it is not printed");
    memset(&p, 0, sizeof p);
    pw_peaks_add(&p, 10.0, fast);
    pw_peaks_add(&p, 10.5, slow);
    pw_peaks_add(&p, 11.2, slow);
    pw_peaks_get(&p, peaks);
    CHECK(peaks[PW_PART_HAND] == 120.0f && peaks[PW_PART_RIGHT_FOOT] == 40.0f, "peaks of the last second kept");
    pw_peaks_add(&p, 12.4, slow);
    pw_peaks_get(&p, peaks);
    CHECK(peaks[PW_PART_HAND] == 2.5f, "older peaks dropped");
    CHECK(PW_SANE_SPEED == 100.0f && PW_SANE_REGION == 60.0f, "the engine's limits (0x0064D6EC, 0x0064D6F0)");
}

void physics_watch_tests(int* checks, int* failures) {
    test_hand_far();
    test_stuck();
    test_snapshot();
    test_sanity();
    test_cheat_lines();
    test_combinations();
    *checks += g_checks;
    *failures += g_failures;
}
