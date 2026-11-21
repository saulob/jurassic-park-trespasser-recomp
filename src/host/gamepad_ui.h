/* What the gamepad does in the game's UI loop, on both hosts: the edges of its buttons, the shortcuts over gameplay (Objectives, Hint, Show
 * Objective, the minimap chord, the controls card), the host menu's own pad navigation and the game's menus through gamepad_menu.c. The
 * host supplies this frame's pad and the clock; the state is the caller's. */
#ifndef JPT_GAMEPAD_UI_H
#define JPT_GAMEPAD_UI_H

#include <stdint.h>

#include "gamepad_policy.h"
#include "jpt_gamepad.h"

typedef enum { GUI_NONE, GUI_GAMEPLAY, GUI_MENU, GUI_VIDEO } gamepad_ui_mode_t;

typedef struct {
    pad_edges_t edges;
    gamepad_ui_mode_t mode;
    uint32_t top;
    int hint_shown;
    int left_gameplay; /* a window of the game or a menu came in front since gameplay last read the pad */
} gamepad_ui_t;

typedef struct {
    const pad_state_t* pad;
    const jpt_gamepad_settings_t* cfg;
    uint32_t buttons;
    uint32_t now_ms;
    float dt;
    int device_gamepad;
    int chord;
    float stick_deadzone;
    void (*toggle_minimap)(const char* source);
} gamepad_ui_in_t;

void gamepad_ui_step(gamepad_ui_t* ui, const gamepad_ui_in_t* in);
/* An interface of the host opened or closed: what is held waits for its release. */
void gamepad_ui_overlay_changed(gamepad_ui_t* ui, uint32_t buttons);
void gamepad_ui_video(gamepad_ui_t* ui, uint32_t buttons);

#endif
