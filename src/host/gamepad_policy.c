#include "gamepad_policy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void pad_stick_shape(float x, float y, float deadzone, float* out_x, float* out_y) {
    float m = sqrtf(x * x + y * y), k;
    *out_x = *out_y = 0.0f;
    if (m <= deadzone || deadzone >= 1.0f) return;
    k = (clampf(m, 0.0f, 1.0f) - deadzone) / (1.0f - deadzone) / m;
    *out_x = x * k;
    *out_y = y * k;
}

float pad_trigger_shape(float value, float deadzone) {
    if (value <= deadzone || deadzone >= 1.0f) return 0.0f;
    return (clampf(value, 0.0f, 1.0f) - deadzone) / (1.0f - deadzone);
}

float pad_look_curve(float m) {
    m = clampf(m, 0.0f, 1.0f);
    return m * (0.35f + 0.65f * m);
}

uint32_t pad_buttons_with_triggers(const pad_state_t* s, float trigger_deadzone) {
    uint32_t b = s->buttons & ~(JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER) | JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER));
    if (s->lt > trigger_deadzone) b |= JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER);
    if (s->rt > trigger_deadzone) b |= JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER);
    return b;
}

void pad_activity_reset(pad_activity_t* a) { memset(a, 0, sizeof *a); }

static int crossed(uint8_t* on, float value, float off_at, float on_at) {
    value = fabsf(value);
    if (!*on && value >= on_at) {
        *on = 1;
        return 1;
    }
    if (*on && value < off_at) *on = 0;
    return 0;
}

int pad_activity_update(pad_activity_t* a, const pad_state_t* s, float stick_deadzone, float trigger_deadzone) {
    const float axes[4] = { s->lx, s->ly, s->rx, s->ry }, triggers[2] = { s->lt, s->rt };
    uint32_t buttons = s->buttons & ~(JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER) | JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER));
    int started = (buttons & ~a->buttons) != 0;
    a->buttons = buttons;
    for (int i = 0; i < 4; i++)
        started |= crossed(&a->stick_on[i], axes[i], stick_deadzone, stick_deadzone + PAD_ACTIVITY_STICK_MARGIN);
    for (int i = 0; i < 2; i++)
        started |= crossed(&a->trigger_on[i], triggers[i], trigger_deadzone, trigger_deadzone + PAD_ACTIVITY_TRIGGER_MARGIN);
    return started;
}

void pad_tracker_init(pad_tracker_t* t) {
    memset(t, 0, sizeof *t);
    t->device = PAD_DEVICE_KEYBOARD_MOUSE;
}

static int switch_to(pad_tracker_t* t, pad_device_t device) {
    if (t->device == device) return 0;
    t->device = device;
    t->mouse_distance = 0;
    t->switches++;
    return 1;
}

int pad_tracker_keyboard(pad_tracker_t* t) { return switch_to(t, PAD_DEVICE_KEYBOARD_MOUSE); }

int pad_tracker_mouse_button(pad_tracker_t* t) { return switch_to(t, PAD_DEVICE_KEYBOARD_MOUSE); }

int pad_tracker_mouse_motion(pad_tracker_t* t, int dx, int dy, uint32_t now_ms) {
    if (!dx && !dy) return 0;
    if (!t->mouse_seen || now_ms - t->mouse_last_ms > PAD_MOUSE_WINDOW_MS) t->mouse_distance = 0;
    t->mouse_seen = 1;
    t->mouse_last_ms = now_ms;
    t->mouse_distance += abs(dx) + abs(dy);
    if (t->mouse_distance < PAD_MOUSE_SWITCH_DISTANCE) return 0;
    t->mouse_distance = PAD_MOUSE_SWITCH_DISTANCE;
    return switch_to(t, PAD_DEVICE_KEYBOARD_MOUSE);
}

int pad_tracker_gamepad(pad_tracker_t* t) { return switch_to(t, PAD_DEVICE_GAMEPAD); }

int pad_tracker_pads_left(pad_tracker_t* t, int pads_left) {
    return pads_left <= 0 ? switch_to(t, PAD_DEVICE_KEYBOARD_MOUSE) : 0;
}

void pad_roster_init(pad_roster_t* r) {
    memset(r, 0, sizeof *r);
    r->active = -1;
}

int pad_roster_find(const pad_roster_t* r, uint32_t id) {
    for (int i = 0; i < PAD_ROSTER_MAX; i++)
        if (r->slots[i].used && r->slots[i].id == id) return i;
    return -1;
}

int pad_roster_add(pad_roster_t* r, uint32_t id, int family) {
    if (pad_roster_find(r, id) >= 0) return -1;
    for (int i = 0; i < PAD_ROSTER_MAX; i++) {
        pad_slot_t* s = &r->slots[i];
        if (s->used) continue;
        s->used = 1;
        s->id = id;
        s->family = family;
        s->order = ++r->clock;
        if (r->active < 0) r->active = i;
        return i;
    }
    return -1;
}

int pad_roster_remove(pad_roster_t* r, uint32_t id) {
    int at = pad_roster_find(r, id), was_active;
    if (at < 0) return 0;
    was_active = at == r->active;
    memset(&r->slots[at], 0, sizeof r->slots[at]);
    if (!was_active) return 0;
    r->active = -1;
    for (int i = 0; i < PAD_ROSTER_MAX; i++)
        if (r->slots[i].used && (r->active < 0 || r->slots[i].order > r->slots[r->active].order)) r->active = i;
    return 1;
}

int pad_roster_activity(pad_roster_t* r, int slot) {
    if (slot < 0 || slot >= PAD_ROSTER_MAX || !r->slots[slot].used) return 0;
    r->slots[slot].order = ++r->clock;
    if (r->active == slot) return 0;
    r->active = slot;
    return 1;
}

int pad_roster_count(const pad_roster_t* r) {
    int n = 0;
    for (int i = 0; i < PAD_ROSTER_MAX; i++) n += r->slots[i].used != 0;
    return n;
}

uint32_t pad_commands_for(const jpt_gamepad_settings_t* cfg, uint32_t buttons) {
    uint32_t commands = 0;
    for (int a = 0; a < JPT_ACT_COUNT; a++) {
        int control = cfg->bind[a];
        if (a == JPT_ACT_CROUCH_TOGGLE || control < 0 || control >= JPT_PAD_CONTROL_COUNT) continue;
        if (buttons & JPT_PAD_BIT(control)) commands |= jpt_pad_actions[a].command;
    }
    return commands;
}

void pad_gameplay_from(const pad_state_t* s, uint32_t buttons, const jpt_gamepad_settings_t* cfg, float dt,
                       int crouch_toggled, pad_gameplay_t* out) {
    float deadzone = cfg->stick_deadzone / 100.0f, mx, my, rx, ry, m, forward;
    memset(out, 0, sizeof *out);
    dt = clampf(dt, 0.0f, PAD_MAX_FRAME_SECONDS);
    pad_stick_shape(s->lx, s->ly, deadzone, &mx, &my);
    forward = -my;
    out->move_x = mx * PAD_MOVE_SIDE;
    out->move_y = forward >= 0.0f ? forward * PAD_MOVE_FORWARD : forward * PAD_MOVE_BACK;
    pad_stick_shape(s->rx, s->ry, deadzone, &rx, &ry);
    m = sqrtf(rx * rx + ry * ry);
    if (m > 0.0f) {
        float k = pad_look_curve(m) / m;
        rx *= k;
        ry *= k;
    }
    out->look_x = rx * PAD_LOOK_YAW_RATE * (cfg->look_x / 100.0f) * dt;
    out->look_y = -ry * PAD_LOOK_PITCH_RATE * (cfg->look_y / 100.0f) * dt * (cfg->invert_y ? -1.0f : 1.0f);
    out->held = pad_commands_for(cfg, buttons) | (crouch_toggled ? JPT_CMD_CROUCH : 0u);
    /* The game turns the wrist only while the hand is up (CPlayer: inside bPressed(uCMD_HAND)). */
    if (out->held & (JPT_CMD_WRIST | JPT_CMD_ARM)) out->held |= JPT_CMD_HAND;
}

void pad_merge_sinput(pad_sinput_t* in, const pad_gameplay_t* pad, uint32_t pad_hit) {
    uint32_t state = in->state | pad->held;
    float k = (state & JPT_CMD_HAND) ? PAD_HAND_LOOK_SCALE : 1.0f;
    in->rotate_x += pad->look_x * k;
    in->rotate_y += pad->look_y * k;
    in->move_x = clampf(in->move_x + pad->move_x, -PAD_MOVE_SIDE, PAD_MOVE_SIDE);
    in->move_y = clampf(in->move_y + pad->move_y, -PAD_MOVE_BACK, PAD_MOVE_FORWARD);
    in->state = state;
    in->hit |= pad_hit;
}

void pad_edges_reset(pad_edges_t* e, uint32_t held_now) {
    e->previous = 0;
    e->blocked = held_now;
}

uint32_t pad_edges_update(pad_edges_t* e, uint32_t held, uint32_t* pressed) {
    uint32_t effective;
    e->blocked &= held;
    effective = held & ~e->blocked;
    *pressed = effective & ~e->previous;
    e->previous = effective;
    return effective;
}

static int overlaps(int a0, int a1, int b0, int b1) { return a0 < b1 && b0 < a1; }

int pad_nav_pick(const pad_rect_t* targets, int count, pad_rect_t from, pad_nav_dir_t dir) {
    double fx = (from.left + from.right) * 0.5, fy = (from.top + from.bottom) * 0.5, best_score = 0;
    int best = -1;
    for (int i = 0; i < count; i++) {
        const pad_rect_t* t = &targets[i];
        double tx = (t->left + t->right) * 0.5, ty = (t->top + t->bottom) * 0.5, major, minor, score;
        int beam;
        if (t->left == from.left && t->top == from.top && t->right == from.right && t->bottom == from.bottom) continue;
        switch (dir) {
        case PAD_NAV_DOWN:
            if (ty <= fy + 1 || t->bottom <= from.bottom) continue;
            major = t->top > from.bottom ? t->top - from.bottom : 0;
            minor = fabs(tx - fx);
            beam = overlaps(t->left, t->right, from.left, from.right);
            break;
        case PAD_NAV_UP:
            if (ty >= fy - 1 || t->top >= from.top) continue;
            major = t->bottom < from.top ? from.top - t->bottom : 0;
            minor = fabs(tx - fx);
            beam = overlaps(t->left, t->right, from.left, from.right);
            break;
        case PAD_NAV_RIGHT:
            if (tx <= fx + 1 || t->right <= from.right) continue;
            major = t->left > from.right ? t->left - from.right : 0;
            minor = fabs(ty - fy);
            beam = overlaps(t->top, t->bottom, from.top, from.bottom);
            break;
        case PAD_NAV_LEFT:
            if (tx >= fx - 1 || t->left >= from.left) continue;
            major = t->right < from.left ? from.left - t->right : 0;
            minor = fabs(ty - fy);
            beam = overlaps(t->top, t->bottom, from.top, from.bottom);
            break;
        default: return -1;
        }
        score = (beam ? 0.0 : 1e7) + major * major * 13.0 + minor * minor;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

#define NAV_STICK_ON 0.55f
#define NAV_STICK_HOLD 0.35f

pad_nav_dir_t pad_nav_direction(uint32_t buttons, float x, float y, pad_nav_dir_t previous) {
    if (buttons & JPT_PAD_BIT(JPT_PAD_DPAD_UP)) return PAD_NAV_UP;
    if (buttons & JPT_PAD_BIT(JPT_PAD_DPAD_DOWN)) return PAD_NAV_DOWN;
    if (buttons & JPT_PAD_BIT(JPT_PAD_DPAD_LEFT)) return PAD_NAV_LEFT;
    if (buttons & JPT_PAD_BIT(JPT_PAD_DPAD_RIGHT)) return PAD_NAV_RIGHT;
    switch (previous) {
    case PAD_NAV_UP: if (y <= -NAV_STICK_HOLD) return previous; break;
    case PAD_NAV_DOWN: if (y >= NAV_STICK_HOLD) return previous; break;
    case PAD_NAV_LEFT: if (x <= -NAV_STICK_HOLD) return previous; break;
    case PAD_NAV_RIGHT: if (x >= NAV_STICK_HOLD) return previous; break;
    default: break;
    }
    if (fabsf(x) < NAV_STICK_ON && fabsf(y) < NAV_STICK_ON) return PAD_NAV_NONE;
    if (fabsf(y) >= fabsf(x)) return y < 0 ? PAD_NAV_UP : PAD_NAV_DOWN;
    return x < 0 ? PAD_NAV_LEFT : PAD_NAV_RIGHT;
}

void pad_repeat_reset(pad_repeat_t* r) {
    r->dir = PAD_NAV_NONE;
    r->next = 0;
}

pad_nav_dir_t pad_repeat_update(pad_repeat_t* r, pad_nav_dir_t held, uint32_t now_ms) {
    if (held == PAD_NAV_NONE) {
        r->dir = PAD_NAV_NONE;
        return PAD_NAV_NONE;
    }
    if (held != r->dir) {
        r->dir = held;
        r->next = now_ms + PAD_REPEAT_DELAY_MS;
        return held;
    }
    if ((int32_t)(now_ms - r->next) >= 0) {
        r->next = now_ms + PAD_REPEAT_MS;
        return held;
    }
    return PAD_NAV_NONE;
}
