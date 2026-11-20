#include "jpt_gamepad.h"
#include "jpt_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* const jpt_glyph_style_keys[JPT_GLYPHS_COUNT] = { "automatic", "xbox", "playstation", "steamdeck",
                                                             "generic" };
const char* const jpt_glyph_style_labels[JPT_GLYPHS_COUNT] = { "Automatic", "Xbox", "PlayStation", "Steam Deck",
                                                               "Generic" };

const char* jpt_pad_family_name(jpt_pad_family_t family) {
    static const char* const names[JPT_FAMILY_COUNT] = { "Xbox", "PlayStation", "Steam Deck", "Nintendo", "Generic" };
    return family >= 0 && family < JPT_FAMILY_COUNT ? names[family] : "Generic";
}

static int contains_nocase(const char* text, const char* word) {
    size_t n = strlen(word);
    if (!text) return 0;
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)word[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

#define VALVE_VENDOR 0x28DEu
#define STEAM_DECK_PRODUCT 0x1205u

jpt_pad_family_t jpt_pad_family_of(jpt_pad_type_t type, uint16_t vendor, uint16_t product, const char* name) {
    if ((vendor == VALVE_VENDOR && product == STEAM_DECK_PRODUCT) || contains_nocase(name, "Steam Deck"))
        return JPT_FAMILY_STEAM_DECK;
    switch (type) {
    case JPT_PAD_TYPE_XBOX360:
    case JPT_PAD_TYPE_XBOXONE: return JPT_FAMILY_XBOX;
    case JPT_PAD_TYPE_PS3:
    case JPT_PAD_TYPE_PS4:
    case JPT_PAD_TYPE_PS5: return JPT_FAMILY_PLAYSTATION;
    case JPT_PAD_TYPE_SWITCH_PRO:
    case JPT_PAD_TYPE_JOYCON_LEFT:
    case JPT_PAD_TYPE_JOYCON_RIGHT:
    case JPT_PAD_TYPE_JOYCON_PAIR:
    case JPT_PAD_TYPE_GAMECUBE: return JPT_FAMILY_NINTENDO;
    default: return JPT_FAMILY_GENERIC;
    }
}

jpt_pad_family_t jpt_glyph_family(jpt_glyph_style_t style, int have_pad, jpt_pad_family_t active) {
    switch (style) {
    case JPT_GLYPHS_XBOX: return JPT_FAMILY_XBOX;
    case JPT_GLYPHS_PLAYSTATION: return JPT_FAMILY_PLAYSTATION;
    case JPT_GLYPHS_STEAM_DECK: return JPT_FAMILY_STEAM_DECK;
    case JPT_GLYPHS_GENERIC: return JPT_FAMILY_GENERIC;
    default: return have_pad && active >= 0 && active < JPT_FAMILY_COUNT ? active : JPT_FAMILY_XBOX;
    }
}

#define LABEL_COUNT (JPT_PAD_RIGHT_STICK_MOVE + 1)

static const char* const k_glyph_text[JPT_FAMILY_COUNT][LABEL_COUNT] = {
    { "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS", "RS", "LB", "RB", "", "", "", "", "LT", "RT", "L", "R" },
    { "", "", "", "", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "", "", "", "", "L2", "R2", "L", "R" },
    { "A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "", "", "", "", "L2", "R2", "L", "R" },
    { "B", "A", "Y", "X", "-", "Home", "+", "LS", "RS", "L", "R", "", "", "", "", "ZL", "ZR", "L", "R" },
    { "", "", "", "", "Select", "Home", "Start", "L3", "R3", "L1", "R1", "", "", "", "", "L2", "R2", "L", "R" },
};

static const char* const k_dpad_names[4] = { "D-pad up", "D-pad down", "D-pad left", "D-pad right" };

static const char* const k_control_names[JPT_FAMILY_COUNT][LABEL_COUNT] = {
    { "A", "B", "X", "Y", "View", "Xbox button", "Menu", "Left stick click (LS)", "Right stick click (RS)",
      "Left bumper (LB)", "Right bumper (RB)", NULL, NULL, NULL, NULL, "Left trigger (LT)", "Right trigger (RT)",
      "Left stick", "Right stick" },
    { "Cross", "Circle", "Square", "Triangle", "Create", "PS button", "Options", "L3", "R3", "L1", "R1", NULL, NULL,
      NULL, NULL, "L2", "R2", "Left stick", "Right stick" },
    { "A", "B", "X", "Y", "View", "Steam button", "Menu", "L3", "R3", "L1", "R1", NULL, NULL, NULL, NULL, "L2", "R2",
      "Left stick", "Right stick" },
    { "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick click", "Right stick click", "L", "R", NULL, NULL, NULL,
      NULL, "ZL", "ZR", "Left stick", "Right stick" },
    { "Bottom button", "Right button", "Left button", "Top button", "Select", "Home", "Start", "L3", "R3", "L1", "R1",
      NULL, NULL, NULL, NULL, "L2", "R2", "Left stick", "Right stick" },
};

static int family_ok(jpt_pad_family_t f) { return f >= 0 && f < JPT_FAMILY_COUNT; }

const char* jpt_pad_glyph_text(jpt_pad_family_t family, int control) {
    if (!family_ok(family) || control < 0 || control >= LABEL_COUNT) return "";
    return k_glyph_text[family][control];
}

const char* jpt_pad_control_name(jpt_pad_family_t family, int control) {
    if (control >= JPT_PAD_DPAD_UP && control <= JPT_PAD_DPAD_RIGHT) return k_dpad_names[control - JPT_PAD_DPAD_UP];
    if (!family_ok(family) || control < 0 || control >= LABEL_COUNT) return "None";
    return k_control_names[family][control];
}

const jpt_pad_action_info_t jpt_pad_actions[JPT_ACT_COUNT] = {
    { "gamepad_bind_jump", "Jump", JPT_CMD_JUMP, JPT_PAD_SOUTH },
    { "gamepad_bind_crouch", "Crouch", JPT_CMD_CROUCH, JPT_PAD_EAST },
    { "gamepad_bind_crouch_toggle", "Crouch (toggle)", JPT_CMD_CROUCH, JPT_PAD_LEFT_STICK },
    { "gamepad_bind_use", "Use/Fire", JPT_CMD_USE, JPT_PAD_RIGHT_TRIGGER },
    { "gamepad_bind_hand", "Move Hand", JPT_CMD_HAND, JPT_PAD_LEFT_TRIGGER },
    { "gamepad_bind_grab", "Grab/Drop", JPT_CMD_GRAB, JPT_PAD_WEST },
    { "gamepad_bind_throw", "Throw", JPT_CMD_THROW, JPT_PAD_NORTH },
    { "gamepad_bind_stow", "Stow/Retrieve", JPT_CMD_STOW, JPT_PAD_DPAD_UP },
    { "gamepad_bind_wrist", "Rotate Wrist", JPT_CMD_WRIST, JPT_PAD_LEFT_SHOULDER },
    { "gamepad_bind_arm", "Rotate Arm", JPT_CMD_ARM, JPT_PAD_RIGHT_SHOULDER },
};

static const char* const k_control_keys[JPT_PAD_CONTROL_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "lefttrigger", "righttrigger",
};

int jpt_pad_control_free(const jpt_gamepad_settings_t* s, int control) {
    if (!s) return 0;
    for (int a = 0; a < JPT_ACT_COUNT; a++)
        if (s->bind[a] == control) return 0;
    return 1;
}

int jpt_pad_bindable(int control) {
    return control >= 0 && control < JPT_PAD_CONTROL_COUNT && control != JPT_PAD_BACK && control != JPT_PAD_GUIDE &&
           control != JPT_PAD_START;
}

const char* jpt_pad_control_key(int control) {
    return control >= 0 && control < JPT_PAD_CONTROL_COUNT ? k_control_keys[control] : "none";
}

int jpt_pad_control_from_key(const char* key) {
    if (!key) return -1;
    if (!_stricmp(key, "none")) return JPT_PAD_NONE;
    for (int i = 0; i < JPT_PAD_CONTROL_COUNT; i++)
        if (!_stricmp(key, k_control_keys[i])) return i;
    return -1;
}

static const char* const k_plain_keys[6] = {
    JPT_KEY_GAMEPAD_GLYPHS,        JPT_KEY_GAMEPAD_LOOK_X,         JPT_KEY_GAMEPAD_LOOK_Y,
    JPT_KEY_GAMEPAD_INVERT_Y,      JPT_KEY_GAMEPAD_STICK_DEADZONE, JPT_KEY_GAMEPAD_TRIGGER_DEADZONE,
};
static const char* const k_plain_defaults[6] = { "automatic", "100", "100", "0", "15", "12" };

const char* jpt_gamepad_setting_key(int index) {
    if (index < 0 || index >= JPT_GAMEPAD_SETTING_COUNT) return NULL;
    return index < 6 ? k_plain_keys[index] : jpt_pad_actions[index - 6].key;
}

const char* jpt_gamepad_setting_default(int index) {
    if (index < 0 || index >= JPT_GAMEPAD_SETTING_COUNT) return NULL;
    return index < 6 ? k_plain_defaults[index] : jpt_pad_control_key(jpt_pad_actions[index - 6].default_control);
}

void jpt_gamepad_defaults(jpt_gamepad_settings_t* s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < JPT_GAMEPAD_SETTING_COUNT; i++) jpt_gamepad_apply(s, jpt_gamepad_setting_key(i), jpt_gamepad_setting_default(i));
}

static int parse_int(const char* text, int lo, int hi, int* out) {
    char* end;
    long v;
    if (!text || !*text) return 0;
    v = strtol(text, &end, 10);
    if (*end || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

static int parse_flag(const char* text, int* out) {
    static const char* const on[] = { "1", "on", "yes", "true" };
    static const char* const off[] = { "0", "off", "no", "false" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(text, on[i])) return *out = 1, 1;
        if (!_stricmp(text, off[i])) return *out = 0, 1;
    }
    return 0;
}

int jpt_gamepad_apply(jpt_gamepad_settings_t* s, const char* key, const char* value) {
    if (!s || !key || !value) return 0;
    if (!strcmp(key, JPT_KEY_GAMEPAD_GLYPHS)) {
        for (int i = 0; i < JPT_GLYPHS_COUNT; i++)
            if (!_stricmp(value, jpt_glyph_style_keys[i])) return s->glyphs = i, 1;
        return 0;
    }
    if (!strcmp(key, JPT_KEY_GAMEPAD_LOOK_X)) return parse_int(value, JPT_PAD_LOOK_MIN, JPT_PAD_LOOK_MAX, &s->look_x);
    if (!strcmp(key, JPT_KEY_GAMEPAD_LOOK_Y)) return parse_int(value, JPT_PAD_LOOK_MIN, JPT_PAD_LOOK_MAX, &s->look_y);
    if (!strcmp(key, JPT_KEY_GAMEPAD_INVERT_Y)) return parse_flag(value, &s->invert_y);
    if (!strcmp(key, JPT_KEY_GAMEPAD_STICK_DEADZONE))
        return parse_int(value, JPT_PAD_STICK_DEADZONE_MIN, JPT_PAD_STICK_DEADZONE_MAX, &s->stick_deadzone);
    if (!strcmp(key, JPT_KEY_GAMEPAD_TRIGGER_DEADZONE))
        return parse_int(value, JPT_PAD_TRIGGER_DEADZONE_MIN, JPT_PAD_TRIGGER_DEADZONE_MAX, &s->trigger_deadzone);
    for (int a = 0; a < JPT_ACT_COUNT; a++) {
        int control;
        if (strcmp(key, jpt_pad_actions[a].key)) continue;
        control = jpt_pad_control_from_key(value);
        if (control != JPT_PAD_NONE && !jpt_pad_bindable(control)) return 0;
        s->bind[a] = control;
        return 1;
    }
    return 0;
}

int jpt_gamepad_is_key(const char* key) {
    for (int i = 0; key && i < JPT_GAMEPAD_SETTING_COUNT; i++)
        if (!strcmp(key, jpt_gamepad_setting_key(i))) return 1;
    return 0;
}

/* The retail table at 0x0066AE78. */
static const jpt_key_binding_t k_default_keymap[JPT_KEYMAP_COUNT] = {
    { 0x01, JPT_CMD_HAND },  { 0x02, JPT_CMD_GRAB },   { 0x10, JPT_CMD_WRIST },  { 0x11, JPT_CMD_ARM },
    { 0x20, JPT_CMD_USE },   { 'Q', JPT_CMD_JUMP },    { 'Z', JPT_CMD_CROUCH },  { 'E', JPT_CMD_STOW },
    { 'F', JPT_CMD_THROW },  { 'R', JPT_CMD_REPLAY },  { 'W', JPT_CMD_RUN },     { 'S', JPT_CMD_WALK },
    { 'X', JPT_CMD_BACKUP }, { 'A', JPT_CMD_LEFT },    { 'D', JPT_CMD_RIGHT },
};

void jpt_keymap_default(jpt_key_binding_t out[JPT_KEYMAP_COUNT]) { memcpy(out, k_default_keymap, sizeof k_default_keymap); }

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int jpt_keymap_parse(const uint8_t* data, size_t size, jpt_key_binding_t out[JPT_KEYMAP_COUNT]) {
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_keymap_default(out);
    if (!data || size != sizeof(uint32_t) * 2 * JPT_KEYMAP_COUNT) return 0;
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++) {
        map[i].vk = le32(data + 8 * i);
        map[i].command = le32(data + 8 * i + 4);
        if (map[i].vk > 0xFF || !map[i].command) return 0;
    }
    memcpy(out, map, sizeof map);
    return 1;
}

uint32_t jpt_keymap_key_for(const jpt_key_binding_t* map, int count, uint32_t command) {
    for (int i = 0; map && i < count; i++)
        if (map[i].command == command && map[i].vk) return map[i].vk;
    return 0;
}

const char* jpt_key_label(uint32_t vk, char* buf, size_t cap) {
    static const struct {
        uint32_t vk;
        const char* label;
    } named[] = {
        { 0x01, "Mouse Left" }, { 0x02, "Mouse Right" }, { 0x04, "Mouse Middle" },     { 0x05, "Mouse 4" },
        { 0x06, "Mouse 5" },    { 0x08, "Backspace" },   { 0x09, "Tab" },         { 0x0D, "Enter" },
        { 0x10, "Shift" },      { 0x11, "Ctrl" },        { 0x12, "Alt" },         { 0x13, "Pause" },
        { 0x14, "Caps Lock" },  { 0x1B, "Esc" },         { 0x20, "Space" },       { 0x21, "Page Up" },
        { 0x22, "Page Down" },  { 0x23, "End" },         { 0x24, "Home" },        { 0x25, "Left" },
        { 0x26, "Up" },         { 0x27, "Right" },       { 0x28, "Down" },        { 0x2C, "Print Screen" },
        { 0x2D, "Insert" },     { 0x2E, "Delete" },      { 0x5B, "Win" },         { 0x5C, "Win" },
        { 0x5D, "Menu" },       { 0x6A, "Num *" },       { 0x6B, "Num +" },       { 0x6D, "Num -" },
        { 0x6E, "Num ." },      { 0x6F, "Num /" },       { 0x90, "Num Lock" },    { 0x91, "Scroll Lock" },
        { 0xA0, "Left Shift" }, { 0xA1, "Right Shift" }, { 0xA2, "Left Ctrl" },   { 0xA3, "Right Ctrl" },
        { 0xA4, "Left Alt" },   { 0xA5, "Right Alt" },   { 0xBA, ";" },           { 0xBB, "=" },
        { 0xBC, "," },          { 0xBD, "-" },           { 0xBE, "." },           { 0xBF, "/" },
        { 0xC0, "`" },          { 0xDB, "[" },           { 0xDC, "\\" },          { 0xDD, "]" },
        { 0xDE, "'" },
    };
    if (!buf || cap < 2) return "";
    if (!vk) return "None";
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].vk == vk) return named[i].label;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) snprintf(buf, cap, "%c", (char)vk);
    else if (vk >= 0x60 && vk <= 0x69) snprintf(buf, cap, "Num %u", vk - 0x60);
    else if (vk >= 0x70 && vk <= 0x87) snprintf(buf, cap, "F%u", vk - 0x6F);
    else snprintf(buf, cap, "Key %02X", vk);
    return buf;
}

const jpt_game_control_t jpt_game_controls[JPT_GAME_CONTROL_COUNT] = {
    { JPT_CMD_WALK, "Walk Forward" },        { JPT_CMD_RUN, "Run Forward" },     { JPT_CMD_BACKUP, "Walk Backwards" },
    { JPT_CMD_LEFT, "Sidestep Left" },       { JPT_CMD_RIGHT, "Sidestep Right" }, { JPT_CMD_JUMP, "Jump" },
    { JPT_CMD_CROUCH, "Crouch" },            { JPT_CMD_USE, "Use/Fire" },         { JPT_CMD_WRIST, "Rotate Wrist" },
    { JPT_CMD_HAND, "Move Hand" },           { JPT_CMD_GRAB, "Grab/Drop" },       { JPT_CMD_STOW, "Stow/Retrieve" },
    { JPT_CMD_REPLAY, "Replay voice over" }, { JPT_CMD_THROW, "Throw" },          { JPT_CMD_ARM, "Rotate Arm" },
};

static int stick_prompt(uint32_t command, jpt_prompt_t* out) {
    static const struct {
        uint32_t command;
        int dir, tilt;
    } moves[] = {
        { JPT_CMD_RUN, JPT_DIR_UP, 2 },     { JPT_CMD_WALK, JPT_DIR_UP, 1 },     { JPT_CMD_BACKUP, JPT_DIR_DOWN, 2 },
        { JPT_CMD_LEFT, JPT_DIR_LEFT, 2 },  { JPT_CMD_RIGHT, JPT_DIR_RIGHT, 2 },
    };
    for (size_t i = 0; i < sizeof moves / sizeof moves[0]; i++)
        if (moves[i].command == command) {
            out->control = JPT_PAD_LEFT_STICK_MOVE;
            out->dir = moves[i].dir;
            out->tilt = moves[i].tilt;
            return 1;
        }
    return 0;
}

int jpt_prompt_for_command(uint32_t command, int gamepad, const jpt_gamepad_settings_t* s,
                           const jpt_key_binding_t* keymap, int keymap_count, jpt_prompt_t* out) {
    memset(out, 0, sizeof *out);
    out->control = JPT_PAD_NONE;
    out->gamepad = gamepad != 0;
    if (!gamepad) {
        out->vk = jpt_keymap_key_for(keymap, keymap_count, command);
        return out->vk != 0;
    }
    if (stick_prompt(command, out)) return 1;
    for (int a = 0; s && a < JPT_ACT_COUNT; a++)
        if (jpt_pad_actions[a].command == command && s->bind[a] != JPT_PAD_NONE) {
            out->control = s->bind[a];
            return 1;
        }
    return 0;
}
