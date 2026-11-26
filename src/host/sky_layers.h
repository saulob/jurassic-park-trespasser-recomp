/* The custom sky of the level a custom session plays, in place of the engine's plane. */
#ifndef SKY_LAYERS_H
#define SKY_LAYERS_H

#include <stdint.h>

#include "jpt_picture.h"
#include "jpt_sky.h"

#define SKY_LAYERS_FVF 0x1C4u

typedef struct {
    const jpt_sky_pass_t* pass;
    const jpt_picture_t* picture; /* NULL: without a texture */
    void** gpu;                   /* what the renderer keeps of the picture, NULL until it does */
    const jpt_sky_screen_vertex_t* vertices;
    int count;
} sky_draw_t;

typedef void (*sky_emit_fn)(void* user, const sky_draw_t* draw);

/* free_gpu: of a draw's gpu. */
void sky_layers_install(void (*free_gpu)(void* gpu));
/* Whether the level has layers to draw. */
int sky_layers_ready(void);
/* The engine's own sky (CSkyRender) as it is drawn: its plane is moved so that the engine draws it whatever is in view. */
void sky_layers_begin(uint32_t sky);
/* rect: where the view is on the screen, left, top, right and bottom; from the engine's own vertices. */
void sky_layers_emit(const float rect[4], sky_emit_fn emit, void* user);
/* The view's rectangle from the engine's vertices; 0 when they are not its corners. */
int sky_layers_rect(const void* vertices, unsigned count, float rect[4]);
void sky_layers_end(uint32_t sky);

#endif
