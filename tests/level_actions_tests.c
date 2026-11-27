/* The ActionTypes of ATX and Trespasser CE: the rules, and the hooks over a fake guest whose triggers, instances and particles are
 * made here. No window, no game data. The guest addresses live in g_window, which the fixed image base puts there. */
#include "test_profile.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_custom_level.h"
#include "jpt_level_scripts.h"
#include "jpt_save.h"
#include "level_actions.h"
#include "level_actions_policy.h"
#include "level_session.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
#ifdef RECOMP_X87_RING
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;
void recomp_x87_recentre(void) {}
#endif
uint16_t g_fpu_cw = 0x027F;
uint32_t g_cur_func, g_alias_entry;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;

#define WINDOW_HI 0x00800000u
#define STACK_TOP 0x007EF000u
#define HEAP 0x00750000u
#define HEAP_END 0x007A0000u
#define TRIGGER 0x007B0000u
#define PLAYER_AT 0x007B1000u
#define LOADER 0x007C0000u
#define SYM(index) (LOADER + 0x2A8u + 8u * (index))
#define SYM_A(n) SYM(89u + (n))

static uint8_t g_window[0x600000];
static unsigned long g_checks, g_failures;
static int g_session, g_full_restart, g_scene_version;
static uint32_t g_types;
static char g_hint_file[260];

#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

/* ---- the host around it ---- */

/* The [session], [level] and [restart] lines, in order. */
static char g_lines[64][160];
static int g_line_count;
void host_log(const char* tag, const char* fmt, ...) {
    va_list ap;
    if (strcmp(tag, "session") && strcmp(tag, "level") && strcmp(tag, "restart")) return;
    va_start(ap, fmt);
    vsnprintf(g_lines[g_line_count % 64], sizeof g_lines[0], fmt, ap);
    va_end(ap);
    g_line_count++;
}
static int logged(const char* text) {
    for (int i = 0; i < g_line_count && i < 64; i++)
        if (strstr(g_lines[i], text)) return 1;
    return 0;
}
int jpt_custom_level_active(void) { return g_session; }
int jpt_custom_level_action(int type) { return g_session && type >= 99 && type <= 121 && (g_types >> (type - 99) & 1u); }
int jpt_custom_level_requires(jpt_level_cap_t cap) { return g_session && cap == JPT_LEVEL_CAP_FULL_RESTART && g_full_restart; }
int jpt_custom_level_scene_version(void) { return g_scene_version; }
int jpt_custom_level_hint_strings(char* out, size_t cap) { return g_hint_file[0] ? snprintf(out, cap, "%s", g_hint_file) > 0 : 0; }
static char g_map_file[260];
int jpt_custom_level_map(const char* name, char* out, size_t cap) { return !strcmp(name, "map.bmp") && g_map_file[0] ? snprintf(out, cap, "%s", g_map_file) > 0 : 0; }
int jpt_custom_level_find(const char* name, char* out, size_t cap) { return (void)out, (void)cap, g_session && (!strcmp(name, "own.scn") || !strcmp(name, "other.scn")); }
const char* jpt_custom_level_cap_name(jpt_level_cap_t cap) { return cap == JPT_LEVEL_CAP_FULL_RESTART ? "full_restart" : "other"; }
const char* jpt_custom_level_package(void) { return "Package"; }
const char* jpt_custom_level_layout(void) { return "loose"; }
const char* jpt_custom_level_scene(void) { return "own.scn"; }
const char* jpt_custom_level_compat_name(void) { return "ready"; }
int jpt_custom_level_experimental(void) { return 0; }
static char g_restarted[64];
static unsigned g_restarts_told;
int jpt_custom_level_restart(const char* scene, char* out, size_t cap) {
    snprintf(g_restarted, sizeof g_restarted, "%s", scene);
    g_restarts_told++;
    return jpt_custom_level_find(scene, out, cap);
}
static char g_package_video[64], g_served_video[64];
int jpt_custom_level_play_video(const char* name) {
    const int there = name && g_package_video[0] && !strcmp(name, g_package_video);
    snprintf(g_served_video, sizeof g_served_video, "%s", there ? name : "");
    return there;
}
static guest_ui_view_t g_front;
int guest_ui_probe(guest_ui_view_t* out) { return *out = g_front, 1; }
static void (*g_end)(void);
void jpt_custom_level_on_end(void (*ended)(void)) { g_end = ended; }

/* What the host tells the custom session's scripts (src/common/jpt_level_scripts.cpp, tested in script_session_tests). */
static struct {
    int starts, kind, begins, readies, ok, steps;
    jpt_level_reason_t reason;
    float seconds;
} g_scripts;
void jpt_level_scripts_start(jpt_session_kind_t kind, jpt_level_scripts_log_fn log) { (void)log, g_scripts.starts++, g_scripts.kind = (int)kind; }
void jpt_level_scripts_begin(jpt_level_reason_t reason, const char* scene, const char* save) { (void)scene, (void)save, g_scripts.begins++, g_scripts.reason = reason; }
void jpt_level_scripts_ready(int ok) { g_scripts.readies++, g_scripts.ok = ok; }
void jpt_level_scripts_step(float seconds) { g_scripts.steps++, g_scripts.seconds = seconds; }

static size_t (*g_write_state)(uint8_t*, size_t);
static uint8_t g_saved[LA_STATE_MAX];
static size_t g_saved_size;
void jpt_save_set_state(jpt_save_state_fn write) { g_write_state = write; }
int jpt_save_is_name(const char* path) { return strstr(path, "SAVEGAME") != NULL; }
size_t jpt_save_read_state(const char* path, uint8_t* out, size_t cap) {
    (void)path;
    if (g_saved_size > cap) return 0;
    memcpy(out, g_saved, g_saved_size);
    return g_saved_size;
}

int guest_read32(uint32_t va, uint32_t* out) { return va >= 0x400000u && va < WINDOW_HI ? (*out = MEM32(va), 1) : 0; }
int guest_read_string(uint32_t va, char* out, int cap) { return snprintf(out, (size_t)cap, "%s", (const char*)(uintptr_t)va) >= 0; }
int guest_host_path(const char* path, char* out, size_t cap) { return snprintf(out, cap, "%s", path) > 0; }
static uint8_t g_scratch[2048];
uint32_t guest_scratch(unsigned slot, uint32_t size) { return (void)slot, size <= sizeof g_scratch ? (uint32_t)(uintptr_t)g_scratch : 0; }

static recomp_func_t g_factory, g_start, g_load, g_save, g_load_world, g_restart;
static unsigned g_lifted_starts, g_lifted_loads, g_lifted_restarts, g_worlds;
static int g_world_triggers, g_world_result;
static void lifted_factory(void) { g_esp += 4; }
static void lifted_start(void) { g_lifted_starts++, g_esp += 4; }
static void lifted_load(void) { g_lifted_loads++, g_eax = GUEST_ARG(0) + 4u, GUEST_RETURN_STDCALL(1); }
static uint32_t nested(recomp_func_t fn, uint32_t self, uint32_t a0, uint32_t a1, uint32_t a2);
static uint8_t g_path_text[260];
/* The game's Restart Level (0x00414480): bLoadScene of the data folder and the application's scene, no loading window. */
static void lifted_restart(void) {
    g_lifted_restarts++;
    snprintf((char*)g_path_text, sizeof g_path_text, "C:\\data\\%s", (const char*)(uintptr_t)(GVA(0x0066E338) + 0xA));
    g_eax = nested(g_load_world, 0x00700100u, (uint32_t)(uintptr_t)g_path_text, 0, 0);
    g_esp += 4;
}
static void build_level(void);

/* The engine's world as far as a restart shows it. CWorld::bLoadScene purges the world only when no world file the scene
 * names is loaded; then it builds everything anew. Otherwise it reads the scene's values over the world it has, and what
 * the scene does not hold (here the player, a crate taken, a fired trigger, an alerted dinosaur) stays as it was. */
static struct {
    char grf[64];
    unsigned purges, engine_purges, scene_reads, grf_opens, pid_opens, terrain_opens, players, player;
    float player_x, game_time;
    int crate_taken, trigger_fired, raptor_alert;
    char scene[64];
} W;
#define SPAWN_X 10.0f

static void world_purge(void) {
    W.grf[0] = 0;
    W.player = 0;
    W.purges++;
}

static void world_read(const char* path) {
    const char* slash = strrchr(path, '\\');
    char grf[64];
    size_t n;
    snprintf(W.scene, sizeof W.scene, "%s", slash ? slash + 1 : path);
    n = strcspn(W.scene, ".");
    snprintf(grf, sizeof grf, "%.*s.grf", (int)n, W.scene);
    if (strstr(path, "SAVEGAME")) snprintf(grf, sizeof grf, "%s", W.grf[0] ? W.grf : "own.grf");
    W.scene_reads++;
    if (strcmp(W.grf, grf)) {
        if (W.grf[0]) W.engine_purges++, world_purge();
        snprintf(W.grf, sizeof W.grf, "%s", grf);
        W.grf_opens++, W.pid_opens++, W.terrain_opens++;
        W.player = ++W.players;
        W.player_x = SPAWN_X;
        W.crate_taken = W.trigger_fired = W.raptor_alert = 0;
    }
    W.game_time = 0.0f;
}

static void lifted_load_world(void) {
    const uint32_t esp = g_esp;
    g_worlds++;
    world_read((const char*)(uintptr_t)GUEST_ARG(0));
    if (g_world_triggers) build_level();
    g_esp = esp;
    g_eax = (uint32_t)g_world_result;
    GUEST_RETURN_STDCALL(3);
}

/* A guest function calling another: its own frame below the caller's. */
static uint32_t nested(recomp_func_t fn, uint32_t self, uint32_t a0, uint32_t a1, uint32_t a2) {
    const uint32_t esp = g_esp, ecx = g_ecx;
    g_esp = esp - 0x200u;
    MEM32(g_esp -= 4) = a2;
    MEM32(g_esp -= 4) = a1;
    MEM32(g_esp -= 4) = a0;
    MEM32(g_esp -= 4) = 0;
    g_ecx = self;
    fn();
    g_esp = esp;
    g_ecx = ecx;
    return g_eax;
}

static int g_installs;
int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    g_installs++;
    if (va == GVA(0x0046BE90)) g_factory = replacement, *lifted = lifted_factory;
    else if (va == GVA(0x00467BB0)) g_start = replacement, *lifted = lifted_start;
    else if (va == GVA(0x00467BC0)) g_load = replacement, *lifted = lifted_load;
    else if (va == GVA(0x00467BE0)) g_save = replacement, *lifted = lifted_load;
    else if (va == GVA(0x00606630)) g_load_world = replacement, *lifted = lifted_load_world;
    else if (va == GVA(0x00414480)) g_restart = replacement, *lifted = lifted_restart;
    else return 0;
    return 1;
}

/* ---- the fake guest ---- */

typedef struct {
    uint32_t group, symbol, value;
} entry_t;
static entry_t g_values[4096];
static int g_value_count;
static uint32_t g_heap = HEAP, g_group;

static uint32_t alloc(uint32_t size) {
    const uint32_t at = g_heap;
    g_heap += (size + 15u) & ~15u;
    if (g_heap > HEAP_END) printf("heap\n"), exit(2);
    memset((void*)(uintptr_t)at, 0, size);
    return at;
}

static uint32_t put(uint32_t symbol, uint32_t type, const void* data, size_t size) {
    const uint32_t v = alloc(0x60);
    MEM32(v + 4) = type;
    memcpy((void*)(uintptr_t)(v + 0x28), data, size);
    g_values[g_value_count++] = (entry_t){ g_group, symbol, v };
    return v;
}

static void put_int(uint32_t symbol, int value) { put(symbol, 3, &value, 4); }
static void put_float(uint32_t symbol, float value) { put(symbol, 4, &value, 4); }
static void put_bool(uint32_t symbol, int value) { put(symbol, 1, &(uint8_t){ (uint8_t)value }, 1); }
static void put_string(uint32_t symbol, const char* text) {
    const uint32_t chars = alloc((uint32_t)strlen(text) + 1u), v = put(symbol, 5, "", 0);
    strcpy((char*)(uintptr_t)chars, text);
    MEM32(v + 0x2C) = (uint32_t)strlen(text) + 1u;
    MEM32(v + 0x40) = chars;
}
static uint32_t enter_group(uint32_t symbol) {
    const uint32_t parent = g_group, v = put(symbol, 6, "", 0);
    g_group = v;
    return parent;
}

static uint32_t hash_of(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static struct {
    uint32_t hash, at;
} g_instances[32];
static int g_instance_count;
static uint32_t instance(const char* name, float x, float y, float z, uint32_t vtable) {
    const uint32_t at = alloc(0x120);
    const float place[7] = { x, y, z, 1.0f, 0.0f, 0.0f, 0.0f };
    MEM32(at) = vtable;
    memcpy((void*)(uintptr_t)(at + 0x30), place, sizeof place);
    g_instances[g_instance_count].hash = hash_of(name);
    g_instances[g_instance_count++].at = at;
    return at;
}

typedef struct {
    uint32_t vtable, references, name;
    float position[3], rotation[4];
    uint8_t flags[4];
} teleported_t;
static teleported_t g_teleports[64];
static int g_teleport_count, g_effects, g_purges, g_level_loads, g_menu_sounds_off;
static float g_effect[8];
static uint32_t g_effect_self;
static char g_loaded_path[260];

typedef struct {
    uint32_t vtable, references;
    float velocity[3];
    uint8_t frozen, pad1[3];
    uint32_t target;
    uint8_t impulse, pad2[3];
    float position[3], facing[3], push;
    uint32_t emitter;
} physics_t;
static physics_t g_physics[64];
static int g_physics_count, g_paused, g_video_windows, g_videos, g_sent, g_uses, g_unlimited, g_deleted;
static uint32_t g_sent_message[5], g_used;
static char g_video_name[64], g_video_served[64];

#define TRIGGER_KIND 0x00646000u
#define ANIMAL_KIND 0x00646100u
#define MESH_KIND 0x00646200u
#define SHAPE_KIND 0x00646300u
#define INFO_KIND 0x00646400u

int guest_call_virtual(uint32_t self, uint32_t slot, const uint32_t* args, int count, uint32_t* result) {
    const uint32_t kind = MEM32(self);
    uint32_t value = 0;
    (void)count;
    switch (slot) {
    case 0x44: MEM32(args[0]) = kind == TRIGGER_KIND ? self : 0; break;
    case 0x64: MEM32(args[0]) = kind == ANIMAL_KIND ? self : 0; break;
    case 0x58: value = 0xABCDEF00u | (kind == MESH_KIND); break;
    case 0x170:
        g_uses++, g_used = self;
        g_unlimited = MEM8(GVA(0x0066ECC0) + 1u);
        break;
    case 0x0C:
        g_sent++;
        memcpy(g_sent_message, (const void*)(uintptr_t)self, sizeof g_sent_message);
        break;
    case 0x04: g_deleted++; break;
    case 0x00:
        CHECK(kind == INFO_KIND);
        value = alloc(0x20);
        memcpy((void*)(uintptr_t)value, (const void*)(uintptr_t)self, 0x14);
        MEM32(value + 4) |= 1u;
        break;
    default: printf("unexpected slot %X\n", slot); return 0;
    }
    if (result) *result = value;
    return 1;
}

int guest_call(uint32_t va, uint32_t self, const uint32_t* args, int count, int caller_pops, uint32_t* result) {
    uint32_t value = 0;
    (void)caller_pops;
    if (va == GVA(0x0041AD50)) {
        for (int i = 0; i < g_value_count; i++)
            if (g_values[i].group == args[1] && g_values[i].symbol == args[0]) value = g_values[i].value;
    } else if (va == GVA(0x005F6100)) {
        value = hash_of((const char*)(uintptr_t)args[0]);
    } else if (va == GVA(0x0043DA80)) {
        value = hash_of((const char*)(uintptr_t)args[0]) ^ 1u;
    } else if (va == GVA(0x00607C10)) {
        for (int i = 0; i < g_instance_count; i++)
            if (g_instances[i].hash == args[0]) value = g_instances[i].at;
    } else if (va == GVA(0x00626B8C)) {
        value = alloc(args[0]);
    } else if (va == GVA(0x0046C6D0)) {
        MEM32(self + 0x54 + 4 * MEM32(self + 0x9C)) = args[0];
        MEM32(self + 0x9C)++;
    } else if (va == GVA(0x00470A70)) {
        memcpy(&g_teleports[g_teleport_count++ % 64], (const void*)(uintptr_t)self, sizeof g_teleports[0]);
    } else if (va == GVA(0x006230F0)) {
        value = args[0] == (hash_of("Sparks") ^ 1u) ? 0x00712340u : 0;
    } else if (va == GVA(0x0053E350)) {
        g_effects++, g_effect_self = self;
        memcpy(g_effect, args, sizeof g_effect);
        CHECK(count == 8);
    } else if (va == GVA(0x00604DD0)) {
        g_purges++;
        world_purge();
    } else if (va == GVA(0x00411B30)) {
        strcpy((char*)(uintptr_t)args[1], "C:\\data\\");
    } else if (va == GVA(0x00466900)) {
        memcpy(&g_physics[g_physics_count++ % 64], (const void*)(uintptr_t)self, sizeof g_physics[0]);
    } else if (va == GVA(0x00611D50)) {
        if (!(MEM32(self + 4) & 8u)) {
            const uint32_t own = alloc(0x18);
            memcpy((void*)(uintptr_t)own, (const void*)(uintptr_t)MEM32(self + 0x10), 0x18);
            MEM32(self + 0x10) = own;
            MEM32(self + 4) |= 8u;
        }
        value = MEM32(self + 0x10);
    } else if (va == GVA(0x0040BCC0)) {
        g_paused++;
        CHECK(self == g_front.top);
    } else if (va == GVA(0x0040BD70)) {
        g_paused--;
        CHECK(self == g_front.top && args[0] == 1 && !g_video_windows);
    } else if (va == GVA(0x00418700)) {
        g_video_windows++;
        CHECK(args[0] == 0x00700200u);
    } else if (va == GVA(0x00418760)) {
        g_video_windows--;
    } else if (va == GVA(0x00418AE0)) {
        g_videos++;
        CHECK(g_paused == 1 && g_video_windows == 1);
        snprintf(g_video_name, sizeof g_video_name, "%s", (const char*)(uintptr_t)args[0]);
        snprintf(g_video_served, sizeof g_video_served, "%s", g_served_video);
        level_actions_step();
        value = 1;
    } else if (va == GVA(0x00414D80)) {
        g_menu_sounds_off++;
        CHECK(self == GVA(0x0066E338));
    } else if (va == GVA(0x004142C0)) {
        /* The level load: CLoaderWnd, the scene name kept by the application, bLoadScene through its override. */
        const char* scene = args[1] ? (const char*)(uintptr_t)args[1] : "";
        g_level_loads++;
        snprintf(g_loaded_path, sizeof g_loaded_path, "%s", (const char*)(uintptr_t)args[0]);
        if (scene[0]) snprintf((char*)(uintptr_t)(GVA(0x0066E338) + 0xA), 16, "%s", scene);
        jpt_session_level_begin(jpt_session_reason(strstr(g_loaded_path, "SAVEGAME") != NULL, 0), scene[0] ? scene : g_loaded_path,
                                level_session_source(scene[0] ? scene : g_loaded_path));
        value = g_load_world ? nested(g_load_world, 0x00700100u, args[0], 0, 0) : 0;
        jpt_session_level_end((int32_t)value >= 0, (int)(int32_t)value);
    } else {
        printf("unexpected call %08X\n", va);
        return 0;
    }
    if (result) *result = value;
    return 1;
}

/* ---- calls ---- */

static void enter(recomp_func_t fn, uint32_t self, int count, ...) {
    uint32_t args[8];
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    g_esp = STACK_TOP;
    for (int i = count - 1; i >= 0; i--) MEM32(g_esp -= 4) = args[i];
    MEM32(g_esp -= 4) = 0;
    g_ecx = self;
    fn();
}

static uint32_t action_at(int i) { return MEM32(TRIGGER + 0x54 + 4 * (uint32_t)i); }

/* The trigger's actions of one group each, from what g_group holds. */
static uint32_t add(uint32_t group) {
    enter(g_factory, 0, 5, TRIGGER, 0u, group, 0u, LOADER);
    return MEM32(TRIGGER + 0x9C) ? action_at((int)MEM32(TRIGGER + 0x9C) - 1) : 0;
}

static uint32_t new_group(int type) {
    static uint32_t s_next = 0x00100000u;
    g_group = s_next += 0x10;
    put_int(SYM(403), type);
    return g_group;
}

static void start(uint32_t action) { enter(g_start, action, 0); }
static void load_world(const char* path, int triggers) {
    const uint32_t text = alloc(64);
    strcpy((char*)(uintptr_t)text, path);
    g_world_triggers = triggers;
    enter(g_load_world, 0x00700100u, 3, text, 0u, 0u);
    CHECK(g_esp == STACK_TOP);
}

static float game_speed(void) {
    float f;
    memcpy(&f, (const void*)(uintptr_t)GVA(0x0066B6FC), 4);
    return f;
}

static void fresh(uint32_t types) {
    g_value_count = g_instance_count = g_teleport_count = g_effects = 0;
    g_physics_count = g_paused = g_video_windows = g_videos = g_sent = g_uses = g_deleted = 0;
    g_package_video[0] = 0;
    memset(&g_front, 0, sizeof g_front);
    g_heap = HEAP;
    memset((void*)(uintptr_t)TRIGGER, 0, 0x200);
    g_session = 1, g_types = types, g_full_restart = 0, g_scene_version = 0, g_saved_size = 0, g_hint_file[0] = 0;
    memset(&W, 0, sizeof W);
    level_session_start("en-116", "d3d11", "en");
    MEM32(GVA(0x0073F444)) = 0x00700100u;
    MEM32(GVA(0x0066EF64)) = PLAYER_AT;
    memset((void*)(uintptr_t)PLAYER_AT, 0, 0x1000);
    memcpy((void*)(uintptr_t)GVA(0x0066B6FC), &(float){ 1.0f }, 4);
    level_actions_install();
    g_world_result = 0;
    load_world("C:\\data\\own.scn", 0);
}

#define BIT(type) (1u << ((type) - 99))

static void build_level(void) {
    memset((void*)(uintptr_t)(TRIGGER + 0x54), 0, 0x4C);
    for (int i = 0; i < 2; i++) {
        new_group(LA_RELOAD);
        put_int(SYM(416), 2);
        put_int(SYM(581), 1);
        add(g_group);
    }
}

/* ---- tests ---- */

static void test_retail_session(void) {
    g_session = 0;
    g_installs = 0;
    g_restart = NULL;
    CHECK(level_actions_install() && g_installs == 1 && g_restart && g_write_state == NULL);
    CHECK(!level_actions_hint(18000, (char[8]){ 0 }, 8));
}

static void test_speed(void) {
    uint32_t a;
    fresh(BIT(101) | BIT(100));
    new_group(LA_SPEED);
    put_float(SYM(1262), 0.5f);
    a = add(g_group);
    CHECK(a && MEM32(a) == GVA(0x0064AF64) && MEM32(a + 4) == 1);
    CHECK(game_speed() == 1.0f);
    start(a);
    CHECK(game_speed() == 0.5f && g_lifted_starts == 0 && g_esp == STACK_TOP);
    load_world("C:\\data\\own.scn", 0);
    CHECK(game_speed() == 1.0f);

    /* Once only, until the level starts again. */
    new_group(LA_SPEED);
    put_float(SYM(1262), 2.0f);
    put_bool(SYM(581), 1);
    a = add(g_group);
    start(a);
    memcpy((void*)(uintptr_t)GVA(0x0066B6FC), &(float){ 1.0f }, 4);
    start(a);
    CHECK(game_speed() == 1.0f);
    load_world("C:\\data\\own.scn", 0);
    start(a);
    CHECK(game_speed() == 2.0f);
    {
        la_speed_t s;
        la_speed_reset(&s);
        CHECK(la_speed_clamp(0.0f) == 0.05f && la_speed_clamp(NAN) == 0.05f && la_speed_clamp(1.0004f) == 1.0f && la_speed_clamp(3.0f) == 3.0f);
        la_speed_set(&s, 0.5f);
        la_speed_slow(&s, 0.5f, 1000.0f, 0.0f, 0.0f);
        CHECK(fabsf(la_speed_factor(&s) - 0.25f) < 1e-6f);
        la_speed_step(&s, 0.5f);
        CHECK(!s.active && la_speed_factor(&s) == 0.5f);
        la_speed_slow(&s, 0.2f, 0.0f, 0.0f, 0.0f);
        CHECK(!s.active);
        la_speed_slow(&s, 0.2f, 100.0f, 1000.0f, 1000.0f);
        CHECK(s.active && s.current == 1.0f);
        /* The interval lies between the transitions. */
        la_speed_reset(&s);
        la_speed_slow(&s, 0.5f, 2000.0f, 1000.0f, 1000.0f);
        s.elapsed = 1.0f;
        la_speed_step(&s, 0.0f);
        CHECK(s.current == 0.5f);
        s.elapsed = 2.99f;
        la_speed_step(&s, 0.0f);
        CHECK(s.current == 0.5f);
        s.elapsed = 3.5f;
        la_speed_step(&s, 0.0f);
        CHECK(fabsf(s.current - 0.75f) < 1e-6f);
        s.elapsed = 4.0f;
        la_speed_step(&s, 0.0f);
        CHECK(!s.active && s.current == 1.0f);
        /* Again while it runs: the transition takes the part of the way that is left, either way. */
        la_speed_slow(&s, 0.5f, 2000.0f, 0.0f, 0.0f);
        la_speed_slow(&s, 0.5f, 2000.0f, 1000.0f, 0.0f);
        CHECK(s.enter == 0.0f && s.current == 0.5f);
        la_speed_slow(&s, 0.25f, 2000.0f, 900.0f, 0.0f);
        CHECK(fabsf(s.enter - 0.3f) < 1e-6f);
        s.current = 0.25f;
        la_speed_slow(&s, 0.5f, 2000.0f, 1000.0f, 0.0f);
        CHECK(fabsf(s.enter - 0.5f) < 1e-6f);
    }
}

static void test_hint(void) {
    char text[64];
    const char* strings = "; comment\r\nHINT1=\"The first\"\r\n\"Look around. Now\" = \"Olha em volta\"\r\nBROKEN=no\r\n";
    uint32_t a, b;
    FILE* f;
    fresh(BIT(106));
    new_group(LA_HINT);
    put_string(SYM(404), "Look around.");
    put_string(SYM(581), "Now");
    a = add(g_group);
    new_group(LA_HINT);
    b = add(g_group);
    MEM32(GVA(0x0066F01C)) = 7;
    CHECK(!level_actions_hint(18007, text, sizeof text));
    start(b);
    CHECK(!level_actions_hint(18007, text, sizeof text));
    start(a);
    CHECK(level_actions_hint(18007, text, sizeof text) && !strcmp(text, "Look around. Now") && !level_actions_hint(18008, text, sizeof text));
    CHECK(MEM32(GVA(0x0066F01C)) == 7);
    f = fopen("level_actions_tests.txt", "wb");
    fwrite(strings, 1, strlen(strings), f);
    fclose(f);
    strcpy(g_hint_file, "level_actions_tests.txt");
    CHECK(level_actions_hint(18007, text, sizeof text) && !strcmp(text, "Olha em volta"));
    remove("level_actions_tests.txt");
    g_hint_file[0] = 0;
    /* The game's own hint takes its place. */
    g_lifted_starts = 0;
    enter(g_start, alloc(0xC), 0);
    CHECK(g_lifted_starts == 1 && !level_actions_hint(18007, text, sizeof text));
    start(a);
    load_world("C:\\data\\own.scn", 0);
    CHECK(!level_actions_hint(18007, text, sizeof text));
    CHECK(la_hint_lookup(strings, strlen(strings), "HINT1", text, sizeof text) && !strcmp(text, "The first") && !la_hint_lookup(strings, strlen(strings), "HINT2", text, sizeof text) && !la_hint_lookup(strings, strlen(strings), "BROKEN", text, sizeof text));
    CHECK(!la_hint_join(NULL, "", text, sizeof text) && la_hint_join(NULL, "b", text, sizeof text) && !strcmp(text, "b"));
}

static void test_controls(void) {
    uint32_t off, walk, on, sinput[8];
    float* values = (float*)sinput;
    const uint32_t guest = alloc(64);
    fresh(BIT(112));
    new_group(LA_CONTROLS);
    put_int(SYM(581), 0);
    put_int(SYM(416), 0);
    off = add(g_group);
    new_group(LA_CONTROLS);
    put_int(SYM(581), 2);
    walk = add(g_group);
    new_group(LA_CONTROLS);
    put_int(SYM(581), 1);
    put_int(SYM(416), 1);
    on = add(g_group);
#define READ_INPUT()                                                    \
    do {                                                                \
        memset(sinput, 0, sizeof sinput);                               \
        values[1] = values[2] = values[3] = values[4] = 1.0f;           \
        sinput[5] = sinput[6] = 0x1F0000FFu;                            \
        memcpy((void*)(uintptr_t)guest, sinput, sizeof sinput);         \
        level_actions_input(guest);                                     \
        memcpy(sinput, (const void*)(uintptr_t)guest, sizeof sinput);   \
    } while (0)
    READ_INPUT();
    CHECK(sinput[5] == 0x1F0000FFu && values[1] == 1.0f && values[3] == 1.0f);
    start(off);
    READ_INPUT();
    CHECK(!sinput[5] && !sinput[6] && values[1] == 0.0f && values[2] == 0.0f && values[3] == 0.0f);
    start(walk);
    READ_INPUT();
    CHECK(sinput[5] == 0xFEu && values[1] == 0.0f && values[3] == 0.0f);
    start(on);
    READ_INPUT();
    CHECK(sinput[5] == 0x1F0000FFu && values[1] == 1.0f && values[4] == 1.0f);
    start(off);
    g_session = 0;
    READ_INPUT();
    CHECK(sinput[5] == 0x1F0000FFu);
    g_session = 1;
    load_world("C:\\data\\own.scn", 0);
    READ_INPUT();
    CHECK(sinput[5] == 0x1F0000FFu);
}

static void test_reload(void) {
    uint32_t gun, other, crate, a;
    fresh(BIT(103));
    gun = instance("Gun", 0, 0, 0, GVA(0x0064A274));
    other = instance("Other", 0, 0, 0, GVA(0x0064A274));
    crate = instance("Crate", 0, 0, 0, 0x00645000u);
    MEM32(gun + 0x68) = 12, MEM32(other + 0x68) = 30, MEM32(crate + 0x68) = 9;
    MEM32(PLAYER_AT + 0xE34) = gun;
    MEM32(PLAYER_AT + 0xE38) = other;
    MEM32(PLAYER_AT + 0xE3C) = crate;
    new_group(LA_RELOAD);
    put_int(SYM(416), 2);
    put_int(SYM(1198), 2);
    put_int(SYM(581), 3);
    put_string(SYM_A(0), "Gun");
    a = add(g_group);
    start(a);
    CHECK(MEM32(gun + 0x6C) == 10 && MEM32(other + 0x6C) == 0 && MEM32(crate + 0x6C) == 0);
    MEM32(gun + 0x6C) = 0;
    start(a);
    CHECK(MEM32(gun + 0x6C) == 10);
    MEM32(gun + 0x6C) = 0;
    start(a);
    CHECK(MEM32(gun + 0x6C) == 0);
    load_world("C:\\data\\own.scn", 0);
    start(a);
    CHECK(MEM32(gun + 0x6C) == 10);
    new_group(LA_RELOAD);
    put_int(SYM(1194), 5);
    put_int(SYM(581), 2);
    a = add(g_group);
    start(a);
    CHECK(MEM32(other + 0x6C) == 5 && MEM32(gun + 0x6C) == 10 && MEM32(crate + 0x6C) == 0);
}

static void place(float x, const char* destination) {
    if (destination) put_string(SYM(467), destination);
    else {
        put_float(SYM(490), x), put_float(SYM(491), 2.0f), put_float(SYM(492), 3.0f);
        put_float(SYM(511), 0.0f), put_float(SYM(512), 0.0f), put_float(SYM(513), 90.0f);
    }
    put_bool(SYM(464), 0);
}

static void test_teleport(void) {
    uint32_t a, top, parent, group;
    int seen[4] = { 0 }, weights[3] = { 0, 3, 1 }, hits[3] = { 0 };
    uint32_t seed = 1;
    float q[4];
    fresh(BIT(114));
    instance("Crate", 0, 0, 0, 0);
    instance("Barrel", 0, 0, 0, 0);
    instance("Mark", 7.0f, 8.0f, 9.0f, 0);
    top = new_group(LA_TELEPORT);
    parent = enter_group(SYM_A(0));
    put_int(SYM(581), 1);
    put_int(SYM(404), 2);
    put_string(SYM_A(1), "Crate");
    put_string(SYM_A(2), "Barrel");
    g_group = parent;
    enter_group(SYM_A(1));
    put_int(SYM(416), 1);
    put_bool(SYM(659), 1);
    group = g_group;
    enter_group(SYM_A(1));
    place(1.0f, NULL);
    g_group = group;
    enter_group(SYM_A(2));
    place(0.0f, "Mark");
    a = add(top);
    for (int i = 0; i < 40; i++) {
        const int before = g_teleport_count;
        start(a);
        CHECK(g_teleport_count == before + 2);
        for (int k = 0; k < 2; k++) {
            const teleported_t* t = &g_teleports[(before + k) % 64];
            CHECK(t->vtable == GVA(0x0064B990) && t->name == hash_of(k ? "Barrel" : "Crate") && t->flags[1] == 0 && t->flags[0] == 1 && t->flags[2] == 1 && t->flags[3] == 1);
            seen[k * 2 + (t->position[0] != 1.0f)]++;
            CHECK(t->position[0] == 1.0f ? fabsf(t->rotation[0] - 0.70710678f) < 1e-5f && fabsf(t->rotation[3] - 0.70710678f) < 1e-5f : t->position[0] == 7.0f && t->position[2] == 9.0f && t->rotation[0] == 1.0f);
        }
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3] && seen[0] == seen[3] && seen[1] == seen[2]);
    for (int i = 0; i < 4000; i++) hits[la_pick_group(weights, 3, la_random(&seed))]++;
    CHECK(!hits[0] && hits[1] > 2700 && hits[1] < 3300 && la_pick_group(weights, 1, 5) == -1);
    la_quaternion(90.0f, 0.0f, 0.0f, q);
    CHECK(fabsf(q[0] - 0.70710678f) < 1e-5f && fabsf(q[1] - 0.70710678f) < 1e-5f && fabsf(q[2]) < 1e-6f);

    /* A group with sub-groups left out moves only the others; one without instances does nothing. */
    fresh(BIT(114));
    top = new_group(LA_TELEPORT);
    parent = enter_group(SYM_A(0));
    put_int(SYM(581), 1);
    put_int(SYM(404), 1);
    put_string(SYM_A(1), "Nobody");
    g_group = parent;
    enter_group(SYM_A(1));
    put_int(SYM(416), 1);
    enter_group(SYM_A(1));
    place(1.0f, NULL);
    start(add(top));
    CHECK(g_teleport_count == 0);
    top = new_group(LA_TELEPORT);
    start(add(top));
    CHECK(g_teleport_count == 0);
}

static void test_particles(void) {
    uint32_t a;
    float forward[3];
    fresh(BIT(117));
    instance("Emitter", 1.0f, 2.0f, 3.0f, 0);
    new_group(LA_PARTICLES);
    put_string(SYM_A(0), "Sparks");
    put_string(SYM(404), "Emitter");
    put_float(SYM(1863), 0.25f);
    a = add(g_group);
    start(a);
    CHECK(g_effects == 1 && g_effect_self == 0x00712340u && g_effect[0] == 1.0f && g_effect[2] == 3.0f && g_effect[3] == 0.0f && g_effect[4] == 1.0f && g_effect[6] == 1.0f && g_effect[7] == 0.25f);
    new_group(LA_PARTICLES);
    put_string(SYM_A(0), "Nothing");
    put_string(SYM(404), "Emitter");
    start(add(g_group));
    new_group(LA_PARTICLES);
    put_string(SYM_A(0), "Sparks");
    start(add(g_group));
    CHECK(g_effects == 1);
    la_forward((const float[4]){ 0.70710678f, 0.0f, 0.0f, 0.70710678f }, forward);
    CHECK(fabsf(forward[0] + 1.0f) < 1e-5f && fabsf(forward[1]) < 1e-5f);
}

static void test_map(void) {
    uint8_t bmp[54 + 4 * 4 * 3] = { 'B', 'M' };
    const minimap_picture_t* map;
    minimap_frame_t frame;
    minimap_point_t points[MM_CLIP_MAX];
    uint32_t a;
    la_state_t state;
    FILE* f;
    bmp[10] = 54, bmp[14] = 40, bmp[18] = 4, bmp[22] = 4, bmp[26] = 1, bmp[28] = 24;
    for (int i = 0; i < 16; i++) bmp[54 + i * 3 + (i < 8 ? 0 : 2)] = 255;
    f = fopen("level_actions_map.bmp", "wb");
    fwrite(bmp, 1, sizeof bmp, f);
    fclose(f);
    fresh(BIT(102));
    strcpy(g_map_file, "level_actions_map.bmp");
    new_group(LA_MAP);
    put_string(SYM(404), "map.bmp\" ignored");
    put_float(SYM(423), -100.0f), put_float(SYM(424), 200.0f), put_float(SYM(490), 100.0f), put_float(SYM(491), -200.0f);
    a = add(g_group);
    CHECK(!level_actions_map());
    start(a);
    map = level_actions_map();
    CHECK(map && map->cols == 4 && map->rows == 4 && map->x0 == -100.0f && map->y0 == 200.0f && map->x1 == 100.0f && map->y1 == -200.0f && map->cells[0] == 0xFFFF0000u && map->cells[15] == 0xFF0000FFu);
    minimap_frame(&frame, 100.0f, 100.0f, 50.0f, 0.0f, 0.0f, 1.0f);
    CHECK(map && minimap_picture_run(&frame, map, 1, 0, 4, points) >= 3 && !minimap_picture_run(&frame, &(minimap_picture_t){ 5000.0f, 10.0f, 6000.0f, 0.0f, 1, 1, map->cells }, 0, 0, 1, points));
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    CHECK(la_state_read(&state, g_saved, g_saved_size) && !strcmp(state.map, "map.bmp") && state.map_corners[3] == -200.0f);
    load_world("C:\\data\\own.scn", 0);
    CHECK(!level_actions_map());
    load_world("D:\\SAVEGAME.001", 0);
    CHECK(level_actions_map());
    remove("level_actions_map.bmp");
    new_group(LA_MAP);
    put_string(SYM(404), "gone.bmp");
    start(add(g_group));
    CHECK(!level_actions_map());
    g_session = 0;
    CHECK(!level_actions_map());
    g_session = 1;
    g_map_file[0] = 0;
}

static float floats_at(uint32_t at) {
    float f;
    memcpy(&f, (const void*)(uintptr_t)at, 4);
    return f;
}

static void put_health(float now, float most) {
    memcpy((void*)(uintptr_t)(PLAYER_AT + 0x50), &now, 4);
    memcpy((void*)(uintptr_t)(PLAYER_AT + 0x54), &most, 4);
}

static void test_health(void) {
    uint32_t hurt, cure, once;
    fresh(BIT(104));
    put_health(80.0f, 100.0f);
    new_group(LA_HEALTH), put_float(SYM(581), 30.0f), hurt = add(g_group);
    new_group(LA_HEALTH), cure = add(g_group);
    new_group(LA_HEALTH), put_float(SYM(581), -5.0f), put_bool(SYM(416), 1), once = add(g_group);
    start(hurt);
    CHECK(floats_at(PLAYER_AT + 0x50) == 50.0f);
    start(once), start(once);
    CHECK(floats_at(PLAYER_AT + 0x50) == 55.0f);
    start(cure);
    CHECK(floats_at(PLAYER_AT + 0x50) == 100.0f);
    load_world("C:\\data\\own.scn", 0);
    start(once);
    CHECK(floats_at(PLAYER_AT + 0x50) == 105.0f);
    /* A scene of Trespasser CE that kept it run. */
    load_world("C:\\data\\own.scn", 0);
    g_scene_version = 26;
    {
        const uint32_t kept = alloc(4);
        MEM32(kept) = 3;
        enter(g_load, once, 1, kept);
        CHECK(g_eax == kept + 4);
    }
    start(once);
    CHECK(floats_at(PLAYER_AT + 0x50) == 105.0f);
    CHECK(la_health(10.0f, 100.0f, 999.0f) == -989.0f);
}

static uint32_t with_physics(uint32_t instance, uint32_t shape) {
    const uint32_t info = alloc(0x20);
    MEM32(info) = INFO_KIND;
    MEM32(info + 0x08) = shape;
    MEM32(info + 0x0C) = 0x00712300u;
    MEM32(instance + 0x28) = info;
    return info;
}

static void step_game(float seconds) {
    float now = floats_at(GVA(0x0073F34C)) + seconds;
    memcpy((void*)(uintptr_t)GVA(0x0073F34C), &now, 4);
    g_esp = STACK_TOP;
    level_actions_step();
    CHECK(g_esp == STACK_TOP);
}

/* The scripts' OnSimStep follows the game's simulation time, as the speed actions do; nothing while it stands still. */
static void test_script_step(void) {
    int steps;
    fresh(0);
    step_game(0.1f);
    steps = g_scripts.steps;
    step_game(0.25f);
    CHECK(g_scripts.steps == steps + 1 && fabsf(g_scripts.seconds - 0.25f) < 1e-3f);
    step_game(0.0f);
    CHECK(g_scripts.steps == steps + 1);
}

static void test_fixed_speed(void) {
    uint32_t arrow, aimed, straight, gentle, end, lone;
    la_state_t state;
    fresh(BIT(110) | BIT(111));
    arrow = instance("Arrow", 0, 0, 0, 0);
    with_physics(arrow, 0);
    instance("Bow", 0, 0, 0, 0);
    lone = instance("Cloud", 0, 0, 0, 0);
    MEM32(with_physics(lone, 0) + 0x0C) = 0;
    new_group(LA_FIX_SPEED), put_string(SYM(404), "Arrow"), put_float(SYM(490), 1.0f), put_float(SYM(491), 2.0f), put_float(SYM(492), 3.0f), straight = add(g_group);
    new_group(LA_FIX_SPEED), put_string(SYM(404), "Arrow"), put_string(SYM(467), "Bow"), put_float(SYM(1262), 8.0f), put_int(SYM(518), 250), put_bool(SYM(581), 0), aimed = add(g_group);
    new_group(LA_FIX_SPEED), put_string(SYM(404), "Cloud"), gentle = add(g_group);
    new_group(LA_END_SPEED), put_string(SYM(404), "Arrow"), end = add(g_group);
    step_game(0.1f);
    start(gentle);
    start(straight);
    step_game(0.1f);
    CHECK(g_physics_count == 1 && g_physics[0].target == arrow && g_physics[0].velocity[0] == 1.0f && g_physics[0].velocity[2] == 3.0f && !g_physics[0].frozen && !g_physics[0].impulse);
    /* Without Frame the speed it has stays. */
    start(aimed);
    step_game(0.1f);
    CHECK(g_physics_count == 2 && g_physics[1].velocity[0] == 1.0f);
    /* Not while the game stands still. */
    step_game(0.0f);
    CHECK(g_physics_count == 2);
    start(end);
    step_game(0.1f);
    CHECK(g_physics_count == 2);
    start(aimed);
    step_game(0.1f);
    CHECK(g_physics_count == 3 && g_physics[2].velocity[0] == 0.0f && fabsf(g_physics[2].velocity[1] - 8.0f) < 1e-5f);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    CHECK(la_state_read(&state, g_saved, g_saved_size) && state.fixed_count == 1 && fabsf(state.fixed[0].left - 0.15f) < 1e-4f);
    step_game(0.1f);
    step_game(0.1f);
    CHECK(g_physics_count == 5);
    step_game(0.1f);
    CHECK(g_physics_count == 5);
    load_world("C:\\data\\own.scn", 0);
    step_game(0.1f);
    CHECK(g_physics_count == 5);
    load_world("D:\\SAVEGAME.001", 0);
    step_game(0.1f);
    CHECK(g_physics_count == 6 && fabsf(g_physics[5].velocity[1] - 8.0f) < 1e-5f);
    {
        la_fixed_t f = { 1, { 0 }, la_fixed_seconds(-1) };
        for (int i = 0; i < 1000; i++) CHECK(la_fixed_step(&f, 10.0f));
        f.left = la_fixed_seconds(0);
        CHECK(!la_fixed_step(&f, 0.01f));
    }
    g_session = 0;
    step_game(0.1f);
    CHECK(g_physics_count == 6);
}

static void test_player_ai(void) {
    uint32_t hide, toggle, info, shared;
    fresh(BIT(113));
    shared = alloc(0x18);
    MEM32(shared + 8) = 5;
    for (int i = 0; i < 3; i++) {
        const uint32_t part = instance("part", 0, 0, 0, 0);
        info = with_physics(part, 0);
        MEM32(info + 0x10) = i < 2 ? shared : 0;
        MEM32(PLAYER_AT + 0xAC + 4u * (uint32_t)i) = part;
    }
    new_group(LA_PLAYER_AI), put_int(SYM(581), 0), hide = add(g_group);
    new_group(LA_PLAYER_AI), put_int(SYM(581), -1), toggle = add(g_group);
#define PART_AI(i) MEM32(MEM32(MEM32(MEM32(PLAYER_AT + 0xAC + 4u * (i)) + 0x28) + 0x10) + 8)
    start(hide);
    CHECK(PART_AI(0u) == 0 && PART_AI(1u) == 0);
    /* Others that had the same AI info keep it. */
    CHECK(MEM32(shared + 8) == 5);
    start(toggle);
    CHECK(PART_AI(0u) == 5);
    start(toggle);
    CHECK(PART_AI(0u) == 0);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    load_world("C:\\data\\own.scn", 0);
    CHECK(PART_AI(0u) == 5 && PART_AI(1u) == 5);
    load_world("D:\\SAVEGAME.001", 0);
    CHECK(PART_AI(0u) == 0 && PART_AI(1u) == 0);
    CHECK(!la_ai_valid(8) && !la_ai_valid(-2) && la_ai_type(3, 5) == 3);
    new_group(LA_PLAYER_AI), put_int(SYM(581), 99);
    start(add(g_group));
    CHECK(PART_AI(0u) == 0);
}

static void test_use(void) {
    uint32_t gun, lever, plain, quiet, counted, silent;
    fresh(BIT(115));
    gun = instance("Gun", 0, 0, 0, GVA(0x0064A274));
    lever = instance("Lever", 0, 0, 0, 0x00645000u);
    MEM32(gun + 0x6C) = 2;
    MEM8(GVA(0x0066ECC0) + 1u) = 0;
    MEM32(GVA(0x0073F280)) = 41;
    new_group(LA_USE), put_string(SYM(38), "Lever"), plain = add(g_group);
    new_group(LA_USE), put_string(SYM(38), "Gun"), put_bool(SYM(404), 1), put_bool(SYM(416), 1), put_bool(SYM(518), 1), quiet = add(g_group);
    new_group(LA_USE), put_string(SYM(38), "Gun"), put_bool(SYM(416), 1), put_bool(SYM(581), 1), counted = add(g_group);
    new_group(LA_USE), silent = add(g_group);
    start(plain);
    CHECK(g_uses == 1 && g_used == lever && !g_unlimited && g_sent == 1 && g_esp == STACK_TOP);
    CHECK(g_sent_message[0] == GVA(0x0064ADF4) && g_sent_message[3] == GVA(0x0073F278) && g_sent_message[4] == lever && MEM32(GVA(0x0073F280)) == 42);
    start(quiet);
    CHECK(g_uses == 2 && g_unlimited == 1 && MEM8(GVA(0x0066ECC0) + 1u) == 0 && MEM32(gun + 0x6C) == 1);
    start(quiet);
    CHECK(MEM32(gun + 0x6C) == 0xFFFFFFFFu);
    MEM32(gun + 0x6C) = 1;
    start(counted);
    CHECK(MEM32(gun + 0x6C) == 0 && g_sent == 3 && !g_unlimited);
    start(counted);
    CHECK(MEM32(gun + 0x6C) == 0xFFFFFFFFu);
    start(silent);
    CHECK(g_uses == 5);
    new_group(LA_USE), put_string(SYM(38), "Nobody");
    start(add(g_group));
    CHECK(g_uses == 5);
}

static void test_jump(void) {
    uint32_t dino, jump, other;
    fresh(BIT(116));
    dino = instance("Dino", 1.0f, 2.0f, 3.0f, ANIMAL_KIND);
    with_physics(dino, 0);
    instance("Crate", 0, 0, 0, 0x00645000u);
    new_group(LA_JUMP), put_string(SYM(38), "Dino"), put_float(SYM(1208), 400.0f), put_float(SYM(581), 45.0f), jump = add(g_group);
    new_group(LA_JUMP), put_string(SYM(38), "Crate"), other = add(g_group);
    start(jump);
    start(other);
    CHECK(g_physics_count == 1 && g_physics[0].target == dino && g_physics[0].impulse == 1 && !g_physics[0].frozen && g_physics[0].push == 400.0f && !g_physics[0].emitter);
    CHECK(g_physics[0].position[0] == 1.0f && g_physics[0].position[2] == -97.0f);
    CHECK(fabsf(g_physics[0].facing[0]) < 1e-5f && fabsf(g_physics[0].facing[1] - 0.70710678f) < 1e-5f && fabsf(g_physics[0].facing[2] - 0.70710678f) < 1e-5f);
}

static uint32_t texture(void) {
    const uint32_t at = alloc(0x40);
    MEM32(at + 4) = 1;
    return at;
}

static void test_texture(void) {
    uint32_t screen, mesh, textures, frames, a, again, beyond, flat;
    la_state_t state;
    fresh(BIT(118));
    mesh = alloc(0xC0), textures = alloc(8), frames = alloc(8);
    MEM32(mesh) = MESH_KIND;
    MEM32(mesh + 0x38) = 2, MEM32(mesh + 0x3C) = textures, MEM32(mesh + 0xA8) = 2, MEM32(mesh + 0xAC) = frames;
    MEM32(textures) = texture(), MEM32(textures + 4) = texture(), MEM32(frames) = texture(), MEM32(frames + 4) = texture();
    screen = instance("Screen", 0, 0, 0, 0);
    with_physics(screen, mesh);
    flat = alloc(0xC0);
    MEM32(flat) = SHAPE_KIND;
    with_physics(instance("Wall", 0, 0, 0, 0), flat);
    new_group(LA_TEXTURE), put_string(SYM_A(0), "Screen"), put_int(SYM(404), 2), put_int(SYM(581), 1), a = add(g_group);
    new_group(LA_TEXTURE), put_string(SYM_A(0), "Screen"), put_int(SYM(404), 2), again = add(g_group);
    new_group(LA_TEXTURE), put_string(SYM_A(0), "Screen"), put_int(SYM(404), 3), beyond = add(g_group);
    {
        const uint32_t original = MEM32(textures + 4), first = MEM32(frames), second = MEM32(frames + 4);
        start(a);
        CHECK(MEM32(textures + 4) == second && MEM32(second + 4) == 2 && MEM32(original + 4) == 1 && !g_deleted);
        start(again);
        CHECK(MEM32(textures + 4) == first && MEM32(first + 4) == 2 && MEM32(second + 4) == 1);
        start(beyond);
        start(a);
        CHECK(MEM32(textures + 4) == second);
        g_saved_size = g_write_state(g_saved, sizeof g_saved);
        CHECK(la_state_read(&state, g_saved, g_saved_size) && state.swap_count == 1 && state.swaps[0].material == 1 && state.swaps[0].frame == 1);
        load_world("C:\\data\\own.scn", 0);
        CHECK(MEM32(textures + 4) == original && MEM32(original + 4) == 1 && MEM32(second + 4) == 1 && MEM32(first + 4) == 1 && !g_deleted);
        load_world("D:\\SAVEGAME.001", 0);
        CHECK(MEM32(textures + 4) == second);
        /* A mesh that is another one by then is left alone. */
        MEM32(MEM32(screen + 0x28) + 8) = flat;
        load_world("C:\\data\\own.scn", 0);
        CHECK(MEM32(textures + 4) == second);
    }
    new_group(LA_TEXTURE), put_string(SYM_A(0), "Wall");
    start(add(g_group));
    CHECK(MEM32(flat + 0x38) == 0);
}

static void test_trigger(void) {
    uint32_t other, loops, again, off, loop;
    fresh(BIT(120));
    other = instance("Other", 0, 0, 0, TRIGGER_KIND);
    loops = instance("Loops", 0, 0, 0, TRIGGER_KIND);
    instance("Crate", 0, 0, 0, 0x00645000u);
    MEM32(loops + 0xBC) = 4;
    MEM8(loops + 0xB8) = 1;
    new_group(LA_TRIGGER), put_string(SYM_A(0), "Other"), put_int(SYM(404), 1), put_int(SYM(416), 25), again = add(g_group);
    new_group(LA_TRIGGER), put_string(SYM_A(0), "Other"), put_int(SYM(404), 0), off = add(g_group);
    new_group(LA_TRIGGER), put_string(SYM_A(0), "Loops"), put_int(SYM(404), 9), put_bool(SYM(581), 1), loop = add(g_group);
    start(again);
    CHECK(MEM32(other + 0xA8) == 1 && MEM32(other + 0xAC) == 25u * 0x028F5C28u && MEM32(other + 0xBC) == 0);
    start(off);
    CHECK(MEM32(other + 0xA8) == 0 && MEM32(other + 0xAC) == 0xFFFFFFFFu);
    start(loop);
    CHECK(MEM32(loops + 0xA8) == 9 && MEM32(loops + 0xBC) == 9);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    load_world("C:\\data\\own.scn", 0);
    CHECK(MEM32(loops + 0xBC) == 4);
    load_world("D:\\SAVEGAME.001", 0);
    CHECK(MEM32(loops + 0xBC) == 9);
    new_group(LA_TRIGGER), put_string(SYM_A(0), "Other");
    start(add(g_group));
    new_group(LA_TRIGGER), put_string(SYM_A(0), "Crate"), put_int(SYM(404), 1);
    start(add(g_group));
    CHECK(MEM32(other + 0xA8) == 0);
}

static void test_reload_guns(void) {
    uint32_t gun, other, a;
    fresh(BIT(121));
    gun = instance("Gun", 0, 0, 0, GVA(0x0064A274));
    other = instance("Other", 0, 0, 0, GVA(0x0064A274));
    instance("Crate", 0, 0, 0, 0x00645000u);
    MEM32(gun + 0x68) = 12, MEM32(other + 0x68) = 30;
    new_group(LA_RELOAD_GUNS), put_int(SYM(1198), 2), put_int(SYM(581), 0);
    put_string(SYM_A(0), "Gun"), put_string(SYM_A(1), "Crate"), put_string(SYM_A(2), "Other");
    a = add(g_group);
    start(a);
    CHECK(MEM32(gun + 0x6C) == 10 && MEM32(other + 0x6C) == 28);
    new_group(LA_RELOAD_GUNS), put_int(SYM(1194), 1), put_string(SYM_A(0), "Gun");
    start(add(g_group));
    CHECK(MEM32(gun + 0x6C) == 1 && MEM32(other + 0x6C) == 28);
    new_group(LA_RELOAD_GUNS), put_int(SYM(1194), 1);
    start(add(g_group));
    CHECK(MEM32(gun + 0x6C) == 1);
}

static void test_colour(void) {
    uint32_t a;
    fresh(BIT(109));
    MEM32(0x00712344u) = 0x00123456u;
    new_group(LA_COLOUR), put_string(SYM(404), "Sparks"), put_int(SYM(659), 300), put_int(SYM(660), 16), put_int(SYM(661), -4), a = add(g_group);
    /* At every load, and when its trigger starts. */
    enter(g_load, a, 1, 0x1000u);
    CHECK(MEM32(0x00712344u) == 0x00FF1000u && g_eax == 0x1000u && g_esp == STACK_TOP);
    load_world("C:\\data\\own.scn", 0);
    CHECK(MEM32(0x00712344u) == 0x00123456u);
    start(a);
    CHECK(MEM32(0x00712344u) == 0x00FF1000u);
    fresh(0);
    MEM32(0x00712344u) = 0x00123456u;
    new_group(LA_COLOUR), put_string(SYM(404), "Sparks"), a = add(g_group);
    enter(g_load, a, 1, 0x1000u);
    CHECK(MEM32(0x00712344u) == 0x00123456u);
}

static void test_video(void) {
    uint32_t a, missing;
    char name[LA_VIDEO_MAX];
    fresh(BIT(105));
    strcpy(g_package_video, "menu\\scene.smk");
    new_group(LA_VIDEO), put_string(SYM(404), "menu/scene"), put_bool(SYM(416), 1), a = add(g_group);
    new_group(LA_VIDEO), put_string(SYM(404), "menu\\gone"), missing = add(g_group);
    g_front.kind = GUEST_WND_GAME, g_front.ui = GUEST_UI_GAMEPLAY, g_front.depth = 2, g_front.top = 0x00700300u;
    MEM32(g_front.top + 4) = 0x00700200u;
    start(a);
    step_game(0.1f);
    CHECK(!g_videos);
    g_front.depth = 1;
    step_game(0.1f);
    CHECK(g_videos == 1 && !strcmp(g_video_name, "menu\\scene") && !strcmp(g_video_served, "menu\\scene.smk") && !g_served_video[0] && !g_paused && !g_video_windows);
    step_game(0.1f);
    start(a);
    step_game(0.1f);
    CHECK(g_videos == 1);
    start(missing);
    step_game(0.1f);
    CHECK(g_videos == 1 && !g_paused);
    load_world("C:\\data\\own.scn", 0);
    start(a);
    load_world("C:\\data\\own.scn", 0);
    step_game(0.1f);
    CHECK(g_videos == 1);
    CHECK(!la_video_name("", name) && !la_video_name("..\\x", name) && !la_video_name("C:\\x", name) && !la_video_name("\\x", name));
    CHECK(!la_video_name("menu\\0123456789012345678901234567890123456789012", name) && la_video_name("menu\\012345678901234567890123456789012345678901", name));
}

/* Only what the package needs runs, unknown types add nothing, and a full trigger takes no more. */
static void test_gating(void) {
    static const int k_none[] = { 0, 35, 36, 98, 107, 108, 119, 122, 1000 };
    uint32_t a;
    fresh(0);
    new_group(LA_SPEED);
    put_float(SYM(1262), 0.5f);
    a = add(g_group);
    start(a);
    CHECK(a && game_speed() == 1.0f);
    for (size_t i = 0; i < sizeof k_none / sizeof k_none[0]; i++) {
        const uint32_t before = MEM32(TRIGGER + 0x9C);
        new_group(k_none[i]);
        add(g_group);
        CHECK(MEM32(TRIGGER + 0x9C) == before && !la_creates(k_none[i]));
    }
    new_group(LA_MAP);
    start(add(g_group));
    CHECK(MEM32(TRIGGER + 0x9C) == 2);
    for (int i = 0; i < 30; i++) new_group(LA_SPEED), add(g_group);
    CHECK(MEM32(TRIGGER + 0x9C) == 16);
}

static void test_scene_bytes(void) {
    uint32_t speed, teleport, map;
    fresh(BIT(101));
    new_group(LA_SPEED), speed = add(g_group);
    new_group(LA_TELEPORT), teleport = add(g_group);
    new_group(LA_MAP), map = add(g_group);
    for (int version = 0; version <= 26; version += 26) {
        g_scene_version = version;
        enter(g_load, speed, 1, 0x1000u);
        CHECK(g_eax == 0x1000u + (version ? 4u : 0u) && g_esp == STACK_TOP);
        enter(g_load, teleport, 1, 0x1000u);
        CHECK(g_eax == 0x1000u);
        enter(g_load, map, 1, 0x1000u);
        CHECK(g_eax == 0x1000u + (version ? 4u : 0u));
        enter(g_save, speed, 1, 0x1000u);
        CHECK(g_eax == 0x1000u && g_esp == STACK_TOP);
    }
    CHECK(la_scene_bytes(102, 25) == 0 && la_scene_bytes(101, 24) == 0 && la_scene_bytes(114, 26) == 0);
    enter(g_load, alloc(0xC), 1, 0x1000u);
    CHECK(g_lifted_loads > 0 && g_eax == 0x1004u);
}

static void test_saves(void) {
    uint32_t speed, hint, reload, gun;
    char text[64];
    la_state_t state;
    fresh(BIT(101) | BIT(103) | BIT(106) | BIT(112));
    gun = instance("Gun", 0, 0, 0, GVA(0x0064A274));
    MEM32(gun + 0x68) = 12;
    MEM32(PLAYER_AT + 0xE34) = gun;
    new_group(LA_RELOAD), put_int(SYM(416), 1), reload = add(g_group);
    new_group(LA_SPEED), put_float(SYM(1262), 0.5f), speed = add(g_group);
    new_group(LA_HINT), put_string(SYM(404), "Saved hint"), hint = add(g_group);
    start(reload), start(speed), start(hint);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    CHECK(g_saved_size > 0 && la_state_read(&state, g_saved, g_saved_size) && state.speed.base == 0.5f && !strcmp(state.hint, "Saved hint") && state.runs[0] == 0);
    for (size_t n = 0; n < g_saved_size; n++) CHECK(!la_state_read(&state, g_saved, n));
    {
        uint8_t first[LA_STATE_MAX];
        memcpy(first, g_saved, g_saved_size);
        first[3] = 0x31;
        CHECK(la_state_read(&state, first, g_saved_size) && state.speed.base == 0.5f && !state.fixed_count && state.player_ai == LA_AI_NONE);
    }
    load_world("C:\\data\\own.scn", 0);
    MEM32(gun + 0x6C) = 0;
    CHECK(game_speed() == 1.0f);
    load_world("D:\\SAVEGAME.001", 0);
    start(reload);
    CHECK(game_speed() == 0.5f);
    CHECK(MEM32(gun + 0x6C) == 0);
    CHECK(level_actions_hint(18000u + MEM32(GVA(0x0066F01C)), text, sizeof text));
    CHECK(!strcmp(text, "Saved hint"));
    g_world_result = -1;
    load_world("D:\\SAVEGAME.002", 0);
    CHECK(game_speed() == 1.0f);

    /* A level read whole again: its actions are new, and the state a save kept is theirs. */
    fresh(BIT(103));
    g_saved_size = 0;
    load_world("C:\\data\\own.scn", 1);
    CHECK(MEM32(TRIGGER + 0x9C) == 2);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    CHECK(la_state_read(&state, g_saved, g_saved_size) && state.run_count == 2 && state.runs[0] == 2 && state.runs[1] == 2);
    load_world("C:\\data\\own.scn", 1);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    CHECK(la_state_read(&state, g_saved, g_saved_size) && state.run_count == 2);
}

/* The game's level load, as a transition, the first level or a save. */
static void play_file(const char* path, const char* scene) {
    uint32_t args[2], result = 0;
    args[0] = alloc(96);
    args[1] = alloc(32);
    strcpy((char*)(uintptr_t)args[0], path);
    strcpy((char*)(uintptr_t)args[1], scene);
    CHECK(guest_call(GVA(0x004142C0), GVA(0x0066E338), args, 2, 0, &result));
}

static void play(const char* scene) {
    char path[96];
    snprintf(path, sizeof path, "C:\\data\\%s", scene);
    play_file(path, scene);
}

/* What playing changes and the scene does not hold. */
static void disturb(void) {
    W.player_x = 99.0f;
    W.crate_taken = W.trigger_fired = W.raptor_alert = 1;
    W.game_time = 50.0f;
}

static int as_new(unsigned player_before) {
    return W.player && W.player != player_before && W.player_x == SPAWN_X && !W.crate_taken && !W.trigger_fired && !W.raptor_alert &&
           W.game_time == 0.0f;
}

static void restart_level(void) { enter(g_restart, GVA(0x0066E338), 0); }

static void test_restart(void) {
    unsigned player, generation, grf, pid, terrain, reads;
    fresh(BIT(101));
    g_world_triggers = 1;
    play("own.scn");
    CHECK(MEM32(TRIGGER + 0x9C) == 2 && !strcmp(W.scene, "own.scn"));

    /* The game's own Restart Level over a custom level reads the scene and keeps its world: the reported bug. */
    player = W.player;
    disturb();
    enter(lifted_restart, GVA(0x0066E338), 0);
    CHECK(W.player == player && W.player_x == 99.0f && W.crate_taken && W.trigger_fired && W.raptor_alert && !g_purges);

    /* Restart Level of a custom session: the level load runs again, the world purged first, everything built anew. */
    g_line_count = 0;
    g_purges = g_level_loads = g_lifted_restarts = g_restarts_told = g_menu_sounds_off = 0;
    player = W.player, grf = W.grf_opens, pid = W.pid_opens, terrain = W.terrain_opens, reads = W.scene_reads;
    generation = jpt_session_world_generation();
    new_group(LA_SPEED), put_float(SYM(1262), 0.5f);
    start(add(g_group));
    CHECK(game_speed() == 0.5f);
    g_saved_size = g_write_state(g_saved, sizeof g_saved);
    restart_level();
    CHECK(g_esp == STACK_TOP && !g_lifted_restarts && g_level_loads == 1 && g_menu_sounds_off == 1 && !strcmp(g_loaded_path, "C:\\data\\own.scn"));
    CHECK(g_purges == 1 && W.scene_reads == reads + 1 && W.grf_opens == grf + 1 && W.pid_opens == pid + 1 && W.terrain_opens == terrain + 1);
    CHECK(as_new(player) && game_speed() == 1.0f && MEM32(TRIGGER + 0x9C) == 2);
    CHECK(jpt_session_world_generation() == generation + 1 && jpt_session_kind() == JPT_SESSION_CUSTOM && !strcmp(jpt_session_current_scene(), "own.scn"));
    CHECK(g_restarts_told == 1 && !strcmp(g_restarted, "own.scn"));
    CHECK(logged("requested scene=own.scn session=custom mode=full source=package"));
    CHECK(logged("begin reason=restart scene=own.scn source=package session=custom"));
    CHECK(logged("ready scene=own.scn source=package world_generation="));
    CHECK(logged("result=ready old_world_generation=") && !logged("result=failed"));

    /* Twice in a row. */
    player = W.player, generation = jpt_session_world_generation();
    disturb();
    restart_level();
    CHECK(as_new(player) && g_purges == 2 && g_level_loads == 2 && jpt_session_world_generation() == generation + 1);

    /* The save made before the restart loads as a save, with the state it kept. */
    g_line_count = 0;
    play_file("D:\\SAVEGAME.001", "own.scn");
    CHECK(logged("begin reason=load_save scene=own.scn") && game_speed() == 0.5f && g_purges == 2);

    /* After a transition, Restart restarts the level the session is in, not its first one. */
    g_line_count = 0;
    play("other.scn");
    CHECK(logged("begin reason=transition scene=other.scn source=package session=custom"));
    player = W.player;
    disturb();
    restart_level();
    CHECK(!strcmp(g_loaded_path, "C:\\data\\other.scn") && !strcmp(W.scene, "other.scn") && as_new(player));

    /* A level of the original game the package leads to: restarted whole, and the session stays custom. */
    g_line_count = 0;
    play("sum.scn");
    player = W.player;
    disturb();
    restart_level();
    CHECK(!strcmp(W.scene, "sum.scn") && as_new(player) && jpt_session_kind() == JPT_SESSION_CUSTOM);
    CHECK(logged("begin reason=transition scene=sum.scn source=retail session=custom"));
    CHECK(logged("requested scene=sum.scn session=custom mode=full source=retail"));

    /* A load that fails is a failed restart. */
    g_line_count = 0;
    g_world_result = -1;
    restart_level();
    CHECK(logged("failed scene=sum.scn") && logged("result=failed"));
    g_world_result = 0;

    /* DisableSimpleLevelRestart also purges before a save of the same level loads. */
    g_full_restart = 1;
    g_purges = 0;
    load_world("C:\\data\\own.scn", 0);
    CHECK(g_purges == 1);
    load_world("C:\\data\\absent.scn", 0);
    CHECK(g_purges == 1);
    g_full_restart = 0;

    /* The campaign after the custom session keeps the game's own Restart Level, unchanged. */
    jpt_session_end();
    g_session = 0;
    g_installs = 0;
    CHECK(level_actions_install() && g_installs == 1);
    g_line_count = 0;
    memset(&g_scripts, 0, sizeof g_scripts);
    level_session_start("en-116", "d3d11", "en");
    CHECK(g_scripts.starts == 1 && g_scripts.kind == JPT_SESSION_CAMPAIGN);
    g_purges = g_level_loads = g_lifted_restarts = g_restarts_told = 0;
    play("be.scn");
    CHECK(logged("type=campaign") && logged("begin reason=new_game scene=be.scn source=retail session=campaign"));
    player = W.player;
    disturb();
    restart_level();
    g_menu_sounds_off = 0;
    restart_level();
    CHECK(g_lifted_restarts == 2 && g_level_loads == 1 && !g_purges && !g_restarts_told && !g_menu_sounds_off && W.player == player && W.crate_taken);
    CHECK(g_scripts.begins == 2 && g_scripts.reason == JPT_REASON_RESTART && g_scripts.readies == 2 && g_scripts.ok);
    CHECK(logged("requested scene=be.scn session=campaign mode=scene_reload source=retail") && logged("begin reason=restart scene=be.scn"));
    CHECK(logged("result=ready") && g_esp == STACK_TOP);
    play("jr.scn");
    CHECK(logged("begin reason=transition scene=jr.scn source=retail session=campaign"));
}

static void test_session_end(void) {
    uint32_t speed;
    char text[32];
    fresh(BIT(101) | BIT(106) | BIT(112));
    new_group(LA_SPEED), put_float(SYM(1262), 0.5f), speed = add(g_group);
    start(speed);
    CHECK(game_speed() == 0.5f && g_write_state);
    g_session = 0;
    g_end();
    CHECK(game_speed() == 1.0f && !g_write_state && !level_actions_hint(18000u + MEM32(GVA(0x0066F01C)), text, sizeof text));
    start(speed);
    CHECK(game_speed() == 1.0f);
}

int main(void) {
    const uintptr_t lo = (uintptr_t)g_window;
    setvbuf(stdout, NULL, _IONBF, 0);
    test_profile_activate();
    if (lo > 0x00645000u || lo + sizeof g_window < WINDOW_HI) {
        printf("the guest window is at %08X\n", (unsigned)lo);
        return 2;
    }
    test_retail_session();
    test_speed();
    test_hint();
    test_controls();
    test_reload();
    test_teleport();
    test_particles();
    test_map();
    test_health();
    test_fixed_speed();
    test_player_ai();
    test_use();
    test_jump();
    test_texture();
    test_trigger();
    test_reload_guns();
    test_colour();
    test_video();
    test_gating();
    test_scene_bytes();
    test_saves();
    test_script_step();
    test_restart();
    test_session_end();
    printf("level_actions_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
