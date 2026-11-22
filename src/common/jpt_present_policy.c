#include "jpt_present_policy.h"

#include <string.h>

#include "jpt_display.h"

static void push_stage(jpt_present_plan_t* out, jpt_present_stage_t stage, int apply_gamma, jpt_present_blend_t blend,
                       jpt_present_filter_t filter) {
    if (out->stage_count >= JPT_STAGE_COUNT) return;
    jpt_present_stage_cmd_t* cmd = &out->stages[out->stage_count++];
    cmd->stage = stage;
    cmd->apply_gamma = apply_gamma ? 1 : 0;
    cmd->blend = blend;
    cmd->filter = filter;
}

void jpt_present_plan_build(const jpt_present_input_t* in, jpt_present_plan_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!in) return;

    out->end_text_frame = 1;
    if (!in->frames_allowed || in->ui_canvas_held || in->loading_hd_held) {
        out->held = 1;
        return;
    }

    push_stage(out, JPT_STAGE_BASE, 1, JPT_PRESENT_BLEND_OPAQUE,
               in->base_is_3d ? JPT_PRESENT_FILTER_POINT : JPT_PRESENT_FILTER_LINEAR);

    if (in->base_is_3d && in->ao_wanted && !in->has_frontend_hd && !in->has_ui_layer) {
        out->draw_ao = 1;
        push_stage(out, JPT_STAGE_AO, 0, JPT_PRESENT_BLEND_OPAQUE, JPT_PRESENT_FILTER_LINEAR);
    }

    if (in->has_frontend_hd && in->fe_w > 0 && in->fe_h > 0) {
        jpt_present_filter_t fe_filter =
            (in->out_w == in->fe_w && in->out_h == in->fe_h) ? JPT_PRESENT_FILTER_POINT : JPT_PRESENT_FILTER_LINEAR;
        out->draw_frontend_hd = 1;
        push_stage(out, JPT_STAGE_FRONTEND_HD, 1, JPT_PRESENT_BLEND_PREMUL, fe_filter);
        if (in->has_pointer) {
            out->draw_pointer = 1;
            push_stage(out, JPT_STAGE_CURSOR, 1, JPT_PRESENT_BLEND_PREMUL, JPT_PRESENT_FILTER_POINT);
        }
    } else if (in->has_ui_layer) {
        out->draw_ui_layer = 1;
        push_stage(out, JPT_STAGE_UI_LAYER, 1, JPT_PRESENT_BLEND_PREMUL, JPT_PRESENT_FILTER_LINEAR);
    }

    if (in->has_subtitle) {
        out->draw_subtitle = 1;
        push_stage(out, JPT_STAGE_VIDEO_SUBTITLES, 0, JPT_PRESENT_BLEND_PREMUL, JPT_PRESENT_FILTER_POINT);
    }

    if (in->has_host_overlay) {
        out->draw_host_overlay = 1;
        out->draw_text_layer_in_hud = (in->has_hud && in->has_text_layer) ? 1 : 0;
        push_stage(out, JPT_STAGE_HOST_OVERLAYS, 0, JPT_PRESENT_BLEND_STRAIGHT, JPT_PRESENT_FILTER_POINT);
    }
}

void jpt_choose_render_size(unsigned box_w, unsigned box_h, int view_w, int view_h, unsigned legacy_w, unsigned legacy_h,
                            unsigned* out_w, unsigned* out_h) {
    static const unsigned heights[] = { 480, 600, 768, 960, 1200, 1440, 1920 };
    unsigned vw, vh, need, i, w, h;

    if (!legacy_w) legacy_w = 640u;
    if (!legacy_h) legacy_h = 480u;

    if (box_w && box_h) {
        jpt_size_t box = { (int)box_w, (int)box_h };
        jpt_size_t size = jpt_render_target(box, (int)legacy_w, (int)legacy_h);
        if (out_w) *out_w = (unsigned)(size.width > 0 ? size.width : (int)legacy_w);
        if (out_h) *out_h = (unsigned)(size.height > 0 ? size.height : (int)legacy_h);
        return;
    }

    vw = view_w > 0 ? (unsigned)view_w : legacy_w;
    vh = view_h > 0 ? (unsigned)view_h : legacy_h;
    need = vh;
    if (vw * 3u < vh * 4u) need = (vw * 3u + 3u) / 4u;
    if (need < legacy_h) need = legacy_h;
    for (i = 0; i + 1 < sizeof heights / sizeof heights[0] && heights[i] < need; i++) {}
    h = heights[i];
    w = h * 4u / 3u;
    if (legacy_w * 3u != legacy_h * 4u && legacy_h) w = (unsigned)(((uint64_t)h * legacy_w) / legacy_h);
    if (out_w) *out_w = w ? w : legacy_w;
    if (out_h) *out_h = h ? h : legacy_h;
}

void jpt_bgra_to_rgba_premul(const uint32_t* src, uint8_t* dst, size_t count) {
    if (!src || !dst) return;
    for (size_t i = 0; i < count; i++) {
        uint32_t p = src[i];
        dst[i * 4 + 0] = (uint8_t)((p >> 16) & 0xFFu);
        dst[i * 4 + 1] = (uint8_t)((p >> 8) & 0xFFu);
        dst[i * 4 + 2] = (uint8_t)(p & 0xFFu);
        dst[i * 4 + 3] = (uint8_t)((p >> 24) & 0xFFu);
    }
}

uint32_t jpt_apply_gamma_rgba(uint32_t rgba, const uint16_t red[256], const uint16_t green[256],
                              const uint16_t blue[256]) {
    uint32_t r = rgba & 0xFFu;
    uint32_t g = (rgba >> 8) & 0xFFu;
    uint32_t b = (rgba >> 16) & 0xFFu;
    uint32_t a = (rgba >> 24) & 0xFFu;
    if (red) r = (uint32_t)(red[r] >> 8);
    if (green) g = (uint32_t)(green[g] >> 8);
    if (blue) b = (uint32_t)(blue[b] >> 8);
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t jpt_blend_premul_over_rgba(uint32_t dst_rgba, uint32_t src_rgba) {
    uint32_t sr = src_rgba & 0xFFu;
    uint32_t sg = (src_rgba >> 8) & 0xFFu;
    uint32_t sb = (src_rgba >> 16) & 0xFFu;
    uint32_t sa = (src_rgba >> 24) & 0xFFu;
    uint32_t inv_a = 255u - sa;
    uint32_t dr = dst_rgba & 0xFFu;
    uint32_t dg = (dst_rgba >> 8) & 0xFFu;
    uint32_t db = (dst_rgba >> 16) & 0xFFu;
    uint32_t or_ = sr + (dr * inv_a + 127u) / 255u;
    uint32_t og = sg + (dg * inv_a + 127u) / 255u;
    uint32_t ob = sb + (db * inv_a + 127u) / 255u;
    if (or_ > 255u) or_ = 255u;
    if (og > 255u) og = 255u;
    if (ob > 255u) ob = 255u;
    return or_ | (og << 8) | (ob << 16) | (0xFFu << 24);
}
