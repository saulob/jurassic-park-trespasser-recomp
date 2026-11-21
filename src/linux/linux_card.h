/* The port's controls card (F2, the gamepad's View button) and its one-line hint on Linux: the card's content, bindings and
 * translations are the Windows host's (jpt_prompt_card, jpt_prompt_text_render, the layout of jpt_card_*), drawn over the guest's
 * picture by the host's overlay stage with the same vector glyphs (pad_glyphs.c). The guest's own text is never touched. */
#ifndef JPT_LINUX_CARD_H
#define JPT_LINUX_CARD_H

#include <stdint.h>

void lx_card_reset(void);
void lx_card_show(void);
void lx_card_hide(void);
int lx_card_shown(void);
/* 1 when the hint is up or the card is (nothing to add), 0 when it could not be shown. */
int lx_card_hint(void);
int lx_card_hint_shown(void);

/* What the last draw laid out, for tests. */
typedef struct {
    int text_h, columns, rows, fits;
    int box_left, box_top, box_right, box_bottom;
    int glyphs;
} lx_card_stats_t;
void lx_card_stats(lx_card_stats_t* out);

/* Draws the card or the hint into XRGB8888 rows (stride pixels apart) of a frame of fw x fh pixels, inside the guest picture's
 * rectangle; 1 when something was drawn. */
int lx_card_draw(uint32_t* pixels, int stride, int fw, int fh, int pic_x, int pic_y, int pic_w, int pic_h);

#endif
