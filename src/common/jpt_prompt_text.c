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
    { "REPLAY", JPT_TOKEN_COMMAND, JPT_CMD_REPLAY, JPT_ACT_REPLAY },
    { "RUN", JPT_TOKEN_COMMAND, JPT_CMD_RUN, -1 },
    { "WALK", JPT_TOKEN_COMMAND, JPT_CMD_WALK, -1 },
    { "BACKUP", JPT_TOKEN_COMMAND, JPT_CMD_BACKUP, -1 },
    { "LEFT", JPT_TOKEN_COMMAND, JPT_CMD_LEFT, -1 },
    { "RIGHT", JPT_TOKEN_COMMAND, JPT_CMD_RIGHT, -1 },
    { "LOOK", JPT_TOKEN_LOOK, 0, -1 },
    { "MOVE", JPT_TOKEN_MOVE, 0, -1 },
    { "PAUSE", JPT_TOKEN_PAUSE, 0, -1 },
    { "CONTROLS", JPT_TOKEN_CONTROLS, 0, -1 },
};

/* The tutorial's subtitles (Stream.tpa) name keys in a few fixed phrases; NULL keeps the phrase for that device. */
typedef struct {
    const char* phrase;
    const char* keyboard;
    const char* gamepad;
} phrase_rule_t;

static const phrase_rule_t k_rules[] = {
    { "Press the 'S' key to walk slowly", "Press <WALK> to walk slowly", "Push <WALK> gently to walk slowly" },
    { "Press 'W' to run", "Press <RUN> to run", "Push <RUN> all the way to run" },
    { "To back up, press 'X'", "To back up, press <BACKUP>", "To back up, pull <BACKUP> back" },
    { "Press 'A' and 'D' to step left and right", "Press <LEFT> and <RIGHT> to step left and right",
      "Push <LEFT> or <RIGHT> to step left and right" },
    { "press 'Z' to crouch", "press <CROUCH> to crouch", "hold <CROUCH> to crouch" },
    { "'Q'", "<JUMP>", "<JUMP>" },
    { "'Z'", "<CROUCH>", "<CROUCH>" },
    { "by moving the mouse", NULL, "with <LOOK>" },
    { "move the mouse", NULL, "move <LOOK>" },
    { "the left mouse button", "<HAND>", "<HAND>" },
    { "the right mouse button", "<GRAB>", "<GRAB>" },
    { "the space bar", "<USE>", "<USE>" },
    { "space bar", "<USE>", "<USE>" },
};

#define CARD_MARKUP "<CARD>"

static const char k_card_gamepad[] =
    "<MOVE> Move     <LOOK> Look     <JUMP> Jump\r"
    "<CROUCH> Crouch (hold)     <CROUCH_TOGGLE> Crouch on/off\r"
    "<HAND> + <LOOK> Raise and move the hand\r"
    "<GRAB> Grab / drop     <USE> Use / fire     <THROW> Throw\r"
    "<WRIST> + <LOOK> Rotate wrist     <ARM> + <LOOK> Rotate arm\r"
    "<STOW> Stow / retrieve     <REPLAY> Replay voice     <PAUSE> Menu";

static const char k_card_keyboard[] =
    "<RUN> Run     <WALK> Walk     <BACKUP> Back     <LEFT> <RIGHT> Step\r"
    "<JUMP> Jump     <CROUCH> Crouch (hold)     <LOOK> Look\r"
    "<HAND> + <LOOK> Raise and move the hand\r"
    "<HAND> + <GRAB> Grab     <GRAB> Drop     <USE> Use / fire     <THROW> Throw\r"
    "<WRIST> + <HAND> + <LOOK> Rotate wrist     <ARM> + <HAND> + <LOOK> Rotate arm\r"
    "<STOW> Stow / retrieve     <REPLAY> Replay voice     <PAUSE> Menu";

const char* jpt_prompt_controls_card(int gamepad) { return gamepad ? k_card_gamepad : k_card_keyboard; }

const char* jpt_prompt_controls_hint(void) { return "<CONTROLS> Controls"; }

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
    case JPT_TOKEN_CONTROLS: pad_token(c, JPT_PAD_BACK, JPT_DIR_NONE, 0, t); break;
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
    { "<GRAB>", "Grab / Drop (hand raised to grab)" },
    { "<THROW>", "Throw" },
    { "<STOW>", "Stow / Retrieve" },
    { "<WRIST> + <LOOK>", "Rotate wrist" },
    { "<ARM> + <LOOK>", "Rotate arm" },
    { "<WRIST> + <ARM>", "Straighten the hand" },
    { "<REPLAY>", "Replay voice over" },
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
