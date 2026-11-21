/*
 * Headless checks of the gamepad's logic: which device the player used last, shaping, controller families, bindings,
 * prompts and glyphs, gameplay input for the game's CInput and focus movement in menus. No SDL device, no window.
 * Build: CMake target gamepad_tests.
 */
#include <ctype.h>
#include "test_profile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamepad_policy.h"
#include "overlay_policy.h"
#include "jpt_config.h"
#include "jpt_display.h"
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
    {
        /* LT resting near its threshold: one press, one release, no chatter in between */
        static const float lt[] = { 0.10f, 0.15f, 0.19f, 0.21f, 0.18f, 0.13f, 0.16f, 0.125f, 0.11f, 0.17f, 0.19f };
        static const int down[] = { 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0 };
        uint8_t held[2] = { 0, 0 };
        pad_state_t s = idle();
        int changes = 0, was = 0, ok = 1;
        for (int i = 0; i < (int)(sizeof lt / sizeof lt[0]); i++) {
            int now;
            s.lt = lt[i];
            now = (pad_buttons_with_trigger_hold(&s, 0.12f, held) & JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER)) != 0;
            ok &= now == down[i];
            CHECK(pad_buttons_with_trigger_hold(&s, 0.12f, held) == (now ? JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER) : 0u));
            changes += now != was;
            was = now;
        }
        CHECK(ok && changes == 2);
        /* the other buttons pass through, RT keeps its own state */
        s = idle();
        s.buttons = JPT_PAD_BIT(JPT_PAD_SOUTH);
        s.rt = 0.5f;
        CHECK(pad_buttons_with_trigger_hold(&s, 0.12f, held) ==
              (JPT_PAD_BIT(JPT_PAD_SOUTH) | JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER)));
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
    CHECK(s.bind[JPT_ACT_STOW] == JPT_PAD_DPAD_UP);
    for (int a = 0; a < JPT_ACT_COUNT; a++) CHECK(s.bind[a] != JPT_PAD_DPAD_DOWN && s.bind[a] != JPT_PAD_DPAD_LEFT && s.bind[a] != JPT_PAD_DPAD_RIGHT);
    CHECK(!jpt_gamepad_is_key("gamepad_bind_replay") && jpt_pad_control_free(&s, JPT_PAD_SHOW_OBJECTIVE_CONTROL));
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
    CHECK(g.held == 0); /* the latch is pad_crouch's */
    pad_gameplay_from(&s, 0, &cfg, 0.016f, 1, &g);
    CHECK(g.held == JPT_CMD_CROUCH);
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_RIGHT_TRIGGER) | JPT_PAD_BIT(JPT_PAD_NORTH)) == (JPT_CMD_USE | JPT_CMD_THROW));
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER) | JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER)) ==
          (JPT_CMD_WRIST | JPT_CMD_ARM));
    CHECK(pad_commands_for(&cfg, JPT_PAD_BIT(JPT_PAD_START) | JPT_PAD_BIT(JPT_PAD_BACK)) == 0);
    /* LB/RB are Shift/Ctrl: their own bits only, Move Hand stays LT's (the left button's) */
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER), &cfg, 0.016f, 0, &g);
    CHECK(g.held == JPT_CMD_WRIST);
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER), &cfg, 0.016f, 0, &g);
    CHECK(g.held == JPT_CMD_ARM);
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER) | JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER), &cfg, 0.016f, 0, &g);
    CHECK(g.held == (JPT_CMD_WRIST | JPT_CMD_HAND));
    pad_gameplay_from(&s, JPT_PAD_BIT(JPT_PAD_EAST), &cfg, 0.016f, 1, &g);
    CHECK(g.held == JPT_CMD_CROUCH);
    /* merged with the keyboard (the look goes through the mouse's cursor, never here) */
    memset(&in, 0, sizeof in);
    in.move_y = 1.0f;
    in.state = JPT_CMD_USE;
    memset(&g, 0, sizeof g);
    g.move_y = 0.5f;
    g.move_x = 0.5f;
    g.look_x = 0.02f;
    g.held = JPT_CMD_JUMP;
    pad_merge_sinput(&in, &g, JPT_CMD_JUMP);
    CHECK(near(in.move_y, 1.0f, 1e-6f) && near(in.move_x, 0.5f, 1e-6f));
    CHECK(in.state == (JPT_CMD_USE | JPT_CMD_JUMP) && in.hit == JPT_CMD_JUMP);
    memset(&in, 0, sizeof in);
    in.move_y = -0.4f;
    g.held = 0;
    g.move_y = -0.4f;
    pad_merge_sinput(&in, &g, 0);
    CHECK(near(in.move_y, -0.4f, 1e-6f));
}

/* What reaches the game's crouch command after a sequence; B is the hold, L3 the latch, A jumps. */
typedef struct {
    int b_down, b_press, l3_press, jump_press, stand;
} crouch_step_t;

static int crouch_after(const crouch_step_t* steps, int n, int* latched) {
    pad_crouch_t c;
    int out = 0;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < n; i++) {
        if (steps[i].stand) pad_crouch_stand(&c);
        out = pad_crouch_update(&c, steps[i].b_down, steps[i].l3_press, steps[i].b_press, steps[i].jump_press);
    }
    *latched = c.latched;
    return out;
}

static void test_crouch(void) {
    /* every sequence ends in a posture the player can predict, and one more B press and release always stands */
    static const crouch_step_t b_tap[] = { { 1, 1, 0, 0, 0 }, { 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t l3_once[] = { { 0, 0, 1, 0, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t l3_twice[] = { { 0, 0, 1, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t b_hold_l3[] = { { 1, 1, 0, 0, 0 }, { 1, 0, 1, 0, 0 }, { 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t l3_then_b[] = { { 0, 0, 1, 0, 0 }, { 1, 1, 0, 0, 0 }, { 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t l3_then_jump[] = { { 0, 0, 1, 0, 0 }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t l3_menu[] = { { 0, 0, 1, 0, 0 }, { 0, 0, 0, 0, 1 }, { 0, 0, 0, 0, 0 } };
    static const crouch_step_t b_held_menu[] = { { 1, 1, 0, 0, 0 }, { 1, 0, 0, 0, 1 }, { 0, 0, 0, 0, 0 } };
    int latched, out;
    out = crouch_after(b_tap, 3, &latched);
    CHECK(!out && !latched);
    {
        pad_crouch_t c;
        memset(&c, 0, sizeof c);
        CHECK(pad_crouch_update(&c, 1, 0, 1, 0) == 1 && pad_crouch_update(&c, 1, 0, 0, 0) == 1); /* down while held */
        CHECK(pad_crouch_update(&c, 0, 0, 0, 0) == 0 && !c.latched);                            /* up on release */
    }
    out = crouch_after(l3_once, 2, &latched);
    CHECK(out && latched);
    out = crouch_after(l3_twice, 4, &latched);
    CHECK(!out && !latched);
    /* L3 while B is held latches: releasing B keeps crouching, the next L3 or B press ends it */
    out = crouch_after(b_hold_l3, 4, &latched);
    CHECK(out && latched);
    /* B takes the latched crouch over; releasing it stands */
    out = crouch_after(l3_then_b, 4, &latched);
    CHECK(!out && !latched);
    out = crouch_after(l3_then_jump, 3, &latched);
    CHECK(!out && !latched);
    /* a menu, focus loss, a device switch or a gameplay gap stands the latch up; a held B still counts while held */
    out = crouch_after(l3_menu, 3, &latched);
    CHECK(!out && !latched);
    out = crouch_after(b_held_menu, 3, &latched);
    CHECK(!out && !latched);
    {
        /* B and L3 in the same frame: B's press releases, L3 latches */
        pad_crouch_t c;
        memset(&c, 0, sizeof c);
        c.latched = 1;
        CHECK(pad_crouch_update(&c, 1, 1, 1, 0) == 1 && c.latched == 1);
        CHECK(pad_crouch_update(&c, 0, 0, 0, 0) == 1);
        CHECK(pad_crouch_update(&c, 1, 0, 1, 0) == 1 && pad_crouch_update(&c, 0, 0, 0, 0) == 0);
    }
}

/* One frame of the pad in gameplay as the game receives it: the command bits, and the mouse counts CInput reads. */
typedef struct {
    uint32_t held;
    int dx, dy;
} pad_frame_t;

static pad_frame_t pad_frame(pad_mouse_t* m, const jpt_gamepad_settings_t* cfg, uint32_t buttons, float rx, float ry,
                             int invert_mouse) {
    pad_state_t s = idle();
    pad_gameplay_t g;
    pad_frame_t f;
    s.rx = rx;
    s.ry = ry;
    pad_gameplay_from(&s, buttons, cfg, 1.0f / 60.0f, 0, &g);
    pad_mouse_counts(m, g.look_x, g.look_y, invert_mouse, &f.dx, &f.dy);
    f.held = g.held;
    return f;
}

/* The mouse's reference: hand up (Move Hand held), right, stop, left, stop, up, stop. The pad gives the game the same
 * kind of input at every step: the Move Hand bit, then counts in the mouse's directions, nothing once the stick stops. */
static void test_stick_as_mouse(void) {
    const uint32_t lt = JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER);
    jpt_gamepad_settings_t cfg;
    pad_mouse_t m;
    pad_frame_t f;
    long sum_x = 0, sum_y = 0;
    jpt_gamepad_defaults(&cfg);
    memset(&m, 0, sizeof m);
    f = pad_frame(&m, &cfg, lt, 0.0f, 0.0f, 0);
    CHECK(f.held == JPT_CMD_HAND && f.dx == 0 && f.dy == 0); /* as the left button: the hand bit, no motion */
    for (int i = 0; i < 30; i++) {
        f = pad_frame(&m, &cfg, lt, 1.0f, 0.0f, 0);
        CHECK(f.held == JPT_CMD_HAND && f.dx > 0 && f.dy == 0);
        sum_x += f.dx;
    }
    /* half a second of full stick: the look's radians, as counts of 0.005 rad, to within one count */
    CHECK(labs(sum_x - (long)(0.5f * PAD_LOOK_YAW_RATE / PAD_RADIANS_PER_COUNT)) <= 1);
    f = pad_frame(&m, &cfg, lt, 0.0f, 0.0f, 0);
    CHECK(f.dx == 0 && f.dy == 0 && m.rest_x == 0.0f && m.rest_y == 0.0f); /* stops at once */
    for (int i = 0; i < 10; i++) {
        f = pad_frame(&m, &cfg, lt, -1.0f, 0.0f, 0);
        CHECK(f.dx < 0 && f.dy == 0);
    }
    f = pad_frame(&m, &cfg, lt, 0.0f, 0.0f, 0);
    CHECK(f.dx == 0 && f.dy == 0);
    for (int i = 0; i < 10; i++) {
        f = pad_frame(&m, &cfg, lt, 0.0f, -1.0f, 0);
        CHECK(f.dx == 0 && f.dy < 0); /* stick up moves the cursor up, as the mouse pushed forward */
        sum_y += f.dy;
    }
    f = pad_frame(&m, &cfg, lt, 0.0f, 0.0f, 0);
    CHECK(f.dx == 0 && f.dy == 0 && sum_y < 0);
    /* a slow push keeps its fractions: no motion is lost, it arrives a count at a time */
    sum_x = 0;
    for (int i = 0; i < 120; i++) {
        f = pad_frame(&m, &cfg, 0, 0.3f, 0.0f, 0);
        CHECK(f.dx >= 0 && f.dx <= 2);
        sum_x += f.dx;
    }
    {
        pad_state_t s = idle();
        pad_gameplay_t g;
        s.rx = 0.3f;
        pad_gameplay_from(&s, 0, &cfg, 1.0f / 60.0f, 0, &g);
        CHECK(labs(sum_x - (long)(120 * g.look_x / PAD_RADIANS_PER_COUNT)) <= 1);
    }
    /* the game's InvertMouse flips the counts it reads, so the pad's up stays up with it on */
    memset(&m, 0, sizeof m);
    f = pad_frame(&m, &cfg, 0, 0.0f, -1.0f, 1);
    CHECK(f.dy > 0);
    cfg.invert_y = 1;
    f = pad_frame(&m, &cfg, 0, 0.0f, -1.0f, 0);
    CHECK(f.dy > 0);
    /* wrist and arm are the mouse's Shift and Ctrl: the same counts, those bits added, the hand LT's as the left
     * button's */
    jpt_gamepad_defaults(&cfg);
    memset(&m, 0, sizeof m);
    f = pad_frame(&m, &cfg, JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER), 1.0f, 0.0f, 0);
    CHECK(f.held == JPT_CMD_WRIST && f.dx > 0);
    f = pad_frame(&m, &cfg, lt | JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER), 1.0f, 0.0f, 0);
    CHECK(f.held == (JPT_CMD_WRIST | JPT_CMD_HAND) && f.dx > 0);
    f = pad_frame(&m, &cfg, lt | JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER), 1.0f, 0.0f, 0);
    CHECK(f.held == (JPT_CMD_ARM | JPT_CMD_HAND) && f.dx > 0);
    f = pad_frame(&m, &cfg, JPT_PAD_BIT(JPT_PAD_WEST), 0.0f, 0.0f, 0);
    CHECK(f.held == JPT_CMD_GRAB);
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
    /* a pause in gameplay's reads: what was held and still is keeps counting with no new press, as a held mouse button;
     * a button pressed meanwhile (the menu's) waits for its release; one let go meanwhile is gone */
    pad_edges_reset(&e, 0);
    pad_edges_update(&e, a, &pressed);
    pad_edges_resume(&e, a | b);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == a && pressed == 0);
    held = pad_edges_update(&e, a, &pressed);
    CHECK(held == a && pressed == 0);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == (a | b) && pressed == b);
    pad_edges_resume(&e, b);
    held = pad_edges_update(&e, a | b, &pressed);
    CHECK(held == (a | b) && pressed == a);
    /* nothing counted yet (the first read): all held buttons wait, as after a reset */
    memset(&e, 0, sizeof e);
    pad_edges_resume(&e, a);
    held = pad_edges_update(&e, a, &pressed);
    CHECK(held == 0 && pressed == 0);
    /* which reads resume: a menu, a video or another window in between, the first, or a gap past 0.25 s */
    CHECK(pad_read_kind(0, 0, 1, 0.016) == PAD_READ_NEXT);
    CHECK(pad_read_kind(0, 0, 1, 0.24) == PAD_READ_NEXT);
    CHECK(pad_read_kind(0, 1, 1, 0.05) == PAD_READ_RESUME);
    CHECK(pad_read_kind(0, 0, 1, 0.30) == PAD_READ_RESTART && pad_read_kind(0, 1, 1, 3.0) == PAD_READ_RESTART);
    CHECK(pad_read_kind(1, 0, 1, 0.0) == PAD_READ_RESTART && pad_read_kind(0, 0, 0, 0.016) == PAD_READ_RESTART);
}

#define L3_BIT JPT_PAD_BIT(JPT_PAD_LEFT_STICK)
#define LT_BIT JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER)
#define X_BIT JPT_PAD_BIT(JPT_PAD_WEST)
#define B_BIT JPT_PAD_BIT(JPT_PAD_EAST)
#define A_BIT JPT_PAD_BIT(JPT_PAD_SOUTH)

typedef struct {
    jpt_gamepad_settings_t cfg;
    pad_play_t play;
    int started;
} l3_rig_t;

typedef struct {
    uint32_t held, hit;
    int crouching;
} l3_frame_t;

static void l3_init(l3_rig_t* r) {
    memset(r, 0, sizeof *r);
    jpt_gamepad_defaults(&r->cfg);
}

static l3_frame_t l3_step_kind(l3_rig_t* r, pad_read_t kind, uint32_t buttons) {
    pad_state_t s = idle();
    pad_sinput_t in;
    l3_frame_t f;
    int dx, dy;
    pad_play_before(&r->play, kind, &s, buttons, &r->cfg, 0.016f, 0, &dx, &dy);
    memset(&in, 0, sizeof in);
    pad_play_after(&r->play, &r->cfg, &in);
    f.held = in.state & (JPT_CMD_GRAB | JPT_CMD_CROUCH | JPT_CMD_HAND);
    f.hit = in.hit;
    f.crouching = (in.state & JPT_CMD_CROUCH) != 0;
    return f;
}

static l3_frame_t l3_step(l3_rig_t* r, uint32_t buttons) {
    pad_read_t kind = r->started ? PAD_READ_NEXT : PAD_READ_RESTART;
    r->started = 1;
    return l3_step_kind(r, kind, buttons);
}

/* cinput_read around interfaces: blocked (a host interface is up) or resumed (a window of the game was up) makes the pad
 * neutral; the pad merges into what the keyboard and mouse wrote, then the command mask applies and a blocked read is
 * emptied. */
typedef struct {
    jpt_gamepad_settings_t cfg;
    pad_play_t play;
    ov_commands_t commands;
    int started;
} ov_rig_t;

typedef struct {
    uint32_t state, hit;
    float move_x, move_y;
    int look;
} ov_frame_t;

enum { PLAY, HOST_UI, AFTER_WINDOW };

static void ov_rig_init(ov_rig_t* r) {
    memset(r, 0, sizeof *r);
    jpt_gamepad_defaults(&r->cfg);
}

static ov_frame_t ov_step(ov_rig_t* r, int when, uint32_t buttons, uint32_t keys, float stick_y, float look_x) {
    pad_state_t s = idle();
    pad_sinput_t in;
    ov_frame_t f;
    int dx, dy, blocked = when == HOST_UI, resumed = when == AFTER_WINDOW;
    uint32_t pass;
    s.buttons = buttons;
    s.ly = stick_y;
    s.rx = look_x;
    if (blocked != r->commands.blocked || resumed) pad_play_neutral(&r->play, buttons);
    pad_play_before(&r->play, !r->started ? PAD_READ_RESTART : resumed ? PAD_READ_RESUME : PAD_READ_NEXT, &s, buttons,
                    &r->cfg, 0.016f, 0, &dx, &dy);
    r->started = 1;
    memset(&in, 0, sizeof in);
    in.state = keys;
    pad_play_after(&r->play, &r->cfg, &in);
    ov_commands_block(&r->commands, blocked, in.state);
    if (resumed) ov_commands_resume(&r->commands, in.state);
    pass = ov_commands_pass(&r->commands, in.state);
    if (blocked) {
        memset(&in, 0, sizeof in);
        dx = dy = 0;
    }
    f.state = pass & in.state;
    f.hit = in.hit & pass;
    f.move_x = in.move_x;
    f.move_y = in.move_y;
    f.look = dx || dy;
    return f;
}

#define LB_BIT JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER)
#define RB_BIT JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER)

static void test_overlay_neutral(void) {
    const uint32_t mouse = JPT_CMD_HAND;
    ov_rig_t r;
    ov_frame_t f;

    /* A: LT, Controls opens (the game sees Move Hand let go), closes with LT held: off until released and pressed */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK((f.state & JPT_CMD_HAND) && (f.hit & JPT_CMD_HAND));
    f = ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0);
    CHECK(!f.state && !f.hit);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND) && !f.hit);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    f = ov_step(&r, PLAY, 0, 0, 0, 0);
    CHECK(!f.state);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK((f.state & JPT_CMD_HAND) && (f.hit & JPT_CMD_HAND));
    for (int i = 0; i < 30; i++) f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK(f.state & JPT_CMD_HAND);

    /* B: LT + right stick, Objectives: no hand movement while up nor after while the stick is still pushed */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0.8f);
    CHECK((f.state & JPT_CMD_HAND) && f.look);
    f = ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0.8f);
    CHECK(!f.look && !f.state);
    f = ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0.0f);
    f = ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0.9f);
    CHECK(!f.look);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0.9f);
    CHECK(!f.look && !(f.state & JPT_CMD_HAND));
    f = ov_step(&r, PLAY, 0, 0, 0, 0.0f);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0.8f);
    CHECK((f.state & JPT_CMD_HAND) && f.look);

    /* each stick waits for its own centre: walking on does not hold the hand's stick back */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, HOST_UI, 0, 0, -1.0f, 0.0f);
    f = ov_step(&r, PLAY, 0, 0, -1.0f, 0.0f);
    CHECK(f.move_y == 0.0f);
    f = ov_step(&r, PLAY, LT_BIT, 0, -1.0f, 0.8f);
    CHECK(f.look && (f.state & JPT_CMD_HAND) && f.move_y == 0.0f);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0.0f, 0.8f);
    f = ov_step(&r, PLAY, LT_BIT, 0, -1.0f, 0.8f);
    CHECK(f.look && f.move_y > 0.0f);

    /* C: the mouse's hand button held through the Hint (a window of the game): off until released and pressed */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, 0, mouse, 0, 0);
    CHECK(f.state & JPT_CMD_HAND);
    f = ov_step(&r, AFTER_WINDOW, 0, mouse, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    f = ov_step(&r, PLAY, 0, mouse, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, 0, mouse, 0, 0);
    CHECK(f.state & JPT_CMD_HAND);
    /* the same with LT through the Hint */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    f = ov_step(&r, AFTER_WINDOW, LT_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK(f.state & JPT_CMD_HAND);

    /* D, E: LT + LB or RB + right stick across an interface: no wrist or arm left on */
    for (int arm = 0; arm < 2; arm++) {
        const uint32_t mod = arm ? RB_BIT : LB_BIT, cmd = arm ? JPT_CMD_ARM : JPT_CMD_WRIST;
        ov_rig_init(&r);
        ov_step(&r, PLAY, 0, 0, 0, 0);
        f = ov_step(&r, PLAY, LT_BIT | mod, 0, 0, 0.7f);
        CHECK(f.state & cmd);
        ov_step(&r, HOST_UI, LT_BIT | mod, 0, 0, 0.7f);
        f = ov_step(&r, PLAY, LT_BIT | mod, 0, 0, 0.7f);
        CHECK(!(f.state & (cmd | JPT_CMD_HAND)) && !f.look);
        ov_step(&r, PLAY, 0, 0, 0, 0);
        f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0.7f);
        CHECK((f.state & JPT_CMD_HAND) && !(f.state & cmd) && f.look);
    }

    /* F: X held across an interface: no phantom Grab or Drop; a new press grabs */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, HOST_UI, X_BIT, 0, 0, 0);
    f = ov_step(&r, PLAY, X_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_GRAB) && !(f.hit & JPT_CMD_GRAB));
    f = ov_step(&r, PLAY, 0, 0, 0, 0);
    CHECK(!(f.hit & JPT_CMD_GRAB));
    f = ov_step(&r, PLAY, X_BIT, 0, 0, 0);
    CHECK(f.hit & JPT_CMD_GRAB);

    /* G: Controls -> Hint -> Objectives -> Controls with LT held throughout, then a clean press */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0);
    f = ov_step(&r, AFTER_WINDOW, LT_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0);
    ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    ov_step(&r, HOST_UI, LT_BIT, 0, 0, 0);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    CHECK(!(f.state & JPT_CMD_HAND));
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, LT_BIT, 0, 0, 0.6f);
    CHECK((f.state & JPT_CMD_HAND) && (f.hit & JPT_CMD_HAND) && f.look);

    /* A and B that closed Objectives: no jump, no crouch while held */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, HOST_UI, A_BIT | B_BIT, 0, 0, 0);
    f = ov_step(&r, PLAY, A_BIT | B_BIT, 0, 0, 0);
    CHECK(!(f.state & (JPT_CMD_JUMP | JPT_CMD_CROUCH)) && !(f.hit & JPT_CMD_JUMP));
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, PLAY, A_BIT, 0, 0, 0);
    CHECK(f.hit & JPT_CMD_JUMP);

    /* a slow frame with no window keeps a held LT (the hand never drops on a long frame) */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    ov_step(&r, PLAY, LT_BIT, 0, 0, 0);
    {
        pad_state_t s = idle();
        pad_sinput_t in;
        int dx, dy;
        s.buttons = LT_BIT;
        pad_play_before(&r.play, PAD_READ_RESTART, &s, LT_BIT, &r.cfg, 0.4f, 0, &dx, &dy);
        memset(&in, 0, sizeof in);
        pad_play_after(&r.play, &r.cfg, &in);
        CHECK(in.state & JPT_CMD_HAND);
    }

    /* while a host interface is up nothing at all reaches the game */
    ov_rig_init(&r);
    ov_step(&r, PLAY, 0, 0, 0, 0);
    f = ov_step(&r, HOST_UI, A_BIT | B_BIT | LT_BIT | X_BIT, JPT_CMD_RUN | mouse, -1.0f, 1.0f);
    CHECK(!f.state && !f.hit && f.move_y == 0.0f && !f.look);
}

static void test_grab_and_crouch_buttons(void) {
    l3_rig_t r;
    l3_frame_t f;
    const uint32_t grab_hand = JPT_CMD_HAND | JPT_CMD_GRAB;

    /* L3 alone is only the crouch toggle */
    l3_init(&r);
    l3_step(&r, 0);
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching && !(f.held & JPT_CMD_GRAB) && !(f.hit & JPT_CMD_GRAB));
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching && !(f.held & JPT_CMD_GRAB));
    f = l3_step(&r, 0);
    CHECK(f.crouching);
    f = l3_step(&r, L3_BIT);
    CHECK(!f.crouching && !(f.held & JPT_CMD_GRAB));
    l3_step(&r, 0);

    /* with LT held L3 is still only the crouch toggle: no Grab bit and no Grab edge at any step */
    l3_init(&r);
    l3_step(&r, 0);
    f = l3_step(&r, LT_BIT);
    CHECK(f.held == JPT_CMD_HAND && f.hit == JPT_CMD_HAND);
    f = l3_step(&r, LT_BIT | L3_BIT);
    CHECK(f.held == (JPT_CMD_HAND | JPT_CMD_CROUCH) && !(f.hit & JPT_CMD_GRAB));
    f = l3_step(&r, LT_BIT | L3_BIT);
    CHECK(f.held == (JPT_CMD_HAND | JPT_CMD_CROUCH) && f.hit == 0);
    f = l3_step(&r, L3_BIT);
    CHECK(f.held == JPT_CMD_CROUCH && f.hit == 0);
    f = l3_step(&r, 0);
    CHECK(f.held == JPT_CMD_CROUCH && f.hit == 0);
    f = l3_step(&r, LT_BIT | L3_BIT);
    CHECK(f.held == JPT_CMD_HAND && !(f.hit & (JPT_CMD_GRAB | JPT_CMD_CROUCH)));
    l3_step(&r, 0);

    /* L3 pressed for the crouch, LT pressed while it is held: the crouch stays, nothing is grabbed */
    l3_init(&r);
    l3_step(&r, 0);
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching);
    f = l3_step(&r, L3_BIT | LT_BIT);
    CHECK(f.crouching && !(f.held & JPT_CMD_GRAB) && !(f.hit & JPT_CMD_GRAB));
    l3_step(&r, 0);

    /* X is Grab / Drop by touch: one edge per press, no hold needed, with or without LT or L3 */
    l3_init(&r);
    l3_step(&r, 0);
    f = l3_step(&r, X_BIT);
    CHECK(f.held == JPT_CMD_GRAB && f.hit == JPT_CMD_GRAB && !f.crouching);
    f = l3_step(&r, X_BIT);
    CHECK(f.held == JPT_CMD_GRAB && f.hit == 0);
    f = l3_step(&r, 0);
    CHECK(f.held == 0 && f.hit == 0);
    f = l3_step(&r, X_BIT);
    CHECK(f.hit == JPT_CMD_GRAB);
    l3_step(&r, 0);
    l3_step(&r, LT_BIT);
    f = l3_step(&r, LT_BIT | X_BIT);
    CHECK(f.held == grab_hand && f.hit == JPT_CMD_GRAB && !f.crouching);
    f = l3_step(&r, LT_BIT);
    CHECK(f.held == JPT_CMD_HAND && f.hit == 0);
    l3_step(&r, 0);
    f = l3_step(&r, X_BIT | L3_BIT);
    CHECK((f.held & JPT_CMD_GRAB) && f.hit == JPT_CMD_GRAB && f.crouching);

    /* B is the crouch hold, with or without LT; B or Jump end an L3 latch */
    l3_init(&r);
    l3_step(&r, 0);
    f = l3_step(&r, B_BIT);
    CHECK(f.crouching);
    f = l3_step(&r, LT_BIT | B_BIT);
    CHECK(f.held == (JPT_CMD_HAND | JPT_CMD_CROUCH));
    f = l3_step(&r, LT_BIT);
    CHECK(!f.crouching);
    l3_step(&r, 0);
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching);
    l3_step(&r, 0);
    f = l3_step(&r, B_BIT);
    CHECK(f.crouching);
    f = l3_step(&r, 0);
    CHECK(!f.crouching);
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching);
    l3_step(&r, 0);
    f = l3_step(&r, A_BIT);
    CHECK(!f.crouching);

    /* a restart read stands up; an L3 still down after a restart or a menu waits for its release */
    l3_init(&r);
    l3_step(&r, 0);
    l3_step(&r, L3_BIT);
    l3_step(&r, 0);
    f = l3_step_kind(&r, PAD_READ_RESTART, L3_BIT);
    CHECK(!f.crouching);
    f = l3_step(&r, L3_BIT);
    CHECK(!f.crouching);
    l3_step(&r, 0);
    f = l3_step(&r, L3_BIT);
    CHECK(f.crouching);
    f = l3_step_kind(&r, PAD_READ_RESUME, L3_BIT);
    CHECK(f.crouching && f.hit == 0);
    l3_step(&r, 0);
    f = l3_step(&r, L3_BIT);
    CHECK(!f.crouching);

    /* a device switch stands the crouch up */
    l3_init(&r);
    l3_step(&r, 0);
    l3_step(&r, L3_BIT);
    pad_play_release(&r.play);
    f = l3_step(&r, 0);
    CHECK(!f.crouching);
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
    /* BE's TrigTutorHint (TEXT action): the keyboard keeps the game's own line, a gamepad sees its Hint button */
    {
        const char* f1 = "You can always press F1 for a hint about what to do next.";
        jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
        CHECK(jpt_prompt_text_has_prompts(f1));
        CHECK(jpt_prompt_text_render(f1, &kb, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS) < 0);
        CHECK(plain_is(f1, &xbox, "You can always press [D-pad left] for a hint about what to do next."));
        CHECK(plain_is(f1, &ps, "You can always press [D-pad left] for a hint about what to do next."));
        CHECK(jpt_prompt_text_render(f1, &ps, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS) == 1 &&
              tokens[0].kind == JPT_TOKEN_HINT && tokens[0].prompt.control == JPT_PAD_HINT_CONTROL);
        CHECK(jpt_prompt_text_plain(f1, &xbox, out, sizeof out) > 0 && !strstr(out, "F1"));
        {
            jpt_gamepad_settings_t taken = cfg;
            jpt_prompt_context_t t;
            taken.bind[JPT_ACT_JUMP] = JPT_PAD_HINT_CONTROL;
            t = prompt_context(1, JPT_FAMILY_XBOX, &taken, map);
            CHECK(plain_is(f1, &t, "You can always press [Unbound] for a hint about what to do next."));
        }
        CHECK(plain_is("<SHOW_OBJECTIVE> Show Objective", &kb, "[F4] Show Objective"));
        CHECK(plain_is("<OBJECTIVES> Objectives", &kb, "[F3] Objectives"));
    }
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
    CHECK(plain_is(jpt_prompt_controls_hint(), &c, "[View] View Controls"));
    c.gamepad = 0;
    CHECK(jpt_prompt_text_render("<CARD>", &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS) >= 14);
    /* the same hint with the keyboard, the F2 keycap in the button's place */
    CHECK(plain_is(jpt_prompt_controls_hint(), &c, "[F2] View Controls"));
    CHECK(jpt_prompt_text_render(jpt_prompt_controls_hint(), &c, out, sizeof out, tokens, JPT_PROMPT_MAX_TOKENS) == 1 &&
          !tokens[0].prompt.gamepad && tokens[0].prompt.vk == 0x71);
    CHECK(pad_glyph_token(&tokens[0], c.family, &g) && g.shape == PAD_GLYPH_KEY && !strcmp(g.text, "F2"));
}

/* ---- the controls card, laid out by the host ---- */

typedef struct {
    const jpt_card_t* card;
    const jpt_prompt_context_t* ctx;
    double char_w; /* a character's width in units of the text height (bold Arial is about 0.55; this is wider) */
    int calls, last_h;
} card_measure_t;

/* What the host's measure does, with a fixed character width: text runs, and each token's glyph at 92% of the text
 * height with an eighth of it either side. */
static int template_width(const char* source, const card_measure_t* cm, int h) {
    char marked[256];
    jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
    int n, next = 0, w = 0, glyph_h = (h * 92 + 50) / 100, margin = glyph_h / 8 > 1 ? glyph_h / 8 : 1;
    if (!source || !*source) return 0;
    n = jpt_prompt_text_render(source, cm->ctx, marked, sizeof marked, tokens, JPT_PROMPT_MAX_TOKENS);
    if (n < 0) {
        if (strchr(source, '<')) return -1;
        return (int)ceil(strlen(source) * cm->char_w * h);
    }
    for (const char* s = marked; *s; s++) {
        if (*s == JPT_PROMPT_MARK) {
            pad_glyph_t g;
            if (!pad_glyph_token(&tokens[next], cm->ctx->family, &g)) pad_glyph_key(0, tokens[next].label, &g);
            w += (int)ceilf(g.width * (float)glyph_h) + 2 * margin;
            next++;
        } else {
            w += (int)ceil(cm->char_w * h);
        }
    }
    return next == n ? w : -1;
}

static int measure_card(void* context, int h, jpt_card_metrics_t* m) {
    card_measure_t* cm = (card_measure_t*)context;
    memset(m, 0, sizeof *m);
    cm->calls++;
    cm->last_h = h;
    m->count = cm->card->count;
    m->first_column = cm->card->first_column;
    m->title_w = template_width(cm->card->title, cm, h);
    for (int i = 0; i < m->count; i++) {
        m->binding_w[i] = template_width(cm->card->rows[i].binding, cm, h);
        m->action_w[i] = template_width(cm->card->rows[i].action, cm, h);
        if (m->binding_w[i] <= 0 || m->action_w[i] <= 0) return 0;
    }
    return m->title_w >= 0;
}

static int boxes_meet(int l0, int t0, int r0, int b0, int l1, int t1, int r1, int b1) {
    return l0 < r1 && l1 < r0 && t0 < b1 && t1 < b0;
}

/* Every resolution and aspect ratio the launcher offers (and the software renderer's), the game's text at the size the
 * port gives it, a line of the game's own text at the top: the card fits, nothing in it overlaps, it stays inside the
 * safe area below that text, and its text is smaller than the game's but readable. */
static void test_controls_card(void) {
    static const int screens[][2] = {
        { 640, 480 },   { 800, 600 },   { 1024, 768 },  { 1440, 1080 }, { 1920, 1080 }, { 1920, 1200 },
        { 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 }, { 3840, 1080 }, { 3320, 2488 },
        { 3840, 2160 }, { 3840, 2400 }, { 5040, 2160 }, { 5120, 1440 }, { 5424, 1520 }, { 7680, 2160 },
        { 2048, 576 },  { 1316, 986 },
    };
    jpt_gamepad_settings_t cfg;
    jpt_key_binding_t map[JPT_KEYMAP_COUNT];
    jpt_prompt_context_t c;
    char out[2048];
    jpt_gamepad_defaults(&cfg);
    jpt_keymap_default(map);
    CHECK(jpt_prompt_panel_kind("<CARD>") == JPT_PANEL_CARD);
    CHECK(jpt_prompt_panel_kind(jpt_prompt_controls_hint()) == JPT_PANEL_HINT);
    {
        /* the hotkeys on the cards: no waypoint any more; Hint and Objectives on their D-pad directions while free */
        static const char* const keys[] = { "[F1] Hint",       "[F2] Controls", "[F3] Objectives",  "[F4] Show Objective",
                                            "[F5] Toggle Minimap", "[F10] Cheats",  "[F11] Fullscreen", "[Esc] Menu" };
        const int key_count = (int)(sizeof keys / sizeof keys[0]);
        const jpt_card_t* kb = jpt_prompt_card(0);
        const jpt_card_t* pad = jpt_prompt_card(1);
        char line[160];
        int found[8] = { 0 }, next_key = 0, hint = 0, objectives = 0, view = 0, waypoint = 0, show = 0, stow = 0,
            replay = 0, keyboard_replay = 0, minimap = 0;
        c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
        for (int i = 0; i < kb->count; i++) {
            snprintf(line, sizeof line, "%s %s", kb->rows[i].binding, kb->rows[i].action);
            waypoint |= strstr(line, "aypoint") != NULL || strstr(line, "F6") != NULL;
            keyboard_replay |= strstr(line, "REPLAY") != NULL;
            if (jpt_prompt_text_plain(line, &c, out, sizeof out) < 0) continue;
            for (int k = 0; k < key_count; k++) found[k] |= !strcmp(out, keys[k]);
            if (next_key < key_count && !strcmp(out, keys[next_key])) next_key++;
        }
        for (int k = 0; k < key_count; k++) CHECK(found[k]);
        CHECK(next_key == key_count); /* in the public order, F1 to Esc */
        CHECK(!waypoint && keyboard_replay && kb->count <= JPT_CARD_MAX_ROWS);
        for (int i = 0; i < kb->count; i++) CHECK(!strstr(kb->rows[i].action, "Console") && !strstr(kb->rows[i].binding, "Ctrl"));
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        for (int i = 0; i < pad->count; i++) {
            snprintf(line, sizeof line, "%s %s", pad->rows[i].binding, pad->rows[i].action);
            if (jpt_prompt_text_plain(line, &c, out, sizeof out) < 0) continue;
            hint |= !strcmp(out, "[D-pad left] Hint");
            objectives |= !strcmp(out, "[D-pad right] Objectives");
            view |= !strcmp(out, "[View] Controls");
            show |= !strcmp(out, "[D-pad down] Show Objective");
            stow |= !strcmp(out, "[D-pad up] Stow / retrieve");
            replay |= strstr(out, "Replay") != NULL || strstr(line, "REPLAY") != NULL;
            minimap |= !strcmp(out, "[Left stick click (LS)] + [Right stick click (RS)] Toggle Minimap");
        }
        CHECK(hint && objectives && view && show && stow && !replay && minimap);
        c = prompt_context(1, JPT_FAMILY_PLAYSTATION, &cfg, map);
        CHECK(plain_is("<PAD_L3> + <PAD_R3> Toggle Minimap", &c, "[L3] + [R3] Toggle Minimap"));
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        CHECK(jpt_pad_control_free(&cfg, JPT_PAD_HINT_CONTROL) && jpt_pad_control_free(&cfg, JPT_PAD_OBJECTIVES_CONTROL) &&
                  jpt_pad_control_free(&cfg, JPT_PAD_SHOW_OBJECTIVE_CONTROL) && !jpt_pad_control_free(&cfg, JPT_PAD_DPAD_UP) &&
                  JPT_PAD_SHOW_OBJECTIVE_CONTROL == JPT_PAD_DPAD_DOWN);
        {
            jpt_gamepad_settings_t taken = cfg;
            taken.bind[JPT_ACT_JUMP] = JPT_PAD_SHOW_OBJECTIVE_CONTROL;
            c = prompt_context(1, JPT_FAMILY_XBOX, &taken, map);
            CHECK(!jpt_pad_control_free(&taken, JPT_PAD_SHOW_OBJECTIVE_CONTROL) &&
                      plain_is("<SHOW_OBJECTIVE> Show Objective", &c, "[Unbound] Show Objective"));
            c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        }
        {
            jpt_gamepad_settings_t taken = cfg;
            taken.bind[JPT_ACT_JUMP] = JPT_PAD_HINT_CONTROL;
            c = prompt_context(1, JPT_FAMILY_XBOX, &taken, map);
            CHECK(!jpt_pad_control_free(&taken, JPT_PAD_HINT_CONTROL) &&
                      plain_is("<HINT> Hint", &c, "[Unbound] Hint"));
        }
    }
    CHECK(jpt_prompt_panel_kind("Press 'Q' to jump.") == JPT_PANEL_NONE && jpt_prompt_panel_kind(NULL) == JPT_PANEL_NONE);
    for (int gamepad = 0; gamepad < 2; gamepad++)
        CHECK(jpt_prompt_hint_card(gamepad)->count == 0 &&
              !strcmp(jpt_prompt_hint_card(gamepad)->title, jpt_prompt_controls_hint()));
    for (int gamepad = 0; gamepad < 2; gamepad++) {
        const jpt_card_t* card = jpt_prompt_card(gamepad);
        int tokens = 0;
        CHECK(card->count > 0 && card->count <= JPT_CARD_MAX_ROWS && card->first_column > 0 &&
              card->first_column < card->count);
        c = prompt_context(gamepad, JPT_FAMILY_XBOX, &cfg, map);
        /* every row has a binding the device can show, and every action is plain text */
        for (int i = 0; i < card->count; i++) {
            jpt_prompt_token_t tk[JPT_PROMPT_MAX_TOKENS];
            int n = jpt_prompt_text_render(card->rows[i].binding, &c, out, sizeof out, tk, JPT_PROMPT_MAX_TOKENS);
            CHECK(n > 0);
            for (int k = 0; k < n; k++) CHECK(strcmp(tk[k].label, "Unbound"));
            tokens += n > 0 ? n : 0;
            CHECK(!strchr(card->rows[i].action, '<') && strlen(card->rows[i].action) < 32);
        }
        if (gamepad) tokens++; /* the title's View button */
        CHECK(tokens <= JPT_PROMPT_MAX_TOKENS);
        for (size_t s = 0; s < sizeof screens / sizeof screens[0]; s++) {
            for (int family = JPT_FAMILY_XBOX; family <= JPT_FAMILY_PLAYSTATION; family++) {
                int w = screens[s][0], h = screens[s][1], game = jpt_ui_text_height(32, h), max_h, fit, rows_top = 1 << 30;
                int band = jpt_card_band_floor(game);
                card_measure_t cm;
                jpt_card_metrics_t m;
                jpt_card_layout_t lay;
                jpt_box_t area;
                c = prompt_context(gamepad, (jpt_pad_family_t)family, &cfg, map);
                memset(&cm, 0, sizeof cm);
                cm.card = card;
                cm.ctx = &c;
                cm.char_w = 0.62;
                jpt_card_area(0, 0, w, h, game, band, &area, &max_h);
                fit = jpt_card_fit(measure_card, &cm, max_h, JPT_CARD_FLOOR_TEXT, area, &m, &lay);
                if (fit != 1) printf("  %dx%d %s: the card does not fit\n", w, h, gamepad ? "gamepad" : "keyboard");
                CHECK(fit == 1);
                CHECK(cm.last_h == lay.text_h);
                /* smaller than the game's own text, and readable */
                CHECK(lay.text_h <= game && lay.text_h <= max_h);
                /* comfortable: the full size unless one pixel more would not fit, never under 3% of the height */
                CHECK(lay.text_h >= (h >= 720 ? h * 30 / 1000 : JPT_CARD_FLOOR_TEXT));
                if (lay.text_h < max_h) {
                    jpt_card_metrics_t bigger;
                    jpt_card_layout_t probe;
                    measure_card(&cm, lay.text_h + 1, &bigger);
                    CHECK(!jpt_card_place(&bigger, lay.text_h + 1, lay.columns, area, &probe));
                } else {
                    CHECK(lay.text_h == max_h);
                }
                CHECK(lay.line_h > lay.text_h);
                /* inside the safe area: below the game's line at the top, clear of the sides and the subtitles */
                CHECK(lay.box.left >= area.left && lay.box.right <= area.right && lay.box.top >= area.top &&
                      lay.box.bottom <= area.bottom);
                CHECK(area.top >= game + 8 && area.left >= w * 4 / 100 && area.bottom <= h - 8);
                /* rows: the binding ends before its action begins, and no two rows touch */
                for (int i = 0; i < m.count; i++) {
                    CHECK(lay.binding_x[i] + m.binding_w[i] < lay.action_x[i]);
                    CHECK(lay.binding_x[i] >= lay.box.left && lay.action_x[i] + m.action_w[i] <= lay.box.right);
                    if (lay.row_y[i] < rows_top) rows_top = lay.row_y[i];
                    for (int j = i + 1; j < m.count; j++)
                        CHECK(!boxes_meet(lay.binding_x[i], lay.row_y[i], lay.action_x[i] + m.action_w[i],
                                          lay.row_y[i] + lay.text_h, lay.binding_x[j], lay.row_y[j],
                                          lay.action_x[j] + m.action_w[j], lay.row_y[j] + lay.text_h));
                }
                /* the title is above every row and centred over the card */
                CHECK(m.title_w > 0 && lay.title_y + lay.text_h < rows_top);
                CHECK(abs((lay.title_x + m.title_w / 2) - (lay.box.left + lay.box.right) / 2) <= 1);
                /* wide screens keep two columns */
                if (w * 3 >= h * 4) {
                    if (lay.columns != 2)
                        printf("  %dx%d %s family %d: %d column\n", w, h, gamepad ? "gamepad" : "keyboard", family,
                               lay.columns);
                    CHECK(lay.columns == 2);
                }
            }
        }
    }
    {
        /* a narrow area: the text steps down, then one column, and the result is still placed */
        card_measure_t cm;
        jpt_card_metrics_t m;
        jpt_card_layout_t lay;
        jpt_box_t tiny = { 0, 0, 120, 60 };
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        memset(&cm, 0, sizeof cm);
        cm.card = jpt_prompt_card(1);
        cm.ctx = &c;
        cm.char_w = 0.62;
        CHECK(jpt_card_fit(measure_card, &cm, 40, JPT_CARD_FLOOR_TEXT, tiny, &m, &lay) == 0);
        CHECK(lay.text_h == JPT_CARD_FLOOR_TEXT && lay.columns == 1 && cm.last_h == JPT_CARD_FLOOR_TEXT);
    }
    {
        /* the hint: a title only, centred at the top of the area, with either device */
        card_measure_t cm;
        jpt_card_metrics_t m;
        jpt_card_layout_t lay;
        jpt_box_t area;
        int max_h;
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        memset(&cm, 0, sizeof cm);
        cm.card = jpt_prompt_hint_card(1);
        cm.ctx = &c;
        cm.char_w = 0.62;
        jpt_card_area(0, 0, 1920, 1080, 58, jpt_card_band_floor(58), &area, &max_h);
        CHECK(jpt_card_fit(measure_card, &cm, max_h, JPT_CARD_FLOOR_TEXT, area, &m, &lay) == 1);
        CHECK(lay.text_h == max_h && lay.title_y == area.top && m.count == 0 && lay.box.bottom == area.top + max_h);
        c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
        cm.card = jpt_prompt_hint_card(0);
        CHECK(jpt_card_fit(measure_card, &cm, max_h, JPT_CARD_FLOOR_TEXT, area, &m, &lay) == 1);
        CHECK(m.title_w > 0 && m.count == 0 && lay.title_y == area.top && lay.box.right > lay.box.left);
    }
    {
        /* the beach level's own hints (compressed in be.GRF) and every Stream.tpa line: no mouse, key or bar left */
        static const char* const hints[] = {
            "To look around, move your mouse.",
            "To look around, move the mouse.",
            "You can look around by moving the mouse.",
            "Move your mouse to look around.",
            "To adjust your wrist, hold shift + left mouse button, and move the mouse.",
            "To rotate your arm, hold ctrl + left mouse button, and move the mouse.",
            "To fire a gun, pick it up, and press the space bar.",
            "To move forward, press and hold 'W'.",
            "Press 'S' to walk, 'X' to back up, 'E' to stow and 'F' to throw.",
            "...hold the left mouse button and space bar,",
            "...press 'Z' to crouch, move your hand  towards the item,",
            " then press the right mouse button to pick it up.",
            "Press right mouse button to drop it.",
        };
        char lower[512];
        for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
            c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
            CHECK(jpt_prompt_text_has_prompts(hints[i]));
            CHECK(jpt_prompt_text_plain(hints[i], &c, out, sizeof out) > 0);
            for (size_t k = 0; k <= strlen(out) && k < sizeof lower; k++) lower[k] = (char)tolower((unsigned char)out[k]);
            if (strstr(lower, "mouse") || strstr(lower, "space bar") || strstr(lower, "shift") || strstr(lower, "ctrl") ||
                strchr(out, '\''))
                printf("  gamepad: \"%s\"\n", out);
            CHECK(!strstr(lower, "mouse") && !strstr(lower, "space bar") && !strstr(lower, "shift") &&
                  !strstr(lower, "ctrl") && !strchr(out, '\''));
        }
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        CHECK(jpt_prompt_text_has_prompts("Press 'R' to hear it again.") &&
              jpt_prompt_text_plain("Press 'R' to hear it again.", &c, out, sizeof out) < 0);
        c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
        CHECK(plain_is("Press 'R' to hear it again.", &c, "Press [R] to hear it again."));
        c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
        CHECK(plain_is("To look around, move your mouse.", &c, "To look around, move [Right stick]."));
        CHECK(plain_is("To adjust your wrist, hold shift + left mouse button, and move the mouse.", &c,
                       "To adjust your wrist, hold [Left trigger (LT)] + [Left bumper (LB)], and move [Right stick]."));
        CHECK(plain_is("To rotate your arm, hold ctrl + left mouse button, and move the mouse.", &c,
                       "To rotate your arm, hold [Left trigger (LT)] + [Right bumper (RB)], and move [Right stick]."));
        c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
        CHECK(plain_is("To adjust your wrist, hold shift + left mouse button, and move the mouse.", &c,
                       "To adjust your wrist, hold [Shift] + [Mouse Left], and move the mouse."));
        CHECK(plain_is("Press 'S' to walk, 'X' to back up, 'E' to stow and 'F' to throw.", &c,
                       "Press [S] to walk, [X] to back up, [E] to stow and [F] to throw."));
    }
    /* the game's own top line asks for the beach's forward key: it follows the device too */
    c = prompt_context(1, JPT_FAMILY_XBOX, &cfg, map);
    CHECK(plain_is("To move forward, press and hold 'W'.", &c, "To move forward, push [Left stick] forward."));
    c = prompt_context(0, JPT_FAMILY_XBOX, &cfg, map);
    CHECK(plain_is("To move forward, press and hold 'W'.", &c, "To move forward, press and hold [W]."));
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
    CHECK(guide_has(rows, n, "Grab / Drop", "X"));
    CHECK(guide_has(rows, n, "Throw", "Y"));
    CHECK(guide_has(rows, n, "Stow / Retrieve", "D-pad up"));
    CHECK(guide_has(rows, n, "Rotate wrist", "Left bumper (LB) + Left trigger (LT) + Right stick"));
    CHECK(guide_has(rows, n, "Rotate arm", "Right bumper (RB) + Left trigger (LT) + Right stick"));
    for (int i = 0; i < n; i++) CHECK(!strstr(rows[i].action, "Replay"));
    CHECK(guide_has(rows, n, "Pause menu", "Menu"));
    CHECK(guide_has(rows, n, "Show these controls in the game", "View"));
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
    CHECK(guide_has(rows, n, "Replay voice over", "R"));
    CHECK(guide_has(rows, n, "Stow / Retrieve", "E"));
    CHECK(guide_has(rows, n, "Replay voice over", "R"));
    CHECK(guide_has(rows, n, "Fire / Use", "Space"));
    CHECK(guide_has(rows, n, "Reach with the hand (hold)", "Mouse Left"));
    CHECK(guide_has(rows, n, "Move the arm", "Mouse Left + Mouse movement"));
    CHECK(guide_has(rows, n, "Grab an object", "Mouse Left + Mouse Right"));
    CHECK(guide_has(rows, n, "Drop the object", "Mouse Right"));
    CHECK(guide_has(rows, n, "Rotate arm", "Ctrl + Mouse Left + Mouse movement"));
    CHECK(guide_has(rows, n, "Rotate wrist", "Shift + Mouse Left + Mouse movement"));
    CHECK(guide_has(rows, n, "Show these controls in the game", "F2"));
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

#define L3 JPT_PAD_BIT(JPT_PAD_LEFT_STICK)
#define R3 JPT_PAD_BIT(JPT_PAD_RIGHT_STICK)

static int chord_step(pad_chord_t* c, uint32_t raw, uint32_t ms, uint32_t* out) {
    return pad_chord_update(c, L3 | R3, raw, ms, out);
}

static void test_chord(void) {
    pad_chord_t c;
    uint32_t out;
    const uint32_t other = JPT_PAD_BIT(JPT_PAD_SOUTH);
    memset(&c, 0, sizeof c);
    CHECK(!chord_step(&c, other, 0, &out) && out == other);
    /* L3 alone: held back for the window, then passed on and held */
    CHECK(!chord_step(&c, L3, 100, &out) && out == 0);
    CHECK(!chord_step(&c, L3, 100 + PAD_CHORD_WINDOW_MS - 1, &out) && out == 0);
    CHECK(!chord_step(&c, L3, 100 + PAD_CHORD_WINDOW_MS, &out) && out == L3);
    CHECK(!chord_step(&c, L3, 1000, &out) && out == L3);
    CHECK(!chord_step(&c, 0, 1010, &out) && out == 0);
    /* R3 alone */
    CHECK(!chord_step(&c, R3, 2000, &out) && out == 0);
    CHECK(!chord_step(&c, R3, 2000 + PAD_CHORD_WINDOW_MS, &out) && out == R3);
    CHECK(!chord_step(&c, 0, 2100, &out) && out == 0);
    /* a tap shorter than the window is still seen, for the tap time */
    CHECK(!chord_step(&c, L3, 3000, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 3020, &out) && out == L3);
    CHECK(!chord_step(&c, 0, 3020 + PAD_CHORD_TAP_MS - 1, &out) && out == L3);
    CHECK(!chord_step(&c, 0, 3020 + PAD_CHORD_TAP_MS, &out) && out == 0);
    /* L3 then R3 inside the window: one toggle, nothing of either reaches the game */
    CHECK(!chord_step(&c, L3 | other, 4000, &out) && out == other);
    CHECK(chord_step(&c, L3 | R3 | other, 4030, &out) && out == other);
    CHECK(!chord_step(&c, L3 | R3, 4100, &out) && out == 0);
    CHECK(!chord_step(&c, L3 | R3, 9000, &out) && out == 0);
    /* releasing one and pressing it again does not toggle: both must go up */
    CHECK(!chord_step(&c, R3, 9010, &out) && out == 0);
    CHECK(!chord_step(&c, L3 | R3, 9020, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 9030, &out) && out == 0);
    /* R3 then L3 inside the window, after releasing both */
    CHECK(!chord_step(&c, R3, 10000, &out) && out == 0);
    CHECK(chord_step(&c, L3 | R3, 10049, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 10100, &out) && out == 0);
    /* both in the same sample, and again after a release */
    CHECK(chord_step(&c, L3 | R3, 11000, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 11100, &out) && out == 0);
    CHECK(chord_step(&c, L3 | R3, 11200, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 11300, &out) && out == 0);
    /* R3 long after L3: two single presses, no chord */
    CHECK(!chord_step(&c, L3, 12000, &out) && out == 0);
    CHECK(!chord_step(&c, L3, 12000 + PAD_CHORD_WINDOW_MS, &out) && out == L3);
    CHECK(!chord_step(&c, L3 | R3, 12500, &out) && out == (L3 | R3));
    CHECK(!chord_step(&c, L3 | R3, 12500 + PAD_CHORD_WINDOW_MS, &out) && out == (L3 | R3));
    CHECK(!chord_step(&c, 0, 13000, &out) && out == 0);
    /* a clock that wrapped */
    CHECK(!chord_step(&c, L3, 0xFFFFFFF0u, &out) && out == 0);
    CHECK(chord_step(&c, L3 | R3, 5, &out) && out == 0);
    CHECK(!chord_step(&c, 0, 100, &out) && out == 0);
}

static void test_rumble_motors(void) {
    uint16_t lo, hi;
    pad_rumble_motors(0.0f, 1.0f, &lo, &hi);
    CHECK(lo == 0 && hi == 65535); /* pad_rumble_motors: the ends of the range are the ends of 16 bits */
    pad_rumble_motors(0.5f, 0.25f, &lo, &hi);
    CHECK(lo == 32767 && hi == 16383); /* pad_rumble_motors: strengths are scaled linearly to 16 bits */
    pad_rumble_motors(-2.0f, 7.0f, &lo, &hi);
    CHECK(lo == 0 && hi == 65535); /* pad_rumble_motors: strengths outside 0..1 are clamped, never wrapped */
}

int main(void) {
    test_rumble_motors();
    test_profile_activate();
    test_chord();
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
    test_crouch();
    test_stick_as_mouse();
    test_grab_and_crouch_buttons();
    test_overlay_neutral();
    test_edges();
    test_navigation();
    test_hotplug_flow();
    test_tutorial_text();
    test_tutorial_bindings();
    test_tutorial_composition();
    test_controls_card();
    test_controls_guide();
    test_launcher_controller();
    printf("gamepad_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
