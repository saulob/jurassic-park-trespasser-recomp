/* The HD frontend's bridge to the game (frontend_hd.c): its menus drawn again at the screen's resolution while they
 * use the UI canvas (ui_canvas.c). */
#ifndef FRONTEND_HD_H
#define FRONTEND_HD_H

#include <stdint.h>

#include "frontend_layout.h"

/* Before the game's UI frame on the canvas: 1 when the HD layer is shown instead of the canvas this frame, then
 * frontend_hd_frame_done after the frame. */
int frontend_hd_frame(int screen_w, int screen_h);
void frontend_hd_frame_done(void);
/* A UI frame without the canvas: the frontend's memory is released once menus have been gone for a while. */
void frontend_hd_idle(void);
/* After the game centres a new window: Options loses its Video entry, Audio its 3D acceleration row. */
void frontend_hd_window_centred(uint32_t window);
/* The game opened <user_dir>\SAVEGAME.NNN to write a save: its HD picture is taken from the frozen gameplay picture. */
void frontend_hd_save_opened(const char* path, const char* user_dir);
/* The game deleted a file: a save's HD picture goes with it. */
void frontend_hd_save_deleted(const char* path, const char* user_dir);
/* The game's window stack as the frontend reads it; 0 when the game's own menu must be shown. */
int frontend_hd_read_scene(fe_scene_t* s, int screen_w, int screen_h);
/* The same for a layer of layer_w x layer_h pixels over a game screen of guest_w x guest_h. */
int frontend_hd_read_scene_for(fe_scene_t* s, int guest_w, int guest_h, int layer_w, int layer_h);
/* The gamepad's prompts on a scene read from the game: its glyphs in the Controls window, the menu's buttons. */
void frontend_hd_prompts(fe_scene_t* s);
/* The HD layer's size over a game screen: the frame the renderer presents, within the layer's pixel budget. */
void frontend_hd_layer_size(int guest_w, int guest_h, int frame_w, int frame_h, int* layer_w, int* layer_h);
/* The painter the HD menus use (fonts, materials, threads), shared with the loading screen; NULL when it cannot start. */
struct fe_painter* frontend_hd_painter(int* threads);
/* Frees the menus' layer and caches now (a level load needs the memory); the next menu frame paints them again. */
void frontend_hd_release(void);

#endif
