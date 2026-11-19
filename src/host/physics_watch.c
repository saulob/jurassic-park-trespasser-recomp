/*
 * DEBUG_SAULO: temporary telemetry of Anne's body and hand, read from memory only. The pelvis state is
 * Pel[pelvis][dof][value, velocity, acceleration] (OT pelvis_def.h: BODY 0, BODY_EULER 3, MOUTH 21, RIGHT_HAND 25).
 */
#include "guest_va.h"
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats.h"
#include "debug_menu_policy.h"
#include "jpt_gamepad.h"
#include "physics_watch.h"
#include "physics_watch_policy.h"

#define PLAYER GVA(0x0066EF64)
#define CURRENT_HINT GVA(0x0066F01C)
#define APP_SCENE GVA(0x0066E342)
#define WORLD GVA(0x0073F444)
#define WORLD_CAMERA 0x14u
#define PRESENCE 0x30u
#define PLACEMENT_BYTES 28u
#define ANIMATE_HP 0x50u
#define ANIMATE_HP_MAX 0x54u
#define PLAYER_HAND_PHASE 0xE28u
#define PLAYER_GRAB 0xE30u
#define PLAYER_REACH_CROUCH 0xE55u
#define PELVIS_COUNT 8u
#define PELVIS_USAGE GVA(0x006CE290)
#define PELVIS_HUMAN 1u
#define PELVIS_STATE GVA(0x006CF198)
#define PELVIS_STATE_STRIDE 0x300u
#define PELVIS_DATA GVA(0x006CEAB8)
#define PELVIS_DATA_STRIDE 0xDCu
#define PELVIS_ALIVE (49u * 4u)
#define PELVIS_HIP (13u * 4u)
#define PELVIS_SHOULDER (37u * 4u)
#define PELVIS_ARM (40u * 4u)
#define DOF_RIGHT_FOOT 7u
#define DOF_LEFT_FOOT 14u
#define DOF_BODY 0u
#define DOF_BODY_EULER 3u
#define DOF_MOUTH 21u
#define DOF_RIGHT_HAND 25u
#define PELVIS_KONTROL GVA(0x0067AE90)
#define PELVIS_KONTROL_STRIDE 0x18u
#define PELVIS_KROUCH GVA(0x0067AE68)
#define BIOTAG GVA(0x0067B0C0)
#define BIOTAG_STRIDE 0x100u
#define BIOTAG_RIGHT_FOOT_Z (9u * 4u)
#define CONTROL_ARM_URGENCY GVA(0x006D0A80)
#define CONTROL_ARM_POSITION GVA(0x006D0A84)
#define PELVIS_JUMP GVA(0x0067AFA0)
#define PELVIS_JUMP_VOLUNTARY GVA(0x0067AF5D)
#define INVALID_LOG_MAX 4

static struct {
    pw_stuck_t stuck;
    int requested, snapshots, invalid_logs, invalid, started, alive, hand_near, pelvis, lost_logs;
    pw_peaks_t peaks, drift;
    pw_snapshot_t last;
    LARGE_INTEGER frequency;
} W;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

static int human_pelvis(void) {
    for (uint32_t i = 0; i < PELVIS_COUNT; i++) {
        uint32_t usage;
        if (guest_read32(PELVIS_USAGE + 4u * i, &usage) && usage == PELVIS_HUMAN) return (int)i;
    }
    return -1;
}

/* Value and velocity of three consecutive DOFs. */
static void dofs(uint32_t state, uint32_t first, float value[3], float velocity[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        value[i] = rdf(state + (first + i) * 12u);
        if (velocity) velocity[i] = rdf(state + (first + i) * 12u + 4u);
    }
}

static int presence(uint32_t instance, dm_placement_t* out) {
    if (!instance || !guest_readable(instance + PRESENCE, PLACEMENT_BYTES)) return 0;
    memcpy(out, (const void*)ADDR(instance + PRESENCE), PLACEMENT_BYTES);
    return 1;
}

static double seconds(void) {
    LARGE_INTEGER now;
    if (!W.frequency.QuadPart) QueryPerformanceFrequency(&W.frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)W.frequency.QuadPart;
}

static void capture(pw_snapshot_t* s, const char* reason, const pw_input_t* in) {
    ch_state_t want;
    int free_camera;
    uint32_t player = 0, world = 0, camera = 0, v;
    dm_placement_t at;
    char scene[DM_SCENE_MAX];
    memset(s, 0, sizeof *s);
    s->reason = reason;
    s->t = seconds();
    snprintf(s->level, sizeof s->level, "?");
    if (guest_read_string(APP_SCENE, scene, sizeof scene)) dm_level_id(scene, s->level, sizeof s->level);
    s->hint = guest_read32(CURRENT_HINT, &v) ? (int)v : -1;
    cheats_watch(&want, &free_camera, &s->speed_real);
    s->speed = ch_speed_text(want.speed);
    s->jump = ch_multiplier_text(want.jump);
    s->camera = ch_camera_name(want.camera);
    if (guest_read32(PLAYER, &player) && presence(player, &at)) {
        s->player_ok = 1;
        memcpy(s->player_pos, at.pos, sizeof s->player_pos);
        s->player_yaw = dm_yaw(at.rot);
        s->hp = rdf(player + ANIMATE_HP);
        s->hp_max = rdf(player + ANIMATE_HP_MAX);
        if (guest_readable(player + PLAYER_HAND_PHASE, PLAYER_REACH_CROUCH - PLAYER_HAND_PHASE + 1)) {
            s->hand_phase = (int)MEM32(player + PLAYER_HAND_PHASE);
            s->grab = (int)MEM32(player + PLAYER_GRAB);
            s->reach_crouch = MEM8(player + PLAYER_REACH_CROUCH);
        }
    }
    s->pelvis = human_pelvis();
    if (s->pelvis >= 0) {
        uint32_t state = PELVIS_STATE + PELVIS_STATE_STRIDE * (uint32_t)s->pelvis;
        dofs(state, DOF_BODY, s->pelvis_pos, s->pelvis_vel);
        for (uint32_t i = 0; i < 4; i++) s->pelvis_orient[i] = rdf(state + (DOF_BODY_EULER + i) * 12u);
        s->mouth = rdf(state + DOF_MOUTH * 12u);
        s->mouth_vel = rdf(state + DOF_MOUTH * 12u + 4u);
        dofs(state, DOF_RIGHT_HAND, s->hand_pos, s->hand_vel);
        uint32_t data = PELVIS_DATA + PELVIS_DATA_STRIDE * (uint32_t)s->pelvis;
        s->alive = rdf(data + PELVIS_ALIVE);
        s->hand_limit = pw_hand_limit(rdf(data + PELVIS_HIP), rdf(data + PELVIS_SHOULDER),
                                      rdf(data + PELVIS_SHOULDER + 4u), rdf(data + PELVIS_SHOULDER + 8u),
                                      rdf(data + PELVIS_ARM));
        for (uint32_t i = 0; i < 6; i++)
            s->drive[i] = rdf(PELVIS_KONTROL + PELVIS_KONTROL_STRIDE * (uint32_t)s->pelvis + 4u * i);
        s->krouch = MEM8(PELVIS_KROUCH + (uint32_t)s->pelvis);
        s->foot_contact = rdf(BIOTAG + BIOTAG_STRIDE * (uint32_t)s->pelvis + BIOTAG_RIGHT_FOOT_Z);
    }
    s->arm_urgency = rdf(CONTROL_ARM_URGENCY);
    for (uint32_t i = 0; i < 3; i++) s->arm_target[i] = rdf(CONTROL_ARM_POSITION + 4u * i);
    for (int i = 0; i < 3; i++) s->jump_request[i] = rdf(PELVIS_JUMP + 4u * (uint32_t)i);
    s->jump_voluntary = MEM8(PELVIS_JUMP_VOLUNTARY);
    if (guest_read32(WORLD, &world) && world && guest_read32(world + WORLD_CAMERA, &camera) && presence(camera, &at)) {
        s->camera_ok = 1;
        memcpy(s->camera_pos, at.pos, sizeof s->camera_pos);
    }
    if (free_camera) s->camera = "Free Camera";
    if (!in) return;
    s->move_x = in->move_x;
    s->move_y = in->move_y;
    s->state = in->state;
    s->hit = in->hit;
    s->pad_buttons = in->pad_buttons;
    s->lt = in->lt;
    s->mouse_hand = in->mouse_hand;
    s->hand_seen = (in->state & JPT_CMD_HAND) != 0;
    s->stick_x = in->stick_x;
    s->stick_y = in->stick_y;
    s->interface_up = in->interface_up;
}

static float speed_of(uint32_t state, uint32_t dof) {
    float value[3], velocity[3];
    dofs(state, dof, value, velocity);
    return sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
}

static float from_pelvis(uint32_t state, uint32_t dof) {
    float body[3], part[3];
    dofs(state, DOF_BODY, body, NULL);
    dofs(state, dof, part, NULL);
    return sqrtf((part[0] - body[0]) * (part[0] - body[0]) + (part[1] - body[1]) * (part[1] - body[1]) +
                 (part[2] - body[2]) * (part[2] - body[2]));
}

static void note_peaks(pw_snapshot_t* s) {
    uint32_t state = PELVIS_STATE + PELVIS_STATE_STRIDE * (uint32_t)s->pelvis;
    float speeds[PW_PARTS], reach[PW_PARTS];
    speeds[PW_PART_BODY] = speed_of(state, DOF_BODY);
    speeds[PW_PART_RIGHT_FOOT] = speed_of(state, DOF_RIGHT_FOOT);
    speeds[PW_PART_LEFT_FOOT] = speed_of(state, DOF_LEFT_FOOT);
    speeds[PW_PART_HAND] = speed_of(state, DOF_RIGHT_HAND);
    pw_peaks_add(&W.peaks, s->t, speeds);
    reach[PW_PART_BODY] = 0.0f;
    reach[PW_PART_RIGHT_FOOT] = from_pelvis(state, DOF_RIGHT_FOOT);
    reach[PW_PART_LEFT_FOOT] = from_pelvis(state, DOF_LEFT_FOOT);
    reach[PW_PART_HAND] = from_pelvis(state, DOF_RIGHT_HAND);
    pw_peaks_add(&W.drift, s->t, reach);
    pw_peaks_get(&W.drift, s->drift);
    pw_peaks_get(&W.peaks, speeds);
    s->left_foot_moves = speeds[PW_PART_LEFT_FOOT] > 0.0f;
    s->drift_ok = 1;
}

static void log_snapshot(const pw_snapshot_t* s);
static void log_automatic_snapshot(const pw_snapshot_t* s);

/* The engine's own sanity check deactivates a pelvis whose boxes move too fast (InfoBox, iCornfield 1): Pel keeps the
 * last state it integrated, so it is read where the pelvis was. */
static void note_pelvis_lost(const pw_snapshot_t* now) {
    uint32_t state = PELVIS_STATE + PELVIS_STATE_STRIDE * (uint32_t)W.pelvis;
    float peaks[PW_PARTS], pos[3], vel[3], hand[3], hand_vel[3];
    if (W.lost_logs++ >= PW_SNAPSHOTS_MAX) return;
    pw_peaks_get(&W.peaks, peaks);
    dofs(state, DOF_BODY, pos, vel);
    dofs(state, DOF_RIGHT_HAND, hand, hand_vel);
    host_log_perf("physics-watch", "human pelvis lost: pelvis %d no longer human (Pel_Usage %u) at %.3f s; player at %.3f %.3f %.3f",
             W.pelvis, MEM32(PELVIS_USAGE + 4u * (uint32_t)W.pelvis), now->t, now->player_pos[0], now->player_pos[1],
             now->player_pos[2]);
    host_log_perf("physics-watch", "last integrated state: body %.3f %.3f %.3f vel %.2f %.2f %.2f, hand %.3f %.3f %.3f vel %.2f "
             "%.2f %.2f",
             pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], hand[0], hand[1], hand[2], hand_vel[0], hand_vel[1],
             hand_vel[2]);
    host_log_perf("physics-watch", "fastest in the last 1-2 s (m/s): body %.1f, right foot %.1f, left foot %.1f, hand %.1f; "
             "the engine deactivates a pelvis whose box passes %.0f m/s or whose query region reaches %.0f m",
             peaks[PW_PART_BODY], peaks[PW_PART_RIGHT_FOOT], peaks[PW_PART_LEFT_FOOT], peaks[PW_PART_HAND],
             PW_SANE_SPEED, PW_SANE_REGION);
    if (W.last.pelvis >= 0) {
        pw_snapshot_t last = W.last;
        last.reason = "LAST SNAPSHOT WITH THE PELVIS";
        log_automatic_snapshot(&last);
    }
}

static void log_snapshot(const pw_snapshot_t* s) {
    char line[PW_LINE_MAX];
    for (int i = 0; i < PW_LINES; i++)
        if (pw_format(s, i, line, sizeof line)) host_log("physics-watch", "%s", line);
}

/* Snapshots the watcher takes by itself (death, stuck, hand far) are telemetry; F9's is asked for. */
static void log_automatic_snapshot(const pw_snapshot_t* s) {
    if (host_perf_logging()) log_snapshot(s);
}

void physics_watch_read(const pw_input_t* in) {
    pw_snapshot_t s;
    pw_tick_t tick;
    ch_state_t want;
    int free_camera;
    float speed_real, stuck_seconds, moved;
    if (!W.started) {
        W.started = 1;
        pw_stuck_init(&W.stuck);
        W.hand_near = 1;
        W.pelvis = -1;
        host_log_perf("physics-watch", "watching Anne: automatic snapshots when she cannot move (at most %d), F9 for one now",
                      PW_SNAPSHOTS_MAX);
    }
    capture(&s, "MANUAL SNAPSHOT", in);
    if (s.pelvis >= 0) note_peaks(&s);
    if (W.requested) {
        W.requested = 0;
        log_snapshot(&s);
    }
    if (!pw_invalid(&s)) {
        W.invalid = 0;
    } else if (!W.invalid) {
        W.invalid = 1;
        if (W.invalid_logs++ < INVALID_LOG_MAX) {
            s.reason = "INVALID PLAYER STATE";
            log_automatic_snapshot(&s);
        }
    }
    if (s.pelvis >= 0 && W.alive && !(s.alive > 0.0f) && W.snapshots < PW_SNAPSHOTS_MAX) {
        W.snapshots++;
        s.reason = "ANNE DIED";
        log_automatic_snapshot(&s);
    }
    W.alive = s.pelvis >= 0 && s.alive > 0.0f;
    if (W.pelvis >= 0 && s.pelvis < 0) note_pelvis_lost(&s);
    W.pelvis = s.pelvis;
    if (s.pelvis >= 0) W.last = s;
    if (s.pelvis >= 0 &&
        pw_hand_far_step(&W.hand_near, hypotf(hypotf(s.hand_pos[0] - s.pelvis_pos[0], s.hand_pos[1] - s.pelvis_pos[1]),
                                              s.hand_pos[2] - s.pelvis_pos[2]),
                         s.hand_limit) &&
        W.snapshots < PW_SNAPSHOTS_MAX) {
        W.snapshots++;
        s.reason = "HAND FAR FROM BODY";
        log_automatic_snapshot(&s);
    }
    cheats_watch(&want, &free_camera, &speed_real);
    tick.t = s.t;
    tick.watching = s.pelvis >= 0 && s.alive > 0.0f && !in->interface_up && !free_camera;
    tick.move = hypotf(in->move_x, in->move_y);
    memcpy(tick.pos, s.pelvis >= 0 ? s.pelvis_pos : s.player_pos, sizeof tick.pos);
    if (pw_stuck_step(&W.stuck, &tick, &stuck_seconds, &moved) && W.snapshots < PW_SNAPSHOTS_MAX) {
        W.snapshots++;
        s.reason = "PLAYER STUCK";
        s.stuck_seconds = stuck_seconds;
        s.stuck_moved = moved;
        log_automatic_snapshot(&s);
    }
}

void physics_watch_request(void) {
    W.requested = 1;
    host_log("physics-watch", "F9: snapshot at the next gameplay frame");
}

void physics_watch_camera(ch_camera_t from, ch_camera_t to) {
    pw_snapshot_t s;
    char line[PW_LINE_MAX];
    if (from == to) return;
    capture(&s, "camera change", NULL);
    host_log("camera", "%s -> %s: player %.3f %.3f %.3f, camera %.3f %.3f %.3f, hand %.3f %.3f %.3f",
             ch_camera_name(from), ch_camera_name(to), s.player_pos[0], s.player_pos[1], s.player_pos[2],
             s.camera_pos[0], s.camera_pos[1], s.camera_pos[2], s.hand_pos[0], s.hand_pos[1], s.hand_pos[2]);
    if (pw_format(&s, 7, line, sizeof line)) host_log("camera", "%s", line);
}
