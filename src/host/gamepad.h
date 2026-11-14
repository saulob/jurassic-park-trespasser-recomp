/* SDL3 gamepads next to the keyboard and mouse (gamepad.c), and their menu navigation (gamepad_menu.c). */
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <stdint.h>

#include "gamepad_policy.h"
#include "jpt_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

void gamepad_configure(const jpt_gamepad_settings_t* settings);
/* The gameplay input: CInput::tinReadDefaultControls (0x005F80B0) gets the gamepad added to its result. */
int gamepad_install(void);
void gamepad_summary(void);

/* window_host.c reports the keyboard and mouse, so prompts follow the device used last. */
void gamepad_note_keyboard(void);
void gamepad_note_mouse_button(void);
void gamepad_note_mouse_motion(int dx, int dy);
/* The game's modal UI loop, just before it reads its cursor each frame: menus, pause and restart. */
void gamepad_ui_loop(void);
/* F2: the controls card opens or closes on the next pass of that loop in a level, as with View/Back. */
void gamepad_controls_key(void);
/* The video player's message pump: a press skips the video as a key would. */
void gamepad_video_loop(void);
/* The host's policy timer: connections are noticed while no frame runs. */
void gamepad_idle(void);

/* The controller in use's buttons (JPT_PAD_BIT) at the last sample; 0 without one. */
uint32_t gamepad_buttons(void);

/* 1 while gamepad focus drives a menu: every pointer is hidden. */
int gamepad_menu_focus(void);

/* What prompts show now. */
typedef struct {
    int gamepad;                          /* the player used a gamepad last */
    jpt_pad_family_t family;              /* the glyphs to draw */
    const jpt_gamepad_settings_t* settings;
} gamepad_prompts_t;

void gamepad_prompts(gamepad_prompts_t* out);

/* Vibration of the controller in use, 0..1 per motor; 0 when it has none. No game event drives it yet. */
int gamepad_rumble(float low, float high, unsigned ms);

/* gamepad_menu.c: called by gamepad_ui_loop while a menu or dialog of the game is in front. */
void gamepad_menu_frame(const pad_state_t* pad, uint32_t held, uint32_t pressed, uint32_t released, int device_gamepad,
                        uint32_t now_ms, float dt, float stick_deadzone);
/* A different window came to the front, or menus started: focus is chosen again. */
void gamepad_menu_reset(void);
/* The mouse took the pointer: the next gamepad press starts from the control under it. */
void gamepad_menu_forget_focus(void);
int gamepad_menu_pointer_free(void);

#ifdef __cplusplus
}
#endif

#endif
