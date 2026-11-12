#include "host.h"
#include "occlusion_kernel.h"
#include "polygon_submit.h"
#include "water_kernels.h"

#define MAX_OVERRIDES 64

static struct {
    uint32_t va;
    recomp_func_t body;
} s_overrides[MAX_OVERRIDES];
static unsigned s_override_count;

recomp_func_t guest_override_body(uint32_t va) {
    for (unsigned i = 0; i < s_override_count; i++)
        if (s_overrides[i].va == va) return s_overrides[i].body;
    return NULL;
}

/* With incremental linking every lifted function is reached through a 5-byte "jmp rel32" thunk, from
 * direct calls and from the dispatch table alike, so repointing the thunk replaces the function
 * everywhere and leaves its body callable. Without a thunk the lifted function stays in use. */
int guest_override_redirect(uint32_t va, recomp_func_t replacement, recomp_func_t* body) {
    BYTE* p = (BYTE*)recomp_lookup(va);
    DWORD old;
    int32_t rel;
    if (!p || p[0] != 0xE9 || s_override_count == MAX_OVERRIDES) return 0;
    memcpy(&rel, p + 1, 4);
    *body = (recomp_func_t)(p + 5 + rel);
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    rel = (int32_t)((BYTE*)replacement - (p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_overrides[s_override_count].va = va;
    s_overrides[s_override_count++].body = *body;
    return 1;
}

/* DEBUG_SAULO: how often the host kernels ran and whether a per-frame cache of the occlusion test would hit. */
void guest_kernels_window(unsigned long long frames) {
    unsigned long long host, lifted, calls, census_calls, repeats;
    double f = frames ? (double)frames : 1.0;
    polygon_submit_take_counts(&host, &lifted);
    occlusion_take_counts(&calls, &census_calls, &repeats);
    if (!host && !lifted && !calls) return;
    host_log("kernels", "last 5 s per frame: polygon submissions %.0f, host path %.1f%%; occlusion tests %.0f, repeating "
             "an earlier test of the same frame %.2f%% (%llu of %llu sampled)", (host + lifted) / f,
             host + lifted ? 100.0 * host / (double)(host + lifted) : 0.0, calls / f,
             census_calls ? 100.0 * repeats / (double)census_calls : 0.0, repeats, census_calls);
}

void guest_overrides_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t kernel;
        recomp_func_t* lifted;
        const char* name;
    } k[] = {
        { WATER_ROWS_VA, water_rows, &water_rows_lifted, "water texture rows" },
        { WATER_WAVE_STEP_VA, water_wave_step, &water_wave_step_lifted, "CSimWaveVar2D step" },
        { OCCLUSION_TEST_VA, occlusion_test, &occlusion_test_lifted, "object occlusion test" },
        { POLYGON_SUBMIT_VA, polygon_submit, &polygon_submit_lifted, "Direct3D polygon submission" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (guest_override_redirect(k[i].va, k[i].kernel, k[i].lifted))
            host_log("host", "sub_%08X (%s) runs as its host kernel", k[i].va, k[i].name);
        else
            host_log("host", "sub_%08X (%s) stays lifted: no incremental-link thunk to redirect", k[i].va, k[i].name);
    }
}
