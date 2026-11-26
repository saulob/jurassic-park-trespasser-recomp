#ifndef LEVEL_ACTIONS_H
#define LEVEL_ACTIONS_H

#include <stddef.h>
#include <stdint.h>

#include "minimap.h"

/* Nothing is hooked in the original game's own session. */
int level_actions_install(void);
void level_actions_step(void);
/* After the game read its controls. */
void level_actions_input(uint32_t sinput);
/* The map picture the level set; NULL when it set none. */
const minimap_picture_t* level_actions_map(void);
/* The text that stands for a string of the game's resources, its current hint; 0 when the game's own does. */
int level_actions_hint(unsigned id, char* out, size_t cap);

#endif
