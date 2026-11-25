/* The guest's screen: the display mode the guest asked for, the one real window the backend opens for its top-level window, and
 * the pictures presented into it. jpt_presentation decides where a frame lands (letterbox, generation commit). */
#ifndef JPT_LINUX_SCREEN_H
#define JPT_LINUX_SCREEN_H

#include <stdint.h>

#include "linux_window.h"

typedef struct {
    int width, height, bpp;
} lx_display_mode_t;

void lx_screen_reset(void);
lx_display_mode_t lx_screen_mode(void);
/* DirectDraw SetDisplayMode: the guest's screen changes; the host window keeps the user's size. */
void lx_screen_set_mode(int width, int height, int bpp);

/* The host window of the guest's top-level window: opened once, on first use. */
int lx_screen_window_open(const char* title, int client_w, int client_h);
int lx_screen_window_is_open(void);
void lx_screen_window_close(void);
void lx_screen_set_title(const char* title);
void lx_screen_set_visible(int visible);
void lx_screen_window_client(int* w, int* h);
void lx_screen_set_client_size(int w, int h);

/* Shows a frame of the guest's screen. */
void lx_screen_present(const lx_frame_t* frame);
/* The same for a frame whose picture is the GPU's scene. */
void lx_screen_present_scene(int width, int height);
unsigned lx_screen_frames(void);
/* Headless runs: ends the run (LX_STOP) after this many frames were presented; 0 = never. */
void lx_screen_set_frame_limit(unsigned frames);
/* Called after each presented frame with its number; lets a headless run feed input at a known frame. */
typedef void (*lx_frame_hook_fn)(unsigned frame, void* user);
void lx_screen_set_frame_hook(lx_frame_hook_fn fn, void* user);
/* A host client pixel as a guest screen pixel; 0 when it falls outside the picture. */
int lx_screen_client_to_guest(int cx, int cy, int* gx, int* gy);

#endif
