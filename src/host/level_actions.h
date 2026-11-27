#ifndef LEVEL_ACTIONS_H
#define LEVEL_ACTIONS_H

#include <stddef.h>
#include <stdint.h>

#include "minimap.h"

/* Nothing is hooked in the original game's own session. */
int level_actions_install(void);
void level_actions_step(void);
/* CE's Teleport of a script: the object of that unique handle placed through the engine's teleport action. */
int level_actions_teleport(uint32_t handle, const float position[3], const float rotation[4]);
/* CE's MoveWithFixedVelocity: the fixed velocity of action 110, kept in saves; 0 when the object has no physics or already
 * moves at a fixed velocity and fail_if_moving is set. milliseconds < 0: until action 111 ends it. */
int level_actions_fixed_velocity(uint32_t handle, const float velocity[3], int milliseconds, int fail_if_moving);
/* After the game read its controls. */
void level_actions_input(uint32_t sinput);
/* The map picture the level set; NULL when it set none. */
const minimap_picture_t* level_actions_map(void);
/* The text that stands for a string of the game's resources, its current hint; 0 when the game's own does. */
int level_actions_hint(unsigned id, char* out, size_t cap);

#endif
