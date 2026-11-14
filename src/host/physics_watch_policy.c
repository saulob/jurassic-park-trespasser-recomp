#include "physics_watch_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jpt_gamepad.h"

static float flat(const float a[2], const float b[2]) { return hypotf(a[0] - b[0], a[1] - b[1]); }

void pw_stuck_init(pw_stuck_t* s) {
    memset(s, 0, sizeof *s);
    s->armed = 1;
}

int pw_stuck_step(pw_stuck_t* s, const pw_tick_t* t, float* seconds, float* moved) {
    float d;
    *seconds = *moved = 0.0f;
    if (!s->armed && flat(t->pos, s->fired_at) >= PW_REARM_METRES) s->armed = 1;
    if (!t->watching || t->move < PW_MOVE_INPUT) {
        s->window = 0;
        return 0;
    }
    if (!s->window) {
        s->window = 1;
        s->since = t->t;
        memcpy(s->origin, t->pos, sizeof s->origin);
        return 0;
    }
    d = flat(t->pos, s->origin);
    if (d >= PW_STUCK_METRES) {
        s->since = t->t;
        memcpy(s->origin, t->pos, sizeof s->origin);
        return 0;
    }
    if (!s->armed || t->t - s->since < PW_STUCK_SECONDS) return 0;
    s->armed = 0;
    memcpy(s->fired_at, t->pos, sizeof s->fired_at);
    *seconds = (float)(t->t - s->since);
    *moved = d;
    return 1;
}

float pw_hand_limit(float hip, float shoulder_x, float shoulder_y, float shoulder_z, float arm) {
    float x = hip + shoulder_x, reach = sqrtf(x * x + shoulder_y * shoulder_y + shoulder_z * shoulder_z);
    if (!isfinite(reach) || !isfinite(arm) || arm <= 0.1f || arm > 3.0f || reach > 3.0f) return PW_HAND_FAR_M;
    return reach + PW_ARM_DROP_FACTOR * arm;
}

void pw_peaks_add(pw_peaks_t* p, double t, const float speeds[PW_PARTS]) {
    if (t - p->window_start >= 1.0) {
        memcpy(p->previous, p->now, sizeof p->previous);
        memset(p->now, 0, sizeof p->now);
        p->window_start = t;
    }
    for (int i = 0; i < PW_PARTS; i++) {
        float v = isfinite(speeds[i]) ? speeds[i] : INFINITY;
        if (v > p->now[i]) p->now[i] = v;
    }
}

void pw_peaks_get(const pw_peaks_t* p, float out[PW_PARTS]) {
    for (int i = 0; i < PW_PARTS; i++) out[i] = p->now[i] > p->previous[i] ? p->now[i] : p->previous[i];
}

int pw_hand_far_step(int* armed, float distance, float limit) {
    if (!*armed) {
        if (distance < PW_HAND_NEAR_M) *armed = 1;
        return 0;
    }
    if (!(distance > limit)) return 0;
    *armed = 0;
    return 1;
}

int pw_finite(const float* v, int n, float limit) {
    for (int i = 0; i < n; i++)
        if (!isfinite(v[i]) || fabsf(v[i]) > limit) return 0;
    return 1;
}

const char* pw_invalid(const pw_snapshot_t* s) {
    if (s->player_ok && !pw_finite(s->player_pos, 3, PW_POSITION_LIMIT)) return "player position";
    if (s->pelvis >= 0) {
        if (!pw_finite(s->pelvis_pos, 3, PW_POSITION_LIMIT)) return "pelvis position";
        if (!pw_finite(s->pelvis_vel, 3, PW_SPEED_LIMIT)) return "pelvis velocity";
        if (!pw_finite(s->hand_pos, 3, PW_POSITION_LIMIT)) return "hand position";
        if (!pw_finite(s->hand_vel, 3, PW_SPEED_LIMIT)) return "hand velocity";
    }
    if (s->camera_ok && !pw_finite(s->camera_pos, 3, PW_POSITION_LIMIT)) return "camera position";
    if (s->speed_real != -1.0f && !(isfinite(s->speed_real) && s->speed_real > 0.0f && s->speed_real < 100.0f))
        return "rSpeedMultiplier";
    return NULL;
}

static float distance(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int bit(uint32_t v, uint32_t b) { return (v & b) != 0; }

int pw_format(const pw_snapshot_t* s, int index, char* out, size_t cap) {
    int n = -1;
    const char* bad;
    switch (index) {
    case 0: n = snprintf(out, cap, "%s at %.3f s, level %s, hint %d", s->reason, s->t, s->level, s->hint); break;
    case 1:
        n = s->player_ok ? snprintf(out, cap, "location %s X=%.3f Y=%.3f Z=%.3f yaw=%.3f", s->level, s->player_pos[0],
                                    s->player_pos[1], s->player_pos[2], s->player_yaw)
                         : snprintf(out, cap, "location %s: player not found", s->level);
        break;
    case 2:
        n = snprintf(out, cap, "cheats: Movement Speed %s, Jump Height %s, camera %s; player rSpeedMultiplier %.3f",
                     s->speed, s->jump, s->camera, s->speed_real);
        break;
    case 3:
        if (s->pelvis < 0) {
            n = snprintf(out, cap, "pelvis: no human pelvis");
            break;
        }
        n = snprintf(out, cap,
                     "pelvis %d: pos %.3f %.3f %.3f, vel %.3f %.3f %.3f (%.2f m/s), orientation %.3f %.3f %.3f %.3f, "
                     "alive %.2f, hp %.1f/%.1f",
                     s->pelvis, s->pelvis_pos[0], s->pelvis_pos[1], s->pelvis_pos[2], s->pelvis_vel[0],
                     s->pelvis_vel[1], s->pelvis_vel[2], sqrtf(s->pelvis_vel[0] * s->pelvis_vel[0] +
                                                              s->pelvis_vel[1] * s->pelvis_vel[1] +
                                                              s->pelvis_vel[2] * s->pelvis_vel[2]),
                     s->pelvis_orient[0], s->pelvis_orient[1], s->pelvis_orient[2], s->pelvis_orient[3], s->alive,
                     s->hp, s->hp_max);
        break;
    case 4:
        n = snprintf(out, cap, "crouch height %.3f (vel %.3f), jump request %.3f %.3f %.3f, voluntary %d", s->mouth,
                     s->mouth_vel, s->jump_request[0], s->jump_request[1], s->jump_request[2], s->jump_voluntary);
        break;
    case 5:
        n = snprintf(out, cap,
                     "input: move x %.2f y %.2f, state 0x%08X, hit 0x%08X (jump %d, crouch %d, hand %d, wrist %d, arm %d, "
                     "grab %d, use %d)",
                     s->move_x, s->move_y, s->state, s->hit, bit(s->state, JPT_CMD_JUMP), bit(s->state, JPT_CMD_CROUCH),
                     bit(s->state, JPT_CMD_HAND), bit(s->state, JPT_CMD_WRIST), bit(s->state, JPT_CMD_ARM),
                     bit(s->state, JPT_CMD_GRAB), bit(s->state, JPT_CMD_USE));
        break;
    case 6:
        n = snprintf(out, cap,
                     "hand input: LT %d, Move Hand seen by the game %d, mouse hand button %d, right stick %.2f %.2f, "
                     "pad buttons 0x%05X, host interface up %d",
                     s->lt, s->hand_seen, s->mouse_hand, s->stick_x, s->stick_y, s->pad_buttons, s->interface_up);
        break;
    case 7:
        n = s->pelvis < 0 ? snprintf(out, cap, "hand: no pelvis")
                          : snprintf(out, cap,
                                     "hand: pos %.3f %.3f %.3f, vel %.3f %.3f %.3f, phase %d, grab %d, reach crouch %d",
                                     s->hand_pos[0], s->hand_pos[1], s->hand_pos[2], s->hand_vel[0], s->hand_vel[1],
                                     s->hand_vel[2], s->hand_phase, s->grab, s->reach_crouch);
        break;
    case 8:
        n = snprintf(out, cap, "distances: pelvis->hand %.3f, camera->hand %.3f, camera->pelvis %.3f, player->pelvis %.3f",
                     s->pelvis >= 0 ? distance(s->pelvis_pos, s->hand_pos) : -1.0f,
                     s->pelvis >= 0 && s->camera_ok ? distance(s->camera_pos, s->hand_pos) : -1.0f,
                     s->pelvis >= 0 && s->camera_ok ? distance(s->camera_pos, s->pelvis_pos) : -1.0f,
                     s->pelvis >= 0 && s->player_ok ? distance(s->player_pos, s->pelvis_pos) : -1.0f);
        break;
    case 9:
        n = s->camera_ok ? snprintf(out, cap, "camera %s at %.3f %.3f %.3f", s->camera, s->camera_pos[0],
                                    s->camera_pos[1], s->camera_pos[2])
                         : snprintf(out, cap, "camera %s: not found", s->camera);
        break;
    case 10:
        if (s->stuck_seconds <= 0.0f) return 0;
        n = snprintf(out, cap, "stuck: trying to move for %.2f s, moved %.3f m", s->stuck_seconds, s->stuck_moved);
        break;
    case 11:
        if (!(bad = pw_invalid(s))) return 0;
        n = snprintf(out, cap, "out of range: %s", bad);
        break;
    case 12:
        if (s->pelvis < 0) return 0;
        n = snprintf(out, cap, "pelvis drive (Kontrol) move %.2f %.2f %.2f turn %.2f %.2f %.2f, crouch flag %d, right foot "
                     "contact %.3f",
                     s->drive[0], s->drive[1], s->drive[2], s->drive[3], s->drive[4], s->drive[5], s->krouch,
                     s->foot_contact);
        break;
    case 13:
        n = s->arm_urgency > 0.0f
                ? snprintf(out, cap, "hand target (ControlArm, body frame): urgency %.2f at %.3f %.3f %.3f; hand limit %.3f m",
                           s->arm_urgency, s->arm_target[0], s->arm_target[1], s->arm_target[2], s->hand_limit)
                : snprintf(out, cap, "hand target (ControlArm): unused (rest pose); hand limit %.3f m", s->hand_limit);
        break;
    case 14: {
        char left[24];
        if (s->pelvis < 0 || !s->drift_ok) return 0;
        if (s->left_foot_moves) snprintf(left, sizeof left, "%.3f m", s->drift[PW_PART_LEFT_FOOT]);
        else snprintf(left, sizeof left, "unused");
        n = snprintf(out, cap, "max relative drift (from the pelvis, last 1-2 s): hand=%.3f m, right_foot=%.3f m, "
                     "left_foot=%s",
                     s->drift[PW_PART_HAND], s->drift[PW_PART_RIGHT_FOOT], left);
        break;
    }
    default: return 0;
    }
    return n > 0 && (size_t)n < cap;
}
