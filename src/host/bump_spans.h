#ifndef TRESPASSER_BUMP_SPANS_H
#define TRESPASSER_BUMP_SPANS_H

#include "guest_va.h"
#include <stdint.h>

/* The software renderer's bump-mapped scanlines (TBumpLinear, SelfMod 0x00640FE0) as span records: the per-pixel
 * colour, the texel index walk and the scanline loop, in the integer arithmetic a pixel shader repeats. */
#define BUMP_LINEAR_SCANLINE_VA GVA(0x00640FE0)
/* TTextureLinear (8-bit palettised): the same scanline loop and walk, a byte texel looked up in the CLUT. */
#define TEXTURE_LINEAR_SCANLINE_VA GVA(0x0063DC60)

typedef struct {
    const uint16_t* clut;  /* CClut table, 16-bit screen pixels */
    uint32_t clut_base;    /* (pvClutConversion - table) / 2: fog slice, ramp 0 */
    const uint8_t* bump;   /* au1SubBumpToIntensity: 1024 ramp rows for the light's elevation */
    uint32_t light_theta;  /* u4LightTheta: the light's azimuth in bits 4-9 */
} bump_pixel_t;

typedef struct {
    int32_t step_carry, step; /* iUVInt[0] (a V fraction carried), iUVInt[1] */
    uint32_t du_frac, dv_frac;
    uint32_t tile_mask;   /* u4TextureTileMask: (h - 1) << 9 | (w - 1), or ~0 */
    uint32_t line_pixels; /* the texel page's row length */
} bump_walk_t;

typedef struct {
    uint32_t index, ufrac, vfrac;
} bump_walk_state_t;

typedef struct {
    uint32_t dst_index; /* pixel index from pvScreen */
    int32_t y, x0, n;
    bump_walk_state_t start;
} bump_span_t;

typedef void (*bump_span_sink_t)(void* context, const bump_span_t* span);

/* 0 when a transparent primitive skips the texel (colour 0). */
int bump_texel_rgb565(const bump_pixel_t* c, uint16_t texel, int transparent, uint16_t* out);
/* One pixel of the guest's loop, and k pixels at once (what the shader computes). */
void bump_walk_step(bump_walk_state_t* s, const bump_walk_t* d);
uint32_t bump_walk_index(const bump_walk_state_t* start, const bump_walk_t* d, uint32_t k);
/* The walk the guest set up for this polygon (iUVInt, the fractions, the tile mask, the texel row length). */
void bump_walk_from_guest(bump_walk_t* d);
/* The scanline loop of 0x00640FE0 for DrawSubtriangle(pscan, pdtri): one span per drawn row, and pscan/pdtri left
 * as the native code leaves them. */
void bump_linear_spans(uint32_t pscan, uint32_t pdtri, const bump_walk_t* d, bump_span_sink_t sink, void* context);
/* Pixel x of a span as the HLSL ps_bump_span computes it, from the same tables; 0x10000 when not written. */
uint32_t bump_shader_pixel(const bump_span_t* span, int32_t x, const bump_walk_t* d, const uint16_t* texels,
                           const bump_pixel_t* c, int transparent);

/* The 8-bit textured pixel as the shader computes it: clut[clut_base + texel]. */
uint32_t texture_shader_pixel(const bump_span_t* span, int32_t x, const bump_walk_t* d, const uint8_t* texels,
                              const uint16_t* clut, uint32_t clut_base);

#endif
