/* The host features that change the lifted game, on Linux: the shared sources of src/host written against guest_abi.h
 * (guest_features.h, the F3 and F10 menus, the minimap), installed over the lift's stable entries after the guest image is
 * verified, and run from the game's UI loop. */
#ifndef JPT_LINUX_FEATURES_H
#define JPT_LINUX_FEATURES_H

#include "jpt_config.h"

/* Activates the profile of `version`, installs the overrides of the settings' features and checks them; 0 when a required one
 * is missing. game_dir is the folder holding the game's data. */
int lx_features_start(const jpt_config_t* cfg, const char* game_dir, const char* version, const char* settings_file, const char* cache_dir);
void lx_features_stop(void);
/* The features are installed over the running guest. */
int lx_features_running(void);
/* The game's CInput read around which the host blocks gameplay while one of its menus is open and applies the cheats' input; the
 * gamepad's own hook (linux_gamepad.h) runs inside it. Installed by lx_features_start, after guest_overrides_enable. */
int lx_features_install_cinput(void);
/* The pointer over an open menu, in guest pixels: 1 when the menu took it (the game must not get it). */
int lx_features_pointer_move(int x, int y);
int lx_features_pointer_button(unsigned button, int down, int x, int y);
/* F5 and the pad's L3 + R3: the minimap on or off, saved to trespasser.ini. */
void lx_features_toggle_minimap(const char* source);
/* The game's UI loop (the call of GetCursorPos from sub_00417840): the menus, the cheats and the camera. */
void lx_host_ui_loop(void);

#endif
