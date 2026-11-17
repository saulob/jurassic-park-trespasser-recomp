#include "jpt_prompt_text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char* name;
    jpt_token_kind_t kind;
    uint32_t command;
    int action;
} token_def_t;

static const token_def_t k_tokens[] = {
    { "JUMP", JPT_TOKEN_COMMAND, JPT_CMD_JUMP, JPT_ACT_JUMP },
    { "CROUCH_TOGGLE", JPT_TOKEN_COMMAND, JPT_CMD_CROUCH, JPT_ACT_CROUCH_TOGGLE },
    { "CROUCH", JPT_TOKEN_COMMAND, JPT_CMD_CROUCH, JPT_ACT_CROUCH },
    { "USE", JPT_TOKEN_COMMAND, JPT_CMD_USE, JPT_ACT_USE },
    { "HAND", JPT_TOKEN_COMMAND, JPT_CMD_HAND, JPT_ACT_HAND },
    { "GRAB", JPT_TOKEN_COMMAND, JPT_CMD_GRAB, JPT_ACT_GRAB },
    { "THROW", JPT_TOKEN_COMMAND, JPT_CMD_THROW, JPT_ACT_THROW },
    { "STOW", JPT_TOKEN_COMMAND, JPT_CMD_STOW, JPT_ACT_STOW },
    { "WRIST", JPT_TOKEN_COMMAND, JPT_CMD_WRIST, JPT_ACT_WRIST },
    { "ARM", JPT_TOKEN_COMMAND, JPT_CMD_ARM, JPT_ACT_ARM },
    { "REPLAY", JPT_TOKEN_COMMAND, JPT_CMD_REPLAY, -1 },
    { "RUN", JPT_TOKEN_COMMAND, JPT_CMD_RUN, -1 },
    { "WALK", JPT_TOKEN_COMMAND, JPT_CMD_WALK, -1 },
    { "BACKUP", JPT_TOKEN_COMMAND, JPT_CMD_BACKUP, -1 },
    { "LEFT", JPT_TOKEN_COMMAND, JPT_CMD_LEFT, -1 },
    { "RIGHT", JPT_TOKEN_COMMAND, JPT_CMD_RIGHT, -1 },
    { "LOOK", JPT_TOKEN_LOOK, 0, -1 },
    { "MOVE", JPT_TOKEN_MOVE, 0, -1 },
    { "PAUSE", JPT_TOKEN_PAUSE, 0, -1 },
    { "CONTROLS", JPT_TOKEN_CONTROLS, 0, -1 },
    { "HINT", JPT_TOKEN_HINT, 0, -1 },
    { "OBJECTIVES", JPT_TOKEN_OBJECTIVES, 0, -1 },
    { "SHOW_OBJECTIVE", JPT_TOKEN_SHOW_OBJECTIVE, 0, -1 },
    { "KEY_F5", JPT_TOKEN_KEY, 0x74, -1 },
    { "KEY_F10", JPT_TOKEN_KEY, 0x79, -1 },
    { "KEY_F11", JPT_TOKEN_KEY, 0x7A, -1 },
    { "PAD_L3", JPT_TOKEN_PAD, JPT_PAD_LEFT_STICK, -1 },
    { "PAD_R3", JPT_TOKEN_PAD, JPT_PAD_RIGHT_STICK, -1 },
};

/* The tutorial's subtitles (Stream.tpa) and BE's TrigTutorHint text name keys in a few fixed phrases; NULL keeps the
 * phrase for that device. */
typedef struct {
    const char* phrase;
    const char* keyboard;
    const char* gamepad;
} phrase_rule_t;

static const phrase_rule_t k_rules[] = {
    { "press F1", NULL, "press <HINT>" },
    { "Press the 'S' key to walk slowly", "Press <WALK> to walk slowly", "Push <WALK> gently to walk slowly" },
    { "Press 'W' to run", "Press <RUN> to run", "Push <RUN> all the way to run" },
    { "To back up, press 'X'", "To back up, press <BACKUP>", "To back up, pull <BACKUP> back" },
    { "Press 'A' and 'D' to step left and right", "Press <LEFT> and <RIGHT> to step left and right",
      "Push <LEFT> or <RIGHT> to step left and right" },
    { "press 'Z' to crouch", "press <CROUCH> to crouch", "hold <CROUCH> to crouch" },
    { "press and hold 'W'", "press and hold <RUN>", "push <RUN> forward" },
    { "hold shift + left mouse button", "hold <WRIST> + <HAND>", "hold <HAND> + <WRIST>" },
    { "hold Shift + left mouse button", "hold <WRIST> + <HAND>", "hold <HAND> + <WRIST>" },
    { "hold ctrl + left mouse button", "hold <ARM> + <HAND>", "hold <HAND> + <ARM>" },
    { "hold Ctrl + left mouse button", "hold <ARM> + <HAND>", "hold <HAND> + <ARM>" },
    { "shift + left mouse button", "<WRIST> + <HAND>", "<HAND> + <WRIST>" },
    { "Shift + left mouse button", "<WRIST> + <HAND>", "<HAND> + <WRIST>" },
    { "ctrl + left mouse button", "<ARM> + <HAND>", "<HAND> + <ARM>" },
    { "Ctrl + left mouse button", "<ARM> + <HAND>", "<HAND> + <ARM>" },
    { "'Q'", "<JUMP>", "<JUMP>" },
    { "'Z'", "<CROUCH>", "<CROUCH>" },
    { "'W'", "<RUN>", "<RUN>" },
    { "'S'", "<WALK>", "<WALK>" },
    { "'X'", "<BACKUP>", "<BACKUP>" },
    { "'A'", "<LEFT>", "<LEFT>" },
    { "'D'", "<RIGHT>", "<RIGHT>" },
    { "'E'", "<STOW>", "<STOW>" },
    { "'F'", "<THROW>", "<THROW>" },
    { "'R'", "<REPLAY>", NULL },
    { "by moving the mouse", NULL, "with <LOOK>" },
    { "by moving your mouse", NULL, "with <LOOK>" },
    { "moving the mouse", NULL, "moving <LOOK>" },
    { "moving your mouse", NULL, "moving <LOOK>" },
    { "move the mouse", NULL, "move <LOOK>" },
    { "move your mouse", NULL, "move <LOOK>" },
    { "Move the mouse", NULL, "Move <LOOK>" },
    { "Move your mouse", NULL, "Move <LOOK>" },
    { "the left mouse button", "<HAND>", "<HAND>" },
    { "the right mouse button", "<GRAB>", "<GRAB>" },
    { "left mouse button", "<HAND>", "<HAND>" },
    { "right mouse button", "<GRAB>", "<GRAB>" },
    { "the space bar", "<USE>", "<USE>" },
    { "space bar", "<USE>", "<USE>" },
};

#define CARD_MARKUP "<CARD>"
#define HINT_MARKUP "<CONTROLS> View Controls"
#define GAMEPAD_CARD_TITLE "<CONTROLS> Controls"

static const jpt_card_row_t k_rows_gamepad[] = {
    { "<MOVE>", "Move" },
    { "<LOOK>", "Look" },
    { "<JUMP>", "Jump" },
    { "<CROUCH>", "Crouch (hold)" },
    { "<CROUCH_TOGGLE>", "Crouch on/off" },
    { "<PAUSE>", "Menu" },
    { "<HAND> + <LOOK>", "Move hand" },
    { "<GRAB>", "Grab / drop" },
    { "<USE>", "Use / fire" },
    { "<THROW>", "Throw" },
    { "<HAND> + <WRIST> + <LOOK>", "Rotate wrist" },
    { "<HAND> + <ARM> + <LOOK>", "Rotate arm" },
    { "<STOW>", "Stow / retrieve" },
    { "<CONTROLS>", "Controls" },
    { "<HINT>", "Hint" },
    { "<OBJECTIVES>", "Objectives" },
    { "<SHOW_OBJECTIVE>", "Show Objective" },
    { "<PAD_L3> + <PAD_R3>", "Toggle Minimap" },
};

static const jpt_card_row_t k_rows_keyboard[] = {
    { "<RUN>", "Run" },
    { "<WALK>", "Walk" },
    { "<BACKUP>", "Back up" },
    { "<LEFT> <RIGHT>", "Step left / right" },
    { "<JUMP>", "Jump" },
    { "<CROUCH>", "Crouch (hold)" },
    { "<LOOK>", "Look" },
    { "<REPLAY>", "Replay voice" },
    { "<HAND> + <LOOK>", "Raise and move the hand" },
    { "<HAND> + <GRAB>", "Grab" },
    { "<GRAB>", "Drop" },
    { "<USE>", "Use / fire" },
    { "<THROW>", "Throw" },
    { "<WRIST> + <HAND> + <LOOK>", "Rotate wrist" },
    { "<ARM> + <HAND> + <LOOK>", "Rotate arm" },
    { "<STOW>", "Stow / retrieve" },
    { "<HINT>", "Hint" },
    { "<CONTROLS>", "Controls" },
    { "<OBJECTIVES>", "Objectives" },
    { "<SHOW_OBJECTIVE>", "Show Objective" },
    { "<KEY_F5>", "Toggle Minimap" },
    { "<KEY_F10>", "Cheats" },
    { "<KEY_F11>", "Fullscreen" },
    { "<PAUSE>", "Menu" },
};

static const jpt_card_t k_cards[2] = {
    { "Controls", k_rows_keyboard, (int)(sizeof k_rows_keyboard / sizeof k_rows_keyboard[0]), 12 },
    { GAMEPAD_CARD_TITLE, k_rows_gamepad, (int)(sizeof k_rows_gamepad / sizeof k_rows_gamepad[0]), 9 },
};

static const jpt_card_t k_hints[2] = { { HINT_MARKUP, NULL, 0, 0 }, { HINT_MARKUP, NULL, 0, 0 } };

const jpt_card_t* jpt_prompt_card(int gamepad) { return &k_cards[gamepad ? 1 : 0]; }

const jpt_card_t* jpt_prompt_hint_card(int gamepad) { return &k_hints[gamepad ? 1 : 0]; }

const char* jpt_prompt_controls_card(int gamepad) {
    static char flat[2][1024];
    const jpt_card_t* c = jpt_prompt_card(gamepad);
    char* out = flat[gamepad ? 1 : 0];
    size_t used;
    if (out[0]) return out;
    used = (size_t)snprintf(out, sizeof flat[0], "%s", c->title);
    for (int i = 0; i < c->count && used < sizeof flat[0]; i++)
        used += (size_t)snprintf(out + used, sizeof flat[0] - used, "\r%s %s", c->rows[i].binding, c->rows[i].action);
    return out;
}

const char* jpt_prompt_controls_hint(void) { return HINT_MARKUP; }

int jpt_prompt_panel_kind(const char* text) {
    if (!text) return JPT_PANEL_NONE;
    if (!strcmp(text, CARD_MARKUP)) return JPT_PANEL_CARD;
    if (!strcmp(text, HINT_MARKUP)) return JPT_PANEL_HINT;
    return JPT_PANEL_NONE;
}

int jpt_card_line_h(int h) { return h + (h * 3 / 10 > 2 ? h * 3 / 10 : 2); }
static int card_pair_gap(int h) { return h / 2 > 4 ? h / 2 : 4; }
static int card_column_gap(int h) { return h * 2 > 12 ? h * 2 : 12; }
static int card_title_gap(int h) { return h * 6 / 10 > 4 ? h * 6 / 10 : 4; }

typedef struct {
    int binding, action, rows;
} card_column_t;

static int card_split(const jpt_card_metrics_t* m, int columns) {
    return columns >= 2 && m->first_column > 0 && m->first_column < m->count ? m->first_column : m->count;
}

static void card_columns(const jpt_card_metrics_t* m, int columns, card_column_t col[2]) {
    int split = card_split(m, columns);
    memset(col, 0, sizeof(card_column_t) * 2);
    for (int i = 0; i < m->count && i < JPT_CARD_MAX_ROWS; i++) {
        card_column_t* c = &col[i < split ? 0 : 1];
        if (m->binding_w[i] > c->binding) c->binding = m->binding_w[i];
        if (m->action_w[i] > c->action) c->action = m->action_w[i];
        c->rows++;
    }
}

static int column_w(const card_column_t* c, int h) { return c->rows ? c->binding + card_pair_gap(h) + c->action : 0; }

static int columns_w(const card_column_t col[2], int h) {
    return column_w(&col[0], h) + (col[1].rows ? card_column_gap(h) + column_w(&col[1], h) : 0);
}

void jpt_card_size(const jpt_card_metrics_t* m, int text_h, int columns, int* w, int* h) {
    card_column_t col[2];
    int rows, width;
    card_columns(m, columns, col);
    width = columns_w(col, text_h);
    if (m->title_w > width) width = m->title_w;
    rows = col[0].rows > col[1].rows ? col[0].rows : col[1].rows;
    *w = width;
    *h = (m->title_w > 0 ? text_h : 0) + (m->title_w > 0 && rows ? card_title_gap(text_h) : 0) +
         (rows ? (rows - 1) * jpt_card_line_h(text_h) + text_h : 0);
}

int jpt_card_place(const jpt_card_metrics_t* m, int text_h, int columns, jpt_box_t area, jpt_card_layout_t* out) {
    card_column_t col[2];
    int w, h, left, rows_top, split, column_left[2];
    memset(out, 0, sizeof *out);
    if (!m || text_h <= 0 || m->count < 0 || m->count > JPT_CARD_MAX_ROWS) return 0;
    card_columns(m, columns, col);
    jpt_card_size(m, text_h, columns, &w, &h);
    left = area.left + (area.right - area.left - w) / 2;
    out->text_h = text_h;
    out->line_h = jpt_card_line_h(text_h);
    out->columns = col[1].rows ? 2 : 1;
    out->box.left = left;
    out->box.top = area.top;
    out->box.right = left + w;
    out->box.bottom = area.top + h;
    out->title_x = left + (w - m->title_w) / 2;
    out->title_y = area.top;
    rows_top = area.top + (m->title_w > 0 ? text_h + card_title_gap(text_h) : 0);
    column_left[0] = left + (w - columns_w(col, text_h)) / 2;
    column_left[1] = column_left[0] + column_w(&col[0], text_h) + card_column_gap(text_h);
    split = card_split(m, columns);
    for (int i = 0; i < m->count; i++) {
        int c = i < split ? 0 : 1, row = c ? i - split : i;
        out->binding_x[i] = column_left[c] + col[c].binding - m->binding_w[i];
        out->action_x[i] = column_left[c] + col[c].binding + card_pair_gap(text_h);
        out->row_y[i] = rows_top + row * out->line_h;
    }
    return w <= area.right - area.left && h <= area.bottom - area.top;
}

void jpt_card_area(int left, int top, int w, int h, int game_text_h, int band, jpt_box_t* area, int* max_h) {
    int side = w * JPT_CARD_SIDE_PERCENT / 100 > JPT_CARD_MARGIN ? w * JPT_CARD_SIDE_PERCENT / 100 : JPT_CARD_MARGIN;
    int text = h * JPT_CARD_TEXT_PERMILLE / 1000;
    area->left = left + side;
    area->right = left + w - side;
    area->top = top + band;
    area->bottom = top + h - (JPT_CARD_MARGIN + 2 * game_text_h);
    if (area->bottom - area->top < h / 3) area->bottom = top + h - JPT_CARD_MARGIN;
    if (text < JPT_CARD_MIN_TEXT) text = JPT_CARD_MIN_TEXT;
    if (game_text_h > 0 && text > game_text_h) text = game_text_h;
    if (text < JPT_CARD_FLOOR_TEXT) text = JPT_CARD_FLOOR_TEXT;
    *max_h = text;
}

int jpt_card_band_floor(int game_text_h) { return JPT_CARD_MARGIN + game_text_h + game_text_h / 4; }

int jpt_card_fit(jpt_card_measure_fn measure, void* ctx, int max_h, int min_h, jpt_box_t area, jpt_card_metrics_t* m,
                 jpt_card_layout_t* out) {
    int columns = 2, h, measured;
    if (min_h < 1) min_h = 1;
    if (max_h < min_h) max_h = min_h;
    if (!measure(ctx, max_h, m)) return -1;
    measured = max_h;
    h = jpt_card_choose(m, max_h, max_h, area, &columns);
    if (h < min_h) h = min_h;
    for (;;) {
        if (h != measured) {
            if (!measure(ctx, h, m)) return -1;
            measured = h;
        }
        if (jpt_card_place(m, h, columns, area, out)) break;
        if (h > min_h) h--;
        else if (columns == 2) columns = 1;
        else return 0;
    }
    /* text widths do not scale linearly with the size */
    while (h < max_h) {
        jpt_card_layout_t larger;
        if (!measure(ctx, h + 1, m)) return -1;
        if (!jpt_card_place(m, h + 1, columns, area, &larger)) {
            if (!measure(ctx, h, m)) return -1;
            break;
        }
        *out = larger;
        h++;
    }
    return 1;
}

int jpt_card_choose(const jpt_card_metrics_t* ref, int ref_h, int max_h, jpt_box_t area, int* columns) {
    int best_h = 0, best_c = 1, aw = area.right - area.left, ah = area.bottom - area.top;
    for (int c = 2; c >= 1; c--) {
        int w, h, fit;
        double k;
        jpt_card_size(ref, ref_h, c, &w, &h);
        if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) continue;
        k = (double)aw / w < (double)ah / h ? (double)aw / w : (double)ah / h;
        fit = (int)(ref_h * k);
        if (fit > max_h) fit = max_h;
        if (!best_h || fit > best_h * 23 / 20) {
            best_h = fit;
            best_c = c;
        }
    }
    *columns = best_c;
    return best_h;
}

static const token_def_t* token_at(const char* p, size_t* length) {
    if (*p != '<') return NULL;
    for (size_t i = 0; i < sizeof k_tokens / sizeof k_tokens[0]; i++) {
        size_t n = strlen(k_tokens[i].name);
        if (!strncmp(p + 1, k_tokens[i].name, n) && p[1 + n] == '>') {
            *length = n + 2;
            return &k_tokens[i];
        }
    }
    return NULL;
}

static const phrase_rule_t* rule_at(const char* p) {
    for (size_t i = 0; i < sizeof k_rules / sizeof k_rules[0]; i++)
        if (!strncmp(p, k_rules[i].phrase, strlen(k_rules[i].phrase))) return &k_rules[i];
    return NULL;
}

int jpt_prompt_text_has_prompts(const char* text) {
    size_t n;
    if (!text) return 0;
    if (strstr(text, CARD_MARKUP)) return 1;
    for (const char* p = text; *p; p++)
        if (token_at(p, &n) || rule_at(p)) return 1;
    return 0;
}

static void copy_label(char* dst, const char* src) {
    strncpy(dst, src ? src : "", sizeof ((jpt_prompt_token_t*)0)->label - 1);
    dst[sizeof ((jpt_prompt_token_t*)0)->label - 1] = 0;
}

static void key_token(uint32_t vk, jpt_prompt_token_t* t) {
    char buf[24];
    t->prompt.gamepad = 0;
    t->prompt.vk = vk;
    copy_label(t->label, vk ? jpt_key_label(vk, buf, sizeof buf) : "Unbound");
}

static void pad_token(const jpt_prompt_context_t* c, int control, int dir, int tilt, jpt_prompt_token_t* t) {
    t->prompt.gamepad = 1;
    t->prompt.control = control;
    t->prompt.dir = dir;
    t->prompt.tilt = tilt;
    copy_label(t->label, control == JPT_PAD_NONE ? "Unbound" : jpt_pad_control_name(c->family, control));
}

static void move_keys_label(const jpt_prompt_context_t* c, char* out, size_t cap) {
    static const uint32_t order[4] = { JPT_CMD_RUN, JPT_CMD_LEFT, JPT_CMD_WALK, JPT_CMD_RIGHT };
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < 4; i++) {
        char buf[24];
        const char* label = jpt_key_label(jpt_keymap_key_for(c->keymap, c->keymap_count, order[i]), buf, sizeof buf);
        int n = snprintf(out + used, cap - used, "%s%s", strlen(label) == 1 || !i ? "" : " ", label);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

static void resolve(const token_def_t* def, const jpt_prompt_context_t* c, jpt_prompt_token_t* t) {
    memset(t, 0, sizeof *t);
    t->kind = def->kind;
    t->prompt.control = JPT_PAD_NONE;
    switch (def->kind) {
    case JPT_TOKEN_COMMAND:
        if (!c->gamepad) {
            key_token(jpt_keymap_key_for(c->keymap, c->keymap_count, def->command), t);
        } else if (def->action >= 0) {
            pad_token(c, c->settings ? c->settings->bind[def->action] : JPT_PAD_NONE, JPT_DIR_NONE, 0, t);
        } else {
            jpt_prompt_t p;
            jpt_prompt_for_command(def->command, 1, c->settings, NULL, 0, &p);
            pad_token(c, p.control, p.dir, p.tilt, t);
        }
        break;
    case JPT_TOKEN_LOOK:
        if (c->gamepad) {
            pad_token(c, JPT_PAD_RIGHT_STICK_MOVE, JPT_DIR_NONE, 0, t);
        } else {
            t->mouse_move = 1;
            copy_label(t->label, "Mouse movement");
        }
        break;
    case JPT_TOKEN_MOVE:
        if (c->gamepad) {
            pad_token(c, JPT_PAD_LEFT_STICK_MOVE, JPT_DIR_NONE, 0, t);
        } else {
            move_keys_label(c, t->label, sizeof t->label);
        }
        break;
    case JPT_TOKEN_PAUSE:
        if (c->gamepad) pad_token(c, JPT_PAD_START, JPT_DIR_NONE, 0, t);
        else key_token(0x1B, t);
        break;
    case JPT_TOKEN_CONTROLS:
        if (c->gamepad) pad_token(c, JPT_PAD_BACK, JPT_DIR_NONE, 0, t);
        else key_token(0x71, t); /* F2 */
        break;
    case JPT_TOKEN_HINT:
    case JPT_TOKEN_OBJECTIVES:
    case JPT_TOKEN_SHOW_OBJECTIVE: {
        int control = def->kind == JPT_TOKEN_HINT         ? JPT_PAD_HINT_CONTROL
                      : def->kind == JPT_TOKEN_OBJECTIVES ? JPT_PAD_OBJECTIVES_CONTROL
                                                          : JPT_PAD_SHOW_OBJECTIVE_CONTROL;
        if (c->gamepad)
            pad_token(c, jpt_pad_control_free(c->settings, control) ? control : JPT_PAD_NONE, JPT_DIR_NONE, 0, t);
        else key_token(def->kind == JPT_TOKEN_HINT ? 0x70 : def->kind == JPT_TOKEN_OBJECTIVES ? 0x72 : 0x73, t);
        break;
    }
    case JPT_TOKEN_KEY: key_token(def->command, t); break;
    case JPT_TOKEN_PAD: pad_token(c, (int)def->command, JPT_DIR_NONE, 0, t); break;
    }
}

typedef struct {
    char* out;
    size_t cap, used;
    jpt_prompt_token_t* tokens;
    int max_tokens, count, overflow;
} writer_t;

static void put(writer_t* w, const char* s, size_t n) {
    if (w->used + n + 1 > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->out + w->used, s, n);
    w->used += n;
    w->out[w->used] = 0;
}

static void put_token(writer_t* w, const token_def_t* def, const jpt_prompt_context_t* c) {
    const char mark = JPT_PROMPT_MARK;
    if (w->count >= w->max_tokens) {
        w->overflow = 1;
        return;
    }
    if (w->tokens) resolve(def, c, &w->tokens[w->count]);
    w->count++;
    put(w, &mark, 1);
}

static void put_template(writer_t* w, const char* t, const jpt_prompt_context_t* c) {
    while (*t) {
        size_t n;
        const token_def_t* def = token_at(t, &n);
        if (def) {
            put_token(w, def, c);
            t += n;
        } else {
            put(w, t++, 1);
        }
    }
}

int jpt_prompt_text_render(const char* text, const jpt_prompt_context_t* c, char* out, size_t cap,
                           jpt_prompt_token_t* tokens, int max_tokens) {
    writer_t w = { out, cap, 0, tokens, max_tokens, 0, 0 };
    if (!text || !c || !out || !cap) return -1;
    out[0] = 0;
    while (*text && !w.overflow) {
        size_t n;
        const token_def_t* def;
        const phrase_rule_t* rule;
        if (!strncmp(text, CARD_MARKUP, strlen(CARD_MARKUP))) {
            put_template(&w, jpt_prompt_controls_card(c->gamepad), c);
            text += strlen(CARD_MARKUP);
        } else if ((def = token_at(text, &n)) != NULL) {
            put_token(&w, def, c);
            text += n;
        } else if ((rule = rule_at(text)) != NULL) {
            const char* with = c->gamepad ? rule->gamepad : rule->keyboard;
            if (with) put_template(&w, with, c);
            else put(&w, rule->phrase, strlen(rule->phrase));
            text += strlen(rule->phrase);
        } else {
            put(&w, text++, 1);
        }
    }
    return w.overflow || !w.count ? -1 : w.count;
}

int jpt_prompt_text_plain(const char* text, const jpt_prompt_context_t* c, char* out, size_t cap) {
    char marked[1024];
    jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
    int count = jpt_prompt_text_render(text, c, marked, sizeof marked, tokens, JPT_PROMPT_MAX_TOKENS), next = 0;
    writer_t w = { out, cap, 0, NULL, 0, 0, 0 };
    if (count < 0 || !out || !cap) return -1;
    out[0] = 0;
    for (const char* p = marked; *p && !w.overflow; p++) {
        if (*p == JPT_PROMPT_MARK && next < count) {
            put(&w, "[", 1);
            put(&w, tokens[next].label, strlen(tokens[next].label));
            put(&w, "]", 1);
            next++;
        } else {
            put(&w, p, 1);
        }
    }
    return w.overflow ? -1 : count;
}

/* "<HAND> + <LOOK>" as binding names. */
static void binding_text(const char* t, const jpt_prompt_context_t* c, char* out, size_t cap) {
    writer_t w = { out, cap, 0, NULL, 0, 0, 0 };
    out[0] = 0;
    while (*t && !w.overflow) {
        size_t n;
        const token_def_t* def = token_at(t, &n);
        if (def) {
            jpt_prompt_token_t tok;
            resolve(def, c, &tok);
            put(&w, tok.label, strlen(tok.label));
            t += n;
        } else {
            put(&w, t++, 1);
        }
    }
}

typedef struct {
    const char* binding;
    const char* action;
} guide_def_t;

static const guide_def_t k_guide_gamepad[] = {
    { "<MOVE>", "Move (walk, run, sidestep)" },
    { "<LOOK>", "Look" },
    { "<JUMP>", "Jump" },
    { "<CROUCH>", "Crouch (hold)" },
    { "<CROUCH_TOGGLE>", "Crouch on/off" },
    { "<USE>", "Use / Fire" },
    { "<HAND> + <LOOK>", "Raise and move the hand" },
    { "<GRAB>", "Grab / Drop" },
    { "<THROW>", "Throw" },
    { "<STOW>", "Stow / Retrieve" },
    { "<WRIST> + <HAND> + <LOOK>", "Rotate wrist" },
    { "<ARM> + <HAND> + <LOOK>", "Rotate arm" },
    { "<WRIST> + <ARM> + <HAND>", "Straighten the hand" },
    { "<PAUSE>", "Pause menu" },
    { "<CONTROLS>", "Show these controls in the game" },
};

static const guide_def_t k_guide_keyboard[] = {
    { "<RUN>", "Run" },
    { "<WALK>", "Walk" },
    { "<LEFT>", "Step left" },
    { "<RIGHT>", "Step right" },
    { "<BACKUP>", "Walk backwards" },
    { "<LOOK>", "Look" },
    { "<JUMP>", "Jump" },
    { "<CROUCH>", "Crouch (hold)" },
    { "<THROW>", "Throw" },
    { "<STOW>", "Stow / Retrieve" },
    { "<REPLAY>", "Replay voice over" },
    { "<USE>", "Fire / Use" },
    { "<HAND>", "Reach with the hand (hold)" },
    { "<HAND> + <LOOK>", "Move the arm" },
    { "<HAND> + <GRAB>", "Grab an object" },
    { "<GRAB>", "Drop the object" },
    { "<ARM> + <HAND> + <LOOK>", "Rotate arm" },
    { "<WRIST> + <HAND> + <LOOK>", "Rotate wrist" },
    { "<PAUSE>", "Pause menu" },
    { "<CONTROLS>", "Show these controls in the game" },
};

int jpt_controls_guide(const jpt_prompt_context_t* c, jpt_guide_row_t* rows, int max_rows) {
    const guide_def_t* defs = c->gamepad ? k_guide_gamepad : k_guide_keyboard;
    int n = c->gamepad ? (int)(sizeof k_guide_gamepad / sizeof k_guide_gamepad[0])
                       : (int)(sizeof k_guide_keyboard / sizeof k_guide_keyboard[0]);
    if (n > max_rows) n = max_rows;
    for (int i = 0; i < n; i++) {
        binding_text(defs[i].binding, c, rows[i].binding, sizeof rows[i].binding);
        rows[i].action = defs[i].action;
    }
    return n;
}
