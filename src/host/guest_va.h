#ifndef JPT_GUEST_VA_H
#define JPT_GUEST_VA_H

#include "active_profile.h"

/* A guest address of the 1.0.116 executable as the active profile's lift has it (active_profile.h), read from the table
 * jpt_profile_activate installed. GSUB is the lifted function at such an address and GHOOK the host function the lift
 * calls there (the same in every lift). An address no table lists is a compile error. */
#define GVA(address) (jpt_gva[GVAI_##address])
#define GSUB(address) (jpt_gsub[GSUBI_##address])
#define GHOOK(address) GHOOK_##address
/* The id of a guest address, a constant for tables built before the profile is chosen; GVA_AT reads it. */
#define GVA_ID(address) GVAI_##address
#define GVA_AT(id) (jpt_gva[id])

#endif
