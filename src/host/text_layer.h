/* The game's gameplay text (subtitles, hints) kept as a picture of its own: only the pixels its draws wrote, so the host can
 * draw its HUD under them. Pure memory: both renderers feed it and the presentation reads it. */
#ifndef TEXT_LAYER_H
#define TEXT_LAYER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565 magenta (0xF81F) standing for "not text" in the layer and in a surface while a draw is being captured. */
#define TL_MARKER 63519u

typedef struct {
    long left, top, right, bottom;
} tl_rect_t;

/* A frame is the span between two calls of tl_frame_end. Text of an earlier frame is never shown. */
void tl_frame_end(void);

/* GPU renderer: the surface already holds the marker where a box was claimed. tl_note announces a box before the draw, tl_scan
 * reads the announced boxes after it. */
void tl_note(unsigned w, unsigned h, const tl_rect_t* box);
void tl_scan(const uint8_t* bits, long pitch, unsigned w, unsigned h);

/* CPU renderer: the draw is made over a marker-filled copy of the box (the pixels under it saved), and tl_settle keeps what
 * the draw wrote as text and puts the saved pixels back everywhere else. 0 when the box is not text-sized. */
int tl_claim(uint8_t* bits, long pitch, unsigned w, unsigned h, const tl_rect_t* box);
void tl_settle(const uint8_t* bits);

typedef struct {
    unsigned w, h; /* the surface the text was drawn on */
    tl_rect_t dirty;
} tl_view_t;

/* The text of the frame being presented; 0 when there is none. */
int tl_current(tl_view_t* out);
/* BGRA of region (inside the dirty box) through table, straight alpha: 0 where there is no text. */
void tl_expand(const tl_rect_t* region, const uint32_t* table, uint32_t* out, size_t out_pitch_pixels);

#ifdef __cplusplus
}
#endif

#endif
