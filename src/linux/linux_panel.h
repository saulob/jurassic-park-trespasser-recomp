/* The host's menus and HUD on Linux: the F3 Objectives window, the F10 Cheats panel, the compass, the objective markers and the
 * minimap, drawn by the same Dear ImGui code the Windows host draws with (src/host/debug_menu_panel.cpp) through a software backend
 * into a copy of the game's frame. The model, the layout and the hit table are shared; only the pixels are Linux's. */
#ifndef JPT_LINUX_PANEL_H
#define JPT_LINUX_PANEL_H

#include <stdint.h>

#include "debug_menu_layout.h"

/* Something of the host is to be drawn over the frame: a menu, the HUD or the frame counter. */
int lx_panel_wanted(int frame_counter_on);
/* Draws it into pixels (XRGB8888, width x height, pitch in pixels); counter is the frame counter's text or NULL. 1 when anything
 * was drawn. */
int lx_panel_draw(uint32_t* pixels, int width, int height, int pitch, const char* counter);
/* Where the drawn menu can be clicked. */
const dml_hits_t* lx_panel_hits(void);
void lx_panel_release(void);

#endif
