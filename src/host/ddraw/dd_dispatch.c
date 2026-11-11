/*
 * The game takes DirectDrawCreate from GetProcAddress and DirectDrawEnumerateA from its import
 * table; both hold ddraw.dll's own addresses. Every call through either reaches the indirect-call
 * dispatch, which sends it here instead, so no DirectDraw object of the system's is ever created.
 */
#include "d3d_legacy.h"
#include "host.h"

typedef struct {
    const char* name;
    uint32_t system;
    void* compat;
} export_route_t;

static export_route_t s_routes[] = {
    { "DirectDrawCreate", 0, (void*)compat_DirectDrawCreate },
    { "DirectDrawEnumerateA", 0, (void*)compat_DirectDrawEnumerateA },
    { "DirectDrawEnumerateExA", 0, (void*)compat_DirectDrawEnumerateExA },
    { "DirectDrawCreateClipper", 0, (void*)compat_DirectDrawCreateClipper },
    { "DirectDrawCreateEx", 0, (void*)compat_DirectDrawCreateEx },
};
#define ROUTE_COUNT ((int)(sizeof s_routes / sizeof s_routes[0]))

static int s_enabled;

int ddraw_compat_init(int enabled) {
    HMODULE ddraw = GetModuleHandleA("ddraw.dll");
    if (!enabled) {
        host_log("ddraw", "video: system ddraw.dll (--ddraw system); DirectDraw calls are observed, not replaced");
        return 1;
    }
    if (!dd_device_vtables_complete() || !dd_surface_vtables_complete() || !dd_object_vtables_complete() ||
        !d3d_vtables_complete()) {
        host_log("ddraw", "compatibility layer has an empty vtable slot; refusing to run");
        return 0;
    }
    if (!ddraw) {
        host_log("ddraw", "ddraw.dll is not loaded after binding, so its exports cannot be routed");
        return 0;
    }
    for (int i = 0; i < ROUTE_COUNT; i++) s_routes[i].system = (uint32_t)(uintptr_t)GetProcAddress(ddraw, s_routes[i].name);
    s_enabled = 1;
    host_log("ddraw", "video: DirectDraw compatibility layer (virtual 16 bpp display, D3D11 presentation); "
             "ddraw.dll's exports are routed to it");
    return 1;
}

int ddraw_compat_active(void) { return s_enabled; }

recomp_func_t ddraw_compat_lookup(uint32_t va) {
    if (!s_enabled) return NULL;
    for (int i = 0; i < ROUTE_COUNT; i++)
        if (s_routes[i].system && s_routes[i].system == va)
            return recomp_lookup_import((uint32_t)(uintptr_t)s_routes[i].compat);
    return NULL;
}
