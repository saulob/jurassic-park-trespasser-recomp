/* The host's own drawing over the game's picture on Linux: the F6 frame counter (the same FPS text as the Windows title and panel,
 * the same measured rate) and the short notices of Quick Save and Quick Load, rasterised by linux_text into a copy of the frame the
 * presenter shows. The guest surface is never touched. */
#ifndef JPT_LINUX_OVERLAY_H
#define JPT_LINUX_OVERLAY_H

#include <stdint.h>

#include "linux_window.h"

void lx_overlay_reset(void);
void lx_overlay_configure(int frame_counter_on);
void lx_overlay_toggle_frame_counter(void);
int lx_overlay_frame_counter_on(void);
/* One frame was presented: the rate is measured over about a second of them. */
void lx_overlay_frame_presented(uint64_t now_ms);
/* The text of the counter ("FPS: 59.9"); 0 while it is off or no rate is measured yet. */
int lx_overlay_frame_counter_text(char* out, unsigned capacity);
void lx_overlay_notice(const char* text, uint64_t now_ms);
/* The guest picture's rectangle inside the next composed frame (the card and the hint sit in it); 0 for the whole frame. */
void lx_overlay_set_picture(int x, int y, int w, int h);
/* The frame with the overlay drawn in (XRGB8888, an internal buffer valid until the next call); 0 when nothing shows. */
int lx_overlay_compose(const lx_frame_t* in, lx_frame_t* out, uint64_t now_ms);
/* The overlay alone, as premultiplied RGBA of w x h (an internal buffer valid until the next call); NULL when nothing shows. */
const uint8_t* lx_overlay_compose_layer(int w, int h, uint64_t now_ms);

#endif
