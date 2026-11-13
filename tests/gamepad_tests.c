/*
 * Headless checks of the gamepad's logic: which device the player used last, shaping, controller families, bindings,
 * prompts and glyphs, gameplay input for the game's CInput and focus movement in menus. No SDL device, no window.
 * Build: CMake target gamepad_tests.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gamepad_policy.h"
#include "jpt_config.h"
#include "jpt_gamepad.h"
#include "jpt_prompt_text.h"
#include "pad_glyphs.h"

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static pad_state_t idle(void) {
    pad_state_t s;
    memset(&s, 0, sizeof s);
    return s;
}

static void test_last_device(void) {
    pad_tracker_t t;
    pad_tracker_init(&t);
    CHECK(t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    CHECK(pad_tracker_gamepad(&t) == 1 && t.device == PAD_DEVICE_GAMEPAD);
    CHECK(pad_tracker_gamepad(&t) == 0);
    /* jitter: a few counts at a time, spread out, never switches */
    for (uint32_t ms = 0; ms < 5000; ms += 400) CHECK(pad_tracker_mouse_motion(&t, 1, -1, ms) == 0);
    CHECK(t.device == PAD_DEVICE_GAMEPAD);
    /* a small nudge inside the window does not switch either */
    CHECK(pad_tracker_mouse_motion(&t, 6, 0, 6000) == 0);
    CHECK(pad_tracker_mouse_motion(&t, 5, 0, 6010) == 0);
    CHECK(t.device == PAD_DEVICE_GAMEPAD);
    /* real movement does */
    CHECK(pad_tracker_mouse_motion(&t, 8, 6, 6020) == 1 && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    CHECK(pad_tracker_gamepad(&t) == 1);
    CHECK(pad_tracker_mouse_button(&t) == 1 && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    CHECK(pad_tracker_gamepad(&t) == 1);
    CHECK(pad_tracker_keyboard(&t) == 1 && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    CHECK(pad_tracker_keyboard(&t) == 0);
    /* disconnect fallback */
    pad_tracker_gamepad(&t);
    CHECK(pad_tracker_pads_left(&t, 1) == 0 && t.device == PAD_DEVICE_GAMEPAD);
    CHECK(pad_tracker_pads_left(&t, 0) == 1 && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    CHECK(t.switches == 8);
}

static void test_activity_filter(void) {
    pad_activity_t a;
    pad_state_t s = idle();
    const float dz = 0.15f, tdz = 0.12f;
    pad_activity_reset(&a);
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    s.buttons = JPT_PAD_BIT(JPT_PAD_SOUTH);
    CHECK(pad_activity_update(&a, &s, dz, tdz));
    CHECK(!pad_activity_update(&a, &s, dz, tdz)); /* held */
    s.buttons |= JPT_PAD_BIT(JPT_PAD_DPAD_UP);
    CHECK(pad_activity_update(&a, &s, dz, tdz));
    s.buttons = 0;
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    /* drift just past the deadzone never counts */
    s.lx = 0.22f;
    s.ry = -0.30f;
    for (int i = 0; i < 50; i++) CHECK(!pad_activity_update(&a, &s, dz, tdz));
    /* a real push counts once, while held it does not */
    s.lx = 0.8f;
    CHECK(pad_activity_update(&a, &s, dz, tdz));
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    s.lx = 0.25f; /* above the deadzone: still on, no new activity */
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    s.lx = 0.0f;
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    s.lx = 0.9f;
    CHECK(pad_activity_update(&a, &s, dz, tdz));
    /* trigger noise */
    s = idle();
    pad_activity_update(&a, &s, dz, tdz);
    s.rt = 0.2f;
    CHECK(!pad_activity_update(&a, &s, dz, tdz));
    s.rt = 0.6f;
    CHECK(pad_activity_update(&a, &s, dz, tdz));
}

static void test_deadzones(void) {
    float x, y;
    pad_stick_shape(0.1f, 0.05f, 0.15f, &x, &y);
    CHECK(x == 0.0f && y == 0.0f);
    pad_stick_shape(1.0f, 0.0f, 0.15f, &x, &y);
    CHECK(near(x, 1.0f, 1e-5f) && y == 0.0f);
    pad_stick_shape(0.575f, 0.0f, 0.15f, &x, &y);
    CHECK(near(x, 0.5f, 1e-4f));
    pad_stick_shape(0.0f, -0.575f, 0.15f, &x, &y);
    CHECK(near(y, -0.5f, 1e-4f));
    /* diagonal: radial, so a corner of a square gate is still at most 1 */
    pad_stick_shape(1.0f, 1.0f, 0.15f, &x, &y);
    CHECK(near(sqrtf(x * x + y * y), 1.0f, 1e-4f) && near(x, y, 1e-6f));
    pad_stick_shape(0.12f, 0.12f, 0.15f, &x, &y); /* each axis inside, the length outside */
    CHECK(x > 0.0f && y > 0.0f);
    CHECK(pad_trigger_shape(0.1f, 0.12f) == 0.0f);
    CHECK(near(pad_trigger_shape(1.0f, 0.12f), 1.0f, 1e-6f));
    CHECK(near(pad_trigger_shape(0.56f, 0.12f), 0.5f, 1e-4f));
    CHECK(pad_look_curve(0.0f) == 0.0f && near(pad_look_curve(1.0f), 1.0f, 1e-6f));
    CHECK(pad_look_curve(0.5f) < 0.5f && pad_look_curve(0.5f) > 0.2f);
    {
        pad_state_t s = idle();
        s.lt = 0.11f;
        s.rt = 0.13f;
        CHECK(pad_buttons_with_triggers(&s, 0.12f) == JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER));
    }
}

static void test_type_detection(void) {
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_XBOX360, 0x045E, 0x028E, "Xbox 360 Controller") == JPT_FAMILY_XBOX);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_XBOXONE, 0x045E, 0x0B13, "Xbox Wireless Controller") == JPT_FAMILY_XBOX);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_PS3, 0x054C, 0x0268, "PS3 Controller") == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_PS4, 0x054C, 0x09CC, "PS4 Controller") == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_PS5, 0x054C, 0x0CE6, "DualSense Wireless Controller") == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_SWITCH_PRO, 0x057E, 0x2009, "Nintendo Switch Pro Controller") == JPT_FAMILY_NINTENDO);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_JOYCON_PAIR, 0x057E, 0x2008, "Joy-Con (L/R)") == JPT_FAMILY_NINTENDO);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_STANDARD, 0x28DE, 0x1205, "Steam Deck") == JPT_FAMILY_STEAM_DECK);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_XBOX360, 0x28DE, 0x11FF, "Steam Deck Controller") == JPT_FAMILY_STEAM_DECK);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_XBOX360, 0x28DE, 0x11FF, "Steam Virtual Gamepad") == JPT_FAMILY_XBOX);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_STANDARD, 0x0079, 0x0006, "Generic USB Joystick") == JPT_FAMILY_GENERIC);
    CHECK(jpt_pad_family_of(JPT_PAD_TYPE_UNKNOWN, 0, 0, NULL) == JPT_FAMILY_GENERIC);
}

static void test_glyph_selection(void) {
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 1, JPT_FAMILY_PLAYSTATION) == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 1, JPT_FAMILY_STEAM_DECK) == JPT_FAMILY_STEAM_DECK);
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 1, JPT_FAMILY_NINTENDO) == JPT_FAMILY_NINTENDO);
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 0, JPT_FAMILY_PLAYSTATION) == JPT_FAMILY_XBOX);
    /* a manual style wins over the controller in use */
    CHECK(jpt_glyph_family(JPT_GLYPHS_XBOX, 1, JPT_FAMILY_PLAYSTATION) == JPT_FAMILY_XBOX);
    CHECK(jpt_glyph_family(JPT_GLYPHS_PLAYSTATION, 1, JPT_FAMILY_XBOX) == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_glyph_family(JPT_GLYPHS_STEAM_DECK, 1, JPT_FAMILY_XBOX) == JPT_FAMILY_STEAM_DECK);
    CHECK(jpt_glyph_family(JPT_GLYPHS_GENERIC, 1, JPT_FAMILY_XBOX) == JPT_FAMILY_GENERIC);
}

static void test_family_mappings(void) {
    /* Xbox */
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_SOUTH), "A"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_NORTH), "Y"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_LEFT_SHOULDER), "LB"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_RIGHT_TRIGGER), "RT"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_BACK), "View"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_START), "Menu"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_XBOX, JPT_PAD_LEFT_STICK), "LS"));
    /* PlayStation */
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_PLAYSTATION, JPT_PAD_SOUTH), "Cross"));
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_PLAYSTATION, JPT_PAD_EAST), "Circle"));
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_PLAYSTATION, JPT_PAD_WEST), "Square"));
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_PLAYSTATION, JPT_PAD_NORTH), "Triangle"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_PLAYSTATION, JPT_PAD_LEFT_SHOULDER), "L1"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_PLAYSTATION, JPT_PAD_RIGHT_TRIGGER), "R2"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_PLAYSTATION, JPT_PAD_BACK), "Create"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_PLAYSTATION, JPT_PAD_START), "Options"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_PLAYSTATION, JPT_PAD_RIGHT_STICK), "R3"));
    /* Steam Deck */
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_SOUTH), "A"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_RIGHT_SHOULDER), "R1"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_LEFT_TRIGGER), "L2"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_BACK), "View"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_START), "Menu"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_STEAM_DECK, JPT_PAD_LEFT_STICK), "L3"));
    /* Nintendo: by position, the bottom button is B */
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_NINTENDO, JPT_PAD_SOUTH), "B"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_NINTENDO, JPT_PAD_EAST), "A"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_NINTENDO, JPT_PAD_RIGHT_TRIGGER), "ZR"));
    /* Generic */
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_GENERIC, JPT_PAD_SOUTH), "Bottom button"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_GENERIC, JPT_PAD_LEFT_SHOULDER), "L1"));
    CHECK(!strcmp(jpt_pad_glyph_text(JPT_FAMILY_GENERIC, JPT_PAD_START), "Start"));
    CHECK(!strcmp(jpt_pad_control_name(JPT_FAMILY_XBOX, JPT_PAD_DPAD_LEFT), "D-pad left"));
    for (int f = 0; f < JPT_FAMILY_COUNT; f++)
        for (int c = 0; c <= JPT_PAD_RIGHT_STICK_MOVE; c++) CHECK(jpt_pad_control_name((jpt_pad_family_t)f, c) != NULL);
}

static void test_settings(void) {
    jpt_gamepad_settings_t s;
    jpt_config_t c;
    jpt_gamepad_defaults(&s);
    CHECK(s.glyphs == JPT_GLYPHS_AUTOMATIC && s.look_x == 100 && s.look_y == 100 && !s.invert_y);
    CHECK(s.stick_deadzone == 15 && s.trigger_deadzone == 12);
    CHECK(s.bind[JPT_ACT_JUMP] == JPT_PAD_SOUTH && s.bind[JPT_ACT_CROUCH] == JPT_PAD_EAST);
    CHECK(s.bind[JPT_ACT_HAND] == JPT_PAD_LEFT_TRIGGER && s.bind[JPT_ACT_USE] == JPT_PAD_RIGHT_TRIGGER);
    CHECK(s.bind[JPT_ACT_GRAB] == JPT_PAD_WEST && s.bind[JPT_ACT_THROW] == JPT_PAD_NORTH);
    CHECK(s.bind[JPT_ACT_WRIST] == JPT_PAD_LEFT_SHOULDER && s.bind[JPT_ACT_ARM] == JPT_PAD_RIGHT_SHOULDER);
    CHECK(s.bind[JPT_ACT_STOW] == JPT_PAD_DPAD_UP && s.bind[JPT_ACT_REPLAY] == JPT_PAD_DPAD_DOWN);
    CHECK(s.bind[JPT_ACT_CROUCH_TOGGLE] == JPT_PAD_LEFT_STICK);
    CHECK(jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_GLYPHS, "PlayStation") && s.glyphs == JPT_GLYPHS_PLAYSTATION);
    CHECK(!jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_GLYPHS, "nintendo") && s.glyphs == JPT_GLYPHS_PLAYSTATION);
    CHECK(jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_LOOK_X, "250") && s.look_x == 250);
    CHECK(!jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_LOOK_X, "301") && !jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_LOOK_Y, "x"));
    CHECK(jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_INVERT_Y, "on") && s.invert_y == 1);
    CHECK(!jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_STICK_DEADZONE, "4") && !jpt_gamepad_apply(&s, JPT_KEY_GAMEPAD_STICK_DEADZONE, "41"));
    CHECK(jpt_gamepad_apply(&s, "gamepad_bind_jump", "north") && s.bind[JPT_ACT_JUMP] == JPT_PAD_NORTH);
    CHECK(jpt_gamepad_apply(&s, "gamepad_bind_jump", "none") && s.bind[JPT_ACT_JUMP] == JPT_PAD_NONE);
    CHECK(!jpt_gamepad_apply(&s, "gamepad_bind_jump", "start")); /* the pause menu's */
    CHECK(!jpt_gamepad_apply(&s, "gamepad_bind_jump", "back"));  /* kept free */
    CHECK(!jpt_gamepad_apply(&s, "gamepad_bind_jump", "guide"));
    CHECK(!jpt_gamepad_apply(&s, "gamepad_bind_nothing", "south"));
    CHECK(jpt_pad_control_from_key("LeftTrigger") == JPT_PAD_LEFT_TRIGGER && jpt_pad_control_from_key("x") == -1);
    /* the shared settings table carries every gamepad key with the same default */
    for (int i = 0; i < JPT_GAMEPAD_SETTING_COUNT; i++) {
        const char* key = jpt_gamepad_setting_key(i);
        CHECK(key && jpt_default_value(key) && !strcmp(jpt_default_value(key), jpt_gamepad_setting_default(i)));
        CHECK(jpt_config_valid(key, jpt_gamepad_setting_default(i)));
    }
    jpt_config_defaults(&c);
    jpt_gamepad_defaults(&s);
    CHECK(!memcmp(&c.gamepad, &s, sizeof s));
    CHECK(jpt_config_apply(&c, "gamepad_bind_use", "west") && c.gamepad.bind[JPT_ACT_USE] == JPT_PAD_WEST);
    CHECK(!jpt_config_apply(&c, JPT_KEY_GAMEPAD_TRIGGER_DEADZONE, "70"));
}

static void test_keyboard_prompts(void) {
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_t p;
    char buf[24];
    uint8_t blob[JPT_KEYMAP_COUNT * 8];
    jpt_keymap_default(map);
    CHECK(jpt_prompt_for_command(JPT_CMD_USE, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && p.vk == 0x20);
    CHECK(!strcmp(jpt_key_label(p.vk, buf, sizeof buf), "Space"));
    CHECK(jpt_prompt_for_command(JPT_CMD_JUMP, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && p.vk == 'Q');
    CHECK(!strcmp(jpt_key_label(p.vk, buf, sizeof buf), "Q"));
    CHECK(jpt_prompt_for_command(JPT_CMD_HAND, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && !strcmp(jpt_key_label(p.vk, buf, sizeof buf), "Mouse Left"));
    CHECK(jpt_prompt_for_command(JPT_CMD_GRAB, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && !strcmp(jpt_key_label(p.vk, buf, sizeof buf), "Mouse Right"));
    CHECK(jpt_prompt_for_command(JPT_CMD_ARM, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && !strcmp(jpt_key_label(p.vk, buf, sizeof buf), "Ctrl"));
    CHECK(jpt_prompt_for_command(JPT_CMD_STOW, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && !strcmp(jpt_key_label(p.vk, buf, sizeof buf), "E"));
    /* the registry's KeyMap as the game writes it, with Jump remapped to Space and Use/Fire left without a key */
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++) {
        uint32_t vk = map[i].vk, cmd = map[i].command;
        if (cmd == JPT_CMD_JUMP) vk = 0x20;
        if (cmd == JPT_CMD_USE) vk = 0;
        memcpy(blob + 8 * i, &vk, 4);
        memcpy(blob + 8 * i + 4, &cmd, 4);
    }
    CHECK(jpt_keymap_parse(blob, sizeof blob, map));
    CHECK(jpt_prompt_for_command(JPT_CMD_JUMP, 0, NULL, map, JPT_KEYMAP_COUNT, &p) && p.vk == 0x20);
    CHECK(!jpt_prompt_for_command(JPT_CMD_USE, 0, NULL, map, JPT_KEYMAP_COUNT, &p));
    CHECK(!jpt_keymap_parse(blob, sizeof blob - 1, map) && map[4].vk == 0x20 && map[4].command == JPT_CMD_USE);
    CHECK(!strcmp(jpt_key_label(0x70, buf, sizeof buf), "F1") && !strcmp(jpt_key_label(0x63, buf, sizeof buf), "Num 3"));
    CHECK(!strcmp(jpt_key_label(0xA0, buf, sizeof buf), "Left Shift") && !strcmp(jpt_key_label(0, buf, sizeof buf), "None"));
    CHECK(!strcmp(jpt_key_label(0xE7, buf, sizeof buf), "Key E7"));
}

static void test_gamepad_prompts(void) {
    jpt_gamepad_settings_t s;
    jpt_prompt_t p;
    pad_glyph_t g;
    jpt_gamepad_defaults(&s);
    CHECK(jpt_prompt_for_command(JPT_CMD_JUMP, 1, &s, NULL, 0, &p) && p.gamepad && p.control == JPT_PAD_SOUTH);
    CHECK(pad_glyph_control(JPT_FAMILY_XBOX, p.control, p.dir, p.tilt, &g) && g.shape == PAD_GLYPH_FACE && !strcmp(g.text, "A"));
    CHECK(pad_glyph_control(JPT_FAMILY_PLAYSTATION, p.control, p.dir, p.tilt, &g) && g.symbol == PAD_SYMBOL_CROSS && !g.text[0]);
    CHECK(pad_glyph_control(JPT_FAMILY_STEAM_DECK, p.control, p.dir, p.tilt, &g) && !strcmp(g.text, "A"));
    CHECK(pad_glyph_control(JPT_FAMILY_GENERIC, p.control, p.dir, p.tilt, &g) && g.shape == PAD_GLYPH_DIAMOND && g.dir == 0);
    /* remapped: the prompt follows the binding, not a fixed letter */
    s.bind[JPT_ACT_JUMP] = JPT_PAD_NORTH;
    CHECK(jpt_prompt_for_command(JPT_CMD_JUMP, 1, &s, NULL, 0, &p) && p.control == JPT_PAD_NORTH);
    CHECK(pad_glyph_control(JPT_FAMILY_XBOX, p.control, 0, 0, &g) && !strcmp(g.text, "Y"));
    CHECK(pad_glyph_control(JPT_FAMILY_PLAYSTATION, p.control, 0, 0, &g) && g.symbol == PAD_SYMBOL_TRIANGLE);
    s.bind[JPT_ACT_JUMP] = JPT_PAD_NONE;
    CHECK(!jpt_prompt_for_command(JPT_CMD_JUMP, 1, &s, NULL, 0, &p));
    /* crouch: the held button, not the toggle */
    CHECK(jpt_prompt_for_command(JPT_CMD_CROUCH, 1, &s, NULL, 0, &p) && p.control == JPT_PAD_EAST);
    CHECK(jpt_prompt_for_command(JPT_CMD_HAND, 1, &s, NULL, 0, &p) && p.control == JPT_PAD_LEFT_TRIGGER);
    CHECK(pad_glyph_control(JPT_FAMILY_PLAYSTATION, p.control, 0, 0, &g) && g.shape == PAD_GLYPH_TRIGGER && !strcmp(g.text, "L2"));
    /* walking is the left stick */
    CHECK(jpt_prompt_for_command(JPT_CMD_WALK, 1, &s, NULL, 0, &p) && p.control == JPT_PAD_LEFT_STICK_MOVE && p.dir == JPT_DIR_UP && p.tilt == 1);
    CHECK(jpt_prompt_for_command(JPT_CMD_RUN, 1, &s, NULL, 0, &p) && p.tilt == 2);
    CHECK(jpt_prompt_for_command(JPT_CMD_BACKUP, 1, &s, NULL, 0, &p) && p.dir == JPT_DIR_DOWN);
    CHECK(jpt_prompt_for_command(JPT_CMD_LEFT, 1, &s, NULL, 0, &p) && p.dir == JPT_DIR_LEFT);
    CHECK(pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_LEFT_STICK_MOVE, JPT_DIR_UP, 2, &g) && g.shape == PAD_GLYPH_STICK &&
          near(g.text_y, 0.35f, 1e-4f));
    CHECK(pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_LEFT_STICK_MOVE, JPT_DIR_UP, 1, &g) && near(g.text_y, 0.425f, 1e-4f));
    CHECK(pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_DPAD_UP, 0, 0, &g) && g.shape == PAD_GLYPH_DPAD && g.dir == JPT_DIR_UP);
    CHECK(!pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_NONE, 0, 0, &g));
    CHECK(pad_glyph_key(0x01, "Mouse 1", &g) && g.shape == PAD_GLYPH_MOUSE && g.dir == 1);
    CHECK(pad_glyph_key(0x45, "E", &g) && g.shape == PAD_GLYPH_KEY && !strcmp(g.text, "E"));
}

static void test_glyph_shapes(void) {
    pad_glyph_t g;
    const float aa = 0.02f;
    pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_SOUTH, 0, 0, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_BODY, 0.5f, 0.5f, aa) == 1.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_BODY, 0.02f, 0.02f, aa) == 0.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_RIM, 0.5f, 0.5f, aa) == 0.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_RIM, 0.5f, 0.07f, aa) > 0.9f);
    pad_glyph_control(JPT_FAMILY_PLAYSTATION, JPT_PAD_SOUTH, 0, 0, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.5f, 0.5f, aa) == 1.0f);   /* the cross's centre */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.5f, 0.36f, aa) == 0.0f);  /* between its arms */
    pad_glyph_control(JPT_FAMILY_PLAYSTATION, JPT_PAD_EAST, 0, 0, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.5f, 0.5f, aa) == 0.0f);   /* a ring */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.67f, 0.5f, aa) == 1.0f);
    pad_glyph_control(JPT_FAMILY_GENERIC, JPT_PAD_NORTH, 0, 0, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.5f, 0.28f, aa) == 1.0f);  /* the top dot lit */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.5f, 0.72f, aa) == 0.0f);
    pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_DPAD_RIGHT, 0, 0, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, 0.77f, 0.5f, aa) == 1.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, 0.23f, 0.5f, aa) == 0.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_BODY, 0.1f, 0.1f, aa) == 0.0f);   /* outside the cross */
    pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_LEFT_STICK_MOVE, JPT_DIR_RIGHT, 2, &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, 0.80f, 0.5f, aa) == 1.0f);  /* the cap moved right */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, 0.30f, 0.5f, aa) == 0.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.94f, 0.5f, aa) > 0.5f);     /* the rim lit on the right */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_MARK, 0.06f, 0.5f, aa) == 0.0f);
    pad_glyph_key(0x02, "Mouse 2", &g);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, g.width * 0.65f, 0.25f, aa) > 0.9f); /* the right button lit */
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, g.width * 0.35f, 0.25f, aa) == 0.0f);
    pad_glyph_control(JPT_FAMILY_XBOX, JPT_PAD_LEFT_SHOULDER, 0, 0, &g);
    CHECK(g.width > 1.2f && pad_glyph_coverage(&g, PAD_LAYER_BODY, g.width * 0.5f, 0.5f, aa) == 1.0f);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_BODY, g.width * 0.5f, 0.05f, aa) == 0.0f);
}

static void test_roster(void) {
    pad_roster_t r;
    pad_roster_init(&r);
    CHECK(r.active == -1 && pad_roster_count(&r) == 0);
    /* start without a controller, connect one */
    CHECK(pad_roster_add(&r, 7, JPT_FAMILY_XBOX) == 0 && r.active == 0);
    CHECK(pad_roster_add(&r, 7, JPT_FAMILY_XBOX) == -1);
    /* a second one waits until it is used */
    CHECK(pad_roster_add(&r, 9, JPT_FAMILY_PLAYSTATION) == 1 && r.active == 0);
    CHECK(pad_roster_activity(&r, 1) == 1 && r.active == 1 && r.slots[r.active].family == JPT_FAMILY_PLAYSTATION);
    CHECK(pad_roster_activity(&r, 1) == 0);
    /* the one in use goes away: the other takes over */
    CHECK(pad_roster_remove(&r, 9) == 1 && r.active == 0 && r.slots[0].id == 7);
    CHECK(pad_roster_remove(&r, 12345) == 0);
    /* reconnect: Xbox swapped for PlayStation */
    CHECK(pad_roster_remove(&r, 7) == 1 && r.active == -1 && pad_roster_count(&r) == 0);
    CHECK(pad_roster_add(&r, 11, JPT_FAMILY_PLAYSTATION) >= 0 && r.slots[r.active].family == JPT_FAMILY_PLAYSTATION);
    /* the latest used of several is chosen when the active one leaves */
    pad_roster_add(&r, 20, JPT_FAMILY_XBOX);
    pad_roster_add(&r, 21, JPT_FAMILY_GENERIC);
    pad_roster_activity(&r, pad_roster_find(&r, 21));
    pad_roster_activity(&r, pad_roster_find(&r, 20));
    CHECK(r.slots[r.active].id == 20);
    pad_roster_remove(&r, 20);
    CHECK(r.slots[r.active].id == 21);
    for (uint32_t id = 100; id < 100 + PAD_ROSTER_MAX; id++) pad_roster_add(&r, id, JPT_FAMILY_GENERIC);
    CHECK(pad_roster_count(&r) == PAD_ROSTER_MAX && pad_roster_add(&r, 999, JPT_FAMILY_XBOX) == -1);
}

static void test_gameplay(void) {
    jpt_gamepad_settings_t cfg;
    pad_state_t s = idle();
    pad_gameplay_t g;
    pad_sinput_t in;
    jpt_gamepad_defaults(&cfg);
    /* full forward is the keyboard's run, half is its walk */
    s.ly = -1.0f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(near(g.move_y, 1.0f, 1e-5f) && g.move_x == 0.0f);
    s.ly = -0.575f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(near(g.move_y, 0.5f, 1e-3f));
    s.ly = 1.0f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(near(g.move_y, -0.4f, 1e-5f));
    s.ly = 0.0f;
    s.lx = -1.0f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(near(g.move_x, -0.6f, 1e-5f));
    s.lx = 0.7071f;
    s.ly = -0.7071f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(g.move_x > 0.4f && g.move_y > 0.69f && sqrtf(g.move_x * g.move_x + g.move_y * g.move_y) <= 1.0f);
    /* drift inside the deadzone moves nothing */
    s.lx = 0.1f;
    s.ly = -0.1f;
    s.rx = 0.12f;
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 0, &g);
    CHECK(g.move_x == 0.0f && g.move_y == 0.0f && g.look_x == 0.0f && g.look_y == 0.0f);
    /* look: radians per second, scaled by the frame; up is +y unless inverted */
    s = idle();
    s.rx = 1.0f;
    pad_gameplay_from(&s, 0, &cfg, 0.5f, 0, &g);
    CHECK(near(g.look_x, PAD_MAX_FRAME_SECONDS * PAD_LOOK_YAW_RATE, 1e-4f)); /* a long frame is clamped */
    pad_gameplay_from(&s, 0, &cfg, 0.05f, 0, &g);
    CHECK(near(g.look_x, 0.05f * PAD_LOOK_YAW_RATE, 1e-4f));
    cfg.look_x = 200;
    pad_gameplay_from(&s, 0, &cfg, 0.05f, 0, &g);
    CHECK(near(g.look_x, 0.1f * PAD_LOOK_YAW_RATE, 1e-4f));
    s = idle();
    s.ry = -1.0f;
    pad_gameplay_from(&s, 0, &cfg, 0.05f, 0, &g);
    CHECK(g.look_y > 0.0f);
    cfg.invert_y = 1;
    pad_gameplay_from(&s, 0, &cfg, 0.05f, 0, &g);
    CHECK(g.look_y < 0.0f);
    cfg.look_y = 50;
    pad_gameplay_from(&s, 0, &cfg, 0.05f, 0, &g);
    CHECK(near(g.look_y, -0.025f * PAD_LOOK_PITCH_RATE, 1e-4f));
    /* buttons: the actions' command bits */
    jpt_gamepad_defaults(&cfg);
    s = idle();
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_SOUTH) | JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER) | JPT_PAD_BIT(JPT_PAD_WEST), &cfg, 0.016f,
                      0, &g);
    CHECK(g.held == (JPT_CMD_JUMP | JPT_CMD_HAND | JPT_CMD_GRAB));
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_LEFT_STICK), &cfg, 0.016f, 0, &g);
    CHECK(g.held == 0); /* the toggle is the caller's state */
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 1, &g);
    CHECK(g.held == JPT_CMD_CROUCH);
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER) | JPT_PAD_BIT(JPT_PAD_NORTH)) == (JPT_CMD_USE | JPT_CMD_THROW));
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER) | JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER)) ==
          (JPT_CMD_WRIST | JPT_CMD_ARM));
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_START) | JPT_PAD_BIT(JPT_PAD_BACK)) == 0);
    /* the game turns the wrist only with the hand up: LB/RB raise it */
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER), &cfg, 0.016f, 0, &g);
    CHECK(g.held == (JPT_CMD_WRIST | JPT_CMD_HAND));
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER), &cfg, 0.016f, 0, &g);
    CHECK(g.held == (JPT_CMD_ARM | JPT_CMD_HAND));
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_EAST), &cfg, 0.016f, 1, &g);
    CHECK(g.held == JPT_CMD_CROUCH);
    /* merged with the keyboard and mouse */
    memset(&in, 0, sizeof in);
    in.rotate_x = 0.01f;
    in.move_y = 1.0f;
    in.state = JPT_CMD_USE;
    memset(&g, 0, sizeof g);
    g.move_y = 0.5f;
    g.move_x = 0.5f;
    g.look_x = 0.02f;
    g.held = JPT_CMD_JUMP;
    pad_merge_sinput(&in, &g, JPT_CMD_JUMP);
    CHECK(near(in.move_y, 1.0f, 1e-6f) && near(in.move_x, 0.5f, 1e-6f) && near(in.rotate_x, 0.03f, 1e-6f));
    CHECK(in.state == (JPT_CMD_USE | JPT_CMD_JUMP) && in.hit == JPT_CMD_JUMP);
    /* while Move Hand is held the stick moves the hand more finely; the mouse is untouched */
    memset(&in, 0, sizeof in);
    in.rotate_x = 0.01f;
    g.held = JPT_CMD_HAND;
    g.move_x = g.move_y = 0;
    pad_merge_sinput(&in, &g, 0);
    CHECK(near(in.rotate_x, 0.01f + 0.02f * PAD_HAND_LOOK_SCALE, 1e-6f));
    memset(&in, 0, sizeof in);
    in.move_y = -0.4f;
    g.held = 0;
    g.move_y = -0.4f;
    pad_merge_sinput(&in, &g, 0);
    CHECK(near(in.move_y, -0.4f, 1e-6f));
}

static void test_edges(void) {
    pad_edges_t e;
    uint32_t pressed, held;
    const uint32_t a = JPT_PAD_BIT(JPT_PAD_SOUTH), b = JPT_PAD_BIT(JPT_PAD_EAST);
    /* the press that closed a menu is held when gameplay resumes: ignored until released */
    pad_edges_reset(&e, a);
    held = pad_edges_update(&e, a, &pressed);
    CHECK(held == 0 && pressed == 0);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == b && pressed == b);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == b && pressed == 0);
    held = pad_edges_update(&e, b, &pressed);
    CHECK(held == b && pressed == 0);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == (a | b) && pressed == a);
    pad_edges_reset(&e, 0);
    pad_edges_update(&e, a, &pressed);
    CHECK(pressed == a);
}

static void test_navigation(void) {
    /* a column of buttons, a row of three at the bottom */
    const pad_rect_t r[] = {
        { 200, 70, 400, 150 }, { 200, 160, 400, 240 }, { 200, 250, 400, 330 },
        { 20, 400, 120, 440 }, { 260, 400, 360, 440 }, { 500, 400, 600, 440 },
    };
    const int n = (int)(sizeof r / sizeof r[0]);
    CHECK(pad_nav_pick(r, n, r[0], PAD_NAV_DOWN) == 1);
    CHECK(pad_nav_pick(r, n, r[1], PAD_NAV_DOWN) == 2);
    CHECK(pad_nav_pick(r, n, r[2], PAD_NAV_DOWN) == 4); /* straight below wins over the corners */
    CHECK(pad_nav_pick(r, n, r[1], PAD_NAV_UP) == 0);
    CHECK(pad_nav_pick(r, n, r[0], PAD_NAV_UP) == -1);
    CHECK(pad_nav_pick(r, n, r[4], PAD_NAV_LEFT) == 3);
    CHECK(pad_nav_pick(r, n, r[4], PAD_NAV_RIGHT) == 5);
    CHECK(pad_nav_pick(r, n, r[5], PAD_NAV_RIGHT) == -1);
    CHECK(pad_nav_pick(r, n, r[3], PAD_NAV_UP) == 2);
    CHECK(pad_nav_pick(r, 0, r[0], PAD_NAV_DOWN) == -1);
    /* list rows next to a column of buttons: down stays in the rows */
    {
        const pad_rect_t l[] = { { 125, 41, 280, 56 }, { 125, 56, 280, 71 }, { 125, 71, 280, 86 }, { 15, 159, 77, 201 } };
        CHECK(pad_nav_pick(l, 4, l[0], PAD_NAV_DOWN) == 1);
        CHECK(pad_nav_pick(l, 4, l[2], PAD_NAV_DOWN) == 3);
    }
    CHECK(pad_nav_direction(JPT_PAD_BIT(JPT_PAD_DPAD_LEFT), 0.9f, 0.0f, PAD_NAV_NONE) == PAD_NAV_LEFT);
    CHECK(pad_nav_direction(0, 0.3f, 0.2f, PAD_NAV_NONE) == PAD_NAV_NONE);
    CHECK(pad_nav_direction(0, 0.2f, 0.8f, PAD_NAV_NONE) == PAD_NAV_DOWN);
    CHECK(pad_nav_direction(0, 0.2f, 0.4f, PAD_NAV_DOWN) == PAD_NAV_DOWN); /* hysteresis */
    CHECK(pad_nav_direction(0, 0.2f, 0.3f, PAD_NAV_DOWN) == PAD_NAV_NONE);
    CHECK(pad_nav_direction(0, -0.9f, 0.3f, PAD_NAV_NONE) == PAD_NAV_LEFT);
    {
        pad_repeat_t rep;
        pad_repeat_reset(&rep);
        CHECK(pad_repeat_update(&rep, PAD_NAV_DOWN, 1000) == PAD_NAV_DOWN);
        CHECK(pad_repeat_update(&rep, PAD_NAV_DOWN, 1000 + PAD_REPEAT_DELAY_MS - 1) == PAD_NAV_NONE);
        CHECK(pad_repeat_update(&rep, PAD_NAV_DOWN, 1000 + PAD_REPEAT_DELAY_MS) == PAD_NAV_DOWN);
        CHECK(pad_repeat_update(&rep, PAD_NAV_DOWN, 1000 + PAD_REPEAT_DELAY_MS + PAD_REPEAT_MS - 1) == PAD_NAV_NONE);
        CHECK(pad_repeat_update(&rep, PAD_NAV_DOWN, 1000 + PAD_REPEAT_DELAY_MS + PAD_REPEAT_MS) == PAD_NAV_DOWN);
        CHECK(pad_repeat_update(&rep, PAD_NAV_UP, 2000) == PAD_NAV_UP);
        CHECK(pad_repeat_update(&rep, PAD_NAV_NONE, 2001) == PAD_NAV_NONE);
        CHECK(pad_repeat_update(&rep, PAD_NAV_UP, 2002) == PAD_NAV_UP);
    }
}

/* Hotplug as the host runs it: tracker and roster together, never leaving input stuck on a missing controller. */
static void test_hotplug_flow(void) {
    pad_roster_t r;
    pad_tracker_t t;
    pad_roster_init(&r);
    pad_tracker_init(&t);
    CHECK(pad_roster_count(&r) == 0 && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    pad_roster_add(&r, 1, JPT_FAMILY_XBOX);
    CHECK(t.device == PAD_DEVICE_KEYBOARD_MOUSE); /* connecting is not using */
    pad_tracker_gamepad(&t);
    pad_roster_remove(&r, 1);
    pad_tracker_pads_left(&t, pad_roster_count(&r));
    CHECK(t.device == PAD_DEVICE_KEYBOARD_MOUSE && r.active == -1);
    pad_roster_add(&r, 2, JPT_FAMILY_PLAYSTATION);
    pad_tracker_gamepad(&t);
    CHECK(t.device == PAD_DEVICE_GAMEPAD && r.slots[r.active].family == JPT_FAMILY_PLAYSTATION);
    pad_tracker_keyboard(&t);
    CHECK(t.device == PAD_DEVICE_KEYBOARD_MOUSE && pad_roster_count(&r) == 1);
}

/* The tutorial's subtitles as Stream.tpa holds them, one string per line. */
static const char* const k_tutorial[] = {
    "To move your arm...",
    "...hold the left mouse button, and move the mouse.",
    "To back up, press 'X'.",
    "Press the right mouse button again to drop the item.",
    "To fire a gun, press the space bar.",
    "Press 'Q' to jump, and 'Z' to crouch.",
    "You can look around by moving the mouse.",
    "To pick up an item...",
    "...press 'Z' to crouch, move your hand  towards the item,",
    " then press the right mouse button to pick it up.",
    "Press 'A' and 'D' to step left and right.",
    "To hit something with an object...",
    "...hold the left mouse button and space bar,",
    "and move the mouse to swing the object.",
    "To throw an object...",
    "...hold the right mouse button,",
    "move the mouse in the direction you want to throw,",
    "then release the right mouse button.",
    "Press the 'S' key to walk slowly.",
    "Press 'W' to run.",
};

static jpt_prompt_context_t prompt_context(int gamepad, jpt_pad_family_t family, const jpt_gamepad_settings_t* cfg,
                                           const jpt_key_binding_t* map) {
    jpt_prompt_context_t c;
    memset(&c, 0, sizeof c);
    c.gamepad = gamepad;
    c.family = family;
    c.settings = cfg;
    c.keymap = map;
    c.keymap_count = JPT_KEYMAP_COUNT;
    return c;
}

static int plain_is(const char* text, const jpt_prompt_context_t* c, const char* want) {
    char out[512];
    if (jpt_prompt_text_plain(text, c, out, sizeof out) < 0) return 0;
    if (strcmp(out, want)) printf("  got \"%s\"\n  want \"%s\"\n", out, want);
    return !strcmp(out, want);
}

static void test_tutorial_text(void) {
    jpt_gamepad_settings_t cfg;
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_context_t kb, xbox, ps, deck, generic;
    const char* jump = "Press 'Q' to jump, and 'Z' to crouch.";
    char out[512];
    jpt_gamepad_defaults(&cfg);
    jpt_keymap_default(map);
    kb = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    xbox = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
    ps = prompt_context(1, JPT_FAMILY_PLAYSTATION, &cfg, map);
    deck = prompt_context(1, JPT_FAMILY_STEAM_DECK, &cfg, map);
    generic = prompt_context(1, JPT_FAMILY_GENERIC, &cfg, map);
    /* token -> keyboard keycap, Xbox, PlayStation, Steam Deck, generic */
    CHECK(plain_is(jump, &kb, "Press [Q] to jump, and [Z] to crouch."));
    CHECK(plain_is(jump, &xbox, "Press [A] to jump, and [B] to crouch."));
    CHECK(plain_is(jump, &ps, "Press [Cross] to jump, and [Circle] to crouch."));
    CHECK(plain_is(jump, &deck, "Press [A] to jump, and [B] to crouch."));
    CHECK(plain_is(jump, &generic, "Press [Bottom button] to jump, and [Right button] to crouch."));
    CHECK(plain_is("...hold the left mouse button, and move the mouse.", &kb,
                   "...hold [Mouse Left], and move the mouse."));
    CHECK(plain_is("...hold the left mouse button, and move the mouse.", &xbox,
                   "...hold [Left trigger (LT)], and move [Right stick]."));
    CHECK(plain_is("...hold the left mouse button and space bar,", &ps, "...hold [L2] and [R2],"));
    CHECK(plain_is("You can look around by moving the mouse.", &xbox, "You can look around with [Right stick]."));
    CHECK(plain_is("...press 'Z' to crouch, move your hand  towards the item,", &xbox,
                   "...hold [B] to crouch, move your hand  towards the item,"));
    CHECK(plain_is(" then press the right mouse button to pick it up.", &ps,
                   " then press [Square] to pick it up."));
    CHECK(plain_is("Press 'W' to run.", &xbox, "Push [Left stick] all the way to run."));
    CHECK(plain_is("Press 'A' and 'D' to step left and right.", &kb, "Press [A] and [D] to step left and right."));
    CHECK(plain_is("To fire a gun, press the space bar.", &kb, "To fire a gun, press [Space]."));
    /* no key or mouse words remain for a gamepad, in any line */
    for (size_t i = 0; i < sizeof k_tutorial / sizeof k_tutorial[0]; i++) {
        int has = jpt_prompt_text_has_prompts(k_tutorial[i]);
        if (!has) continue;
        CHECK(jpt_prompt_text_plain(k_tutorial[i], &xbox, out, sizeof out) > 0);
        CHECK(!strstr(out, "mouse") && !strstr(out, "'") && !strstr(out, "space bar"));
    }
    CHECK(!jpt_prompt_text_has_prompts("To move your arm..."));
    CHECK(!jpt_prompt_text_has_prompts("Keyboards rattled into the early morning."));
    CHECK(jpt_prompt_text_plain("Keyboards rattled into the early morning.", &xbox, out, sizeof out) < 0);
    /* the keyboard keeps a line with nothing bound to a key */
    CHECK(jpt_prompt_text_plain("You can look around by moving the mouse.", &kb, out, sizeof out) < 0);
}

static void test_tutorial_bindings(void) {
    jpt_gamepad_settings_t cfg;
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_context_t c;
    const char* jump = "Press 'Q' to jump, and 'Z' to crouch.";
    jpt_gamepad_defaults(&cfg);
    jpt_keymap_default(map);
    /* the device in use switches the same line back and forth */
    c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    CHECK(plain_is(jump, &c, "Press [Q] to jump, and [Z] to crouch."));
    c.gamepad = 1;
    CHECK(plain_is(jump, &c, "Press [A] to jump, and [B] to crouch."));
    c.gamepad = 0;
    CHECK(plain_is(jump, &c, "Press [Q] to jump, and [Z] to crouch."));
    c.gamepad = 1;
    c.family = JPT_FAMILY_PLAYSTATION;
    CHECK(plain_is(jump, &c, "Press [Cross] to jump, and [Circle] to crouch."));
    /* a remapped key follows */
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++)
        if (map[i].command == JPT_CMD_JUMP) map[i].vk = 'J';
    c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    CHECK(plain_is(jump, &c, "Press [J] to jump, and [Z] to crouch."));
    /* a remapped gamepad button follows */
    cfg.bind[JPT_ACT_JUMP] = JPT_PAD_NORTH;
    c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
    CHECK(plain_is(jump, &c, "Press [Y] to jump, and [B] to crouch."));
    cfg.bind[JPT_ACT_JUMP] = JPT_PAD_NONE;
    CHECK(plain_is(jump, &c, "Press [Unbound] to jump, and [B] to crouch."));
}

static void test_tutorial_composition(void) {
    jpt_gamepad_settings_t cfg;
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_context_t c;
    jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
    pad_glyph_t g;
    char out[512];
    int n;
    jpt_gamepad_defaults(&cfg);
    jpt_keymap_default(map);
    /* text, glyph, text on one line: a mark stands where each glyph goes */
    c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
    n = jpt_prompt_text_render("Press 'Q' to jump, and 'Z' to crouch.", &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS);
    CHECK(n == 2 && !strcmp(out, "Press \x7F to jump, and \x7F to crouch."));
    CHECK(pad_glyph_token(&tokens[0], c.family, &g) && g.shape == PAD_GLYPH_FACE && !strcmp(g.text, "A"));
    CHECK(pad_glyph_token(&tokens[1], c.family, &g) && g.shape == PAD_GLYPH_FACE && !strcmp(g.text, "B"));
    c.family = JPT_FAMILY_PLAYSTATION;
    jpt_prompt_text_render("Press 'Q' to jump.", &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS);
    CHECK(pad_glyph_token(&tokens[0], c.family, &g) && g.symbol == PAD_SYMBOL_CROSS);
    c.family = JPT_FAMILY_STEAM_DECK;
    jpt_prompt_text_render("...hold the left mouse button, and move the mouse.", &c, out, sizeof out, tokens,
                           JPT_PROMPT_MAX_TOKENS);
    CHECK(pad_glyph_token(&tokens[0], c.family, &g) && g.shape == PAD_GLYPH_TRIGGER && !strcmp(g.text, "L2"));
    CHECK(pad_glyph_token(&tokens[1], c.family, &g) && g.shape == PAD_GLYPH_STICK && !strcmp(g.text, "R"));
    /* keyboard: keycaps with the bound key, mouse buttons as a mouse, mouse movement as a plain mouse */
    c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    n = jpt_prompt_text_render("<HAND> + <LOOK>, <GRAB>, <USE>, <JUMP>", &c, out, sizeof out, tokens,
                               JPT_PROMPT_MAX_TOKENS);
    CHECK(n == 5 && !strcmp(out, "\x7F + \x7F, \x7F, \x7F, \x7F"));
    CHECK(pad_glyph_token(&tokens[0], c.family, &g) && g.shape == PAD_GLYPH_MOUSE && g.dir == 1);
    CHECK(pad_glyph_token(&tokens[1], c.family, &g) && g.shape == PAD_GLYPH_MOUSE && g.dir == 0);
    CHECK(pad_glyph_coverage(&g, PAD_LAYER_ACCENT, 0.3f, 0.2f, 0.01f) == 0.0f);
    CHECK(pad_glyph_token(&tokens[2], c.family, &g) && g.shape == PAD_GLYPH_MOUSE && g.dir == 2);
    CHECK(pad_glyph_token(&tokens[3], c.family, &g) && g.shape == PAD_GLYPH_KEY && !strcmp(g.text, "Space"));
    CHECK(pad_glyph_token(&tokens[4], c.family, &g) && g.shape == PAD_GLYPH_KEY && !strcmp(g.text, "Q"));
    /* too small an output is refused, never cut */
    CHECK(jpt_prompt_text_render("Press 'Q' to jump.", &c, out, 8, tokens, JPT_PROMPT_MAX_TOKENS) < 0);
    CHECK(jpt_prompt_text_render("Press 'Q' to jump, and 'Z' to crouch.", &c, out, sizeof out, tokens, 1) < 0);
    /* the controls card follows the device */
    c.gamepad = 1;
    CHECK(jpt_prompt_text_has_prompts("<CARD>"));
    n = jpt_prompt_text_render("<CARD>", &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS);
    CHECK(n >= 14 && strchr(out, '\r') && strstr(out, "Rotate wrist"));
    c.gamepad = 0;
    CHECK(jpt_prompt_text_render("<CARD>", &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS) >= 14);
    CHECK(plain_is(jpt_prompt_controls_hint(), &c, "[View] Controls"));
}

static int guide_has(const jpt_guide_row_t* rows, int n, const char* action, const char* binding) {
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].action, action) && !strcmp(rows[i].binding, binding)) return 1;
    printf("  no row \"%s\" = \"%s\"\n", action, binding);
    return 0;
}

static void test_controls_guide(void) {
    jpt_gamepad_settings_t cfg;
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_context_t c;
    jpt_guide_row_t rows[JPT_GUIDE_MAX_ROWS];
    int n;
    jpt_gamepad_defaults(&cfg);
    jpt_keymap_default(map);
    c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
    n = jpt_controls_guide(&c, rows, JPT_GUIDE_MAX_ROWS);
    CHECK(guide_has(rows, n, "Move (walk, run, sidestep)", "Left stick"));
    CHECK(guide_has(rows, n, "Look", "Right stick"));
    CHECK(guide_has(rows, n, "Jump", "A"));
    CHECK(guide_has(rows, n, "Crouch (hold)", "B"));
    CHECK(guide_has(rows, n, "Crouch on/off", "Left stick click (LS)"));
    CHECK(guide_has(rows, n, "Use / Fire", "Right trigger (RT)"));
    CHECK(guide_has(rows, n, "Raise and move the hand", "Left trigger (LT) + Right stick"));
    CHECK(guide_has(rows, n, "Grab / Drop (hand raised to grab)", "X"));
    CHECK(guide_has(rows, n, "Throw", "Y"));
    CHECK(guide_has(rows, n, "Stow / Retrieve", "D-pad up"));
    CHECK(guide_has(rows, n, "Rotate wrist", "Left bumper (LB) + Right stick"));
    CHECK(guide_has(rows, n, "Rotate arm", "Right bumper (RB) + Right stick"));
    CHECK(guide_has(rows, n, "Replay voice over", "D-pad down"));
    CHECK(guide_has(rows, n, "Pause menu", "Menu"));
    c.family = JPT_FAMILY_PLAYSTATION;
    n = jpt_controls_guide(&c, rows, JPT_GUIDE_MAX_ROWS);
    CHECK(guide_has(rows, n, "Jump", "Cross"));
    CHECK(guide_has(rows, n, "Pause menu", "Options"));
    cfg.bind[JPT_ACT_THROW] = JPT_PAD_DPAD_LEFT;
    n = jpt_controls_guide(&c, rows, JPT_GUIDE_MAX_ROWS);
    CHECK(guide_has(rows, n, "Throw", "D-pad left"));
    c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    n = jpt_controls_guide(&c, rows, JPT_GUIDE_MAX_ROWS);
    CHECK(guide_has(rows, n, "Run", "W"));
    CHECK(guide_has(rows, n, "Walk", "S"));
    CHECK(guide_has(rows, n, "Step left", "A"));
    CHECK(guide_has(rows, n, "Step right", "D"));
    CHECK(guide_has(rows, n, "Walk backwards", "X"));
    CHECK(guide_has(rows, n, "Jump", "Q"));
    CHECK(guide_has(rows, n, "Crouch (hold)", "Z"));
    CHECK(guide_has(rows, n, "Throw", "F"));
    CHECK(guide_has(rows, n, "Stow / Retrieve", "E"));
    CHECK(guide_has(rows, n, "Replay voice over", "R"));
    CHECK(guide_has(rows, n, "Fire / Use", "Space"));
    CHECK(guide_has(rows, n, "Reach with the hand (hold)", "Mouse Left"));
    CHECK(guide_has(rows, n, "Move the arm", "Mouse Left + Mouse movement"));
    CHECK(guide_has(rows, n, "Grab an object", "Mouse Left + Mouse Right"));
    CHECK(guide_has(rows, n, "Drop the object", "Mouse Right"));
    CHECK(guide_has(rows, n, "Rotate arm", "Ctrl + Mouse Left + Mouse movement"));
    CHECK(guide_has(rows, n, "Rotate wrist", "Shift + Mouse Left + Mouse movement"));
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++)
        if (map[i].command == JPT_CMD_STOW) map[i].vk = 'G';
    n = jpt_controls_guide(&c, rows, JPT_GUIDE_MAX_ROWS);
    CHECK(guide_has(rows, n, "Stow / Retrieve", "G"));
}

static void test_launcher_controller(void) {
    /* the launcher's connected controller and the glyphs it shows */
    jpt_pad_family_t xbox = jpt_pad_family_of(JPT_PAD_TYPE_XBOXONE, 0x045E, 0x0B12, "Xbox Series X Controller");
    CHECK(xbox == JPT_FAMILY_XBOX && !strcmp(jpt_pad_family_name(xbox), "Xbox"));
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 1, xbox) == JPT_FAMILY_XBOX);
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 1, JPT_FAMILY_PLAYSTATION) == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_glyph_family(JPT_GLYPHS_PLAYSTATION, 1, xbox) == JPT_FAMILY_PLAYSTATION);
    CHECK(jpt_glyph_family(JPT_GLYPHS_STEAM_DECK, 0, xbox) == JPT_FAMILY_STEAM_DECK);
    CHECK(jpt_glyph_family(JPT_GLYPHS_GENERIC, 1, xbox) == JPT_FAMILY_GENERIC);
    CHECK(jpt_glyph_family(JPT_GLYPHS_AUTOMATIC, 0, JPT_FAMILY_PLAYSTATION) == JPT_FAMILY_XBOX);
}

int main(void) {
    test_last_device();
    test_activity_filter();
    test_deadzones();
    test_type_detection();
    test_glyph_selection();
    test_family_mappings();
    test_settings();
    test_keyboard_prompts();
    test_gamepad_prompts();
    test_glyph_shapes();
    test_roster();
    test_gameplay();
    test_edges();
    test_navigation();
    test_hotplug_flow();
    test_tutorial_text();
    test_tutorial_bindings();
    test_tutorial_composition();
    test_controls_guide();
    test_launcher_controller();
    printf("gamepad_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
