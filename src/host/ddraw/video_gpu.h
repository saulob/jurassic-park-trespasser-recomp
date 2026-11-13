#ifndef TRESPASSER_VIDEO_GPU_H
#define TRESPASSER_VIDEO_GPU_H

#include <stdint.h>

#include "video_layout.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

typedef struct video_gpu video_gpu_t;

/* One decoded frame as the game's screen holds it. */
typedef struct {
    const uint8_t* rows; /* the first row of picture */
    long stride;         /* bytes from one row of picture to the next */
    int width;
    vl_policy_t policy;
    int rgb565;                  /* else XRGB1555 */
    const uint32_t* palette_map; /* vl_palette_map_t.map, or NULL */
    int palette_changed;
    const uint16_t* gamma; /* DDGAMMARAMP (red, green, blue), or NULL */
    int smooth;            /* light smoothing of the palette's dither */
    int deblock;           /* the codec's 4x4 block edges softened in smooth areas (progressive files) */
    int stray_bottom_line; /* the last row's lit pixels over a black block rebuilt (progressive files) */
} video_gpu_frame_t;

/* Compiles the passes on a thread of their own, ahead of the first video. */
void video_gpu_prepare(void);
int video_gpu_create(struct ID3D11Device* device, struct ID3D11DeviceContext* context, video_gpu_t** out);
const char* video_gpu_error(void);
/* The frame laid out on an out_w x out_h target; 0 on a failure (video_gpu_error says which). */
int video_gpu_draw(video_gpu_t* g, const video_gpu_frame_t* f, const vl_layout_t* layout, int out_w, int out_h,
                   struct ID3D11RenderTargetView* target);
void video_gpu_draw_black(video_gpu_t* g, struct ID3D11RenderTargetView* target);
void video_gpu_destroy(video_gpu_t* g);
/* D3D11 objects the module holds, every instance together. */
long video_gpu_live_objects(void);

#endif
