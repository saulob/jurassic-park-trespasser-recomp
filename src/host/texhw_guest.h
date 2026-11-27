#ifndef TEXHW_GUEST_H
#define TEXHW_GUEST_H

#include <stdint.h>

/* id: of jpt_texhw.h, 0 for none. */
typedef void (*texhw_tag_fn)(uint32_t texture_interface, int id);
/* After the game linked the polygons' textures to Direct3D: tells the renderer which of them a custom level overrides. */
void texhw_tag_polygons(uint32_t count, uint32_t polygons, texhw_tag_fn tag);
/* Around the game's drawing of one of them: how the renderer lights it, when its override has a normal map. */
void texhw_lit_begin(uint32_t polygon);
void texhw_lit_end(void);

#endif
