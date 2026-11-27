/* The host's menus over the game and its HUD on both hosts (debug_menu.c): F10 Cheats in a level or with the main menu alone
 * in front, F3 Objectives in a level, the compass and the minimap. The model and the actions are shared; the frame
 * (debug_menu_fill_frame) is what a renderer draws and the key, pointer and pad events are what a window feeds it. */
#ifndef JPT_DEBUG_MENU_H
#define JPT_DEBUG_MENU_H

#include <stdint.h>

#include "debug_menu_layout.h"
#include "debug_menu_panel.h"

void debug_menu_f10(void);
void debug_menu_f3(void);
void debug_menu_show_objective(void);
/* A short line on the HUD under the compass, in a level. */
void debug_menu_notice(const char* text);
/* 0 closed, 1 Cheats, 2 Objectives. */
int debug_menu_open_menu(void);
void debug_menu_close(void);
int debug_menu_positions(float player[3], float camera[3]);
int debug_menu_visible(void);
/* While open in a level the game's CInput result is emptied: Anne does not move or look. */
int debug_menu_blocks_gameplay(void);
/* The game's modal UI loop, after the gamepad: requested actions run here. */
void debug_menu_ui_loop(void);
/* Loads the active custom level on the main menu's first turn, before the menu draws. */
void debug_menu_start_custom_level(void);
/* 1 while that level is still to load: a present now would show the main menu. */
int debug_menu_start_holds_present(void);

/* 1 when the game must not see this key; vk is a Windows virtual-key code. */
int debug_menu_key_event(unsigned vk, int down, int repeat, int shift, int control);
int debug_menu_char(char c);
/* A pointer move or click over the open menu, in the presentation surface's pixels: 1 when the menu took it. */
int debug_menu_pointer(int click, int x, int y);
/* Gamepad buttons (JPT_PAD_BIT) while the menu is open. */
void debug_menu_pad(uint32_t held, uint32_t pressed);

/* What the renderer provides: where the drawn menu can be clicked, and a call when the menu opens or closes. */
void debug_menu_set_hooks(void (*input_changed)(void), const dml_hits_t* (*hits)(void));
int debug_menu_overlay_wanted(int frame_counter_on);
int debug_menu_hud_on_screen(void);
/* The menu and HUD parts of the frame (rows, titles, help, the HUD); the view, the text layer and the counter are the
 * renderer's. */
void debug_menu_fill_frame(dmp_frame_t* frame);

#endif
