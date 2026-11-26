/* The game's world as Trespasser CE's script API reaches it (jpt_script_engine.h), and the AI's Bite and Feint actions for
 * OnActBite. Objects are found again by their unique handle at every call; nothing of a script holds a guest pointer.
 * Installed for a custom session only. */
#include "guest_features.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_abi.h"
#include "host_input.h"
#include "jpt_fs.h"
#include "jpt_picture.h"
#include "jpt_custom_level.h"
#include "jpt_level_scripts.h"
#include "jpt_script_engine.h"
#include "level_actions.h"

#define FIND_INSTANCE GVA(0x00607C10) /* CWorld::pinsFindInstance(handle), thiscall */
#define NAME_HASH GVA(0x005F6100)     /* (chars, 0, 1), cdecl */
#define HEAD_POSITION GVA(0x006187A0) /* CAnimal::v3GetHeadPos(out), thiscall */
#define BITE_ACT GVA(0x004329A0)      /* CActivityBite::Act(influence), thiscall */
#define FEINT_ACT GVA(0x00432B10)     /* CActivityFeint::Act(influence), thiscall */
#define WORLD GVA(0x0073F444)
#define GAME_TIME GVA(0x0073F34C)
#define AI_SYSTEM GVA(0x0066E77C)
#define PLAYER GVA(0x0066EF64)
#define TEXT_OVERLAY GVA(0x0066F03C)
#define TEXT_SHOW GVA(0x00468710) /* CTextOverlay::u4DisplayFormattedString(text, seconds, format, rgb, previous, type) */
#define TEXT_END GVA(0x00468980)  /* CTextOverlay::u4FindSequenceEnd(type) */
#define RASTER_PTR GVA(0x0073DC40) /* the game's main rendering surface */
#define PLAYER_SAY GVA(0x00464010)   /* CPlayer's voice: thiscall (sample ID), ret 4 */
#define JUMP_VOICE GVA(0x004F8F00)   /* the player's jump vocal, from the pelvis's jump step */
#define JUMP_Z GVA(0x0067AFA8)       /* the pelvis jump impulse's z: 1 for a standard jump, 0 for none pending */

#define INSTANCE_HANDLE 0x2Cu
#define INSTANCE_POSITION 0x30u
#define INSTANCE_ROTATION 0x3Cu
#define ANIMATE_BOXES 0xACu
#define ANIMAL_BRAIN 0xDCu
#define SYSTEM_BRAIN 0x31B8u /* the brain the AI is thinking with */
#define BRAIN_ANIMAL 0x11Cu
#define INFLUENCE_LOCATION 0x2ECu
#define INFLUENCE_DISTANCE 0x304u
#define INFLUENCE_TARGET 0x33Cu
#define ANIMATE_HIT_POINTS 0x50u
#define PLAYER_HAND_OBJECT 0xE34u
#define PLAYER_HEAD 0xD88u /* the view's rotation relative to the body, w first */
/* CPlayer::Process's movement request, [ebp-0x118] just before it sends it: walk speed, jump amount (0: no jump), jump scale. */
#define PROCESS_REQUEST 0x118u
#define REQUEST_SPEED 0x14u
#define REQUEST_JUMP_AMOUNT 0x84u
#define REQUEST_JUMP_SCALE 0x88u
#define GUN_MAX_AMMO 0x68u
#define GUN_AMMO 0x6Cu
#define VF_DEAD 0x1FCu /* CAnimate::bDead: its hit points at or below zero */
#define NAME_MAX 256u
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define SINPUT_ROTATE_X 0x04u
#define SINPUT_ROTATE_Y 0x08u
#define SINPUT_MOVE_X 0x0Cu
#define SINPUT_MOVE_Y 0x10u
#define SINPUT_STATE 0x14u
#define SINPUT_HIT 0x18u
#define OVERLAY_LIST 0x54u /* its entries: a ring from this sentinel */
#define NODE_HANDLE 0x8u   /* what the overlay hands out for an entry */
#define NODE_LIMIT 256

static const struct {
    uint32_t id;
    const char* name;
} kTypes[] = {
    { GVA_ID(0x0064B22C), "CInstance" }, { GVA_ID(0x0064EF88), "CBoundaryBox" }, { GVA_ID(0x0064A46C), "CEntity" },
    { GVA_ID(0x0064ED18), "CAnimate" },  { GVA_ID(0x0064F9A8), "CAnimal" },      { GVA_ID(0x0064AB48), "CPlayer" },
    { GVA_ID(0x0064A8F4), "CPlayer" },   { GVA_ID(0x0064A274), "CGun" },
};

static struct {
    recomp_func_t bite, feint, jump_voice;
    jpt_script_engine engine;
    int jump_scaled; /* the request just sent asks to jump with jump_scale */
    float jump_scale;
    uint32_t user_vk[JPT_SCRIPT_USER_KEYS];
    uint32_t held, hit;
} S;

static uint32_t instance_of(uint32_t handle) {
    uint32_t world, instance = 0;
    return handle && guest_read32(WORLD, &world) && world && guest_call(FIND_INSTANCE, world, &handle, 1, 0, &instance) ? instance : 0;
}

static uint32_t handle_of(uint32_t instance) {
    uint32_t handle = 0;
    return instance && guest_read32(instance + INSTANCE_HANDLE, &handle) ? handle : 0;
}

static const char* type_of(uint32_t instance) {
    uint32_t vtable;
    if (!instance || !guest_read32(instance, &vtable)) return NULL;
    for (size_t i = 0; i < sizeof kTypes / sizeof kTypes[0]; i++)
        if (vtable == GVA_AT(kTypes[i].id)) return kTypes[i].name;
    return "unknown";
}

static int animate(const char* type) { return type && (!strcmp(type, "CAnimate") || !strcmp(type, "CAnimal") || !strcmp(type, "CPlayer")); }

static const char* native_type(void* user, uint32_t handle) {
    (void)user;
    return type_of(instance_of(handle));
}

static uint32_t find(void* user, const char* name) {
    const size_t n = name ? strlen(name) : 0;
    const uint32_t chars = n && n < NAME_MAX ? guest_scratch(2, NAME_MAX) : 0;
    uint32_t hash = 0;
    const uint32_t args[3] = { chars, 0, 1 };
    (void)user;
    if (!chars) return 0;
    memcpy((void*)ADDR(chars), name, n + 1);
    if (!guest_call(NAME_HASH, 0, args, 3, 1, &hash)) return 0;
    return instance_of(hash) ? hash : 0;
}

static int read_placement(uint32_t object, jpt_vec3* position, jpt_rot3* rotation) {
    if (!object || !guest_readable(object + INSTANCE_POSITION, 28u)) return 0;
    memcpy(position, (const void*)ADDR(object + INSTANCE_POSITION), sizeof *position);
    memcpy(rotation, (const void*)ADDR(object + INSTANCE_ROTATION), sizeof *rotation);
    return 1;
}

static int placement(void* user, uint32_t handle, int box, jpt_vec3* position, jpt_rot3* rotation) {
    const uint32_t instance = instance_of(handle);
    uint32_t part = 0;
    (void)user;
    if (box < 0) return read_placement(instance, position, rotation);
    if (box >= JPT_SCRIPT_BOXES || !animate(type_of(instance)) || !guest_read32(instance + ANIMATE_BOXES + 4u * (uint32_t)box, &part)) return 0;
    return read_placement(part, position, rotation);
}

static int head_position(void* user, uint32_t handle, jpt_vec3* out) {
    const uint32_t animal = instance_of(handle), esp = g_esp;
    uint32_t at;
    (void)user;
    if (!animal || strcmp(type_of(animal), "CAnimal")) return 0;
    g_esp -= sizeof *out;
    at = g_esp;
    if (!guest_call(HEAD_POSITION, animal, &at, 1, 0, NULL)) {
        g_esp = esp;
        return 0;
    }
    memcpy(out, (const void*)ADDR(at), sizeof *out);
    g_esp = esp;
    return 1;
}

static int teleport(void* user, uint32_t handle, const jpt_vec3* position, const jpt_rot3* rotation) {
    const float p[3] = { position->x, position->y, position->z }, r[4] = { rotation->w, rotation->x, rotation->y, rotation->z };
    (void)user;
    return level_actions_teleport(handle, p, r);
}

static int fixed_velocity(void* user, uint32_t handle, const jpt_vec3* velocity, int milliseconds, int fail_if_moving) {
    const float v[3] = { velocity->x, velocity->y, velocity->z };
    (void)user;
    return level_actions_fixed_velocity(handle, v, milliseconds, fail_if_moving);
}

static float sim_time(void* user) {
    uint32_t bits = 0;
    float seconds = 0.0f;
    (void)user;
    if (guest_read32(GAME_TIME, &bits)) memcpy(&seconds, &bits, sizeof seconds);
    return seconds;
}

static uint32_t player(void* user) {
    uint32_t instance = 0;
    (void)user;
    return guest_read32(PLAYER, &instance) && instance ? handle_of(instance) : 0;
}

static uint32_t held_object(void* user, uint32_t handle) {
    const uint32_t instance = instance_of(handle);
    uint32_t held = 0;
    (void)user;
    if (!instance || strcmp(type_of(instance), "CPlayer") || !guest_read32(instance + PLAYER_HAND_OBJECT, &held)) return 0;
    return handle_of(held);
}

static uint32_t player_instance(uint32_t handle) {
    const uint32_t instance = instance_of(handle);
    return instance && !strcmp(type_of(instance), "CPlayer") ? instance : 0;
}

static int head_rotation(void* user, uint32_t handle, jpt_rot3* out) {
    const uint32_t instance = player_instance(handle);
    (void)user;
    if (!instance || !guest_readable(instance + PLAYER_HEAD, (uint32_t)sizeof *out)) return 0;
    memcpy(out, (const void*)ADDR(instance + PLAYER_HEAD), sizeof *out);
    return 1;
}

static int say(void* user, uint32_t handle, uint32_t sample) {
    const uint32_t instance = player_instance(handle);
    (void)user;
    return instance && sample && guest_call(PLAYER_SAY, instance, &sample, 1, 0, NULL);
}

static int health(void* user, uint32_t handle, float* hit_points, int* dead) {
    const uint32_t instance = instance_of(handle);
    uint32_t r = 0;
    (void)user;
    if (!animate(type_of(instance)) || !guest_readable(instance + ANIMATE_HIT_POINTS, 4u)) return 0;
    memcpy(hit_points, (const void*)ADDR(instance + ANIMATE_HIT_POINTS), sizeof *hit_points);
    if (!guest_call_virtual(instance, VF_DEAD, NULL, 0, &r)) return 0;
    *dead = (r & 0xFFu) != 0;
    return 1;
}

static uint32_t gun(uint32_t handle) {
    const uint32_t instance = instance_of(handle);
    return instance && !strcmp(type_of(instance), "CGun") && guest_readable(instance + GUN_MAX_AMMO, 8u) ? instance : 0;
}

static int gun_ammo(void* user, uint32_t handle, int* ammo, int* max_ammo) {
    const uint32_t instance = gun(handle);
    (void)user;
    if (!instance) return 0;
    *ammo = (int)MEM32(instance + GUN_AMMO);
    *max_ammo = (int)MEM32(instance + GUN_MAX_AMMO);
    return 1;
}

static int set_gun_ammo(void* user, uint32_t handle, int ammo, int max_ammo) {
    const uint32_t instance = gun(handle);
    (void)user;
    if (!instance || ammo < 0 || max_ammo < 0 || ammo > max_ammo) return 0;
    MEM32(instance + GUN_MAX_AMMO) = (uint32_t)max_ammo;
    MEM32(instance + GUN_AMMO) = (uint32_t)ammo;
    return 1;
}

static uint32_t text_overlay(void) {
    uint32_t overlay = 0;
    return guest_read32(TEXT_OVERLAY, &overlay) ? overlay : 0;
}

/* The overlay follows previous as a pointer: only a handle of an entry it holds now is passed on. */
static int held_text(uint32_t overlay, uint32_t handle) {
    uint32_t sentinel, node;
    int n = 0;
    if (!handle) return 1;
    if (!guest_read32(overlay + OVERLAY_LIST, &sentinel) || !sentinel || !guest_read32(sentinel, &node)) return 0;
    for (; node && node != sentinel && n < NODE_LIMIT; n++) {
        if (node + NODE_HANDLE == handle) return 1;
        if (!guest_read32(node, &node)) return 0;
    }
    return 0;
}

static uint32_t show_text(void* user, const char* text, float seconds, uint32_t format, uint32_t rgb, uint32_t previous, uint32_t type) {
    const uint32_t overlay = text_overlay(), chars = guest_scratch(2, JPT_TEXT_MAX);
    const size_t n = strlen(text);
    uint32_t args[6], handle = 0;
    (void)user;
    if (!overlay || !chars || n >= JPT_TEXT_MAX || !held_text(overlay, previous)) return 0;
    memcpy((void*)ADDR(chars), text, n + 1);
    args[0] = chars;
    memcpy(&args[1], &seconds, sizeof seconds);
    args[2] = format & JPT_TEXT_FORMATS;
    args[3] = rgb & 0xFFFFFFu;
    args[4] = previous;
    args[5] = type;
    return guest_call(TEXT_SHOW, overlay, args, 6, 0, &handle) ? handle : 0;
}

static uint32_t text_end(void* user, uint32_t type) {
    const uint32_t overlay = text_overlay();
    uint32_t handle = 0;
    (void)user;
    return overlay && guest_call(TEXT_END, overlay, &type, 1, 0, &handle) ? handle : 0;
}

static int safe_relative(const char* name) {
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':') || strstr(name, "..")) return 0;
    return strlen(name) < NAME_MAX - 8u;
}

static int find_picture(const char* relative, char* out, size_t cap) {
    return jpt_custom_level_find_in_game(relative, out, cap) || jpt_custom_level_find(relative, out, cap);
}

/* A picture of the package by a path relative to its game paths; without an extension, as CE's loader the .tga, .dds or .bmp. */
static uint8_t* load_texture(void* user, const char* name, int* width, int* height) {
    static const char* const kinds[] = { "", ".tga", ".dds", ".bmp" };
    const char* base = name ? strrchr(name, '/') : NULL;
    const char* back = name ? strrchr(name, '\\') : NULL;
    char relative[NAME_MAX], host[JPT_PATH_MAX];
    jpt_picture_t picture;
    (void)user;
    if (!safe_relative(name)) return NULL;
    if (back && (!base || back > base)) base = back;
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        uint8_t* rgba;
        if (k && strchr(base ? base : name, '.')) break;
        snprintf(relative, sizeof relative, "%s%s", name, kinds[k]);
        memset(&picture, 0, sizeof picture);
        if (!find_picture(relative, host, sizeof host) || !jpt_picture_load(host, &picture)) continue;
        rgba = (uint8_t*)malloc((size_t)picture.width * picture.height * 4u);
        if (rgba) {
            for (size_t i = 0; i < (size_t)picture.width * picture.height; i++) {
                rgba[4 * i] = picture.bgra[4 * i + 2];
                rgba[4 * i + 1] = picture.bgra[4 * i + 1];
                rgba[4 * i + 2] = picture.bgra[4 * i];
                rgba[4 * i + 3] = picture.bgra[4 * i + 3];
            }
            *width = (int)picture.width;
            *height = (int)picture.height;
        }
        jpt_picture_free(&picture);
        return rgba;
    }
    return NULL;
}

static int screen_size(void* user, int* width, int* height) {
    uint32_t raster = 0, w = 0, h = 0;
    (void)user;
    if (!guest_read32(RASTER_PTR, &raster) || !raster || !guest_read32(raster + RASTER_WIDTH, &w) || !guest_read32(raster + RASTER_HEIGHT, &h) ||
        !w || !h || w > 16384u || h > 16384u)
        return 0;
    *width = (int)w;
    *height = (int)h;
    return 1;
}

/* Left and right Shift, Control and Alt are the same command keys. */
static unsigned plain_key(unsigned vk) {
    if (vk == 0xA0u || vk == 0xA1u) return 0x10u;
    if (vk == 0xA2u || vk == 0xA3u) return 0x11u;
    if (vk == 0xA4u || vk == 0xA5u) return 0x12u;
    return vk;
}

void script_engine_configure(const uint32_t user_vk[JPT_SCRIPT_USER_KEYS]) {
    memcpy(S.user_vk, user_vk, sizeof S.user_vk);
}

int script_engine_key(unsigned vk, int down, int repeat) {
    const unsigned used = jpt_custom_level_active() ? jpt_level_scripts_user_commands() : 0u, key = plain_key(vk);
    if (!used) return 0;
    for (unsigned n = 0; n < JPT_SCRIPT_USER_KEYS; n++) {
        const uint32_t bit = 1u << n;
        if (!(used & bit) || !S.user_vk[n] || S.user_vk[n] != key) continue;
        if (down && !repeat && !(S.held & bit)) S.hit |= bit;
        if (down) S.held |= bit;
        else S.held &= ~bit;
    }
    return jpt_level_scripts_key(key, down, host_key_held(0x10), host_key_held(0x11), host_key_held(0x12));
}

void script_engine_input(uint32_t sinput) {
    jpt_script_input in;
    jpt_script_input_result out;
    if (!jpt_custom_level_active() || !jpt_level_scripts_user_commands()) S.held = S.hit = 0;
    if (!jpt_custom_level_active() || !guest_readable(sinput, SINPUT_HIT + (uint32_t)sizeof(uint32_t))) return;
    in.user_held = S.held;
    in.user_hit = S.hit;
    memcpy(&in.move_x, (const void*)ADDR(sinput + SINPUT_MOVE_X), sizeof in.move_x);
    memcpy(&in.move_y, (const void*)ADDR(sinput + SINPUT_MOVE_Y), sizeof in.move_y);
    S.hit = 0;
    jpt_level_scripts_input(&in, &out);
    if (!out.modal) return;
    MEM32(sinput + SINPUT_MOVE_X) = MEM32(sinput + SINPUT_MOVE_Y) = 0;
    MEM32(sinput + SINPUT_STATE) = MEM32(sinput + SINPUT_HIT) = 0;
    if (!out.look) MEM32(sinput + SINPUT_ROTATE_X) = MEM32(sinput + SINPUT_ROTATE_Y) = 0;
}

/* The animal whose brain the AI is thinking with, as the activity's own code finds it. */
static uint32_t thinking_animal(void) {
    uint32_t system, brain, animal;
    if (!guest_read32(AI_SYSTEM, &system) || !system || !guest_read32(system + SYSTEM_BRAIN, &brain) || !brain ||
        !guest_read32(brain + BRAIN_ANIMAL, &animal) || !animal)
        return 0;
    return strcmp(type_of(animal), "CAnimal") == 0 && MEM32(animal + ANIMAL_BRAIN) == brain ? animal : 0;
}

static void act(recomp_func_t lifted, int feint) {
    const uint32_t influence = g_alias_entry ? 0 : GUEST_ARG(0), animal = influence ? thinking_animal() : 0;
    float location[3], distance = 0.0f;
    uint32_t target = 0;
    lifted();
    if (!animal || !guest_readable(influence + INFLUENCE_LOCATION, INFLUENCE_TARGET - INFLUENCE_LOCATION + (uint32_t)sizeof(uint32_t))) return;
    memcpy(location, (const void*)ADDR(influence + INFLUENCE_LOCATION), sizeof location);
    memcpy(&distance, (const void*)ADDR(influence + INFLUENCE_DISTANCE), sizeof distance);
    target = MEM32(influence + INFLUENCE_TARGET);
    jpt_level_scripts_act_bite(handle_of(animal), handle_of(target), location, distance, feint);
}

static void bite_act(void) { act(S.bite, 0); }
static void feint_act(void) { act(S.feint, 1); }

static float read_float(uint32_t va) {
    float f;
    memcpy(&f, (const void*)ADDR(va), sizeof f);
    return f;
}

static void write_float(uint32_t va, float f) { memcpy((void*)ADDR(va), &f, sizeof f); }

/* Where CE asks the player's script: the walk speed of every request, the jump scale only of one that asks to jump. */
void GHOOK(0x00455C9E)(void) {
    const uint32_t request = g_ebp - PROCESS_REQUEST;
    uint32_t instance = 0, handle;
    float speed, factor;
    S.jump_scaled = 0;
    if (!jpt_custom_level_active() || !jpt_level_scripts_drives_player() || !guest_readable(request, REQUEST_JUMP_SCALE + (uint32_t)sizeof(float)) ||
        !guest_read32(PLAYER, &instance) || !instance)
        return;
    handle = handle_of(instance);
    speed = read_float(request + REQUEST_SPEED);
    factor = jpt_level_scripts_speed_factor(handle, speed);
    if (factor != speed) write_float(request + REQUEST_SPEED, factor);
    if (read_float(request + REQUEST_JUMP_AMOUNT) == 0.0f) return;
    S.jump_scale = jpt_level_scripts_jump_factor(handle);
    S.jump_scaled = S.jump_scale >= 0.0f && S.jump_scale <= FLT_MAX;
    if (S.jump_scaled) write_float(request + REQUEST_JUMP_SCALE, S.jump_scale);
}

/* The hand-off just armed the jump impulse with z = 1; CE arms it with the request's scale, the script's jump factor. */
void GHOOK(0x004F0C00)(void) {
    if (!S.jump_scaled || !jpt_custom_level_active()) return;
    S.jump_scaled = 0;
    write_float(JUMP_Z, S.jump_scale);
}

/* The pelvis jumped for the player's own request: OnJump first, then the vocal, as CE. */
static void jump_voice(void) {
    uint32_t instance = 0;
    if (!g_alias_entry && guest_read32(PLAYER, &instance) && instance) jpt_level_scripts_jump(handle_of(instance));
    S.jump_voice();
}

void script_engine_install(void) {
    if (!jpt_custom_level_active()) return;
    S.engine.native_type = native_type;
    S.engine.find = find;
    S.engine.placement = placement;
    S.engine.head_position = head_position;
    S.engine.teleport = teleport;
    S.engine.fixed_velocity = fixed_velocity;
    S.engine.sim_time = sim_time;
    S.engine.player = player;
    S.engine.held_object = held_object;
    S.engine.health = health;
    S.engine.gun_ammo = gun_ammo;
    S.engine.set_gun_ammo = set_gun_ammo;
    S.engine.show_text = show_text;
    S.engine.text_end = text_end;
    S.engine.load_texture = load_texture;
    S.engine.screen_size = screen_size;
    S.engine.head_rotation = head_rotation;
    S.engine.say = say;
    jpt_level_scripts_set_engine(&S.engine);
    if (!guest_override_install(BITE_ACT, bite_act, &S.bite) || !guest_override_install(FEINT_ACT, feint_act, &S.feint))
        host_log("custom-level", "warning: the AI's bite and feint actions could not be hooked: OnActBite is not called");
    if (!guest_override_install(JUMP_VOICE, jump_voice, &S.jump_voice))
        host_log("custom-level", "warning: the player's jump vocal could not be hooked: OnJump is not called");
}
