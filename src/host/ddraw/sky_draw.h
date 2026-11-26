#ifndef SKY_DRAW_H
#define SKY_DRAW_H

#include "d3d_legacy.h"
#include "sky_layers.h"

/* In place of the engine's draw of its sky plane, whose vertices these are. */
void sky_draw_layers(d3d_device_t* dev, const void* vertices, DWORD count);
void sky_draw_free(void* gpu);

#endif
