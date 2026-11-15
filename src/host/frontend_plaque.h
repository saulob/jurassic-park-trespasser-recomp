/* The English lettering of the game's menu pictures, found and taken out so that the HD frontend can set a translated
 * label in its place: the buttons' stencilled words, the engraved titles of the Quit and Sound windows, the main
 * screen's legal lines. Works on the player's own pictures in memory; nothing is drawn or kept on disk. */
#ifndef FRONTEND_PLAQUE_H
#define FRONTEND_PLAQUE_H

#include <stdint.h>

#include "frontend_art.h"
#include "frontend_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int w, h;
    uint16_t* clean;   /* RGB565 rows w apart: the picture without its letters, transparent pixels kept */
    fe_rect_t letters; /* the letters' box, picture pixels */
    fe_rect_t room;    /* where a label may go: the plate around the letters, inset from its edges */
    uint32_t ink;      /* 0xRRGGBB of the letters */
    int lines;         /* text lines found */
} fp_plaque_t;

/* A button: its normal picture and its hover picture, which shows the same letters in red. */
int fp_button(const fa_source_t* normal, const fa_source_t* hover, fp_plaque_t* out);
/* Letters darker than their plate inside zone (engraved titles); room is the plate a label may fill. */
int fp_engraved(const fa_source_t* picture, fe_rect_t zone, fe_rect_t room, fp_plaque_t* out);
/* Light, grey letters inside area (the main screen's legal lines). */
int fp_light(const fa_source_t* picture, fe_rect_t area, fp_plaque_t* out);
/* The clean picture in the tones of another state's picture of the same button (pressed, disabled); out holds w*h. */
int fp_tone(const fp_plaque_t* p, const fa_source_t* state, uint16_t* out);
void fp_free(fp_plaque_t* p);

/* The shared step: the pixels under hole filled from the picture's own texture (a shifted copy, blended in the gradient
 * domain). valid marks the pixels that may be used; rgb is 3 floats per pixel, changed in place. */
void fp_fill(float* rgb, const uint8_t* valid, const uint8_t* hole, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
