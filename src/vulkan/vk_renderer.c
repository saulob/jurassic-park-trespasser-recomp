#include "vk_renderer.h"
#include "ao_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_timer.h>

#include "jpt_config.h"

#define JPT_VK_MAX_POOLS 128
#define JPT_VK_WORLD_SAMPLERS 20
#define JPT_VK_POOL_MIN_SLICES 64u
#define JPT_VK_POOL_MAX_SLICES 2048u
#define JPT_VK_POOL_BUDGET (16u * 1024u * 1024u)
#define JPT_VK_POOL_MASK_WORDS (JPT_VK_POOL_MAX_SLICES / 64u)
#define JPT_VK_INIT_TEXTURES 4096u
#define JPT_VK_MAX_PIPELINES 256
#define JPT_VK_STAGING_SIZE (32u * 1024u * 1024u)
#define JPT_VK_STAGING_EXTRA_MAX 8u
#define JPT_VK_READ_RECT_MAX 128u
#define JPT_VK_STAGING_EXTRA_SIZE (64u * 1024u * 1024u)
#define JPT_VK_VB_SIZE (32u * 1024u * 1024u)
#define JPT_VK_IB_SIZE (16u * 1024u * 1024u)
#define JPT_VK_WORLD_POOL_SETS 256u
#define JPT_VK_TS_COUNT 6u
#define JPT_VK_TS_MAX_SEGMENTS 24u
#define JPT_VK_TS_TOTAL (JPT_VK_TS_COUNT + 2u * JPT_VK_TS_MAX_SEGMENTS)
#define JPT_VK_DEPTH_BIAS_UNITS_PER_STEP 84.0f

typedef struct {
    float sx, sy, sz, rhw;
    uint32_t diffuse;
    uint32_t specular;
    float tu, tv;
    uint32_t layer;
} vk_gpu_vertex_t;

typedef struct {
    float target[4];
    uint32_t draw_u[4];
    float fog_color[4];
    float fx_water[4];
    float fx_sky[4];
    float fx_extra[4];
    float fx_water3[4];
} vk_world_push_t;

typedef struct {
    float pass_rect[4];
    float pass_uv[4];
    float pass_color[4];
    float pass_params[4];
    float pass_extra0[4];
    float pass_extra1[4];
    float pass_extra2[4];
} vk_quad_push_t;

typedef struct {
    VkImage image;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    VkFramebuffer fb;
    uint32_t w, h;
} vk_video_target_t;

typedef struct {
    int used;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t max_slices;
    uint32_t live;
    uint64_t slice_mask[JPT_VK_POOL_MASK_WORDS];
    VkImage image;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    VkImageLayout layout;
} vk_tex_pool_t;

typedef struct {
    int alive;
    int standalone;
    uint32_t generation;
    uint32_t pool_slot;
    uint32_t slice;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    int has_alpha;
    int has_colorkey;
    VkImage image;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    VkImageLayout layout;
} vk_tex_slot_t;

typedef struct {
    VkImage image;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    uint64_t retire_seq;
} vk_retired_img_t;

typedef struct {
    int used;
    jpt_vk_pipeline_key_t key;
    uint64_t hash;
    VkPipeline pipeline;
} vk_pipe_entry_t;

typedef struct {
    int active;
    int native_res;
    int blend_mode;   /* 0 = opaque, 1 = straight, 2 = premultiplied */
    int apply_gamma;  /* 0 = copy.frag, 1 = present.frag */
    int point_filter; /* 0 = linear, 1 = nearest */
    float norm_rect[4]; /* x0, y0, x1, y1 in [0..1] inside target/present rect */
    uint32_t w, h;
    VkImage image;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    VkDescriptorSet desc_linear;
    VkDescriptorSet desc_point;
    VkImageLayout layout;
} vk_layer_tex_t;

typedef struct {
    VkBuffer staging_buf;
    jpt_vk_allocation_t staging_alloc;
    VkDeviceSize staging_used;
    VkBuffer extra_buf[JPT_VK_STAGING_EXTRA_MAX];
    jpt_vk_allocation_t extra_alloc[JPT_VK_STAGING_EXTRA_MAX];
    VkDeviceSize extra_used[JPT_VK_STAGING_EXTRA_MAX];
    VkDeviceSize extra_size[JPT_VK_STAGING_EXTRA_MAX];
    uint32_t extra_count;

    VkBuffer vb_buf;
    jpt_vk_allocation_t vb_alloc;
    VkDeviceSize vb_used;

    VkBuffer ib_buf;
    jpt_vk_allocation_t ib_alloc;
    VkDeviceSize ib_used;

    VkDescriptorPool* world_pools;
    uint32_t world_pool_count;
    uint32_t world_pool_cap;
    uint32_t world_pool_cur;
} vk_frame_rings_t;

typedef struct {
    int active;
    VkPipeline pipe;
    VkImageView views[16];
    uint32_t view_count;
    vk_world_push_t push;
    VkViewport vp;
    VkRect2D sc;
    float bias;
    VkDeviceSize vb_off;
    VkDeviceSize ib_off;
    uint32_t verts;
    uint32_t indices;
} vk_batch_t;

typedef struct {
    VkImageView views[16];
    VkImageView water;
    VkImageView behind;
    VkDescriptorSet ds;
} vk_world_ds_entry_t;

#define JPT_VK_WORLD_DS_CACHE 48u

#define JPT_VK_UI_TEXTURES 8u

typedef struct {
    uint32_t id, w, h, revision;
    VkImage img;
    jpt_vk_allocation_t alloc;
    VkImageView view;
    VkImageLayout layout;
} vk_ui_tex_t;

struct jpt_vk_renderer_s {
    jpt_vk_context_t ctx;
    jpt_vk_swapchain_t sc;
    jpt_render_frame_config_t cfg;
    jpt_vk_frame_geometry_t geom;
    uint32_t active_msaa;
    float active_aniso;
    int fullscreen;

    VkShaderModule shaders[JPT_VK_SH_COUNT];

    /* Layouts & Descriptor Pools */
    VkDescriptorSetLayout world_dsl;
    VkPipelineLayout world_pl;
    VkDescriptorSetLayout quad_dsl;
    VkPipelineLayout quad_pl;
    VkDescriptorPool desc_pool;
    VkDescriptorSet last_world_ds;
    VkImageView last_world_views[16];
    VkImageView last_water_view;
    VkImageView last_behind_view;
    vk_world_ds_entry_t world_ds_cache[JPT_VK_WORLD_DS_CACHE];
    VkDescriptorSetLayout ui_dsl;
    VkPipelineLayout ui_pl;
    VkPipeline ui_pipe;
    VkRenderPass ui_pipe_pass;
    VkDescriptorPool ui_dpool[JPT_VK_FRAMES_IN_FLIGHT];
    vk_ui_tex_t ui_tex[JPT_VK_UI_TEXTURES];
    VkBuffer ui_vb[JPT_VK_FRAMES_IN_FLIGHT], ui_ib[JPT_VK_FRAMES_IN_FLIGHT];
    jpt_vk_allocation_t ui_vb_alloc[JPT_VK_FRAMES_IN_FLIGHT], ui_ib_alloc[JPT_VK_FRAMES_IN_FLIGHT];
    VkDeviceSize ui_vb_cap[JPT_VK_FRAMES_IN_FLIGHT], ui_ib_cap[JPT_VK_FRAMES_IN_FLIGHT];
    jpt_vk_ui_cmd_t* ui_cmds;
    VkDescriptorSet* ui_cmd_sets;
    uint32_t ui_cmd_count, ui_cmd_cap, ui_frame_index;
    uint32_t ui_target_w, ui_target_h;
    float ui_scale[2], ui_translate[2];
    uint8_t* ui_expand;
    size_t ui_expand_cap;
    int ui_active;
    int world_bound_valid;
    int break_reason;
    uint32_t ov_count;
    uint8_t* ov_rgba[64];
    jpt_render_rect_t ov_box[64];
    uint8_t ov_opaque[64];
    VkPipeline world_bound_pipe;
    VkViewport world_bound_vp;
    VkRect2D world_bound_sc;
    float world_bound_bias;
    VkDescriptorSet world_bound_ds;
    vk_world_push_t world_bound_push;
    uint32_t world_ds_cache_count;
    vk_batch_t batch;
    jpt_vk_texture_error_t tex_error;
    int collect_timings;
    VkQueryPool ts_pool[JPT_VK_FRAMES_IN_FLIGHT];
    int ts_pending[JPT_VK_FRAMES_IN_FLIGHT];
    uint32_t ts_segments_done[JPT_VK_FRAMES_IN_FLIGHT];
    uint32_t ts_segments;
    int ts_invalid_frame;
    double ts_period_ms;
    double frame_start_ms;
    VkDescriptorSet present_ds;
    VkDescriptorSet ao_prep_ds;
    VkDescriptorSet ao_gen_ds;
    VkDescriptorSet ao_blur_h_ds;
    VkDescriptorSet ao_blur_v_ds;
    VkDescriptorSet ao_apply_ds;

    /* Samplers */
    VkSampler world_samplers[JPT_VK_WORLD_SAMPLERS]; /* class * 4 + address: mips, point, min point mag linear, min linear mag point, no mips */
    VkSampler linear_clamp_sampler;
    VkSampler nearest_clamp_sampler;

    /* Render passes */
    VkRenderPass scene_rp_load;
    VkRenderPass scene_rp_clear;
    VkRenderPass resolved_rp_load;
    VkRenderPass r32_rp;
    VkRenderPass r8_rp;

    /* Scene targets at internal_w x internal_h */
    uint32_t rt_w, rt_h;
    VkImage scene_color;
    jpt_vk_allocation_t scene_color_alloc;
    VkImageView scene_color_view;

    VkImage scene_resolved;
    jpt_vk_allocation_t scene_resolved_alloc;
    VkImageView scene_resolved_view;

    VkImage scene_behind;
    jpt_vk_allocation_t scene_behind_alloc;
    VkImageView scene_behind_view;

    VkImage last_img;
    jpt_vk_allocation_t last_alloc;
    VkImageView last_view;
    VkImageLayout last_layout;
    int last_valid;

    VkImage scene_depth;
    jpt_vk_allocation_t scene_depth_alloc;
    VkImageView scene_depth_view;
    VkImageView scene_depth_sample_view;
    VkImageView scene_stencil_view;

    VkFramebuffer scene_fb;
    VkFramebuffer resolved_fb;

    /* AO targets */
    VkImage ao_lin_depth;
    jpt_vk_allocation_t ao_lin_depth_alloc;
    VkImageView ao_lin_depth_view;
    VkFramebuffer ao_lin_depth_fb;

    VkImage ao_raw;
    jpt_vk_allocation_t ao_raw_alloc;
    VkImageView ao_raw_view;
    VkFramebuffer ao_raw_fb;

    VkImage ao_blur_tmp;
    jpt_vk_allocation_t ao_blur_tmp_alloc;
    VkImageView ao_blur_tmp_view;
    VkFramebuffer ao_blur_tmp_fb;

    VkImage ao_blur_out;
    jpt_vk_allocation_t ao_blur_out_alloc;
    VkImageView ao_blur_out_view;
    VkFramebuffer ao_blur_out_fb;

    /* Water waves & Gamma LUT */
    struct {
        VkImage img;
        jpt_vk_allocation_t alloc;
        VkImageView view;
        uint32_t w, h;
        VkImageLayout layout;
    } waves[JPT_VK_WAVE_SLOTS];
    uint32_t wave_slot;
    VkImageView water_waves_view; /* waves[wave_slot]'s, or the zero field's until it has one */

    VkImage gamma_lut_img;
    jpt_vk_allocation_t gamma_lut_alloc;
    VkImageView gamma_lut_view;
    int gamma_dirty;
    uint8_t gamma_rgba[256 * 4];

    /* Texture pools & handles */
    vk_tex_pool_t pools[JPT_VK_MAX_POOLS];
    vk_tex_slot_t* textures;
    uint32_t texture_cap;
    uint32_t next_tex_gen;
    vk_retired_img_t* retired;
    uint32_t retired_count;
    uint32_t retired_cap;
    uint64_t frame_seq;

    /* CPU-drawn picture composited into the scene, and mid-frame readback */
    VkImage overlay_img;
    jpt_vk_allocation_t overlay_alloc;
    VkImageView overlay_view;
    VkImageLayout overlay_layout;
    uint32_t overlay_w, overlay_h;
    VkDescriptorSet overlay_ds_point;
    VkDescriptorSet overlay_ds_linear;
    VkImage rb_img;
    jpt_vk_allocation_t rb_alloc;
    VkImageView rb_view;
    uint32_t rb_w, rb_h;
    VkBuffer rb_buf;
    jpt_vk_allocation_t rb_buf_alloc;
    VkDeviceSize rb_buf_bytes;

    /* Overlays / Frontend / Loading / Video layers */
    vk_layer_tex_t layers[JPT_LAYER_COUNT];

    /* Cached pipelines */
    vk_pipe_entry_t world_pipes[JPT_VK_MAX_PIPELINES];
    VkPipeline present_pipe;
    VkPipeline present_blend_pipe;
    VkPipeline present_premul_pipe;
    VkPipeline copy_blend_pipe;
    VkPipeline copy_premul_pipe;
    VkPipeline copy_opaque_pipe;
    VkPipeline copy_present_blend_pipe;
    VkPipeline copy_present_premul_pipe;
    VkPipeline copy_present_opaque_pipe;
    VkPipeline ao_prepare_pipe;
    VkPipeline ao_generate_pipe;
    VkPipeline ao_blur_pipe;
    VkPipeline ao_apply_pipe;
    VkPipeline overlay_blend_pipe;
    VkPipeline overlay_opaque_pipe;

    /* Per-frame ring buffers */
    vk_frame_rings_t rings[JPT_VK_FRAMES_IN_FLIGHT];

    /* Video passes */
    int video_ready;
    int video_failed;
    int video_active;
    int gamma_identity;
    VkRenderPass video_rp;
    VkDescriptorSetLayout video_dsl;
    VkPipelineLayout video_pl;
    VkDescriptorPool video_dpool;
    VkDescriptorSet video_ds[JPT_VK_FRAMES_IN_FLIGHT][2];
    VkPipeline video_pipe[6];
    vk_video_target_t video_tgt[3];
    VkImage video_codes;
    jpt_vk_allocation_t video_codes_alloc;
    VkImageView video_codes_view;
    VkImageLayout video_codes_layout;
    uint32_t video_codes_w, video_codes_h;
    VkImage video_map;
    jpt_vk_allocation_t video_map_alloc;
    VkImageView video_map_view;
    VkImageLayout video_map_layout;
    int video_map_loaded;
    int video_picture;
    uint16_t video_gamma[768];
    int video_gamma_valid;
    jpt_video_constants_t video_constants;

    /* Perspective-textured polygons drawn by the GPU */
    int poly_ready;
    int poly_failed;
    VkBuffer poly_pool[JPT_VK_POLY_POOLS];
    jpt_vk_allocation_t poly_pool_alloc[JPT_VK_POLY_POOLS];
    VkDescriptorSetLayout poly_dsl;
    VkPipelineLayout poly_pl;
    VkDescriptorPool poly_dpool;
    VkDescriptorSet poly_ds[JPT_VK_FRAMES_IN_FLIGHT];
    VkBuffer poly_ubo[JPT_VK_FRAMES_IN_FLIGHT];
    jpt_vk_allocation_t poly_ubo_alloc[JPT_VK_FRAMES_IN_FLIGHT];
    VkDeviceSize poly_ubo_used, poly_ubo_stride;
    VkPipeline poly_pipe[2];

    /* Active frame state */
    int in_frame;
    int in_scene_pass;
    int scene_has_draws;
    int scene_has_depth_draws;
    int scene_behind_copied;
    int scene_resolved_ready;
    VkCommandBuffer cmd;
    VkFramebuffer present_fb;
    VkExtent2D present_extent;

    /* Dynamic state */
    jpt_render_rect_t viewport;
    int scissor_enabled;
    jpt_render_rect_t scissor;

    /* Effect state */
    float water_strength;
    float water_rhw_metre;
    float water_vp[4];
    float sky_cam_u, sky_cam_v, sky_plane_h, sky_fog_lo, sky_fog_hi;
    int ao_level;
    float ao_tan_half_h;
    int ao_gameplay;
    uint32_t debug_flags;

    jpt_vk_renderer_stats_t stats;
    int startup_logged;
};

static double now_ms(void) {
    static uint64_t freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)freq;
}

static void destroy_poly_resources(jpt_vk_renderer_t* r);
static void destroy_video_resources(jpt_vk_renderer_t* r);

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void cmd_image_barrier(const jpt_vk_loader_t* vk, VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                              VkImageLayout old_layout, VkImageLayout new_layout, uint32_t base_mip, uint32_t mip_count,
                              uint32_t base_layer, uint32_t layer_count) {
    jpt_vk_barrier_plan_t plan;
    VkImageMemoryBarrier b;
    if (old_layout == new_layout) return;
    jpt_vk_plan_image_barrier(old_layout, new_layout, aspect, &plan);
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = plan.src_access;
    b.dstAccessMask = plan.dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = base_mip;
    b.subresourceRange.levelCount = mip_count;
    b.subresourceRange.baseArrayLayer = base_layer;
    b.subresourceRange.layerCount = layer_count;
    vk->vkCmdPipelineBarrier(cmd, plan.src_stage, plan.dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkSampleCountFlagBits msaa_bits(uint32_t samples) {
    if (samples >= 8) return VK_SAMPLE_COUNT_8_BIT;
    if (samples >= 4) return VK_SAMPLE_COUNT_4_BIT;
    if (samples >= 2) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

static VkBlendFactor map_blend_factor(uint32_t d3d) {
    switch (d3d) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkCompareOp map_compare_op(uint32_t d3d) {
    switch (d3d) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkCullModeFlags map_cull_mode(uint32_t d3d) {
    if (d3d == 2u) return VK_CULL_MODE_FRONT_BIT; /* D3DCULL_CW */
    if (d3d == 3u) return VK_CULL_MODE_BACK_BIT;  /* D3DCULL_CCW */
    return VK_CULL_MODE_NONE;
}

static VkPrimitiveTopology map_topology(uint8_t cls) {
    if (cls == 1) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (cls == 2) return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static void convert_region_to_rgba8(const jpt_render_tex_desc_t* desc, const void* src, uint32_t x0, uint32_t y0,
                                    uint32_t rw, uint32_t rh, uint8_t* dst) {
    const uint8_t* bytes = (const uint8_t*)src;
    uint32_t w = desc->width;
    uint32_t pitch = desc->pitch;
    if (!pitch) {
        uint32_t bpp = (desc->format == JPT_TEXFMT_PAL8 || desc->format == JPT_TEXFMT_L8) ? 1u
                     : (desc->format == JPT_TEXFMT_RGB888) ? 3u
                     : (desc->format == JPT_TEXFMT_ARGB8888 || desc->format == JPT_TEXFMT_XRGB8888 ||
                      desc->format == JPT_TEXFMT_BGRA8888_PREPARED) ? 4u
                     : 2u;
        pitch = w * bpp;
    }

    for (uint32_t y = y0; y < y0 + rh; y++) {
        const uint8_t* row = bytes + (size_t)y * pitch;
        uint8_t* out = dst + (size_t)(y - y0) * rw * 4u;
        for (uint32_t x = x0; x < x0 + rw; x++) {
            uint8_t r = 255, g = 255, b = 255, a = 255;
            uint32_t raw = 0;
            switch (desc->format) {
            case JPT_TEXFMT_RGB565: {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                raw = p;
                uint32_t r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
                r = (uint8_t)((r5 << 3) | (r5 >> 2));
                g = (uint8_t)((g6 << 2) | (g6 >> 4));
                b = (uint8_t)((b5 << 3) | (b5 >> 2));
                break;
            }
            case JPT_TEXFMT_ARGB1555: {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                raw = p;
                uint32_t r5 = (p >> 10) & 31u, g5 = (p >> 5) & 31u, b5 = p & 31u;
                r = (uint8_t)((r5 << 3) | (r5 >> 2));
                g = (uint8_t)((g5 << 3) | (g5 >> 2));
                b = (uint8_t)((b5 << 3) | (b5 >> 2));
                a = (p & 0x8000u) ? 255u : 0u;
                break;
            }
            case JPT_TEXFMT_XRGB1555: {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                raw = p & 0x7FFFu;
                uint32_t r5 = (p >> 10) & 31u, g5 = (p >> 5) & 31u, b5 = p & 31u;
                r = (uint8_t)((r5 << 3) | (r5 >> 2));
                g = (uint8_t)((g5 << 3) | (g5 >> 2));
                b = (uint8_t)((b5 << 3) | (b5 >> 2));
                break;
            }
            case JPT_TEXFMT_ARGB4444: {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                raw = p;
                uint32_t a4 = (p >> 12) & 15u, r4 = (p >> 8) & 15u, g4 = (p >> 4) & 15u, b4 = p & 15u;
                a = (uint8_t)((a4 << 4) | a4);
                r = (uint8_t)((r4 << 4) | r4);
                g = (uint8_t)((g4 << 4) | g4);
                b = (uint8_t)((b4 << 4) | b4);
                break;
            }
            case JPT_TEXFMT_XRGB4444: {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                uint32_t r4 = (p >> 8) & 15u, g4 = (p >> 4) & 15u, b4 = p & 15u;
                raw = p & 0x0FFFu;
                r = (uint8_t)((r4 << 4) | r4);
                g = (uint8_t)((g4 << 4) | g4);
                b = (uint8_t)((b4 << 4) | b4);
                break;
            }
            case JPT_TEXFMT_BGRA8888_PREPARED:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
                raw = 0xFFFFFFFFu;
                break;
            case JPT_TEXFMT_RGB888:
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                raw = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                break;
            case JPT_TEXFMT_XRGB8888:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                raw = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                break;
            case JPT_TEXFMT_ARGB8888:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
                raw = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                break;
            case JPT_TEXFMT_PAL8: {
                uint8_t idx = row[x];
                raw = idx;
                if (desc->palette) {
                    uint32_t c = desc->palette[idx];
                    r = (uint8_t)(c & 0xFFu);
                    g = (uint8_t)((c >> 8) & 0xFFu);
                    b = (uint8_t)((c >> 16) & 0xFFu);
                } else {
                    r = g = b = idx;
                }
                break;
            }
            case JPT_TEXFMT_L8:
                r = g = b = row[x];
                raw = row[x];
                break;
            }
            if (desc->has_colorkey && desc->format != JPT_TEXFMT_BGRA8888_PREPARED && raw >= desc->colorkey_lo &&
                raw <= desc->colorkey_hi)
                a = 0;
            out[(x - x0) * 4 + 0] = r;
            out[(x - x0) * 4 + 1] = g;
            out[(x - x0) * 4 + 2] = b;
            out[(x - x0) * 4 + 3] = a;
        }
    }
}

static void convert_pixels_to_rgba8(const jpt_render_tex_desc_t* desc, const void* src, uint8_t* dst) {
    convert_region_to_rgba8(desc, src, 0, 0, desc->width, desc->height, dst);
}

static int run_one_shot_cmd(jpt_vk_renderer_t* r, void (*fn)(jpt_vk_renderer_t*, VkCommandBuffer, void*), void* user,
                            jpt_vk_one_shot_reason_t reason) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci;
    VkCommandBufferAllocateInfo cbai;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci;
    double wait_start;

    memset(&cpci, 0, sizeof cpci);
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = r->ctx.graphics_family;
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vk->vkCreateCommandPool(r->ctx.device, &cpci, NULL, &pool) != VK_SUCCESS) return 0;

    memset(&cbai, 0, sizeof cbai);
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vk->vkAllocateCommandBuffers(r->ctx.device, &cbai, &cmd) != VK_SUCCESS) {
        vk->vkDestroyCommandPool(r->ctx.device, pool, NULL);
        return 0;
    }

    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk->vkCreateFence(r->ctx.device, &fci, NULL, &fence) != VK_SUCCESS) {
        vk->vkDestroyCommandPool(r->ctx.device, pool, NULL);
        return 0;
    }

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk->vkBeginCommandBuffer(cmd, &bi);
    fn(r, cmd, user);
    vk->vkEndCommandBuffer(cmd);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r->stats.one_shot_submits++;
    wait_start = now_ms();
    vk->vkQueueSubmit(r->ctx.graphics_queue, 1, &si, fence);
    vk->vkWaitForFences(r->ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
    wait_start = now_ms() - wait_start;
    r->stats.cpu_one_shot_ms += wait_start;
    r->stats.one_shot_by_reason[reason]++;
    r->stats.one_shot_ms_by_reason[reason] += wait_start;
    if (wait_start > r->stats.cpu_one_shot_max_ms) r->stats.cpu_one_shot_max_ms = wait_start;

    vk->vkDestroyFence(r->ctx.device, fence, NULL);
    vk->vkDestroyCommandPool(r->ctx.device, pool, NULL);
    return 1;
}

typedef struct {
    VkImage img;
    VkBuffer src;
    VkDeviceSize src_off;
    uint32_t x, y, w, h, slice, mip;
    VkImageLayout old_layout;
} img_upload_job_t;

static void record_img_upload(jpt_vk_renderer_t* r, VkCommandBuffer cmd, void* user) {
    const img_upload_job_t* j = (const img_upload_job_t*)user;
    VkBufferImageCopy region;
    cmd_image_barrier(&r->ctx.vk, cmd, j->img, VK_IMAGE_ASPECT_COLOR_BIT, j->old_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, j->mip, 1, j->slice, 1);
    memset(&region, 0, sizeof region);
    region.bufferOffset = j->src_off;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = j->mip;
    region.imageSubresource.baseArrayLayer = j->slice;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = (int32_t)j->x;
    region.imageOffset.y = (int32_t)j->y;
    region.imageExtent.width = j->w;
    region.imageExtent.height = j->h;
    region.imageExtent.depth = 1;
    r->ctx.vk.vkCmdCopyBufferToImage(cmd, j->src, j->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    cmd_image_barrier(&r->ctx.vk, cmd, j->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, j->mip, 1, j->slice, 1);
}

static void end_scene_pass_if_open(jpt_vk_renderer_t* r);
static void end_scene_pass_for(jpt_vk_renderer_t* r, int reason) {
    r->break_reason = reason;
    end_scene_pass_if_open(r);
}
static void ensure_scene_pass(jpt_vk_renderer_t* r, int clear_now, const float clear_rgba[4], float clear_depth,
                              uint32_t clear_stencil);
static int upload_rgba_to_image_level(jpt_vk_renderer_t* r, VkImage img, VkImageLayout* cur_layout, uint32_t mip,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t slice, const void* data,
                                      size_t byte_size);

static int upload_rgba_to_image(jpt_vk_renderer_t* r, VkImage img, VkImageLayout* cur_layout, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h, uint32_t slice, const void* data, size_t byte_size) {
    return upload_rgba_to_image_level(r, img, cur_layout, 0, x, y, w, h, slice, data, byte_size);
}

static void reset_staging_extras(jpt_vk_renderer_t* r, vk_frame_rings_t* ring) {
    for (uint32_t e = 0; e < ring->extra_count; e++) ring->extra_used[e] = 0;
    while (ring->extra_count > 1) {
        ring->extra_count--;
        jpt_vk_destroy_buffer(&r->ctx.alloc, &ring->extra_buf[ring->extra_count], &ring->extra_alloc[ring->extra_count]);
    }
}

static int ring_stage_alloc(jpt_vk_renderer_t* r, VkDeviceSize size, VkBuffer* buf, VkDeviceSize* off, uint8_t** ptr) {
    vk_frame_rings_t* ring = &r->rings[r->sc.frame_index];
    VkDeviceSize at = (ring->staging_used + 15u) & ~((VkDeviceSize)15u);
    if (at + size <= JPT_VK_STAGING_SIZE && ring->staging_alloc.mapped) {
        ring->staging_used = at + size;
        *buf = ring->staging_buf;
        *off = at;
        *ptr = (uint8_t*)ring->staging_alloc.mapped + at;
        return 1;
    }
    for (uint32_t e = 0; e < ring->extra_count; e++) {
        at = (ring->extra_used[e] + 15u) & ~((VkDeviceSize)15u);
        if (at + size <= ring->extra_size[e]) {
            ring->extra_used[e] = at + size;
            *buf = ring->extra_buf[e];
            *off = at;
            *ptr = (uint8_t*)ring->extra_alloc[e].mapped + at;
            return 1;
        }
    }
    if (ring->extra_count < JPT_VK_STAGING_EXTRA_MAX) {
        const uint32_t e = ring->extra_count;
        VkDeviceSize bytes = size > JPT_VK_STAGING_EXTRA_SIZE ? size : JPT_VK_STAGING_EXTRA_SIZE;
        if (jpt_vk_create_buffer(&r->ctx.alloc, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1,
                                 &ring->extra_buf[e], &ring->extra_alloc[e]) == VK_SUCCESS &&
            ring->extra_alloc[e].mapped) {
            ring->extra_size[e] = bytes;
            ring->extra_used[e] = size;
            ring->extra_count++;
            r->stats.staging_extra_buffers++;
            *buf = ring->extra_buf[e];
            *off = 0;
            *ptr = (uint8_t*)ring->extra_alloc[e].mapped;
            return 1;
        }
        jpt_vk_destroy_buffer(&r->ctx.alloc, &ring->extra_buf[e], &ring->extra_alloc[e]);
    }
    return 0;
}

static int upload_rgba_to_image_level(jpt_vk_renderer_t* r, VkImage img, VkImageLayout* cur_layout, uint32_t mip,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t slice, const void* data,
                                      size_t byte_size) {
    if (r->in_frame) {
        VkBuffer staging;
        VkDeviceSize off;
        uint8_t* dst;
        if (ring_stage_alloc(r, byte_size, &staging, &off, &dst)) {
            img_upload_job_t job = { img, staging, off, x, y, w, h, slice, mip, *cur_layout };
            const int was_open = r->in_scene_pass;
            memcpy(dst, data, byte_size);
            if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_TEXTURE_UPLOAD);
            record_img_upload(r, r->cmd, &job);
            if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
            r->stats.uploads_in_frame++;
            *cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return 1;
        }
        r->stats.upload_fallback_staging_full++;
    } else {
        r->stats.upload_fallback_no_frame++;
    }

    VkBuffer tmp = VK_NULL_HANDLE;
    jpt_vk_allocation_t tmp_alloc;
    if (jpt_vk_create_buffer(&r->ctx.alloc, byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1, &tmp,
                             &tmp_alloc) != VK_SUCCESS)
        return 0;
    if (tmp_alloc.mapped) memcpy(tmp_alloc.mapped, data, byte_size);
    img_upload_job_t job = { img, tmp, 0, x, y, w, h, slice, mip, *cur_layout };
    int ok = run_one_shot_cmd(r, record_img_upload, &job, JPT_VK_OS_UPLOAD);
    jpt_vk_destroy_buffer(&r->ctx.alloc, &tmp, &tmp_alloc);
    if (ok) *cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return ok;
}

static int create_samplers(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    static const struct {
        VkSamplerAddressMode u;
        VkSamplerAddressMode v;
        int force_point;
    } k_specs[4] = {
        { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0 },
        { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0 },
        { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0 },
        { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0 },
    };
    /* mag, min, mips */
    static const int k_class[5][3] = { { 1, 1, 1 }, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } };

    for (int i = 0; i < JPT_VK_WORLD_SAMPLERS; i++) {
        VkSamplerCreateInfo sci;
        const int* c = k_class[i / 4];
        const int aniso = c[2] && r->active_aniso > 1.0f;
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = c[0] ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = c[1] ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = c[2] ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = k_specs[i % 4].u;
        sci.addressModeV = k_specs[i % 4].v;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.anisotropyEnable = aniso ? VK_TRUE : VK_FALSE;
        sci.maxAnisotropy = aniso ? r->active_aniso : 1.0f;
        sci.maxLod = c[2] ? 12.0f : 0.0f;
        if (vk->vkCreateSampler(r->ctx.device, &sci, NULL, &r->world_samplers[i]) != VK_SUCCESS) return 0;
        r->ctx.lifetime.samplers++;
    }

    VkSamplerCreateInfo lci;
    memset(&lci, 0, sizeof lci);
    lci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    lci.magFilter = VK_FILTER_LINEAR;
    lci.minFilter = VK_FILTER_LINEAR;
    lci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    lci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk->vkCreateSampler(r->ctx.device, &lci, NULL, &r->linear_clamp_sampler) != VK_SUCCESS) return 0;
    r->ctx.lifetime.samplers++;

    lci.magFilter = VK_FILTER_NEAREST;
    lci.minFilter = VK_FILTER_NEAREST;
    if (vk->vkCreateSampler(r->ctx.device, &lci, NULL, &r->nearest_clamp_sampler) != VK_SUCCESS) return 0;
    r->ctx.lifetime.samplers++;
    return 1;
}

static void world_ds_invalidate(jpt_vk_renderer_t* r) {
    r->world_bound_valid = 0;
    r->last_world_ds = VK_NULL_HANDLE;
    r->world_ds_cache_count = 0;
}

static int create_layouts_and_descriptors(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkDescriptorSetLayoutBinding wb[4];
    VkDescriptorSetLayoutCreateInfo wci;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plci;

    memset(wb, 0, sizeof wb);
    wb[0].binding = 0;
    wb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wb[0].descriptorCount = 16;
    wb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    wb[1].binding = 1;
    wb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    wb[1].descriptorCount = JPT_VK_WORLD_SAMPLERS;
    wb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    wb[1].pImmutableSamplers = r->world_samplers;
    wb[2].binding = 2;
    wb[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wb[2].descriptorCount = 1;
    wb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    wb[3].binding = 3;
    wb[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wb[3].descriptorCount = 1;
    wb[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    memset(&wci, 0, sizeof wci);
    wci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    wci.bindingCount = 4;
    wci.pBindings = wb;
    if (vk->vkCreateDescriptorSetLayout(r->ctx.device, &wci, NULL, &r->world_dsl) != VK_SUCCESS) return 0;

    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(vk_world_push_t);

    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &r->world_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vk->vkCreatePipelineLayout(r->ctx.device, &plci, NULL, &r->world_pl) != VK_SUCCESS) return 0;
    r->ctx.lifetime.pipeline_layouts++;

    VkDescriptorSetLayoutBinding qb[3];
    memset(qb, 0, sizeof qb);
    for (uint32_t i = 0; i < 3; i++) {
        qb[i].binding = i;
        qb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        qb[i].descriptorCount = 1;
        qb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&wci, 0, sizeof wci);
    wci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    wci.bindingCount = 3;
    wci.pBindings = qb;
    if (vk->vkCreateDescriptorSetLayout(r->ctx.device, &wci, NULL, &r->quad_dsl) != VK_SUCCESS) return 0;

    pcr.size = sizeof(vk_quad_push_t);
    plci.pSetLayouts = &r->quad_dsl;
    if (vk->vkCreatePipelineLayout(r->ctx.device, &plci, NULL, &r->quad_pl) != VK_SUCCESS) return 0;
    r->ctx.lifetime.pipeline_layouts++;

    VkDescriptorPoolSize sizes[1] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 192 },
    };
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof dpci);
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = sizes;
    if (vk->vkCreateDescriptorPool(r->ctx.device, &dpci, NULL, &r->desc_pool) != VK_SUCCESS) return 0;
    r->ctx.lifetime.descriptor_pools++;

    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof dsai);
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = r->desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &r->quad_dsl;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->present_ds) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->ao_prep_ds) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->ao_gen_ds) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->ao_blur_h_ds) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->ao_blur_v_ds) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->ao_apply_ds) != VK_SUCCESS) return 0;
    for (int i = 0; i < JPT_LAYER_COUNT; i++) {
        if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->layers[i].desc_linear) != VK_SUCCESS) return 0;
        if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->layers[i].desc_point) != VK_SUCCESS) return 0;
    }
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->overlay_ds_point) != VK_SUCCESS) return 0;
    if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->overlay_ds_linear) != VK_SUCCESS) return 0;
    return 1;
}

static int create_render_passes(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkSampleCountFlagBits samples = msaa_bits(r->active_msaa);

    /* Scene render passes (load + clear) */
    for (int clear_pass = 0; clear_pass < 2; clear_pass++) {
        VkAttachmentDescription atts[2];
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub;
        VkRenderPassCreateInfo rpci;
        VkRenderPass* dst = clear_pass ? &r->scene_rp_clear : &r->scene_rp_load;

        memset(atts, 0, sizeof atts);
        atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        atts[0].samples = samples;
        atts[0].loadOp = clear_pass ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = clear_pass ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        atts[1].format = r->ctx.gpu.preferred_depth_format;
        atts[1].samples = samples;
        atts[1].loadOp = clear_pass ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp = r->ctx.gpu.depth_has_stencil
                                    ? (clear_pass ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD)
                                    : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = r->ctx.gpu.depth_has_stencil ? VK_ATTACHMENT_STORE_OP_STORE
                                                              : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout =
            clear_pass ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        memset(&sub, 0, sizeof sub);
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;

        VkSubpassDependency deps[2];
        const VkPipelineStageFlags attach_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const VkAccessFlags attach_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        memset(deps, 0, sizeof deps);
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = attach_stages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask = attach_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = attach_access | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = attach_access | VK_ACCESS_SHADER_READ_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = attach_stages;
        deps[1].dstStageMask = attach_stages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = attach_access;
        deps[1].dstAccessMask = attach_access | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

        memset(&rpci, 0, sizeof rpci);
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
        if (vk->vkCreateRenderPass(r->ctx.device, &rpci, NULL, dst) != VK_SUCCESS) return 0;
        r->ctx.lifetime.render_passes++;
    }

    /* Single-sample color load pass (resolved scene: AO apply + overlays) */
    {
        VkAttachmentDescription att;
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub;
        VkRenderPassCreateInfo rpci;
        memset(&att, 0, sizeof att);
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        memset(&sub, 0, sizeof sub);
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        memset(&rpci, 0, sizeof rpci);
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        if (vk->vkCreateRenderPass(r->ctx.device, &rpci, NULL, &r->resolved_rp_load) != VK_SUCCESS) return 0;
        r->ctx.lifetime.render_passes++;
    }

    /* R32_SFLOAT and R8_UNORM post-process passes for AO */
    {
        VkFormat fmts[2] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UNORM };
        VkRenderPass* dsts[2] = { &r->r32_rp, &r->r8_rp };
        for (int i = 0; i < 2; i++) {
            VkAttachmentDescription att;
            VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription sub;
            VkRenderPassCreateInfo rpci;
            memset(&att, 0, sizeof att);
            att.format = fmts[i];
            att.samples = VK_SAMPLE_COUNT_1_BIT;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            memset(&sub, 0, sizeof sub);
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &cref;
            memset(&rpci, 0, sizeof rpci);
            rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpci.attachmentCount = 1;
            rpci.pAttachments = &att;
            rpci.subpassCount = 1;
            rpci.pSubpasses = &sub;
            if (vk->vkCreateRenderPass(r->ctx.device, &rpci, NULL, dsts[i]) != VK_SUCCESS) return 0;
            r->ctx.lifetime.render_passes++;
        }
    }
    return 1;
}

static int create_image_2d(jpt_vk_renderer_t* r, uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage* out_img,
                           jpt_vk_allocation_t* out_alloc, VkImageView* out_view) {
    VkImageCreateInfo ici;
    VkImageViewCreateInfo ivci;

    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (jpt_vk_create_image(&r->ctx.alloc, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, out_img, out_alloc) !=
        VK_SUCCESS)
        return 0;

    memset(&ivci, 0, sizeof ivci);
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = *out_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = fmt;
    ivci.subresourceRange.aspectMask = aspect;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    if (r->ctx.vk.vkCreateImageView(r->ctx.device, &ivci, NULL, out_view) != VK_SUCCESS) {
        jpt_vk_destroy_image(&r->ctx.alloc, out_img, out_alloc);
        return 0;
    }
    r->ctx.lifetime.image_views++;
    return 1;
}

static void destroy_rt_image(jpt_vk_renderer_t* r, VkImage* img, jpt_vk_allocation_t* alloc, VkImageView* view) {
    if (view && *view != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyImageView(r->ctx.device, *view, NULL);
        *view = VK_NULL_HANDLE;
        r->ctx.lifetime.image_views--;
    }
    if (img && *img != VK_NULL_HANDLE) jpt_vk_destroy_image(&r->ctx.alloc, img, alloc);
}

static void destroy_fb(jpt_vk_renderer_t* r, VkFramebuffer* fb) {
    if (fb && *fb != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyFramebuffer(r->ctx.device, *fb, NULL);
        *fb = VK_NULL_HANDLE;
        r->ctx.lifetime.framebuffers--;
    }
}

static void destroy_scene_targets(jpt_vk_renderer_t* r) {
    destroy_fb(r, &r->scene_fb);
    destroy_fb(r, &r->resolved_fb);
    destroy_fb(r, &r->ao_lin_depth_fb);
    destroy_fb(r, &r->ao_raw_fb);
    destroy_fb(r, &r->ao_blur_tmp_fb);
    destroy_fb(r, &r->ao_blur_out_fb);

    if (r->scene_stencil_view != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyImageView(r->ctx.device, r->scene_stencil_view, NULL);
        r->scene_stencil_view = VK_NULL_HANDLE;
        r->ctx.lifetime.image_views--;
    }
    if (r->scene_depth_sample_view != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyImageView(r->ctx.device, r->scene_depth_sample_view, NULL);
        r->scene_depth_sample_view = VK_NULL_HANDLE;
        r->ctx.lifetime.image_views--;
    }
    destroy_rt_image(r, &r->scene_color, &r->scene_color_alloc, &r->scene_color_view);
    destroy_rt_image(r, &r->scene_resolved, &r->scene_resolved_alloc, &r->scene_resolved_view);
    destroy_rt_image(r, &r->scene_behind, &r->scene_behind_alloc, &r->scene_behind_view);
    destroy_rt_image(r, &r->last_img, &r->last_alloc, &r->last_view);
    r->last_valid = 0;
    r->last_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    destroy_rt_image(r, &r->scene_depth, &r->scene_depth_alloc, &r->scene_depth_view);

    destroy_rt_image(r, &r->ao_lin_depth, &r->ao_lin_depth_alloc, &r->ao_lin_depth_view);
    destroy_rt_image(r, &r->ao_raw, &r->ao_raw_alloc, &r->ao_raw_view);
    destroy_rt_image(r, &r->ao_blur_tmp, &r->ao_blur_tmp_alloc, &r->ao_blur_tmp_view);
    destroy_rt_image(r, &r->ao_blur_out, &r->ao_blur_out_alloc, &r->ao_blur_out_view);
}

static void update_quad_ds_1(jpt_vk_renderer_t* r, VkDescriptorSet ds, VkImageView v0, VkSampler s0) {
    VkDescriptorImageInfo ii = { s0, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = ds;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    r->ctx.vk.vkUpdateDescriptorSets(r->ctx.device, 1, &w, 0, NULL);
}

static void update_quad_ds_2(jpt_vk_renderer_t* r, VkDescriptorSet ds, VkImageView v0, VkSampler s0, VkImageView v1,
                             VkSampler s1) {
    VkDescriptorImageInfo ii[2] = {
        { s0, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { s1, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof w);
    for (uint32_t i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = ds;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    r->ctx.vk.vkUpdateDescriptorSets(r->ctx.device, 2, w, 0, NULL);
}

typedef struct {
    VkImage img;
    uint32_t layers;
    uint32_t levels;
} pool_init_job_t;

static void record_pool_init(jpt_vk_renderer_t* r, VkCommandBuffer cmd, void* user) {
    const pool_init_job_t* j = (const pool_init_job_t*)user;
    cmd_image_barrier(&r->ctx.vk, cmd, j->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, j->levels ? j->levels : 1u, 0, j->layers);
}

static int create_scene_targets(jpt_vk_renderer_t* r, uint32_t w, uint32_t h) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkSampleCountFlagBits samples = msaa_bits(r->active_msaa);
    VkImageAspectFlags depth_aspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (r->ctx.gpu.depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    VkFramebufferCreateInfo fbci;
    VkImageView atts[2];

    destroy_scene_targets(r);
    r->rt_w = w;
    r->rt_h = h;

    if (r->active_msaa > 1) {
        if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, samples,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &r->scene_color, &r->scene_color_alloc, &r->scene_color_view))
            return 0;
    }

    if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &r->scene_resolved, &r->scene_resolved_alloc,
                         &r->scene_resolved_view))
        return 0;

    if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &r->scene_behind, &r->scene_behind_alloc, &r->scene_behind_view))
        return 0;
    {
        pool_init_job_t job = { r->scene_behind, 1, 1 };
        run_one_shot_cmd(r, record_pool_init, &job, JPT_VK_OS_RESOURCE_INIT);
    }

    if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &r->last_img, &r->last_alloc, &r->last_view))
        return 0;

    if (!create_image_2d(r, w, h, r->ctx.gpu.preferred_depth_format, samples,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, depth_aspect,
                         &r->scene_depth, &r->scene_depth_alloc, &r->scene_depth_view))
        return 0;

    /* Depth-only view for AO sampling */
    {
        VkImageViewCreateInfo ivci;
        memset(&ivci, 0, sizeof ivci);
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = r->scene_depth;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = r->ctx.gpu.preferred_depth_format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (vk->vkCreateImageView(r->ctx.device, &ivci, NULL, &r->scene_depth_sample_view) != VK_SUCCESS) return 0;
        r->ctx.lifetime.image_views++;
        if (r->ctx.gpu.depth_has_stencil) {
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            if (vk->vkCreateImageView(r->ctx.device, &ivci, NULL, &r->scene_stencil_view) != VK_SUCCESS) return 0;
            r->ctx.lifetime.image_views++;
        }
    }

    atts[0] = (r->active_msaa > 1) ? r->scene_color_view : r->scene_resolved_view;
    atts[1] = r->scene_depth_view;
    memset(&fbci, 0, sizeof fbci);
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = r->scene_rp_clear;
    fbci.attachmentCount = 2;
    fbci.pAttachments = atts;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->scene_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    fbci.renderPass = r->resolved_rp_load;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &r->scene_resolved_view;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->resolved_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    /* AO targets */
    if (!create_image_2d(r, w, h, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &r->ao_lin_depth, &r->ao_lin_depth_alloc, &r->ao_lin_depth_view))
        return 0;
    if (!create_image_2d(r, w, h, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &r->ao_raw, &r->ao_raw_alloc, &r->ao_raw_view))
        return 0;
    if (!create_image_2d(r, w, h, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &r->ao_blur_tmp, &r->ao_blur_tmp_alloc, &r->ao_blur_tmp_view))
        return 0;
    if (!create_image_2d(r, w, h, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &r->ao_blur_out, &r->ao_blur_out_alloc, &r->ao_blur_out_view))
        return 0;

    fbci.renderPass = r->r32_rp;
    fbci.pAttachments = &r->ao_lin_depth_view;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->ao_lin_depth_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    fbci.renderPass = r->r8_rp;
    fbci.pAttachments = &r->ao_raw_view;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->ao_raw_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    fbci.pAttachments = &r->ao_blur_tmp_view;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->ao_blur_tmp_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    fbci.pAttachments = &r->ao_blur_out_view;
    if (vk->vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &r->ao_blur_out_fb) != VK_SUCCESS) return 0;
    r->ctx.lifetime.framebuffers++;

    /* Update post/present descriptor sets */
    update_quad_ds_2(r, r->present_ds, r->scene_resolved_view, r->linear_clamp_sampler, r->gamma_lut_view,
                     r->nearest_clamp_sampler);
    update_quad_ds_1(r, r->ao_prep_ds, r->scene_depth_sample_view, r->nearest_clamp_sampler);
    update_quad_ds_1(r, r->ao_gen_ds, r->ao_lin_depth_view, r->nearest_clamp_sampler);
    update_quad_ds_2(r, r->ao_blur_h_ds, r->ao_raw_view, r->linear_clamp_sampler, r->ao_lin_depth_view,
                     r->nearest_clamp_sampler);
    update_quad_ds_2(r, r->ao_blur_v_ds, r->ao_blur_tmp_view, r->linear_clamp_sampler, r->ao_lin_depth_view,
                     r->nearest_clamp_sampler);
    if (r->scene_stencil_view != VK_NULL_HANDLE)
        update_quad_ds_2(r, r->ao_apply_ds, r->ao_blur_out_view, r->linear_clamp_sampler, r->scene_stencil_view,
                         r->nearest_clamp_sampler);
    else
        update_quad_ds_1(r, r->ao_apply_ds, r->ao_blur_out_view, r->linear_clamp_sampler);

    world_ds_invalidate(r);
    return 1;
}

static void set_ao_stencil(VkPipelineDepthStencilStateCreateInfo* d, const jpt_vk_renderer_t* r, VkStencilOp pass) {
    if (!r->ctx.gpu.depth_has_stencil) return;
    d->stencilTestEnable = VK_TRUE;
    d->front.failOp = VK_STENCIL_OP_KEEP;
    d->front.passOp = pass;
    d->front.depthFailOp = VK_STENCIL_OP_KEEP;
    d->front.compareOp = VK_COMPARE_OP_ALWAYS;
    d->front.compareMask = 0xFFu;
    d->front.writeMask = 0xFFu;
    d->back = d->front;
}

static int create_quad_pipeline_ex(jpt_vk_renderer_t* r, VkRenderPass rp, jpt_vk_shader_id_t frag_id, int blend_mode,
                                   VkSampleCountFlagBits samples, int with_depth, VkPipeline* out_pipe) {
    /* blend_mode: 0=opaque, 1=straight alpha, 2=multiply, 3=premultiplied alpha */
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo quad_dss;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo pci;

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shaders[JPT_VK_SH_QUAD_VERT];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shaders[frag_id];
    stages[1].pName = "main";

    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = samples;

    memset(&quad_dss, 0, sizeof quad_dss);
    quad_dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    set_ao_stencil(&quad_dss, r, VK_STENCIL_OP_INCREMENT_AND_CLAMP);

    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    if (blend_mode == 1) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend_mode == 2) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend_mode == 3) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyns;

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = with_depth ? &quad_dss : NULL;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = r->quad_pl;
    pci.renderPass = rp;

    if (r->ctx.vk.vkCreateGraphicsPipelines(r->ctx.device, r->ctx.pipeline_cache, 1, &pci, NULL, out_pipe) !=
        VK_SUCCESS)
        return 0;
    r->ctx.lifetime.pipelines++;
    r->stats.pipelines_created++;
    return 1;
}

static int create_quad_pipeline(jpt_vk_renderer_t* r, VkRenderPass rp, jpt_vk_shader_id_t frag_id, int blend_mode,
                                VkPipeline* out_pipe) {
    return create_quad_pipeline_ex(r, rp, frag_id, blend_mode, VK_SAMPLE_COUNT_1_BIT, 0, out_pipe);
}

static VkPipeline get_or_create_world_pipeline(jpt_vk_renderer_t* r, const jpt_vk_pipeline_key_t* key) {
    uint64_t h = jpt_vk_pipeline_key_hash(key);
    int free_slot = -1;
    for (int i = 0; i < JPT_VK_MAX_PIPELINES; i++) {
        if (r->world_pipes[i].used) {
            if (r->world_pipes[i].hash == h && jpt_vk_pipeline_key_equal(&r->world_pipes[i].key, key)) {
                r->stats.pipeline_cache_hits++;
                return r->world_pipes[i].pipeline;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) return VK_NULL_HANDLE;

    jpt_vk_shader_id_t frag = (key->shader_kind == JPT_VK_SHADER_WATER) ? JPT_VK_SH_WATER_FRAG
                            : (key->shader_kind == JPT_VK_SHADER_SKY)   ? JPT_VK_SH_SKY_FRAG
                                                                        : JPT_VK_SH_FIXED_FRAG;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vbind = { 0, sizeof(vk_gpu_vertex_t), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattr[5];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyns[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo pci;
    VkPipeline pipe = VK_NULL_HANDLE;

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shaders[JPT_VK_SH_TL_VERT];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shaders[frag];
    stages[1].pName = "main";

    memset(vattr, 0, sizeof vattr);
    vattr[0].location = 0;
    vattr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[0].offset = (uint32_t)offsetof(vk_gpu_vertex_t, sx);
    vattr[1].location = 1;
    vattr[1].format = VK_FORMAT_B8G8R8A8_UNORM;
    vattr[1].offset = (uint32_t)offsetof(vk_gpu_vertex_t, diffuse);
    vattr[2].location = 2;
    vattr[2].format = VK_FORMAT_B8G8R8A8_UNORM;
    vattr[2].offset = (uint32_t)offsetof(vk_gpu_vertex_t, specular);
    vattr[3].location = 3;
    vattr[3].format = VK_FORMAT_R32G32_SFLOAT;
    vattr[3].offset = (uint32_t)offsetof(vk_gpu_vertex_t, tu);
    vattr[4].location = 4;
    vattr[4].format = VK_FORMAT_R32_UINT;
    vattr[4].offset = (uint32_t)offsetof(vk_gpu_vertex_t, layer);

    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = vattr;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = map_topology(key->topology);

    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = (VkPolygonMode)key->fill_mode;
    rs.cullMode = map_cull_mode(key->cull_mode);
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = msaa_bits(key->sample_count);

    memset(&dss, 0, sizeof dss);
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = key->z_enable ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = key->z_write ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = key->z_enable ? map_compare_op(key->z_func) : VK_COMPARE_OP_ALWAYS;
    set_ao_stencil(&dss, r, key->z_enable && key->z_write ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_INCREMENT_AND_CLAMP);

    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    if (key->blend_enable && key->shader_kind != JPT_VK_SHADER_WATER) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = map_blend_factor(key->src_blend);
        cba.dstColorBlendFactor = map_blend_factor(key->dst_blend);
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 3;
    ds.pDynamicStates = dyns;

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dss;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = r->world_pl;
    pci.renderPass = r->scene_rp_load;

    double create_start = r->collect_timings ? now_ms() : 0.0;
    VkResult pvr = r->ctx.vk.vkCreateGraphicsPipelines(r->ctx.device, r->ctx.pipeline_cache, 1, &pci, NULL, &pipe);
    if (r->collect_timings) r->stats.cpu_pipeline_ms += now_ms() - create_start;
    if (pvr != VK_SUCCESS) {
        jpt_vk_log(&r->ctx, "vkCreateGraphicsPipelines failed: %s (%d)", jpt_vk_result_str(pvr), (int)pvr);
        return VK_NULL_HANDLE;
    }
    r->ctx.lifetime.pipelines++;
    r->stats.pipelines_created++;
    r->world_pipes[free_slot].used = 1;
    r->world_pipes[free_slot].key = *key;
    r->world_pipes[free_slot].hash = h;
    r->world_pipes[free_slot].pipeline = pipe;
    return pipe;
}

static void retire_image(jpt_vk_renderer_t* r, VkImage* img, jpt_vk_allocation_t* alloc, VkImageView* view) {
    vk_retired_img_t* grown;
    uint32_t new_cap;
    if ((!img || *img == VK_NULL_HANDLE) && (!view || *view == VK_NULL_HANDLE)) return;
    if (r->retired_count == r->retired_cap) {
        new_cap = r->retired_cap ? r->retired_cap * 2u : 64u;
        grown = (vk_retired_img_t*)realloc(r->retired, (size_t)new_cap * sizeof *grown);
        if (!grown) {
            r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
            destroy_rt_image(r, img, alloc, view);
            return;
        }
        r->retired = grown;
        r->retired_cap = new_cap;
    }
    vk_retired_img_t* e = &r->retired[r->retired_count++];
    e->image = img ? *img : VK_NULL_HANDLE;
    e->alloc = alloc ? *alloc : (jpt_vk_allocation_t){ 0 };
    e->view = view ? *view : VK_NULL_HANDLE;
    e->retire_seq = r->frame_seq + (r->in_frame ? 1u : 0u);
    if (img) *img = VK_NULL_HANDLE;
    if (alloc) memset(alloc, 0, sizeof *alloc);
    if (view) *view = VK_NULL_HANDLE;
    world_ds_invalidate(r);
}

static void drain_retired_images(jpt_vk_renderer_t* r, int force_all) {
    uint64_t completed_seq = 0;
    uint32_t dst = 0;
    if (!r->retired_count) return;
    if (!force_all) {
        if (r->frame_seq + 1u < (uint64_t)JPT_VK_FRAMES_IN_FLIGHT) return;
        completed_seq = r->frame_seq + 1u - (uint64_t)JPT_VK_FRAMES_IN_FLIGHT;
    }
    for (uint32_t i = 0; i < r->retired_count; i++) {
        vk_retired_img_t* e = &r->retired[i];
        if (force_all || e->retire_seq <= completed_seq) {
            destroy_rt_image(r, &e->image, &e->alloc, &e->view);
        } else {
            if (dst != i) r->retired[dst] = *e;
            dst++;
        }
    }
    r->retired_count = dst;
}

static int create_world_desc_pool(jpt_vk_renderer_t* r, vk_frame_rings_t* ring) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16u * JPT_VK_WORLD_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 5u * JPT_VK_WORLD_POOL_SETS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * JPT_VK_WORLD_POOL_SETS },
    };
    VkDescriptorPoolCreateInfo dpci;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (ring->world_pool_count == ring->world_pool_cap) {
        uint32_t new_cap = ring->world_pool_cap ? ring->world_pool_cap * 2u : 4u;
        VkDescriptorPool* grown = (VkDescriptorPool*)realloc(ring->world_pools, (size_t)new_cap * sizeof *grown);
        if (!grown) return 0;
        ring->world_pools = grown;
        ring->world_pool_cap = new_cap;
    }
    memset(&dpci, 0, sizeof dpci);
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = JPT_VK_WORLD_POOL_SETS;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (vk->vkCreateDescriptorPool(r->ctx.device, &dpci, NULL, &pool) != VK_SUCCESS) return 0;
    r->ctx.lifetime.descriptor_pools++;
    ring->world_pools[ring->world_pool_count++] = pool;
    return 1;
}

static void reset_frame_world_pools(jpt_vk_renderer_t* r, vk_frame_rings_t* ring) {
    for (uint32_t i = 0; i < ring->world_pool_count; i++) {
        if (ring->world_pools[i] != VK_NULL_HANDLE)
            r->ctx.vk.vkResetDescriptorPool(r->ctx.device, ring->world_pools[i], 0);
    }
    ring->world_pool_cur = 0;
    world_ds_invalidate(r);
}

static VkDescriptorSet acquire_world_descriptor_set(jpt_vk_renderer_t* r, const VkImageView views[16]) {
    vk_frame_rings_t* ring = &r->rings[r->sc.frame_index];
    VkDescriptorSetAllocateInfo dsai;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkResult vr;
    VkDescriptorImageInfo imgs[16];
    VkDescriptorImageInfo water_ii;
    VkDescriptorImageInfo behind_ii;
    VkWriteDescriptorSet writes[3];
    double t0;

    if (r->last_world_ds != VK_NULL_HANDLE && r->last_water_view == r->water_waves_view &&
        r->last_behind_view == r->scene_behind_view &&
        memcmp(r->last_world_views, views, sizeof r->last_world_views) == 0) {
        return r->last_world_ds;
    }

    for (uint32_t i = 0; i < r->world_ds_cache_count; i++) {
        const vk_world_ds_entry_t* e = &r->world_ds_cache[i];
        if (e->water == r->water_waves_view && e->behind == r->scene_behind_view &&
            memcmp(e->views, views, sizeof e->views) == 0) {
            r->last_world_ds = e->ds;
            memcpy(r->last_world_views, views, sizeof r->last_world_views);
            r->last_water_view = r->water_waves_view;
            r->last_behind_view = r->scene_behind_view;
            r->stats.descriptor_cache_hits++;
            return e->ds;
        }
    }

    t0 = r->collect_timings ? now_ms() : 0.0;
    if (ring->world_pool_count == 0 && !create_world_desc_pool(r, ring)) return VK_NULL_HANDLE;

    memset(&dsai, 0, sizeof dsai);
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &r->world_dsl;

    for (;;) {
        if (ring->world_pool_cur >= ring->world_pool_count) {
            if (!create_world_desc_pool(r, ring)) return VK_NULL_HANDLE;
        }
        dsai.descriptorPool = ring->world_pools[ring->world_pool_cur];
        vr = r->ctx.vk.vkAllocateDescriptorSets(r->ctx.device, &dsai, &ds);
        if (vr == VK_SUCCESS) break;
        if (vr == VK_ERROR_OUT_OF_POOL_MEMORY || vr == VK_ERROR_FRAGMENTED_POOL) {
            ring->world_pool_cur++;
            continue;
        }
        return VK_NULL_HANDLE;
    }

    for (int i = 0; i < 16; i++) {
        imgs[i].sampler = VK_NULL_HANDLE;
        imgs[i].imageView = views[i] != VK_NULL_HANDLE ? views[i] : r->pools[0].view;
        imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    water_ii.sampler = r->linear_clamp_sampler;
    water_ii.imageView = r->water_waves_view;
    water_ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    behind_ii.sampler = r->linear_clamp_sampler;
    behind_ii.imageView = r->scene_behind_view;
    behind_ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    memset(writes, 0, sizeof writes);
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 16;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = imgs;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 2;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &water_ii;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = ds;
    writes[2].dstBinding = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &behind_ii;

    r->ctx.vk.vkUpdateDescriptorSets(r->ctx.device, 3, writes, 0, NULL);
    r->last_world_ds = ds;
    memcpy(r->last_world_views, views, sizeof r->last_world_views);
    r->last_water_view = r->water_waves_view;
    r->last_behind_view = r->scene_behind_view;
    if (r->world_ds_cache_count < JPT_VK_WORLD_DS_CACHE) {
        vk_world_ds_entry_t* e = &r->world_ds_cache[r->world_ds_cache_count++];
        memcpy(e->views, views, sizeof e->views);
        e->water = r->water_waves_view;
        e->behind = r->scene_behind_view;
        e->ds = ds;
    }
    r->stats.descriptor_updates++;
    if (r->collect_timings) r->stats.cpu_descriptor_ms += now_ms() - t0;
    return ds;
}

static int create_tex_pool(jpt_vk_renderer_t* r, int slot, uint32_t w, uint32_t h, uint32_t levels, uint32_t max_slices) {
    vk_tex_pool_t* p = &r->pools[slot];
    VkImageCreateInfo ici;
    VkImageViewCreateInfo ivci;
    uint32_t limit = r->ctx.props.limits.maxImageArrayLayers;

    if (limit > 0 && max_slices > limit) max_slices = limit;
    if (max_slices > JPT_VK_POOL_MAX_SLICES) max_slices = JPT_VK_POOL_MAX_SLICES;

    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = levels;
    ici.arrayLayers = max_slices;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (jpt_vk_create_image(&r->ctx.alloc, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->image, &p->alloc) !=
        VK_SUCCESS)
        return 0;

    memset(&ivci, 0, sizeof ivci);
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = p->image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = levels;
    ivci.subresourceRange.layerCount = max_slices;
    if (r->ctx.vk.vkCreateImageView(r->ctx.device, &ivci, NULL, &p->view) != VK_SUCCESS) {
        jpt_vk_destroy_image(&r->ctx.alloc, &p->image, &p->alloc);
        return 0;
    }
    r->ctx.lifetime.image_views++;

    if (r->in_frame && !r->in_scene_pass) {
        cmd_image_barrier(&r->ctx.vk, r->cmd, p->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, levels, 0, max_slices);
    } else {
        pool_init_job_t job = { p->image, max_slices, levels };
        run_one_shot_cmd(r, record_pool_init, &job, JPT_VK_OS_RESOURCE_INIT);
    }

    p->used = 1;
    p->width = w;
    p->height = h;
    p->mip_levels = levels;
    p->max_slices = max_slices;
    p->live = 0;
    memset(p->slice_mask, 0, sizeof p->slice_mask);
    p->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return 1;
}

static void flush_batch(jpt_vk_renderer_t* r);

static void init_default_textures(jpt_vk_renderer_t* r) {
    /* Pool 0 slice 0: 1x1 white fallback so all 16 array slots are always valid */
    if (create_tex_pool(r, 0, 1, 1, 1, 1)) {
        uint8_t white[4] = { 255, 255, 255, 255 };
        upload_rgba_to_image(r, r->pools[0].image, &r->pools[0].layout, 0, 0, 1, 1, 0, white, sizeof white);
        r->pools[0].slice_mask[0] = 1ULL;
    }

    /* 2x2 zero water waves texture (R16G16_SFLOAT) */
    if (create_image_2d(r, 2, 2, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                        &r->waves[0].img, &r->waves[0].alloc, &r->waves[0].view)) {
        uint16_t zeros[2 * 2 * 2] = { 0 };
        r->waves[0].layout = VK_IMAGE_LAYOUT_UNDEFINED;
        upload_rgba_to_image(r, r->waves[0].img, &r->waves[0].layout, 0, 0, 2, 2, 0, zeros, sizeof zeros);
        r->waves[0].w = r->waves[0].h = 2;
        r->water_waves_view = r->waves[0].view;
    }

    /* 256x1 identity gamma LUT */
    for (int i = 0; i < 256; i++) {
        r->gamma_identity = 1;
        r->gamma_rgba[i * 4 + 0] = (uint8_t)i;
        r->gamma_rgba[i * 4 + 1] = (uint8_t)i;
        r->gamma_rgba[i * 4 + 2] = (uint8_t)i;
        r->gamma_rgba[i * 4 + 3] = 255;
    }
    if (create_image_2d(r, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                        &r->gamma_lut_img, &r->gamma_lut_alloc, &r->gamma_lut_view)) {
        VkImageLayout l = VK_IMAGE_LAYOUT_UNDEFINED;
        upload_rgba_to_image(r, r->gamma_lut_img, &l, 0, 0, 256, 1, 0, r->gamma_rgba, sizeof r->gamma_rgba);
    }
    r->gamma_dirty = 0;
}

static int create_frame_rings(jpt_vk_renderer_t* r) {
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (jpt_vk_create_buffer(&r->ctx.alloc, JPT_VK_STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1,
                                 &r->rings[i].staging_buf, &r->rings[i].staging_alloc) != VK_SUCCESS)
            return 0;
        if (jpt_vk_create_buffer(&r->ctx.alloc, JPT_VK_VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1,
                                 &r->rings[i].vb_buf, &r->rings[i].vb_alloc) != VK_SUCCESS)
            return 0;
        if (jpt_vk_create_buffer(&r->ctx.alloc, JPT_VK_IB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1,
                                 &r->rings[i].ib_buf, &r->rings[i].ib_alloc) != VK_SUCCESS)
            return 0;
        if (!create_world_desc_pool(r, &r->rings[i])) return 0;
    }
    return 1;
}

static void create_timing_queries(jpt_vk_renderer_t* r) {
    VkQueryPoolCreateInfo qci;
    if (!r->collect_timings || !r->ctx.vk.vkCreateQueryPool || r->ctx.props.limits.timestampPeriod <= 0.0f) return;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = JPT_VK_TS_TOTAL;
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (r->ctx.vk.vkCreateQueryPool(r->ctx.device, &qci, NULL, &r->ts_pool[i]) != VK_SUCCESS) {
            r->ts_pool[i] = VK_NULL_HANDLE;
            return;
        }
    }
    r->ts_period_ms = (double)r->ctx.props.limits.timestampPeriod / 1.0e6;
}

static void reclaim_empty_pools(jpt_vk_renderer_t* r) {
    for (int i = 1; i < JPT_VK_MAX_POOLS; i++) {
        vk_tex_pool_t* p = &r->pools[i];
        if (!p->used || p->live) continue;
        retire_image(r, &p->image, &p->alloc, &p->view);
        memset(p, 0, sizeof *p);
        r->stats.pools_reclaimed++;
    }
}

/* A frame that submitted early (the guest's own pixels read back mid-frame) has closed segments: query 6 + 2k ends
 * segment k and 6 + 2k + 1 starts the next, the first starting at query 0. Their GPU time counts as scene time. */
static void read_timing_queries(jpt_vk_renderer_t* r, int ring_index) {
    uint64_t t[JPT_VK_TS_COUNT], seg[2u * JPT_VK_TS_MAX_SEGMENTS];
    uint32_t closed = r->ts_segments_done[ring_index];
    uint64_t start, closed_ticks = 0;
    if (r->ts_pool[ring_index] == VK_NULL_HANDLE || !r->ts_pending[ring_index]) return;
    r->ts_pending[ring_index] = 0;
    if (r->ctx.vk.vkGetQueryPoolResults(r->ctx.device, r->ts_pool[ring_index], 0, JPT_VK_TS_COUNT, sizeof t, t,
                                        sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        r->stats.gpu_frames_unsampled++;
        return;
    }
    start = t[0];
    if (closed) {
        if (closed > JPT_VK_TS_MAX_SEGMENTS ||
            r->ctx.vk.vkGetQueryPoolResults(r->ctx.device, r->ts_pool[ring_index], JPT_VK_TS_COUNT, 2u * closed,
                                            sizeof seg[0] * 2u * closed, seg, sizeof seg[0],
                                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            r->stats.gpu_frames_unsampled++;
            return;
        }
        for (uint32_t k = 0; k < closed; k++) {
            uint64_t begin = k ? seg[2u * k - 1u] : t[0];
            if (seg[2u * k] < begin) {
                r->stats.gpu_frames_unsampled++;
                return;
            }
            closed_ticks += seg[2u * k] - begin;
        }
        start = seg[2u * closed - 1u];
    }
    if (t[5] <= start || t[1] < start || t[2] < t[1] || t[3] < t[2] || t[4] < t[3] || t[5] < t[4]) {
        r->stats.gpu_frames_unsampled++;
        return;
    }
    r->stats.gpu_scene_ms += (double)(closed_ticks + (t[1] - start)) * r->ts_period_ms;
    r->stats.gpu_ao_ms += (double)(t[3] - t[2]) * r->ts_period_ms;
    r->stats.gpu_layers_ms += (double)(t[4] - t[3]) * r->ts_period_ms;
    r->stats.gpu_present_ms += (double)(t[5] - t[4]) * r->ts_period_ms;
    r->stats.gpu_post_ms += (double)((t[5] - t[1]) + 0u) * r->ts_period_ms;
    r->stats.gpu_total_ms += (double)(closed_ticks + (t[5] - start)) * r->ts_period_ms;
    r->stats.gpu_samples++;
}

static void write_timestamp(jpt_vk_renderer_t* r, uint32_t query) {
    VkQueryPool pool = r->ts_pool[r->sc.frame_index];
    if (pool == VK_NULL_HANDLE) return;
    r->ctx.vk.vkCmdWriteTimestamp(r->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, query);
}

jpt_vk_renderer_t* jpt_vk_renderer_create(const jpt_vk_renderer_desc_t* desc, char* err, size_t err_cap) {
    jpt_vk_renderer_t* r;
    jpt_vk_context_desc_t cdesc;
    uint32_t win_w, win_h;

    if (!desc) return NULL;
    r = (jpt_vk_renderer_t*)calloc(1, sizeof *r);
    if (!r) return NULL;

    r->cfg = desc->cfg;
    r->collect_timings = desc->collect_timings ? 1 : 0;
    if (!r->cfg.guest_w) r->cfg.guest_w = 640;
    if (!r->cfg.guest_h) r->cfg.guest_h = 480;
    if (!r->cfg.internal_w) r->cfg.internal_w = r->cfg.guest_w;
    if (!r->cfg.internal_h) r->cfg.internal_h = r->cfg.guest_h;
    win_w = r->cfg.window_w ? r->cfg.window_w : r->cfg.internal_w;
    win_h = r->cfg.window_h ? r->cfg.window_h : r->cfg.internal_h;
    r->cfg.window_w = win_w;
    r->cfg.window_h = win_h;

    memset(&cdesc, 0, sizeof cdesc);
    cdesc.window = desc->window;
    cdesc.native_hwnd = desc->native_hwnd;
    cdesc.gpu_request = desc->gpu_request;
    cdesc.cache_path = desc->cache_path;
    cdesc.enable_validation = desc->enable_validation;
    cdesc.log_fn = desc->log_fn;
    cdesc.log_user = desc->log_user;

    if (!jpt_vk_context_init(&r->ctx, &cdesc)) {
        if (err && err_cap) snprintf(err, err_cap, "%s", r->ctx.last_error);
        free(r);
        return NULL;
    }

    r->active_msaa = jpt_vk_clamp_msaa(r->cfg.msaa ? r->cfg.msaa : 1u, r->ctx.gpu.msaa_color_counts,
                                       r->ctx.gpu.msaa_depth_counts);
    r->active_aniso =
        jpt_vk_clamp_anisotropy(r->cfg.texture_filter, r->ctx.gpu.has_anisotropy, r->ctx.gpu.max_anisotropy);
    jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->cfg.internal_w, r->cfg.internal_h, win_w, win_h,
                            r->cfg.stretch, &r->geom);

    if (!jpt_vk_swapchain_init(&r->sc, &r->ctx, win_w, win_h, r->cfg.vsync, r->fullscreen) ||
        !jpt_vk_shaders_create(&r->ctx, r->shaders) || !create_samplers(r) || !create_layouts_and_descriptors(r) ||
        !create_render_passes(r) || !create_frame_rings(r)) {
        if (err && err_cap) snprintf(err, err_cap, "Vulkan renderer initialization failed");
        jpt_vk_renderer_destroy(r);
        return NULL;
    }

    init_default_textures(r);
    create_timing_queries(r);
    if (!create_scene_targets(r, r->geom.internal_w, r->geom.internal_h)) {
        if (err && err_cap) snprintf(err, err_cap, "Vulkan scene target creation failed");
        jpt_vk_renderer_destroy(r);
        return NULL;
    }

    r->textures = (vk_tex_slot_t*)calloc(JPT_VK_INIT_TEXTURES, sizeof *r->textures);
    if (!r->textures) {
        if (err && err_cap) snprintf(err, err_cap, "Vulkan texture table allocation failed");
        jpt_vk_renderer_destroy(r);
        return NULL;
    }
    r->texture_cap = JPT_VK_INIT_TEXTURES;

    if (!create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_PRESENT_FRAG, 0, &r->present_pipe) ||
        !create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_PRESENT_FRAG, 1, &r->present_blend_pipe) ||
        !create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_PRESENT_FRAG, 3, &r->present_premul_pipe) ||
        !create_quad_pipeline(r, r->resolved_rp_load, JPT_VK_SH_COPY_FRAG, 1, &r->copy_blend_pipe) ||
        !create_quad_pipeline(r, r->resolved_rp_load, JPT_VK_SH_COPY_FRAG, 3, &r->copy_premul_pipe) ||
        !create_quad_pipeline(r, r->resolved_rp_load, JPT_VK_SH_COPY_FRAG, 0, &r->copy_opaque_pipe) ||
        !create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_COPY_FRAG, 1, &r->copy_present_blend_pipe) ||
        !create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_COPY_FRAG, 3, &r->copy_present_premul_pipe) ||
        !create_quad_pipeline(r, r->sc.present_pass, JPT_VK_SH_COPY_FRAG, 0, &r->copy_present_opaque_pipe) ||
        !create_quad_pipeline(r, r->r32_rp,
                              r->active_msaa > 1 ? JPT_VK_SH_AO_PREPARE_MSAA_FRAG : JPT_VK_SH_AO_PREPARE_FRAG, 0,
                              &r->ao_prepare_pipe) ||
        !create_quad_pipeline(r, r->r8_rp, JPT_VK_SH_AO_GENERATE_FRAG, 0, &r->ao_generate_pipe) ||
        !create_quad_pipeline(r, r->r8_rp, JPT_VK_SH_AO_BLUR_FRAG, 0, &r->ao_blur_pipe) ||
        !create_quad_pipeline(r, r->resolved_rp_load,
                              !r->ctx.gpu.depth_has_stencil ? JPT_VK_SH_AO_APPLY_FRAG
                              : r->active_msaa > 1          ? JPT_VK_SH_AO_APPLY_MASKED_MSAA_FRAG
                                                            : JPT_VK_SH_AO_APPLY_MASKED_FRAG,
                              2, &r->ao_apply_pipe) ||
        !create_quad_pipeline_ex(r, r->scene_rp_load, JPT_VK_SH_COPY_CPU_FRAG, 1, msaa_bits(r->active_msaa), 1,
                                 &r->overlay_blend_pipe) ||
        !create_quad_pipeline_ex(r, r->scene_rp_load, JPT_VK_SH_COPY_FRAG, 0, msaa_bits(r->active_msaa), 1,
                                 &r->overlay_opaque_pipe)) {
        if (err && err_cap) snprintf(err, err_cap, "Vulkan core pipeline creation failed");
        jpt_vk_renderer_destroy(r);
        return NULL;
    }

    r->next_tex_gen = 1;
    r->viewport.x = 0;
    r->viewport.y = 0;
    r->viewport.w = r->rt_w;
    r->viewport.h = r->rt_h;
    r->water_strength = 0.015f;
    r->water_rhw_metre = 1.0f;
    r->water_vp[0] = 0.0f;
    r->water_vp[1] = 0.0f;
    r->water_vp[2] = 1.0f;
    r->water_vp[3] = 1.0f;
    r->ao_level = r->cfg.ao_level;
    r->ao_tan_half_h = 0.57735f;

    if (!r->startup_logged) {
        char summary[384];
        jpt_vk_format_startup_summary(&r->ctx, r->sc.present_mode, r->sc.format, r->rt_w, r->rt_h, r->active_msaa,
                                      r->active_aniso, summary, sizeof summary);
        jpt_vk_log(&r->ctx, "%s", summary);
        r->startup_logged = 1;
    }
    return r;
}

static void destroy_pipe(jpt_vk_renderer_t* r, VkPipeline* p) {
    if (p && *p != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyPipeline(r->ctx.device, *p, NULL);
        *p = VK_NULL_HANDLE;
        r->ctx.lifetime.pipelines--;
    }
}

static void destroy_ui_resources(jpt_vk_renderer_t* r);

void jpt_vk_renderer_destroy(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk;
    if (!r) return;
    vk = &r->ctx.vk;

    if (r->ctx.device != VK_NULL_HANDLE) vk->vkDeviceWaitIdle(r->ctx.device);

    for (int i = 0; i < JPT_VK_MAX_PIPELINES; i++) {
        if (r->world_pipes[i].used) destroy_pipe(r, &r->world_pipes[i].pipeline);
    }
    destroy_pipe(r, &r->present_pipe);
    destroy_pipe(r, &r->present_blend_pipe);
    destroy_pipe(r, &r->present_premul_pipe);
    destroy_pipe(r, &r->copy_blend_pipe);
    destroy_pipe(r, &r->copy_premul_pipe);
    destroy_pipe(r, &r->copy_opaque_pipe);
    destroy_pipe(r, &r->copy_present_blend_pipe);
    destroy_pipe(r, &r->copy_present_premul_pipe);
    destroy_pipe(r, &r->copy_present_opaque_pipe);
    destroy_pipe(r, &r->ao_prepare_pipe);
    destroy_pipe(r, &r->ao_generate_pipe);
    destroy_pipe(r, &r->ao_blur_pipe);
    destroy_pipe(r, &r->ao_apply_pipe);
    destroy_pipe(r, &r->overlay_blend_pipe);
    destroy_pipe(r, &r->overlay_opaque_pipe);
    destroy_video_resources(r);
    destroy_poly_resources(r);
    destroy_rt_image(r, &r->overlay_img, &r->overlay_alloc, &r->overlay_view);
    destroy_rt_image(r, &r->rb_img, &r->rb_alloc, &r->rb_view);
    if (r->rb_buf != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rb_buf, &r->rb_buf_alloc);

    destroy_scene_targets(r);

    drain_retired_images(r, 1);
    free(r->retired);
    r->retired = NULL;
    r->retired_count = 0;
    r->retired_cap = 0;

    if (r->textures) {
        for (uint32_t i = 1; i < r->texture_cap; i++) {
            if (r->textures[i].alive && r->textures[i].standalone)
                destroy_rt_image(r, &r->textures[i].image, &r->textures[i].alloc, &r->textures[i].view);
        }
        free(r->textures);
        r->textures = NULL;
        r->texture_cap = 0;
    }

    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (r->ts_pool[i] != VK_NULL_HANDLE) vk->vkDestroyQueryPool(r->ctx.device, r->ts_pool[i], NULL);
    }
    for (int i = 0; i < JPT_LAYER_COUNT; i++) {
        destroy_rt_image(r, &r->layers[i].image, &r->layers[i].alloc, &r->layers[i].view);
    }
    for (int i = 0; i < JPT_VK_MAX_POOLS; i++) {
        if (r->pools[i].used) destroy_rt_image(r, &r->pools[i].image, &r->pools[i].alloc, &r->pools[i].view);
    }
    for (uint32_t i = 0; i < JPT_VK_WAVE_SLOTS; i++)
        destroy_rt_image(r, &r->waves[i].img, &r->waves[i].alloc, &r->waves[i].view);
    destroy_rt_image(r, &r->gamma_lut_img, &r->gamma_lut_alloc, &r->gamma_lut_view);

    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        for (uint32_t p = 0; p < r->rings[i].world_pool_count; p++) {
            if (r->rings[i].world_pools[p] != VK_NULL_HANDLE) {
                vk->vkDestroyDescriptorPool(r->ctx.device, r->rings[i].world_pools[p], NULL);
                r->ctx.lifetime.descriptor_pools--;
            }
        }
        free(r->rings[i].world_pools);
        r->rings[i].world_pools = NULL;
        r->rings[i].world_pool_count = 0;
        r->rings[i].world_pool_cap = 0;
        for (uint32_t e = 0; e < r->rings[i].extra_count; e++)
            jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rings[i].extra_buf[e], &r->rings[i].extra_alloc[e]);
        r->rings[i].extra_count = 0;
        jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rings[i].staging_buf, &r->rings[i].staging_alloc);
        jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rings[i].vb_buf, &r->rings[i].vb_alloc);
        jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rings[i].ib_buf, &r->rings[i].ib_alloc);
    }

    if (r->scene_rp_load != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->scene_rp_load, NULL);
        r->ctx.lifetime.render_passes--;
    }
    if (r->scene_rp_clear != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->scene_rp_clear, NULL);
        r->ctx.lifetime.render_passes--;
    }
    if (r->resolved_rp_load != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->resolved_rp_load, NULL);
        r->ctx.lifetime.render_passes--;
    }
    if (r->r32_rp != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->r32_rp, NULL);
        r->ctx.lifetime.render_passes--;
    }
    if (r->r8_rp != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->r8_rp, NULL);
        r->ctx.lifetime.render_passes--;
    }

    if (r->desc_pool != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorPool(r->ctx.device, r->desc_pool, NULL);
        r->ctx.lifetime.descriptor_pools--;
    }
    if (r->world_pl != VK_NULL_HANDLE) {
        vk->vkDestroyPipelineLayout(r->ctx.device, r->world_pl, NULL);
        r->ctx.lifetime.pipeline_layouts--;
    }
    if (r->quad_pl != VK_NULL_HANDLE) {
        vk->vkDestroyPipelineLayout(r->ctx.device, r->quad_pl, NULL);
        r->ctx.lifetime.pipeline_layouts--;
    }
    destroy_ui_resources(r);
    if (r->world_dsl != VK_NULL_HANDLE) vk->vkDestroyDescriptorSetLayout(r->ctx.device, r->world_dsl, NULL);
    if (r->quad_dsl != VK_NULL_HANDLE) vk->vkDestroyDescriptorSetLayout(r->ctx.device, r->quad_dsl, NULL);

    for (int i = 0; i < JPT_VK_WORLD_SAMPLERS; i++) {
        if (r->world_samplers[i] != VK_NULL_HANDLE) {
            vk->vkDestroySampler(r->ctx.device, r->world_samplers[i], NULL);
            r->ctx.lifetime.samplers--;
        }
    }
    if (r->linear_clamp_sampler != VK_NULL_HANDLE) {
        vk->vkDestroySampler(r->ctx.device, r->linear_clamp_sampler, NULL);
        r->ctx.lifetime.samplers--;
    }
    if (r->nearest_clamp_sampler != VK_NULL_HANDLE) {
        vk->vkDestroySampler(r->ctx.device, r->nearest_clamp_sampler, NULL);
        r->ctx.lifetime.samplers--;
    }

    jpt_vk_shaders_destroy(&r->ctx, r->shaders);
    jpt_vk_swapchain_shutdown(&r->sc);
    jpt_vk_context_shutdown(&r->ctx);
    free(r);
}

const jpt_vk_context_t* jpt_vk_renderer_context(const jpt_vk_renderer_t* r) { return r ? &r->ctx : NULL; }
const jpt_vk_lifetime_t* jpt_vk_renderer_lifetime(const jpt_vk_renderer_t* r) { return r ? &r->ctx.lifetime : NULL; }
void jpt_vk_renderer_get_stats(const jpt_vk_renderer_t* r, jpt_vk_renderer_stats_t* out) {
    if (r && out) *out = r->stats;
}
void jpt_vk_renderer_stats_add(jpt_vk_renderer_stats_t* total, const jpt_vk_renderer_stats_t* add) {
    if (!total || !add) return;
#define ADD(f) total->f += add->f
#define ADD_ARRAY(f)                                                       for (size_t i_ = 0; i_ < sizeof total->f / sizeof total->f[0]; i_++)     total->f[i_] += add->f[i_]
    ADD(draws);
    ADD(triangles);
    ADD(textures_uploaded);
    ADD(pipelines_created);
    ADD(frames_presented);
    ADD(water_batches);
    ADD(sky_batches);
    ADD(ao_passes);
    ADD(draw_calls_requested);
    ADD(draw_calls_recorded);
    ADD(draw_calls_merged);
    ADD_ARRAY(draws_skipped);
    ADD(pipeline_cache_hits);
    ADD(textures_created);
    ADD(standalone_textures_created);
    ADD(texture_updates);
    ADD(texture_upload_bytes);
    ADD_ARRAY(texture_failures);
    ADD(batch_flushes_texture_full);
    ADD(water_draws);
    ADD(sky_draws);
    ADD(textured_draws);
    ADD(alpha_test_draws);
    ADD(alpha_blend_draws);
    ADD(fog_draws);
    ADD(colorkey_draws);
    ADD(indexed_draws);
    ADD(clears);
    ADD(scene_passes);
    ADD(one_shot_submits);
    ADD(uploads_in_frame);
    ADD(overlay_batches);
    ADD_ARRAY(pass_breaks);
    ADD(readbacks_in_frame);
    ADD(scene_readback_rects);
    ADD(ui_draws);
    ADD(ui_vertices);
    ADD(ui_indices);
    ADD(ui_texture_binds);
    ADD(ui_texture_uploads);
    ADD(ui_frames);
    ADD(cpu_ui_ms);
    ADD(layer_rect_uploads);
    ADD(layer_rect_bytes);
    ADD(layer_rects);
    ADD(upload_fallback_no_frame);
    ADD(upload_fallback_staging_full);
    ADD(staging_extra_buffers);
    ADD_ARRAY(one_shot_by_reason);
    ADD_ARRAY(one_shot_ms_by_reason);
    ADD(descriptor_updates);
    ADD(descriptor_cache_hits);
    ADD(pools_reclaimed);
    ADD(overlays_drawn);
    ADD(scene_readbacks);
    ADD(scene_readback_pixels);
    ADD(frame_syncs);
    ADD(last_frame_copies);
    ADD(poly_draws);
    ADD(poly_uploads);
    ADD(poly_upload_bytes);
    ADD(poly_failures);
    ADD(last_frame_readbacks);
    ADD_ARRAY(cull_draws);
    ADD_ARRAY(zfunc_draws);
    ADD_ARRAY(alpha_func_draws);
    ADD_ARRAY(src_blend_draws);
    ADD_ARRAY(dst_blend_draws);
    ADD_ARRAY(prim_draws);
    ADD(cpu_frame_ms);
    ADD(cpu_draw_ms);
    ADD(cpu_pipeline_ms);
    ADD(cpu_upload_ms);
    ADD(cpu_descriptor_ms);
    ADD(cpu_submit_ms);
    ADD(cpu_present_ms);
    ADD(cpu_acquire_ms);
    ADD(cpu_sync_ms);
    ADD(cpu_one_shot_ms);
    ADD(cpu_readback_ms);
    ADD(gpu_scene_ms);
    ADD(gpu_ao_ms);
    ADD(gpu_layers_ms);
    ADD(gpu_present_ms);
    ADD(gpu_post_ms);
    ADD(gpu_total_ms);
    ADD(gpu_samples);
    ADD(gpu_frames_unsampled);
#define MAXF(f) if (add->f > total->f) total->f = add->f
    MAXF(cpu_sync_max_ms);
    MAXF(cpu_one_shot_max_ms);
    MAXF(cpu_readback_max_ms);
#undef MAXF
#undef ADD
#undef ADD_ARRAY
}

long jpt_vk_renderer_validation_errors(const jpt_vk_renderer_t* r) { return r ? r->ctx.validation_errors : 0; }
long jpt_vk_renderer_validation_warnings(const jpt_vk_renderer_t* r) { return r ? r->ctx.validation_warnings : 0; }
int jpt_vk_renderer_validation_enabled(const jpt_vk_renderer_t* r) { return r ? r->ctx.validation_enabled : 0; }

void jpt_vk_renderer_get_geometry(const jpt_vk_renderer_t* r, jpt_vk_frame_geometry_t* out) {
    if (r && out) *out = r->geom;
}
int jpt_vk_renderer_is_device_lost(const jpt_vk_renderer_t* r) { return r ? r->sc.health.device_lost : 1; }

void jpt_vk_renderer_on_resize(jpt_vk_renderer_t* r, uint32_t window_w, uint32_t window_h) {
    if (!r) return;
    r->cfg.window_w = window_w;
    r->cfg.window_h = window_h;
    jpt_vk_sc_on_resize(&r->sc.health, window_w, window_h);
    jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->rt_w, r->rt_h, window_w, window_h, r->cfg.stretch,
                            &r->geom);
}

void jpt_vk_renderer_set_present_options(jpt_vk_renderer_t* r, int vsync, int fullscreen, int stretch) {
    if (!r) return;
    r->cfg.vsync = vsync ? 1 : 0;
    r->fullscreen = fullscreen ? 1 : 0;
    r->cfg.stretch = stretch ? 1 : 0;
    jpt_vk_sc_on_settings(&r->sc.health, r->cfg.vsync, r->fullscreen);
    jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->rt_w, r->rt_h, r->cfg.window_w, r->cfg.window_h,
                            r->cfg.stretch, &r->geom);
}

void jpt_vk_renderer_set_guest_size(jpt_vk_renderer_t* r, uint32_t guest_w, uint32_t guest_h) {
    if (!r || !guest_w || !guest_h) return;
    r->cfg.guest_w = guest_w;
    r->cfg.guest_h = guest_h;
    jpt_vk_compute_geometry(guest_w, guest_h, r->rt_w, r->rt_h, r->cfg.window_w, r->cfg.window_h, r->cfg.stretch,
                            &r->geom);
}

int jpt_vk_renderer_set_internal_size(jpt_vk_renderer_t* r, uint32_t internal_w, uint32_t internal_h) {
    if (!r || !internal_w || !internal_h) return 0;
    if (r->rt_w == internal_w && r->rt_h == internal_h) return 1;
    if (r->in_frame) return 0;
    r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
    if (!create_scene_targets(r, internal_w, internal_h)) return 0;
    r->cfg.internal_w = internal_w;
    r->cfg.internal_h = internal_h;
    r->viewport.x = 0;
    r->viewport.y = 0;
    r->viewport.w = r->rt_w;
    r->viewport.h = r->rt_h;
    world_ds_invalidate(r);
    jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->rt_w, r->rt_h, r->cfg.window_w, r->cfg.window_h,
                            r->cfg.stretch, &r->geom);
    return 1;
}

void jpt_vk_renderer_set_debug_output(jpt_vk_renderer_t* r, uint32_t debug_flags) {
    if (r) r->debug_flags = debug_flags;
}

uint32_t jpt_vk_renderer_active_msaa(const jpt_vk_renderer_t* r) { return r ? r->active_msaa : 1u; }
float jpt_vk_renderer_active_anisotropy(const jpt_vk_renderer_t* r) { return r ? r->active_aniso : 1.0f; }
VkFormat jpt_vk_renderer_swapchain_format(const jpt_vk_renderer_t* r) { return r ? r->sc.format : VK_FORMAT_UNDEFINED; }
VkPresentModeKHR jpt_vk_renderer_present_mode(const jpt_vk_renderer_t* r) {
    return r ? r->sc.present_mode : VK_PRESENT_MODE_FIFO_KHR;
}

void jpt_vk_renderer_set_gamma_ramp(jpt_vk_renderer_t* r, const uint16_t red[256], const uint16_t green[256],
                                    const uint16_t blue[256]) {
    if (!r || !red || !green || !blue) return;
    r->gamma_identity = 1;
    for (int i = 0; i < 256; i++) {
        if ((red[i] >> 8) != i || (green[i] >> 8) != i || (blue[i] >> 8) != i) r->gamma_identity = 0;
        r->gamma_rgba[i * 4 + 0] = (uint8_t)(red[i] >> 8);
        r->gamma_rgba[i * 4 + 1] = (uint8_t)(green[i] >> 8);
        r->gamma_rgba[i * 4 + 2] = (uint8_t)(blue[i] >> 8);
        r->gamma_rgba[i * 4 + 3] = 255;
    }
    r->gamma_dirty = 1;
}

static void pool_slice_set(vk_tex_pool_t* p, uint32_t s) {
    p->slice_mask[s >> 6] |= (1ULL << (s & 63u));
    p->live++;
}
static void pool_slice_clear(vk_tex_pool_t* p, uint32_t s) {
    p->slice_mask[s >> 6] &= ~(1ULL << (s & 63u));
    if (p->live) p->live--;
}

static uint32_t pool_slices_for(uint32_t w, uint32_t h) {
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    uint64_t n = bytes ? JPT_VK_POOL_BUDGET / bytes : JPT_VK_POOL_MAX_SLICES;
    if (n < JPT_VK_POOL_MIN_SLICES) n = JPT_VK_POOL_MIN_SLICES;
    if (n > JPT_VK_POOL_MAX_SLICES) n = JPT_VK_POOL_MAX_SLICES;
    return (uint32_t)(n & ~63ull);
}
static int pool_slice_used(const vk_tex_pool_t* p, uint32_t s) {
    return (p->slice_mask[s >> 6] & (1ULL << (s & 63u))) != 0;
}

static int find_or_create_pool(jpt_vk_renderer_t* r, uint32_t w, uint32_t h, uint32_t levels, uint32_t* out_slot,
                               uint32_t* out_slice) {
    for (int i = 1; i < JPT_VK_MAX_POOLS; i++) {
        if (r->pools[i].used && r->pools[i].width == w && r->pools[i].height == h && r->pools[i].mip_levels == levels) {
            for (uint32_t s = 0; s < r->pools[i].max_slices; s++) {
                if (!pool_slice_used(&r->pools[i], s)) {
                    pool_slice_set(&r->pools[i], s);
                    *out_slot = (uint32_t)i;
                    *out_slice = s;
                    return 1;
                }
            }
        }
    }
    for (int i = 1; i < JPT_VK_MAX_POOLS; i++) {
        if (!r->pools[i].used) {
            if (!create_tex_pool(r, i, w, h, levels, pool_slices_for(w, h))) return 0;
            pool_slice_set(&r->pools[i], 0);
            *out_slot = (uint32_t)i;
            *out_slice = 0;
            return 1;
        }
    }
    return 0;
}

static void end_scene_pass_if_open(jpt_vk_renderer_t* r);
static void ensure_scene_pass(jpt_vk_renderer_t* r, int clear_now, const float clear_rgba[4], float clear_depth,
                              uint32_t clear_stencil);

static uint64_t texture_fail(jpt_vk_renderer_t* r, jpt_vk_texture_error_t e) {
    r->tex_error = e;
    r->stats.texture_failures[e]++;
    return 0;
}

static int texture_upload_has_alpha(const jpt_render_tex_desc_t* desc) {
    if (desc->format == JPT_TEXFMT_BGRA8888_PREPARED) return desc->has_alpha ? 1 : 0;
    return desc->format == JPT_TEXFMT_ARGB1555 || desc->format == JPT_TEXFMT_ARGB4444 ||
           desc->format == JPT_TEXFMT_ARGB8888 || desc->has_colorkey;
}

static int alloc_tex_slot(jpt_vk_renderer_t* r, uint32_t* out_idx) {
    for (uint32_t i = 1; i < r->texture_cap; i++) {
        if (!r->textures[i].alive) {
            *out_idx = i;
            return 1;
        }
    }
    uint32_t new_cap = r->texture_cap ? r->texture_cap * 2u : JPT_VK_INIT_TEXTURES;
    vk_tex_slot_t* grown = (vk_tex_slot_t*)realloc(r->textures, (size_t)new_cap * sizeof *grown);
    if (!grown) return 0;
    memset(grown + r->texture_cap, 0, (size_t)(new_cap - r->texture_cap) * sizeof *grown);
    *out_idx = r->texture_cap ? r->texture_cap : 1u;
    r->textures = grown;
    r->texture_cap = new_cap;
    return 1;
}

static int create_standalone_texture(jpt_vk_renderer_t* r, uint32_t w, uint32_t h, uint32_t levels, vk_tex_slot_t* t) {
    VkImageCreateInfo ici;
    VkImageViewCreateInfo ivci;

    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (jpt_vk_create_image(&r->ctx.alloc, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->image, &t->alloc) !=
        VK_SUCCESS)
        return 0;

    memset(&ivci, 0, sizeof ivci);
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = t->image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = levels;
    ivci.subresourceRange.layerCount = 1;
    if (r->ctx.vk.vkCreateImageView(r->ctx.device, &ivci, NULL, &t->view) != VK_SUCCESS) {
        jpt_vk_destroy_image(&r->ctx.alloc, &t->image, &t->alloc);
        return 0;
    }
    r->ctx.lifetime.image_views++;
    t->standalone = 1;
    t->pool_slot = 0;
    t->slice = 0;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (levels > 1) {
        pool_init_job_t job = { t->image, 1, levels };
        run_one_shot_cmd(r, record_pool_init, &job, JPT_VK_OS_RESOURCE_INIT);
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    r->stats.standalone_textures_created++;
    return 1;
}

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1u;
}

static size_t chain_texels(uint32_t w, uint32_t h, uint32_t levels) {
    size_t n = 0;
    for (uint32_t i = 0; i < levels; i++) n += (size_t)level_extent(w, i) * level_extent(h, i);
    return n;
}

static int upload_chain(jpt_vk_renderer_t* r, VkImage img, VkImageLayout* layout, uint32_t slice, uint32_t w, uint32_t h,
                        uint32_t levels, const uint8_t* rgba) {
    size_t at = 0;
    for (uint32_t i = 0; i < levels; i++) {
        uint32_t lw = level_extent(w, i), lh = level_extent(h, i);
        size_t bytes = (size_t)lw * lh * 4u;
        if (!upload_rgba_to_image_level(r, img, layout, i, 0, 0, lw, lh, slice, rgba + at, bytes)) return 0;
        at += bytes;
    }
    return 1;
}

static void convert_chain_to_rgba8(const jpt_render_tex_desc_t* desc, const void* pixels, uint8_t* dst) {
    const uint8_t* src = (const uint8_t*)pixels;
    size_t at = 0;
    for (uint32_t i = 0; i < desc->mip_levels; i++) {
        jpt_render_tex_desc_t level = *desc;
        uint32_t lw = level_extent(desc->width, i), lh = level_extent(desc->height, i);
        level.width = lw;
        level.height = lh;
        level.pitch = lw * 4u;
        convert_pixels_to_rgba8(&level, src + at, dst + at);
        at += (size_t)lw * lh * 4u;
    }
}

uint64_t jpt_vk_renderer_create_texture(jpt_vk_renderer_t* r, const jpt_render_tex_desc_t* desc, const void* pixels) {
    uint32_t slot = 0, slice = 0, idx = 0;
    uint8_t* rgba = NULL;
    size_t rgba_bytes;
    int was_open, ok, use_standalone = 0;
    uint32_t levels;
    vk_tex_slot_t tmp;
    double t0;

    if (!r) return 0;
    r->tex_error = JPT_VK_TEX_OK;
    if (!desc || !desc->width || !desc->height || !pixels) return texture_fail(r, JPT_VK_TEX_BAD_ARGS);
    t0 = r->collect_timings ? now_ms() : 0.0;
    if (!alloc_tex_slot(r, &idx)) return texture_fail(r, JPT_VK_TEX_NO_SLOT);

    levels = desc->mip_levels > 1u ? desc->mip_levels : 1u;
    rgba_bytes = chain_texels(desc->width, desc->height, levels) * 4u;
    rgba = (uint8_t*)malloc(rgba_bytes);
    if (!rgba) return texture_fail(r, JPT_VK_TEX_NO_MEMORY);
    if (levels > 1u) convert_chain_to_rgba8(desc, pixels, rgba);
    else convert_pixels_to_rgba8(desc, pixels, rgba);

    was_open = r->in_scene_pass;
    if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_TEXTURE_UPLOAD);

    memset(&tmp, 0, sizeof tmp);
    if (rgba_bytes > JPT_VK_POOL_BUDGET / 2u || !find_or_create_pool(r, desc->width, desc->height, levels, &slot, &slice)) {
        if (!create_standalone_texture(r, desc->width, desc->height, levels, &tmp)) {
            if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
            free(rgba);
            return texture_fail(r, JPT_VK_TEX_NO_POOL);
        }
        use_standalone = 1;
    }

    if (use_standalone) {
        ok = upload_chain(r, tmp.image, &tmp.layout, 0, desc->width, desc->height, levels, rgba);
    } else {
        ok = upload_chain(r, r->pools[slot].image, &r->pools[slot].layout, slice, desc->width, desc->height, levels, rgba);
    }
    if (!ok) {
        if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
        free(rgba);
        if (use_standalone)
            retire_image(r, &tmp.image, &tmp.alloc, &tmp.view);
        else
            pool_slice_clear(&r->pools[slot], slice);
        return texture_fail(r, JPT_VK_TEX_UPLOAD);
    }
    if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    free(rgba);

    tmp.alive = 1;
    tmp.generation = r->next_tex_gen++;
    if (!use_standalone) {
        tmp.standalone = 0;
        tmp.pool_slot = slot;
        tmp.slice = slice;
    }
    tmp.width = desc->width;
    tmp.height = desc->height;
    tmp.levels = levels;
    tmp.has_alpha = texture_upload_has_alpha(desc) ? 1 : 0;
    tmp.has_colorkey = desc->has_colorkey ? 1 : 0;
    r->textures[idx] = tmp;

    r->stats.textures_uploaded++;
    r->stats.textures_created++;
    r->stats.texture_upload_bytes += rgba_bytes;
    if (r->collect_timings) r->stats.cpu_upload_ms += now_ms() - t0;
    return ((uint64_t)r->textures[idx].generation << 32) | idx;
}

jpt_vk_texture_error_t jpt_vk_renderer_texture_error(const jpt_vk_renderer_t* r) {
    return r ? r->tex_error : JPT_VK_TEX_BAD_ARGS;
}

const char* jpt_vk_texture_error_name(jpt_vk_texture_error_t e) {
    static const char* const names[JPT_VK_TEX_ERRORS] = { "ok",
                                                          "invalid arguments",
                                                          "no free texture slot",
                                                          "no texture pool",
                                                          "out of host memory",
                                                          "upload failed",
                                                          "stale texture id" };
    return (unsigned)e < JPT_VK_TEX_ERRORS ? names[e] : "unknown";
}

const char* jpt_vk_skip_reason_name(jpt_vk_draw_result_t reason) {
    static const char* const names[JPT_VK_SKIP_REASONS] = { "submitted",        "no open frame",    "invalid draw",
                                                            "no pipeline",      "vertex ring full", "index ring full",
                                                            "no indices produced" };
    return (unsigned)reason < JPT_VK_SKIP_REASONS ? names[reason] : "unknown";
}

int jpt_vk_renderer_texture_alive(const jpt_vk_renderer_t* r, uint64_t tex_id) {
    uint32_t idx = (uint32_t)(tex_id & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(tex_id >> 32);
    return r && r->textures && idx > 0 && idx < r->texture_cap && r->textures[idx].alive &&
           r->textures[idx].generation == gen;
}

int jpt_vk_renderer_update_texture(jpt_vk_renderer_t* r, uint64_t tex_id, const jpt_render_tex_desc_t* desc,
                                   const void* pixels, const jpt_render_rect_t* dirty_rect) {
    uint32_t idx = (uint32_t)(tex_id & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(tex_id >> 32);
    vk_tex_slot_t* t;
    uint8_t* rgba;
    size_t sent_bytes;
    int was_open, ok, chain;
    uint32_t ux = 0, uy = 0, uw, uh;
    double t0;

    if (!r) return 0;
    r->tex_error = JPT_VK_TEX_OK;
    if (!desc || !pixels || !r->textures || idx == 0 || idx >= r->texture_cap)
        return (int)texture_fail(r, JPT_VK_TEX_BAD_ARGS);
    t = &r->textures[idx];
    if (!t->alive || t->generation != gen || desc->width != t->width || desc->height != t->height)
        return (int)texture_fail(r, JPT_VK_TEX_STALE);
    t0 = r->collect_timings ? now_ms() : 0.0;

    uw = desc->width;
    uh = desc->height;
    if (dirty_rect && dirty_rect->w > 0 && dirty_rect->h > 0 && dirty_rect->x >= 0 && dirty_rect->y >= 0 &&
        (uint32_t)dirty_rect->x + dirty_rect->w <= desc->width &&
        (uint32_t)dirty_rect->y + dirty_rect->h <= desc->height) {
        ux = (uint32_t)dirty_rect->x;
        uy = (uint32_t)dirty_rect->y;
        uw = dirty_rect->w;
        uh = dirty_rect->h;
    }
    chain = desc->mip_levels > 1u;
    if (chain && desc->mip_levels != t->levels) return (int)texture_fail(r, JPT_VK_TEX_STALE);
    if (chain) {
        ux = uy = 0;
        uw = desc->width;
        uh = desc->height;
    }
    sent_bytes = (chain ? chain_texels(uw, uh, desc->mip_levels) : (size_t)uw * uh) * 4u;
    rgba = (uint8_t*)malloc(sent_bytes);
    if (!rgba) return (int)texture_fail(r, JPT_VK_TEX_NO_MEMORY);
    if (chain) convert_chain_to_rgba8(desc, pixels, rgba);
    else convert_region_to_rgba8(desc, pixels, ux, uy, uw, uh, rgba);
    t->has_alpha = texture_upload_has_alpha(desc) ? 1 : 0;
    t->has_colorkey = desc->has_colorkey ? 1 : 0;

    was_open = r->in_scene_pass;
    if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_TEXTURE_UPLOAD);
    if (chain) {
        ok = t->standalone ? upload_chain(r, t->image, &t->layout, 0, uw, uh, desc->mip_levels, rgba)
                           : upload_chain(r, r->pools[t->pool_slot].image, &r->pools[t->pool_slot].layout, t->slice, uw, uh,
                                          desc->mip_levels, rgba);
    } else if (t->standalone) {
        ok = upload_rgba_to_image(r, t->image, &t->layout, ux, uy, uw, uh, 0, rgba, sent_bytes);
    } else {
        ok = upload_rgba_to_image(r, r->pools[t->pool_slot].image, &r->pools[t->pool_slot].layout, ux, uy, uw, uh,
                                  t->slice, rgba, sent_bytes);
    }
    if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    free(rgba);
    if (!ok) return (int)texture_fail(r, JPT_VK_TEX_UPLOAD);
    r->stats.textures_uploaded++;
    r->stats.texture_updates++;
    r->stats.texture_upload_bytes += sent_bytes;
    if (r->collect_timings) r->stats.cpu_upload_ms += now_ms() - t0;
    return 1;
}

void jpt_vk_renderer_destroy_texture(jpt_vk_renderer_t* r, uint64_t tex_id) {
    uint32_t idx = (uint32_t)(tex_id & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(tex_id >> 32);
    if (!r || !r->textures || idx == 0 || idx >= r->texture_cap) return;
    vk_tex_slot_t* t = &r->textures[idx];
    if (!t->alive || t->generation != gen) return;
    if (t->standalone) {
        retire_image(r, &t->image, &t->alloc, &t->view);
    } else {
        pool_slice_clear(&r->pools[t->pool_slot], t->slice);
    }
    t->alive = 0;
    t->standalone = 0;
}

static void end_scene_pass_if_open(jpt_vk_renderer_t* r) {
    if (r->in_scene_pass) {
        r->stats.pass_breaks[r->break_reason]++;
        r->break_reason = JPT_VK_BREAK_OTHER;
        flush_batch(r);
        r->ctx.vk.vkCmdEndRenderPass(r->cmd);
        r->in_scene_pass = 0;
    }
}

static void ensure_scene_pass(jpt_vk_renderer_t* r, int clear_now, const float clear_rgba[4], float clear_depth,
                              uint32_t clear_stencil) {
    VkRenderPassBeginInfo rpbi;
    VkClearValue clears[2];
    if (r->in_scene_pass) return;

    memset(&rpbi, 0, sizeof rpbi);
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = clear_now ? r->scene_rp_clear : r->scene_rp_load;
    rpbi.framebuffer = r->scene_fb;
    rpbi.renderArea.extent.width = r->rt_w;
    rpbi.renderArea.extent.height = r->rt_h;
    if (clear_now) {
        clears[0].color.float32[0] = clear_rgba[0];
        clears[0].color.float32[1] = clear_rgba[1];
        clears[0].color.float32[2] = clear_rgba[2];
        clears[0].color.float32[3] = clear_rgba[3];
        clears[1].depthStencil.depth = clear_depth;
        clears[1].depthStencil.stencil = clear_stencil;
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
    }
    r->ctx.vk.vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    r->world_bound_valid = 0;
    r->in_scene_pass = 1;
    r->stats.scene_passes++;
}

int jpt_vk_renderer_begin_frame(jpt_vk_renderer_t* r) {
    jpt_vk_sc_action_t act;
    double acquire_start;
    if (!r || r->sc.health.device_lost) return 0;

    if (r->sc.health.dirty) {
        if (r->sc.health.surface_lost) jpt_vk_context_recreate_surface(&r->ctx);
        if (r->cfg.window_w == 0 || r->cfg.window_h == 0) return 0;
        if (!jpt_vk_swapchain_recreate(&r->sc, r->cfg.window_w, r->cfg.window_h, r->cfg.vsync, r->fullscreen))
            return 0;
        jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->rt_w, r->rt_h, r->sc.extent.width,
                                r->sc.extent.height, r->cfg.stretch, &r->geom);
    }

    acquire_start = now_ms();
    act = jpt_vk_swapchain_begin_frame(&r->sc, &r->cmd, &r->present_fb, &r->present_extent);
    if (act == JPT_VK_SC_ACTION_RECREATE || act == JPT_VK_SC_ACTION_RECREATE_SURFACE) {
        if (act == JPT_VK_SC_ACTION_RECREATE_SURFACE) jpt_vk_context_recreate_surface(&r->ctx);
        if (!jpt_vk_swapchain_recreate(&r->sc, r->cfg.window_w, r->cfg.window_h, r->cfg.vsync, r->fullscreen))
            return 0;
        act = jpt_vk_swapchain_begin_frame(&r->sc, &r->cmd, &r->present_fb, &r->present_extent);
    }
    r->stats.cpu_acquire_ms += now_ms() - acquire_start;
    if (act != JPT_VK_SC_ACTION_OK) return 0;

    vk_frame_rings_t* ring = &r->rings[r->sc.frame_index];
    ring->staging_used = 0;
    reset_staging_extras(r, ring);
    ring->vb_used = 0;
    ring->ib_used = 0;
    r->poly_ubo_used = 0;
    reset_frame_world_pools(r, ring);
    drain_retired_images(r, 0);

    reclaim_empty_pools(r);

    r->in_frame = 1;
    r->in_scene_pass = 0;
    r->batch.active = 0;
    r->batch.view_count = 0;
    r->scene_has_draws = 0;
    r->scene_has_depth_draws = 0;
    r->scene_behind_copied = 0;
    r->scene_resolved_ready = 0;
    r->ts_invalid_frame = 0;
    r->ts_segments = 0;

    if (r->collect_timings) {
        read_timing_queries(r, (int)r->sc.frame_index);
        if (r->ts_pool[r->sc.frame_index] != VK_NULL_HANDLE) {
            r->ctx.vk.vkCmdResetQueryPool(r->cmd, r->ts_pool[r->sc.frame_index], 0, JPT_VK_TS_TOTAL);
            write_timestamp(r, 0);
        }
        r->frame_start_ms = now_ms();
    }

    if (r->gamma_dirty) {
        VkImageLayout l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        upload_rgba_to_image(r, r->gamma_lut_img, &l, 0, 0, 256, 1, 0, r->gamma_rgba, sizeof r->gamma_rgba);
        r->gamma_dirty = 0;
    }

    {
        float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ensure_scene_pass(r, 1, black, 0.0f, 0);
    }
    return 1;
}

void jpt_vk_renderer_clear(jpt_vk_renderer_t* r, int clear_color, uint32_t argb, int clear_depth, float depth,
                           int clear_stencil, uint32_t stencil) {
    VkClearAttachment atts[2];
    uint32_t count = 0;
    VkClearRect rect;

    if (!r || !r->in_frame) return;
    if (!r->in_scene_pass) {
        float c[4] = { ((argb >> 16) & 0xFFu) / 255.0f, ((argb >> 8) & 0xFFu) / 255.0f, (argb & 0xFFu) / 255.0f,
                       ((argb >> 24) & 0xFFu) / 255.0f };
        ensure_scene_pass(r, 1, c, depth, stencil);
        return;
    }

    if (clear_color) {
        memset(&atts[count], 0, sizeof atts[count]);
        atts[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[count].colorAttachment = 0;
        atts[count].clearValue.color.float32[0] = ((argb >> 16) & 0xFFu) / 255.0f;
        atts[count].clearValue.color.float32[1] = ((argb >> 8) & 0xFFu) / 255.0f;
        atts[count].clearValue.color.float32[2] = (argb & 0xFFu) / 255.0f;
        atts[count].clearValue.color.float32[3] = ((argb >> 24) & 0xFFu) / 255.0f;
        count++;
    }
    if (clear_color && !clear_depth && !clear_stencil && r->ctx.gpu.depth_has_stencil) {
        clear_stencil = 1;
        stencil = 1;
    }
    if (clear_depth || (clear_stencil && r->ctx.gpu.depth_has_stencil)) {
        memset(&atts[count], 0, sizeof atts[count]);
        atts[count].aspectMask = (clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                                 ((clear_stencil && r->ctx.gpu.depth_has_stencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        atts[count].clearValue.depthStencil.depth = depth;
        atts[count].clearValue.depthStencil.stencil = stencil;
        count++;
    }
    if (!count) return;
    flush_batch(r);
    r->stats.clears++;
    memset(&rect, 0, sizeof rect);
    rect.rect.offset.x = r->scissor_enabled ? r->scissor.x : 0;
    rect.rect.offset.y = r->scissor_enabled ? r->scissor.y : 0;
    rect.rect.extent.width = r->scissor_enabled ? r->scissor.w : r->rt_w;
    rect.rect.extent.height = r->scissor_enabled ? r->scissor.h : r->rt_h;
    rect.layerCount = 1;
    r->ctx.vk.vkCmdClearAttachments(r->cmd, count, atts, 1, &rect);
}

void jpt_vk_renderer_set_viewport(jpt_vk_renderer_t* r, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!r) return;
    r->viewport.x = x;
    r->viewport.y = y;
    r->viewport.w = w ? w : r->rt_w;
    r->viewport.h = h ? h : r->rt_h;
}

void jpt_vk_renderer_set_scissor(jpt_vk_renderer_t* r, int enabled, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!r) return;
    r->scissor_enabled = enabled ? 1 : 0;
    r->scissor.x = x;
    r->scissor.y = y;
    r->scissor.w = w;
    r->scissor.h = h;
}

static uint16_t float_to_half(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    uint32_t sign = (u >> 16) & 0x8000u;
    int exp = (int)((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

void jpt_vk_renderer_select_waves(jpt_vk_renderer_t* r, uint32_t slot) {
    if (!r || slot >= JPT_VK_WAVE_SLOTS || r->wave_slot == slot) return;
    flush_batch(r);
    r->wave_slot = slot;
    r->water_waves_view = r->waves[slot].view != VK_NULL_HANDLE ? r->waves[slot].view : r->waves[0].view;
}

void jpt_vk_renderer_set_water_params(jpt_vk_renderer_t* r, float strength, float rhw_metre, float vp_u0, float vp_v0,
                                      float vp_u1, float vp_v1, const float* waves_rg, uint32_t waves_w,
                                      uint32_t waves_h) {
    if (!r) return;
    r->water_strength = strength;
    r->water_rhw_metre = rhw_metre > 1e-6f ? rhw_metre : 1.0f;
    r->water_vp[0] = vp_u0;
    r->water_vp[1] = vp_v0;
    r->water_vp[2] = vp_u1;
    r->water_vp[3] = vp_v1;

    if (waves_rg && waves_w > 0 && waves_h > 0) {
        const uint32_t slot = r->wave_slot;
        if (waves_w != r->waves[slot].w || waves_h != r->waves[slot].h) {
            VkImage img = VK_NULL_HANDLE;
            jpt_vk_allocation_t alloc;
            VkImageView view = VK_NULL_HANDLE;
            memset(&alloc, 0, sizeof alloc);
            if (!create_image_2d(r, waves_w, waves_h, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, &img, &alloc, &view))
                return;
            flush_batch(r);
            retire_image(r, &r->waves[slot].img, &r->waves[slot].alloc, &r->waves[slot].view);
            r->waves[slot].img = img;
            r->waves[slot].alloc = alloc;
            r->waves[slot].view = view;
            r->waves[slot].w = waves_w;
            r->waves[slot].h = waves_h;
            r->waves[slot].layout = VK_IMAGE_LAYOUT_UNDEFINED;
            r->water_waves_view = view;
            world_ds_invalidate(r);
        }
        size_t count = (size_t)waves_w * (size_t)waves_h * 2u;
        uint16_t* halfs = (uint16_t*)malloc(count * sizeof(uint16_t));
        if (halfs) {
            for (size_t i = 0; i < count; i++) halfs[i] = float_to_half(waves_rg[i]);
            int was_open = r->in_scene_pass;
            if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_WATER);
            upload_rgba_to_image(r, r->waves[slot].img, &r->waves[slot].layout, 0, 0, waves_w, waves_h, 0, halfs,
                                 count * sizeof(uint16_t));
            if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
            free(halfs);
        }
    }
}

void jpt_vk_renderer_set_sky_params(jpt_vk_renderer_t* r, float cam_u, float cam_v, float plane_h, float fog_lo,
                                    float fog_hi) {
    if (!r) return;
    r->sky_cam_u = cam_u;
    r->sky_cam_v = cam_v;
    r->sky_plane_h = plane_h;
    r->sky_fog_lo = fog_lo;
    r->sky_fog_hi = fog_hi;
}

void jpt_vk_renderer_set_ao_params(jpt_vk_renderer_t* r, int level, float tan_half_h, int gameplay) {
    if (!r) return;
    r->ao_level = level;
    if (tan_half_h > 0.01f) r->ao_tan_half_h = tan_half_h;
    r->ao_gameplay = gameplay ? 1 : 0;
}

static void capture_scene_behind_for_water(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkImageCopy copy;
    if (r->scene_behind_copied) return;
    end_scene_pass_for(r, JPT_VK_BREAK_WATER);

    if (r->active_msaa > 1) {
        VkImageResolve res;
        cmd_image_barrier(vk, r->cmd, r->scene_color, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        cmd_image_barrier(vk, r->cmd, r->scene_behind, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
        memset(&res, 0, sizeof res);
        res.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.srcSubresource.layerCount = 1;
        res.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.dstSubresource.layerCount = 1;
        res.extent.width = r->rt_w;
        res.extent.height = r->rt_h;
        res.extent.depth = 1;
        vk->vkCmdResolveImage(r->cmd, r->scene_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->scene_behind,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
        cmd_image_barrier(vk, r->cmd, r->scene_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 1);
    } else {
        cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        cmd_image_barrier(vk, r->cmd, r->scene_behind, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
        memset(&copy, 0, sizeof copy);
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent.width = r->rt_w;
        copy.extent.height = r->rt_h;
        copy.extent.depth = 1;
        vk->vkCmdCopyImage(r->cmd, r->scene_resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->scene_behind,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 1);
    }
    cmd_image_barrier(vk, r->cmd, r->scene_behind, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
    r->scene_behind_copied = 1;
    ensure_scene_pass(r, 0, NULL, 0.0f, 0);
}

static const vk_tex_slot_t* lookup_tex_slot(const jpt_vk_renderer_t* r, const jpt_render_state_t* st) {
    if (!st->has_texture || !st->texture_id || !r->textures) return NULL;
    uint32_t idx = (uint32_t)(st->texture_id & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(st->texture_id >> 32);
    if (idx == 0 || idx >= r->texture_cap) return NULL;
    if (!r->textures[idx].alive || r->textures[idx].generation != gen) return NULL;
    return &r->textures[idx];
}

static uint32_t encode_layer_word(const jpt_vk_renderer_t* r, const jpt_render_state_t* st, const vk_tex_slot_t* t,
                                  uint32_t batch_slot) {
    if (!t) return 0;
    uint32_t tf = 1u; /* F_TEXTURED */
    if (t->has_alpha) tf |= 2u;
    if (t->has_colorkey && st->colorkey_enable) tf |= 4u;
    uint32_t samp = 0;
    int cu = (st->tex_addr_u == 3u); /* D3DTADDRESS_CLAMP */
    int cv = (st->tex_addr_v == 3u);
    int game_mag = st->tex_mag_filter >= 2u && st->tex_mag_filter <= 5u;
    int game_min = st->tex_min_filter == 2u || st->tex_min_filter == 3u;
    int forced = r->cfg.texture_filter == JPT_FILTER_BILINEAR;
    int mag = forced ? 1 : game_mag, min = forced ? 1 : game_min;
    int mips = r->cfg.texture_filter != JPT_FILTER_ORIGINAL && mag && min;
    uint32_t addr = (cu && cv) ? 3u : cv ? 2u : cu ? 1u : 0u;
    uint32_t filter_class = (!mag && !min) ? 1u : (mag && min) ? (mips ? 0u : 4u) : mag ? 2u : 3u;
    samp = filter_class * 4u + addr;
    uint32_t slice = t->standalone ? 0u : t->slice;
    return (slice & 0xFFFFu) | ((batch_slot & 15u) << 16u) | ((tf & 7u) << 20u) | ((samp & 31u) << 24u);
}

static int batch_find_view(const vk_batch_t* b, VkImageView v) {
    for (uint32_t i = 0; i < b->view_count; i++) {
        if (b->views[i] == v) return (int)i;
    }
    return -1;
}

static void flush_batch(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    vk_batch_t* b = &r->batch;
    vk_frame_rings_t* ring;
    VkImageView full_views[16];
    VkDescriptorSet ds;
    double t0;

    if (!b->active) return;
    b->active = 0;
    if (!r->in_frame || !r->in_scene_pass || !b->indices) {
        b->view_count = 0;
        return;
    }
    for (uint32_t i = 0; i < 16u; i++) {
        full_views[i] = (i < b->view_count && b->views[i] != VK_NULL_HANDLE) ? b->views[i] : r->pools[0].view;
    }
    b->view_count = 0;
    ds = acquire_world_descriptor_set(r, full_views);
    if (ds == VK_NULL_HANDLE) return;

    t0 = r->collect_timings ? now_ms() : 0.0;
    ring = &r->rings[r->sc.frame_index];
    {
        const int same = r->world_bound_valid;
        if (!same || r->world_bound_pipe != b->pipe) vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b->pipe);
        if (!same || memcmp(&r->world_bound_vp, &b->vp, sizeof b->vp)) vk->vkCmdSetViewport(r->cmd, 0, 1, &b->vp);
        if (!same || memcmp(&r->world_bound_sc, &b->sc, sizeof b->sc)) vk->vkCmdSetScissor(r->cmd, 0, 1, &b->sc);
        if (!same || r->world_bound_bias != b->bias) vk->vkCmdSetDepthBias(r->cmd, b->bias, 0.0f, 0.0f);
        if (!same || r->world_bound_ds != ds)
            vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->world_pl, 0, 1, &ds, 0, NULL);
        if (!same || memcmp(&r->world_bound_push, &b->push, sizeof b->push))
            vk->vkCmdPushConstants(r->cmd, r->world_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof b->push, &b->push);
        r->world_bound_valid = 1;
        r->world_bound_pipe = b->pipe;
        r->world_bound_vp = b->vp;
        r->world_bound_sc = b->sc;
        r->world_bound_bias = b->bias;
        r->world_bound_ds = ds;
        r->world_bound_push = b->push;
    }
    vk->vkCmdBindVertexBuffers(r->cmd, 0, 1, &ring->vb_buf, &b->vb_off);
    vk->vkCmdBindIndexBuffer(r->cmd, ring->ib_buf, b->ib_off, VK_INDEX_TYPE_UINT32);
    vk->vkCmdDrawIndexed(r->cmd, b->indices, 1, 0, 0, 0);
    r->stats.draw_calls_recorded++;
    if (r->collect_timings) r->stats.cpu_draw_ms += now_ms() - t0;
}

static float depth_bias_for(const jpt_render_state_t* state) {
    uint32_t steps = state->z_bias > 16u ? 16u : state->z_bias;
    float units = JPT_VK_DEPTH_BIAS_UNITS_PER_STEP * (float)steps;
    int towards_greater = state->z_func == 5u || state->z_func == 7u;
    return towards_greater ? units : -units;
}

static jpt_vk_draw_result_t skip_draw(jpt_vk_renderer_t* r, jpt_vk_draw_result_t reason) {
    r->stats.draws_skipped[reason]++;
    return reason;
}

static void count_draw_state(jpt_vk_renderer_t* r, const jpt_render_state_t* state, jpt_render_prim_t prim,
                             uint32_t index_count) {
    jpt_vk_renderer_stats_t* st = &r->stats;
    st->draw_calls_requested++;
    st->cull_draws[state->cull_mode < 4u ? state->cull_mode : 0u]++;
    st->zfunc_draws[state->z_func < 9u ? state->z_func : 0u]++;
    st->prim_draws[(unsigned)prim < 7u ? (unsigned)prim : 0u]++;
    if (index_count) st->indexed_draws++;
    if (state->has_texture) st->textured_draws++;
    if (state->has_texture && state->colorkey_enable) st->colorkey_draws++;
    if (state->fog_enable) st->fog_draws++;
    if (state->alpha_test) {
        st->alpha_test_draws++;
        st->alpha_func_draws[state->alpha_func < 9u ? state->alpha_func : 0u]++;
    }
    if (state->alpha_blend) {
        st->alpha_blend_draws++;
        st->src_blend_draws[state->src_blend < 14u ? state->src_blend : 0u]++;
        st->dst_blend_draws[state->dst_blend < 14u ? state->dst_blend : 0u]++;
    }
}

static uint32_t expand_indices(uint32_t* ib, uint32_t max_idx, uint32_t base, jpt_render_prim_t prim, uint32_t src_n,
                               const uint16_t* indices16, const uint32_t* indices32) {
    uint32_t n = 0;
#define SRC_IDX(k) (base + (indices32 ? indices32[(k)] : indices16 ? (uint32_t)indices16[(k)] : (k)))
    if (prim == JPT_PRIM_TRIANGLEFAN && src_n >= 3) {
        uint32_t tris = src_n - 2u;
        if (tris * 3u > max_idx) return 0;
        for (uint32_t t = 0; t < tris; t++) {
            ib[n++] = SRC_IDX(0);
            ib[n++] = SRC_IDX(t + 1u);
            ib[n++] = SRC_IDX(t + 2u);
        }
    } else if (prim == JPT_PRIM_TRIANGLESTRIP && src_n >= 3) {
        uint32_t tris = src_n - 2u;
        if (tris * 3u > max_idx) return 0;
        for (uint32_t t = 0; t < tris; t++) {
            if (t & 1u) {
                ib[n++] = SRC_IDX(t);
                ib[n++] = SRC_IDX(t + 2u);
                ib[n++] = SRC_IDX(t + 1u);
            } else {
                ib[n++] = SRC_IDX(t);
                ib[n++] = SRC_IDX(t + 1u);
                ib[n++] = SRC_IDX(t + 2u);
            }
        }
    } else if (prim == JPT_PRIM_LINESTRIP && src_n >= 2) {
        uint32_t segs = src_n - 1u;
        if (segs * 2u > max_idx) return 0;
        for (uint32_t s = 0; s < segs; s++) {
            ib[n++] = SRC_IDX(s);
            ib[n++] = SRC_IDX(s + 1u);
        }
    } else if (prim == JPT_PRIM_TRIANGLELIST) {
        uint32_t whole = src_n - src_n % 3u;
        if (whole > max_idx) return 0;
        for (uint32_t i = 0; i < whole; i++) ib[n++] = SRC_IDX(i);
    } else if (prim == JPT_PRIM_LINELIST) {
        uint32_t whole = src_n & ~1u;
        if (whole > max_idx) return 0;
        for (uint32_t i = 0; i < whole; i++) ib[n++] = SRC_IDX(i);
    } else if (prim == JPT_PRIM_POINTLIST) {
        if (src_n > max_idx) return 0;
        for (uint32_t i = 0; i < src_n; i++) ib[n++] = SRC_IDX(i);
    }
#undef SRC_IDX
    return n;
}

static int batch_matches(const vk_batch_t* b, VkPipeline pipe, const vk_world_push_t* push, const VkViewport* vp,
                         const VkRect2D* sc, float bias) {
    return b->active && b->pipe == pipe && b->bias == bias && !memcmp(&b->push, push, sizeof *push) &&
           !memcmp(&b->vp, vp, sizeof *vp) && !memcmp(&b->sc, sc, sizeof *sc);
}

jpt_vk_draw_result_t jpt_vk_renderer_draw(jpt_vk_renderer_t* r, const jpt_render_state_t* state, jpt_render_prim_t prim,
                                          const jpt_render_vertex_t* verts, uint32_t vert_count,
                                          const uint16_t* indices16, const uint32_t* indices32, uint32_t index_count) {
    jpt_vk_pipeline_key_t key;
    VkPipeline pipe;
    vk_frame_rings_t* ring;
    vk_batch_t* b;
    vk_world_push_t push;
    VkViewport vp;
    VkRect2D sc;
    const vk_tex_slot_t* tex_slot;
    VkImageView tex_view = VK_NULL_HANDLE;
    uint32_t batch_slot = 0, layer_word, src_n, max_idx, added;
    VkDeviceSize vb_bytes;
    double t0 = 0.0;
    float bias;

    if (!r) return JPT_VK_SKIP_INVALID;
    if (!state || !verts || !vert_count) return skip_draw(r, JPT_VK_SKIP_INVALID);
    if (!r->in_frame) return skip_draw(r, JPT_VK_SKIP_NO_FRAME);
    if (r->collect_timings) t0 = now_ms();
    count_draw_state(r, state, prim, index_count);

    if (state->water_mode && r->cfg.water_refraction) {
        capture_scene_behind_for_water(r);
        r->stats.water_batches++;
    }
    if (state->sky_mode && r->cfg.enhanced_sky) r->stats.sky_batches++;

    ensure_scene_pass(r, 0, NULL, 0.0f, 0);

    jpt_vk_pipeline_key_from_state(state, prim, r->active_msaa,
                                   r->active_msaa > 1 ? JPT_VK_RP_SCENE_MSAA : JPT_VK_RP_SCENE,
                                   r->ctx.gpu.has_fill_non_solid, &key);
    if (!r->cfg.water_refraction && key.shader_kind == JPT_VK_SHADER_WATER) key.shader_kind = JPT_VK_SHADER_FIXED;
    if (!r->cfg.enhanced_sky && key.shader_kind == JPT_VK_SHADER_SKY) key.shader_kind = JPT_VK_SHADER_FIXED;

    pipe = get_or_create_world_pipeline(r, &key);
    if (pipe == VK_NULL_HANDLE) return skip_draw(r, JPT_VK_SKIP_NO_PIPELINE);

    ring = &r->rings[r->sc.frame_index];
    b = &r->batch;
    vb_bytes = (VkDeviceSize)vert_count * sizeof(vk_gpu_vertex_t);
    src_n = index_count ? index_count : vert_count;

    memset(&push, 0, sizeof push);
    push.target[0] = (float)r->rt_w;
    push.target[1] = (float)r->rt_h;
    push.target[2] = 1.0f / (float)r->rt_w;
    push.target[3] = 1.0f / (float)r->rt_h;
    {
        uint32_t flags = r->debug_flags;
        if (state->alpha_test) flags |= 8u;
        if (state->fog_enable) flags |= 16u;
        if (state->specular_enable) flags |= 32u;
        if (state->shade_mode == 1u) flags |= 64u;
        push.draw_u[0] = flags;
    }
    push.draw_u[1] = state->tex_map_blend;
    push.draw_u[2] = state->alpha_func;
    push.fog_color[0] = ((state->fog_color >> 16) & 0xFFu) / 255.0f;
    push.fog_color[1] = ((state->fog_color >> 8) & 0xFFu) / 255.0f;
    push.fog_color[2] = (state->fog_color & 0xFFu) / 255.0f;
    push.fog_color[3] = (float)(state->alpha_ref & 0xFFu) / 255.0f;
    push.fx_water[0] = 1.0f / (float)r->rt_w;
    push.fx_water[1] = 1.0f / (float)r->rt_h;
    push.fx_water[2] = r->water_strength;
    push.fx_water[3] = (float)r->rt_h / (float)r->rt_w;
    push.fx_sky[0] = r->sky_cam_u;
    push.fx_sky[1] = r->sky_cam_v;
    push.fx_sky[2] = r->sky_plane_h;
    push.fx_sky[3] = r->sky_fog_lo;
    push.fx_extra[0] = r->sky_fog_hi;
    push.fx_extra[1] = 1.0f / r->water_rhw_metre;
    memcpy(push.fx_water3, r->water_vp, sizeof push.fx_water3);

    /* Pre-transformed vertices are in render-target pixels: the Direct3D viewport only clips, it never rescales. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)r->rt_w;
    vp.height = (float)r->rt_h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    {
        int32_t x0 = r->viewport.x < 0 ? 0 : r->viewport.x, y0 = r->viewport.y < 0 ? 0 : r->viewport.y;
        int32_t x1 = x0 + (int32_t)r->viewport.w, y1 = y0 + (int32_t)r->viewport.h;
        if (r->scissor_enabled) {
            int32_t sx0 = r->scissor.x < 0 ? 0 : r->scissor.x, sy0 = r->scissor.y < 0 ? 0 : r->scissor.y;
            int32_t sx1 = sx0 + (int32_t)r->scissor.w, sy1 = sy0 + (int32_t)r->scissor.h;
            if (sx0 > x0) x0 = sx0;
            if (sy0 > y0) y0 = sy0;
            if (sx1 < x1) x1 = sx1;
            if (sy1 < y1) y1 = sy1;
        }
        if (x1 > (int32_t)r->rt_w) x1 = (int32_t)r->rt_w;
        if (y1 > (int32_t)r->rt_h) y1 = (int32_t)r->rt_h;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        sc.offset.x = x0;
        sc.offset.y = y0;
        sc.extent.width = clamp_u32((uint32_t)(x1 - x0), 1u, r->rt_w);
        sc.extent.height = clamp_u32((uint32_t)(y1 - y0), 1u, r->rt_h);
    }
    bias = depth_bias_for(state);

    tex_slot = lookup_tex_slot(r, state);
    if (tex_slot) tex_view = tex_slot->standalone ? tex_slot->view : r->pools[tex_slot->pool_slot].view;

    if (b->active && !batch_matches(b, pipe, &push, &vp, &sc, bias)) flush_batch(r);
    if (b->active && tex_view != VK_NULL_HANDLE && batch_find_view(b, tex_view) < 0 && b->view_count >= 16u) {
        r->stats.batch_flushes_texture_full++;
        flush_batch(r);
    }
    if (!b->active) {
        b->vb_off = (ring->vb_used + 15u) & ~((VkDeviceSize)15u);
        b->ib_off = (ring->ib_used + 15u) & ~((VkDeviceSize)15u);
        b->verts = 0;
        b->indices = 0;
        b->view_count = 0;
    }
    if (b->vb_off + (VkDeviceSize)b->verts * sizeof(vk_gpu_vertex_t) + vb_bytes > JPT_VK_VB_SIZE ||
        !ring->vb_alloc.mapped)
        return skip_draw(r, JPT_VK_SKIP_VERTEX_RING);
    if (b->ib_off + (VkDeviceSize)b->indices * sizeof(uint32_t) >= JPT_VK_IB_SIZE || !ring->ib_alloc.mapped)
        return skip_draw(r, JPT_VK_SKIP_INDEX_RING);

    max_idx = (uint32_t)((JPT_VK_IB_SIZE - b->ib_off) / sizeof(uint32_t)) - b->indices;
    added = expand_indices((uint32_t*)((uint8_t*)ring->ib_alloc.mapped + b->ib_off) + b->indices, max_idx, b->verts,
                           prim, src_n, indices16, indices32);
    if (!added) return skip_draw(r, max_idx < src_n ? JPT_VK_SKIP_INDEX_RING : JPT_VK_SKIP_NO_INDICES);

    if (tex_view != VK_NULL_HANDLE) {
        int found = batch_find_view(b, tex_view);
        if (found >= 0) {
            batch_slot = (uint32_t)found;
        } else {
            batch_slot = b->view_count++;
            b->views[batch_slot] = tex_view;
        }
    }

    layer_word = encode_layer_word(r, state, tex_slot, batch_slot);
    {
        vk_gpu_vertex_t* dst = (vk_gpu_vertex_t*)((uint8_t*)ring->vb_alloc.mapped + b->vb_off) + b->verts;
        for (uint32_t i = 0; i < vert_count; i++) {
            dst[i].sx = verts[i].sx;
            dst[i].sy = verts[i].sy;
            dst[i].sz = verts[i].sz;
            dst[i].rhw = verts[i].rhw;
            dst[i].diffuse = verts[i].diffuse;
            dst[i].specular = verts[i].specular;
            dst[i].tu = verts[i].tu;
            dst[i].tv = verts[i].tv;
            dst[i].layer = layer_word;
        }
    }

    if (b->active) {
        r->stats.draw_calls_merged++;
    } else {
        b->active = 1;
        b->pipe = pipe;
        b->push = push;
        b->vp = vp;
        b->sc = sc;
        b->bias = bias;
    }
    b->verts += vert_count;
    b->indices += added;
    ring->vb_used = b->vb_off + (VkDeviceSize)b->verts * sizeof(vk_gpu_vertex_t);
    ring->ib_used = b->ib_off + (VkDeviceSize)b->indices * sizeof(uint32_t);

    r->scene_has_draws = 1;
    if (state->z_write) r->scene_has_depth_draws = 1;
    r->stats.draws++;
    if (key.topology == 0) r->stats.triangles += added / 3u;
    if (key.shader_kind == JPT_VK_SHADER_WATER) r->stats.water_draws++;
    if (key.shader_kind == JPT_VK_SHADER_SKY) r->stats.sky_draws++;
    if (r->collect_timings) r->stats.cpu_draw_ms += now_ms() - t0;
    return JPT_VK_DRAW_SUBMITTED;
}

/* ---- Perspective-textured polygons ---- */

#define JPT_VK_POLY_CLUT_POOL (1u << 20)
#define JPT_VK_POLY_UBO_RING (16u << 20)

static VkDeviceSize poly_pool_bytes(jpt_vk_poly_pool_t pool) {
    switch (pool) {
    case JPT_VK_POLY_TEXELS: return JPT_VK_POLY_TEXEL_BYTES;
    case JPT_VK_POLY_CLUT: return JPT_VK_POLY_CLUT_POOL;
    case JPT_VK_POLY_BUMP_TABLES: return JPT_VK_POLY_TABLE_BYTES;
    default: return JPT_VK_POLY_BUMP_CLUT_BYTES;
    }
}

static void destroy_poly_resources(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    for (int i = 0; i < 2; i++) destroy_pipe(r, &r->poly_pipe[i]);
    for (int i = 0; i < JPT_VK_POLY_POOLS; i++)
        if (r->poly_pool[i] != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->poly_pool[i], &r->poly_pool_alloc[i]);
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++)
        if (r->poly_ubo[i] != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->poly_ubo[i], &r->poly_ubo_alloc[i]);
    if (r->poly_dpool != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorPool(r->ctx.device, r->poly_dpool, NULL);
        r->poly_dpool = VK_NULL_HANDLE;
        r->ctx.lifetime.descriptor_pools--;
    }
    if (r->poly_pl != VK_NULL_HANDLE) {
        vk->vkDestroyPipelineLayout(r->ctx.device, r->poly_pl, NULL);
        r->poly_pl = VK_NULL_HANDLE;
        r->ctx.lifetime.pipeline_layouts--;
    }
    if (r->poly_dsl != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorSetLayout(r->ctx.device, r->poly_dsl, NULL);
        r->poly_dsl = VK_NULL_HANDLE;
    }
    r->poly_ready = 0;
}

static VkPipeline create_poly_pipeline(jpt_vk_renderer_t* r, int bump) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo pci;
    VkPipeline pipe = VK_NULL_HANDLE;

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shaders[JPT_VK_SH_POLY_VERT];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shaders[bump ? JPT_VK_SH_POLY_BUMP_FRAG : JPT_VK_SH_POLY_TEXTURE_FRAG];
    stages[1].pName = "main";
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = msaa_bits(r->active_msaa);
    memset(&dss, 0, sizeof dss);
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    set_ao_stencil(&dss, r, VK_STENCIL_OP_INCREMENT_AND_CLAMP);
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyns;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dss;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = r->poly_pl;
    pci.renderPass = r->scene_rp_load;
    if (r->ctx.vk.vkCreateGraphicsPipelines(r->ctx.device, r->ctx.pipeline_cache, 1, &pci, NULL, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    r->ctx.lifetime.pipelines++;
    r->stats.pipelines_created++;
    return pipe;
}

int jpt_vk_renderer_poly_ready(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk;
    VkDescriptorSetLayoutBinding b[5];
    VkDescriptorSetLayoutCreateInfo dlci;
    VkPipelineLayoutCreateInfo plci;
    VkDescriptorPoolSize sizes[2];
    VkDescriptorPoolCreateInfo dpci;
    VkDescriptorSetAllocateInfo dsai;
    VkDeviceSize align;
    if (!r) return 0;
    if (r->poly_ready) return 1;
    if (r->poly_failed) return 0;
    vk = &r->ctx.vk;
    r->poly_failed = 1;

    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < 5; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dlci, 0, sizeof dlci);
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 5;
    dlci.pBindings = b;
    if (vk->vkCreateDescriptorSetLayout(r->ctx.device, &dlci, NULL, &r->poly_dsl) != VK_SUCCESS) goto fail;
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &r->poly_dsl;
    if (vk->vkCreatePipelineLayout(r->ctx.device, &plci, NULL, &r->poly_pl) != VK_SUCCESS) goto fail;
    r->ctx.lifetime.pipeline_layouts++;

    for (int i = 0; i < JPT_VK_POLY_POOLS; i++) {
        if (jpt_vk_create_buffer(&r->ctx.alloc, poly_pool_bytes((jpt_vk_poly_pool_t)i),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &r->poly_pool[i],
                                 &r->poly_pool_alloc[i]) != VK_SUCCESS)
            goto fail;
    }
    align = r->ctx.props.limits.minUniformBufferOffsetAlignment;
    if (align < 16u) align = 16u;
    r->poly_ubo_stride = ((VkDeviceSize)sizeof(jpt_vk_poly_constants_t) + align - 1u) / align * align;
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (jpt_vk_create_buffer(&r->ctx.alloc, JPT_VK_POLY_UBO_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1,
                                 &r->poly_ubo[i], &r->poly_ubo_alloc[i]) != VK_SUCCESS ||
            !r->poly_ubo_alloc[i].mapped)
            goto fail;
    }

    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = JPT_VK_FRAMES_IN_FLIGHT;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = 4u * JPT_VK_FRAMES_IN_FLIGHT;
    memset(&dpci, 0, sizeof dpci);
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = JPT_VK_FRAMES_IN_FLIGHT;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (vk->vkCreateDescriptorPool(r->ctx.device, &dpci, NULL, &r->poly_dpool) != VK_SUCCESS) goto fail;
    r->ctx.lifetime.descriptor_pools++;
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo infos[5];
        VkWriteDescriptorSet w[5];
        memset(&dsai, 0, sizeof dsai);
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = r->poly_dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &r->poly_dsl;
        if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->poly_ds[i]) != VK_SUCCESS) goto fail;
        memset(infos, 0, sizeof infos);
        memset(w, 0, sizeof w);
        infos[0].buffer = r->poly_ubo[i];
        infos[0].range = sizeof(jpt_vk_poly_constants_t);
        for (int k = 0; k < 4; k++) {
            infos[k + 1].buffer = r->poly_pool[k];
            infos[k + 1].range = VK_WHOLE_SIZE;
        }
        for (uint32_t k = 0; k < 5; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = r->poly_ds[i];
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = k ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[k].pBufferInfo = &infos[k];
        }
        vk->vkUpdateDescriptorSets(r->ctx.device, 5, w, 0, NULL);
    }
    r->poly_pipe[0] = create_poly_pipeline(r, 0);
    r->poly_pipe[1] = create_poly_pipeline(r, 1);
    if (r->poly_pipe[0] == VK_NULL_HANDLE || r->poly_pipe[1] == VK_NULL_HANDLE) goto fail;
    r->poly_ready = 1;
    r->poly_failed = 0;
    return 1;
fail:
    jpt_vk_log(&r->ctx, "GPU polygon resources could not be created: the guest rasterises those polygons itself");
    destroy_poly_resources(r);
    return 0;
}

static void poly_memory_barrier(jpt_vk_renderer_t* r, int after_copy) {
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    if (after_copy) {
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        r->ctx.vk.vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                                       &mb, 0, NULL, 0, NULL);
    } else {
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        r->ctx.vk.vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       1, &mb, 0, NULL, 0, NULL);
    }
}

typedef struct {
    VkBuffer src, dst;
    VkDeviceSize src_off, dst_off, bytes;
} poly_copy_job_t;

static void record_poly_copy(jpt_vk_renderer_t* r, VkCommandBuffer cmd, void* user) {
    const poly_copy_job_t* j = (const poly_copy_job_t*)user;
    VkBufferCopy c;
    VkMemoryBarrier mb;
    c.srcOffset = j->src_off;
    c.dstOffset = j->dst_off;
    c.size = j->bytes;
    r->ctx.vk.vkCmdCopyBuffer(cmd, j->src, j->dst, 1, &c);
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    r->ctx.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb,
                                   0, NULL, 0, NULL);
}

int jpt_vk_renderer_poly_upload(jpt_vk_renderer_t* r, jpt_vk_poly_pool_t pool, uint32_t offset, const void* data,
                                uint32_t bytes) {
    if (!r || !data || !bytes || pool >= JPT_VK_POLY_POOLS || !jpt_vk_renderer_poly_ready(r)) return 0;
    if ((VkDeviceSize)offset + bytes > poly_pool_bytes(pool)) return 0;
    if (r->in_frame) {
        vk_frame_rings_t* ring = &r->rings[r->sc.frame_index];
        VkDeviceSize off = (ring->staging_used + 15u) & ~((VkDeviceSize)15u);
        if (off + bytes <= JPT_VK_STAGING_SIZE && ring->staging_alloc.mapped) {
            int was_open = r->in_scene_pass;
            poly_copy_job_t job = { ring->staging_buf, r->poly_pool[pool], off, offset, bytes };
            memcpy((uint8_t*)ring->staging_alloc.mapped + off, data, bytes);
            ring->staging_used = off + bytes;
            if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_POLY_UPLOAD);
            poly_memory_barrier(r, 0);
            record_poly_copy(r, r->cmd, &job);
            if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
            r->stats.poly_uploads++;
            r->stats.poly_upload_bytes += bytes;
            return 1;
        }
    }
    {
        VkBuffer tmp = VK_NULL_HANDLE;
        jpt_vk_allocation_t tmp_alloc;
        poly_copy_job_t job;
        int ok;
        if (jpt_vk_create_buffer(&r->ctx.alloc, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, 1, &tmp,
                                 &tmp_alloc) != VK_SUCCESS)
            return 0;
        if (tmp_alloc.mapped) memcpy(tmp_alloc.mapped, data, bytes);
        job.src = tmp;
        job.dst = r->poly_pool[pool];
        job.src_off = 0;
        job.dst_off = offset;
        job.bytes = bytes;
        ok = run_one_shot_cmd(r, record_poly_copy, &job, JPT_VK_OS_POLYGON);
        jpt_vk_destroy_buffer(&r->ctx.alloc, &tmp, &tmp_alloc);
        if (ok) {
            r->stats.poly_uploads++;
            r->stats.poly_upload_bytes += bytes;
        }
        return ok;
    }
}

int jpt_vk_renderer_poly_draw(jpt_vk_renderer_t* r, int bump, const jpt_vk_poly_constants_t* c, unsigned count) {
    const jpt_vk_loader_t* vk;
    vk_frame_rings_t* ring;
    VkViewport vp;
    VkRect2D sc;
    uint32_t dyn_off;
    if (!r || !c || !r->in_frame || count < 3 || count > JPT_VK_POLY_MAX_VERTICES || !r->poly_ready) return 0;
    if (r->poly_ubo_used + r->poly_ubo_stride > JPT_VK_POLY_UBO_RING) {
        r->stats.poly_failures++;
        return 0;
    }
    vk = &r->ctx.vk;
    ring = &r->rings[r->sc.frame_index];
    (void)ring;
    ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    flush_batch(r);
    memcpy((uint8_t*)r->poly_ubo_alloc[r->sc.frame_index].mapped + r->poly_ubo_used, c, sizeof *c);
    dyn_off = (uint32_t)r->poly_ubo_used;
    r->poly_ubo_used += r->poly_ubo_stride;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)r->rt_w;
    vp.height = (float)r->rt_h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    sc.offset.x = 0;
    sc.offset.y = 0;
    sc.extent.width = r->rt_w;
    sc.extent.height = r->rt_h;
    r->world_bound_valid = 0;
    vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->poly_pipe[bump ? 1 : 0]);
    vk->vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vk->vkCmdSetScissor(r->cmd, 0, 1, &sc);
    vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->poly_pl, 0, 1, &r->poly_ds[r->sc.frame_index],
                                1, &dyn_off);
    vk->vkCmdDraw(r->cmd, (count - 2u) * 3u, 1, 0, 0);
    r->scene_has_draws = 1;
    world_ds_invalidate(r);
    r->stats.poly_draws++;
    return 1;
}


/* ---- Video passes ---- */

enum { VP_CONVERT, VP_SMOOTH, VP_DEBLOCK_X, VP_DEBLOCK_Y, VP_GLOW, VP_FINAL };
enum { VT_A, VT_B, VT_GLOW };

static void destroy_video_target(jpt_vk_renderer_t* r, vk_video_target_t* t) {
    if (t->fb != VK_NULL_HANDLE) {
        r->ctx.vk.vkDestroyFramebuffer(r->ctx.device, t->fb, NULL);
        t->fb = VK_NULL_HANDLE;
        r->ctx.lifetime.framebuffers--;
    }
    destroy_rt_image(r, &t->image, &t->alloc, &t->view);
    t->w = t->h = 0;
}

static void destroy_video_resources(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    if (r->ctx.device != VK_NULL_HANDLE && (r->video_ready || r->video_rp != VK_NULL_HANDLE)) vk->vkDeviceWaitIdle(r->ctx.device);
    for (int i = 0; i < 6; i++) destroy_pipe(r, &r->video_pipe[i]);
    for (int i = 0; i < 3; i++) destroy_video_target(r, &r->video_tgt[i]);
    destroy_rt_image(r, &r->video_codes, &r->video_codes_alloc, &r->video_codes_view);
    destroy_rt_image(r, &r->video_map, &r->video_map_alloc, &r->video_map_view);
    if (r->video_dpool != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorPool(r->ctx.device, r->video_dpool, NULL);
        r->video_dpool = VK_NULL_HANDLE;
        r->ctx.lifetime.descriptor_pools--;
    }
    if (r->video_pl != VK_NULL_HANDLE) {
        vk->vkDestroyPipelineLayout(r->ctx.device, r->video_pl, NULL);
        r->video_pl = VK_NULL_HANDLE;
        r->ctx.lifetime.pipeline_layouts--;
    }
    if (r->video_dsl != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorSetLayout(r->ctx.device, r->video_dsl, NULL);
        r->video_dsl = VK_NULL_HANDLE;
    }
    if (r->video_rp != VK_NULL_HANDLE) {
        vk->vkDestroyRenderPass(r->ctx.device, r->video_rp, NULL);
        r->video_rp = VK_NULL_HANDLE;
        r->ctx.lifetime.render_passes--;
    }
    r->video_ready = 0;
    r->video_codes_w = r->video_codes_h = 0;
    r->video_map_loaded = 0;
}

static VkPipeline create_video_pipeline(jpt_vk_renderer_t* r, VkRenderPass rp, jpt_vk_shader_id_t frag) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo pci;
    VkPipeline pipe = VK_NULL_HANDLE;
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shaders[JPT_VK_SH_VIDEO_VERT];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shaders[frag];
    stages[1].pName = "main";
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyns;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = r->video_pl;
    pci.renderPass = rp;
    if (r->ctx.vk.vkCreateGraphicsPipelines(r->ctx.device, r->ctx.pipeline_cache, 1, &pci, NULL, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    r->ctx.lifetime.pipelines++;
    r->stats.pipelines_created++;
    return pipe;
}

static int create_video_resources(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkAttachmentDescription att;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub;
    VkSubpassDependency deps[2];
    VkRenderPassCreateInfo rpci;
    VkDescriptorSetLayoutBinding b[5];
    VkDescriptorSetLayoutCreateInfo dlci;
    VkPipelineLayoutCreateInfo plci;
    VkPushConstantRange pcr;
    VkDescriptorPoolSize size;
    VkDescriptorPoolCreateInfo dpci;
    static const jpt_vk_shader_id_t k_frag[6] = { JPT_VK_SH_VIDEO_CONVERT_FRAG, JPT_VK_SH_VIDEO_SMOOTH_FRAG,
                                                  JPT_VK_SH_VIDEO_DEBLOCK_X_FRAG, JPT_VK_SH_VIDEO_DEBLOCK_Y_FRAG,
                                                  JPT_VK_SH_VIDEO_GLOW_FRAG, JPT_VK_SH_VIDEO_FINAL_FRAG };
    if (r->video_ready) return 1;
    if (r->video_failed) return 0;
    r->video_failed = 1;

    memset(&att, 0, sizeof att);
    att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    memset(deps, 0, sizeof deps);
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    memset(&rpci, 0, sizeof rpci);
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (vk->vkCreateRenderPass(r->ctx.device, &rpci, NULL, &r->video_rp) != VK_SUCCESS) goto fail;
    r->ctx.lifetime.render_passes++;

    memset(b, 0, sizeof b);
    for (uint32_t i = 0; i < 5; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dlci, 0, sizeof dlci);
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 5;
    dlci.pBindings = b;
    if (vk->vkCreateDescriptorSetLayout(r->ctx.device, &dlci, NULL, &r->video_dsl) != VK_SUCCESS) goto fail;
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(jpt_video_constants_t);
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &r->video_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vk->vkCreatePipelineLayout(r->ctx.device, &plci, NULL, &r->video_pl) != VK_SUCCESS) goto fail;
    r->ctx.lifetime.pipeline_layouts++;

    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 5u * 2u * JPT_VK_FRAMES_IN_FLIGHT;
    memset(&dpci, 0, sizeof dpci);
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 2u * JPT_VK_FRAMES_IN_FLIGHT;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    if (vk->vkCreateDescriptorPool(r->ctx.device, &dpci, NULL, &r->video_dpool) != VK_SUCCESS) goto fail;
    r->ctx.lifetime.descriptor_pools++;

    for (int i = 0; i < 6; i++) {
        r->video_pipe[i] = create_video_pipeline(r, i == VP_FINAL ? r->sc.present_pass : r->video_rp, k_frag[i]);
        if (r->video_pipe[i] == VK_NULL_HANDLE) goto fail;
    }
    r->video_ready = 1;
    r->video_failed = 0;
    return 1;
fail:
    jpt_vk_log(&r->ctx, "video pass resources could not be created");
    destroy_video_resources(r);
    r->video_failed = 1;
    return 0;
}

static int ensure_video_target(jpt_vk_renderer_t* r, vk_video_target_t* t, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo fbci;
    pool_init_job_t job;
    if (t->image != VK_NULL_HANDLE && t->w == w && t->h == h) return 1;
    destroy_video_target(r, t);
    if (!create_image_2d(r, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &t->image, &t->alloc, &t->view))
        return 0;
    memset(&fbci, 0, sizeof fbci);
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = r->video_rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &t->view;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    if (r->ctx.vk.vkCreateFramebuffer(r->ctx.device, &fbci, NULL, &t->fb) != VK_SUCCESS) {
        destroy_rt_image(r, &t->image, &t->alloc, &t->view);
        return 0;
    }
    r->ctx.lifetime.framebuffers++;
    job.img = t->image;
    job.layers = 1;
    job.levels = 1;
    run_one_shot_cmd(r, record_pool_init, &job, JPT_VK_OS_RESOURCE_INIT);
    t->w = w;
    t->h = h;
    return 1;
}

static void write_video_sets(jpt_vk_renderer_t* r) {
    VkDescriptorSetAllocateInfo dsai;
    r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
    for (int f = 0; f < JPT_VK_FRAMES_IN_FLIGHT; f++) {
        for (int pic = 0; pic < 2; pic++) {
            VkDescriptorImageInfo ii[5];
            VkWriteDescriptorSet w[5];
            VkImageView views[5] = { r->video_codes_view, r->video_map_view, r->video_tgt[pic].view,
                                     r->video_tgt[VT_GLOW].view, r->gamma_lut_view };
            VkSampler samplers[5] = { r->nearest_clamp_sampler, r->nearest_clamp_sampler, r->nearest_clamp_sampler,
                                      r->linear_clamp_sampler, r->nearest_clamp_sampler };
            if (r->video_ds[f][pic] == VK_NULL_HANDLE) {
                memset(&dsai, 0, sizeof dsai);
                dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                dsai.descriptorPool = r->video_dpool;
                dsai.descriptorSetCount = 1;
                dsai.pSetLayouts = &r->video_dsl;
                if (r->ctx.vk.vkAllocateDescriptorSets(r->ctx.device, &dsai, &r->video_ds[f][pic]) != VK_SUCCESS) return;
            }
            memset(w, 0, sizeof w);
            for (uint32_t k = 0; k < 5; k++) {
                ii[k].sampler = samplers[k];
                ii[k].imageView = views[k];
                ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[k].dstSet = r->video_ds[f][pic];
                w[k].dstBinding = k;
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &ii[k];
            }
            r->ctx.vk.vkUpdateDescriptorSets(r->ctx.device, 5, w, 0, NULL);
        }
    }
}

static void video_pass(jpt_vk_renderer_t* r, int pipe, vk_video_target_t* target, int picture_set) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkRenderPassBeginInfo rpbi;
    VkViewport vp = { 0.0f, 0.0f, (float)target->w, (float)target->h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { target->w, target->h } };
    memset(&rpbi, 0, sizeof rpbi);
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = r->video_rp;
    rpbi.framebuffer = target->fb;
    rpbi.renderArea.extent.width = target->w;
    rpbi.renderArea.extent.height = target->h;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    r->world_bound_valid = 0;
    vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->video_pipe[pipe]);
    vk->vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vk->vkCmdSetScissor(r->cmd, 0, 1, &sc);
    vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->video_pl, 0, 1,
                                &r->video_ds[r->sc.frame_index][picture_set], 0, NULL);
    vk->vkCmdPushConstants(r->cmd, r->video_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof r->video_constants,
                           &r->video_constants);
    vk->vkCmdDraw(r->cmd, 3, 1, 0, 0);
    vk->vkCmdEndRenderPass(r->cmd);
}

int jpt_vk_renderer_video_draw(jpt_vk_renderer_t* r, const jpt_vk_video_frame_t* f) {
    const jpt_video_params_t* p;
    int sets_stale, deblock, picture;
    uint32_t w, rows, shown;
    uint8_t* packed;
    size_t bytes;
    if (!r || !f || !f->rows || !r->in_frame) return 0;
    p = &f->params;
    w = (uint32_t)p->width;
    rows = (uint32_t)p->stored_rows;
    shown = (uint32_t)p->shown_rows;
    if (!w || w > JPT_VIDEO_MAX_SIDE || !rows || !shown || shown > JPT_VIDEO_MAX_SIDE || p->out_w <= 0 || p->out_h <= 0 ||
        p->video_w <= 0 || p->video_h <= 0 || !create_video_resources(r))
        return 0;
    deblock = f->deblock && p->progressive;
    end_scene_pass_if_open(r);

    sets_stale = r->video_tgt[VT_GLOW].image == VK_NULL_HANDLE || !r->video_codes_view || !r->video_map_view;
    if (r->video_tgt[VT_A].w != w || r->video_tgt[VT_A].h != shown) sets_stale = 1;
    if (!ensure_video_target(r, &r->video_tgt[VT_A], w, shown) || !ensure_video_target(r, &r->video_tgt[VT_B], w, shown) ||
        !ensure_video_target(r, &r->video_tgt[VT_GLOW], 2u * JPT_VIDEO_GLOW_STEPS, JPT_VIDEO_GLOW_ROWS))
        return 0;
    if (r->video_map == VK_NULL_HANDLE) {
        if (!create_image_2d(r, 256, 256, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             &r->video_map, &r->video_map_alloc, &r->video_map_view))
            return 0;
        r->video_map_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        sets_stale = 1;
    }
    if (r->video_codes == VK_NULL_HANDLE || r->video_codes_w != w || r->video_codes_h != rows) {
        r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
        destroy_rt_image(r, &r->video_codes, &r->video_codes_alloc, &r->video_codes_view);
        if (!create_image_2d(r, w, rows, VK_FORMAT_R16_UINT, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             &r->video_codes, &r->video_codes_alloc, &r->video_codes_view))
            return 0;
        r->video_codes_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        r->video_codes_w = w;
        r->video_codes_h = rows;
        sets_stale = 1;
    }
    if (sets_stale) write_video_sets(r);

    bytes = (size_t)w * rows * 2u;
    packed = (uint8_t*)malloc(bytes);
    if (!packed) return 0;
    for (uint32_t y = 0; y < rows; y++) memcpy(packed + (size_t)y * w * 2u, f->rows + (size_t)y * (size_t)f->stride, (size_t)w * 2u);
    if (!upload_rgba_to_image(r, r->video_codes, &r->video_codes_layout, 0, 0, w, rows, 0, packed, bytes)) {
        free(packed);
        return 0;
    }
    free(packed);
    if (!r->video_map_loaded || (f->palette_map && f->palette_changed)) {
        static uint32_t s_none[256 * 256];
        if (!upload_rgba_to_image(r, r->video_map, &r->video_map_layout, 0, 0, 256, 256, 0,
                                  f->palette_map ? f->palette_map : s_none, 256u * 256u * 4u))
            return 0;
        r->video_map_loaded = 1;
    }

    if (f->gamma && (!r->video_gamma_valid || memcmp(r->video_gamma, f->gamma, sizeof r->video_gamma))) {
        memcpy(r->video_gamma, f->gamma, sizeof r->video_gamma);
        r->video_gamma_valid = 1;
        jpt_vk_renderer_set_gamma_ramp(r, f->gamma, f->gamma + 256, f->gamma + 512);
    }
    {
        jpt_video_params_t q = *p;
        q.gamma_on = !r->gamma_identity;
        jpt_video_constants(&q, &r->video_constants);
    }
    video_pass(r, VP_CONVERT, &r->video_tgt[VT_A], 1);
    picture = VT_A;
    if (deblock) {
        video_pass(r, VP_DEBLOCK_X, &r->video_tgt[VT_B], picture);
        picture = VT_B;
        video_pass(r, VP_DEBLOCK_Y, &r->video_tgt[VT_A], picture);
        picture = VT_A;
    }
    if (f->smooth) {
        int out = picture == VT_A ? VT_B : VT_A;
        video_pass(r, VP_SMOOTH, &r->video_tgt[out], picture);
        picture = out;
    }
    if (p->glow) video_pass(r, VP_GLOW, &r->video_tgt[VT_GLOW], picture);
    r->video_picture = picture;
    r->video_active = 1;
    return 1;
}

static void record_video_final(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkViewport vp = { 0.0f, 0.0f, (float)r->present_extent.width, (float)r->present_extent.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, r->present_extent };
    r->world_bound_valid = 0;
    vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->video_pipe[VP_FINAL]);
    vk->vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vk->vkCmdSetScissor(r->cmd, 0, 1, &sc);
    vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->video_pl, 0, 1,
                                &r->video_ds[r->sc.frame_index][r->video_picture], 0, NULL);
    vk->vkCmdPushConstants(r->cmd, r->video_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof r->video_constants,
                           &r->video_constants);
    vk->vkCmdDraw(r->cmd, 3, 1, 0, 0);
    r->video_active = 0;
}


static void apply_layer_opts(vk_layer_tex_t* l, const jpt_vk_layer_opts_t* opts) {
    l->active = 1;
    l->native_res = opts && opts->native_res ? 1 : 0;
    l->blend_mode = opts ? opts->blend_mode : 0;
    l->apply_gamma = opts && opts->apply_gamma ? 1 : 0;
    l->point_filter = opts && opts->point_filter ? 1 : 0;
    if (opts && opts->norm_rect) {
        l->norm_rect[0] = opts->norm_rect[0];
        l->norm_rect[1] = opts->norm_rect[1];
        l->norm_rect[2] = opts->norm_rect[2];
        l->norm_rect[3] = opts->norm_rect[3];
    } else {
        l->norm_rect[0] = 0.0f;
        l->norm_rect[1] = 0.0f;
        l->norm_rect[2] = 1.0f;
        l->norm_rect[3] = 1.0f;
    }
}

static int ensure_layer_image(jpt_vk_renderer_t* r, vk_layer_tex_t* l, uint32_t w, uint32_t h, int* recreated) {
    if (recreated) *recreated = 0;
    if (l->image != VK_NULL_HANDLE && l->w == w && l->h == h) return 1;
    r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
    drain_retired_images(r, 1);
    destroy_rt_image(r, &l->image, &l->alloc, &l->view);
    if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &l->image, &l->alloc, &l->view))
        return 0;
    l->w = w;
    l->h = h;
    l->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    update_quad_ds_2(r, l->desc_linear, l->view, r->linear_clamp_sampler, r->gamma_lut_view, r->nearest_clamp_sampler);
    update_quad_ds_2(r, l->desc_point, l->view, r->nearest_clamp_sampler, r->gamma_lut_view, r->nearest_clamp_sampler);
    if (recreated) *recreated = 1;
    return 1;
}

#define JPT_VK_LAYER_RECT_MAX 64u

void jpt_vk_renderer_update_layer_rects(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba, uint32_t full_w,
                                        uint32_t full_h, const jpt_render_rect_t* rects, uint32_t count,
                                        const jpt_vk_layer_opts_t* opts) {
    vk_layer_tex_t* l;
    VkBufferImageCopy regions[JPT_VK_LAYER_RECT_MAX];
    VkBuffer staging;
    VkDeviceSize off, total = 0, cursor = 0;
    uint8_t* dst;
    int recreated = 0, was_open;

    if (!r || layer < 0 || layer >= JPT_LAYER_COUNT || !rgba || !full_w || !full_h) return;
    l = &r->layers[layer];
    if (!ensure_layer_image(r, l, full_w, full_h, &recreated)) return;
    if (recreated || !count || count > JPT_VK_LAYER_RECT_MAX || !r->in_frame) {
        if (recreated || !count) {
            jpt_vk_renderer_update_layer_rect_ex(r, layer, rgba, full_w, full_h, NULL, opts);
            return;
        }
        for (uint32_t i = 0; i < count; i++) jpt_vk_renderer_update_layer_rect_ex(r, layer, rgba, full_w, full_h, &rects[i], opts);
        return;
    }
    for (uint32_t i = 0; i < count; i++) total += (VkDeviceSize)rects[i].w * rects[i].h * 4u;
    if (!ring_stage_alloc(r, total, &staging, &off, &dst)) {
        for (uint32_t i = 0; i < count; i++) jpt_vk_renderer_update_layer_rect_ex(r, layer, rgba, full_w, full_h, &rects[i], opts);
        return;
    }
    memset(regions, 0, sizeof regions);
    for (uint32_t i = 0; i < count; i++) {
        const jpt_render_rect_t* rc = &rects[i];
        for (uint32_t row = 0; row < rc->h; row++)
            memcpy(dst + cursor + (size_t)row * rc->w * 4u, rgba + ((size_t)(rc->y + row) * full_w + (size_t)rc->x) * 4u,
                   (size_t)rc->w * 4u);
        regions[i].bufferOffset = off + cursor;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageOffset.x = rc->x;
        regions[i].imageOffset.y = rc->y;
        regions[i].imageExtent.width = rc->w;
        regions[i].imageExtent.height = rc->h;
        regions[i].imageExtent.depth = 1;
        cursor += (VkDeviceSize)rc->w * rc->h * 4u;
    }
    was_open = r->in_scene_pass;
    if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_LAYER_UPLOAD);
    cmd_image_barrier(&r->ctx.vk, r->cmd, l->image, VK_IMAGE_ASPECT_COLOR_BIT, l->layout,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
    r->ctx.vk.vkCmdCopyBufferToImage(r->cmd, staging, l->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions);
    cmd_image_barrier(&r->ctx.vk, r->cmd, l->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
    l->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    r->stats.layer_rect_uploads++;
    r->stats.layer_rect_bytes += total;
    r->stats.layer_rects += count;
    apply_layer_opts(l, opts);
}

void jpt_vk_renderer_update_layer_rect_ex(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                          uint32_t full_w, uint32_t full_h, const jpt_render_rect_t* dirty,
                                          const jpt_vk_layer_opts_t* opts) {
    vk_layer_tex_t* l;
    int recreated = 0;
    int was_open;
    uint32_t ux = 0, uy = 0, uw, uh;

    if (!r || layer < 0 || layer >= JPT_LAYER_COUNT || !rgba || !full_w || !full_h) return;
    l = &r->layers[layer];
    if (!ensure_layer_image(r, l, full_w, full_h, &recreated)) return;

    uw = full_w;
    uh = full_h;
    if (!recreated && dirty && dirty->w > 0 && dirty->h > 0 && dirty->x >= 0 && dirty->y >= 0 &&
        (uint32_t)dirty->x + dirty->w <= full_w && (uint32_t)dirty->y + dirty->h <= full_h) {
        ux = (uint32_t)dirty->x;
        uy = (uint32_t)dirty->y;
        uw = dirty->w;
        uh = dirty->h;
    }

    was_open = r->in_scene_pass;
    if (was_open) end_scene_pass_for(r, JPT_VK_BREAK_LAYER_UPLOAD);
    if (ux == 0 && uy == 0 && uw == full_w && uh == full_h) {
        upload_rgba_to_image(r, l->image, &l->layout, 0, 0, full_w, full_h, 0, rgba,
                             (size_t)full_w * (size_t)full_h * 4u);
    } else {
        size_t bytes = (size_t)uw * (size_t)uh * 4u;
        uint8_t* packed = (uint8_t*)malloc(bytes);
        if (packed) {
            for (uint32_t row = 0; row < uh; row++) {
                memcpy(packed + (size_t)row * uw * 4u, rgba + ((size_t)(uy + row) * full_w + ux) * 4u,
                       (size_t)uw * 4u);
            }
            upload_rgba_to_image(r, l->image, &l->layout, ux, uy, uw, uh, 0, packed, bytes);
            free(packed);
        } else {
            upload_rgba_to_image(r, l->image, &l->layout, 0, 0, full_w, full_h, 0, rgba,
                                 (size_t)full_w * (size_t)full_h * 4u);
        }
    }
    if (was_open) ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    apply_layer_opts(l, opts);
}

void jpt_vk_renderer_submit_layer_ex(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                     uint32_t src_w, uint32_t src_h, const jpt_vk_layer_opts_t* opts) {
    jpt_vk_renderer_update_layer_rect_ex(r, layer, rgba, src_w, src_h, NULL, opts);
}

void jpt_vk_renderer_submit_layer_rect_rgba(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                            uint32_t src_w, uint32_t src_h, int native_res, int alpha_blend,
                                            const float norm_rect[4]) {
    jpt_vk_layer_opts_t opts;
    opts.native_res = native_res ? 1 : 0;
    opts.blend_mode = alpha_blend ? 1 : 0;
    opts.apply_gamma = 0;
    opts.point_filter = 0;
    opts.norm_rect = norm_rect;
    jpt_vk_renderer_submit_layer_ex(r, layer, rgba, src_w, src_h, &opts);
}

void jpt_vk_renderer_submit_layer_rgba(jpt_vk_renderer_t* r, jpt_render_layer_t layer, const uint8_t* rgba,
                                       uint32_t src_w, uint32_t src_h, int native_res, int alpha_blend) {
    jpt_vk_renderer_submit_layer_rect_rgba(r, layer, rgba, src_w, src_h, native_res, alpha_blend, NULL);
}

int jpt_vk_renderer_reactivate_layer(jpt_vk_renderer_t* r, jpt_render_layer_t layer, uint32_t w, uint32_t h,
                                    const jpt_vk_layer_opts_t* opts) {
    vk_layer_tex_t* l;
    if (!r || layer < 0 || layer >= JPT_LAYER_COUNT) return 0;
    l = &r->layers[layer];
    if (l->image == VK_NULL_HANDLE || l->w != w || l->h != h || l->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return 0;
    apply_layer_opts(l, opts);
    return 1;
}

void jpt_vk_renderer_clear_layer(jpt_vk_renderer_t* r, jpt_render_layer_t layer) {
    if (!r || layer < 0 || layer >= JPT_LAYER_COUNT) return;
    r->layers[layer].active = 0;
}

static void draw_quad(jpt_vk_renderer_t* r, VkPipeline pipe, VkDescriptorSet ds, const vk_quad_push_t* push,
                      uint32_t vp_x, uint32_t vp_y, uint32_t vp_w, uint32_t vp_h) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkViewport vp = { (float)vp_x, (float)vp_y, (float)vp_w, (float)vp_h, 0.0f, 1.0f };
    VkRect2D sc = { { (int32_t)vp_x, (int32_t)vp_y }, { vp_w, vp_h } };
    r->world_bound_valid = 0;
    vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vk->vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vk->vkCmdSetScissor(r->cmd, 0, 1, &sc);
    vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->quad_pl, 0, 1, &ds, 0, NULL);
    vk->vkCmdPushConstants(r->cmd, r->quad_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof *push, push);
    vk->vkCmdDraw(r->cmd, 4, 1, 0, 0);
}

static void run_ao_pass(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    ao_preset_t preset;
    float tan_x = 0.0f, tan_y = 0.0f;
    ao_frame_t af;
    vk_quad_push_t push;
    VkRenderPassBeginInfo rpbi;

    af.level = r->ao_level;
    af.ready = 1;
    af.masked = 1;
    af.scene = r->scene_has_depth_draws;
    af.gameplay = r->ao_gameplay;
    af.tan_half_h = r->ao_tan_half_h;
    if (!ao_frame_wanted(&af) || !ao_preset(r->ao_level, &preset)) return;
    if (!ao_view_tangents(r->ao_tan_half_h, r->rt_w, r->rt_h, &tan_x, &tan_y)) return;

    VkImageAspectFlags depth_aspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (r->ctx.gpu.depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    cmd_image_barrier(vk, r->cmd, r->scene_depth, depth_aspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);

    memset(&push, 0, sizeof push);
    push.pass_rect[0] = -1.0f;
    push.pass_rect[1] = -1.0f;
    push.pass_rect[2] = 1.0f;
    push.pass_rect[3] = 1.0f;
    push.pass_uv[0] = 0.0f;
    push.pass_uv[1] = 0.0f;
    push.pass_uv[2] = 1.0f;
    push.pass_uv[3] = 1.0f;

    /* 1. Prepare linear depth */
    push.pass_params[1] = AO_DEPTH_SCALE;
    push.pass_params[2] = AO_FAR_LIMIT;
    push.pass_params[3] = AO_DEPTH_FLOOR;
    memset(&rpbi, 0, sizeof rpbi);
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = r->r32_rp;
    rpbi.framebuffer = r->ao_lin_depth_fb;
    rpbi.renderArea.extent.width = r->rt_w;
    rpbi.renderArea.extent.height = r->rt_h;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    draw_quad(r, r->ao_prepare_pipe, r->ao_prep_ds, &push, 0, 0, r->rt_w, r->rt_h);
    vk->vkCmdEndRenderPass(r->cmd);

    /* 2. Generate AO */
    push.pass_color[0] = preset.radius;
    push.pass_color[1] = preset.shadow_multiplier;
    push.pass_color[2] = preset.shadow_power;
    push.pass_color[3] = preset.shadow_clamp;
    push.pass_params[1] = preset.horizon_angle_threshold;
    push.pass_params[2] = preset.fade_out_from;
    push.pass_params[3] = preset.fade_out_to;
    push.pass_extra0[0] = tan_x;
    push.pass_extra0[1] = tan_y;
    push.pass_extra0[2] = 1.0f / (float)r->rt_w;
    push.pass_extra0[3] = 1.0f / (float)r->rt_h;
    push.pass_extra1[0] = (float)(8 + preset.quality_level * 4);
    push.pass_extra1[1] = AO_FAR_LIMIT;

    rpbi.renderPass = r->r8_rp;
    rpbi.framebuffer = r->ao_raw_fb;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    draw_quad(r, r->ao_generate_pipe, r->ao_gen_ds, &push, 0, 0, r->rt_w, r->rt_h);
    vk->vkCmdEndRenderPass(r->cmd);

    /* 3. Separable depth-aware blur */
    push.pass_color[0] = 1.0f / (float)r->rt_w;
    push.pass_color[1] = 0.0f;
    push.pass_color[2] = preset.sharpness;
    rpbi.framebuffer = r->ao_blur_tmp_fb;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    draw_quad(r, r->ao_blur_pipe, r->ao_blur_h_ds, &push, 0, 0, r->rt_w, r->rt_h);
    vk->vkCmdEndRenderPass(r->cmd);

    push.pass_color[0] = 0.0f;
    push.pass_color[1] = 1.0f / (float)r->rt_h;
    rpbi.framebuffer = r->ao_blur_out_fb;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    draw_quad(r, r->ao_blur_pipe, r->ao_blur_v_ds, &push, 0, 0, r->rt_w, r->rt_h);
    vk->vkCmdEndRenderPass(r->cmd);

    /* 4. Multiply AO onto resolved scene color */
    rpbi.renderPass = r->resolved_rp_load;
    rpbi.framebuffer = r->resolved_fb;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    draw_quad(r, r->ao_apply_pipe, r->ao_apply_ds, &push, 0, 0, r->rt_w, r->rt_h);
    vk->vkCmdEndRenderPass(r->cmd);

    r->stats.ao_passes++;
}

int jpt_vk_renderer_sync_frame(jpt_vk_renderer_t* r) {
    if (!r || !r->in_frame) return 0;
    end_scene_pass_for(r, JPT_VK_BREAK_SYNC);
    world_ds_invalidate(r);
    r->stats.frame_syncs++;
    if (r->collect_timings && !r->ts_invalid_frame) {
        if (r->ts_segments >= JPT_VK_TS_MAX_SEGMENTS || r->ts_pool[r->sc.frame_index] == VK_NULL_HANDLE)
            r->ts_invalid_frame = 1;
        else
            write_timestamp(r, JPT_VK_TS_COUNT + 2u * r->ts_segments);
    }
    {
        double start = now_ms(), took;
        int ok = jpt_vk_swapchain_flush_frame(&r->sc);
        took = now_ms() - start;
        r->stats.cpu_sync_ms += took;
        if (took > r->stats.cpu_sync_max_ms) r->stats.cpu_sync_max_ms = took;
        if (r->collect_timings && !r->ts_invalid_frame) {
            write_timestamp(r, JPT_VK_TS_COUNT + 2u * r->ts_segments + 1u);
            r->ts_segments++;
        }
        return ok;
    }
}

static int ensure_overlay_image(jpt_vk_renderer_t* r, uint32_t w, uint32_t h) {
    if (r->overlay_img != VK_NULL_HANDLE && r->overlay_w == w && r->overlay_h == h) return 1;
    r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
    destroy_rt_image(r, &r->overlay_img, &r->overlay_alloc, &r->overlay_view);
    if (!create_image_2d(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         &r->overlay_img, &r->overlay_alloc, &r->overlay_view))
        return 0;
    r->overlay_w = w;
    r->overlay_h = h;
    r->overlay_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    update_quad_ds_1(r, r->overlay_ds_point, r->overlay_view, r->nearest_clamp_sampler);
    update_quad_ds_1(r, r->overlay_ds_linear, r->overlay_view, r->linear_clamp_sampler);
    return 1;
}

#define JPT_VK_OVERLAY_QUEUE 64u

static int overlay_boxes_overlap(const jpt_render_rect_t* a, const jpt_render_rect_t* b) {
    return a->x < b->x + (int32_t)b->w && b->x < a->x + (int32_t)a->w && a->y < b->y + (int32_t)b->h &&
           b->y < a->y + (int32_t)a->h;
}

int jpt_vk_renderer_flush_overlays(jpt_vk_renderer_t* r, uint32_t legacy_w, uint32_t legacy_h) {
    int ok = 1;
    if (!r || !r->ov_count) return 1;
    if (!r->in_frame || !ensure_overlay_image(r, legacy_w, legacy_h)) ok = 0;
    if (ok) {
        end_scene_pass_for(r, JPT_VK_BREAK_OVERLAY);
        for (uint32_t i = 0; i < r->ov_count && ok; i++)
            ok = upload_rgba_to_image(r, r->overlay_img, &r->overlay_layout, (uint32_t)r->ov_box[i].x, (uint32_t)r->ov_box[i].y,
                                      r->ov_box[i].w, r->ov_box[i].h, 0, r->ov_rgba[i],
                                      (size_t)r->ov_box[i].w * r->ov_box[i].h * 4u);
        ensure_scene_pass(r, 0, NULL, 0.0f, 0);
    }
    for (uint32_t i = 0; i < r->ov_count; i++) {
        if (ok) {
            vk_quad_push_t push;
            const jpt_render_rect_t* box = &r->ov_box[i];
            memset(&push, 0, sizeof push);
            push.pass_rect[0] = (float)box->x / (float)legacy_w * 2.0f - 1.0f;
            push.pass_rect[1] = (float)box->y / (float)legacy_h * 2.0f - 1.0f;
            push.pass_rect[2] = (float)(box->x + (int32_t)box->w) / (float)legacy_w * 2.0f - 1.0f;
            push.pass_rect[3] = (float)(box->y + (int32_t)box->h) / (float)legacy_h * 2.0f - 1.0f;
            push.pass_uv[0] = (float)box->x / (float)legacy_w;
            push.pass_uv[1] = (float)box->y / (float)legacy_h;
            push.pass_uv[2] = (float)(box->x + (int32_t)box->w) / (float)legacy_w;
            push.pass_uv[3] = (float)(box->y + (int32_t)box->h) / (float)legacy_h;
            push.pass_color[0] = push.pass_color[1] = push.pass_color[2] = push.pass_color[3] = 1.0f;
            draw_quad(r, r->ov_opaque[i] ? r->overlay_opaque_pipe : r->overlay_blend_pipe,
                      r->ov_opaque[i] ? r->overlay_ds_linear : r->overlay_ds_point, &push, 0, 0, r->rt_w, r->rt_h);
            r->scene_has_draws = 1;
            r->stats.overlays_drawn++;
        }
        free(r->ov_rgba[i]);
        r->ov_rgba[i] = NULL;
    }
    r->stats.overlay_batches++;
    r->ov_count = 0;
    return ok;
}

int jpt_vk_renderer_queue_overlay(jpt_vk_renderer_t* r, const uint32_t* bgra, size_t bgra_pitch_pixels, const jpt_render_rect_t* box,
                                  uint32_t legacy_w, uint32_t legacy_h, int opaque) {
    uint8_t* rgba;
    uint32_t bw, bh;
    if (!r || !r->in_frame || !bgra || !box || !box->w || !box->h || !legacy_w || !legacy_h) return 0;
    if (box->x < 0 || box->y < 0 || (uint32_t)box->x + box->w > legacy_w || (uint32_t)box->y + box->h > legacy_h) return 0;
    for (uint32_t i = 0; i < r->ov_count; i++)
        if (overlay_boxes_overlap(&r->ov_box[i], box)) {
            jpt_vk_renderer_flush_overlays(r, legacy_w, legacy_h);
            break;
        }
    if (r->ov_count >= JPT_VK_OVERLAY_QUEUE) jpt_vk_renderer_flush_overlays(r, legacy_w, legacy_h);
    bw = box->w;
    bh = box->h;
    rgba = (uint8_t*)malloc((size_t)bw * (size_t)bh * 4u);
    if (!rgba) return 0;
    for (uint32_t y = 0; y < bh; y++) {
        const uint32_t* in = bgra + (size_t)y * bgra_pitch_pixels;
        uint8_t* out = rgba + (size_t)y * bw * 4u;
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t pix = in[x];
            out[x * 4 + 0] = (uint8_t)(pix >> 16);
            out[x * 4 + 1] = (uint8_t)(pix >> 8);
            out[x * 4 + 2] = (uint8_t)pix;
            out[x * 4 + 3] = (uint8_t)(pix >> 24);
        }
    }
    r->ov_rgba[r->ov_count] = rgba;
    r->ov_box[r->ov_count] = *box;
    r->ov_opaque[r->ov_count] = opaque ? 1 : 0;
    r->ov_count++;
    return 1;
}

int jpt_vk_renderer_draw_overlay(jpt_vk_renderer_t* r, const uint32_t* bgra, size_t bgra_pitch_pixels,
                                 const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h, int opaque) {
    return jpt_vk_renderer_queue_overlay(r, bgra, bgra_pitch_pixels, box, legacy_w, legacy_h, opaque) &&
           jpt_vk_renderer_flush_overlays(r, legacy_w, legacy_h);
}

typedef struct {
    VkImage src;
    VkImageLayout src_layout;
    VkImageLayout src_restore;
    VkImage resolve_to;
    VkImage rb;
    VkBuffer buf;
    VkOffset3D src_a, src_b, dst_a, dst_b;
    VkRect2D copy;
    uint32_t rt_w, rt_h;
    uint32_t copy_count;
    VkBufferImageCopy copies[JPT_VK_READ_RECT_MAX];
} readback_job_t;

static void record_scene_readback(jpt_vk_renderer_t* r, VkCommandBuffer cmd, void* user) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    readback_job_t* j = (readback_job_t*)user;
    VkImageBlit blit;
    VkBufferImageCopy copy;
    VkImage src = j->src;
    VkImageLayout src_layout = j->src_layout;

    if (j->resolve_to != VK_NULL_HANDLE) {
        VkImageResolve res;
        cmd_image_barrier(vk, cmd, j->src, VK_IMAGE_ASPECT_COLOR_BIT, j->src_layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        cmd_image_barrier(vk, cmd, j->resolve_to, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
        memset(&res, 0, sizeof res);
        res.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.srcSubresource.layerCount = 1;
        res.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.dstSubresource.layerCount = 1;
        res.extent.width = j->rt_w;
        res.extent.height = j->rt_h;
        res.extent.depth = 1;
        vk->vkCmdResolveImage(cmd, j->src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->resolve_to,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
        cmd_image_barrier(vk, cmd, j->src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          j->src_restore, 0, 1, 0, 1);
        cmd_image_barrier(vk, cmd, j->resolve_to, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        src = j->resolve_to;
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    } else {
        cmd_image_barrier(vk, cmd, j->src, VK_IMAGE_ASPECT_COLOR_BIT, j->src_layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    (void)src_layout;

    cmd_image_barrier(vk, cmd, j->rb, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[0] = j->src_a;
    blit.srcOffsets[1] = j->src_b;
    blit.dstOffsets[0] = j->dst_a;
    blit.dstOffsets[1] = j->dst_b;
    vk->vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->rb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
    cmd_image_barrier(vk, cmd, j->rb, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
    memset(&copy, 0, sizeof copy);
    copy.bufferRowLength = j->copy.extent.width;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = j->copy.offset.x;
    copy.imageOffset.y = j->copy.offset.y;
    copy.imageExtent.width = j->copy.extent.width;
    copy.imageExtent.height = j->copy.extent.height;
    copy.imageExtent.depth = 1;
    if (j->copy_count)
        vk->vkCmdCopyImageToBuffer(cmd, j->rb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->buf, j->copy_count, j->copies);
    else
        vk->vkCmdCopyImageToBuffer(cmd, j->rb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->buf, 1, &copy);
    if (j->resolve_to == VK_NULL_HANDLE)
        cmd_image_barrier(vk, cmd, j->src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          j->src_restore, 0, 1, 0, 1);
}

static int scene_readback_submit(jpt_vk_renderer_t* r, readback_job_t* job) {
    if (!r->in_frame) return run_one_shot_cmd(r, record_scene_readback, job, JPT_VK_OS_READBACK);
    end_scene_pass_for(r, JPT_VK_BREAK_READBACK);
    record_scene_readback(r, r->cmd, job);
    r->stats.readbacks_in_frame++;
    return jpt_vk_renderer_sync_frame(r);
}

int jpt_vk_renderer_read_scene_rgb565(jpt_vk_renderer_t* r, const jpt_render_rect_t* box, uint32_t legacy_w,
                                      uint32_t legacy_h, uint16_t* dst, size_t dst_pitch_pixels) {
    readback_job_t job;
    VkDeviceSize need;
    const uint8_t* mapped;

    if (!r || !box || !dst || !box->w || !box->h || !legacy_w || !legacy_h) return 0;
    if (box->x < 0 || box->y < 0 || (uint32_t)box->x + box->w > legacy_w || (uint32_t)box->y + box->h > legacy_h) return 0;
    if (!r->in_frame && !r->scene_resolved_ready) return 0;

    if (r->rb_img == VK_NULL_HANDLE || r->rb_w != legacy_w || r->rb_h != legacy_h) {
        destroy_rt_image(r, &r->rb_img, &r->rb_alloc, &r->rb_view);
        if (!create_image_2d(r, legacy_w, legacy_h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &r->rb_img, &r->rb_alloc, &r->rb_view))
            return 0;
        r->rb_w = legacy_w;
        r->rb_h = legacy_h;
    }
    need = (VkDeviceSize)box->w * (VkDeviceSize)box->h * 4u;
    if (r->rb_buf == VK_NULL_HANDLE || r->rb_buf_bytes < need) {
        VkDeviceSize cap = (VkDeviceSize)legacy_w * (VkDeviceSize)legacy_h * 4u;
        if (cap < need) cap = need;
        if (r->rb_buf != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rb_buf, &r->rb_buf_alloc);
        if (jpt_vk_create_buffer(&r->ctx.alloc, cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &r->rb_buf, &r->rb_buf_alloc) != VK_SUCCESS) {
            r->rb_buf = VK_NULL_HANDLE;
            r->rb_buf_bytes = 0;
            return 0;
        }
        r->rb_buf_bytes = cap;
    }

    memset(&job, 0, sizeof job);
    if (r->in_frame) {
        job.src = r->active_msaa > 1 ? r->scene_color : r->scene_resolved;
        job.src_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        job.src_restore = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        job.resolve_to = r->active_msaa > 1 ? r->scene_behind : VK_NULL_HANDLE;
    } else {
        job.src = r->scene_resolved;
        job.src_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        job.src_restore = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    job.rb = r->rb_img;
    job.buf = r->rb_buf;
    job.rt_w = r->rt_w;
    job.rt_h = r->rt_h;
    job.src_a.x = (int32_t)((uint64_t)box->x * r->rt_w / legacy_w);
    job.src_a.y = (int32_t)((uint64_t)box->y * r->rt_h / legacy_h);
    job.src_b.x = (int32_t)(((uint64_t)(box->x + (int32_t)box->w) * r->rt_w + legacy_w - 1u) / legacy_w);
    job.src_b.y = (int32_t)(((uint64_t)(box->y + (int32_t)box->h) * r->rt_h + legacy_h - 1u) / legacy_h);
    job.src_a.z = 0;
    job.src_b.z = 1;
    job.dst_a.x = box->x;
    job.dst_a.y = box->y;
    job.dst_a.z = 0;
    job.dst_b.x = box->x + (int32_t)box->w;
    job.dst_b.y = box->y + (int32_t)box->h;
    job.dst_b.z = 1;
    job.copy.offset.x = box->x;
    job.copy.offset.y = box->y;
    job.copy.extent.width = box->w;
    job.copy.extent.height = box->h;
    if (!scene_readback_submit(r, &job)) return 0;
    r->stats.scene_readbacks++;
    r->stats.scene_readback_pixels += (uint64_t)box->w * box->h;
    if (!r->rb_buf_alloc.mapped) return 0;

    mapped = (const uint8_t*)r->rb_buf_alloc.mapped;
    for (uint32_t y = 0; y < box->h; y++) {
        const uint8_t* in = mapped + (size_t)y * box->w * 4u;
        uint16_t* out = dst + (size_t)y * dst_pitch_pixels;
        for (uint32_t x = 0; x < box->w; x++) {
            uint32_t r5 = ((uint32_t)in[x * 4 + 0] * 31u + 127u) / 255u;
            uint32_t g6 = ((uint32_t)in[x * 4 + 1] * 63u + 127u) / 255u;
            uint32_t b5 = ((uint32_t)in[x * 4 + 2] * 31u + 127u) / 255u;
            out[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
    return 1;
}

int jpt_vk_renderer_read_scene_rects_rgb565(jpt_vk_renderer_t* r, const jpt_render_rect_t* rects, uint32_t count,
                                            uint32_t legacy_w, uint32_t legacy_h, uint16_t* dst) {
    readback_job_t job;
    VkDeviceSize need = 0, cursor = 0;
    int32_t ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    const uint8_t* mapped;

    if (!r || !rects || !dst || !count || count > JPT_VK_READ_RECT_MAX || !legacy_w || !legacy_h) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const jpt_render_rect_t* b = &rects[i];
        if (!b->w || !b->h || b->x < 0 || b->y < 0 || (uint32_t)b->x + b->w > legacy_w || (uint32_t)b->y + b->h > legacy_h)
            return 0;
        need += (VkDeviceSize)b->w * b->h * 4u;
        if (!i || b->x < ux0) ux0 = b->x;
        if (!i || b->y < uy0) uy0 = b->y;
        if (!i || b->x + (int32_t)b->w > ux1) ux1 = b->x + (int32_t)b->w;
        if (!i || b->y + (int32_t)b->h > uy1) uy1 = b->y + (int32_t)b->h;
    }
    if (!r->in_frame && !r->scene_resolved_ready) return 0;
    if (r->rb_img == VK_NULL_HANDLE || r->rb_w != legacy_w || r->rb_h != legacy_h) {
        destroy_rt_image(r, &r->rb_img, &r->rb_alloc, &r->rb_view);
        if (!create_image_2d(r, legacy_w, legacy_h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &r->rb_img, &r->rb_alloc, &r->rb_view))
            return 0;
        r->rb_w = legacy_w;
        r->rb_h = legacy_h;
    }
    if (r->rb_buf == VK_NULL_HANDLE || r->rb_buf_bytes < need) {
        VkDeviceSize cap = (VkDeviceSize)legacy_w * (VkDeviceSize)legacy_h * 4u;
        if (cap < need) cap = need;
        if (r->rb_buf != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rb_buf, &r->rb_buf_alloc);
        if (jpt_vk_create_buffer(&r->ctx.alloc, cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &r->rb_buf, &r->rb_buf_alloc) != VK_SUCCESS) {
            r->rb_buf = VK_NULL_HANDLE;
            r->rb_buf_bytes = 0;
            return 0;
        }
        r->rb_buf_bytes = cap;
    }
    memset(&job, 0, sizeof job);
    if (r->in_frame) {
        job.src = r->active_msaa > 1 ? r->scene_color : r->scene_resolved;
        job.src_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        job.src_restore = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        job.resolve_to = r->active_msaa > 1 ? r->scene_behind : VK_NULL_HANDLE;
    } else {
        job.src = r->scene_resolved;
        job.src_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        job.src_restore = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    job.rb = r->rb_img;
    job.buf = r->rb_buf;
    job.rt_w = r->rt_w;
    job.rt_h = r->rt_h;
    job.src_a.x = (int32_t)((uint64_t)ux0 * r->rt_w / legacy_w);
    job.src_a.y = (int32_t)((uint64_t)uy0 * r->rt_h / legacy_h);
    job.src_b.x = (int32_t)(((uint64_t)ux1 * r->rt_w + legacy_w - 1u) / legacy_w);
    job.src_b.y = (int32_t)(((uint64_t)uy1 * r->rt_h + legacy_h - 1u) / legacy_h);
    job.src_b.z = 1;
    job.dst_a.x = ux0;
    job.dst_a.y = uy0;
    job.dst_b.x = ux1;
    job.dst_b.y = uy1;
    job.dst_b.z = 1;
    for (uint32_t i = 0; i < count; i++) {
        VkBufferImageCopy* c = &job.copies[i];
        c->bufferOffset = cursor;
        c->bufferRowLength = rects[i].w;
        c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c->imageSubresource.layerCount = 1;
        c->imageOffset.x = rects[i].x;
        c->imageOffset.y = rects[i].y;
        c->imageExtent.width = rects[i].w;
        c->imageExtent.height = rects[i].h;
        c->imageExtent.depth = 1;
        cursor += (VkDeviceSize)rects[i].w * rects[i].h * 4u;
    }
    job.copy_count = count;
    if (!scene_readback_submit(r, &job)) return 0;
    r->stats.scene_readbacks++;
    r->stats.scene_readback_rects += count;
    if (!r->rb_buf_alloc.mapped) return 0;
    mapped = (const uint8_t*)r->rb_buf_alloc.mapped;
    for (VkDeviceSize i = 0; i < need / 4u; i++) {
        uint32_t r5 = ((uint32_t)mapped[i * 4 + 0] * 31u + 127u) / 255u;
        uint32_t g6 = ((uint32_t)mapped[i * 4 + 1] * 63u + 127u) / 255u;
        uint32_t b5 = ((uint32_t)mapped[i * 4 + 2] * 31u + 127u) / 255u;
        dst[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
    return 1;
}

static VkPipeline pick_internal_layer_pipe(const jpt_vk_renderer_t* r, const vk_layer_tex_t* l) {
    if (l->blend_mode == 2) return r->copy_premul_pipe;
    if (l->blend_mode == 1) return r->copy_blend_pipe;
    return r->copy_opaque_pipe;
}

static VkPipeline pick_present_layer_pipe(const jpt_vk_renderer_t* r, const vk_layer_tex_t* l) {
    if (l->apply_gamma) {
        if (l->blend_mode == 2) return r->present_premul_pipe;
        if (l->blend_mode == 1) return r->present_blend_pipe;
        return r->present_pipe;
    }
    if (l->blend_mode == 2) return r->copy_present_premul_pipe;
    if (l->blend_mode == 1) return r->copy_present_blend_pipe;
    return r->copy_present_opaque_pipe;
}

int jpt_vk_ui_scissor(const float clip[4], uint32_t target_w, uint32_t target_h, int32_t rect_x, int32_t rect_y,
                      uint32_t rect_w, uint32_t rect_h, int32_t out[4]) {
    float sx, sy, x0, y0, x1, y1;
    if (!target_w || !target_h || !rect_w || !rect_h) return 0;
    sx = (float)rect_w / (float)target_w;
    sy = (float)rect_h / (float)target_h;
    x0 = (float)rect_x + clip[0] * sx;
    y0 = (float)rect_y + clip[1] * sy;
    x1 = (float)rect_x + clip[2] * sx;
    y1 = (float)rect_y + clip[3] * sy;
    if (x0 < (float)rect_x) x0 = (float)rect_x;
    if (y0 < (float)rect_y) y0 = (float)rect_y;
    if (x1 > (float)(rect_x + (int32_t)rect_w)) x1 = (float)(rect_x + (int32_t)rect_w);
    if (y1 > (float)(rect_y + (int32_t)rect_h)) y1 = (float)(rect_y + (int32_t)rect_h);
    if (x1 <= x0 || y1 <= y0) return 0;
    out[0] = (int32_t)x0;
    out[1] = (int32_t)y0;
    out[2] = (int32_t)ceilf(x1) - out[0];
    out[3] = (int32_t)ceilf(y1) - out[1];
    return out[2] > 0 && out[3] > 0;
}

void jpt_vk_ui_projection(const float display_pos[2], const float display_size[2], float scale[2], float translate[2]) {
    scale[0] = 2.0f / display_size[0];
    scale[1] = 2.0f / display_size[1];
    translate[0] = -1.0f - display_pos[0] * scale[0];
    translate[1] = -1.0f - display_pos[1] * scale[1];
}

static void destroy_ui_pipeline(jpt_vk_renderer_t* r) {
    destroy_pipe(r, &r->ui_pipe);
    r->ui_pipe_pass = VK_NULL_HANDLE;
}

static void destroy_ui_resources(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    destroy_ui_pipeline(r);
    if (r->ui_pl != VK_NULL_HANDLE) {
        vk->vkDestroyPipelineLayout(r->ctx.device, r->ui_pl, NULL);
        r->ui_pl = VK_NULL_HANDLE;
        r->ctx.lifetime.pipeline_layouts--;
    }
    if (r->ui_dsl != VK_NULL_HANDLE) {
        vk->vkDestroyDescriptorSetLayout(r->ctx.device, r->ui_dsl, NULL);
        r->ui_dsl = VK_NULL_HANDLE;
    }
    for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
        if (r->ui_dpool[i] != VK_NULL_HANDLE) {
            vk->vkDestroyDescriptorPool(r->ctx.device, r->ui_dpool[i], NULL);
            r->ui_dpool[i] = VK_NULL_HANDLE;
            r->ctx.lifetime.descriptor_pools--;
        }
        jpt_vk_destroy_buffer(&r->ctx.alloc, &r->ui_vb[i], &r->ui_vb_alloc[i]);
        jpt_vk_destroy_buffer(&r->ctx.alloc, &r->ui_ib[i], &r->ui_ib_alloc[i]);
        r->ui_vb_cap[i] = r->ui_ib_cap[i] = 0;
    }
    for (uint32_t i = 0; i < JPT_VK_UI_TEXTURES; i++) {
        destroy_rt_image(r, &r->ui_tex[i].img, &r->ui_tex[i].alloc, &r->ui_tex[i].view);
        memset(&r->ui_tex[i], 0, sizeof r->ui_tex[i]);
    }
    free(r->ui_cmds);
    free(r->ui_cmd_sets);
    free(r->ui_expand);
    r->ui_cmds = NULL;
    r->ui_cmd_sets = NULL;
    r->ui_expand = NULL;
    r->ui_cmd_cap = 0;
    r->ui_expand_cap = 0;
    r->ui_active = 0;
}

static int create_ui_pipeline(jpt_vk_renderer_t* r) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription bind;
    VkVertexInputAttributeDescription attrs[3];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo pci;

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shaders[JPT_VK_SH_UI_VERT];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shaders[JPT_VK_SH_UI_FRAG];
    stages[1].pName = "main";

    bind.binding = 0;
    bind.stride = 20;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = 8;
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attrs[2].offset = 16;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyns;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = r->ui_pl;
    pci.renderPass = r->sc.present_pass;
    if (r->ctx.vk.vkCreateGraphicsPipelines(r->ctx.device, r->ctx.pipeline_cache, 1, &pci, NULL, &r->ui_pipe) !=
        VK_SUCCESS)
        return 0;
    r->ctx.lifetime.pipelines++;
    r->stats.pipelines_created++;
    r->ui_pipe_pass = r->sc.present_pass;
    return 1;
}

static int ensure_ui_resources(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    if (r->ui_pipe != VK_NULL_HANDLE && r->ui_pipe_pass == r->sc.present_pass) return 1;
    if (r->ui_pipe != VK_NULL_HANDLE) destroy_ui_pipeline(r);
    if (r->ui_dsl == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding b;
        VkDescriptorSetLayoutCreateInfo dci;
        VkPushConstantRange pcr;
        VkPipelineLayoutCreateInfo plci;
        VkDescriptorPoolSize psize;
        VkDescriptorPoolCreateInfo dpci;
        memset(&b, 0, sizeof b);
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dci.bindingCount = 1;
        dci.pBindings = &b;
        if (vk->vkCreateDescriptorSetLayout(r->ctx.device, &dci, NULL, &r->ui_dsl) != VK_SUCCESS) return 0;
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset = 0;
        pcr.size = 16;
        memset(&plci, 0, sizeof plci);
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &r->ui_dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vk->vkCreatePipelineLayout(r->ctx.device, &plci, NULL, &r->ui_pl) != VK_SUCCESS) return 0;
        r->ctx.lifetime.pipeline_layouts++;
        psize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        psize.descriptorCount = JPT_VK_UI_TEXTURES;
        memset(&dpci, 0, sizeof dpci);
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = JPT_VK_UI_TEXTURES;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &psize;
        for (int i = 0; i < JPT_VK_FRAMES_IN_FLIGHT; i++) {
            if (vk->vkCreateDescriptorPool(r->ctx.device, &dpci, NULL, &r->ui_dpool[i]) != VK_SUCCESS) return 0;
            r->ctx.lifetime.descriptor_pools++;
        }
    }
    return create_ui_pipeline(r);
}

static int ensure_ui_buffer(jpt_vk_renderer_t* r, VkBuffer* buf, jpt_vk_allocation_t* alloc, VkDeviceSize* cap,
                            VkDeviceSize need, VkBufferUsageFlags usage) {
    VkDeviceSize bytes;
    if (*buf != VK_NULL_HANDLE && *cap >= need) return 1;
    bytes = *cap ? *cap : (256u * 1024u);
    while (bytes < need) bytes *= 2u;
    jpt_vk_destroy_buffer(&r->ctx.alloc, buf, alloc);
    if (jpt_vk_create_buffer(&r->ctx.alloc, bytes, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             0, 1, buf, alloc) != VK_SUCCESS ||
        !alloc->mapped) {
        jpt_vk_destroy_buffer(&r->ctx.alloc, buf, alloc);
        *cap = 0;
        return 0;
    }
    *cap = bytes;
    return 1;
}

static vk_ui_tex_t* ui_texture_slot(jpt_vk_renderer_t* r, uint32_t id) {
    vk_ui_tex_t* free_slot = NULL;
    for (uint32_t i = 0; i < JPT_VK_UI_TEXTURES; i++) {
        if (r->ui_tex[i].id == id) return &r->ui_tex[i];
        if (!r->ui_tex[i].id && !free_slot) free_slot = &r->ui_tex[i];
    }
    return free_slot;
}

static int ui_upload_texture(jpt_vk_renderer_t* r, vk_ui_tex_t* t, const jpt_vk_ui_texture_t* src) {
    const uint8_t* data = src->pixels;
    const size_t bytes = (size_t)src->width * src->height * 4u;
    if (!src->width || !src->height || !src->pixels) return 0;
    if (t->img != VK_NULL_HANDLE && (t->w != src->width || t->h != src->height)) {
        retire_image(r, &t->img, &t->alloc, &t->view);
        t->revision = 0;
    }
    if (t->img == VK_NULL_HANDLE) {
        if (!create_image_2d(r, src->width, src->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             &t->img, &t->alloc, &t->view))
            return 0;
        t->w = src->width;
        t->h = src->height;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->revision = 0;
    }
    t->id = src->id;
    if (t->revision == src->revision && t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return 1;
    if (src->bytes_per_pixel == 1) {
        if (r->ui_expand_cap < bytes) {
            uint8_t* grown = (uint8_t*)realloc(r->ui_expand, bytes);
            if (!grown) return 0;
            r->ui_expand = grown;
            r->ui_expand_cap = bytes;
        }
        for (size_t i = 0; i < (size_t)src->width * src->height; i++) {
            r->ui_expand[i * 4u + 0] = 255;
            r->ui_expand[i * 4u + 1] = 255;
            r->ui_expand[i * 4u + 2] = 255;
            r->ui_expand[i * 4u + 3] = src->pixels[i];
        }
        data = r->ui_expand;
    } else if (src->bytes_per_pixel != 4) {
        return 0;
    }
    if (!upload_rgba_to_image(r, t->img, &t->layout, 0, 0, src->width, src->height, 0, data, bytes)) return 0;
    t->revision = src->revision;
    r->stats.ui_texture_uploads++;
    return 1;
}

int jpt_vk_renderer_submit_ui(jpt_vk_renderer_t* r, const jpt_vk_ui_draw_t* d) {
    const jpt_vk_loader_t* vk;
    VkDescriptorSet sets[JPT_VK_UI_TEXTURES];
    VkDeviceSize vb_bytes, ib_bytes;
    uint32_t fi;
    double t0;
    if (!r) return 0;
    r->ui_active = 0;
    if (!d || !r->in_frame || !d->vertices || !d->indices || !d->cmds || !d->vertex_count || !d->index_count ||
        !d->cmd_count || d->texture_count > JPT_VK_UI_TEXTURES || !d->display_size[0] || !d->display_size[1])
        return 0;
    vk = &r->ctx.vk;
    t0 = r->collect_timings ? now_ms() : 0.0;
    if (!ensure_ui_resources(r)) return 0;
    fi = r->sc.frame_index;
    vk->vkResetDescriptorPool(r->ctx.device, r->ui_dpool[fi], 0);
    for (uint32_t i = 0; i < d->texture_count; i++) {
        vk_ui_tex_t* t = ui_texture_slot(r, d->textures[i].id);
        VkDescriptorSetAllocateInfo dsai;
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w;
        if (!t || !ui_upload_texture(r, t, &d->textures[i])) return 0;
        memset(&dsai, 0, sizeof dsai);
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = r->ui_dpool[fi];
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &r->ui_dsl;
        if (vk->vkAllocateDescriptorSets(r->ctx.device, &dsai, &sets[i]) != VK_SUCCESS) return 0;
        ii.sampler = r->linear_clamp_sampler;
        ii.imageView = t->view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vk->vkUpdateDescriptorSets(r->ctx.device, 1, &w, 0, NULL);
        r->stats.ui_texture_binds++;
    }
    vb_bytes = (VkDeviceSize)d->vertex_count * 20u;
    ib_bytes = (VkDeviceSize)d->index_count * sizeof(uint16_t);
    if (!ensure_ui_buffer(r, &r->ui_vb[fi], &r->ui_vb_alloc[fi], &r->ui_vb_cap[fi], vb_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensure_ui_buffer(r, &r->ui_ib[fi], &r->ui_ib_alloc[fi], &r->ui_ib_cap[fi], ib_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return 0;
    if (r->ui_cmd_cap < d->cmd_count) {
        uint32_t cap = r->ui_cmd_cap ? r->ui_cmd_cap : 64u;
        jpt_vk_ui_cmd_t* cmds;
        VkDescriptorSet* cmd_sets;
        while (cap < d->cmd_count) cap *= 2u;
        cmds = (jpt_vk_ui_cmd_t*)realloc(r->ui_cmds, (size_t)cap * sizeof *cmds);
        if (cmds) r->ui_cmds = cmds;
        cmd_sets = (VkDescriptorSet*)realloc(r->ui_cmd_sets, (size_t)cap * sizeof *cmd_sets);
        if (cmd_sets) r->ui_cmd_sets = cmd_sets;
        if (!cmds || !cmd_sets) return 0;
        r->ui_cmd_cap = cap;
    }
    memcpy(r->ui_vb_alloc[fi].mapped, d->vertices, (size_t)vb_bytes);
    memcpy(r->ui_ib_alloc[fi].mapped, d->indices, (size_t)ib_bytes);
    memcpy(r->ui_cmds, d->cmds, (size_t)d->cmd_count * sizeof *d->cmds);
    for (uint32_t i = 0; i < d->cmd_count; i++) {
        if (d->cmds[i].texture >= d->texture_count) return 0;
        r->ui_cmd_sets[i] = sets[d->cmds[i].texture];
    }
    r->ui_cmd_count = d->cmd_count;
    r->ui_frame_index = fi;
    r->ui_target_w = d->target_w;
    r->ui_target_h = d->target_h;
    jpt_vk_ui_projection(d->display_pos, d->display_size, r->ui_scale, r->ui_translate);
    r->ui_active = 1;
    r->stats.ui_draws += d->cmd_count;
    r->stats.ui_vertices += d->vertex_count;
    r->stats.ui_indices += d->index_count;
    r->stats.ui_frames++;
    if (r->collect_timings) r->stats.cpu_ui_ms += now_ms() - t0;
    return 1;
}

void jpt_vk_renderer_clear_ui(jpt_vk_renderer_t* r) {
    if (r) r->ui_active = 0;
}

static void record_ui(jpt_vk_renderer_t* r, const jpt_render_rect_t* area) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    const uint32_t fi = r->ui_frame_index;
    VkViewport vp;
    VkDeviceSize zero = 0;
    double t0 = r->collect_timings ? now_ms() : 0.0;
    float push[4];
    if (!r->ui_active || r->ui_pipe == VK_NULL_HANDLE) return;
    r->ui_active = 0;
    vp.x = (float)area->x;
    vp.y = (float)area->y;
    vp.width = (float)area->w;
    vp.height = (float)area->h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    push[0] = r->ui_scale[0];
    push[1] = r->ui_scale[1];
    push[2] = r->ui_translate[0];
    push[3] = r->ui_translate[1];
    r->world_bound_valid = 0;
    vk->vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ui_pipe);
    vk->vkCmdSetViewport(r->cmd, 0, 1, &vp);
    vk->vkCmdPushConstants(r->cmd, r->ui_pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    vk->vkCmdBindVertexBuffers(r->cmd, 0, 1, &r->ui_vb[fi], &zero);
    vk->vkCmdBindIndexBuffer(r->cmd, r->ui_ib[fi], 0, VK_INDEX_TYPE_UINT16);
    for (uint32_t i = 0; i < r->ui_cmd_count; i++) {
        const jpt_vk_ui_cmd_t* c = &r->ui_cmds[i];
        int32_t sc[4];
        VkRect2D rect;
        if (!c->idx_count ||
            !jpt_vk_ui_scissor(c->clip, r->ui_target_w, r->ui_target_h, area->x, area->y, area->w, area->h, sc))
            continue;
        rect.offset.x = sc[0];
        rect.offset.y = sc[1];
        rect.extent.width = (uint32_t)sc[2];
        rect.extent.height = (uint32_t)sc[3];
        vk->vkCmdSetScissor(r->cmd, 0, 1, &rect);
        vk->vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ui_pl, 0, 1, &r->ui_cmd_sets[i], 0, NULL);
        vk->vkCmdDrawIndexed(r->cmd, c->idx_count, 1, c->idx_offset, c->vtx_offset, 0);
    }
    if (r->collect_timings) r->stats.cpu_ui_ms += now_ms() - t0;
}

static void record_last_frame_copy(jpt_vk_renderer_t* r) {
    const jpt_vk_loader_t* vk = &r->ctx.vk;
    VkImageCopy copy;
    if (r->last_img == VK_NULL_HANDLE) return;
    cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
    cmd_image_barrier(vk, r->cmd, r->last_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
    memset(&copy, 0, sizeof copy);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent.width = r->rt_w;
    copy.extent.height = r->rt_h;
    copy.extent.depth = 1;
    vk->vkCmdCopyImage(r->cmd, r->scene_resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->last_img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    cmd_image_barrier(vk, r->cmd, r->last_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
    cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 1);
    r->last_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    r->last_valid = 1;
    r->stats.last_frame_copies++;
}

int jpt_vk_renderer_end_and_present(jpt_vk_renderer_t* r) {
    static const jpt_render_layer_t k_layer_order[] = {
        JPT_LAYER_VIDEO,
        JPT_LAYER_FRONTEND_HD,
        JPT_LAYER_LOADING_HD,
        JPT_LAYER_CURSOR,
        JPT_LAYER_VIDEO_SUBTITLES,
        JPT_LAYER_HOST_OVERLAYS,
    };
    const jpt_vk_loader_t* vk;
    VkRenderPassBeginInfo rpbi;
    VkClearValue clear_black;
    vk_quad_push_t push;
    jpt_vk_sc_action_t act;
    int any_internal = 0;

    if (!r || !r->in_frame) return 0;
    vk = &r->ctx.vk;
    end_scene_pass_for(r, JPT_VK_BREAK_FRAME_END);
    if (r->collect_timings && !r->ts_invalid_frame) write_timestamp(r, 1);

    /* Resolve MSAA color to single-sample scene_resolved before post-processing / overlays / present */
    if (r->active_msaa > 1) {
        VkImageResolve res;
        cmd_image_barrier(vk, r->cmd, r->scene_color, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
        memset(&res, 0, sizeof res);
        res.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.srcSubresource.layerCount = 1;
        res.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        res.dstSubresource.layerCount = 1;
        res.extent.width = r->rt_w;
        res.extent.height = r->rt_h;
        res.extent.depth = 1;
        vk->vkCmdResolveImage(r->cmd, r->scene_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->scene_resolved,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
        cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 1);
    }

    if (r->collect_timings && !r->ts_invalid_frame) write_timestamp(r, 2);
    for (size_t k = 0; k < sizeof k_layer_order / sizeof k_layer_order[0]; k++) {
        const vk_layer_tex_t* l = &r->layers[k_layer_order[k]];
        if (l->active && !l->native_res) any_internal = 1;
    }
    if (!any_internal) record_last_frame_copy(r);
    run_ao_pass(r);
    if (r->collect_timings && !r->ts_invalid_frame) write_timestamp(r, 3);

    /* Internal-resolution layers composited onto resolved scene */
    {
        if (any_internal) {
            memset(&rpbi, 0, sizeof rpbi);
            rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbi.renderPass = r->resolved_rp_load;
            rpbi.framebuffer = r->resolved_fb;
            rpbi.renderArea.extent.width = r->rt_w;
            rpbi.renderArea.extent.height = r->rt_h;
            vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

            memset(&push, 0, sizeof push);
            push.pass_rect[0] = -1.0f;
            push.pass_rect[1] = -1.0f;
            push.pass_rect[2] = 1.0f;
            push.pass_rect[3] = 1.0f;
            push.pass_uv[2] = 1.0f;
            push.pass_uv[3] = 1.0f;
            push.pass_color[0] = push.pass_color[1] = push.pass_color[2] = push.pass_color[3] = 1.0f;

            for (size_t k = 0; k < sizeof k_layer_order / sizeof k_layer_order[0]; k++) {
                const vk_layer_tex_t* l = &r->layers[k_layer_order[k]];
                if (l->active && !l->native_res) {
                    push.pass_rect[0] = l->norm_rect[0] * 2.0f - 1.0f;
                    push.pass_rect[1] = l->norm_rect[1] * 2.0f - 1.0f;
                    push.pass_rect[2] = l->norm_rect[2] * 2.0f - 1.0f;
                    push.pass_rect[3] = l->norm_rect[3] * 2.0f - 1.0f;
                    draw_quad(r, pick_internal_layer_pipe(r, l), l->point_filter ? l->desc_point : l->desc_linear,
                              &push, 0, 0, r->rt_w, r->rt_h);
                }
            }
            vk->vkCmdEndRenderPass(r->cmd);
            record_last_frame_copy(r);
        }
    }

    if (r->gamma_dirty) {
        VkImageLayout gl = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        upload_rgba_to_image(r, r->gamma_lut_img, &gl, 0, 0, 256, 1, 0, r->gamma_rgba, sizeof r->gamma_rgba);
        r->gamma_dirty = 0;
    }

    /* Transition resolved scene for final presentation sampling */
    cmd_image_barrier(vk, r->cmd, r->scene_resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
    r->scene_resolved_ready = 1;

    if (r->collect_timings && !r->ts_invalid_frame) write_timestamp(r, 4);

    /* Final Present Pass (letterbox/stretch + gamma LUT + native-resolution layers in D3D11 order) */
    memset(&clear_black, 0, sizeof clear_black);
    clear_black.color.float32[3] = 1.0f;
    memset(&rpbi, 0, sizeof rpbi);
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = r->sc.present_pass;
    rpbi.framebuffer = r->present_fb;
    rpbi.renderArea.extent = r->present_extent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear_black;
    vk->vkCmdBeginRenderPass(r->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    memset(&push, 0, sizeof push);
    push.pass_rect[0] = -1.0f;
    push.pass_rect[1] = -1.0f;
    push.pass_rect[2] = 1.0f;
    push.pass_rect[3] = 1.0f;
    push.pass_uv[2] = 1.0f;
    push.pass_uv[3] = 1.0f;
    push.pass_color[0] = push.pass_color[1] = push.pass_color[2] = push.pass_color[3] = 1.0f;

    jpt_vk_compute_geometry(r->cfg.guest_w, r->cfg.guest_h, r->rt_w, r->rt_h, r->present_extent.width,
                            r->present_extent.height, r->cfg.stretch, &r->geom);

    draw_quad(r, r->present_pipe, r->present_ds, &push, (uint32_t)r->geom.present_rect.x,
              (uint32_t)r->geom.present_rect.y, r->geom.present_rect.w, r->geom.present_rect.h);
    if (r->video_active) record_video_final(r);

    for (size_t k = 0; k < sizeof k_layer_order / sizeof k_layer_order[0]; k++) {
        const vk_layer_tex_t* l = &r->layers[k_layer_order[k]];
        if (l->active && l->native_res) {
            push.pass_rect[0] = l->norm_rect[0] * 2.0f - 1.0f;
            push.pass_rect[1] = l->norm_rect[1] * 2.0f - 1.0f;
            push.pass_rect[2] = l->norm_rect[2] * 2.0f - 1.0f;
            push.pass_rect[3] = l->norm_rect[3] * 2.0f - 1.0f;
            draw_quad(r, pick_present_layer_pipe(r, l), l->point_filter ? l->desc_point : l->desc_linear, &push,
                      (uint32_t)r->geom.present_rect.x, (uint32_t)r->geom.present_rect.y, r->geom.present_rect.w,
                      r->geom.present_rect.h);
        }
    }
    record_ui(r, &r->geom.present_rect);

    vk->vkCmdEndRenderPass(r->cmd);
    if (r->collect_timings && r->ts_invalid_frame) r->stats.gpu_frames_unsampled++;
    if (r->collect_timings && !r->ts_invalid_frame) {
        write_timestamp(r, 5);
        if (r->ts_pool[r->sc.frame_index] != VK_NULL_HANDLE) {
            r->ts_pending[r->sc.frame_index] = 1;
            r->ts_segments_done[r->sc.frame_index] = r->ts_segments;
        }
    }

    r->in_frame = 0;
    if (r->collect_timings) r->stats.cpu_frame_ms += now_ms() - r->frame_start_ms;
    {
        double submit_start = r->collect_timings ? now_ms() : 0.0;
        act = jpt_vk_swapchain_end_frame(&r->sc);
        if (r->collect_timings) r->stats.cpu_present_ms += now_ms() - submit_start;
    }
    if (act == JPT_VK_SC_ACTION_DEVICE_LOST) {
        jpt_vk_log(&r->ctx, "Vulkan device lost during presentation; stopping cleanly");
        return 0;
    }
    r->frame_seq++;
    r->stats.frames_presented++;
    return 1;
}

int jpt_vk_renderer_readback_rgba(jpt_vk_renderer_t* r, uint8_t* dst_rgba, uint32_t* out_w, uint32_t* out_h,
                                  size_t dst_bytes) {
    if (!r || !dst_rgba) return 0;
    if (r->ctx.headless) {
        if (out_w) *out_w = r->sc.extent.width;
        if (out_h) *out_h = r->sc.extent.height;
        return jpt_vk_swapchain_readback_headless(&r->sc, dst_rgba, dst_bytes);
    }

    /* Windowed readback from scene_resolved (save previews / diagnostics) */
    if (!r->scene_resolved_ready || r->in_frame) return 0;
    VkDeviceSize needed = (VkDeviceSize)r->rt_w * (VkDeviceSize)r->rt_h * 4u;
    if (dst_bytes < (size_t)needed) return 0;
    if (out_w) *out_w = r->rt_w;
    if (out_h) *out_h = r->rt_h;

    VkBuffer staging = VK_NULL_HANDLE;
    jpt_vk_allocation_t alloc;
    r->ctx.vk.vkDeviceWaitIdle(r->ctx.device);
    if (jpt_vk_create_buffer(&r->ctx.alloc, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &staging, &alloc) != VK_SUCCESS)
        return 0;

    typedef struct {
        VkImage img;
        VkBuffer dst;
        uint32_t w, h;
    } rb_job_t;
    rb_job_t job = { r->scene_resolved, staging, r->rt_w, r->rt_h };

    /* Reuse one-shot helper via a local static-like dispatch */
    {
        const jpt_vk_loader_t* vk = &r->ctx.vk;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo cpci = { 0 };
        VkCommandBufferAllocateInfo cbai = { 0 };
        VkCommandBufferBeginInfo bi = { 0 };
        VkBufferImageCopy region = { 0 };
        VkSubmitInfo si = { 0 };
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci = { 0 };

        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = r->ctx.graphics_family;
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vk->vkCreateCommandPool(r->ctx.device, &cpci, NULL, &pool) == VK_SUCCESS) {
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vk->vkAllocateCommandBuffers(r->ctx.device, &cbai, &cmd) == VK_SUCCESS &&
                vk->vkCreateFence(r->ctx.device, &fci, NULL, &fence) == VK_SUCCESS) {
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vk->vkBeginCommandBuffer(cmd, &bi);
                cmd_image_barrier(vk, cmd, job.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = job.w;
                region.imageExtent.height = job.h;
                region.imageExtent.depth = 1;
                vk->vkCmdCopyImageToBuffer(cmd, job.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, job.dst, 1, &region);
                cmd_image_barrier(vk, cmd, job.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
                vk->vkEndCommandBuffer(cmd);
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                vk->vkQueueSubmit(r->ctx.graphics_queue, 1, &si, fence);
                vk->vkWaitForFences(r->ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vk->vkDestroyFence(r->ctx.device, fence, NULL);
            }
            vk->vkDestroyCommandPool(r->ctx.device, pool, NULL);
        }
    }

    if (alloc.mapped) memcpy(dst_rgba, alloc.mapped, (size_t)needed);
    jpt_vk_destroy_buffer(&r->ctx.alloc, &staging, &alloc);
    return 1;
}

int jpt_vk_renderer_readback_rgb565(jpt_vk_renderer_t* r, uint16_t* dst_565, uint32_t dst_w, uint32_t dst_h) {
    uint32_t sw = 0, sh = 0;
    uint8_t* rgba;
    size_t bytes;

    if (!r || !dst_565 || !dst_w || !dst_h) return 0;
    bytes = (size_t)(r->ctx.headless ? r->sc.extent.width : r->rt_w) *
            (size_t)(r->ctx.headless ? r->sc.extent.height : r->rt_h) * 4u;
    rgba = (uint8_t*)malloc(bytes);
    if (!rgba) return 0;
    if (!jpt_vk_renderer_readback_rgba(r, rgba, &sw, &sh, bytes)) {
        free(rgba);
        return 0;
    }

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)x * sw) / dst_w);
            const uint8_t* p = rgba + ((size_t)sy * sw + sx) * 4u;
            uint32_t r5 = (p[0] * 31u + 127u) / 255u;
            uint32_t g6 = (p[1] * 63u + 127u) / 255u;
            uint32_t b5 = (p[2] * 31u + 127u) / 255u;
            dst_565[(size_t)y * dst_w + x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
    free(rgba);
    return 1;
}

int jpt_vk_renderer_last_frame_valid(const jpt_vk_renderer_t* r) { return r && r->last_valid; }

int jpt_vk_renderer_read_last_frame_rgb565(jpt_vk_renderer_t* r, uint16_t* dst, uint32_t dst_w, uint32_t dst_h,
                                           size_t dst_pitch_pixels) {
    readback_job_t job;
    VkDeviceSize need;
    const uint8_t* mapped;
    double start;

    if (!r || !dst || !dst_w || !dst_h || !r->last_valid || r->last_img == VK_NULL_HANDLE) return 0;
    if (r->rb_img == VK_NULL_HANDLE || r->rb_w != dst_w || r->rb_h != dst_h) {
        destroy_rt_image(r, &r->rb_img, &r->rb_alloc, &r->rb_view);
        if (!create_image_2d(r, dst_w, dst_h, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &r->rb_img, &r->rb_alloc, &r->rb_view))
            return 0;
        r->rb_w = dst_w;
        r->rb_h = dst_h;
    }
    need = (VkDeviceSize)dst_w * (VkDeviceSize)dst_h * 4u;
    if (r->rb_buf == VK_NULL_HANDLE || r->rb_buf_bytes < need) {
        if (r->rb_buf != VK_NULL_HANDLE) jpt_vk_destroy_buffer(&r->ctx.alloc, &r->rb_buf, &r->rb_buf_alloc);
        if (jpt_vk_create_buffer(&r->ctx.alloc, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &r->rb_buf, &r->rb_buf_alloc) != VK_SUCCESS) {
            r->rb_buf = VK_NULL_HANDLE;
            r->rb_buf_bytes = 0;
            return 0;
        }
        r->rb_buf_bytes = need;
    }

    memset(&job, 0, sizeof job);
    job.src = r->last_img;
    job.src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    job.src_restore = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    job.rb = r->rb_img;
    job.buf = r->rb_buf;
    job.rt_w = r->rt_w;
    job.rt_h = r->rt_h;
    job.src_b.x = (int32_t)r->rt_w;
    job.src_b.y = (int32_t)r->rt_h;
    job.src_b.z = 1;
    job.dst_b.x = (int32_t)dst_w;
    job.dst_b.y = (int32_t)dst_h;
    job.dst_b.z = 1;
    job.copy.extent.width = dst_w;
    job.copy.extent.height = dst_h;
    start = now_ms();
    if (!run_one_shot_cmd(r, record_scene_readback, &job, JPT_VK_OS_READBACK)) return 0;
    start = now_ms() - start;
    r->stats.cpu_readback_ms += start;
    if (start > r->stats.cpu_readback_max_ms) r->stats.cpu_readback_max_ms = start;
    r->stats.last_frame_readbacks++;
    if (!r->rb_buf_alloc.mapped) return 0;

    mapped = (const uint8_t*)r->rb_buf_alloc.mapped;
    for (uint32_t y = 0; y < dst_h; y++) {
        const uint8_t* in = mapped + (size_t)y * dst_w * 4u;
        uint16_t* out = dst + (size_t)y * dst_pitch_pixels;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t r5 = ((uint32_t)in[x * 4 + 0] * 31u + 127u) / 255u;
            uint32_t g6 = ((uint32_t)in[x * 4 + 1] * 63u + 127u) / 255u;
            uint32_t b5 = ((uint32_t)in[x * 4 + 2] * 31u + 127u) / 255u;
            out[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
    return 1;
}
