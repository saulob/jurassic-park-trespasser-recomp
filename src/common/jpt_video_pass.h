#ifndef JPT_VIDEO_PASS_H
#define JPT_VIDEO_PASS_H

#include <stdint.h>

#define JPT_VIDEO_GLOW_STEPS 32
#define JPT_VIDEO_GLOW_ROWS 96
#define JPT_VIDEO_MAX_SIDE 4096
#define JPT_VIDEO_SMOOTH_FULL (16.0f / 255.0f)
#define JPT_VIDEO_SMOOTH_NONE (28.0f / 255.0f)
#define JPT_VIDEO_DEBLOCK_STEP_FULL (12.0f / 255.0f)
#define JPT_VIDEO_DEBLOCK_STEP_NONE (32.0f / 255.0f)
#define JPT_VIDEO_DEBLOCK_FLAT_FULL (4.0f / 255.0f)
#define JPT_VIDEO_DEBLOCK_FLAT_NONE (10.0f / 255.0f)
#define JPT_VIDEO_GLOW_NEAR 0.5f
#define JPT_VIDEO_GLOW_FAR 0.12f

/* The constants of every video pass, as the shaders of both backends read them. */
typedef struct {
    int32_t src[4], opt[4];
    float video[4], outp[4], tune[4], deblock[4];
    int32_t stray[4];
} jpt_video_constants_t;

typedef struct {
    int width, stored_rows, shown_rows, rebuild, parity;
    int has_palette_map, rgb565, gamma_on;
    int video_x, video_y, video_w, video_h;
    int out_w, out_h, side, glow, progressive, stray_line;
} jpt_video_params_t;

static inline void jpt_video_constants(const jpt_video_params_t* p, jpt_video_constants_t* c) {
    c->src[0] = p->width;
    c->src[1] = p->stored_rows;
    c->src[2] = p->shown_rows;
    c->src[3] = p->rebuild;
    c->opt[0] = p->parity;
    c->opt[1] = p->has_palette_map;
    c->opt[2] = p->rgb565;
    c->opt[3] = p->gamma_on;
    c->video[0] = (float)p->video_x;
    c->video[1] = (float)p->video_y;
    c->video[2] = (float)p->video_w;
    c->video[3] = (float)p->video_h;
    c->outp[0] = (float)p->out_w;
    c->outp[1] = (float)p->out_h;
    c->outp[2] = (float)p->side;
    c->outp[3] = p->glow ? 1.0f : 0.0f;
    c->tune[0] = JPT_VIDEO_SMOOTH_FULL;
    c->tune[1] = JPT_VIDEO_GLOW_NEAR;
    c->tune[2] = JPT_VIDEO_GLOW_FAR;
    c->tune[3] = JPT_VIDEO_SMOOTH_NONE;
    c->deblock[0] = JPT_VIDEO_DEBLOCK_STEP_FULL;
    c->deblock[1] = JPT_VIDEO_DEBLOCK_STEP_NONE;
    c->deblock[2] = JPT_VIDEO_DEBLOCK_FLAT_FULL;
    c->deblock[3] = JPT_VIDEO_DEBLOCK_FLAT_NONE;
    c->stray[0] = p->progressive && p->stray_line;
    c->stray[1] = c->stray[2] = c->stray[3] = 0;
}

#endif
