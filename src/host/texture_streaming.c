/*
 * The game's Direct3D texture uploads. Each frame UploadPolygonArray links the textures of newly visible polygons to
 * Direct3D textures within a time window, measured with the performance counter instead of GetTickCount's 15.6 ms
 * steps.
 */
#include "host.h"

#define UPLOAD_POLYGONS_VA 0x0055A4E0u /* CScreenRenderAuxD3D::UploadPolygonArray(n, polygons) */

static recomp_func_t s_upload_lifted;
static int s_installed;
static struct {
    volatile int active;
    int started;
    DWORD thread;
    uint32_t tick;
    LARGE_INTEGER start;
} s_clock;
static double s_qpc_ms;

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

static void upload_polygons(void) {
    LARGE_INTEGER start;
    if (g_alias_entry) {
        s_upload_lifted();
        return;
    }
    QueryPerformanceCounter(&start);
    s_clock.thread = GetCurrentThreadId();
    s_clock.start = start;
    s_clock.started = 0;
    s_clock.active = engine_limits_modern();
    s_upload_lifted();
    s_clock.active = 0;
}

void texture_streaming_install(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;
    s_installed = guest_override_install(UPLOAD_POLYGONS_VA, upload_polygons, &s_upload_lifted);
}

void texture_streaming_log(void) {
    if (s_installed && engine_limits_modern())
        host_log("graphics", "  texture uploads: up to %.0f ms per frame measured with the performance counter (the "
                 "game's %u ms in GetTickCount steps of 15.6 ms)", ENGINE_UPLOAD_BUDGET_MS, ENGINE_UPLOAD_MS_ORIGINAL);
}
