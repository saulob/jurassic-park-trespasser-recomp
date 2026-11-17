#include "cheats_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jpt_tr.h"

static const jpt_cheat_info_t* speed_info(void) { return &jpt_cheats[JPT_CHEAT_SPEED]; }

static const jpt_cheat_info_t* jump_info(void) { return &jpt_cheats[JPT_CHEAT_JUMP]; }

int ch_speed_clamp(int index) { return index < 0 ? 0 : index >= CH_SPEED_COUNT ? CH_SPEED_COUNT - 1 : index; }

float ch_speed(int index) { return speed_info()->choices[ch_speed_clamp(index)].value; }

const char* ch_speed_text(int index) { return speed_info()->choices[ch_speed_clamp(index)].label; }

int ch_multiplier_clamp(int index) {
    return index < 0 ? 0 : index >= CH_MULTIPLIER_COUNT ? CH_MULTIPLIER_COUNT - 1 : index;
}

float ch_multiplier(int index) { return jump_info()->choices[ch_multiplier_clamp(index)].value; }

const char* ch_multiplier_text(int index) { return jump_info()->choices[ch_multiplier_clamp(index)].label; }

float ch_jump_speed_scale(int index) { return sqrtf(ch_multiplier(index)); }

void ch_init(ch_state_t* s) {
    memset(s, 0, sizeof *s);
    s->speed = s->jump = CH_MULTIPLIER_DEFAULT;
    s->camera = CH_CAMERA_NORMAL;
}

void ch_from_session(ch_state_t* s, const jpt_cheat_session_t* session) {
    char level[DM_LEVEL_ID_MAX];
    memcpy(level, s->level, sizeof level);
    ch_init(s);
    memcpy(s->level, level, sizeof level);
    s->speed = ch_speed_clamp(jpt_cheat_effective(session, JPT_CHEAT_SPEED));
    s->jump = ch_multiplier_clamp(jpt_cheat_effective(session, JPT_CHEAT_JUMP));
    s->super_health = jpt_cheat_effective(session, JPT_CHEAT_SUPER_HEALTH) != 0;
    s->stealth = jpt_cheat_effective(session, JPT_CHEAT_STEALTH) != 0;
    s->noclip = jpt_cheat_effective(session, JPT_CHEAT_NOCLIP) != 0;
    s->camera = jpt_cheat_effective(session, JPT_CHEAT_FREE_CAMERA) ? CH_CAMERA_FREE : CH_CAMERA_NORMAL;
}

int ch_any(const ch_state_t* s) {
    return s->speed != CH_MULTIPLIER_DEFAULT || s->jump != CH_MULTIPLIER_DEFAULT || s->super_health ||
           s->camera != CH_CAMERA_NORMAL;
}

int ch_ai_hide_flags(uint8_t flags, uint8_t* hidden) {
    *hidden = (uint8_t)(flags | CH_AI_INFO_IGNORE);
    return (flags & CH_AI_INFO_IGNORE) == 0;
}

int ch_level_seen(ch_state_t* s, dm_place_t place, const char* level) {
    int left;
    if (place == DM_PLACE_MAIN_MENU) {
        left = s->level[0] != 0;
        s->level[0] = 0;
    } else if (place == DM_PLACE_LEVEL && level && level[0]) {
        left = s->level[0] && strcmp(level, s->level);
        ch_set_level(s, level);
    } else {
        return 0;
    }
    if (left) s->camera = CH_CAMERA_NORMAL, s->noclip = 0;
    return left;
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
    return jpt_tr(camera == CH_CAMERA_FREE ? "Free Camera" : "Normal");
}

int ch_format_toggle(char* out, size_t cap, const char* name, int on) {
    int n = snprintf(out, cap, "%s %s", name, on ? "On" : "Off");
    return n > 0 && (size_t)n < cap;
}

static const char* on_off(int v) { return v < 0 ? "?" : jpt_tr(v ? "On" : "Off"); }

int ch_format_change(char* out, size_t cap, dm_action_kind_t kind, int on, int value) {
    int n;
    switch (kind) {
    case DM_ACT_SPEED: n = snprintf(out, cap, "%s = %s", jpt_tr("Movement Speed"), ch_speed_text(value)); break;
    case DM_ACT_JUMP: n = snprintf(out, cap, "%s = %s", jpt_tr("Jump Height"), ch_multiplier_text(value)); break;
    case DM_ACT_INVULNERABLE: n = snprintf(out, cap, "%s = %s", jpt_tr("Invulnerability"), on_off(on)); break;
    case DM_ACT_AMMO: n = snprintf(out, cap, "%s = %s", jpt_tr("Infinite Ammo"), on_off(on)); break;
    case DM_ACT_SUPER_HEALTH: n = snprintf(out, cap, "%s = %s", jpt_tr("Super Health"), on_off(on)); break;
    case DM_ACT_FREEZE_AI: n = snprintf(out, cap, "%s = %s", jpt_tr("Freeze Dinosaurs"), on_off(on)); break;
    case DM_ACT_CAMERA: n = snprintf(out, cap, "%s = %s", jpt_tr("Camera"), ch_camera_name((ch_camera_t)value)); break;
    case DM_ACT_HEAL: n = snprintf(out, cap, "%s", jpt_tr("Heal")); break;
    default: return 0;
    }
    return n > 0 && (size_t)n < cap;
}

int ch_format_active(char* out, size_t cap, const ch_state_t* s, int invulnerable, int ammo) {
    int n = snprintf(out, cap, "movement=%s jump=%s super health=%s stealth=%s noclip=%s camera=%s invulnerability=%s infinite ammo=%s",
                     ch_speed_text(s->speed), ch_multiplier_text(s->jump), on_off(s->super_health),
                     on_off(s->stealth), on_off(s->noclip), ch_camera_name(s->camera), on_off(invulnerable), on_off(ammo));
    return n > 0 && (size_t)n < cap;
}
