/* The gameplay text overlay's word wrap (0x0050E8C0), for text with bytes above 0x7F: retail reads each advance
 * width through a signed char, so accented and Cyrillic letters took widths from memory before the font. Everything
 * else is retail's: break characters, CR, alignment and the laid-out blob. */
#ifndef TEXT_LAYOUT_H
#define TEXT_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TL_ALIGN_CENTRE 1u
#define TL_ALIGN_LEFT 2u
#define TL_ALIGN_RIGHT 4u
#define TL_ALIGN_BOTTOM 0x10u
#define TL_MAX_LINES 256
#define TL_RECORD 12u /* x, y, character count, then the line's NUL-terminated text */

typedef struct {
    int32_t width, height; /* the raster's +0x08, +0x0C */
    int32_t left, top;     /* its +0x88, +0x8C */
    int32_t line_height;   /* the font's tallest glyph (+0x0C) */
} tl_view_t;

typedef uint32_t (*tl_width_fn)(void* ctx, unsigned char c);

/* Lays text out into out (u32 blob size, u32 line count, the lines' records), as 0x0050E8C0 does. Returns the blob
 * size; 0 when cap cannot hold it. */
size_t tl_layout(const char* text, uint32_t flags, const tl_view_t* view, tl_width_fn width, void* ctx, uint8_t* out,
                 size_t cap);

#ifdef __cplusplus
}
#endif

#endif
