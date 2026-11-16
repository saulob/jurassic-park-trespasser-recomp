/* cheats.c: the F10 cheats on the game's state. */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdint.h>

#include "cheats_policy.h"

#define CHEATS_AI_BORING_SHIFT 16 /* bBoring, the third byte of the AI system's flags: DINOS */

/* Once, before the game runs: the launcher's saved cheats (jpt_config_t.cheat; NULL: the defaults) are the values F10
 * starts from, and every change of an earlier session is forgotten. */
void cheats_start(const int saved[JPT_CHEAT_COUNT]);
/* The AI system's flags: 1 when read; the freeze is the Freeze Dinosaurs cheat, kept for the session. */
int cheats_read_ai(uint32_t* system, uint32_t* flags);
int cheats_freeze_ai(int on);
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
/* The Movement Speed multiplier written into the player's rSpeedMultiplier now; 1 when none is. */
float cheats_speed_factor(void);
/* Pelvic_IDOF of the human pelvis, before Foot_IDOF reads the jump. */
void cheats_before_pelvis(void);
/* What the cheats ask for, whether the free camera is in use, and the player's rSpeedMultiplier (-1 unknown). */
void cheats_watch(ch_state_t* want, int* free_camera, float* speed_real);
/* Runs a cheat action: why it could not be done, else NULL (the row shows the result). */
const char* cheats_run(const dm_action_t* a, const char* level, uint32_t scratch);

#endif
