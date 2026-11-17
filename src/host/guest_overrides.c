#include "host.h"

#include "lift_registry.h"
#include "occlusion_kernel.h"
#include "override_registry.h"
#include "polygon_submit.h"
#include "water_kernels.h"

#define OVERRIDE_LIFT lift116_
#define OVERRIDE_LIST "guest_override_list.h"
#include "guest_override_lift.inc"
#undef OVERRIDE_LIFT
#undef OVERRIDE_LIST

#define OVERRIDE_LIFT lift_es117_
#define OVERRIDE_LIST "profiles/lift_es117/guest_override_list.h"
#include "guest_override_lift.inc"
#undef OVERRIDE_LIFT
#undef OVERRIDE_LIST

#define OVERRIDE_LIFT lift_ptbr117_
#define OVERRIDE_LIST "profiles/lift_ptbr117/guest_override_list.h"
#include "guest_override_lift.inc"
#undef OVERRIDE_LIFT
#undef OVERRIDE_LIST

static override_registry_t* const s_registries[JPT_LIFT_COUNT] = {
    [JPT_LIFT_116] = &lift116_registry,
    [JPT_LIFT_ES117] = &lift_es117_registry,
    [JPT_LIFT_PTBR117] = &lift_ptbr117_registry,
};

static override_registry_t* s_registry;

int guest_overrides_enable(void) {
    jpt_lift_t lift = jpt_active_lift();
    if (lift == JPT_LIFT_NONE) {
        host_log("host", "no profile is active: refusing to run");
        return 0;
    }
    s_registry = s_registries[lift];
    if (!override_registry_valid(s_registry)) {
        host_log("host", "the override list of %s is not sorted or repeats an address: refusing to run",
                 jpt_lift_data[lift].id);
        return 0;
    }
    s_registry->target_verified = 1;
    return 1;
}

int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    static override_registry_t none;
    return override_registry_install(s_registry ? s_registry : &none, va, replacement, lifted);
}

recomp_func_t guest_override_lifted(uint32_t va) {
    override_entry_t* e = s_registry ? override_registry_find(s_registry, va) : NULL;
    return e ? e->lifted : NULL;
}

void guest_kernels_install(void) {
    const struct {
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
    override_counts_t c = override_registry_counts(s_registry);
    host_log("host", "host overrides: %u of %u installed (%u required, %u optional), %u not used by this configuration",
             c.installed, c.listed, c.required_installed, c.optional_installed, c.unused);
    for (unsigned i = 0; i < c.refused && i < OVERRIDE_MAX_REFUSALS; i++)
        host_log("host", "%s override of sub_%08X not installed: %s", s_registry->refusals[i].required ? "required" :
                 "optional", s_registry->refusals[i].va, override_refusal_text(s_registry->refusals[i].why));
    if (!c.required_refused) return 1;
    host_log("host", "refusing to run without its required overrides");
    return 0;
}
