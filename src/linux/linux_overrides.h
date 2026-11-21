/* The host's overrides and lift hooks of the lifted game on Linux: the same stable entries and registry as the Windows host
 * (guest_override_list.h, lift_hook_list.h, guest_overrides.c's machinery) over the lifted code lift116 carries. The
 * features install themselves through guest_override_install (guest_abi.h). */
#ifndef JPT_LINUX_OVERRIDES_H
#define JPT_LINUX_OVERRIDES_H

#include "override_registry.h"

/* The table of stable entries of a lift (guest_override_lift.inc over its guest_override_list.h): the one lift116 carries is
 * registered by lx_lift116_register_overrides, which the program that links the lifted code calls before it boots. */
void lx_overrides_set_registry(override_registry_t* table);
void lx_lift116_register_overrides(void);
/* Chooses the registry of the active profile's lift and trusts the target executable, which the boot has checked against
 * the lift's source build. 0 when no profile is active or the list is not sorted and unique. */
int guest_overrides_enable(void);
/* Back to the lifted bodies: the next run in this process starts clean. */
void lx_overrides_reset(void);
override_counts_t lx_overrides_counts(void);
/* The state (override_state_t) of the override at va, -1 when it is not listed or no run is active. */
int lx_overrides_state(uint32_t va);
/* The Linux log lines of what was installed and refused; 0 when a required override that was attempted is missing. */
int guest_overrides_check(void);
/* The lift hooks lift116 carries: each is defined by a host feature (guest_va.h GHOOK). */
unsigned lx_lift116_hook_count(void);
unsigned lx_lift116_override_count(void);

#endif
