#include "cheats_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const float k_multipliers[CH_MULTIPLIER_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f };
static const char* const k_multiplier_text[CH_MULTIPLIER_COUNT] = { "0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x" };

static const float k_speeds[CH_SPEED_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.5f, 10.0f };
static const char* const k_speed_text[CH_SPEED_COUNT] = { "0.5x", "1.0x", "1.5x", "2.0x", "3.0x", "5.0x", "7.5x", "10.0x" };

int ch_speed_clamp(int index) { return index < 0 ? 0 : index >= CH_SPEED_COUNT ? CH_SPEED_COUNT - 1 : index; }

float ch_speed(int index) { return k_speeds[ch_speed_clamp(index)]; }

const char* ch_speed_text(int index) { return k_speed_text[ch_speed_clamp(index)]; }

int ch_multiplier_clamp(int index) {
    return index < 0 ? 0 : index >= CH_MULTIPLIER_COUNT ? CH_MULTIPLIER_COUNT - 1 : index;
}

float ch_multiplier(int index) { return k_multipliers[ch_multiplier_clamp(index)]; }

const char* ch_multiplier_text(int index) { return k_multiplier_text[ch_multiplier_clamp(index)]; }

float ch_jump_speed_scale(int index) { return sqrtf(ch_multiplier(index)); }

void ch_init(ch_state_t* s) {
    memset(s, 0, sizeof *s);
    s->speed = s->jump = CH_MULTIPLIER_DEFAULT;
    s->camera = CH_CAMERA_NORMAL;
}

int ch_any(const ch_state_t* s) {
    return s->speed != CH_MULTIPLIER_DEFAULT || s->jump != CH_MULTIPLIER_DEFAULT || s->super_health ||
           s->camera != CH_CAMERA_NORMAL;
}

int ch_level_seen(ch_state_t* s, dm_place_t place, const char* level) {
    int left = place == DM_PLACE_MAIN_MENU ||
               (place == DM_PLACE_LEVEL && level && level[0] && s->level[0] && strcmp(level, s->level));
    if (!left || !ch_any(s)) return 0;
    ch_init(s);
    return 1;
}

void ch_set_level(ch_state_t* s, const char* level) { snprintf(s->level, sizeof s->level, "%s", level ? level : ""); }

int ch_should_apply(dm_place_t place, int load_pending, int save_pending, int player) {
    return place == DM_PLACE_LEVEL && !load_pending && !save_pending && player;
}

float ch_super_max(float normal_max) { return normal_max * CH_SUPER_HEALTH_FACTOR; }

void ch_super_off(float normal_max, float* hp, float* max) {
    *max = normal_max;
    if (*hp > normal_max) *hp = normal_max;
}

int ch_heal(float hp, float max, float* out) {
    if (!(hp > 0.0f) || !(max > 0.0f)) return 0;
    *out = max;
    return 1;
}

void ch_third_person_relative(dm_placement_t* out) {
    memset(out, 0, sizeof *out);
    out->pos[1] = -CH_THIRD_PERSON_BACK;
    out->pos[2] = CH_THIRD_PERSON_UP;
    out->rot[0] = 1.0f;
}

void ch_freecam_from(const dm_placement_t* at, ch_freecam_t* out) {
    double c = at->rot[0], x = at->rot[1], y = at->rot[2], z = at->rot[3];
    double up = 2.0 * (y * z + c * x);
    memcpy(out->pos, at->pos, sizeof out->pos);
    out->yaw = dm_yaw(at->rot);
    out->pitch = (float)asin(up > 1.0 ? 1.0 : up < -1.0 ? -1.0 : up);
}

void ch_freecam_step(ch_freecam_t* c, const ch_freecam_input_t* in, float dt) {
    float speed = CH_FREE_CAMERA_SPEED * (in->fast ? CH_FREE_CAMERA_FAST : 1.0f) * dt;
    float mx = in->move[0], my = in->move[1], len = sqrtf(mx * mx + my * my), s, co;
    c->yaw -= in->rotate[0];
    c->pitch += in->rotate[1];
    if (c->pitch > CH_FREE_CAMERA_PITCH_MAX) c->pitch = CH_FREE_CAMERA_PITCH_MAX;
    if (c->pitch < -CH_FREE_CAMERA_PITCH_MAX) c->pitch = -CH_FREE_CAMERA_PITCH_MAX;
    c->yaw = remainderf(c->yaw, 6.2831853f);
    if (len > 1.0f) {
        mx /= len;
        my /= len;
    }
    s = sinf(c->yaw);
    co = cosf(c->yaw);
    c->pos[0] += speed * (-s * my + co * mx);
    c->pos[1] += speed * (co * my + s * mx);
    c->pos[2] += speed * (float)((in->up != 0) - (in->down != 0));
}

/* The game composes a pitch about X, then a yaw about Z (CPlayer's r3FromAngles); as one quaternion, scalar first. */
void ch_freecam_placement(const ch_freecam_t* c, dm_placement_t* out) {
    float hy = c->yaw * 0.5f, hp = c->pitch * 0.5f, cy = cosf(hy), sy = sinf(hy), cp = cosf(hp), sp = sinf(hp);
    memcpy(out->pos, c->pos, sizeof out->pos);
    out->rot[0] = cy * cp;
    out->rot[1] = cy * sp;
    out->rot[2] = sy * sp;
    out->rot[3] = sy * cp;
}

const char* ch_camera_name(ch_camera_t camera) {
    return camera == CH_CAMERA_THIRD ? "Third Person" : camera == CH_CAMERA_FREE ? "Free Camera" : "Normal";
}

static const char* on_off(int v) { return v < 0 ? "?" : v ? "On" : "Off"; }

int ch_format_change(char* out, size_t cap, dm_action_kind_t kind, int on, int value) {
    int n;
    switch (kind) {
    case DM_ACT_SPEED: n = snprintf(out, cap, "Movement Speed = %s", ch_speed_text(value)); break;
    case DM_ACT_JUMP: n = snprintf(out, cap, "Jump Height = %s", ch_multiplier_text(value)); break;
    case DM_ACT_INVULNERABLE: n = snprintf(out, cap, "Invulnerability = %s", on_off(on)); break;
    case DM_ACT_AMMO: n = snprintf(out, cap, "Infinite Ammo = %s", on_off(on)); break;
    case DM_ACT_SUPER_HEALTH: n = snprintf(out, cap, "Super Health = %s", on_off(on)); break;
    case DM_ACT_FREEZE_AI: n = snprintf(out, cap, "Freeze Dinosaurs = %s", on_off(on)); break;
    case DM_ACT_CAMERA: n = snprintf(out, cap, "Camera = %s", ch_camera_name((ch_camera_t)value)); break;
    case DM_ACT_HEAL: n = snprintf(out, cap, "Heal"); break;
    default: return 0;
    }
    return n > 0 && (size_t)n < cap;
}

int ch_format_active(char* out, size_t cap, const ch_state_t* s, int invulnerable, int ammo) {
    int n = snprintf(out, cap, "movement=%s jump=%s super health=%s camera=%s invulnerability=%s infinite ammo=%s",
                     ch_speed_text(s->speed), ch_multiplier_text(s->jump), on_off(s->super_health),
                     ch_camera_name(s->camera), on_off(invulnerable), on_off(ammo));
    return n > 0 && (size_t)n < cap;
}
