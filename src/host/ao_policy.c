#include "ao_policy.h"

#include <math.h>
#include <string.h>

#include "jpt_config.h"

/* ASSAO's own defaults (ASSAO_Settings()), with the radius and fade distances in Trespasser's metres. */
static const ao_preset_t k_base = {
    .quality_level = 2,
    .blur_pass_count = 2,
    .radius = 1.0f,
    .shadow_multiplier = 1.0f,
    .shadow_power = 1.5f,
    .shadow_clamp = 0.98f,
    .horizon_angle_threshold = 0.06f,
    .fade_out_from = 40.0f,
    .fade_out_to = 150.0f,
    .adaptive_quality_limit = 0.45f,
    .sharpness = 0.98f,
    .detail_shadow_strength = 0.5f,
};

int ao_preset(int level, ao_preset_t* out) {
    /* ASSAO's Low, Medium, High and Highest (adaptive); its Lowest (-1, half-resolution checkerboard) is not offered. */
    static const int quality[JPT_AO_COUNT] = { 0, 0, 1, 2, 3 };
    if (level <= JPT_AO_OFF || level >= JPT_AO_COUNT) return 0;
    *out = k_base;
    out->quality_level = quality[level];
    return 1;
}

void ao_projection(float tan_half_x, float tan_half_y, float m[16]) {
    memset(m, 0, 16 * sizeof m[0]);
    m[0] = 1.0f / tan_half_x;
    m[5] = 1.0f / tan_half_y;
    m[11] = 1.0f;
    m[14] = AO_DEPTH_SCALE;
}

float ao_decoded_depth(const float m[16], float device_depth) {
    float mul = -m[14], add = m[10];
    if (mul * add < 0.0f) add = -add;
    return mul / (add - device_depth);
}

int ao_view_tangents(float tan_half_h, unsigned target_w, unsigned target_h, float* tan_half_x, float* tan_half_y) {
    if (!(tan_half_h > 0.0f) || !isfinite(tan_half_h) || !target_w || !target_h) return 0;
    *tan_half_y = tan_half_h;
    *tan_half_x = tan_half_h * (float)target_w / (float)target_h;
    return 1;
}

int ao_frame_wanted(const ao_frame_t* f) {
    return f->level > JPT_AO_OFF && f->level < JPT_AO_COUNT && f->ready && f->masked && f->scene && f->gameplay &&
           f->tan_half_h > 0.0f;
}
