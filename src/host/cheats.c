/*
 * F10 cheats on the game's own state. INVUL is CPlayer::bInvulnerable, which retail still tests before collision damage
 * (0x004569D7) though its console case was compiled out; WOO is g_bUnlimitedAmmo (CGun, 0x0043FA5B). Movement Speed is
 * the player's own rSpeedMultiplier (CPhysicsInfoSkeleton, 0x004F0B27), Jump Height scales the voluntary jump's
 * Pelvis_Jump before Foot_IDOF reads it, Super Health is CAnimate's maximum, the free camera is the camera's
 * CEntityAttached link. Host-side cheats leave the game whenever it is not in a running level (menus, saves, loads).
 * The launcher's saved values start each session; F10 only records what it changed over them (jpt_cheats.h).
 */
#include "guest_abi.h"

#include <math.h>
#include <string.h>

#include "cheats.h"
#include "host_input.h"
#include "jpt_sync.h"
#include "jpt_tr.h"
#include "physics_watch.h"

#define WORLD GVA(0x0073F444)
#define WORLD_CAMERA 0x14u
#define WORLD_LOAD_PENDING 0x38u
#define WORLD_SAVE_PENDING 0x44u
#define PLAYER GVA(0x0066EF64)
#define PLAYER_FLAGS 0xDCu /* bPhysics, bInvulnerable, bControl */
#define INVULNERABLE_SHIFT 8
#define ANIMATE_HP 0x50u
#define ANIMATE_HP_MAX 0x54u
#define INSTANCE_INFO 0x28u
#define INFO_PHYSICS 0x0Cu
#define INFO_AI 0x10u
#define AI_INFO_FLAGS 0x0Cu
#define ANIMATE_PARTS 0xACu
#define ANIMATE_PART_COUNT 8
#define PHYSICS_PLAYER_VTABLE GVA(0x0064D644)
#define PHYSICS_SPEED 0x70u
#define AI_SYSTEM GVA(0x0066E77C)
#define AI_FLAGS 0x50u /* bActive, bSleep, bBoring: DINOS toggles the third byte */
#define AMMO_FLAGS GVA(0x0066ECC0) /* g_bUnlimitedAmmo is its second byte */
#define AMMO_SHIFT 8
#define CAMERA_PRESENCE 0x30u
#define CAMERA_ATTACHED 0x50u
#define CAMERA_ANIMATE 0x54u
#define CAMERA_RELATIVE 0x58u
#define PLACEMENT_BYTES 28u
#define UPDATE_FROM_ATTACHED GVA(0x00617F20)
#define INSTANCE_MOVE GVA(0x00614B10)
#define PELVIS_JUMP_Z GVA(0x0067AFA8)
#define PELVIS_JUMP_VOLUNTARY GVA(0x0067AF5C) /* the flag is the dword's second byte */
#define VOLUNTARY_SHIFT 8
#define FRAME_SECONDS_MAX 0.1f
#define LOG_LIMIT 48

static jpt_cheat_session_t S;

static struct {
    ch_state_t want;
    int started, applied, free_valid, logs;
    int armed_player, armed_ai;
    uint32_t flags_player, flags_ai;
    uint32_t player, camera, camera_vtable, attached, animate, info;
    float speed_original, normal_max, super_hp;
    uint8_t relative[PLACEMENT_BYTES];
    ch_freecam_t free;
    ch_freecam_input_t input;
    int64_t last;
    char level[DM_LEVEL_ID_MAX];
    char reported[DM_LEVEL_ID_MAX];
    char text[DM_TEXT_MAX];
} C = { .want = { .speed = CH_MULTIPLIER_DEFAULT, .jump = CH_MULTIPLIER_DEFAULT }, .super_hp = -1.0f };

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static void note(const char* text) {
    if (C.logs < LOG_LIMIT && ++C.logs) host_log("cheats", "%s", text);
}

static jpt_cheat_session_t* session(void) {
    if (!C.started) cheats_start(NULL);
    return &S;
}

/* A change F10 made: kept over the saved value for this session only. */
static void remember(jpt_cheat_t cheat, int value) {
    jpt_cheat_set(session(), cheat, value);
    ch_from_session(&C.want, &S);
}

static int effective(jpt_cheat_t cheat) { return jpt_cheat_effective(session(), cheat); }

void cheats_start(const int saved[JPT_CHEAT_COUNT]) {
    char line[256];
    jpt_cheat_session_start(&S, saved);
    ch_from_session(&C.want, &S);
    C.started = C.armed_player = C.armed_ai = 1;
    C.applied = C.free_valid = C.logs = 0;
    C.flags_player = C.flags_ai = 0;
    C.super_hp = -1.0f;
    if (jpt_cheat_format_saved(line, sizeof line, &S)) host_log("cheats", "saved: %s", line);
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
        wrf(C.info + PHYSICS_SPEED, C.speed_original * ch_speed(C.want.speed));
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
        remember(JPT_CHEAT_FREE_CAMERA, 0);
        note("Fly / Free Camera off: the camera does not follow the player");
    }
    C.applied = 1;
}

static int held(int vk) { return host_key_held((unsigned)vk); }

/* The game's own keys make S walk forward; the free camera's WASD is literal, the stick comes through CInput. */
static void free_camera_keys(ch_freecam_input_t* in) {
    uint32_t pad = host_pad_buttons();
    int w = held('W'), a = held('A'), s = held('S'), d = held('D');
    if (w || a || s || d) {
        in->move[0] = (float)(d - a);
        in->move[1] = (float)(w - s);
    }
    in->up = held(HOST_VK_SPACE) || (pad & JPT_PAD_BIT(JPT_PAD_SOUTH));
    in->down = held(HOST_VK_CONTROL) || (pad & JPT_PAD_BIT(JPT_PAD_EAST));
    in->fast = held(HOST_VK_SHIFT) || (pad & JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER));
}

static void free_camera_step(uint32_t scratch) {
    uint32_t now = jpt_time_ms();
    ch_freecam_input_t in = C.input;
    dm_placement_t at;
    float dt;
    dt = C.last ? (float)(now - C.last) / 1000.0f : 0.0f;
    C.last = now;
    if (dt > FRAME_SECONDS_MAX) dt = FRAME_SECONDS_MAX;
    memset(&C.input, 0, sizeof C.input);
    if (host_panel_visible()) memset(&in, 0, sizeof in);
    else if (host_input_focused()) free_camera_keys(&in);
    ch_freecam_step(&C.free, &in, dt);
    ch_freecam_placement(&C.free, &at);
    move_camera(C.camera, &at, scratch);
}

/* Once per level, when Anne is first there to act on: what the session plays with. */
static void report_active(const dm_world_t* w) {
    char line[160];
    if (w->place != DM_PLACE_LEVEL || !w->level[0] || !player() || !strcmp(C.reported, w->level)) return;
    snprintf(C.reported, sizeof C.reported, "%s", w->level);
    if (ch_format_active(line, sizeof line, &C.want, effective(JPT_CHEAT_INVULNERABLE), effective(JPT_CHEAT_AMMO)))
        host_log("cheats", "active in %s: %s", w->level, line);
}

int cheats_read_ai(uint32_t* system, uint32_t* flags) {
    return guest_read32(AI_SYSTEM, system) && *system && guest_read32(*system + AI_FLAGS, flags);
}

int cheats_freeze_ai(int on) {
    uint32_t system, flags;
    if (!cheats_read_ai(&system, &flags)) return 0;
    flags = (flags & ~(0xFFu << CHEATS_AI_BORING_SHIFT)) | ((on ? 1u : 0u) << CHEATS_AI_BORING_SHIFT);
    if (!guest_write32(system + AI_FLAGS, flags)) return 0;
    remember(JPT_CHEAT_FREEZE_AI, on != 0);
    return 1;
}

/* The game's own flags take the effective values once per level, load or player: never every frame, so the game's own
 * console cheats still work over them. */
static void apply_flags(int should) {
    uint32_t p = player(), system, flags;
    if (!should) return;
    if (p != C.flags_player) {
        C.flags_player = p;
        C.armed_player = 1;
    }
    if (C.armed_player) {
        C.armed_player = 0;
        if (effective(JPT_CHEAT_INVULNERABLE) && !write_byte(p + PLAYER_FLAGS, INVULNERABLE_SHIFT, 1))
            note("Invulnerability could not be applied");
        if (effective(JPT_CHEAT_AMMO) && !write_byte(AMMO_FLAGS, AMMO_SHIFT, 1))
            note("Infinite Ammo could not be applied");
    }
    if (!cheats_read_ai(&system, &flags)) return;
    if (system != C.flags_ai) {
        C.flags_ai = system;
        C.armed_ai = 1;
    }
    if (!C.armed_ai) return;
    C.armed_ai = 0;
    if (effective(JPT_CHEAT_FREEZE_AI) && !((flags >> CHEATS_AI_BORING_SHIFT) & 0xFFu) && !cheats_freeze_ai(1))
        note("Freeze Dinosaurs could not be applied");
}

void cheats_frame(const dm_world_t* w, uint32_t scratch) {
    int should;
    report_active(w);
    if (ch_level_seen(&C.want, w->place, w->level)) {
        if (C.applied) drop("cheats dropped with their level");
        C.free_valid = 0;
        C.super_hp = -1.0f;
        C.armed_player = C.armed_ai = 1;
        remember(JPT_CHEAT_FREE_CAMERA, 0);
        remember(JPT_CHEAT_NOCLIP, 0);
        note("another level: Fly / Free Camera and Noclip off, the other cheats stay");
    }
    if (pending(WORLD_LOAD_PENDING)) C.armed_player = C.armed_ai = 1;
    should = ch_should_apply(w->place, pending(WORLD_LOAD_PENDING), pending(WORLD_SAVE_PENDING), player() != 0);
    if (C.applied && (!should || !unchanged())) suspend();
    if (!C.applied && should) apply(w->level, scratch);
    apply_flags(should);
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
    w->stealth = C.want.stealth;
    w->noclip = C.want.noclip;
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

float cheats_speed_factor(void) {
    return C.applied && C.info && C.want.speed != CH_MULTIPLIER_DEFAULT ? ch_speed(C.want.speed) : 1.0f;
}

void cheats_before_pelvis(void) {
    int voluntary;
    if (!C.applied || C.want.jump == CH_MULTIPLIER_DEFAULT ||
        !read_byte(PELVIS_JUMP_VOLUNTARY, VOLUNTARY_SHIFT, &voluntary) || !voluntary || rdf(PELVIS_JUMP_Z) != 1.0f)
        return;
    wrf(PELVIS_JUMP_Z, ch_jump_speed_scale(C.want.jump));
}

/* The menu's row already shows the new value: a change is only logged, the menu gets no feedback line. */
static const char* changed(dm_action_kind_t kind, int on, int value) {
    if (ch_format_change(C.text, sizeof C.text, kind, on, value)) host_log("cheats", "%s", C.text);
    return NULL;
}

/* The menu's row shows the new value; the log gets the cheat's own name. */
static const char* run_logged_toggle(jpt_cheat_t cheat, int on) {
    remember(cheat, on != 0);
    if (ch_format_toggle(C.text, sizeof C.text, jpt_cheats[cheat].label, on)) host_log("cheats", "%s", C.text);
    return NULL;
}

static const char* run_toggle(int ok, dm_action_kind_t kind, int on, jpt_cheat_t cheat) {
    if (!ok) return jpt_tr("Not available");
    remember(cheat, on != 0);
    return changed(kind, on, 0);
}

const char* cheats_run(const dm_action_t* a, const char* level, uint32_t scratch) {
    uint32_t p = player();
    float hp;
    switch (a->kind) {
    case DM_ACT_INVULNERABLE:
        return run_toggle(p && write_byte(p + PLAYER_FLAGS, INVULNERABLE_SHIFT, a->on), a->kind, a->on,
                          JPT_CHEAT_INVULNERABLE);
    case DM_ACT_AMMO: return run_toggle(write_byte(AMMO_FLAGS, AMMO_SHIFT, a->on), a->kind, a->on, JPT_CHEAT_AMMO);
    case DM_ACT_HEAL:
        if (!p || !ch_heal(rdf(p + ANIMATE_HP), rdf(p + ANIMATE_HP_MAX), &hp)) return jpt_tr("Anne cannot be healed now");
        wrf(p + ANIMATE_HP, hp);
        return changed(a->kind, 1, 0);
    case DM_ACT_STEALTH: return run_logged_toggle(JPT_CHEAT_STEALTH, a->on);
    case DM_ACT_NOCLIP: return run_logged_toggle(JPT_CHEAT_NOCLIP, a->on);
    default: break;
    }
    suspend();
    switch (a->kind) {
    case DM_ACT_SPEED: remember(JPT_CHEAT_SPEED, ch_speed_clamp(a->value)); break;
    case DM_ACT_JUMP: remember(JPT_CHEAT_JUMP, ch_multiplier_clamp(a->value)); break;
    case DM_ACT_SUPER_HEALTH:
        remember(JPT_CHEAT_SUPER_HEALTH, a->on != 0);
        C.super_hp = -1.0f;
        break;
    case DM_ACT_CAMERA:
        if (a->value != CH_CAMERA_FREE) C.free_valid = 0;
        physics_watch_camera(C.want.camera, (ch_camera_t)a->value);
        remember(JPT_CHEAT_FREE_CAMERA, a->value == CH_CAMERA_FREE);
        break;
    default: return NULL;
    }
    apply(level, scratch);
    switch (a->kind) {
    case DM_ACT_SPEED: return changed(a->kind, 1, C.want.speed);
    case DM_ACT_JUMP: return changed(a->kind, 1, C.want.jump);
    case DM_ACT_SUPER_HEALTH: return changed(a->kind, C.want.super_health, 0);
    default: return changed(a->kind, 1, C.want.camera);
    }
}


void cheats_watch(ch_state_t* want, int* free_camera, float* speed_real) {
    uint32_t p = player(), info = p ? speed_info(p) : 0;
    *want = C.want;
    *free_camera = C.applied && C.want.camera == CH_CAMERA_FREE;
    *speed_real = info ? rdf(info + PHYSICS_SPEED) : -1.0f;
}

int cheats_stealth(void) { return C.want.stealth; }

int cheats_noclip(void) { return C.want.noclip; }

static int read_byte_at(uint32_t va, uint8_t* out) {
    if (!guest_readable(va, 1)) return 0;
    *out = *(const uint8_t*)ADDR(va);
    return 1;
}

/* Every sighting of an object enters a brain's influences through CInfluenceList::Add, which drops an object whose AI
 * info has the ignore bit. Anne's parts carry it only while a perception update runs, so nothing else reads it. */
void cheats_stealth_begin(cheats_hidden_t* h) {
    uint32_t p;
    h->count = 0;
    if (!C.want.stealth || !(p = player())) return;
    for (uint32_t i = 0; i < ANIMATE_PART_COUNT; i++) {
        uint32_t part, info, ai;
        uint8_t flags, hidden;
        if (!guest_read32(p + ANIMATE_PARTS + 4u * i, &part) || !part || !guest_readable(part, INSTANCE_INFO + 4u) ||
            !guest_read32(part + INSTANCE_INFO, &info) || !info || !guest_readable(info, INFO_AI + 4u) ||
            !guest_read32(info + INFO_AI, &ai) || !ai || !read_byte_at(ai + AI_INFO_FLAGS, &flags) ||
            !ch_ai_hide_flags(flags, &hidden))
            continue;
        *(uint8_t*)ADDR(ai + AI_INFO_FLAGS) = hidden;
        h->flags_va[h->count] = ai + AI_INFO_FLAGS;
        h->original[h->count++] = flags;
    }
}

void cheats_stealth_end(const cheats_hidden_t* h) {
    for (int i = 0; i < h->count; i++)
        if (guest_readable(h->flags_va[i], 1)) *(uint8_t*)ADDR(h->flags_va[i]) = h->original[i];
}
