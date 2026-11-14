/* objectives.c: the current objective from the game's own hint (CSetHintAction::iCurrentHint, the text F1 shows), its
 * target and route resolved by name in the loaded level. */
#ifndef OBJECTIVES_H
#define OBJECTIVES_H

#include <stdint.h>

#include "compass_policy.h"
#include "debug_menu_policy.h"

/* Each pass of the UI loop: follows the hint, resolves positions when the objective or the level changes, fills
 * w->goals when list is set. Returns a notice for the HUD when the objective moved on, else NULL. */
const char* objectives_frame(dm_world_t* w, int list, uint32_t scratch);
const cp_goal_t* objectives_goal(void);

/* debug_menu.c: an instance's CPlacement3 through its p3Placement (vtable + 0x18). */
int debug_menu_placement(uint32_t instance, uint32_t buffer, dm_placement_t* out);

#endif
