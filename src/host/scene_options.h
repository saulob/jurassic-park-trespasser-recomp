/* Fog Off and Shadows Off: the engine's own data changed where every renderer reads it, so the Software, Direct3D 11 and
 * Vulkan renderers of both hosts follow. On leaves the game as it is. */
#ifndef JPT_SCENE_OPTIONS_H
#define JPT_SCENE_OPTIONS_H

#include <stdint.h>

void scene_options_configure(int fog, int shadows);
/* Idempotent: the game rebuilds its fog tables with every level, fog trigger and quality change. */
void scene_options_step(void);
/* At BeginScene: the step above, and the level's fog colour, which the device and the game's background copy follow (a
 * recreated device loses it and the background is cached when the device is created). 0 when it cannot be read. */
int scene_fog_color(uint32_t* color);

#endif
