/* The HD frontend's picture pipeline: the game's own menu bitmaps resampled to the screen with a Catmull-Rom filter and
 * kept in a bounded cache until the size or the picture changes. Pure memory work, no Windows calls. */
#ifndef FRONTEND_ART_H
#define FRONTEND_ART_H

#include <stddef.h>
#include <stdint.h>

#include "frontend_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Premultiplied BGRA rows, w pixels apart. */
typedef struct {
    int w, h;
    uint32_t* px;
} fa_bitmap_t;

/* One of the game's 16-bit rasters (RGB565); a keyed picture has one transparent value. */
typedef struct {
    const uint16_t* bits;
    int w, h, pitch;
    int keyed;
    uint16_t key;
    uint32_t hash;
    const uint16_t* normal; /* FA_HOVER_LIFT: the same button's normal picture, same size (normal_pitch apart) */
    int normal_pitch;
} fa_source_t;

enum {
    FA_DEDITHER = 1,   /* smooths the 16-bit dither lattice before scaling, never across a real edge */
    FA_CRISP_EDGE = 2, /* narrows the soft ramp a scaled colour key leaves at the outline */
    FA_HOVER_LIFT = 4  /* a hover picture's dark red lettering made bright red */
};

void fa_hover_lift(float* rgba, const uint8_t* solid, int w, int h, const uint16_t* normal, int normal_pitch,
                   uint16_t key, int keyed);

/* The part of a source shown, in source pixels (x0, y0, x1, y1); NULL for all of it. */
int fa_resample16(const fa_source_t* src, const double* part, int dst_w, int dst_h, unsigned flags, fa_bitmap_t* out);
/* An opaque 32-bit picture (BGRA rows, alpha ignored). */
int fa_resample32(const uint32_t* bgra, int w, int h, int pitch, const double* part, int dst_w, int dst_h,
                  fa_bitmap_t* out);
void fa_bitmap_free(fa_bitmap_t* b);

/* A TGA file (uncompressed or run-length, 16-bit 5-5-5, 24 or 32 bits) as opaque BGRA rows, top row first; the rows
 * are malloc'ed. 0 when it is not one of those. */
int fa_tga_decode(const uint8_t* data, size_t size, uint32_t** bgra, int* w, int* h);

/* The bitmap over a clip of premultiplied BGRA rows, its corner at (x, y); pixels outside clip stay untouched. */
void fa_blit_over(uint32_t* dst, long pitch, fe_rect_t clip, const fa_bitmap_t* b, int x, int y);

typedef struct fa_cache fa_cache_t;

/* budget_bytes bounds the pictures kept; the oldest unused ones go first. */
fa_cache_t* fa_cache_create(size_t budget_bytes);
void fa_cache_destroy(fa_cache_t* c);
void fa_cache_clear(fa_cache_t* c);
/* Starts a frame: pictures got since this call are never evicted by later gets of the same frame. */
void fa_cache_begin(fa_cache_t* c);
/* The scaled picture (part as for fa_resample16), valid until the frame ends; NULL when it cannot be made. */
const fa_bitmap_t* fa_cache_get16(fa_cache_t* c, const fa_source_t* src, const double* part, int dst_w, int dst_h,
                                  unsigned flags);
const fa_bitmap_t* fa_cache_get32(fa_cache_t* c, uint32_t hash, const uint32_t* bgra, int w, int h, int pitch,
                                  const double* part, int dst_w, int dst_h);
size_t fa_cache_bytes(const fa_cache_t* c);
int fa_cache_entries(const fa_cache_t* c);
unsigned long fa_cache_hits(const fa_cache_t* c);
unsigned long fa_cache_misses(const fa_cache_t* c);

#ifdef __cplusplus
}
#endif

#endif
