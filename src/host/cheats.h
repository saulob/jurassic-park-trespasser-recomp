/* cheats.c: the F10 cheats on the game's state. */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdint.h>

#include "cheats_policy.h"

/* Each pass of the game's UI loop: values in or out of the game, the free camera moved. */
void cheats_frame(const dm_world_t* w, uint32_t scratch);
int cheats_busy(void);
/* Before the host moves the player or loads: the game's own values back. */
void cheats_suspend(void);
void cheats_world(dm_world_t* w);
/* The player a free camera left, for the camera given; 0 otherwise. */
uint32_t cheats_detached_from(uint32_t camera);
/* After CInput's read (sinput = its SInput): 1 when the free camera took it and Anne must not get it. */
int cheats_take_input(uint32_t sinput);
/* Pelvic_IDOF of the human pelvis, before Foot_IDOF reads the jump. */
void cheats_before_pelvis(void);
/* The menu's feedback for a cheat action; NULL for an action that is not a cheat's. */
const char* cheats_run(const dm_action_t* a, const char* level, uint32_t scratch);

#endif
