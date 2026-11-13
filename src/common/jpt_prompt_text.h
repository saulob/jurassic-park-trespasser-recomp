/* Prompts inside the game's own text: the beach tutorial's subtitles (Stream.tpa) and the port's controls card, turned
 * into text plus button/key tokens for the device in use. Every token is an action resolved through the current
 * binding, so remapped keys and gamepad buttons show up as they are. Shared by the host, the launcher and the tests. */
#ifndef JPT_PROMPT_TEXT_H
#define JPT_PROMPT_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where a token goes in rendered text; the game's text never contains it. */
#define JPT_PROMPT_MARK '\x7F'
#define JPT_PROMPT_MAX_TOKENS 32

typedef enum {
    JPT_TOKEN_COMMAND,   /* one of the game's commands (or a gamepad action when action >= 0) */
    JPT_TOKEN_LOOK,      /* the mouse, or the right stick */
    JPT_TOKEN_MOVE,      /* W/A/S/D, or the left stick */
    JPT_TOKEN_PAUSE,     /* Esc, or Start */
    JPT_TOKEN_CONTROLS   /* the port's controls card: the gamepad's View/Back button */
} jpt_token_kind_t;

typedef struct {
    jpt_token_kind_t kind;
    jpt_prompt_t prompt;   /* the binding: a key (gamepad 0) or a gamepad control */
    int mouse_move;        /* keyboard and mouse: draw a mouse without a lit button */
    char label[24];        /* the binding as text ("Q", "Mouse Left", "A", "Cross", "Right stick") */
} jpt_prompt_token_t;

typedef struct {
    int gamepad;                              /* prompts for the gamepad (else keyboard and mouse) */
    jpt_pad_family_t family;                  /* whose button names */
    const jpt_gamepad_settings_t* settings;   /* gamepad bindings */
    const jpt_key_binding_t* keymap;          /* the game's key map */
    int keymap_count;
} jpt_prompt_context_t;

/* 1 when the text names keys, mouse buttons or <TOKENS> that prompts replace. */
int jpt_prompt_text_has_prompts(const char* text);

/* The text for the context with JPT_PROMPT_MARK where each token goes, in order. Returns the token count, -1 when the
 * text has nothing to replace or out is too small. */
int jpt_prompt_text_render(const char* text, const jpt_prompt_context_t* c, char* out, size_t cap,
                           jpt_prompt_token_t* tokens, int max_tokens);

/* The same with each token written as [label]: logs and tests. */
int jpt_prompt_text_plain(const char* text, const jpt_prompt_context_t* c, char* out, size_t cap);

/* The in-game controls card for a device, as <TOKEN> text with '\r' line breaks, and the one-line hint that names the
 * button opening it. */
const char* jpt_prompt_controls_card(int gamepad);
const char* jpt_prompt_controls_hint(void);

/* A row of the controls reference: what to press and what it does. */
typedef struct {
    char binding[64];
    const char* action;
} jpt_guide_row_t;

#define JPT_GUIDE_MAX_ROWS 24
/* The reference shown by the launcher, from the live bindings. Returns the row count. */
int jpt_controls_guide(const jpt_prompt_context_t* c, jpt_guide_row_t* rows, int max_rows);

#ifdef __cplusplus
}
#endif

#endif
