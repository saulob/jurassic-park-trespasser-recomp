/*
 * The game's Direct3D texture uploads. Each frame UploadPolygonArray links the textures of newly visible polygons to
 * Direct3D textures within a time window, measured with the performance counter instead of GetTickCount's 15.6 ms
 * steps.
 */
#include "guest_va.h"
#include "host.h"
#include "perf_sections.h"

#define UPLOAD_POLYGONS_VA GVA(0x0055A4E0) /* CScreenRenderAuxD3D::UploadPolygonArray(n, polygons) */

static recomp_func_t s_upload_lifted;
static int s_installed;
static struct {
    volatile int active;
    int started, ran_out;
    DWORD thread;
    uint32_t tick;
    double budget_ms;
    LARGE_INTEGER start;
} s_clock;
static struct {
    unsigned long long windows, ran_out, longer;
    double max_ms;
} s_stats;
static double s_qpc_ms;

static double elapsed_ms(const LARGE_INTEGER* since) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - since->QuadPart) * s_qpc_ms;
}

void shim_GetTickCount(void) {
    DWORD now = GetTickCount();
    if (s_clock.active && GetCurrentThreadId() == s_clock.thread) {
        double spent = elapsed_ms(&s_clock.start);
        if (!s_clock.started) {
            s_clock.started = 1;
            s_clock.tick = now;
        }
        if (spent > s_clock.budget_ms) s_clock.ran_out = 1;
        now = engine_upload_clock(s_clock.tick, spent, s_clock.budget_ms);
    }
    g_eax = now;
    GUEST_RETURN_STDCALL(0);
}

static void upload_polygons(void) {
    LARGE_INTEGER start;
    double ms;
    int previous_ran_out = s_clock.ran_out;
    if (g_alias_entry) {
        s_upload_lifted();
        return;
    }
    QueryPerformanceCounter(&start);
    s_clock.thread = GetCurrentThreadId();
    s_clock.start = start;
    s_clock.started = 0;
    s_clock.ran_out = 0;
    s_clock.budget_ms = engine_upload_budget_ms(previous_ran_out);
    s_clock.active = engine_limits_modern();
    s_upload_lifted();
    s_clock.active = 0;
    ms = elapsed_ms(&start);
    perf_section_end(PS_TEXTURE_UPLOADS, start.QuadPart);
    s_stats.windows++;
    s_stats.ran_out += (unsigned long long)s_clock.ran_out;
    s_stats.longer += (unsigned long long)previous_ran_out;
    if (ms > s_stats.max_ms) s_stats.max_ms = ms;
}

void texture_streaming_install(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;
    s_installed = guest_override_install(UPLOAD_POLYGONS_VA, upload_polygons, &s_upload_lifted);
}

void texture_streaming_log(void) {
    if (s_installed && engine_limits_modern())
        host_log("graphics", "  texture uploads: up to %.0f ms per frame measured with the performance counter, %.0f ms "
                 "right after a frame that left textures for later (the game's %u ms in GetTickCount steps of 15.6 ms)",
                 ENGINE_UPLOAD_BUDGET_MS, ENGINE_UPLOAD_BACKLOG_MS, ENGINE_UPLOAD_MS_ORIGINAL);
}

void texture_streaming_summary(void) {
    if (!s_stats.windows) return;
    host_log("perf-detail", "upload windows: %llu, ran out %llu, given the longer window %llu, longest "
             "%.2f ms", s_stats.windows, s_stats.ran_out, s_stats.longer, s_stats.max_ms);
}
