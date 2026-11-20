/* The one text rasteriser behind TextOutA and DrawTextExA: an embedded TrueType face (Arial's stand-in) scaled to the size the guest
 * asked for, measured per glyph in whole pixels the way GDI does, drawn as coverage the caller blends onto its surface. */
#ifndef JPT_LINUX_TEXT_H
#define JPT_LINUX_TEXT_H

#include <stdint.h>

typedef struct {
    int height, ascent, descent, avg_width, max_width;
} lx_text_metrics_t;

typedef void (*lx_text_pixel_fn)(void* ctx, int x, int y, int coverage);

/* GDI lfHeight: negative = character height in pixels, positive = cell height, 0 = a 16 pixel cell. Never 0 as a result. */
float lx_text_scale(int gdi_height);
void lx_text_metrics(float scale, lx_text_metrics_t* out);
/* Code page of the guest's 8-bit strings: 1252 (default) or 1251. */
void lx_text_set_codepage(int codepage);
int lx_text_advance(float scale, uint8_t ch, int bold);
int lx_text_width(float scale, const char* s, int n, int bold);
/* Draws one glyph with its baseline origin at (x, baseline). */
void lx_text_glyph(float scale, uint8_t ch, int bold, int x, int baseline, lx_text_pixel_fn put, void* ctx);

#endif
