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
    JPT_TOKEN_CONTROLS,  /* the port's controls card: F2, or the gamepad's View/Back button */
    JPT_TOKEN_HINT,      /* the game's Hint: F1, or the D-pad shortcut */
    JPT_TOKEN_OBJECTIVES, /* the port's Objectives: F3, or the D-pad shortcut */
    JPT_TOKEN_SHOW_OBJECTIVE, /* the port's Show Objective: F4, or the D-pad shortcut */
    JPT_TOKEN_KEY,       /* a fixed key of the port (F5, F10, F11) */
    JPT_TOKEN_PAD        /* a fixed gamepad control of the port (L3, R3) */
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

/* The controls card, laid out by the host rather than the game's word wrap. */
typedef struct {
    const char* binding; /* <TOKEN> text */
    const char* action;
} jpt_card_row_t;

typedef struct {
    const char* title;           /* <TOKEN> text; "" for none */
    const jpt_card_row_t* rows;
    int count;                   /* rows in all */
    int first_column;            /* rows in the first column */
} jpt_card_t;

const jpt_card_t* jpt_prompt_card(int gamepad);
const jpt_card_t* jpt_prompt_hint_card(int gamepad);

enum { JPT_PANEL_NONE, JPT_PANEL_CARD, JPT_PANEL_HINT };
int jpt_prompt_panel_kind(const char* text);

#define JPT_CARD_MAX_ROWS 24

typedef struct {
    int count, first_column;
    int title_w;
    int binding_w[JPT_CARD_MAX_ROWS];
    int action_w[JPT_CARD_MAX_ROWS];
} jpt_card_metrics_t;

typedef struct {
    int left, top, right, bottom;
} jpt_box_t;

typedef struct {
    int text_h, line_h, columns;
    jpt_box_t box;
    int title_x, title_y;
    int binding_x[JPT_CARD_MAX_ROWS];
    int action_x[JPT_CARD_MAX_ROWS];
    int row_y[JPT_CARD_MAX_ROWS];
} jpt_card_layout_t;

/* The card stays below band (the game's top text) and clear of two subtitle lines; its text never exceeds the
 * game's own. */
#define JPT_CARD_MARGIN 8
#define JPT_CARD_SIDE_PERCENT 4
#define JPT_CARD_TEXT_PERMILLE 46
#define JPT_CARD_MIN_TEXT 18
#define JPT_CARD_FLOOR_TEXT 9
void jpt_card_area(int left, int top, int w, int h, int game_text_h, int band, jpt_box_t* area, int* max_h);
int jpt_card_band_floor(int game_text_h);

int jpt_card_line_h(int text_h);
void jpt_card_size(const jpt_card_metrics_t* m, int text_h, int columns, int* w, int* h);
/* 1 when the card fits inside area. */
int jpt_card_place(const jpt_card_metrics_t* m, int text_h, int columns, jpt_box_t area, jpt_card_layout_t* out);
int jpt_card_choose(const jpt_card_metrics_t* ref, int ref_h, int max_h, jpt_box_t area, int* columns);
/* 1 fits, 0 placed at min_h without fitting, -1 measure failed. The last measure call is at out->text_h. */
typedef int (*jpt_card_measure_fn)(void* ctx, int text_h, jpt_card_metrics_t* m);
int jpt_card_fit(jpt_card_measure_fn measure, void* ctx, int max_h, int min_h, jpt_box_t area, jpt_card_metrics_t* m,
                 jpt_card_layout_t* out);

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
