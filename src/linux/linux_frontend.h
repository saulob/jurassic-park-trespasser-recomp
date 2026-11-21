/* The HD front end and the HD loading screen on Linux. The painter, layouts and bridges are the Windows host's (frontend_hd.c,
 * loading_hd.c); this is their presenting side: the layer they paint is composed at the host window's resolution over the guest's
 * scaled picture, inside the rectangle the guest screen occupies, before the host's overlays. The guest's own framebuffer is never
 * touched, so a menu the front end cannot show is the game's own picture. */
#ifndef JPT_LINUX_FRONTEND_H
#define JPT_LINUX_FRONTEND_H

#include <stdint.h>

#include "linux_window.h"

/* Installs the overrides the front end needs (window centring, the loading screen); 0 when one is missing. */
int lx_frontend_install(void);
void lx_frontend_reset(void);
/* The front end shows HD menus and loading screens at all (a launcher setting on Windows, on here). */
void lx_frontend_enable(int enabled);
int lx_frontend_enabled(void);

/* Before a frame of the guest's screen is shown in a window of window_w x window_h: the menus the game has open are read and their
 * layer painted. Returns 1 when a layer is ready for lx_frontend_compose. */
int lx_frontend_frame(int guest_w, int guest_h, int window_w, int window_h);
/* The layer over the guest's picture, in the window's resolution: black bars around the picture, the picture scaled into its
 * rectangle, the layer and the pointer on top (XRGB8888, an internal buffer valid until the next call). 0 when there is no layer. */
int lx_frontend_compose(const lx_frame_t* in, lx_frame_t* out, int window_w, int window_h, int stretch);
/* The frame is shown: the layer and pointer are cleared until the next frame sets them. */
void lx_frontend_frame_done(void);
/* A level load ended or the game's UI loop began a frame. */
void lx_frontend_ui_frame(void);

/* The game's file calls the front end follows: a save was created, a file deleted (their HD picture goes with them). */
void lx_frontend_file_created(const char* guest_path);
void lx_frontend_file_deleted(const char* guest_path);

/* What the last frame showed. */
typedef struct {
    unsigned frames_with_layer, composed_frames;
    int layer_w, layer_h, view_x, view_y, view_w, view_h;
    int pointer_drawn;
} lx_frontend_stats_t;
void lx_frontend_stats(lx_frontend_stats_t* out);

#endif
