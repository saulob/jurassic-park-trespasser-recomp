/* The engine's own arrays and heaps (the lifted functions, copied out of src/recomp/gen) over the host's allocation checks:
 * full, one element past full, out of memory, and the session after each. No window, no game data.
 * The guest addresses the lifted code uses live in g_window, which the fixed image base puts there. */
#include <windows.h>

#include <psapi.h>

#include "test_profile.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "engine_mode.h"
#include "jpt_custom_level.h"
#include "guest_abi.h"
#include "jpt_level_gate.h"
#include "level_abort.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
#ifdef RECOMP_X87_RING
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;
void recomp_x87_recentre(void) {}
#endif
uint16_t g_fpu_cw = 0x027F;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;

void shim_VirtualAlloc(void);
void shim_operator_new(void);
void shim_malloc(void);
void shim_set_new_handler(void);
void shim__set_new_mode(void);
void lifted_sub_005F8A20(void);
void lifted_sub_005F8B10(void);
void sub_004B7390(void);
void sub_004B73E0(void);
void sub_004D6820(void);
void sub_004912D0(void);
void sub_0051D820(void);
void sub_0047E470(void);
void sub_004A3320(void);
void sub_0048A3D0(void);
void sub_005FA870(void);

#define FAKE_VIRTUAL_ALLOC 0x7E000010u
#define IAT_VIRTUAL_ALLOC 0x00645108u
#define PAGE_SIZE_VA 0x00669B88u
#define STACK_TOP 0x007EF000u
#define OBJECT 0x00700000u
#define WINDOW_HI 0x00800000u
#define GUARD 0x10000u
#define PAGE 0x1000u

static uint8_t g_window[0x600000];
static unsigned long g_checks, g_failures;
static jmp_buf g_stop;
static char g_log[8192], g_message[128];
static int g_custom, g_original;
static unsigned g_faults;

#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static void put32(uint32_t va, uint32_t v) { memcpy((void*)(uintptr_t)va, &v, 4); }
static uint32_t get32(uint32_t va) {
    uint32_t v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    return v;
}

/* ---- the host around the checks ---- */

void host_log(const char* tag, const char* fmt, ...) {
    const size_t used = strlen(g_log);
    va_list ap;
    (void)tag;
    va_start(ap, fmt);
    if (used + 2 < sizeof g_log) vsnprintf(g_log + used, sizeof g_log - used - 1, fmt, ap);
    va_end(ap);
    strncat(g_log, "\n", sizeof g_log - strlen(g_log) - 1);
}

__declspec(noreturn) void host_stop_level(const char* message) {
    snprintf(g_message, sizeof g_message, "%s", message);
    longjmp(g_stop, 1);
}

int jpt_custom_level_active(void) { return g_custom; }
const char* jpt_custom_level_package(void) { return "Package"; }
const char* jpt_custom_level_current_scene(void) { return "level.scn"; }
static unsigned g_mesh_heap_mb, g_terrain_nodes, g_pool_mb, g_cull_mm, g_mesh_faces = 8192;
unsigned jpt_custom_level_limit(const char* key) {
    if (!strcmp(key, "terrain_cull_mm")) return g_cull_mm;
    return !strcmp(key, "mesh_heap_mb") ? g_mesh_heap_mb : !strcmp(key, "terrain_nodes") ? g_terrain_nodes : !strcmp(key, "mesh_faces") ? g_mesh_faces : g_pool_mb;
}
int jpt_custom_level_requires(jpt_level_cap_t cap) { return g_custom && cap == JPT_LEVEL_CAP_TERRAIN_CULL && g_cull_mm; }
static void (*g_session_end)(void);
void jpt_custom_level_on_end(void (*ended)(void)) { g_session_end = ended; }
int engine_limits_original(void) { return g_original; }
const char* jpt_gate_message(jpt_gate_t gate) { return gate == JPT_GATE_UNSAFE ? "This level cannot be loaded safely" : NULL; }

static recomp_func_t g_reserve = lifted_sub_005F8A20, g_commit = lifted_sub_005F8B10;
int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    if (va == GVA(0x005F8A20)) g_reserve = replacement, *lifted = lifted_sub_005F8A20;
    else if (va == GVA(0x005F8B10)) g_commit = replacement, *lifted = lifted_sub_005F8B10;
    else return 0;
    return 1;
}

void sub_005F8A20(void) { g_reserve(); }
void sub_005F8B10(void) { g_commit(); }
/* The engine's memory statistics. */
void sub_005F6BC0(void) { g_esp += 4; }

/* The engine's own end of a reservation that cannot be made: never reached, the host asks first. */
void sub_005F69A0(void) { g_faults++; }
void sub_005F8770(void) { g_faults++; }

recomp_func_t recomp_lookup(uint32_t va) { return va == FAKE_VIRTUAL_ALLOC ? shim_VirtualAlloc : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}
recomp_func_t recomp_lookup_import(uint32_t va) {
    (void)va;
    return NULL;
}

/* ---- calls ---- */

typedef enum { RAN, STOPPED, FAULTED } outcome_t;

static int count_fault(DWORD code) {
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    g_faults++;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void enter(recomp_func_t fn, uint32_t self, const uint32_t* args, int count) {
    g_esp = STACK_TOP;
    for (int i = count - 1; i >= 0; i--) put32(g_esp -= 4, args[i]);
    put32(g_esp -= 4, 0);
    g_ecx = self;
    fn();
}

static outcome_t guarded(recomp_func_t fn, uint32_t self, const uint32_t* args, int count) {
    outcome_t outcome = RAN;
    __try {
        enter(fn, self, args, count);
    } __except (count_fault(GetExceptionCode())) {
        outcome = FAULTED;
    }
    return outcome;
}

static outcome_t call(recomp_func_t fn, uint32_t self, int count, ...) {
    uint32_t args[8];
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    g_log[0] = g_message[0] = 0;
    if (setjmp(g_stop)) return STOPPED;
    return guarded(fn, self, args, count);
}

static int logged(const char* text) { return strstr(g_log, text) != NULL; }

static int limit_stop(const char* what) {
    return logged("ENGINE_LIMIT_EXCEEDED") && logged(what) && logged("CONTROLLED_LOAD_FAILURE: level.scn of Package") && !strcmp(g_message, "This level cannot be loaded safely");
}

static uint32_t g_made[64];
static int g_made_count;

static void session(int custom, unsigned mesh_heap_mb, unsigned terrain_nodes) {
    for (int i = 0; i < g_made_count; i++) VirtualFree((void*)(uintptr_t)g_made[i], 0, MEM_RELEASE);
    g_made_count = 0;
    memset((void*)(uintptr_t)OBJECT, 0, 0x10000);
    g_custom = custom;
    g_mesh_heap_mb = mesh_heap_mb;
    g_terrain_nodes = terrain_nodes;
    engine_heaps_install();
}

static uint32_t made(uint32_t base) {
    if (base) g_made[g_made_count++] = base;
    return base;
}

/* The guard behind a heap is reserved, was never committed and never written. */
static int guard_is_whole(uint32_t end) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((const void*)(uintptr_t)end, &mi, sizeof mi) && mi.State == MEM_RESERVE && mi.RegionSize >= GUARD && engine_heaps_guard(end) && engine_heaps_guard(end + GUARD - 1) && !engine_heaps_guard(end + GUARD) && !engine_heaps_guard(end - 1);
}

/* ---- the arrays a mesh is built in ---- */

typedef struct {
    uint32_t at;        /* of the array in the builder */
    recomp_func_t grow; /* its pages for a count */
    uint32_t element, elements;
    const char* name;
} array_t;

static const array_t k_mesh[] = {
    { 0x10, sub_004B7390, 40, 8192, "the mesh builder's polygons" },      { 0x24, sub_004D6820, 4, 8192 * 16, "the mesh builder's vertex pointers" },
    { 0x38, sub_004B73E0, 32, 8192 * 4, "the mesh builder's vertices" }, { 0x4C, sub_004912D0, 12, 8192 * 4, "the mesh builder's points" },
};

static outcome_t add(const array_t* a, uint32_t count) {
    put32(OBJECT + a->at + 4, count);
    return call(a->grow, OBJECT + a->at, 1, count);
}

static void build(void) {
    CHECK(call(sub_0048A3D0, OBJECT, 1, 2048u) == RAN);
    for (size_t i = 0; i < 6; i++) made(get32(OBJECT + (i < 5 ? 0x10 + 0x14 * (uint32_t)i : 0x74) + 8));
}

static void fill(const array_t* a, uint32_t count) {
    uint8_t* base = (uint8_t*)(uintptr_t)get32(OBJECT + a->at + 8);
    CHECK(add(a, count) == RAN && !g_log[0]);
    for (uint32_t i = 0; i < count * a->element; i += 64) base[i] = (uint8_t)i;
    base[count * a->element - 1] = 1;
}

static void test_mesh_arrays(void) {
    for (size_t i = 0; i < sizeof k_mesh / sizeof k_mesh[0]; i++) {
        const array_t* a = &k_mesh[i];
        const uint32_t bytes = a->element * a->elements, pages = (bytes + PAGE - 1) & ~(PAGE - 1), most = pages / a->element;
        uint32_t end;
        session(1, 0, 0);
        build();
        end = get32(OBJECT + a->at + 8) + pages;
        fill(a, 1);
        fill(a, a->elements / 2);
        fill(a, a->elements);
        CHECK(guard_is_whole(end));
        CHECK(add(a, most + 1) == STOPPED && limit_stop(a->name) && guard_is_whole(end));
        CHECK(add(a, 0x00FFFFFFu / a->element) == STOPPED && limit_stop(a->name) && guard_is_whole(end));

        /* The same builder after the failure, and one that was never filled. */
        fill(a, a->elements / 4);
        session(1, 0, 0);
        build();
        CHECK(add(a, most + 1) == STOPPED && limit_stop(a->name));
    }
}

static void test_mesh_halfway(void) {
    uint32_t ends[4];
    session(1, 0, 0);
    build();
    for (size_t i = 0; i < 4; i++) ends[i] = get32(OBJECT + k_mesh[i].at + 8) + ((k_mesh[i].element * k_mesh[i].elements + PAGE - 1) & ~(PAGE - 1));
    fill(&k_mesh[0], 3000);
    fill(&k_mesh[1], 9000);
    CHECK(add(&k_mesh[2], 8192 * 4 + 1) == STOPPED && limit_stop(k_mesh[2].name));
    CHECK(get32(OBJECT + k_mesh[3].at + 4) == 0);
    for (size_t i = 0; i < 4; i++) CHECK(guard_is_whole(ends[i]));
    session(1, 0, 0);
    build();
    for (size_t i = 0; i < 4; i++) fill(&k_mesh[i], k_mesh[i].elements);
    session(0, 0, 0);
    build();
    fill(&k_mesh[0], 2048);
}

/* ---- heaps that hand out memory one after another ---- */

static uint32_t make_heap(uint32_t bytes) {
    CHECK(call(sub_004A3320, OBJECT, 2, bytes, 0u) == RAN);
    return made(get32(OBJECT + 8));
}

static void test_heap(uint32_t asked, uint32_t reserved, const char* name) {
    const uint32_t pages = (reserved + PAGE - 1) & ~(PAGE - 1);
    uint32_t base = make_heap(asked), got = 0;
    CHECK(base && guard_is_whole(base + pages));
    while (got + 0x8000 <= pages && call(sub_0047E470, OBJECT, 1, 0x8000u) == RAN && g_eax == base + got) {
        memset((void*)(uintptr_t)g_eax, 7, 0x8000);
        got += 0x8000;
    }
    CHECK(got + 0x8000 > pages);
    if (got < pages) {
        CHECK(call(sub_0047E470, OBJECT, 1, pages - got) == RAN && g_eax == base + got);
        memset((void*)(uintptr_t)(base + got), 7, pages - got);
    }
    CHECK(get32(OBJECT + 4) == pages && guard_is_whole(base + pages));
    CHECK(call(sub_0047E470, OBJECT, 1, 4u) == STOPPED && limit_stop(name) && guard_is_whole(base + pages));
}

static void test_heaps(void) {
    session(1, 0, 0);
    test_heap(0x2000000u, 32u << 20, "the heap of loaded meshes");
    session(1, 40, 0);
    test_heap(0x2000000u, 40u << 20, "the heap of loaded meshes");
    session(1, 64, 0);
    test_heap(0x2000000u, 64u << 20, "the heap of loaded meshes");
    session(1, 0, 0);
    test_heap(996520u, 996520u, "the nodes of the terrain's queries");
    session(1, 0, 10);
    test_heap(996520u, 996520u * 2u, "the nodes of the terrain's queries");
    session(1, 0, 0);
    test_heap(1758600u, 1758600u * TERRAIN_HEAP_SCALE, "the terrain's nodes");
    session(1, 0, 0);
    test_heap(0x1000000u, 0x1000000u, "a 16 MB heap of objects");
    session(1, 0, 0);
    test_heap(4000u, 4000u, "the terrain's query rectangles");
    g_original = 1;
    session(1, 0, 0);
    test_heap(1758600u, 1758600u, "the terrain's nodes");
    g_original = 0;

    /* One request larger than the whole heap. */
    session(1, 0, 0);
    make_heap(996520u);
    CHECK(call(sub_0047E470, OBJECT, 1, 996520u * 3u) == STOPPED && limit_stop("the nodes of the terrain's queries"));
    CHECK(call(sub_0047E470, OBJECT, 1, 0x40000000u) == STOPPED && limit_stop("the nodes of the terrain's queries"));
}

/* ---- the scene's arrays: polygons and their pointers are asked for room first, the others are not ---- */

#define POLYGONS 0x00732F28u
#define POLYGON_POINTERS 0x00732F10u
#define VERTICES 0x00732EF0u

static uint32_t make_scene_array(uint32_t va, uint32_t capacity, uint32_t element) {
    memset((void*)(uintptr_t)va, 0, 0x14);
    put32(va + 0xC, capacity);
    CHECK(call(sub_005F8A20, 0, 1, capacity * element) == RAN);
    put32(va + 8, made(g_eax));
    return g_eax + ((get32(va + 0xC) * element + PAGE - 1) & ~(PAGE - 1));
}

static void test_scene(void) {
    for (int custom = 1; custom >= 0; custom--) {
        uint32_t end, capacity;
        session(custom, 0, 0);
        end = make_scene_array(POLYGONS, 20000, 0x7C);
        capacity = get32(POLYGONS + 0xC);
        CHECK(capacity == 80000u);
        put32(POLYGONS + 4, 79000);
        CHECK(call(sub_0051D820, POLYGONS, 1, 79000u) == RAN);
        memset((void*)(uintptr_t)get32(POLYGONS + 8), 1, 79000 * 0x7C);
        /* A shape asks for more than is left: the engine looks at the capacity and leaves it out. */
        CHECK(call(sub_0051D820, POLYGONS, 1, 79000u + 5000u) == RAN && !g_message[0]);
        CHECK(!custom || guard_is_whole(end));
        CHECK(call(sub_0051D820, POLYGONS, 1, 79000u + 9000u) == RAN && !logged("ENGINE_LIMIT_EXCEEDED"));
        memset((void*)(uintptr_t)(get32(POLYGONS + 8) + 79000 * 0x7C), 2, 1000 * 0x7C);
        if (!custom) continue;
        /* Counted before it is there: the polygon would be written. */
        put32(POLYGONS + 4, (end - get32(POLYGONS + 8)) / 0x7C + 1);
        CHECK(call(sub_0051D820, POLYGONS, 1, get32(POLYGONS + 4)) == STOPPED && limit_stop("the scene's polygons") && guard_is_whole(end));
    }
    {
        uint32_t end;
        session(1, 0, 0);
        end = make_scene_array(POLYGON_POINTERS, 20000, 4);
        put32(POLYGON_POINTERS + 4, 80000);
        CHECK(call(sub_004D6820, POLYGON_POINTERS, 1, 80000u) == RAN);
        CHECK(call(sub_004D6820, POLYGON_POINTERS, 1, 90000u) == RAN && !g_message[0] && guard_is_whole(end));
        put32(POLYGON_POINTERS + 4, (end - get32(POLYGON_POINTERS + 8)) / 4 + 1);
        CHECK(call(sub_004D6820, POLYGON_POINTERS, 1, get32(POLYGON_POINTERS + 4)) == STOPPED && limit_stop("the scene's polygon pointers") && guard_is_whole(end));
        end = make_scene_array(VERTICES, 80000, 40);
        put32(VERTICES + 4, 320000);
        CHECK(call(sub_004B7390, VERTICES, 1, 320000u) == RAN);
        memset((void*)(uintptr_t)get32(VERTICES + 8), 1, 320000 * 40);
        put32(VERTICES + 4, (end - get32(VERTICES + 8)) / 40 + 1);
        CHECK(call(sub_004B7390, VERTICES, 1, get32(VERTICES + 4)) == STOPPED && limit_stop("the scene's vertices") && guard_is_whole(end));
    }
}

/* The campaign's heaps are as they were: no guard, requests kept inside, nothing ended. */
static void test_campaign(void) {
    uint32_t base;
    MEMORY_BASIC_INFORMATION mi;
    session(0, 0, 0);
    base = make_heap(996520u);
    CHECK(VirtualQuery((const void*)(uintptr_t)base, &mi, sizeof mi) && mi.RegionSize == ((996520u + PAGE - 1) & ~(PAGE - 1)) && !engine_heaps_guard(base + mi.RegionSize));
    CHECK(call(sub_0047E470, OBJECT, 1, 996520u) == RAN && call(sub_0047E470, OBJECT, 1, 0x10000u) == RAN && !g_message[0]);
    build();
    CHECK(VirtualQuery((const void*)(uintptr_t)get32(OBJECT + 0x18), &mi, sizeof mi) && mi.RegionSize == 2048 * 40);
}

/* A level that asks for nothing gets the engine's own sizes, and one that ends leaves nothing: without another install, what is
 * reserved next is the campaign's. */
static void test_session_end(void) {
    MEMORY_BASIC_INFORMATION mi;
    g_mesh_faces = 0;
    session(1, 0, 0);
    build();
    CHECK(VirtualQuery((const void*)(uintptr_t)get32(OBJECT + 0x18), &mi, sizeof mi) && guard_is_whole(get32(OBJECT + 0x18) + 2048 * 40));
    CHECK(add(&k_mesh[0], 2048) == RAN && add(&k_mesh[0], 2049) == STOPPED && limit_stop(k_mesh[0].name));
    g_mesh_faces = 8192;
    g_pool_mb = 168;
    session(1, 64, 10);
    CHECK(engine_heaps_region_bytes(0x8000000u) == 168u << 20 && g_session_end != NULL);
    CHECK(engine_heaps_buffer_bytes(0x100000u) == 0x400000u && engine_heaps_buffer_bytes(0x200000u) == 0x200000u);
    g_custom = 0;
    g_session_end();
    memset((void*)(uintptr_t)OBJECT, 0, 0x10000);
    CHECK(engine_heaps_region_bytes(0x8000000u) == 0x8000000u && engine_heaps_buffer_bytes(0x100000u) == 0x100000u);
    build();
    CHECK(VirtualQuery((const void*)(uintptr_t)get32(OBJECT + 0x18), &mi, sizeof mi) && mi.RegionSize == 2048 * 40 && !engine_heaps_guard(get32(OBJECT + 0x18) + 2048 * 40));
    memset((void*)(uintptr_t)OBJECT, 0, 0x10000);
    CHECK(VirtualQuery((const void*)(uintptr_t)make_heap(0x2000000u), &mi, sizeof mi) && mi.RegionSize == 0x2000000u);
    memset((void*)(uintptr_t)OBJECT, 0, 0x10000);
    CHECK(VirtualQuery((const void*)(uintptr_t)make_heap(996520u), &mi, sizeof mi) && mi.RegionSize == ((996520u + PAGE - 1) & ~(PAGE - 1)));
    g_pool_mb = 0;
}

/* ---- regions and memory ---- */

static void test_regions(void) {
    uint32_t pool;
    session(1, 0, 0);
    pool = (uint32_t)(uintptr_t)VirtualAlloc(NULL, 0x100000, MEM_RESERVE, PAGE_READWRITE);
    /* The texture pool grows by commits in its own region. */
    memset((void*)(uintptr_t)OBJECT, 0, 0x200);
    put32(OBJECT + 0x1C, pool + 0x80000);
    put32(OBJECT + 0x17C, 0x1000);
    CHECK(call(sub_005FA870, OBJECT, 1, 0x80000u) == RAN && (g_eax & 0xFF) == 1 && get32(OBJECT + 0x20) == 0x80000);
    memset((void*)(uintptr_t)(pool + 0x80000), 5, 0x80000);
    CHECK(call(sub_005FA870, OBJECT, 1, 0x80001u) == STOPPED && limit_stop("a region of the engine") && get32(OBJECT + 0x20) == 0x80000);
    CHECK(call(shim_VirtualAlloc, 0, 4, 0xFFFFF000u, 0x2000u, (uint32_t)MEM_COMMIT, (uint32_t)PAGE_READWRITE) == STOPPED && limit_stop("a region of the engine"));
    CHECK(call(shim_VirtualAlloc, 0, 4, pool, 0x1000u, (uint32_t)MEM_COMMIT, (uint32_t)PAGE_READWRITE) == RAN && g_eax == pool);
    CHECK(call(shim_VirtualAlloc, 0, 4, 0u, 0xF0000000u, (uint32_t)(MEM_RESERVE | MEM_COMMIT), (uint32_t)PAGE_READWRITE) == STOPPED && logged("ALLOCATION_FAILED: a reservation of the engine") && logged("CONTROLLED_LOAD_FAILURE"));
    /* Outside a custom level the game gets what Windows gives it. */
    g_custom = 0;
    CHECK(call(shim_VirtualAlloc, 0, 4, 0u, 0xF0000000u, (uint32_t)(MEM_RESERVE | MEM_COMMIT), (uint32_t)PAGE_READWRITE) == RAN && !g_eax && !g_log[0]);
    put32(OBJECT + 0x20, 0x80000);
    CHECK(call(sub_005FA870, OBJECT, 1, 0x80001u) == RAN);
    VirtualFree((void*)(uintptr_t)pool, 0, MEM_RELEASE);
}

static void test_pool(void) {
    MEMORY_BASIC_INFORMATION mi;
    for (unsigned mb = 0; mb <= 3900; mb += mb < 168 ? 168 : 3732) {
        const uint32_t reserved = mb == 168 ? 168u << 20 : 128u << 20;
        g_pool_mb = mb;
        session(1, 0, 0);
        CHECK(call(shim_VirtualAlloc, 0, 4, 0u, 0x8000000u, (uint32_t)MEM_RESERVE, (uint32_t)PAGE_READWRITE) == RAN && made(g_eax));
        CHECK(VirtualQuery((const void*)(uintptr_t)g_made[g_made_count - 1], &mi, sizeof mi) && mi.RegionSize == reserved && logged("no address space") == (mb == 3900));
        CHECK(call(shim_VirtualAlloc, 0, 4, g_made[g_made_count - 1] + reserved - 0x10000, 0x10000u, (uint32_t)MEM_COMMIT, (uint32_t)PAGE_READWRITE) == RAN && g_eax);
        CHECK(call(shim_VirtualAlloc, 0, 4, g_made[g_made_count - 1] + reserved, 0x10000u, (uint32_t)MEM_COMMIT, (uint32_t)PAGE_READWRITE) == STOPPED && limit_stop("a region of the engine"));
    }
    g_pool_mb = 168;
    session(0, 0, 0);
    CHECK(call(shim_VirtualAlloc, 0, 4, 0u, 0x8000000u, (uint32_t)MEM_RESERVE, (uint32_t)PAGE_READWRITE) == RAN && made(g_eax) && VirtualQuery((const void*)(uintptr_t)g_eax, &mi, sizeof mi) && mi.RegionSize == 128u << 20);
    g_pool_mb = 0;
}

static void test_reservations(void) {
    session(1, 0, 0);
    CHECK(call(sub_005F8A20, 0, 1, 0xE0000000u) == STOPPED && logged("ALLOCATION_FAILED: a heap of the engine") && logged("CONTROLLED_LOAD_FAILURE"));
    CHECK(call(sub_005F8A20, 0, 1, 0x10000u) == RAN && made(g_eax));
    /* More than the address space has: the engine's own size instead, and the session goes on. */
    session(1, 3900, 0);
    CHECK(call(sub_005F8A20, 0, 1, 0x2000000u) == RAN && made(g_eax) && logged("no address space") && !g_message[0] && guard_is_whole(g_eax + 0x2000000u));
}

static void test_new(void) {
    session(1, 0, 0);
    CHECK(call(shim_operator_new, 0, 1, 0x88u) == RAN && g_eax);
    memset((void*)(uintptr_t)g_eax, 1, 0x88);
    CHECK(call(shim_operator_new, 0, 1, 0xF0000000u) == STOPPED && logged("ALLOCATION_FAILED: operator new, 4026531840 bytes") && logged("CONTROLLED_LOAD_FAILURE"));
    CHECK(call(shim_malloc, 0, 1, 0xF0000000u) == STOPPED && logged("ALLOCATION_FAILED: malloc"));
    CHECK(call(shim_operator_new, 0, 1, 0u) == RAN && g_eax);
    CHECK(call(shim_set_new_handler, 0, 1, 0x005F5840u) == RAN && !g_eax && call(shim__set_new_mode, 0, 1, 1u) == RAN && !g_eax);
    CHECK(call(shim_operator_new, 0, 1, 0xF0000000u) == STOPPED);
    g_custom = 0;
    CHECK(call(shim_malloc, 0, 1, 0xF0000000u) == RAN && !g_eax && call(shim_operator_new, 0, 1, 0x14u) == RAN && g_eax);
}

static SIZE_T committed_now(void) {
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof counters);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof counters);
    return counters.PrivateUsage;
}

/* The process is kept from committing more: real failures, at any point of a build. */
static void test_out_of_memory(void) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
    const HANDLE job = CreateJobObjectA(NULL, NULL);
    unsigned stopped = 0;
    if (!job || !AssignProcessToJobObject(job, GetCurrentProcess())) {
        printf("note: no job for the out of memory test\n");
        return;
    }
    session(1, 0, 0);
    make_heap(0x1000000u);
    memset(&limit, 0, sizeof limit);
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = committed_now() + (3u << 20);
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit));
    for (int i = 0; i < 64 && !stopped; i++) {
        const uint32_t before = get32(OBJECT + 0x10);
        if (call(sub_0047E470, OBJECT, 1, 0x40000u) == STOPPED) {
            stopped = logged("ALLOCATION_FAILED: a commit of the engine") && logged("CONTROLLED_LOAD_FAILURE") && get32(OBJECT + 0x10) == before;
            CHECK(stopped);
        }
    }
    CHECK(stopped);
    CHECK(call(shim_operator_new, 0, 1, 64u << 20) == STOPPED && logged("ALLOCATION_FAILED: operator new"));
    limit.BasicLimitInformation.LimitFlags = 0;
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit));
    session(1, 0, 0);
    test_heap(0x1000000u, 0x1000000u, "a 16 MB heap of objects");
    session(0, 0, 0);
    make_heap(0x1000000u);
    CHECK(call(sub_0047E470, OBJECT, 1, 0x100000u) == RAN);
    CloseHandle(job);
}

/* The last defence, reached on purpose: a write that nobody asked the room for. */
static void test_guard(void) {
    uint32_t base, end;
    unsigned before;
    session(1, 0, 0);
    base = make_heap(996520u);
    end = base + ((996520u + PAGE - 1) & ~(PAGE - 1));
    CHECK(call(sub_0047E470, OBJECT, 1, end - base) == RAN && guard_is_whole(end));
    before = g_faults;
    __try {
        *(volatile uint8_t*)(uintptr_t)end = 1;
    } __except (count_fault(GetExceptionCode())) {
        CHECK(engine_heaps_guard(end));
    }
    CHECK(g_faults == before + 1);
    g_faults = before;
}

static void test_overflow_rule(void) {
    CHECK(engine_heap_overflow(0x1000, 0, 0x2000) == 0 && engine_heap_overflow(0x1000, 0x1000, 0x2000) == 0 && engine_heap_overflow(0x1000, 0x1001, 0x2000) == 1);
    CHECK(engine_heap_overflow(0x2000, 1, 0x2000) == 1 && engine_heap_overflow(0x3000, 5, 0x2000) == 5 && engine_heap_overflow(0x1000, 0xFFFFFFFFu, 0x2000) == 0xFFFFEFFFu && engine_heap_overflow(0xFFFFF000u, 0xFFFFFFFFu, 0xFFFFF000u) == 0xFFFFFFFFu);
}

static void test_terrain_cull(void) {
    const uint32_t kept = jpt_gva[GVAI_0x00664D50];
    float thickness = -0.05f;
    jpt_gva[GVAI_0x00664D50] = (uint32_t)(uintptr_t)&thickness;
    session(1, 0, 0);
    CHECK(thickness == -0.05f);
    g_cull_mm = 200;
    session(0, 0, 0);
    CHECK(thickness == -0.05f);
    session(1, 0, 0);
    CHECK(thickness == -0.05f - 0.2f);
    session(1, 0, 0);
    CHECK(thickness == -0.05f - 0.2f);
    g_cull_mm = 0;
    g_session_end();
    CHECK(thickness == -0.05f);
    /* Another build's value stays. */
    thickness = -0.07f, g_cull_mm = 100;
    session(1, 0, 0);
    g_session_end();
    CHECK(thickness == -0.07f);
    g_cull_mm = 0;
    jpt_gva[GVAI_0x00664D50] = kept;
}

int main(void) {
    const uintptr_t lo = (uintptr_t)g_window;
    setvbuf(stdout, NULL, _IONBF, 0);
    test_profile_activate();
    if (lo > 0x00645000u || lo + sizeof g_window < WINDOW_HI) {
        printf("the guest window 0x%08X does not cover the guest's data: link at /BASE:0x00400000 /FIXED\n", (unsigned)lo);
        return 2;
    }
    LoadLibraryA("msvcrt.dll");
    put32(IAT_VIRTUAL_ALLOC, FAKE_VIRTUAL_ALLOC);
    put32(PAGE_SIZE_VA, PAGE);
    test_overflow_rule();
    test_mesh_arrays();
    test_mesh_halfway();
    test_heaps();
    test_scene();
    test_campaign();
    test_session_end();
    test_regions();
    test_pool();
    test_reservations();
    test_new();
    test_out_of_memory();
    test_terrain_cull();
    CHECK(g_faults == 0);
    test_guard();
    printf("engine_heaps_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
