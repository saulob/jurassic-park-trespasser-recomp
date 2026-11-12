#include "host.h"
#include "occlusion_kernel.h"
#include "override_registry.h"
#include "polygon_submit.h"
#include "water_kernels.h"

/* A listed function whose lifted body is missing or still named sub_<va> fails the link, never the run. */
#define GUEST_OVERRIDE OVERRIDE_LIFTED_DECL
#include "guest_override_list.h"
#undef GUEST_OVERRIDE
#define GUEST_OVERRIDE OVERRIDE_STABLE_ENTRY
#include "guest_override_list.h"
#undef GUEST_OVERRIDE

static override_entry_t s_entries[] = {
#define GUEST_OVERRIDE OVERRIDE_ROW
#include "guest_override_list.h"
#undef GUEST_OVERRIDE
};

static override_registry_t s_registry = { s_entries, sizeof s_entries / sizeof s_entries[0] };

int guest_overrides_enable(void) {
    if (!override_registry_valid(&s_registry)) {
        host_log("host", "src/host/guest_override_list.h is not sorted or repeats an address: refusing to run");
        return 0;
    }
    s_registry.target_verified = 1;
    return 1;
}

int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    return override_registry_install(&s_registry, va, replacement, lifted);
}

recomp_func_t guest_override_lifted(uint32_t va) {
    override_entry_t* e = override_registry_find(&s_registry, va);
    return e ? e->lifted : NULL;
}

void guest_kernels_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t kernel;
        recomp_func_t* lifted;
    } k[] = {
        { WATER_ROWS_VA, water_rows, &water_rows_lifted },
        { WATER_WAVE_STEP_VA, water_wave_step, &water_wave_step_lifted },
        { OCCLUSION_TEST_VA, occlusion_test, &occlusion_test_lifted },
        { POLYGON_SUBMIT_VA, polygon_submit, &polygon_submit_lifted },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) guest_override_install(k[i].va, k[i].kernel, k[i].lifted);
}

int guest_overrides_check(void) {
    override_counts_t c = override_registry_counts(&s_registry);
    host_log("host", "host overrides: %u of %u installed (%u required, %u optional), %u not used by this configuration",
             c.installed, c.listed, c.required_installed, c.optional_installed, c.unused);
    for (unsigned i = 0; i < c.refused && i < OVERRIDE_MAX_REFUSALS; i++)
        host_log("host", "%s override of sub_%08X not installed: %s", s_registry.refusals[i].required ? "required" :
                 "optional", s_registry.refusals[i].va, override_refusal_text(s_registry.refusals[i].why));
    if (!c.required_refused) return 1;
    host_log("host", "refusing to run without its required overrides");
    return 0;
}
