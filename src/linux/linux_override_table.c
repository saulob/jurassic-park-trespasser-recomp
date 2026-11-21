#include "guest_abi.h"
#include "linux_overrides.h"

#define OVERRIDE_LIFT lift116_
#define OVERRIDE_LIST "guest_override_list.h"
#include "guest_override_lift.inc"

void lx_lift116_register_overrides(void) { lx_overrides_set_registry(&lift116_registry); }
