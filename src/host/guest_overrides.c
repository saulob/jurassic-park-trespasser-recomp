#include "host.h"
#include "water_kernels.h"

/* With incremental linking every lifted function is reached through a 5-byte "jmp rel32" thunk, from
 * direct calls and from the dispatch table alike, so repointing the thunk replaces the function
 * everywhere and leaves its body callable. Without a thunk the lifted function stays in use. */
int guest_override_redirect(uint32_t va, recomp_func_t replacement, recomp_func_t* body) {
    BYTE* p = (BYTE*)recomp_lookup(va);
    DWORD old;
    int32_t rel;
    if (!p || p[0] != 0xE9) return 0;
    memcpy(&rel, p + 1, 4);
    *body = (recomp_func_t)(p + 5 + rel);
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    rel = (int32_t)((BYTE*)replacement - (p + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
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
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (guest_override_redirect(k[i].va, k[i].kernel, k[i].lifted))
            host_log("host", "sub_%08X (%s) runs as its host kernel", k[i].va, k[i].name);
        else
            host_log("host", "sub_%08X (%s) stays lifted: no incremental-link thunk to redirect", k[i].va, k[i].name);
    }
}
