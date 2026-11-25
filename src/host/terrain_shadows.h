/* The terrain's cast shadows keep the contrast of the game's default brightness (Gamma 5) at any brighter one: the Direct3D
 * terrain ramp keeps its first row for them and ShadowLighting draws with it. */
#ifndef JPT_TERRAIN_SHADOWS_H
#define JPT_TERRAIN_SHADOWS_H

#include <stdint.h>

void terrain_shadows_configure(int shadows);
/* Before the terrain's frame: the game builds its ramp again with every level and terrain texture pool. */
void terrain_shadows_frame(void);
/* A camera being given its properties: ShadowLighting builds its own, then reads its light level. */
void terrain_shadows_camera(uint32_t camera, uint32_t props);
/* DEBUG_SAULO: the shadow span routines ShadowLighting reached, and the totals at exit. */
void terrain_shadows_span(uint32_t routine);
void terrain_shadows_summary(void);

#endif
