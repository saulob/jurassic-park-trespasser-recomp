#ifndef JPT_VK_RENDERER_H
#define JPT_VK_RENDERER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "jpt_render_seam.h"
#include "jpt_video_pass.h"
#include "vk_context.h"
#include "vk_policy.h"
#include "vk_shaders.h"
#include "vk_swapchain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpt_vk_renderer_s jpt_vk_renderer_t;

typedef struct {
    struct SDL_Window* window; /* SDL window (Linux); NULL for native_hwnd or headless */
    void* native_hwnd;         /* Win32 HWND on Windows; NULL for SDL window or headless */
    const char* gpu_request;
    const char* cache_path;
    int enable_validation;
    jpt_render_frame_config_t cfg;
    jpt_vk_log_fn log_fn;
    void* log_user;
    int collect_timings;
} jpt_vk_renderer_desc_t;

typedef enum {
    JPT_VK_DRAW_SUBMITTED = 0,
    JPT_VK_SKIP_NO_FRAME,
    JPT_VK_SKIP_INVALID,
    JPT_VK_SKIP_NO_PIPELINE,
    JPT_VK_SKIP_VERTEX_RING,
    JPT_VK_SKIP_INDEX_RING,
    JPT_VK_SKIP_NO_INDICES,
    JPT_VK_SKIP_REASONS
} jpt_vk_draw_result_t;

typedef enum {
    JPT_VK_TEX_OK = 0,
    JPT_VK_TEX_BAD_ARGS,
    JPT_VK_TEX_NO_SLOT,
    JPT_VK_TEX_NO_POOL,
    JPT_VK_TEX_NO_MEMORY,
    JPT_VK_TEX_UPLOAD,
    JPT_VK_TEX_STALE,
    JPT_VK_TEX_ERRORS
} jpt_vk_texture_error_t;

typedef enum { JPT_VK_BREAK_TEXTURE_UPLOAD, JPT_VK_BREAK_POLY_UPLOAD, JPT_VK_BREAK_LAYER_UPLOAD, JPT_VK_BREAK_SYNC, JPT_VK_BREAK_READBACK, JPT_VK_BREAK_WATER, JPT_VK_BREAK_OVERLAY, JPT_VK_BREAK_FRAME_END, JPT_VK_BREAK_OTHER, JPT_VK_BREAK_COUNT } jpt_vk_pass_break_t;

typedef enum { JPT_VK_OS_UPLOAD, JPT_VK_OS_POLYGON, JPT_VK_OS_RESOURCE_INIT, JPT_VK_OS_READBACK, JPT_VK_OS_COUNT } jpt_vk_one_shot_reason_t;

typedef struct {
    uint64_t draws;
    uint64_t triangles;
    uint64_t textures_uploaded;
    uint64_t pipelines_created;
    uint64_t frames_presented;
    uint64_t water_batches;
    uint64_t sky_batches;
    uint64_t ao_passes;

    uint64_t draw_calls_requested;
    uint64_t draw_calls_recorded;
    uint64_t draw_calls_merged;
    uint64_t draws_skipped[JPT_VK_SKIP_REASONS];
    uint64_t pipeline_cache_hits;
    uint64_t textures_created;
    uint64_t standalone_textures_created;
    uint64_t texture_updates;
    uint64_t texture_upload_bytes;
    uint64_t texture_failures[JPT_VK_TEX_ERRORS];
    uint64_t batch_flushes_texture_full;
    uint64_t water_draws;
    uint64_t sky_draws;
    uint64_t textured_draws;
    uint64_t alpha_test_draws;
    uint64_t alpha_blend_draws;
    uint64_t fog_draws;
    uint64_t colorkey_draws;
    uint64_t indexed_draws;
    uint64_t clears;
    uint64_t scene_passes;
    uint64_t one_shot_submits;
    uint64_t uploads_in_frame;
    uint64_t pass_breaks[JPT_VK_BREAK_COUNT];
    uint64_t readbacks_in_frame;
    uint64_t scene_readback_rects;
    uint64_t ui_draws, ui_vertices, ui_indices, ui_texture_binds, ui_texture_uploads, ui_frames;
    double cpu_ui_ms;
    uint64_t layer_rect_uploads, layer_rect_bytes, layer_rects;
    uint64_t upload_fallback_no_frame, upload_fallback_staging_full, staging_extra_buffers;
    uint64_t one_shot_by_reason[JPT_VK_OS_COUNT];
    double one_shot_ms_by_reason[JPT_VK_OS_COUNT];
    uint64_t descriptor_updates;
    uint64_t descriptor_cache_hits;
    uint64_t pools_reclaimed;
    uint64_t overlays_drawn;
    uint64_t overlay_batches;
    uint64_t scene_readbacks;
    uint64_t scene_readback_pixels;
    uint64_t frame_syncs;
    uint64_t last_frame_copies;
    uint64_t poly_draws;
    uint64_t poly_uploads;
    uint64_t poly_upload_bytes;
    uint64_t poly_failures;
    uint64_t last_frame_readbacks;
    uint64_t cull_draws[4];
    uint64_t zfunc_draws[9];
    uint64_t alpha_func_draws[9];
    uint64_t src_blend_draws[14];
    uint64_t dst_blend_draws[14];
    uint64_t prim_draws[7];

    double cpu_frame_ms;
    double cpu_draw_ms;
    double cpu_pipeline_ms;
    double cpu_upload_ms;
    double cpu_descriptor_ms;
    double cpu_submit_ms;
    double cpu_present_ms;
    double cpu_acquire_ms;
    double cpu_sync_ms;
    double cpu_sync_max_ms;
    double cpu_one_shot_ms;
    double cpu_one_shot_max_ms;
    double cpu_readback_ms;
    double cpu_readback_max_ms;
    double gpu_scene_ms;
    double gpu_ao_ms;
    double gpu_layers_ms;
    double gpu_present_ms;
    double gpu_post_ms;
    double gpu_total_ms;
    uint64_t gpu_samples;
    uint64_t gpu_frames_unsampled;
} jpt_vk_renderer_stats_t;

typedef struct {
    int native_res;          /* 0 = scene_resolved, 1 = swapchain present pass */
    int blend_mode;          /* 0 = opaque, 1 = straight alpha, 2 = premultiplied alpha */
    int apply_gamma;         /* 0 = copy.frag, 1 = present.frag (gamma LUT) */
    int point_filter;        /* 0 = linear clamp, 1 = nearest clamp */
    const float* norm_rect;  /* NULL = full [0,0,1,1] */
} jpt_vk_layer_opts_t;

jpt_vk_renderer_t* jpt_vk_renderer_create(const jpt_vk_renderer_desc_t* desc, char* err, size_t err_cap);
void jpt_vk_renderer_destroy(jpt_vk_renderer_t* r);

const jpt_vk_context_t* jpt_vk_renderer_context(const jpt_vk_renderer_t* r);
const jpt_vk_lifetime_t* jpt_vk_renderer_lifetime(const jpt_vk_renderer_t* r);
void jpt_vk_renderer_get_stats(const jpt_vk_renderer_t* r, jpt_vk_renderer_stats_t* out);
void jpt_vk_renderer_stats_add(jpt_vk_renderer_stats_t* total, const jpt_vk_renderer_stats_t* add);
long jpt_vk_renderer_validation_errors(const jpt_vk_renderer_t* r);
long jpt_vk_renderer_validation_warnings(const jpt_vk_renderer_t* r);
int jpt_vk_renderer_validation_enabled(const jpt_vk_renderer_t* r);
void jpt_vk_renderer_get_geometry(const jpt_vk_renderer_t* r, jpt_vk_frame_geometry_t* out);
int jpt_vk_renderer_is_device_lost(const jpt_vk_renderer_t* r);

void jpt_vk_renderer_on_resize(jpt_vk_renderer_t* r, uint32_t window_w, uint32_t window_h);
void jpt_vk_renderer_set_present_options(jpt_vk_renderer_t* r, int vsync, int fullscreen, int stretch);
void jpt_vk_renderer_set_guest_size(jpt_vk_renderer_t* r, uint32_t guest_w, uint32_t guest_h);
int jpt_vk_renderer_set_internal_size(jpt_vk_renderer_t* r, uint32_t internal_w, uint32_t internal_h);
void jpt_vk_renderer_set_debug_output(jpt_vk_renderer_t* r, uint32_t debug_flags);
void jpt_vk_renderer_set_gamma_ramp(jpt_vk_renderer_t* r, const uint16_t red[256], const uint16_t green[256],
                                    const uint16_t blue[256]);
uint32_t jpt_vk_renderer_active_msaa(const jpt_vk_renderer_t* r);
float jpt_vk_renderer_active_anisotropy(const jpt_vk_renderer_t* r);
VkFormat jpt_vk_renderer_swapchain_format(const jpt_vk_renderer_t* r);
VkPresentModeKHR jpt_vk_renderer_present_mode(const jpt_vk_renderer_t* r);

/* Texture management */
uint64_t jpt_vk_renderer_create_texture(jpt_vk_renderer_t* r, const jpt_render_tex_desc_t* desc, const void* pixels);
jpt_vk_texture_error_t jpt_vk_renderer_texture_error(const jpt_vk_renderer_t* r);
const char* jpt_vk_texture_error_name(jpt_vk_texture_error_t e);
const char* jpt_vk_skip_reason_name(jpt_vk_draw_result_t r);
int jpt_vk_renderer_texture_alive(const jpt_vk_renderer_t* r, uint64_t tex_id);
int jpt_vk_renderer_update_texture(jpt_vk_renderer_t* r, uint64_t tex_id, const jpt_render_tex_desc_t* desc,
                                   const void* pixels, const jpt_render_rect_t* dirty_rect);
void jpt_vk_renderer_destroy_texture(jpt_vk_renderer_t* r, uint64_t tex_id);

/* Frame lifecycle & 3D submission */
int jpt_vk_renderer_begin_frame(jpt_vk_renderer_t* r);
void jpt_vk_renderer_clear(jpt_vk_renderer_t* r, int clear_color, uint32_t argb, int clear_depth, float depth,
                           int clear_stencil, uint32_t stencil);
void jpt_vk_renderer_set_viewport(jpt_vk_renderer_t* r, int32_t x, int32_t y, uint32_t w, uint32_t h);
void jpt_vk_renderer_set_scissor(jpt_vk_renderer_t* r, int enabled, int32_t x, int32_t y, uint32_t w, uint32_t h);

#define JPT_VK_WAVE_SLOTS 4u
/* The wave field the next set_water_params uploads to and the water draws after it sample. */
void jpt_vk_renderer_select_waves(jpt_vk_renderer_t* r, uint32_t slot);
void jpt_vk_renderer_set_water_params(jpt_vk_renderer_t* r, float strength, float rhw_metre, float vp_u0, float vp_v0,
                                      float vp_u1, float vp_v1, const float* waves_rg, uint32_t waves_w,
                                      uint32_t waves_h);
void jpt_vk_renderer_set_sky_params(jpt_vk_renderer_t* r, float cam_u, float cam_v, float plane_h, float fog_lo,
                                    float fog_hi);
void jpt_vk_renderer_set_ao_params(jpt_vk_renderer_t* r, int level, float tan_half_h, int gameplay);

jpt_vk_draw_result_t jpt_vk_renderer_draw(jpt_vk_renderer_t* r, const jpt_render_state_t* state, jpt_render_prim_t prim,
                                          const jpt_render_vertex_t* verts, uint32_t vert_count,
                                          const uint16_t* indices16, const uint32_t* indices32, uint32_t index_count);

/* 2D / Overlay / Frontend HD / Loading HD / Video RGBA layer submission */
void jpt_vk_renderer_submit_layer_rgba(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                       uint32_t src_w, uint32_t src_h, int native_res, int alpha_blend);
void jpt_vk_renderer_submit_layer_rect_rgba(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                            uint32_t src_w, uint32_t src_h, int native_res, int alpha_blend,
                                            const float norm_rect[4]);
void jpt_vk_renderer_submit_layer_ex(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                     uint32_t src_w, uint32_t src_h, const jpt_vk_layer_opts_t* opts);
void jpt_vk_renderer_update_layer_rects(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba, uint32_t full_w,
                                        uint32_t full_h, const jpt_render_rect_t* rects, uint32_t count,
                                        const jpt_vk_layer_opts_t* opts);
void jpt_vk_renderer_update_layer_rect_ex(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                          uint32_t full_w, uint32_t full_h, const jpt_render_rect_t* dirty,
                                          const jpt_vk_layer_opts_t* opts);
typedef struct {
    float clip[4];
    uint32_t idx_offset, idx_count;
    int32_t vtx_offset;
    uint32_t texture;
} jpt_vk_ui_cmd_t;

typedef struct {
    uint32_t id, width, height, bytes_per_pixel;
    const uint8_t* pixels;
    uint32_t revision;
} jpt_vk_ui_texture_t;

typedef struct {
    float display_pos[2], display_size[2];
    uint32_t target_w, target_h;
    const void* vertices;
    uint32_t vertex_count;
    const uint16_t* indices;
    uint32_t index_count;
    const jpt_vk_ui_cmd_t* cmds;
    uint32_t cmd_count;
    const jpt_vk_ui_texture_t* textures;
    uint32_t texture_count;
} jpt_vk_ui_draw_t;

int jpt_vk_renderer_submit_ui(jpt_vk_renderer_t* r, const jpt_vk_ui_draw_t* draw);
void jpt_vk_renderer_clear_ui(jpt_vk_renderer_t* r);
int jpt_vk_ui_scissor(const float clip[4], uint32_t target_w, uint32_t target_h, int32_t rect_x, int32_t rect_y,
                      uint32_t rect_w, uint32_t rect_h, int32_t out[4]);
void jpt_vk_ui_projection(const float display_pos[2], const float display_size[2], float scale[2], float translate[2]);

void jpt_vk_renderer_clear_layer(jpt_vk_renderer_t* r, jpt_render_layer_t layer);
/* Shows a layer again with the picture it already holds (same size, uploaded at least once); 0 when it holds none. */
int jpt_vk_renderer_reactivate_layer(jpt_vk_renderer_t* r, jpt_render_layer_t layer, uint32_t w, uint32_t h,
                                     const jpt_vk_layer_opts_t* opts);

/* Hybrid CPU/GPU picture: the guest rasterises some polygons itself into its 16-bit copy of the render target.
 * Reads a box of the current scene (legacy surface coordinates) back as RGB565 into dst (pitch in pixels);
 * submits and waits for everything recorded so far. */
int jpt_vk_renderer_sync_frame(jpt_vk_renderer_t* r);
/* Up to 128 rectangles in one submit; their RGB565 pixels packed one rectangle after the other, each at its own width. */
int jpt_vk_renderer_read_scene_rects_rgb565(jpt_vk_renderer_t* r, const jpt_render_rect_t* rects, uint32_t count,
                                            uint32_t legacy_w, uint32_t legacy_h, uint16_t* dst);
int jpt_vk_renderer_read_scene_rgb565(jpt_vk_renderer_t* r, const jpt_render_rect_t* box, uint32_t legacy_w,
                                      uint32_t legacy_h, uint16_t* dst, size_t dst_pitch_pixels);
/* Draws BGRA texels covering box (legacy coordinates) over the scene at this point of the frame. Opaque texels replace
 * the scene, otherwise the texels' alpha blends them. */
/* Queue an overlay box (converted at once, so the caller may reuse bgra); the flush uploads the boxes and draws them in one scene pass. */
int jpt_vk_renderer_queue_overlay(jpt_vk_renderer_t* r, const uint32_t* bgra, size_t bgra_pitch_pixels, const jpt_render_rect_t* box,
                                  uint32_t legacy_w, uint32_t legacy_h, int opaque);
int jpt_vk_renderer_flush_overlays(jpt_vk_renderer_t* r, uint32_t legacy_w, uint32_t legacy_h);
int jpt_vk_renderer_draw_overlay(jpt_vk_renderer_t* r, const uint32_t* bgra, size_t bgra_pitch_pixels,
                                 const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h, int opaque);

/* Ends scene, runs MSAA resolve, AO, overlays, and presents to swapchain (or headless target). */
int jpt_vk_renderer_end_and_present(jpt_vk_renderer_t* r);

/* Readback helpers (save previews, backbuffer Lock coherence, headless visual parity tests). */
int jpt_vk_renderer_readback_rgba(jpt_vk_renderer_t* r, uint8_t* dst_rgba, uint32_t* out_w, uint32_t* out_h,
                                  size_t dst_bytes);
int jpt_vk_renderer_readback_rgb565(jpt_vk_renderer_t* r, uint16_t* dst_565, uint32_t dst_w, uint32_t dst_h);

/* The Direct3D 11 video passes on the GPU: decoder codes to colours (rows of half-height files rebuilt), deblocking,
 * the palette's dither smoothed, the side glow, and the Catmull-Rom scale onto the window. The final pass is drawn by
 * the next end_and_present over the whole window. */
typedef struct {
    const uint8_t* rows; /* the first row of picture, 16-bit codes */
    long stride;         /* bytes between rows */
    const uint32_t* palette_map; /* 256 x 256 BGRA, or NULL */
    int palette_changed;
    const uint16_t* gamma; /* red, green, blue ramps of 256, or NULL */
    int smooth, deblock;
    jpt_video_params_t params; /* gamma_on is the renderer's */
} jpt_vk_video_frame_t;
int jpt_vk_renderer_video_draw(jpt_vk_renderer_t* r, const jpt_vk_video_frame_t* f);

/* Perspective-textured polygons the guest rasterises itself, drawn by the GPU from the guest's 8-bit texels and CLUTs
 * (or 16-bit bump texels and their tables): four buffers, one pixel shader per kind, the polygon's fan from constants. */
#define JPT_VK_POLY_MAX_VERTICES 64u
#define JPT_VK_POLY_TEXEL_BYTES (32u << 20)
#define JPT_VK_POLY_TABLE_BYTES (1u << 20)
#define JPT_VK_POLY_BUMP_CLUT_BYTES (4u << 20)
typedef enum {
    JPT_VK_POLY_TEXELS,
    JPT_VK_POLY_CLUT,
    JPT_VK_POLY_BUMP_TABLES,
    JPT_VK_POLY_BUMP_CLUT,
    JPT_VK_POLY_POOLS
} jpt_vk_poly_pool_t;
typedef struct {
    float xy[JPT_VK_POLY_MAX_VERTICES / 2][4];
    float u[4], v[4], w[4];
    float origin[4], size[4], clamp[4];
    uint32_t tex[4], lut[4], bump[4];
} jpt_vk_poly_constants_t;
/* Creates the buffers, layout and pipelines on first use; 0 when the device cannot. */
int jpt_vk_renderer_poly_ready(jpt_vk_renderer_t* r);
int jpt_vk_renderer_poly_upload(jpt_vk_renderer_t* r, jpt_vk_poly_pool_t pool, uint32_t offset, const void* data,
                                uint32_t bytes);
/* One polygon of count vertices over the whole scene target; 0 when it cannot be recorded (the caller draws it on the
 * CPU). */
int jpt_vk_renderer_poly_draw(jpt_vk_renderer_t* r, int bump, const jpt_vk_poly_constants_t* c, unsigned count);

/* The last presented frame's scene (before ambient occlusion and the host layers), kept on the GPU at every present:
 * it reads back as RGB565 at the guest's size whatever the current frame is doing, with no device-wide wait. */
int jpt_vk_renderer_last_frame_valid(const jpt_vk_renderer_t* r);
int jpt_vk_renderer_read_last_frame_rgb565(jpt_vk_renderer_t* r, uint16_t* dst, uint32_t dst_w, uint32_t dst_h,
                                           size_t dst_pitch_pixels);

#ifdef __cplusplus
}
#endif

#endif
