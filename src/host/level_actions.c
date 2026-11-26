/* The ActionTypes of ATX and Trespasser CE, for a custom level that has them. A trigger holds each as one of the engine's own
 * CSetHintAction objects, marked: its Start, Load and Save are overridden. */
#include "level_actions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_level_scripts.h"
#include "jpt_picture.h"
#include "jpt_save.h"
#include "level_actions_policy.h"
#include "level_session.h"

#define ACTION_FACTORY GVA(0x0046BE90)  /* (trigger, name, group, table, loader), cdecl */
#define ADD_ACTION GVA(0x0046C6D0)      /* CTrigger::AddAction(action), thiscall */
#define HINT_START GVA(0x00467BB0)      /* CSetHintAction */
#define HINT_LOAD GVA(0x00467BC0)
#define HINT_SAVE GVA(0x00467BE0)
#define HINT_VTABLE GVA(0x0064AF64)
#define LOAD_WORLD GVA(0x00606630)      /* CWorld::bLoadScene(path, callback, user), thiscall */
#define PURGE_WORLD GVA(0x00604DD0)
#define RESTART_LEVEL GVA(0x00414480)
#define LOAD_LEVEL GVA(0x004142C0)
#define MENU_SOUNDS_OFF GVA(0x00414D80)
#define LEVEL_PATH GVA(0x00411B30)      /* (kind, out, bytes), cdecl */
#define OPERATOR_NEW GVA(0x00626B8C)
#define FIND_VALUE GVA(0x0041AD50)      /* (symbol, group, table, 0), cdecl */
#define NAME_HASH GVA(0x005F6100)
#define FIND_INSTANCE GVA(0x00607C10)
#define TELEPORT GVA(0x00470A70)        /* CTeleportAction::Start */
#define TELEPORT_VTABLE GVA(0x0064B990)
#define PARTICLES_HASH GVA(0x0043DA80)
#define FIND_PARTICLES GVA(0x006230F0)
#define PARTICLES_EFFECT GVA(0x0053E350)
#define GUN_VTABLE GVA(0x0064A274)
#define SET_PHYSICS GVA(0x00466900)     /* CSetPhysicsAction::Start */
#define UNIQUE_AI_INFO GVA(0x00611D50)  /* CInfo, thiscall: its AI info, its own */
#define USE_VTABLE GVA(0x0064ADF4)      /* CMessageUse */
#define USE_RECEIVERS GVA(0x0073F278)
#define USE_COUNT GVA(0x0073F280)
#define AMMO_FLAGS GVA(0x0066ECC0)      /* g_bUnlimitedAmmo is its second byte */
#define PAUSE_GAME GVA(0x0040BCC0)      /* CGameWnd, thiscall */
#define RESUME_GAME GVA(0x0040BD70)     /* CGameWnd(continue), thiscall */
#define VIDEO_WND_CTOR GVA(0x00418700)  /* CVideoWnd(window manager), thiscall */
#define VIDEO_WND_PLAY GVA(0x00418AE0)
#define VIDEO_WND_DTOR GVA(0x00418760)
#define APP GVA(0x0066E338)
#define WORLD GVA(0x0073F444)
#define PLAYER GVA(0x0066EF64)
#define CURRENT_HINT GVA(0x0066F01C)
#define HINT_STRINGS 18000u
#define GAME_SPEED GVA(0x0066B6FC)
#define GAME_TIME GVA(0x0073F34C)

#define SYMBOLS 0x2A8u
#define SYM(index) (SYMBOLS + 8u * (index))
#define SYM_A(n) SYM(89u + (n))
enum {
    S_TARGET = 38, S_ACTION_TYPE = 403, S_SAMPLE = 404, S_LOOP_COUNT = 416, S_X_POS = 423, S_Y_POS = 424, S_HEIGHT_RELATIVE = 463, S_ON_TERRAIN = 464, S_SET_POSITION = 465, S_SET_ORIENTATION = 466,
    S_TELEPORT_DEST = 467, S_X = 490, S_Y = 491, S_Z = 492, S_X_FREE = 511, S_Y_FREE = 512, S_Z_FREE = 513, S_INTERVAL = 518, S_FRAME = 581, S_R = 659, S_G = 660, S_B = 661,
    S_AMMO = 1194, S_ALT_AMMO_COUNT = 1198, S_PUSH = 1208, S_SPEED = 1262, S_THRESHOLD = 1863
};
enum { V_BOOL = 1, V_INT = 3, V_FLOAT = 4, V_STRING = 5, V_GROUP = 6 };
#define VALUE_TYPE 4u
#define VALUE_DATA 0x28u
#define VALUE_STRING_LENGTH (VALUE_DATA + 4u)
#define VALUE_STRING_CHARS (VALUE_DATA + 24u)
#define ACTION_REFERENCES 4u
#define ACTION_MARK 8u
#define ACTION_BYTES 0xCu
#define MARK 0x41545831u
#define TRIGGER_ACTION_COUNT 0x9Cu
#define TRIGGER_ACTIONS_MAX 16u
#define APP_SCENE 0xAu
#define INSTANCE_POSITION 0x30u
#define INSTANCE_ROTATION 0x3Cu
#define INSTANCE_INFO 0x28u
#define INFO_FLAGS 0x4u
#define INFO_SHAPE 0x8u
#define INFO_PHYSICS 0xCu
#define INFO_AI 0x10u
#define INFO_OWN 1u
#define AI_INFO_TYPE 0x8u
#define VF_CLONE_INFO 0u
#define VF_DELETE 0x4u
#define VF_SEND 0xCu
#define VF_AS_TRIGGER 0x44u
#define VF_AS_ANIMAL 0x64u
#define VF_ANIMATES 0x58u
#define VF_USE 0x170u
#define ANIMATE_HP 0x50u
#define ANIMATE_HP_MAX 0x54u
#define ANIMATE_PARTS 0xACu
#define ANIMATE_PART_COUNT 8u
#define TRIGGER_FIRE_COUNT 0xA8u
#define TRIGGER_PROBABILITY 0xACu
#define TRIGGER_FIRES_AT_ZERO 0xB8u
#define TRIGGER_RESET_COUNT 0xBCu
#define MESH_MATERIALS 0x38u
#define MESH_TEXTURES 0x3Cu
#define MESH_FRAMES 0xA8u
#define MESH_FRAME_TEXTURES 0xACu
#define PARTICLES_COLOUR 0x4u
#define WND_OWNER 0x4u
#define VIDEO_WND_STACK 0x200u
#define JUMP_FROM_BELOW 100.0f
#define GUN_MAX_AMMO 0x68u
#define GUN_AMMO 0x6Cu
#define PLAYER_HAND_OBJECT 0xE34u
#define PLAYER_BELT 0xE38u
#define PLAYER_BACK 0xE3Cu
#define GUNS_MAX 200
#define SCRATCH_SLOT 1u
#define LOG_LIMIT 64
#define STEP_LONGEST 0.5f
#define COLOURS_MAX 32

/* The engine's CTeleportAction. */
typedef struct {
    uint32_t vtable, references, name;
    float position[3], rotation[4];
    uint8_t height_relative, on_terrain, set_position, set_orientation;
} teleport_action_t;

/* The engine's CSetPhysicsAction. */
typedef struct {
    uint32_t vtable, references;
    float velocity[3];
    uint32_t frozen, target, impulse;
    float position[3], facing[3], push;
    uint32_t emitter;
} physics_action_t;

/* The engine's CMessageUse. */
typedef struct {
    uint32_t vtable, sent, received, receivers, instance;
} use_message_t;

typedef struct {
    uint32_t destination;
    float position[3], rotation[4];
    uint8_t valid, set_position, set_orientation, on_terrain, height_relative;
} place_t;

typedef struct {
    int instances, groups;
    uint32_t* names;
    int* weights;
    uint8_t* shuffled;
    place_t* places; /* groups x instances */
} teleport_t;

typedef struct {
    uint32_t object;
    int type, ordinal, valid, once;
    int numbers[4];
    float floats[4];
    uint32_t hash, instance;
    uint32_t* guns;
    int gun_count, has_guns;
    char* text;
    teleport_t teleport;
} action_t;

/* What the actions changed in the game's own objects, to be as it was when a level starts again. */
typedef struct {
    uint32_t instance, mesh, material, original, now; /* original: a reference of it is held */
} swapped_t;

typedef struct {
    uint32_t name, object, original, now;
} changed_t;

static struct {
    swapped_t textures[LA_SWAPS_MAX];
    changed_t triggers[LA_RESETS_MAX], colours[COLOURS_MAX];
    int texture_count, trigger_count, colour_count;
    uint32_t player, types[ANIMATE_PART_COUNT];
    unsigned parts;
} U;

static struct {
    recomp_func_t factory, start, load, save, load_world, restart;
    action_t* actions;
    int count, cap, stale, logs;
    la_state_t state;
    int32_t initial[LA_RUNS_MAX];
    uint32_t seed;
    float last_time;
    int speed_changed, playing;
    int32_t scene_ai;
    char video[LA_VIDEO_MAX];
    minimap_picture_t map;
    uint32_t map_cells[MM_PICTURE_SIDE * MM_PICTURE_SIDE];
    char map_loaded[LA_MAP_NAME_MAX];
    uint32_t map_serial;
} A;

static void note(const char* fmt, int type, const char* what) {
    if (A.logs >= LOG_LIMIT) return;
    if (++A.logs == LOG_LIMIT) host_log("custom-level", "later action messages are not logged");
    else host_log("custom-level", fmt, type, what);
}

static void release(action_t* a) {
    free(a->guns);
    free(a->text);
    free(a->teleport.names);
    free(a->teleport.weights);
    free(a->teleport.shuffled);
    free(a->teleport.places);
    memset(a, 0, sizeof *a);
}

static void release_all(void) {
    for (int i = 0; i < A.count; i++) release(&A.actions[i]);
    A.count = 0;
    A.state.run_count = 0;
}

static action_t* action_of(uint32_t object) {
    uint32_t mark;
    if (!object || !guest_read32(object + ACTION_MARK, &mark) || mark != MARK) return NULL;
    for (int i = 0; i < A.count; i++)
        if (A.actions[i].object == object) return &A.actions[i];
    return NULL;
}

/* ---- the trigger's properties ---- */

typedef struct {
    uint32_t group, table, loader;
} source_t;

static uint32_t value_of(const source_t* s, uint32_t symbol, uint32_t type) {
    const uint32_t args[4] = { s->loader + symbol, s->group, s->table, 0 };
    uint32_t value = 0, found;
    if (!s->group || !guest_call(FIND_VALUE, 0, args, 4, 1, &value) || !value) return 0;
    return guest_read32(value + VALUE_TYPE, &found) && found == type ? value : 0;
}

static int int_of(const source_t* s, uint32_t symbol, int fallback) {
    const uint32_t v = value_of(s, symbol, V_INT);
    return v ? (int)MEM32(v + VALUE_DATA) : fallback;
}

static int float_in(const source_t* s, uint32_t symbol, float* out) {
    const uint32_t v = value_of(s, symbol, V_FLOAT);
    if (v) memcpy(out, (const void*)ADDR(v + VALUE_DATA), 4);
    return v != 0;
}

static float float_of(const source_t* s, uint32_t symbol, float fallback) {
    float_in(s, symbol, &fallback);
    return fallback;
}

static int bool_of(const source_t* s, uint32_t symbol, int fallback) {
    const uint32_t v = value_of(s, symbol, V_BOOL);
    return v ? MEM8(v + VALUE_DATA) != 0 : fallback;
}

static int group_of(const source_t* s, uint32_t symbol, source_t* out) {
    *out = *s;
    out->group = value_of(s, symbol, V_GROUP);
    return out->group != 0;
}

/* The guest address of its characters; 0 when it is missing or empty. */
static uint32_t string_of(const source_t* s, uint32_t symbol) {
    const uint32_t v = value_of(s, symbol, V_STRING);
    uint32_t chars;
    if (!v || !MEM32(v + VALUE_STRING_LENGTH) || !guest_read32(v + VALUE_STRING_CHARS, &chars) || !chars) return 0;
    return MEM8(chars) ? chars : 0;
}

static uint32_t name_hash(uint32_t chars) {
    const uint32_t args[3] = { chars, 0, 1 };
    uint32_t hash = 0;
    return chars && guest_call(NAME_HASH, 0, args, 3, 1, &hash) ? hash : 0;
}

static uint32_t instance_of(uint32_t hash) {
    uint32_t world, instance = 0;
    return hash && guest_read32(WORLD, &world) && world && guest_call(FIND_INSTANCE, world, &hash, 1, 0, &instance) ? instance : 0;
}

/* A frame of the guest's stack: calls may come back here, and each has its own. */
static uint32_t push(const void* data, uint32_t size) {
    g_esp -= size;
    if (data) memcpy((void*)ADDR(g_esp), data, size);
    return g_esp;
}

static uint32_t cast(uint32_t object, uint32_t slot) {
    const uint32_t esp = g_esp, zero = 0, out = push(&zero, sizeof zero);
    const uint32_t result = object && guest_call_virtual(object, slot, &out, 1, NULL) ? MEM32(out) : 0;
    g_esp = esp;
    return result;
}

static uint32_t info_of(uint32_t instance) { return instance ? MEM32(instance + INSTANCE_INFO) : 0; }

static void drop_reference(uint32_t object) {
    if (--MEM32(object + ACTION_REFERENCES) == 0) guest_call_virtual(object, VF_DELETE, NULL, 0, NULL);
}

/* ---- what each type reads ---- */

static void read_guns(action_t* a, const source_t* s) {
    uint32_t chars;
    a->numbers[0] = int_of(s, SYM(S_AMMO), -1);
    a->numbers[1] = int_of(s, SYM(S_ALT_AMMO_COUNT), 0);
    a->guns = (uint32_t*)calloc(GUNS_MAX, sizeof *a->guns);
    for (uint32_t i = 0; a->guns && i < GUNS_MAX && (chars = string_of(s, SYM_A(i))) != 0; i++) {
        const uint32_t hash = name_hash(chars);
        if (hash) a->guns[a->gun_count++] = hash;
    }
}

static void read_reload(action_t* a, const source_t* s) {
    a->numbers[2] = int_of(s, SYM(S_LOOP_COUNT), -1);
    a->numbers[3] = int_of(s, SYM(S_FRAME), 1);
    read_guns(a, s);
    a->valid = a->guns != NULL;
}

static void read_video(action_t* a, const source_t* s) {
    char sample[LA_TEXT_MAX] = "";
    const uint32_t chars = string_of(s, SYM(S_SAMPLE));
    if (chars) guest_read_string(chars, sample, sizeof sample);
    a->text = (char*)malloc(LA_VIDEO_MAX);
    a->valid = a->text && la_video_name(sample, a->text);
    a->once = bool_of(s, SYM(S_LOOP_COUNT), 0);
}

static void read_fixed_speed(action_t* a, const source_t* s) {
    a->hash = name_hash(string_of(s, SYM(S_SAMPLE)));
    a->floats[0] = float_of(s, SYM(S_X), 0.0f);
    a->floats[1] = float_of(s, SYM(S_Y), 0.0f);
    a->floats[2] = float_of(s, SYM(S_Z), 0.0f);
    a->floats[3] = float_of(s, SYM(S_SPEED), 0.0f);
    a->numbers[0] = int_of(s, SYM(S_INTERVAL), -1);
    a->numbers[1] = bool_of(s, SYM(S_FRAME), 1);
    a->instance = name_hash(string_of(s, SYM(S_TELEPORT_DEST)));
    a->valid = a->hash != 0;
}

static void read_use(action_t* a, const source_t* s) {
    a->instance = name_hash(string_of(s, SYM(S_TARGET)));
    a->numbers[0] = bool_of(s, SYM(S_SAMPLE), 0);
    a->numbers[1] = bool_of(s, SYM(S_LOOP_COUNT), 0);
    a->numbers[2] = bool_of(s, SYM(S_INTERVAL), 0);
    a->numbers[3] = bool_of(s, SYM(S_FRAME), 0);
    a->valid = a->instance != 0;
}

static void read_trigger(action_t* a, const source_t* s) {
    a->instance = name_hash(string_of(s, SYM_A(0)));
    a->numbers[0] = int_of(s, SYM(S_SAMPLE), 0);
    a->numbers[1] = int_of(s, SYM(S_LOOP_COUNT), 100);
    a->numbers[2] = bool_of(s, SYM(S_FRAME), 0);
    a->valid = a->instance && value_of(s, SYM(S_SAMPLE), V_INT);
}

static void read_hint(action_t* a, const source_t* s) {
    char parts[2][LA_TEXT_MAX] = { "", "" };
    const uint32_t chars[2] = { string_of(s, SYM(S_SAMPLE)), string_of(s, SYM(S_FRAME)) };
    for (int i = 0; i < 2; i++)
        if (chars[i]) guest_read_string(chars[i], parts[i], sizeof parts[i]);
    a->text = (char*)malloc(LA_TEXT_MAX);
    a->valid = a->text && la_hint_join(parts[0], parts[1], a->text, LA_TEXT_MAX);
}

static void read_map(action_t* a, const source_t* s) {
    char sample[LA_TEXT_MAX] = "";
    const uint32_t chars = string_of(s, SYM(S_SAMPLE));
    if (chars) guest_read_string(chars, sample, sizeof sample);
    a->text = (char*)malloc(LA_MAP_NAME_MAX);
    a->valid = a->text && la_map_name(sample, a->text);
    a->floats[0] = float_of(s, SYM(S_X_POS), 0.0f);
    a->floats[1] = float_of(s, SYM(S_Y_POS), 0.0f);
    a->floats[2] = float_of(s, SYM(S_X), 0.0f);
    a->floats[3] = float_of(s, SYM(S_Y), 0.0f);
}

static void read_place(place_t* p, const source_t* s) {
    float degrees[3] = { 0.0f, 0.0f, 0.0f };
    const uint32_t chars = string_of(s, SYM(S_TELEPORT_DEST));
    p->set_position = (uint8_t)bool_of(s, SYM(S_SET_POSITION), 1);
    p->set_orientation = (uint8_t)bool_of(s, SYM(S_SET_ORIENTATION), 1);
    p->on_terrain = (uint8_t)bool_of(s, SYM(S_ON_TERRAIN), 1);
    p->height_relative = (uint8_t)bool_of(s, SYM(S_HEIGHT_RELATIVE), 1);
    p->destination = chars ? name_hash(chars) : 0;
    p->valid = p->set_position || p->set_orientation;
    if (!p->destination) {
        const int placed = float_in(s, SYM(S_X), &p->position[0]) & float_in(s, SYM(S_Y), &p->position[1]) & float_in(s, SYM(S_Z), &p->position[2]);
        const int turned = float_in(s, SYM(S_X_FREE), &degrees[0]) & float_in(s, SYM(S_Y_FREE), &degrees[1]) & float_in(s, SYM(S_Z_FREE), &degrees[2]);
        if ((p->set_position && !placed) || (p->set_orientation && !turned)) p->valid = 0;
    }
    la_quaternion(degrees[0], degrees[1], degrees[2], p->rotation);
}

static int in_range(int n) { return n >= 1 && n <= LA_ENTRIES_MAX; }

static void read_teleport(action_t* a, const source_t* s) {
    teleport_t* t = &a->teleport;
    source_t head, group, entry;
    if (!group_of(s, SYM_A(0), &head)) return;
    t->groups = int_of(&head, SYM(S_FRAME), 0);
    t->instances = int_of(&head, SYM(S_SAMPLE), 0);
    if (!in_range(t->groups) || !in_range(t->instances)) return;
    t->names = (uint32_t*)calloc((size_t)t->instances, sizeof *t->names);
    t->weights = (int*)calloc((size_t)t->groups, sizeof *t->weights);
    t->shuffled = (uint8_t*)calloc((size_t)t->groups, 1);
    t->places = (place_t*)calloc((size_t)t->groups * (size_t)t->instances, sizeof *t->places);
    if (!t->names || !t->weights || !t->shuffled || !t->places) return;
    for (int i = 0; i < t->instances; i++)
        if ((t->names[i] = name_hash(string_of(&head, SYM_A(1u + (uint32_t)i)))) == 0) return;
    for (int g = 0; g < t->groups; g++) {
        if (!group_of(s, SYM_A(1u + (uint32_t)g), &group)) continue;
        t->weights[g] = int_of(&group, SYM(S_LOOP_COUNT), 0);
        t->shuffled[g] = (uint8_t)bool_of(&group, SYM(S_R), 0);
        for (int i = 0; i < t->instances; i++)
            if (group_of(&group, SYM_A(1u + (uint32_t)i), &entry)) read_place(&t->places[g * t->instances + i], &entry);
    }
    a->valid = la_pick_group(t->weights, t->groups, 0) >= 0;
}

static void read_particles(action_t* a, const source_t* s) {
    const uint32_t particles = string_of(s, SYM_A(0)), emitter = string_of(s, SYM(S_SAMPLE));
    a->floats[0] = float_of(s, SYM(S_FRAME), 1.0f);
    a->floats[1] = float_of(s, SYM(S_THRESHOLD), 0.5f);
    a->instance = name_hash(emitter);
    a->valid = particles && a->instance && guest_call(PARTICLES_HASH, 0, &particles, 1, 1, &a->hash);
}

static void read_action(action_t* a, const source_t* s) {
    switch (a->type) {
    case LA_SLOW:
        a->floats[0] = float_of(s, SYM(S_SPEED), 0.3f);
        a->numbers[0] = int_of(s, SYM(S_INTERVAL), 4500);
        a->numbers[1] = int_of(s, SYM(S_X), 1200);
        a->numbers[2] = int_of(s, SYM(S_Y), 1200);
        a->once = bool_of(s, SYM(S_FRAME), 0);
        a->valid = a->numbers[0] > 0;
        break;
    case LA_SPEED:
        a->floats[0] = float_of(s, SYM(S_SPEED), 0.4f);
        a->once = bool_of(s, SYM(S_FRAME), 0);
        a->valid = 1;
        break;
    case LA_MAP: read_map(a, s); break;
    case LA_RELOAD: read_reload(a, s); break;
    case LA_HINT: read_hint(a, s); break;
    case LA_CONTROLS:
        a->numbers[0] = int_of(s, SYM(S_FRAME), -1);
        a->numbers[1] = int_of(s, SYM(S_LOOP_COUNT), -1);
        a->valid = 1;
        break;
    case LA_TELEPORT: read_teleport(a, s); break;
    case LA_PARTICLES: read_particles(a, s); break;
    case LA_HEALTH:
        a->floats[0] = float_of(s, SYM(S_FRAME), 0.0f);
        a->once = bool_of(s, SYM(S_LOOP_COUNT), 0);
        a->valid = 1;
        break;
    case LA_VIDEO: read_video(a, s); break;
    case LA_COLOUR: {
        const uint32_t particles = string_of(s, SYM(S_SAMPLE));
        a->numbers[0] = (int)la_colour(int_of(s, SYM(S_R), 0), int_of(s, SYM(S_G), 0), int_of(s, SYM(S_B), 0));
        a->valid = particles && guest_call(PARTICLES_HASH, 0, &particles, 1, 1, &a->hash) && a->hash;
        break;
    }
    case LA_FIX_SPEED: read_fixed_speed(a, s); break;
    case LA_END_SPEED:
        a->hash = name_hash(string_of(s, SYM(S_SAMPLE)));
        a->valid = a->hash != 0;
        break;
    case LA_PLAYER_AI:
        a->numbers[0] = int_of(s, SYM(S_FRAME), 5);
        a->valid = la_ai_valid(a->numbers[0]);
        break;
    case LA_USE: read_use(a, s); break;
    case LA_JUMP:
        a->instance = name_hash(string_of(s, SYM(S_TARGET)));
        a->floats[0] = float_of(s, SYM(S_PUSH), 0.0f);
        a->floats[1] = float_of(s, SYM(S_FRAME), 0.0f);
        a->valid = a->instance != 0;
        break;
    case LA_TEXTURE:
        a->instance = name_hash(string_of(s, SYM_A(0)));
        a->numbers[0] = int_of(s, SYM(S_SAMPLE), 1) - 1;
        a->numbers[1] = int_of(s, SYM(S_FRAME), 0);
        a->valid = a->instance && a->numbers[0] >= 0 && a->numbers[1] >= 0;
        break;
    case LA_TRIGGER: read_trigger(a, s); break;
    case LA_RELOAD_GUNS:
        read_guns(a, s);
        a->valid = a->gun_count > 0;
        break;
    default: break;
    }
    if (!a->valid && jpt_custom_level_action(a->type)) note("ActionType %d: a trigger of it lacks %s; it does nothing", a->type, "what it needs");
}

/* ---- what each type does ---- */

static void set_game_speed(void) {
    const float factor = la_speed_factor(&A.state.speed);
    memcpy((void*)ADDR(GAME_SPEED), &factor, 4);
    A.speed_changed = 1;
}

static int32_t* runs_of(const action_t* a) { return a->ordinal < LA_RUNS_MAX ? &A.state.runs[a->ordinal] : NULL; }

static int is_gun(uint32_t object) {
    uint32_t vtable;
    return object && guest_read32(object, &vtable) && vtable == GUN_VTABLE;
}

static int reload_gun(const action_t* a, uint32_t gun) {
    int listed = !a->gun_count;
    if (!is_gun(gun)) return 0;
    for (int i = 0; i < a->gun_count && !listed; i++) listed = instance_of(a->guns[i]) == gun;
    if (!listed) return 0;
    MEM32(gun + GUN_AMMO) = (uint32_t)la_reload_rounds(a->numbers[0], a->numbers[1], (int)MEM32(gun + GUN_MAX_AMMO));
    return 1;
}

static void start_reload(const action_t* a) {
    int32_t* left = runs_of(a);
    uint32_t player, hand, object;
    int done = 0;
    if ((left && *left == 0) || !guest_read32(PLAYER, &player) || !player) return;
    hand = MEM32(player + PLAYER_HAND_OBJECT);
    if (a->numbers[3] & 1) done |= reload_gun(a, hand);
    for (uint32_t slot = PLAYER_BELT; (a->numbers[3] & 2) && slot <= PLAYER_BACK; slot += 4u)
        if ((object = MEM32(player + slot)) != hand) done |= reload_gun(a, object);
    if (done && left && *left > 0) --*left;
}

static void teleport(uint32_t name, const place_t* p) {
    const uint32_t object = guest_scratch(SCRATCH_SLOT, sizeof(teleport_action_t)), destination = p->destination ? instance_of(p->destination) : 0;
    teleport_action_t action = { TELEPORT_VTABLE, 1, name, { 0 }, { 0 }, p->height_relative, p->on_terrain, p->set_position, p->set_orientation };
    if (!object || !p->valid || !instance_of(name)) return;
    if (p->destination && !destination) {
        note("ActionType %d: %s is not in the level; nothing is moved there", LA_TELEPORT, "a destination");
        return;
    }
    memcpy(action.position, destination ? (const void*)ADDR(destination + INSTANCE_POSITION) : (const void*)p->position, sizeof action.position);
    memcpy(action.rotation, destination ? (const void*)ADDR(destination + INSTANCE_ROTATION) : (const void*)p->rotation, sizeof action.rotation);
    memcpy((void*)ADDR(object), &action, sizeof action);
    guest_call(TELEPORT, object, NULL, 0, 0, NULL);
}

static void start_teleport(const action_t* a) {
    const teleport_t* t = &a->teleport;
    const int group = la_pick_group(t->weights, t->groups, la_random(&A.seed));
    uint8_t order[LA_ENTRIES_MAX];
    if (group < 0) return;
    for (int i = 0; i < t->instances; i++) order[i] = (uint8_t)i;
    for (int i = 0; i < t->instances; i++) {
        const int entry = t->shuffled[group] ? la_shuffle(order, t->instances - i, la_random(&A.seed)) : i;
        teleport(t->names[i], &t->places[group * t->instances + entry]);
    }
}

static void start_particles(const action_t* a) {
    const uint32_t find[2] = { a->hash, 0 }, instance = instance_of(a->instance);
    uint32_t particles = 0, args[8];
    float values[8], rotation[4];
    if (!instance) return;
    if (!guest_call(FIND_PARTICLES, 0, find, 2, 1, &particles) || !particles) {
        note("ActionType %d: the game has no %s of that name", LA_PARTICLES, "particles");
        return;
    }
    memcpy(values, (const void*)ADDR(instance + INSTANCE_POSITION), 3 * sizeof values[0]);
    memcpy(rotation, (const void*)ADDR(instance + INSTANCE_ROTATION), sizeof rotation);
    la_forward(rotation, values + 3);
    values[6] = a->floats[0];
    values[7] = a->floats[1];
    memcpy(args, values, sizeof args);
    guest_call(PARTICLES_EFFECT, particles, args, 8, 0, NULL);
}

static void start_health(const action_t* a) {
    float hp, most;
    uint32_t player;
    if (!guest_read32(PLAYER, &player) || !player) return;
    memcpy(&hp, (const void*)ADDR(player + ANIMATE_HP), 4);
    memcpy(&most, (const void*)ADDR(player + ANIMATE_HP_MAX), 4);
    hp = la_health(hp, most, a->floats[0]);
    memcpy((void*)ADDR(player + ANIMATE_HP), &hp, 4);
}

static uint32_t particles_of(uint32_t hash) {
    const uint32_t find[2] = { hash, 0 };
    uint32_t particles = 0;
    return guest_call(FIND_PARTICLES, 0, find, 2, 1, &particles) ? particles : 0;
}

static void start_colour(const action_t* a) {
    const uint32_t particles = particles_of(a->hash), colour = (uint32_t)a->numbers[0];
    int i = 0;
    if (!particles) return;
    while (i < U.colour_count && U.colours[i].object != particles) i++;
    if (i == COLOURS_MAX) return;
    if (i == U.colour_count) U.colours[U.colour_count++] = (changed_t){ a->hash, particles, MEM32(particles + PARTICLES_COLOUR), 0 };
    U.colours[i].now = MEM32(particles + PARTICLES_COLOUR) = colour;
}

static void start_fixed_speed(const action_t* a) {
    const uint32_t instance = instance_of(a->hash), source = a->instance ? instance_of(a->instance) : 0;
    float velocity[3] = { a->floats[0], a->floats[1], a->floats[2] };
    if (!instance || !MEM32(info_of(instance) + INFO_PHYSICS) || (a->instance && !source)) {
        note("ActionType %d: %s is not in the level, or has no physics", LA_FIX_SPEED, "an object");
        return;
    }
    if (source) {
        float rotation[4];
        memcpy(rotation, (const void*)ADDR(source + INSTANCE_ROTATION), sizeof rotation);
        la_forward(rotation, velocity);
        for (int i = 0; i < 3; i++) velocity[i] *= a->floats[3];
    }
    la_fixed_set(&A.state, a->hash, velocity, la_fixed_seconds(a->numbers[0]), a->numbers[1]);
}

static void set_physics(const physics_action_t* action) {
    const uint32_t esp = g_esp;
    guest_call(SET_PHYSICS, push(action, sizeof *action), NULL, 0, 0, NULL);
    g_esp = esp;
}

static void step_fixed_speeds(float seconds) {
    for (uint32_t i = 0; i < A.state.fixed_count;) {
        la_fixed_t* f = &A.state.fixed[i];
        const uint32_t name = f->instance, instance = instance_of(name);
        if (instance && MEM32(info_of(instance) + INFO_PHYSICS)) {
            physics_action_t action;
            memset(&action, 0, sizeof action);
            action.target = instance;
            memcpy(action.velocity, f->velocity, sizeof action.velocity);
            set_physics(&action);
        }
        if (!instance || !la_fixed_step(f, seconds)) la_fixed_end(&A.state, name);
        else i++;
    }
}

/* The AI info a part of the player has of its own; 0 for a part without one. */
static uint32_t own_ai_info(uint32_t part) {
    uint32_t info = info_of(part), ai = 0;
    if (!info || !MEM32(info + INFO_AI)) return 0;
    if (!(MEM32(info + INFO_FLAGS) & INFO_OWN)) {
        if (!guest_call_virtual(info, VF_CLONE_INFO, NULL, 0, &info) || !info) return 0;
        MEM32(part + INSTANCE_INFO) = info;
    }
    return guest_call(UNIQUE_AI_INFO, info, NULL, 0, 0, &ai) ? ai : 0;
}

static void set_player_ai(int type) {
    uint32_t player;
    int result = LA_AI_NONE;
    if (!guest_read32(PLAYER, &player) || !player) return;
    if (U.player != player) U.parts = 0;
    U.player = player;
    for (uint32_t i = 0; i < ANIMATE_PART_COUNT; i++) {
        const uint32_t part = MEM32(player + ANIMATE_PARTS + 4u * i), ai = part ? own_ai_info(part) : 0;
        if (!ai) continue;
        if (!(U.parts >> i & 1u)) U.types[i] = MEM32(ai + AI_INFO_TYPE);
        U.parts |= 1u << i;
        MEM32(ai + AI_INFO_TYPE) = (uint32_t)la_ai_type(type, (int)MEM32(ai + AI_INFO_TYPE));
        if (result == LA_AI_NONE) result = (int)MEM32(ai + AI_INFO_TYPE);
    }
    A.state.player_ai = result;
}

static void start_use(const action_t* a) {
    const uint32_t target = instance_of(a->instance), esp = g_esp, zero = 0;
    uint32_t unlimited;
    if (!target) {
        note("ActionType %d: %s is not in the level", LA_USE, "its object");
        return;
    }
    unlimited = MEM8(AMMO_FLAGS + 1u);
    if (a->numbers[0]) MEM8(AMMO_FLAGS + 1u) = 1;
    guest_call_virtual(target, VF_USE, &zero, 1, NULL);
    MEM8(AMMO_FLAGS + 1u) = (uint8_t)unlimited;
    if (a->numbers[1] && is_gun(target)) MEM32(target + GUN_AMMO) = (uint32_t)la_rounds_left((int)MEM32(target + GUN_AMMO), a->numbers[2]);
    if (!a->numbers[3]) {
        const use_message_t message = { USE_VTABLE, 0, 0, USE_RECEIVERS, target };
        MEM32(USE_COUNT) += 1u;
        guest_call_virtual(push(&message, sizeof message), VF_SEND, NULL, 0, NULL);
        g_esp = esp;
    }
}

static void start_jump(const action_t* a) {
    const uint32_t animal = cast(instance_of(a->instance), VF_AS_ANIMAL);
    physics_action_t action;
    float rotation[4], pitched[4];
    if (!animal) {
        note("ActionType %d: %s is not in the level", LA_JUMP, "its animal");
        return;
    }
    memset(&action, 0, sizeof action);
    memcpy(action.position, (const void*)ADDR(animal + INSTANCE_POSITION), sizeof action.position);
    memcpy(rotation, (const void*)ADDR(animal + INSTANCE_ROTATION), sizeof rotation);
    la_pitched(rotation, a->floats[1], pitched);
    la_forward(pitched, action.facing);
    action.position[2] -= JUMP_FROM_BELOW;
    action.target = animal;
    action.impulse = 1;
    action.push = a->floats[0];
    set_physics(&action);
}

static uint32_t shape_of(uint32_t instance) {
    const uint32_t info = info_of(instance);
    return info ? MEM32(info + INFO_SHAPE) : 0;
}

static int swap_texture(uint32_t name, uint32_t material, uint32_t frame) {
    const uint32_t mesh = shape_of(instance_of(name));
    uint32_t animates = 0, slot, texture, old;
    int i = 0;
    if (!mesh || !guest_call_virtual(mesh, VF_ANIMATES, NULL, 0, &animates) || !(animates & 0xFFu)) return 0;
    if (frame >= MEM32(mesh + MESH_FRAMES) || material >= MEM32(mesh + MESH_MATERIALS)) return 0;
    slot = MEM32(mesh + MESH_TEXTURES) + 4u * material;
    texture = MEM32(MEM32(mesh + MESH_FRAME_TEXTURES) + 4u * frame);
    old = MEM32(slot);
    while (i < U.texture_count && (U.textures[i].mesh != mesh || U.textures[i].material != material)) i++;
    if (i == LA_SWAPS_MAX) return 0;
    if (i == U.texture_count) {
        U.textures[U.texture_count++] = (swapped_t){ name, mesh, material, old, old };
        MEM32(old + ACTION_REFERENCES) += 1u;
    }
    U.textures[i].now = texture;
    MEM32(texture + ACTION_REFERENCES) += 1u;
    MEM32(slot) = texture;
    drop_reference(old);
    return 1;
}

static void start_texture(const action_t* a) {
    if (swap_texture(a->instance, (uint32_t)a->numbers[0], (uint32_t)a->numbers[1])) la_swap_set(&A.state, a->instance, a->numbers[0], a->numbers[1]);
    else note("ActionType %d: %s of that name, material or frame", LA_TEXTURE, "there is no animating mesh");
}

static void set_reset_count(uint32_t trigger, uint32_t name, int count) {
    int i = 0;
    while (i < U.trigger_count && U.triggers[i].name != name) i++;
    if (i == LA_RESETS_MAX) return;
    if (i == U.trigger_count) U.triggers[U.trigger_count++] = (changed_t){ name, trigger, MEM32(trigger + TRIGGER_RESET_COUNT), 0 };
    U.triggers[i].now = MEM32(trigger + TRIGGER_RESET_COUNT) = (uint32_t)count;
    la_reset_set(&A.state, name, count);
}

static void start_trigger(const action_t* a) {
    const uint32_t trigger = cast(instance_of(a->instance), VF_AS_TRIGGER);
    if (!trigger) {
        note("ActionType %d: %s is not in the level", LA_TRIGGER, "its trigger");
        return;
    }
    MEM32(trigger + TRIGGER_FIRE_COUNT) = (uint32_t)a->numbers[0];
    MEM32(trigger + TRIGGER_PROBABILITY) = la_probability(a->numbers[1]);
    if (a->numbers[2] && MEM8(trigger + TRIGGER_FIRES_AT_ZERO)) set_reset_count(trigger, a->instance, a->numbers[0]);
}

static void start_action(const action_t* a) {
    int32_t* runs = runs_of(a);
    if (!a->valid || !jpt_custom_level_action(a->type) || (a->once && runs && *runs)) return;
    if (a->once && runs) *runs = 1;
    switch (a->type) {
    case LA_SLOW:
        la_speed_slow(&A.state.speed, a->floats[0], (float)a->numbers[0], (float)a->numbers[1], (float)a->numbers[2]);
        set_game_speed();
        break;
    case LA_SPEED:
        la_speed_set(&A.state.speed, a->floats[0]);
        set_game_speed();
        break;
    case LA_MAP:
        snprintf(A.state.map, sizeof A.state.map, "%s", a->text);
        memcpy(A.state.map_corners, a->floats, sizeof A.state.map_corners);
        break;
    case LA_RELOAD: start_reload(a); break;
    case LA_HINT: snprintf(A.state.hint, sizeof A.state.hint, "%s", a->text); break;
    case LA_CONTROLS: la_controls_set(&A.state.controls, a->numbers[0], a->numbers[1]); break;
    case LA_TELEPORT: start_teleport(a); break;
    case LA_PARTICLES: start_particles(a); break;
    case LA_HEALTH: start_health(a); break;
    case LA_VIDEO: snprintf(A.video, sizeof A.video, "%s", a->text); break;
    case LA_COLOUR: start_colour(a); break;
    case LA_FIX_SPEED: start_fixed_speed(a); break;
    case LA_END_SPEED: la_fixed_end(&A.state, a->hash); break;
    case LA_PLAYER_AI: set_player_ai(a->numbers[0]); break;
    case LA_USE: start_use(a); break;
    case LA_JUMP: start_jump(a); break;
    case LA_TEXTURE: start_texture(a); break;
    case LA_TRIGGER: start_trigger(a); break;
    case LA_RELOAD_GUNS:
        for (int i = 0; i < a->gun_count; i++) reload_gun(a, instance_of(a->guns[i]));
        break;
    default: break;
    }
}

/* ---- the game's objects as they were ---- */

static void undo_textures(void) {
    for (int i = 0; i < U.texture_count; i++) {
        const swapped_t* s = &U.textures[i];
        uint32_t slot;
        if (shape_of(instance_of(s->instance)) != s->mesh || s->material >= MEM32(s->mesh + MESH_MATERIALS)) continue;
        slot = MEM32(s->mesh + MESH_TEXTURES) + 4u * s->material;
        if (MEM32(slot) != s->now) continue;
        MEM32(slot) = s->original;
        drop_reference(s->now);
    }
}

static void undo_player_ai(void) {
    uint32_t player;
    if (!U.parts || !guest_read32(PLAYER, &player) || player != U.player) return;
    for (uint32_t i = 0; i < ANIMATE_PART_COUNT; i++) {
        const uint32_t part = U.parts >> i & 1u ? MEM32(player + ANIMATE_PARTS + 4u * i) : 0, ai = part ? own_ai_info(part) : 0;
        if (ai) MEM32(ai + AI_INFO_TYPE) = U.types[i];
    }
}

static void undo(void) {
    undo_textures();
    undo_player_ai();
    for (int i = 0; i < U.trigger_count; i++) {
        const uint32_t trigger = cast(instance_of(U.triggers[i].name), VF_AS_TRIGGER);
        if (trigger && trigger == U.triggers[i].object && MEM32(trigger + TRIGGER_RESET_COUNT) == U.triggers[i].now) MEM32(trigger + TRIGGER_RESET_COUNT) = U.triggers[i].original;
    }
    for (int i = 0; i < U.colour_count; i++) {
        const uint32_t particles = particles_of(U.colours[i].name);
        if (particles == U.colours[i].object && MEM32(particles + PARTICLES_COLOUR) == U.colours[i].now) MEM32(particles + PARTICLES_COLOUR) = U.colours[i].original;
    }
    memset(&U, 0, sizeof U);
}

/* ---- the engine's side ---- */

static void add_action(uint32_t trigger, const source_t* s, int type) {
    const uint32_t size = ACTION_BYTES;
    uint32_t object = 0;
    action_t* a;
    if (MEM32(trigger + TRIGGER_ACTION_COUNT) >= TRIGGER_ACTIONS_MAX) return;
    if (A.count == A.cap) {
        action_t* grown = (action_t*)realloc(A.actions, (size_t)(A.cap ? A.cap * 2 : 64) * sizeof *grown);
        if (!grown) return;
        A.actions = grown;
        A.cap = A.cap ? A.cap * 2 : 64;
    }
    if (!guest_call(OPERATOR_NEW, 0, &size, 1, 1, &object) || !object) return;
    for (int i = 0; i < A.count; i++)
        if (A.actions[i].object == object) release(&A.actions[i]);
    a = &A.actions[A.count];
    memset(a, 0, sizeof *a);
    a->object = object;
    a->type = type;
    a->ordinal = A.count++;
    read_action(a, s);
    MEM32(object) = HINT_VTABLE;
    MEM32(object + ACTION_REFERENCES) = 1;
    MEM32(object + ACTION_MARK) = MARK;
    if (a->ordinal < LA_RUNS_MAX) {
        A.initial[a->ordinal] = A.state.runs[a->ordinal] = type == LA_RELOAD ? a->numbers[2] : 0;
        A.state.run_count = (uint32_t)a->ordinal + 1u;
    }
    guest_call(ADD_ACTION, trigger, &object, 1, 0, NULL);
}

static void factory(void) {
    const uint32_t trigger = GUEST_ARG(0);
    const source_t s = { GUEST_ARG(2), GUEST_ARG(3), GUEST_ARG(4) };
    int type;
    A.factory();
    if (g_alias_entry || !jpt_custom_level_active()) return;
    if (A.stale) {
        release_all();
        A.stale = 0;
    }
    type = int_of(&s, SYM(S_ACTION_TYPE), -1);
    if (la_creates(type)) add_action(trigger, &s, type);
}

static void hint_start(void) {
    const action_t* a = g_alias_entry ? NULL : action_of(g_ecx);
    if (!a) {
        if (!g_alias_entry) A.state.hint[0] = 0;
        A.start();
        return;
    }
    start_action(a);
    GUEST_RETURN_STDCALL(0);
}

/* What a scene of Trespasser CE keeps of an action, and what loading one does. */
static void load_action(const action_t* a, uint32_t at, uint32_t bytes) {
    int32_t* runs = runs_of(a);
    if (a->type == LA_COLOUR) start_action(a);
    if (bytes != 4) return;
    if ((a->type == LA_HEALTH || a->type == LA_VIDEO) && runs) *runs = MEM32(at) >> 1 & 1u;
    if (a->type == LA_PLAYER_AI && MEM32(at) <= INT32_MAX && la_ai_valid((int)MEM32(at)) && jpt_custom_level_action(a->type)) A.scene_ai = (int32_t)MEM32(at);
}

static void hint_load(void) {
    const action_t* a = g_alias_entry ? NULL : action_of(g_ecx);
    uint32_t at, bytes;
    if (!a) {
        A.load();
        return;
    }
    at = GUEST_ARG(0);
    bytes = la_scene_bytes(a->type, jpt_custom_level_scene_version());
    load_action(a, at, bytes);
    g_eax = at + bytes;
    GUEST_RETURN_STDCALL(1);
}

static void hint_save(void) {
    const action_t* a = g_alias_entry ? NULL : action_of(g_ecx);
    if (!a) {
        A.save();
        return;
    }
    g_eax = GUEST_ARG(0);
    GUEST_RETURN_STDCALL(1);
}

static void reset(void) {
    la_speed_reset(&A.state.speed);
    la_controls_reset(&A.state.controls);
    A.state.hint[0] = A.state.map[0] = A.video[0] = 0;
    A.state.fixed_count = A.state.swap_count = A.state.reset_count = 0;
    A.state.player_ai = A.scene_ai = LA_AI_NONE;
    memcpy(A.state.runs, A.initial, sizeof A.state.runs);
    if (A.speed_changed) set_game_speed();
    A.speed_changed = 0;
}

static void restore(const char* guest_path) {
    char path[JPT_PATH_MAX];
    uint8_t bytes[LA_STATE_MAX];
    la_state_t saved;
    size_t size;
    if (!jpt_save_is_name(guest_path) || !guest_host_path(guest_path, path, sizeof path)) return;
    size = jpt_save_read_state(path, bytes, sizeof bytes);
    if (!size || !la_state_read(&saved, bytes, size)) return;
    A.state.speed = saved.speed;
    A.state.controls = saved.controls;
    memcpy(A.state.hint, saved.hint, sizeof A.state.hint);
    memcpy(A.state.map, saved.map, sizeof A.state.map);
    memcpy(A.state.map_corners, saved.map_corners, sizeof A.state.map_corners);
    for (uint32_t i = 0; i < saved.run_count && i < A.state.run_count; i++) A.state.runs[i] = saved.runs[i];
    if (la_speed_factor(&A.state.speed) != 1.0f) set_game_speed();
    A.state.fixed_count = saved.fixed_count;
    memcpy(A.state.fixed, saved.fixed, sizeof A.state.fixed);
    for (uint32_t i = 0; i < saved.swap_count; i++)
        if (swap_texture(saved.swaps[i].instance, (uint32_t)saved.swaps[i].material, (uint32_t)saved.swaps[i].frame))
            la_swap_set(&A.state, saved.swaps[i].instance, saved.swaps[i].material, saved.swaps[i].frame);
    for (uint32_t i = 0; i < saved.reset_count; i++) {
        const uint32_t trigger = cast(instance_of(saved.resets[i].trigger), VF_AS_TRIGGER);
        if (trigger) set_reset_count(trigger, saved.resets[i].trigger, saved.resets[i].count);
    }
    if (saved.player_ai != LA_AI_NONE) set_player_ai(saved.player_ai);
}

static int exists(const char* guest_path) {
    char host[JPT_PATH_MAX];
    const char* name = strrchr(guest_path, '\\');
    return jpt_custom_level_find(name ? name + 1 : guest_path, host, sizeof host) || (guest_host_path(guest_path, host, sizeof host) && jpt_fs_exists(host));
}

static void load_world(void) {
    const uint32_t self = g_ecx, at = g_alias_entry ? 0 : GUEST_ARG(0);
    char path[JPT_PATH_MAX] = "";
    if (g_alias_entry || !jpt_custom_level_active()) {
        A.load_world();
        return;
    }
    if (at) guest_read_string(at, path, sizeof path);
    undo();
    reset();
    A.stale = 1;
    if (jpt_session_restart_purges() || (jpt_custom_level_requires(JPT_LEVEL_CAP_FULL_RESTART) && exists(path))) {
        guest_call(PURGE_WORLD, self, NULL, 0, 0, NULL);
        jpt_session_world_purged();
    }
    g_ecx = self;
    A.load_world();
    A.stale = 0;
    if ((int32_t)g_eax >= 0) {
        const uint32_t result = g_eax;
        if (A.scene_ai != LA_AI_NONE) set_player_ai(A.scene_ai);
        restore(path);
        g_eax = result;
    }
}

/* The game's Restart Level reads the scene again over the world it has, so the player, the objects and the triggers a custom level
 * changed stay as they are: a custom session loads the level whole instead, purged first, through the game's level load. */
static void game_restart(uint32_t self, const char* scene, jpt_level_source_t source) {
    jpt_session_level_begin(JPT_REASON_RESTART, scene, source);
    jpt_level_scripts_begin(JPT_REASON_RESTART, scene, NULL);
    g_ecx = self;
    A.restart();
    jpt_session_level_end((int32_t)g_eax >= 0, (int)(int32_t)g_eax);
    jpt_level_scripts_ready((int32_t)g_eax >= 0);
    jpt_session_restart_end((int32_t)g_eax >= 0);
}

static void restart(void) {
    const uint32_t self = g_ecx, scratch = guest_scratch(SCRATCH_SLOT, 2 * JPT_PATH_MAX);
    uint32_t args[3] = { 0, scratch, 0x104u }, result = 0;
    char scene[64] = "", own[JPT_PATH_MAX];
    jpt_level_source_t source;
    size_t used;
    if (g_alias_entry) {
        A.restart();
        return;
    }
    if (!guest_read_string(APP + APP_SCENE, scene, sizeof scene) || !scene[0]) {
        host_log("restart", "requested without a current scene: the game's own Restart Level");
        g_ecx = self;
        A.restart();
        return;
    }
    if (jpt_custom_level_active()) jpt_custom_level_restart(scene, own, sizeof own);
    source = level_session_source(scene);
    if (jpt_session_restart_begin(scene, source) != JPT_RESTART_FULL) {
        game_restart(self, scene, source);
        return;
    }
    if (!scratch || !guest_call(LEVEL_PATH, 0, args, 3, 1, NULL)) {
        host_log("restart", "the level's folder is unknown: the world is purged and read again without a loading screen");
        game_restart(self, scene, source);
        return;
    }
    used = strlen((const char*)ADDR(scratch));
    snprintf((char*)ADDR(scratch) + used, 0x104u - used, "%s", scene);
    snprintf((char*)ADDR(scratch + JPT_PATH_MAX), JPT_PATH_MAX, "%s", scene);
    args[0] = scratch;
    args[1] = scratch + JPT_PATH_MAX;
    if (!guest_call(MENU_SOUNDS_OFF, APP, NULL, 0, 0, NULL) || !guest_call(LOAD_LEVEL, APP, args, 2, 0, &result)) result = 0xFFFFFFFFu;
    jpt_session_restart_end((int32_t)result >= 0);
    g_eax = result;
    GUEST_RETURN_STDCALL(0);
}

static size_t write_state(uint8_t* out, size_t cap) { return la_state_write(&A.state, out, cap); }

static void session_ended(void) {
    release_all();
    reset();
    memset(&U, 0, sizeof U);
    memset(A.initial, 0, sizeof A.initial);
    A.map_loaded[0] = 0;
    A.logs = 0;
    jpt_save_set_state(NULL);
}

int level_actions_install(void) {
    if (!jpt_custom_level_active()) {
        guest_override_install(RESTART_LEVEL, restart, &A.restart);
        return 1;
    }
    jpt_custom_level_on_end(session_ended);
    jpt_save_set_state(write_state);
    A.seed = 0x2F6E2B1u;
    reset();
    if (guest_override_install(LOAD_WORLD, load_world, &A.load_world) && guest_override_install(HINT_START, hint_start, &A.start) &&
        guest_override_install(HINT_LOAD, hint_load, &A.load) && guest_override_install(HINT_SAVE, hint_save, &A.save) &&
        guest_override_install(RESTART_LEVEL, restart, &A.restart) && guest_override_install(ACTION_FACTORY, factory, &A.factory))
        return 1;
    host_log("custom-level", "warning: the actions of ATX and Trespasser CE could not be hooked");
    return 0;
}

static int video_is_there(uint32_t scratch) {
    char file[JPT_PATH_MAX], request[LA_VIDEO_MAX + 8];
    const uint32_t args[3] = { 0, scratch, 0x104u };
    size_t used;
    snprintf(request, sizeof request, "%s.smk", A.video);
    if (jpt_custom_level_play_video(request)) return 1;
    if (!scratch || !guest_call(LEVEL_PATH, 0, args, 3, 1, NULL)) return 0;
    used = strlen((const char*)ADDR(scratch));
    snprintf((char*)ADDR(scratch) + used, 0x104u - used, "%s", request);
    return guest_host_path((const char*)ADDR(scratch), file, sizeof file) && jpt_fs_exists(file);
}

/* As the game shows a window of its own over the game: paused, with the window's manager, and on again. */
static void play_video(void) {
    const uint32_t esp = g_esp, eax = g_eax;
    guest_ui_view_t front;
    uint32_t owner = 0, window, name, go_on = 1, played = 0;
    if (!A.video[0] || !guest_ui_probe(&front) || front.kind != GUEST_WND_GAME || front.ui != GUEST_UI_GAMEPLAY || front.depth != 1) return;
    if (!guest_read32(front.top + WND_OWNER, &owner) || !owner) return;
    A.playing = 1;
    if (video_is_there(guest_scratch(SCRATCH_SLOT, 2 * JPT_PATH_MAX))) {
        name = push(A.video, sizeof A.video);
        window = push(NULL, VIDEO_WND_STACK);
        guest_call(PAUSE_GAME, front.top, NULL, 0, 0, NULL);
        guest_call(VIDEO_WND_CTOR, window, &owner, 1, 0, NULL);
        guest_call(VIDEO_WND_PLAY, window, &name, 1, 0, &played);
        guest_call(VIDEO_WND_DTOR, window, NULL, 0, 0, NULL);
        guest_call(RESUME_GAME, front.top, &go_on, 1, 0, NULL);
    }
    if (!played) note("ActionType %d: the video %s could not be played", LA_VIDEO, A.video);
    jpt_custom_level_play_video(NULL);
    A.video[0] = 0;
    A.playing = 0;
    g_esp = esp;
    g_eax = eax;
}

void level_actions_step(void) {
    float now, passed;
    uint32_t bits;
    if (!jpt_custom_level_active() || A.playing) return;
    play_video();
    if (!guest_read32(GAME_TIME, &bits)) return;
    memcpy(&now, &bits, 4);
    passed = now - A.last_time;
    A.last_time = now;
    if (!(passed > 0.0f)) return;
    if (passed > STEP_LONGEST) passed = 0.0f;
    jpt_level_scripts_step(passed);
    if (A.state.speed.active && passed > 0.0f) {
        la_speed_step(&A.state.speed, passed);
        set_game_speed();
    }
    if (A.state.fixed_count) step_fixed_speeds(passed);
}

void level_actions_input(uint32_t sinput) {
    float* values = (float*)ADDR(sinput);
    uint32_t* words = (uint32_t*)ADDR(sinput);
    if (!jpt_custom_level_active() || !la_controls_active(&A.state.controls)) return;
    la_controls_apply(&A.state.controls, values + 1, values + 3, &words[5], &words[6]);
}

const minimap_picture_t* level_actions_map(void) {
    char file[JPT_PATH_MAX];
    if (!jpt_custom_level_active() || !A.state.map[0]) return NULL;
    if (strcmp(A.map_loaded, A.state.map)) {
        jpt_picture_t picture;
        memset(&picture, 0, sizeof picture);
        memset(&A.map, 0, sizeof A.map);
        snprintf(A.map_loaded, sizeof A.map_loaded, "%s", A.state.map);
        if (jpt_custom_level_map(A.state.map, file, sizeof file) && jpt_picture_load(file, &picture)) {
            la_map_cells(picture.bgra, (int)picture.width, (int)picture.height, MM_PICTURE_SIDE, &A.map.cols, &A.map.rows, A.map_cells);
            A.map.cells = A.map_cells;
            A.map.serial = ++A.map_serial;
            jpt_picture_free(&picture);
        } else {
            note("ActionType %d: the map picture %s cannot be read", LA_MAP, A.state.map);
        }
    }
    A.map.x0 = A.state.map_corners[0], A.map.y0 = A.state.map_corners[1], A.map.x1 = A.state.map_corners[2], A.map.y1 = A.state.map_corners[3];
    return A.map.cells && A.map.x0 != A.map.x1 && A.map.y0 != A.map.y1 ? &A.map : NULL;
}

int level_actions_hint(unsigned id, char* out, size_t cap) {
    char file[JPT_PATH_MAX];
    uint64_t size = 0;
    char* strings;
    jpt_fs_file_t* f;
    int found = 0;
    if (!jpt_custom_level_active() || !A.state.hint[0] || id != HINT_STRINGS + MEM32(CURRENT_HINT)) return 0;
    f = jpt_custom_level_hint_strings(file, sizeof file) ? jpt_fs_open_read(file, &size) : NULL;
    strings = f && size && size < (1u << 20) ? (char*)malloc((size_t)size) : NULL;
    if (strings && jpt_fs_read_at(f, 0, strings, (size_t)size)) found = la_hint_lookup(strings, (size_t)size, A.state.hint, out, cap);
    free(strings);
    if (f) jpt_fs_close(f);
    if (!found) snprintf(out, cap, "%s", A.state.hint);
    return 1;
}
