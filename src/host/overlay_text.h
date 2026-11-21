/* The game's text overlay (CTextOverlay) read from guest memory: the laid-out text blobs of its entries and which kind of text
 * is on screen. Reads only; both hosts use it. */
#ifndef JPT_OVERLAY_TEXT_H
#define JPT_OVERLAY_TEXT_H

#include <stdint.h>

#define OVERLAY_BLOB_MAX 0x2000u
#define OVERLAY_CARD_CATEGORY 0x4A50u
#define OVERLAY_HINT_CATEGORY 0x4A51u

typedef struct {
    uint32_t text; /* guest address of the line's characters */
    int x, y, count;
} overlay_line_t;

/* The lines of a laid-out text {size, lines, {x, y, count, chars, 0}...}: how many, or -1 when it is not well formed. */
int overlay_blob_lines(uint32_t blob, overlay_line_t* lines, int max);
int overlay_blob_readable(uint32_t blob);
/* The controls card is up (card), and a gameplay or host prompt sits in the HUD's part of the picture (game_text; voice
 * subtitles at the bottom do not count). */
void overlay_text_state(int* card, int* game_text);

#endif
