/* The HD front end's text without GDI: the embedded TrueType face measured and rasterised with stb_truetype, text in the display
 * code page. Used where frontend_assets.c has no Windows GDI (the Linux host). */
#ifndef JPT_FRONTEND_TEXT_STB_H
#define JPT_FRONTEND_TEXT_STB_H

#include <stdint.h>

/* The text's advance width and the face's line height at px (em height), as GDI's extent and tmHeight would be. 1 when measured. */
int fe_stb_measure(int px, int weight, int spacing, const char* text, int len, int* advance, int* line_h);
/* An 8-bit coverage cell of (advance + 2 pad) x (line height + 2 pad) with the text's origin at (pad, pad); free it. NULL on failure. */
uint8_t* fe_stb_render(int px, int weight, int spacing, const char* text, int len, int pad, int* w, int* h);

#endif
