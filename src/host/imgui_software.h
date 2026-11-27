/* Dear ImGui's draw data rendered by the CPU into a 32-bit XRGB picture: the backend of the panels and the HUD where there is no
 * GPU device (the Linux host draws them over a copy of the game's frame). Triangles are filled with the interpolated vertex colour
 * and, when the command has a texture, its alpha or RGBA texels (the font atlas), clipped to the command's rectangle and blended
 * over what is there. */
#ifndef JPT_IMGUI_SOFTWARE_H
#define JPT_IMGUI_SOFTWARE_H

#include <stdint.h>

#include "imgui.h"

/* Tells the context the backend creates and updates textures from ImDrawData::Textures (what this one does: it samples them in
 * place). Once, after the context is created. */
void imgui_software_init(ImGuiIO& io);
/* Draws data into pixels (width x height, pitch_pixels between rows). */
void imgui_software_render(ImDrawData* data, uint32_t* pixels, int width, int height, int pitch_pixels);
/* Draws data into straight-alpha RGBA8 pixels (width x height x 4 bytes, tightly packed). */
#ifndef OVERLAY_TILE_SHIFT
#define OVERLAY_TILE_SHIFT 5
#endif
void imgui_software_update_textures(ImDrawData* data);
/* A draw list callback: what follows is blended additively (colour times alpha added) until ImDrawCallback_ResetRenderState.
 * Backends that draw themselves recognise it; the Direct3D 11 one calls it, and it runs imgui_blend_additive_hook. */
void imgui_blend_additive(const ImDrawList* list, const ImDrawCmd* cmd);
extern void (*imgui_blend_additive_hook)(void);
/* Straight-alpha layers cannot add to what is under them: additive commands of the RGBA backend are blended as alpha, counted here. */
extern unsigned imgui_software_rgba_additive;
void imgui_software_render_rgba(ImDrawData* data, uint8_t* rgba, int width, int height, uint8_t* tiles, int tiles_w,
                                int tiles_h);

#endif
