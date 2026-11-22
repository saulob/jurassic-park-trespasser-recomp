#ifndef JPT_PRESENT_POLICY_H
#define JPT_PRESENT_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPT_PRESENT_BLEND_OPAQUE = 0,
    JPT_PRESENT_BLEND_STRAIGHT = 1,
    JPT_PRESENT_BLEND_PREMUL = 2
} jpt_present_blend_t;

typedef enum {
    JPT_PRESENT_FILTER_POINT = 0,
    JPT_PRESENT_FILTER_LINEAR = 1
} jpt_present_filter_t;

typedef enum {
    JPT_STAGE_BASE = 0,
    JPT_STAGE_AO = 1,
    JPT_STAGE_FRONTEND_HD = 2,
    JPT_STAGE_CURSOR = 3,
    JPT_STAGE_UI_LAYER = 4,
    JPT_STAGE_VIDEO_SUBTITLES = 5,
    JPT_STAGE_HOST_OVERLAYS = 6,
    JPT_STAGE_COUNT = 7
} jpt_present_stage_t;

typedef struct {
    jpt_present_stage_t stage;
    int apply_gamma;
    jpt_present_blend_t blend;
    jpt_present_filter_t filter;
} jpt_present_stage_cmd_t;

typedef struct {
    int frames_allowed;
    int ui_canvas_held;
    int loading_hd_held;
    int base_is_3d;
    int ao_wanted;
    int has_frontend_hd;
    int fe_w, fe_h;
    int has_pointer;
    int has_ui_layer;
    int has_subtitle;
    int has_host_overlay;
    int has_hud;
    int has_text_layer;
    int out_w, out_h;
} jpt_present_input_t;

typedef struct {
    int held;
    int draw_ao;
    int draw_frontend_hd;
    int draw_pointer;
    int draw_ui_layer;
    int draw_subtitle;
    int draw_host_overlay;
    int draw_text_layer_in_hud;
    int end_text_frame;
    int stage_count;
    jpt_present_stage_cmd_t stages[JPT_STAGE_COUNT];
} jpt_present_plan_t;

void jpt_present_plan_build(const jpt_present_input_t* in, jpt_present_plan_t* out);

/* Matches D3D11 choose_size: jpt_render_target inside a fixed box, else smallest 4:3/aspect height covering view. */
void jpt_choose_render_size(unsigned box_w, unsigned box_h, int view_w, int view_h, unsigned legacy_w, unsigned legacy_h,
                            unsigned* out_w, unsigned* out_h);

/* Lossless BGRA8888 -> RGBA8888 swizzle preserving premultiplied RGB and alpha. */
void jpt_bgra_to_rgba_premul(const uint32_t* src, uint8_t* dst, size_t count);

/* Reference gamma LUT lookup matching D3D11 ps_present and Vulkan present.frag. */
uint32_t jpt_apply_gamma_rgba(uint32_t rgba, const uint16_t red[256], const uint16_t green[256],
                              const uint16_t blue[256]);

/* Reference premultiplied-over blend matching D3D11 R.blend_premultiplied and Vulkan present_premul_pipe. */
uint32_t jpt_blend_premul_over_rgba(uint32_t dst_rgba, uint32_t src_rgba);

#ifdef __cplusplus
}
#endif

#endif
