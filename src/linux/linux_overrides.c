#include "linux_overrides.h"

#include "guest_abi.h"
#include "linux_log.h"

static override_registry_t* s_table;
static override_registry_t* s_registry;
static jpt_lift_t s_lift = JPT_LIFT_NONE;

void lx_overrides_set_registry(override_registry_t* table) { lx_overrides_set_lift_registry(table, JPT_LIFT_NONE); }

void lx_overrides_set_lift_registry(override_registry_t* table, jpt_lift_t lift) {
    s_table = table;
    s_lift = lift;
}

int guest_overrides_enable(void) {
    jpt_lift_t active = jpt_active_lift();
    if (!s_table || (s_lift != JPT_LIFT_NONE && s_lift != active)) {
        lx_log("host", "no override table for the active lift: refusing to run");
        return 0;
    }
    s_registry = s_table;
    if (!override_registry_valid(s_registry)) {
        lx_log("host", "the override list is not sorted or repeats an address: refusing to run");
        return 0;
    }
    s_registry->target_verified = 1;
    return 1;
}

void lx_overrides_reset(void) {
    if (s_table) override_registry_reset(s_table);
    s_registry = NULL;
}

int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted) {
    static override_registry_t none;
    return override_registry_install(s_registry ? s_registry : &none, va, replacement, lifted);
}

recomp_func_t guest_override_lifted(uint32_t va) {
    override_entry_t* e = s_registry ? override_registry_find(s_registry, va) : NULL;
    return e ? e->lifted : NULL;
}

override_counts_t lx_overrides_counts(void) {
    override_counts_t none = { 0 };
    return s_registry ? override_registry_counts(s_registry) : none;
}

int lx_overrides_state(uint32_t va) {
    override_entry_t* e = s_registry ? override_registry_find(s_registry, va) : NULL;
    return e ? (int)e->state : -1;
}

int guest_overrides_check(void) {
    override_counts_t c = lx_overrides_counts();
    lx_log("host", "host overrides: %u of %u installed (%u required, %u optional), %u not used by this configuration", c.installed,
           c.listed, c.required_installed, c.optional_installed, c.unused);
    for (unsigned i = 0; s_registry && i < c.refused && i < OVERRIDE_MAX_REFUSALS; i++)
        lx_log("host", "%s override of sub_%08X not installed: %s", s_registry->refusals[i].required ? "required" : "optional",
               s_registry->refusals[i].va, override_refusal_text(s_registry->refusals[i].why));
    if (!c.required_refused) return 1;
    lx_log("host", "refusing to run without its required overrides");
    return 0;
}

static unsigned count_hooks(void) {
    unsigned n = 0;
#define LIFT_HOOK(va) n++;
#include "lift_hook_list.h"
#undef LIFT_HOOK
    return n;
}

unsigned lx_lift_hook_count(jpt_lift_t lift) { return lift == JPT_LIFT_116 ? count_hooks() : 0; }

unsigned lx_lift_override_count(jpt_lift_t lift) {
    unsigned n = 0;
    switch (lift) {
    case JPT_LIFT_116:
#define GUEST_OVERRIDE(va, need) n++;
#include "guest_override_list.h"
#undef GUEST_OVERRIDE
        break;
    case JPT_LIFT_ES117:
#define GUEST_OVERRIDE(va, need) n++;
#include "profiles/lift_es117/guest_override_list.h"
#undef GUEST_OVERRIDE
        break;
    case JPT_LIFT_PTBR117:
#define GUEST_OVERRIDE(va, need) n++;
#include "profiles/lift_ptbr117/guest_override_list.h"
#undef GUEST_OVERRIDE
        break;
    default: break;
    }
    return n;
}

unsigned lx_lift116_hook_count(void) { return lx_lift_hook_count(JPT_LIFT_116); }
unsigned lx_lift116_override_count(void) { return lx_lift_override_count(JPT_LIFT_116); }
