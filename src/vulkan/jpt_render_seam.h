#ifndef JPT_RENDER_SEAM_H
#define JPT_RENDER_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_lit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float sx, sy, sz, rhw;
    uint32_t diffuse;
    uint32_t specular;
    float tu, tv;
} jpt_render_vertex_t;

typedef enum {
    JPT_PRIM_POINTLIST = 1,
    JPT_PRIM_LINELIST = 2,
    JPT_PRIM_LINESTRIP = 3,
    JPT_PRIM_TRIANGLELIST = 4,
    JPT_PRIM_TRIANGLESTRIP = 5,
    JPT_PRIM_TRIANGLEFAN = 6
} jpt_render_prim_t;

typedef enum {
    JPT_TEXFMT_RGB565 = 0,
    JPT_TEXFMT_ARGB1555 = 1,
    JPT_TEXFMT_XRGB1555 = 2,
    JPT_TEXFMT_ARGB4444 = 3,
    JPT_TEXFMT_RGB888 = 4,
    JPT_TEXFMT_XRGB8888 = 5,
    JPT_TEXFMT_ARGB8888 = 6,
    JPT_TEXFMT_PAL8 = 7,
    JPT_TEXFMT_L8 = 8,
    JPT_TEXFMT_XRGB4444 = 9,
    JPT_TEXFMT_BGRA8888_PREPARED = 10
} jpt_render_tex_format_t;

typedef enum {
    JPT_LAYER_WORLD_3D = 0,
    JPT_LAYER_VIDEO = 1,
    JPT_LAYER_VIDEO_SUBTITLES = 2,
    JPT_LAYER_FRONTEND_HD = 3,
    JPT_LAYER_LOADING_HD = 4,
    JPT_LAYER_HOST_OVERLAYS = 5,
    JPT_LAYER_CURSOR = 6,
    JPT_LAYER_PRESENT = 7,
    JPT_LAYER_COUNT = 8
} jpt_render_layer_t;

typedef enum {
    JPT_TEX_FILTER_ORIGINAL = 0,
    JPT_TEX_FILTER_POINT = 1,
    JPT_TEX_FILTER_BILINEAR = 2,
    JPT_TEX_FILTER_TRILINEAR = 3,
    JPT_TEX_FILTER_ANISOTROPIC = 4
} jpt_render_filter_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    jpt_render_tex_format_t format;
    int has_colorkey;
    int standalone; /* an image of its own, not a slice of a shared one */
    uint32_t colorkey_lo;
    uint32_t colorkey_hi;
    const uint32_t* palette;
    int dynamic_hint;
    int has_alpha; /* only for JPT_TEXFMT_BGRA8888_PREPARED: the texels' alpha is meaningful */
    uint32_t mip_levels; /* above 1: BGRA8888_PREPARED chain, level 0 first, each level packed after the one before */
} jpt_render_tex_desc_t;

typedef struct {
    uint32_t z_enable;
    uint32_t z_write;
    uint32_t z_func;
    uint32_t z_bias;
    uint32_t alpha_blend;
    uint32_t src_blend;
    uint32_t dst_blend;
    uint32_t alpha_test;
    uint32_t alpha_ref;
    uint32_t alpha_func;
    uint32_t fog_enable;
    uint32_t fog_color;
    uint32_t specular_enable;
    uint32_t shade_mode;
    uint32_t cull_mode;
    uint32_t fill_mode;
    uint32_t colorkey_enable;
    uint32_t tex_map_blend;
    uint32_t tex_addr_u;
    uint32_t tex_addr_v;
    uint32_t tex_min_filter;
    uint32_t tex_mag_filter;
    uint32_t tex_mip_filter;
    uint32_t stencil_mark;
    uint32_t water_mode;
    uint32_t sky_mode;
    int has_texture;
    uint64_t texture_id;
    uint64_t normal_id;         /* with lit: the normal map */
    const jpt_lit_draw_t* lit;  /* NULL: the vertices' own colours */
} jpt_render_state_t;

typedef struct {
    uint32_t guest_w, guest_h;
    uint32_t internal_w, internal_h;
    uint32_t window_w, window_h;
    int aspect_idx;
    int stretch;
    int vsync;
    uint32_t msaa;
    int texture_filter;
    uint32_t max_anisotropy;
    int ao_level;
    int water_refraction;
    int enhanced_sky;
    int max_object_detail;
    int fov_degrees;
} jpt_render_frame_config_t;

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} jpt_render_rect_t;

#ifdef __cplusplus
}
#endif

#endif
