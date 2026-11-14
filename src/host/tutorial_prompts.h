/* Glyphs in the game's text overlay (tutorial_prompts.c). */
#ifndef TUTORIAL_PROMPTS_H
#define TUTORIAL_PROMPTS_H

#include "host.h"

/* From the overlay's text draw (0x0050EAF0, font in ecx, laid-out text at [esp + 4]): 1 when the text had prompts and
 * was drawn through lifted with them, 0 when the caller draws it unchanged. */
int tutorial_prompts_draw(recomp_func_t lifted);
/* From TextOutA while such a text is drawn: 1 when this line was drawn here. */
int tutorial_prompts_textout(HDC dc, int x, int y, const char* text, int count);
/* The controls card in the game's text overlay. Guest thread, gameplay only. */
void tutorial_prompts_show_card(void);
void tutorial_prompts_hide_card(void);
int tutorial_prompts_card_shown(void);
/* The one-line hint naming the controls card's button; 1 when it was shown. */
int tutorial_prompts_hint(void);
/* Memory reads only: the controls card is up; a gameplay or host prompt sits in the HUD's part of the picture
 * (compass_policy.h's cp_text_kind: voice subtitles at the bottom do not count). */
void tutorial_prompts_overlay_state(int* card, int* game_text);

#endif
