/* The lifted builds linked into this program (src/recomp/gen*, one per lift) and the choice of the one that runs. */
#ifndef JPT_LIFT_REGISTRY_H
#define JPT_LIFT_REGISTRY_H

#include <stddef.h>

#include "active_profile.h"
#include "native32.h"

typedef struct {
    const jpt_lift_meta_t* meta;
    const recomp_dispatch_entry_t* dispatch; /* meta->function_count entries, sorted by address */
} lift_image_t;

/* The generated sources of a lift. */
const lift_image_t* lift_image(jpt_lift_t lift);

/* Chooses the lift `version` runs, before any guest code does: its generated sources must be the ones this program was
 * built to carry (id, source profile and executable, generator version, image layout), the profile's tables become the
 * host's (active_profile.h, GSUB) and its dispatch table the runtime's. 0 with the reason in `error` otherwise; nothing
 * is installed then. */
int lift_activate(const jpt_game_version_t* version, char* error, size_t cap);

/* The active lift's generated sources; NULL before lift_activate. */
const lift_image_t* lift_active_image(void);

/* Where the active lift's guest image has its entry point, lifted .text and SelfMod section (0 before lift_activate). */
extern uint32_t lift_entry_va, lift_text_start, lift_text_end, lift_selfmod_start, lift_selfmod_end;

#endif
