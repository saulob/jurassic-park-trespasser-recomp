/* The host's overrides and lift hooks of the lifted game on Linux: the same stable entries and registry as the Windows host
 * (guest_override_list.h, lift_hook_list.h, guest_overrides.c's machinery) over the lift the selected version runs. The
 * features install themselves through guest_override_install (guest_abi.h). */
#ifndef JPT_LINUX_OVERRIDES_H
#define JPT_LINUX_OVERRIDES_H

#include "active_profile.h"
#include "override_registry.h"

/* The table of stable entries of a lift (guest_override_lift.inc over its guest_override_list.h). lx_lift_register_overrides
 * chooses the one the selected version runs; the program calls it before the features start. */
void lx_overrides_set_registry(override_registry_t* table);
void lx_overrides_set_lift_registry(override_registry_t* table, jpt_lift_t lift);
void lx_lift_register_overrides(jpt_lift_t lift);
/* Chooses the registry of the active profile's lift and trusts the target executable, which the boot has checked against
 * the lift's source build. 0 when no profile is active, the registry belongs to another lift, or the list is not sorted and
 * unique. */
int guest_overrides_enable(void);
/* Back to the lifted bodies: the next run in this process starts clean. */
void lx_overrides_reset(void);
override_counts_t lx_overrides_counts(void);
/* The state (override_state_t) of the override at va, -1 when it is not listed or no run is active. */
int lx_overrides_state(uint32_t va);
/* The Linux log lines of what was installed and refused; 0 when a required override that was attempted is missing. */
int guest_overrides_check(void);
unsigned lx_lift_hook_count(jpt_lift_t lift);
unsigned lx_lift_override_count(jpt_lift_t lift);
unsigned lx_lift116_hook_count(void);
unsigned lx_lift116_override_count(void);

#endif
