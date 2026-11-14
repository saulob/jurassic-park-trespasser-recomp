/*
 * F10 cheats on the game's own state. INVUL is CPlayer::bInvulnerable, which retail still tests before collision damage
 * (0x004569D7) though its console case was compiled out; WOO is g_bUnlimitedAmmo (CGun, 0x0043FA5B). Movement Speed is
 * the player's own rSpeedMultiplier (CPhysicsInfoSkeleton, 0x004F0B27), Jump Height scales the voluntary jump's
 * Pelvis_Jump before Foot_IDOF reads it, Super Health is CAnimate's maximum, the camera modes are the camera's
 * CEntityAttached link. Host-side cheats leave the game whenever it is not in a running level (menus, saves, loads).
 */
#include "host.h"

#include <math.h>
#include <string.h>

#include "cheats.h"
#include "gamepad.h"

#define WORLD 0x0073F444u
#define WORLD_CAMERA 0x14u
#define WORLD_LOAD_PENDING 0x38u
#define WORLD_SAVE_PENDING 0x44u
#define PLAYER 0x0066EF64u
#define PLAYER_FLAGS 0xDCu /* bPhysics, bInvulnerable, bControl */
#define INVULNERABLE_SHIFT 8
#define ANIMATE_HP 0x50u
#define ANIMATE_HP_MAX 0x54u
#define INSTANCE_INFO 0x28u
#define INFO_PHYSICS 0x0Cu
#define PHYSICS_PLAYER_VTABLE 0x0064D644u
#define PHYSICS_SPEED 0x70u
#define AMMO_FLAGS 0x0066ECC0u /* g_bUnlimitedAmmo is its second byte */
#define AMMO_SHIFT 8
#define CAMERA_PRESENCE 0x30u
#define CAMERA_ATTACHED 0x50u
#define CAMERA_ANIMATE 0x54u
#define CAMERA_RELATIVE 0x58u
#define PLACEMENT_BYTES 28u
#define UPDATE_FROM_ATTACHED 0x00617F20u
#define INSTANCE_MOVE 0x00614B10u
#define PELVIS_JUMP_Z 0x0067AFA8u
#define PELVIS_JUMP_VOLUNTARY 0x0067AF5Cu /* the flag is the dword's second byte */
#define VOLUNTARY_SHIFT 8
#define FRAME_SECONDS_MAX 0.1f
#define LOG_LIMIT 48

static struct {
    ch_state_t want;
    int applied, free_valid, logs;
    uint32_t player, camera, camera_vtable, attached, animate, info;
    float speed_original, normal_max, super_hp;
    uint8_t relative[PLACEMENT_BYTES];
    ch_freecam_t free;
    ch_freecam_input_t input;
    int64_t last;
    char level[DM_LEVEL_ID_MAX];
} C = { .super_hp = -1.0f };

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static void note(const char* text) {
    if (C.logs < LOG_LIMIT && ++C.logs) host_log("cheats", "%s", text);
}

static int read_byte(uint32_t dword_va, int shift, int* out) {
    uint32_t v;
    if (!guest_read32(dword_va, &v)) return 0;
    *out = (int)((v >> shift) & 0xFFu) != 0;
    return 1;
}

static int write_byte(uint32_t dword_va, int shift, int on) {
    uint32_t v;
    if (!guest_read32(dword_va, &v)) return 0;
    v = (v & ~(0xFFu << shift)) | ((on ? 1u : 0u) << shift);
    return guest_write32(dword_va, v);
}

static uint32_t player(void) {
    uint32_t p = 0;
    return guest_read32(PLAYER, &p) && p && guest_readable(p, 0xE0u) ? p : 0;
}

static uint32_t active_camera(void) {
    uint32_t world, camera = 0;
    return guest_read32(WORLD, &world) && world && guest_read32(world + WORLD_CAMERA, &camera) && camera &&
                   guest_readable(camera, CAMERA_RELATIVE + PLACEMENT_BYTES)
               ? camera
               : 0;
}

static uint32_t speed_info(uint32_t p) {
    uint32_t info, physics, vtable;
    if (!guest_read32(p + INSTANCE_INFO, &info) || !info || !guest_read32(info + INFO_PHYSICS, &physics) || !physics ||
        !guest_read32(physics, &vtable) || vtable != PHYSICS_PLAYER_VTABLE ||
        !guest_readable(physics + PHYSICS_SPEED, 4))
        return 0;
    return physics;
}

static int pending(uint32_t offset) {
    uint32_t world, v;
    return guest_read32(WORLD, &world) && world && guest_read32(world + offset, &v) && (v & 0xFFu);
}

static void camera_follow(uint32_t camera) { guest_call(UPDATE_FROM_ATTACHED, camera, NULL, 0, 0, NULL); }

static void move_camera(uint32_t camera, const dm_placement_t* at, uint32_t scratch) {
    uint32_t args[2] = { scratch, camera };
    memcpy((void*)ADDR(scratch), at, PLACEMENT_BYTES);
    guest_call(INSTANCE_MOVE, camera, args, 2, 0, NULL);
}

static void drop(const char* why) {
    C.applied = 0;
    note(why);
}

static int same_player(void) { return C.player && player() == C.player; }

/* The camera a mode changed, still the object it was (cameras are swapped, not freed, outside a load). */
static int camera_kept(void) {
    uint32_t vtable;
    return C.camera && guest_readable(C.camera, CAMERA_RELATIVE + PLACEMENT_BYTES) &&
           guest_read32(C.camera, &vtable) && vtable == C.camera_vtable;
}

static int unchanged(void) {
    return same_player() && (C.want.camera == CH_CAMERA_NORMAL || active_camera() == C.camera);
}

static void suspend(void) {
    float hp, max;
    if (!C.applied) return;
    if (C.want.camera != CH_CAMERA_NORMAL && camera_kept()) {
        guest_write32(C.camera + CAMERA_ATTACHED, C.attached);
        guest_write32(C.camera + CAMERA_ANIMATE, C.animate);
        memcpy((void*)ADDR(C.camera + CAMERA_RELATIVE), C.relative, PLACEMENT_BYTES);
        camera_follow(C.camera);
    }
    if (!same_player()) {
        drop("cheats dropped: the player was replaced");
        return;
    }
    if (C.info && C.want.speed != CH_MULTIPLIER_DEFAULT) wrf(C.info + PHYSICS_SPEED, C.speed_original);
    if (C.want.super_health) {
        hp = rdf(C.player + ANIMATE_HP);
        max = rdf(C.player + ANIMATE_HP_MAX);
        C.super_hp = hp;
        ch_super_off(C.normal_max, &hp, &max);
        wrf(C.player + ANIMATE_HP, hp);
        wrf(C.player + ANIMATE_HP_MAX, max);
    }
    C.applied = 0;
}

static int apply_camera(uint32_t scratch) {
    dm_placement_t at;
    if (!C.camera || !guest_read32(C.camera, &C.camera_vtable) ||
        !guest_read32(C.camera + CAMERA_ATTACHED, &C.attached) ||
        !guest_read32(C.camera + CAMERA_ANIMATE, &C.animate) || !C.attached || !C.animate)
        return 0;
    memcpy(C.relative, (const void*)ADDR(C.camera + CAMERA_RELATIVE), PLACEMENT_BYTES);
    if (C.want.camera == CH_CAMERA_THIRD) {
        ch_third_person_relative(&at);
        memcpy((void*)ADDR(C.camera + CAMERA_RELATIVE), &at, PLACEMENT_BYTES);
        camera_follow(C.camera);
        return 1;
    }
    if (!C.free_valid) {
        memcpy(&at, (const void*)ADDR(C.camera + CAMERA_PRESENCE), PLACEMENT_BYTES);
        ch_freecam_from(&at, &C.free);
        C.free_valid = 1;
    }
    guest_write32(C.camera + CAMERA_ATTACHED, 0);
    guest_write32(C.camera + CAMERA_ANIMATE, 0);
    ch_freecam_placement(&C.free, &at);
    move_camera(C.camera, &at, scratch);
    return 1;
}

static void apply(const char* level, uint32_t scratch) {
    float hp, max;
    if (C.applied || !ch_any(&C.want)) return;
    C.player = player();
    C.camera = active_camera();
    if (!C.player) return;
    ch_set_level(&C.want, level);
    C.info = speed_info(C.player);
    if (C.info && C.want.speed != CH_MULTIPLIER_DEFAULT) {
        C.speed_original = rdf(C.info + PHYSICS_SPEED);
        wrf(C.info + PHYSICS_SPEED, C.speed_original * ch_multiplier(C.want.speed));
    }
    if (C.want.super_health) {
        hp = rdf(C.player + ANIMATE_HP);
        C.normal_max = rdf(C.player + ANIMATE_HP_MAX);
        max = ch_super_max(C.normal_max);
        if (hp > 0.0f) hp = C.super_hp >= 0.0f ? fminf(fmaxf(C.super_hp, hp), max) : max;
        wrf(C.player + ANIMATE_HP_MAX, max);
        wrf(C.player + ANIMATE_HP, hp);
    }
    if (C.want.camera != CH_CAMERA_NORMAL && !apply_camera(scratch)) {
        C.want.camera = CH_CAMERA_NORMAL;
        note("camera mode off: the camera does not follow the player");
    }
    C.applied = 1;
}

static int held(int vk) { return GetAsyncKeyState(vk) < 0; }

/* The game's own keys make S walk forward; the free camera's WASD is literal, the stick comes through CInput. */
static void free_camera_keys(ch_freecam_input_t* in) {
    uint32_t pad = gamepad_buttons();
    int w = held('W'), a = held('A'), s = held('S'), d = held('D');
    if (w || a || s || d) {
        in->move[0] = (float)(d - a);
        in->move[1] = (float)(w - s);
    }
    in->up = held(VK_SPACE) || (pad & JPT_PAD_BIT(JPT_PAD_SOUTH));
    in->down = held(VK_CONTROL) || (pad & JPT_PAD_BIT(JPT_PAD_EAST));
    in->fast = held(VK_SHIFT) || (pad & JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER));
}

static void free_camera_step(uint32_t scratch) {
    LARGE_INTEGER now, freq;
    ch_freecam_input_t in = C.input;
    dm_placement_t at;
    float dt;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    dt = C.last ? (float)(now.QuadPart - C.last) / (float)freq.QuadPart : 0.0f;
    C.last = now.QuadPart;
    if (dt > FRAME_SECONDS_MAX) dt = FRAME_SECONDS_MAX;
    memset(&C.input, 0, sizeof C.input);
    if (debug_menu_visible()) memset(&in, 0, sizeof in);
    else if (window_host_input_focused()) free_camera_keys(&in);
    ch_freecam_step(&C.free, &in, dt);
    ch_freecam_placement(&C.free, &at);
    move_camera(C.camera, &at, scratch);
}

void cheats_frame(const dm_world_t* w, uint32_t scratch) {
    int should;
    if (ch_level_seen(&C.want, w->place, w->level)) {
        if (C.applied) drop("cheats dropped with their level");
        C.free_valid = 0;
        C.super_hp = -1.0f;
        note("Movement Speed, Jump Height, Super Health and the camera modes reset: another level");
    }
    should = ch_should_apply(w->place, pending(WORLD_LOAD_PENDING), pending(WORLD_SAVE_PENDING), player() != 0);
    if (C.applied && (!should || !unchanged())) suspend();
    if (!C.applied && should) apply(w->level, scratch);
    if (C.applied && C.want.camera == CH_CAMERA_FREE) free_camera_step(scratch);
    else C.last = 0;
}

int cheats_busy(void) { return ch_any(&C.want) || C.applied; }

void cheats_suspend(void) { suspend(); }

void cheats_world(dm_world_t* w) {
    uint32_t p = player(), camera = active_camera(), animate = 0;
    w->invulnerable = w->ammo = -1;
    w->hp = w->hp_max = 0.0f;
    w->speed = C.want.speed;
    w->jump = C.want.jump;
    w->super_health = C.want.super_health;
    w->camera = C.want.camera;
    if (w->place != DM_PLACE_LEVEL) return;
    if (p) {
        read_byte(p + PLAYER_FLAGS, INVULNERABLE_SHIFT, &w->invulnerable);
        w->hp = rdf(p + ANIMATE_HP);
        w->hp_max = rdf(p + ANIMATE_HP_MAX);
    }
    read_byte(AMMO_FLAGS, AMMO_SHIFT, &w->ammo);
    w->camera_head = camera && ((guest_read32(camera + CAMERA_ANIMATE, &animate) && animate) ||
                                (C.applied && C.want.camera == CH_CAMERA_FREE && camera == C.camera));
}

uint32_t cheats_detached_from(uint32_t camera) {
    return C.applied && C.want.camera == CH_CAMERA_FREE && camera == C.camera ? C.attached : 0;
}

int cheats_take_input(uint32_t sinput) {
    if (!C.applied || C.want.camera != CH_CAMERA_FREE) return 0;
    C.input.rotate[0] += rdf(sinput + 4);
    C.input.rotate[1] += rdf(sinput + 8);
    C.input.move[0] = rdf(sinput + 12);
    C.input.move[1] = rdf(sinput + 16);
    return 1;
}

void cheats_before_pelvis(void) {
    int voluntary;
    if (!C.applied || C.want.jump == CH_MULTIPLIER_DEFAULT ||
        !read_byte(PELVIS_JUMP_VOLUNTARY, VOLUNTARY_SHIFT, &voluntary) || !voluntary || rdf(PELVIS_JUMP_Z) != 1.0f)
        return;
    wrf(PELVIS_JUMP_Z, ch_jump_speed_scale(C.want.jump));
}

static const char* run_toggle(int ok, const char* on_text, const char* off_text, int on) {
    return !ok ? "Not available" : on ? on_text : off_text;
}

const char* cheats_run(const dm_action_t* a, const char* level, uint32_t scratch) {
    uint32_t p = player();
    float hp;
    switch (a->kind) {
    case DM_ACT_INVULNERABLE:
        return run_toggle(p && write_byte(p + PLAYER_FLAGS, INVULNERABLE_SHIFT, a->on), "Invulnerability on",
                          "Invulnerability off", a->on);
    case DM_ACT_AMMO:
        return run_toggle(write_byte(AMMO_FLAGS, AMMO_SHIFT, a->on), "Infinite Ammo on", "Infinite Ammo off", a->on);
    case DM_ACT_HEAL:
        if (!p || !ch_heal(rdf(p + ANIMATE_HP), rdf(p + ANIMATE_HP_MAX), &hp)) return "Anne cannot be healed now";
        wrf(p + ANIMATE_HP, hp);
        return "Healed";
    default: break;
    }
    suspend();
    switch (a->kind) {
    case DM_ACT_SPEED: C.want.speed = ch_multiplier_clamp(a->value); break;
    case DM_ACT_JUMP: C.want.jump = ch_multiplier_clamp(a->value); break;
    case DM_ACT_SUPER_HEALTH:
        C.want.super_health = a->on;
        C.super_hp = -1.0f;
        break;
    case DM_ACT_CAMERA:
        if (a->value != CH_CAMERA_FREE) C.free_valid = 0;
        C.want.camera = (ch_camera_t)a->value;
        break;
    default: return NULL;
    }
    apply(level, scratch);
    switch (a->kind) {
    case DM_ACT_SPEED: return ch_multiplier_text(C.want.speed);
    case DM_ACT_JUMP: return ch_multiplier_text(C.want.jump);
    case DM_ACT_SUPER_HEALTH: return C.want.super_health ? "Super Health on" : "Super Health off";
    default:
        return C.want.camera == CH_CAMERA_THIRD ? "Third Person on"
               : C.want.camera == CH_CAMERA_FREE ? "Free Camera on"
                                                 : "Camera back on Anne";
    }
}
