#include "guest_abi.h"
#include "linux_overrides.h"

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

void lx_lift_register_overrides(jpt_lift_t lift) {
    switch (lift) {
    case JPT_LIFT_116: lx_overrides_set_lift_registry(&lift116_registry, lift); break;
    case JPT_LIFT_ES117: lx_overrides_set_lift_registry(&lift_es117_registry, lift); break;
    case JPT_LIFT_PTBR117: lx_overrides_set_lift_registry(&lift_ptbr117_registry, lift); break;
    default: lx_overrides_set_lift_registry(NULL, JPT_LIFT_NONE); break;
    }
}
