/* Gamepad settings, the game's actions and their bindings, and the names prompts show for keys and gamepad controls.
 * Shared by the host, the launcher and the tests: no SDL and no window calls. */
#ifndef JPT_GAMEPAD_H
#define JPT_GAMEPAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physical controls in SDL3's positional layout: the buttons keep SDL_GamepadButton's values, the triggers follow. */
enum {
    JPT_PAD_SOUTH,
    JPT_PAD_EAST,
    JPT_PAD_WEST,
    JPT_PAD_NORTH,
    JPT_PAD_BACK,
    JPT_PAD_GUIDE,
    JPT_PAD_START,
    JPT_PAD_LEFT_STICK,
    JPT_PAD_RIGHT_STICK,
    JPT_PAD_LEFT_SHOULDER,
    JPT_PAD_RIGHT_SHOULDER,
    JPT_PAD_DPAD_UP,
    JPT_PAD_DPAD_DOWN,
    JPT_PAD_DPAD_LEFT,
    JPT_PAD_DPAD_RIGHT,
    JPT_PAD_LEFT_TRIGGER,
    JPT_PAD_RIGHT_TRIGGER,
    JPT_PAD_CONTROL_COUNT,
    /* Prompt-only: a stick as a whole (movement, looking). */
    JPT_PAD_LEFT_STICK_MOVE = JPT_PAD_CONTROL_COUNT,
    JPT_PAD_RIGHT_STICK_MOVE,
    JPT_PAD_NONE = 0xFF
};

#define JPT_PAD_BIT(control) (1u << (control))

/* SDL_GamepadType's values, in its order. */
typedef enum {
    JPT_PAD_TYPE_UNKNOWN,
    JPT_PAD_TYPE_STANDARD,
    JPT_PAD_TYPE_XBOX360,
    JPT_PAD_TYPE_XBOXONE,
    JPT_PAD_TYPE_PS3,
    JPT_PAD_TYPE_PS4,
    JPT_PAD_TYPE_PS5,
    JPT_PAD_TYPE_SWITCH_PRO,
    JPT_PAD_TYPE_JOYCON_LEFT,
    JPT_PAD_TYPE_JOYCON_RIGHT,
    JPT_PAD_TYPE_JOYCON_PAIR,
    JPT_PAD_TYPE_GAMECUBE,
    JPT_PAD_TYPE_COUNT
} jpt_pad_type_t;

/* Whose button names and glyphs a controller has. Nintendo is chosen only automatically. */
typedef enum {
    JPT_FAMILY_XBOX,
    JPT_FAMILY_PLAYSTATION,
    JPT_FAMILY_STEAM_DECK,
    JPT_FAMILY_NINTENDO,
    JPT_FAMILY_GENERIC,
    JPT_FAMILY_COUNT
} jpt_pad_family_t;

typedef enum {
    JPT_GLYPHS_AUTOMATIC,
    JPT_GLYPHS_XBOX,
    JPT_GLYPHS_PLAYSTATION,
    JPT_GLYPHS_STEAM_DECK,
    JPT_GLYPHS_GENERIC,
    JPT_GLYPHS_COUNT
} jpt_glyph_style_t;

extern const char* const jpt_glyph_style_keys[JPT_GLYPHS_COUNT];
extern const char* const jpt_glyph_style_labels[JPT_GLYPHS_COUNT];
const char* jpt_pad_family_name(jpt_pad_family_t family);

/* The family of a connected controller: the Steam Deck by its USB ids or name, the rest by SDL's type. */
jpt_pad_family_t jpt_pad_family_of(jpt_pad_type_t type, uint16_t vendor, uint16_t product, const char* name);
/* The glyphs to show: the chosen style, or with Automatic the controller in use (Xbox when there is none). */
jpt_pad_family_t jpt_glyph_family(jpt_glyph_style_t style, int have_pad, jpt_pad_family_t active);

/* The short text on a control's glyph ("A", "LB", "L2", "Options"); "" for a PlayStation face button, whose glyph is
 * its symbol, and for generic face buttons, whose glyph shows their position. */
const char* jpt_pad_glyph_text(jpt_pad_family_t family, int control);
/* A control's name in text ("A", "Cross", "Left bumper (LB)"). */
const char* jpt_pad_control_name(jpt_pad_family_t family, int control);

/* The game's actions a gamepad button can take, with the game's own names for them (menu\controls.ddf). */
typedef enum {
    JPT_ACT_JUMP,
    JPT_ACT_CROUCH,
    JPT_ACT_CROUCH_TOGGLE,
    JPT_ACT_USE,
    JPT_ACT_HAND,
    JPT_ACT_GRAB,
    JPT_ACT_THROW,
    JPT_ACT_STOW,
    JPT_ACT_WRIST,
    JPT_ACT_ARM,
    JPT_ACT_COUNT
} jpt_pad_action_t;

/* The game's command bits (CInput's u4ButtonState, the key map's actions). */
#define JPT_CMD_JUMP 0x00000001u
#define JPT_CMD_CROUCH 0x00000002u
#define JPT_CMD_ARM 0x00000008u
#define JPT_CMD_WRIST 0x00000010u
#define JPT_CMD_USE 0x00000020u
#define JPT_CMD_HAND 0x00000040u
#define JPT_CMD_GRAB 0x00000080u
#define JPT_CMD_STOW 0x00000100u
#define JPT_CMD_REPLAY 0x00000200u
#define JPT_CMD_THROW 0x00000400u
#define JPT_CMD_RUN 0x01000000u
#define JPT_CMD_WALK 0x02000000u
#define JPT_CMD_BACKUP 0x04000000u
#define JPT_CMD_LEFT 0x08000000u
#define JPT_CMD_RIGHT 0x10000000u

typedef struct {
    const char* key;   /* trespasser.ini */
    const char* name;
    uint32_t command;
    int default_control;
} jpt_pad_action_info_t;

extern const jpt_pad_action_info_t jpt_pad_actions[JPT_ACT_COUNT];

/* The controls an action may be bound to: Start stays the pause menu, Back is kept for a later map screen, Guide
 * belongs to the system. */
int jpt_pad_bindable(int control);
/* trespasser.ini names of controls ("south", "lefttrigger", "none"). */
const char* jpt_pad_control_key(int control);
int jpt_pad_control_from_key(const char* key);

#define JPT_KEY_GAMEPAD_GLYPHS "gamepad_glyphs"
#define JPT_KEY_GAMEPAD_LOOK_X "gamepad_look_sensitivity_x"
#define JPT_KEY_GAMEPAD_LOOK_Y "gamepad_look_sensitivity_y"
#define JPT_KEY_GAMEPAD_INVERT_Y "gamepad_invert_y"
#define JPT_KEY_GAMEPAD_STICK_DEADZONE "gamepad_stick_deadzone"
#define JPT_KEY_GAMEPAD_TRIGGER_DEADZONE "gamepad_trigger_deadzone"

#define JPT_PAD_LOOK_MIN 25
#define JPT_PAD_LOOK_MAX 300
#define JPT_PAD_STICK_DEADZONE_MIN 5
#define JPT_PAD_STICK_DEADZONE_MAX 40
#define JPT_PAD_TRIGGER_DEADZONE_MIN 5
#define JPT_PAD_TRIGGER_DEADZONE_MAX 50

typedef struct {
    int glyphs;                /* jpt_glyph_style_t */
    int look_x, look_y;        /* percent of the default look speed */
    int invert_y;
    int stick_deadzone;        /* percent of the stick's travel */
    int trigger_deadzone;      /* percent of the trigger's travel */
    int bind[JPT_ACT_COUNT];   /* a control, or JPT_PAD_NONE */
} jpt_gamepad_settings_t;

/* The port's gameplay shortcuts: Hint (the game's F1), Objectives (F3) and Show Objective (the current objective's
 * title for a few seconds). Each works, and is shown, only while no action is bound to its control. */
#define JPT_PAD_HINT_CONTROL JPT_PAD_DPAD_LEFT
#define JPT_PAD_OBJECTIVES_CONTROL JPT_PAD_DPAD_RIGHT
#define JPT_PAD_SHOW_OBJECTIVE_CONTROL JPT_PAD_DPAD_DOWN
int jpt_pad_control_free(const jpt_gamepad_settings_t* s, int control);

#define JPT_GAMEPAD_SETTING_COUNT (6 + JPT_ACT_COUNT)
/* The setting keys and defaults in a fixed order; keys are static strings. */
const char* jpt_gamepad_setting_key(int index);
const char* jpt_gamepad_setting_default(int index);
void jpt_gamepad_defaults(jpt_gamepad_settings_t* s);
/* 1 when key is a gamepad setting and value is valid for it (then applied); 0 otherwise, s unchanged. */
int jpt_gamepad_apply(jpt_gamepad_settings_t* s, const char* key, const char* value);
int jpt_gamepad_is_key(const char* key);

/* The game's key map (registry "KeyMap", the table at 0x0066AE78): 15 {virtual key, command} pairs. */
#define JPT_KEYMAP_COUNT 15
typedef struct {
    uint32_t vk, command;
} jpt_key_binding_t;

void jpt_keymap_default(jpt_key_binding_t out[JPT_KEYMAP_COUNT]);
/* The registry value's bytes; 0 (out set to the defaults) when they are not a key map. */
int jpt_keymap_parse(const uint8_t* data, size_t size, jpt_key_binding_t out[JPT_KEYMAP_COUNT]);
/* The key bound to a command; 0 when none is. */
uint32_t jpt_keymap_key_for(const jpt_key_binding_t* map, int count, uint32_t command);
/* A key's label as prompts show it ("E", "Space", "Ctrl", "Mouse Left"); buf when it has to be built. */
const char* jpt_key_label(uint32_t vk, char* buf, size_t cap);

/* The rows of the game's Controls screen in its order (field 1030 + index, hotspot 1010 + index). */
#define JPT_GAME_CONTROL_COUNT 15
typedef struct {
    uint32_t command;
    const char* name;
} jpt_game_control_t;
extern const jpt_game_control_t jpt_game_controls[JPT_GAME_CONTROL_COUNT];

/* What a prompt for one of the game's commands shows. */
enum { JPT_DIR_NONE, JPT_DIR_UP, JPT_DIR_DOWN, JPT_DIR_LEFT, JPT_DIR_RIGHT };
typedef struct {
    int gamepad;              /* 1: control/dir/tilt; 0: the key */
    uint32_t vk;              /* keyboard: the bound key, 0 for none */
    int control;              /* gamepad: JPT_PAD_*, JPT_PAD_NONE for none */
    int dir;                  /* a stick's direction */
    int tilt;                 /* a stick's travel: 1 half, 2 full */
} jpt_prompt_t;

/* The binding that performs a command for the device: the key from the key map, or the gamepad control from the
 * settings (the left stick for walking and sidestepping). 0 when nothing performs it. */
int jpt_prompt_for_command(uint32_t command, int gamepad, const jpt_gamepad_settings_t* s,
                           const jpt_key_binding_t* keymap, int keymap_count, jpt_prompt_t* out);

#ifdef __cplusplus
}
#endif

#endif
