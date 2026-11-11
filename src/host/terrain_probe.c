/*
 * DEBUG_SAULO: guest-side terrain counters and timings. With incremental linking every lifted
 * function is reached through a 5-byte "jmp rel32" thunk, from direct calls and from the dispatch
 * table alike; pointing a thunk at a wrapper counts and times every call without regenerating the
 * lifted code. A sampler thread reads g_cur_func at 1 kHz for self time per lifted function.
 */
#include "host.h"

#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define SAMPLE_SLOTS 4096
#define SAMPLE_TOP 12

enum { TIMED, COUNTED };

enum {
    P_RENDER_SCENE,
    P_TERRAIN_SHAPE,
    P_NODE_BRANCH,
    P_TRI_TEST,
    P_OCCLUSION,
    P_TEX_FETCH,
    P_TEX_LOD,
    P_TEX_SCHEDULE,
    P_TEX_STATIC,
    P_TEX_SOLID,
    P_CLIP,
    P_TEX_UPDATE,
    P_TEX_ALLOC,
    P_TEX_INIT,
    P_TEX_RENDER,
    P_TEX_LIGHT,
    P_TEX_PIPELINE,
    P_RASTER_LOCK,
    P_RASTER_UNLOCK,
    P_UPLOAD_LOAD,
    P_UPLOAD_BLT,
    P_TERRAIN_SUBMIT,
    P_TERRAIN_DRAW,
    P_SHAPES,
    P_POLYGONS,
    P_SCHEDULER,
    P_TERRAIN_ITEM,
    P_TERRAIN_SCHEDULE,
    P_FLUSH_BATCH,
    P_WATER_UPDATE,
    P_WATER_ROWS,
    P_WAVE_STEP,
    P_BOUND_VOLUME,
    P_COUNT
};

typedef struct {
    uint32_t va;
    const char* name;
    int kind;
    int terrain_only;
    recomp_func_t body;
    int depth;
    unsigned long long start;
    unsigned long long calls, ticks, max_ticks, inline_calls;
} probe_t;

/* Names follow the retail RTTI where it exists and OpenTrespasser otherwise; "?" marks a role read from
 * the call structure only. */
static probe_t s_probes[P_COUNT] = {
    [P_RENDER_SCENE] = { 0x0051A630u, "render scene (terrain, objects, terrain schedule)", TIMED, 0 },
    [P_TERRAIN_SHAPE] = { 0x00519570u, "CQuadRootTINShape render: terrain polygons", TIMED, 0 },
    [P_NODE_BRANCH] = { 0x004A1E20u, "quad node triangle-list branch ?", COUNTED, 1 },
    [P_TRI_TEST] = { 0x0048F6F0u, "terrain triangle iterated", COUNTED, 1 },
    [P_OCCLUSION] = { 0x00630D90u, "occluder test", TIMED, 1 },
    [P_TEX_FETCH] = { 0x0049FB30u, "CPolyIterator texture: visible terrain triangles", TIMED, 0 },
    [P_TEX_LOD] = { 0x0049BD80u, "CQuadNodeTIN::EvaluateTexture ?", TIMED, 0 },
    [P_TEX_SCHEDULE] = { 0x00492730u, "CTextureNode::ScheduleUpdate ?", TIMED, 0 },
    [P_TEX_STATIC] = { 0x00492C90u, "CTextureNode static texture ?", TIMED, 0 },
    [P_TEX_SOLID] = { 0x00492F10u, "CTextureNode solid colour ?", TIMED, 0 },
    [P_CLIP] = { 0x0053B670u, "CClipPlane clip", TIMED, 1 },
    [P_TEX_UPDATE] = { 0x004933E0u, "CTextureNode texture update (DoTextureUpdate)", TIMED, 0 },
    [P_TEX_ALLOC] = { 0x004934D0u, "texture region allocation/eviction ?", TIMED, 0 },
    [P_TEX_INIT] = { 0x00493960u, "texture init from parent/base ?", TIMED, 0 },
    [P_TEX_RENDER] = { 0x00493C30u, "texture render (terrain objects into the page)", TIMED, 0 },
    [P_TEX_LIGHT] = { 0x00494AB0u, "texture lighting/shadows ?", TIMED, 0 },
    [P_TEX_PIPELINE] = { 0x0051AAE0u, "render pipeline used by texture render", TIMED, 0 },
    [P_RASTER_LOCK] = { 0x005101B0u, "CRasterD3D lock (system-memory page)", TIMED, 0 },
    [P_RASTER_UNLOCK] = { 0x00510210u, "CRasterD3D unlock", TIMED, 0 },
    [P_UPLOAD_LOAD] = { 0x00510240u, "CRasterD3D upload: IDirect3DTexture2::Load", TIMED, 0 },
    [P_UPLOAD_BLT] = { 0x00510320u, "CRasterD3D upload: region Blt", TIMED, 0 },
    [P_TERRAIN_SUBMIT] = { 0x0051B2C0u, "terrain submit", TIMED, 0 },
    [P_TERRAIN_DRAW] = { 0x0055D8B0u, "RasterizeTerrainBatch: SetTexture/DrawPrimitive", TIMED, 0 },
    [P_SHAPES] = { 0x00519B20u, "object shapes into polygons", TIMED, 0 },
    [P_POLYGONS] = { 0x0051B0A0u, "polygon draw (D3D and software)", TIMED, 0 },
    [P_SCHEDULER] = { 0x005F9CE0u, "CScheduler::Execute", TIMED, 0 },
    [P_TERRAIN_ITEM] = { 0x00492C80u, "CScheduleTerrainTextureItem::Execute", TIMED, 0 },
    [P_TERRAIN_SCHEDULE] = { 0x0051B2A0u, "ExecuteScheduleForTerrain", TIMED, 0 },
    [P_FLUSH_BATCH] = { 0x00555AB0u, "CScreenRenderAuxD3D::FlushBatch", TIMED, 0 },
    [P_WATER_UPDATE] = { 0x0060E590u, "CEntityWater texture update (FillWaterTexture)", TIMED, 0 },
    [P_WATER_ROWS] = { 0x0060E3F0u, "water texture rows from the wave heights", TIMED, 0 },
    [P_WAVE_STEP] = { 0x00638670u, "CSimWaveVar2D wave integration", TIMED, 0 },
    [P_BOUND_VOLUME] = { 0x004A7B10u, "CBoundVolPolyhedron test ?", TIMED, 0 },
};

static DWORD s_thread;
static int s_installed;
static LARGE_INTEGER s_qpc0;
static unsigned long long s_tsc0;
static unsigned long long s_unbalanced;

typedef struct {
    uint32_t va;
    unsigned long samples, in_scene, in_terrain, in_texture;
} sample_slot_t;

static CRITICAL_SECTION s_sample_lock;
static sample_slot_t s_slots[SAMPLE_SLOTS];
static unsigned long s_samples, s_samples_scene, s_samples_terrain, s_samples_texture;

static int probe_enter(int i) {
    probe_t* p = &s_probes[i];
    DWORD me = GetCurrentThreadId();
    if (!s_thread) {
        if (i != P_RENDER_SCENE) return 0;
        s_thread = me;
    }
    if (me != s_thread) return 0;
    if (p->terrain_only && !s_probes[P_TERRAIN_SHAPE].depth) return 0;
    p->calls++;
    if (i == P_TEX_UPDATE && !s_probes[P_TERRAIN_ITEM].depth) p->inline_calls++;
    if (i == P_WATER_UPDATE) tdiag_note_water_node(g_ecx);
    if (p->kind == COUNTED) return 0;
    if (p->depth++ == 0) p->start = __rdtsc();
    return 1;
}

static void probe_leave(int i) {
    probe_t* p = &s_probes[i];
    if (--p->depth == 0) {
        unsigned long long t = __rdtsc() - p->start;
        p->ticks += t;
        if (t > p->max_ticks) p->max_ticks = t;
    }
}

#define WRAPPER(i)                                                                                                     \
    static void wrap_##i(void) {                                                                                       \
        int on = probe_enter(i);                                                                                       \
        s_probes[i].body();                                                                                            \
        if (on) probe_leave(i);                                                                                        \
    }
WRAPPER(0) WRAPPER(1) WRAPPER(2) WRAPPER(3) WRAPPER(4) WRAPPER(5) WRAPPER(6) WRAPPER(7) WRAPPER(8) WRAPPER(9)
WRAPPER(10) WRAPPER(11) WRAPPER(12) WRAPPER(13) WRAPPER(14) WRAPPER(15) WRAPPER(16) WRAPPER(17) WRAPPER(18)
WRAPPER(19) WRAPPER(20) WRAPPER(21) WRAPPER(22) WRAPPER(23) WRAPPER(24) WRAPPER(25) WRAPPER(26) WRAPPER(27)
WRAPPER(28) WRAPPER(29) WRAPPER(30) WRAPPER(31) WRAPPER(32)

static const recomp_func_t s_wrappers[] = { wrap_0,  wrap_1,  wrap_2,  wrap_3,  wrap_4,  wrap_5,  wrap_6,  wrap_7,
                                            wrap_8,  wrap_9,  wrap_10, wrap_11, wrap_12, wrap_13, wrap_14, wrap_15,
                                            wrap_16, wrap_17, wrap_18, wrap_19, wrap_20, wrap_21, wrap_22, wrap_23,
                                            wrap_24, wrap_25, wrap_26, wrap_27, wrap_28, wrap_29, wrap_30, wrap_31,
                                            wrap_32 };
C_ASSERT(sizeof s_wrappers / sizeof s_wrappers[0] == P_COUNT);

/* The address a function pointer gives under /INCREMENTAL is its thunk: E9 rel32 to the body. */
static int redirect_thunk(recomp_func_t thunk, recomp_func_t wrapper, recomp_func_t* body) {
    BYTE* p = (BYTE*)thunk;
    DWORD old;
    int32_t rel;
    if (!p || p[0] != 0xE9) return 0;
    memcpy(&rel, p + 1, 4);
    *body = (recomp_func_t)(p + 5 + rel);
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    rel = (int32_t)((BYTE*)wrapper - (p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static void sample_once(void) {
    uint32_t va = *(volatile uint32_t*)&g_cur_func;
    int scene = s_probes[P_RENDER_SCENE].depth > 0, terrain = s_probes[P_TERRAIN_SHAPE].depth > 0;
    int texture = s_probes[P_TEX_UPDATE].depth > 0;
    unsigned h = (va * 2654435761u) >> 20;
    EnterCriticalSection(&s_sample_lock);
    for (unsigned n = 0; n < SAMPLE_SLOTS; n++, h = (h + 1) & (SAMPLE_SLOTS - 1)) {
        sample_slot_t* s = &s_slots[h];
        if (s->va != va && s->samples) continue;
        s->va = va;
        s->samples++;
        s->in_scene += scene;
        s->in_terrain += terrain;
        s->in_texture += texture;
        break;
    }
    s_samples++;
    s_samples_scene += scene;
    s_samples_terrain += terrain;
    s_samples_texture += texture;
    LeaveCriticalSection(&s_sample_lock);
}

static DWORD WINAPI sampler_main(void* arg) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    (void)arg;
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!timer) return 0;
    due.QuadPart = -10000;
    if (!SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) return 0;
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        if (s_thread) sample_once();
    }
}

void terrain_probe_install(void) {
    unsigned hooked = 0;
    HANDLE thread;
    QueryPerformanceCounter(&s_qpc0);
    s_tsc0 = __rdtsc();
    for (int i = 0; i < P_COUNT; i++) {
        recomp_func_t thunk = recomp_lookup(s_probes[i].va);
        if (redirect_thunk(thunk, s_wrappers[i], &s_probes[i].body)) hooked++;
        else host_log("terrain", "probe for sub_%08X (%s) not installed: no incremental-link thunk", s_probes[i].va,
                      s_probes[i].name);
    }
    InitializeCriticalSection(&s_sample_lock);
    thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
        CloseHandle(thread);
    }
    s_installed = 1;
    host_log("terrain", "DEBUG_SAULO terrain probes: %u of %d guest functions counted and timed through their thunks; "
             "self-time sampler at 1 kHz %s", hooked, P_COUNT, thread ? "running" : "not started");
}

static double tsc_ms(void) {
    LARGE_INTEGER now, freq;
    unsigned long long tsc = __rdtsc() - s_tsc0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!tsc) return 0.0;
    return (double)(now.QuadPart - s_qpc0.QuadPart) * 1000.0 / (double)freq.QuadPart / (double)tsc;
}

static void report_counts(double frames) {
    const probe_t* p = s_probes;
    double considered = p[P_TRI_TEST].calls / frames, visible = p[P_TEX_FETCH].calls / frames;
    host_log("terrain", "last 5 s per frame: quad-node branches %.1f, terrain triangles iterated %.1f, culled before the "
             "texture lookup (back-face, frustum outcodes, occluders) %.1f, visible %.1f, clipped %.1f, occluder tests "
             "%.1f", p[P_NODE_BRANCH].calls / frames, considered, considered > visible ? considered - visible : 0.0,
             visible, p[P_CLIP].calls / frames, p[P_OCCLUSION].calls / frames);
    host_log("terrain", "last 5 s per frame: textures evaluated %.1f, schedule requests %.1f, static %.1f, solid %.1f; "
             "texture updates %.2f (inline in the iterator %.2f, scheduler %.2f), renders %.2f, region allocations %.2f, "
             "page locks %.2f, uploads Load %.2f / Blt %.2f", p[P_TEX_LOD].calls / frames, p[P_TEX_SCHEDULE].calls / frames,
             p[P_TEX_STATIC].calls / frames, p[P_TEX_SOLID].calls / frames, p[P_TEX_UPDATE].calls / frames,
             p[P_TEX_UPDATE].inline_calls / frames, (p[P_TEX_UPDATE].calls - p[P_TEX_UPDATE].inline_calls) / frames,
             p[P_TEX_RENDER].calls / frames, p[P_TEX_ALLOC].calls / frames, p[P_RASTER_LOCK].calls / frames,
             p[P_UPLOAD_LOAD].calls / frames, p[P_UPLOAD_BLT].calls / frames);
}

static void report_times(double frames, double ms_per_tick) {
    host_log("terrain", "last 5 s guest time per frame (calls, inclusive ms, slowest call ms):");
    for (int i = 0; i < P_COUNT; i++) {
        const probe_t* p = &s_probes[i];
        if (p->kind != TIMED || !p->calls) continue;
        host_log("terrain", "  sub_%08X %-50s %8.1f %8.2f %8.2f", p->va, p->name, p->calls / frames,
                 p->ticks * ms_per_tick / frames, p->max_ticks * ms_per_tick);
    }
}

static int by_samples(const void* a, const void* b) {
    unsigned long x = ((const sample_slot_t*)a)->samples, y = ((const sample_slot_t*)b)->samples;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report_samples(void) {
    static sample_slot_t copy[SAMPLE_SLOTS];
    unsigned long total, scene, terrain, texture;
    EnterCriticalSection(&s_sample_lock);
    memcpy(copy, s_slots, sizeof copy);
    total = s_samples;
    scene = s_samples_scene;
    terrain = s_samples_terrain;
    texture = s_samples_texture;
    memset(s_slots, 0, sizeof s_slots);
    s_samples = s_samples_scene = s_samples_terrain = s_samples_texture = 0;
    LeaveCriticalSection(&s_sample_lock);
    if (!total) return;
    qsort(copy, SAMPLE_SLOTS, sizeof copy[0], by_samples);
    host_log("profile", "last 5 s: %lu samples of the executing lifted function (any guest thread); inside render scene "
             "%.1f%%, terrain polygon generation %.1f%%, terrain texture updates %.1f%%; top self time:", total,
             100.0 * scene / total, 100.0 * terrain / total, 100.0 * texture / total);
    for (int i = 0; i < SAMPLE_TOP && copy[i].samples; i++)
        host_log("profile", "  sub_%08X %5.1f%% (in render scene %3.0f%%, terrain polygons %3.0f%%, texture updates %3.0f%%)",
                 copy[i].va, 100.0 * copy[i].samples / total, 100.0 * copy[i].in_scene / copy[i].samples,
                 100.0 * copy[i].in_terrain / copy[i].samples, 100.0 * copy[i].in_texture / copy[i].samples);
}

void terrain_probe_report(double frames) {
    if (!s_installed) return;
    if (frames <= 0.0) frames = 1.0;
    if (!s_probes[P_RENDER_SCENE].depth) {
        for (int i = 0; i < P_COUNT; i++)
            if (s_probes[i].depth) {
                s_unbalanced++;
                s_probes[i].depth = 0;
            }
    }
    if (s_probes[P_RENDER_SCENE].calls) {
        report_counts(frames);
        report_times(frames, tsc_ms());
        if (s_unbalanced) host_log("terrain", "probes left by a guest exception and reset: %llu", s_unbalanced);
    }
    report_samples();
    for (int i = 0; i < P_COUNT; i++) {
        s_probes[i].calls = s_probes[i].ticks = s_probes[i].max_ticks = s_probes[i].inline_calls = 0;
    }
}
