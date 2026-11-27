#ifndef TRESPASSER_D3D_TRANSLATE_H
#define TRESPASSER_D3D_TRANSLATE_H

/* Direct3D 6 -> Direct3D 11 translation that needs neither Windows nor a device, so tests run it headless. */

#include <stddef.h>
#include <stdint.h>

enum {
    D3DT_PT_POINTLIST = 1,
    D3DT_PT_LINELIST = 2,
    D3DT_PT_LINESTRIP = 3,
    D3DT_PT_TRIANGLELIST = 4,
    D3DT_PT_TRIANGLESTRIP = 5,
    D3DT_PT_TRIANGLEFAN = 6
};

unsigned d3dt_primitive_count(int type, unsigned vertices);
/* Triangle lists for D3D11; first is the batch index of the primitive's vertex 0. The provoking
 * (first) vertex of each triangle matches Direct3D's flat-shading vertex for that primitive type. */
unsigned d3dt_fan_indices(uint32_t first, unsigned count, uint32_t* out);
unsigned d3dt_strip_indices(uint32_t first, unsigned count, uint32_t* out);
unsigned d3dt_list_indices(uint32_t first, unsigned count, uint32_t* out);
unsigned d3dt_indexed_triangles(int type, const uint16_t* in, unsigned count, uint32_t base, uint32_t* out);

/* Render and texture stage state values; 0 means the value is not valid for Direct3D 6. */
int d3dt_blend(uint32_t value, int alpha_channel);
int d3dt_blend_pair(uint32_t src, uint32_t dst, int* src11, int* dst11, int* src_alpha11, int* dst_alpha11);
/* The same pair for a target whose alpha is the fog visibility: the shader's alpha is the second source and the
 * destination's reads 1, as in RGB565; visibility mixes as colour does, or stays when no factor says how. */
int d3dt_ao_blend_pair(uint32_t src, uint32_t dst, int* src11, int* dst11, int* src_alpha11, int* dst_alpha11);
int d3dt_compare(uint32_t value);
int d3dt_address(uint32_t value);
int d3dt_cull(uint32_t value);
int d3dt_fill(uint32_t value);
int d3dt_mag_linear(uint32_t value);
int d3dt_min_linear(uint32_t value);
int d3dt_mip_mode(uint32_t value);
uint32_t d3dt_alpha_ref(uint32_t value);

const char* d3dt_render_state_name(uint32_t state);
const char* d3dt_stage_state_name(uint32_t type);
/* D3DRENDERSTATE_ZBIAS (0..16) as a D3D11 DepthBias for a 24-bit buffer, towards the viewer of zfunc:
 * 84 units of 2^-24 per step, the 5e-6 per step Direct3D 7-era drivers (and wined3d) apply. */
int d3dt_depth_bias(uint32_t zbias, uint32_t zfunc);

/* The Direct3D 6 legacy texture blend (D3DRENDERSTATE_TEXTUREMAPBLEND) on 0xAARRGGBB colours, the
 * reference the pixel shader follows; texture_alpha says whether the texture format has alpha. */
uint32_t d3dt_texture_blend(uint32_t mode, uint32_t texel, uint32_t diffuse, int texture_alpha);
int d3dt_texture_blend_known(uint32_t mode);
/* The pixel shader's per-draw constants (cbuffer Draw of d3d11_shaders.c) for a device's render states. */
typedef struct {
    uint32_t flags, blend_mode, alpha_func;
    float alpha_ref;
    float fog[4];
} d3dt_draw_constants_t;
void d3dt_draw_constants(const uint32_t* render, d3dt_draw_constants_t* out);
/* Vertex fog: fog_alpha (specular alpha) 255 keeps the colour, 0 gives the fog colour; alpha kept. */
uint32_t d3dt_apply_fog(uint32_t color, uint32_t fog_color, uint32_t fog_alpha);

enum {
    D3DT_FVF_POSITION_MASK = 0x00E,
    D3DT_FVF_XYZRHW = 0x004,
    D3DT_FVF_DIFFUSE = 0x040,
    D3DT_FVF_SPECULAR = 0x080,
    D3DT_FVF_TEXCOUNT_MASK = 0xF00,
    D3DT_FVF_TEXCOUNT_SHIFT = 8
};
/* Byte offsets of a pre-transformed vertex; 0 when the format is not XYZRHW-only. */
int d3dt_fvf_layout(uint32_t fvf, unsigned* stride, unsigned* diffuse, unsigned* specular, unsigned* uv);

/* D3DCMPFUNC on depth values. */
int d3dt_depth_test(uint32_t func, float incoming, float stored);
float d3dt_signed_area(const float* x, const float* y);
/* D3DCULL_NONE/CW/CCW (1/2/3) on a screen-space triangle: 1 when it is removed. */
int d3dt_triangle_culled(uint32_t cull, const float* x, const float* y);
/* Barycentric weights of (px, py); 1 when the point is inside the triangle. */
int d3dt_barycentric(const float* x, const float* y, float px, float py, float w[3]);
void d3dt_perspective_uv(const float* u, const float* v, const float* rhw, const float w[3], float* out_u, float* out_v);
/* NaN or infinite. */
int d3dt_float_bad(float f);

/* The render and stage-0 states that decide how a draw comes out. */
typedef struct {
    uint32_t textured, tmb, blend_enable, src_blend, dst_blend, alpha_test, alpha_func, alpha_ref, fog, fog_color,
        specular, shade, zenable, zwrite, zfunc, cull, fill, colorkey, address_u, address_v, mag, min;
} d3dt_draw_state_t;
uint32_t d3dt_state_fingerprint(const d3dt_draw_state_t* s);
void d3dt_describe_state(const d3dt_draw_state_t* s, char* out, unsigned cap);

enum {
    D3DT_FMT_UNKNOWN = 0,
    D3DT_FMT_RGB565,
    D3DT_FMT_XRGB1555,
    D3DT_FMT_ARGB1555,
    D3DT_FMT_ARGB4444,
    D3DT_FMT_XRGB8888,
    D3DT_FMT_ARGB8888,
    D3DT_FMT_XRGB4444,
    D3DT_FMT_PAL8,
    D3DT_FMT_COUNT
};

/* DDPIXELFORMAT.dwFlags bits the translation reads. */
enum { D3DT_PF_ALPHAPIXELS = 0x1, D3DT_PF_PALETTEINDEXED8 = 0x20, D3DT_PF_RGB = 0x40 };

int d3dt_format_from_masks(uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
/* The whole DDPIXELFORMAT: palettised 8-bit as well as the RGB layouts; the alpha mask counts only
 * with DDPF_ALPHAPIXELS. */
int d3dt_format_from_pixelformat(uint32_t flags, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
int d3dt_format_has_alpha(int format);
int d3dt_format_bytes(int format);
const char* d3dt_format_name(int format);
/* To B8G8R8A8 as 0xAARRGGBB; texels inside the key range keep their colour with alpha 0. PAL8
 * texels index palette (0xAARRGGBB entries); other formats ignore it. */
void d3dt_convert_row(int format, const void* src, uint32_t* dst, unsigned count, int keyed, uint32_t key_low,
                      uint32_t key_high);
/* The per-texel loop d3dt_convert_row, d3dt_alpha_kind and d3dt_mip_downsample must match byte for byte. */
void d3dt_convert_row_reference(int format, const void* src, uint32_t* dst, unsigned count, int keyed, uint32_t key_low,
                                uint32_t key_high);
int d3dt_alpha_kind_reference(const uint32_t* bgra, size_t count);
void d3dt_mip_downsample_reference(const uint32_t* src, unsigned w, unsigned h, int alpha_kind, uint32_t* dst);
void d3dt_convert_row_pal8(const uint8_t* src, uint32_t* dst, unsigned count, const uint32_t* palette, int keyed,
                           uint32_t key_low, uint32_t key_high);
uint32_t d3dt_rgb565_to_bgra(uint16_t v);
uint16_t d3dt_bgra_to_rgb565(uint32_t v);

/* Changed pixels become opaque overlay texels, the rest transparent; returns how many changed. */
unsigned d3dt_overlay_row(const uint16_t* current, const uint16_t* pristine, unsigned count, uint32_t* out);
/* Pixels the CPU turned black where the previous picture was not: what could paint the scene black. */
unsigned d3dt_black_over_content(const uint16_t* current, const uint16_t* previous, unsigned count);
/* d3dt_overlay_row, d3dt_black_over_content and the copy of the row into keep, in one pass; adds the black count. */
unsigned d3dt_overlay_row_sync(const uint16_t* current, uint16_t* keep, unsigned count, uint32_t* out, unsigned long* black);
/* First and one-past-last differing pixel of a row; 0 when the rows are equal. */
int d3dt_row_span(const uint16_t* current, const uint16_t* pristine, unsigned count, unsigned* first, unsigned* end);

typedef struct {
    long left, top, right, bottom;
} d3dt_rect_t;

/* Where the CPU wrote, as up to D3DT_DIRTY_MAX separate boxes. Boxes closer than D3DT_DIRTY_GAP pixels are one box:
 * each composite also rewrites a 1-pixel apron, which must never reach another box. */
#define D3DT_DIRTY_MAX 8u
#define D3DT_DIRTY_GAP 2
typedef struct {
    d3dt_rect_t r[D3DT_DIRTY_MAX];
    unsigned n;
} d3dt_dirty_t;

d3dt_rect_t d3dt_scale_rect(long x, long y, long w, long h, unsigned legacy_w, unsigned legacy_h, unsigned target_w,
                            unsigned target_h);
int d3dt_rect_empty(const d3dt_rect_t* r);
/* 1 when target row k's pixel centre falls exactly on a texel edge of a legacy-high texture stretched to target
 * rows; point sampling may then take either neighbour. Never at integer scales. */
int d3dt_row_on_texel_edge(unsigned k, unsigned legacy, unsigned target);
/* The texels uploaded for an overlay box: the box and one texel around it, within the surface, so that sampling at
 * the box's edge reads only texels of this upload. */
d3dt_rect_t d3dt_overlay_upload_rect(const d3dt_rect_t* box, unsigned width, unsigned height);
void d3dt_rect_union(d3dt_rect_t* acc, const d3dt_rect_t* add);
/* The legacy pixels a batch of pre-transformed vertices (x, y first, `stride` bytes apart) can reach, with a pixel
 * of margin for rasterisation at the render scale; the whole surface when a coordinate is not finite. */
d3dt_rect_t d3dt_vertex_box(const float* first, unsigned stride, unsigned count, unsigned width, unsigned height);
/* What a readback must read: the whole surface unless the GPU is known to have drawn only inside `drawn` since a full
 * clear; then that box and two pixels around it (filtered calibration reads), or nothing when it is empty. */
d3dt_rect_t d3dt_readback_box(int drawn_known, const d3dt_rect_t* drawn, unsigned width, unsigned height);

int d3dt_parse_resolution(const char* text, unsigned* w, unsigned* h);
void d3dt_auto_resolution(unsigned view_w, unsigned view_h, unsigned legacy_w, unsigned legacy_h, unsigned* w,
                          unsigned* h);
/* supported: bit n set when n samples work; the largest of 16, 8, 4, 2 supported and <= requested, else 1 (off). */
unsigned d3dt_pick_msaa(unsigned requested, uint32_t supported);
/* The largest of 16, 8, 4, 2 within both, else 1. */
unsigned d3dt_pick_anisotropy(unsigned requested, unsigned device_max);

/* Texture filter settings; d3d_legacy.h's D3DR_FILTER_* are these. */
enum { D3DT_FILTER_ORIGINAL, D3DT_FILTER_POINT, D3DT_FILTER_BILINEAR, D3DT_FILTER_TRILINEAR, D3DT_FILTER_ANISOTROPIC };
int d3dt_filter_uses_mips(int filter);

typedef struct {
    unsigned filter;     /* D3D11_FILTER */
    unsigned anisotropy; /* MaxAnisotropy */
    int mips;            /* generated levels are sampled (MaxLOD unbounded); else level 0 only */
} d3dt_sampler_choice_t;
d3dt_sampler_choice_t d3dt_choose_sampler(int filter, int mag_linear, int min_linear, unsigned anisotropy);

/* Generated mipmaps: Direct3D 6 textures come with one level. Levels are packed one after another, level 0 first. */
unsigned d3dt_mip_count(unsigned w, unsigned h);
void d3dt_mip_size(unsigned w, unsigned h, unsigned level, unsigned* mw, unsigned* mh);
size_t d3dt_mip_chain_texels(unsigned w, unsigned h, unsigned levels);
enum { D3DT_ALPHA_OPAQUE, D3DT_ALPHA_BINARY, D3DT_ALPHA_BLENDED };
int d3dt_alpha_kind(const uint32_t* bgra, size_t count);
void d3dt_mip_downsample(const uint32_t* src, unsigned w, unsigned h, int alpha_kind, uint32_t* dst);
void d3dt_build_mips(uint32_t* chain, unsigned w, unsigned h, unsigned levels, int alpha_kind);

/* A texture re-uploaded in D3DT_DYNAMIC_STREAK more presented frames, each within D3DT_DYNAMIC_GAP frames of the
 * one before (water, render-cache sprites, animations), is dynamic: it keeps level 0 only. */
#define D3DT_DYNAMIC_GAP 2u
#define D3DT_DYNAMIC_STREAK 3u
typedef struct {
    unsigned long long last_frame;
    unsigned long uploads;
    unsigned streak;
    int dynamic;
} d3dt_upload_history_t;
/* 1 when this upload made the texture dynamic. */
int d3dt_note_upload(d3dt_upload_history_t* h, unsigned long long frame);

/* D3DMATRIX order: row vectors, out = a * b. */
void d3dt_matrix_multiply(const float* a, const float* b, float* out);

/* A world vertex's layer field (D3D11_LAYER_* in d3d11_shaders.h): slice, the batch's array slot, texture flags and
 * sampler. */
/* A texture map blend of the host's own: MODULATEALPHA with the colour times two. */
#define D3DT_TBLEND_MODULATEALPHA_2X 16
uint32_t d3dt_world_layer(uint32_t slice, uint32_t slot, uint32_t flags, uint32_t sampler);
/* The sampler for stage 0's address modes: wrap or clamp per axis travel in the vertex, anything else is the device's. */
uint32_t d3dt_vertex_sampler(uint32_t address_u, uint32_t address_v);
/* Copies the bytes of src's rows that differ from dst's: 0 when none does, else the changed box in pixels. */
int d3dt_copy_changed(uint8_t* dst, long dst_pitch, const uint8_t* src, long src_pitch, unsigned width, unsigned height,
                      unsigned pixel_bytes, d3dt_rect_t* changed);
/* Adds a written box: merged with every box it touches, overlaps or nearly fills a union with (at most 25% more
 * pixels than both), and into the box it grows least when the list is full. */
void d3dt_dirty_add(d3dt_dirty_t* d, const d3dt_rect_t* add);
/* Whether two boxes are closer than D3DT_DIRTY_GAP pixels on both axes. */
int d3dt_rects_near(const d3dt_rect_t* a, const d3dt_rect_t* b);
/* Whether an address mode change keeps the open batch: both values are wrap or clamp. */
int d3dt_address_joins(uint32_t old_value, uint32_t value);
/* Whether a render state is never read when a batch is drawn (ignored, or baked into each vertex by DrawPrimitive). */
int d3dt_render_state_keeps_batch(uint32_t state);
/* Whether SetRenderState(state, value) over old_value must draw the open batch first. */
int d3dt_render_change_flushes(uint32_t state, uint32_t old_value, uint32_t value);
/* The slot of item among the max a batch binds, taking a free one; -1 when every slot holds another item. */
int d3dt_batch_slot(const void** bound, unsigned* count, unsigned max, const void* item);

#endif
