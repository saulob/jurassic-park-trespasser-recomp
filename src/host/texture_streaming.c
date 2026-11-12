/*
 * The game's Direct3D texture uploads. Each frame UploadPolygonArray links the textures of newly visible polygons to
 * Direct3D textures within a time window, measured with the performance counter instead of GetTickCount's 15.6 ms
 * steps.
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

#define UPLOAD_POLYGONS_VA 0x0055A4E0u /* CScreenRenderAuxD3D::UploadPolygonArray(n, polygons) */
#define AGP_CREATE_VA 0x00515240u      /* CAGPTextureMemManager::prasCreate */
#define AGP_ADD_VA 0x00515290u         /* bAdd: uploads one raster, AL = success */
#define AGP_EVICT_VA 0x00515410u       /* bReducedUnused(n) */
#define REQUEST_PAGES_VA 0x0053AF80u   /* CVirtualMem::RequestMemory: a mip not yet read from the swap file */
#define AGP_MANAGER 0x006D2040u        /* +4 bytes of every raster held, +8 allocated, +0x10 eviction mark */
#define AGP_ACTIVE_COUNT 0x006D205Cu
#define AGP_UNUSED_COUNT 0x006D209Cu
#define POLY_TEXTURE 0x0Cu
#define POLY_MIP 0x30u
#define SEEN_SLOTS 65536u

static recomp_func_t s_upload_lifted, s_create_lifted, s_add_lifted, s_evict_lifted, s_request_lifted;
static int s_installed;
static struct {
    volatile int active;
    int started;
    DWORD thread;
    uint32_t tick;
    LARGE_INTEGER start;
} s_clock;
static double s_qpc_ms;

/* DEBUG_SAULO: what the uploads did, per 5 s window and for the session. */
typedef struct {
    unsigned long long frames, uploaded, reloads, created, failed, evicted, requests;
} upload_stats_t;
static upload_stats_t s_window, s_total;
static uint32_t* s_seen;

static double elapsed_ms(const LARGE_INTEGER* since) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - since->QuadPart) * s_qpc_ms;
}

void shim_GetTickCount(void) {
    DWORD now = GetTickCount();
    if (s_clock.active && GetCurrentThreadId() == s_clock.thread) {
        if (!s_clock.started) {
            s_clock.started = 1;
            s_clock.tick = now;
        }
        now = engine_upload_clock(s_clock.tick, elapsed_ms(&s_clock.start), ENGINE_UPLOAD_BUDGET_MS);
    }
    g_eax = now;
    GUEST_RETURN_STDCALL(0);
}

/* DEBUG_SAULO: a texture mip uploaded earlier in the session and uploaded again was evicted in between. */
static int seen_before(uint32_t key) {
    uint32_t i = (key * 2654435761u) & (SEEN_SLOTS - 1u);
    if (!s_seen && !(s_seen = (uint32_t*)calloc(SEEN_SLOTS, sizeof(uint32_t)))) return 0;
    for (unsigned probe = 0; probe < 64; probe++, i = (i + 1u) & (SEEN_SLOTS - 1u)) {
        if (s_seen[i] == key) return 1;
        if (!s_seen[i]) {
            s_seen[i] = key;
            return 0;
        }
    }
    return 0;
}

static void count_uploaded(uint32_t polygons, uint32_t uploaded) {
    uint32_t frame_keys[64];
    unsigned keys = 0;
    for (uint32_t j = 0; j < uploaded; j++) {
        uint32_t poly = MEM32(polygons + 4u * j), key = (MEM32(poly + POLY_TEXTURE) << 3) ^ (MEM32(poly + POLY_MIP) & 7u);
        unsigned k = 0;
        while (k < keys && frame_keys[k] != key) k++;
        if (k < keys) continue;
        if (keys < 64) frame_keys[keys++] = key;
        if (seen_before(key)) s_window.reloads++;
    }
}

static void upload_polygons(void) {
    uint32_t n, polygons;
    LARGE_INTEGER start;
    if (g_alias_entry) {
        s_upload_lifted();
        return;
    }
    n = GUEST_ARG(0);
    polygons = GUEST_ARG(1);
    QueryPerformanceCounter(&start);
    s_clock.thread = GetCurrentThreadId();
    s_clock.start = start;
    s_clock.started = 0;
    s_clock.active = engine_limits_modern();
    s_upload_lifted();
    s_clock.active = 0;
    if (!n) return;
    s_window.frames++;
    s_window.uploaded += n;
    count_uploaded(polygons, n);
}

static void agp_create(void) {
    int alias = g_alias_entry != 0;
    s_create_lifted();
    if (!alias) s_window.created++;
}

static void agp_add(void) {
    int alias = g_alias_entry != 0;
    s_add_lifted();
    if (!alias && !(g_eax & 0xFFu)) s_window.failed++;
}

static void agp_evict(void) {
    int alias = g_alias_entry != 0;
    uint32_t before = MEM32(AGP_UNUSED_COUNT);
    s_evict_lifted();
    if (!alias && MEM32(AGP_UNUSED_COUNT) < before) s_window.evicted += before - MEM32(AGP_UNUSED_COUNT);
}

static void request_pages(void) {
    int alias = g_alias_entry != 0;
    s_request_lifted();
    if (!alias) s_window.requests++;
}

void texture_streaming_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { AGP_CREATE_VA, agp_create, &s_create_lifted },
        { AGP_ADD_VA, agp_add, &s_add_lifted },
        { AGP_EVICT_VA, agp_evict, &s_evict_lifted },
        { REQUEST_PAGES_VA, request_pages, &s_request_lifted },
    };
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;
    if (!guest_override_redirect(UPLOAD_POLYGONS_VA, upload_polygons, &s_upload_lifted)) {
        host_log("graphics", "sub_%08X (texture uploads) has no incremental-link thunk: the game's %u ms GetTickCount "
                 "window stays", UPLOAD_POLYGONS_VA, ENGINE_UPLOAD_MS_ORIGINAL);
        return;
    }
    s_installed = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!guest_override_redirect(k[i].va, k[i].replacement, k[i].lifted))
            host_log("graphics", "sub_%08X has no incremental-link thunk: texture upload counts are incomplete", k[i].va);
}

static void add_stats(upload_stats_t* total, const upload_stats_t* w) {
    total->frames += w->frames;
    total->uploaded += w->uploaded;
    total->reloads += w->reloads;
    total->created += w->created;
    total->failed += w->failed;
    total->evicted += w->evicted;
    total->requests += w->requests;
}

static void log_stats(const upload_stats_t* s, const char* when) {
    host_log("textures", "%s: game uploads %llu in %llu passes (%llu of them uploaded before and evicted since), "
             "Direct3D rasters created %llu, uploads failed %llu, evicted %llu, mips read from the swap file %llu",
             when, s->uploaded, s->frames, s->reloads, s->created, s->failed, s->evicted, s->requests);
}

void texture_streaming_summary(unsigned long long frames) {
    if (!s_installed) return;
    add_stats(&s_total, &s_window);
    memset(&s_window, 0, sizeof s_window);
    log_stats(&s_total, "session");
}

void texture_streaming_log(void) {
    if (s_installed && engine_limits_modern())
        host_log("graphics", "  texture uploads: up to %.0f ms per frame measured with the performance counter (the "
                 "game's %u ms in GetTickCount steps of 15.6 ms)", ENGINE_UPLOAD_BUDGET_MS, ENGINE_UPLOAD_MS_ORIGINAL);
}
