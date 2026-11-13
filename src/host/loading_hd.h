/* The HD loading screen: the game's loading window (CLoaderWnd) and its "Loading Level" message drawn again at the
 * screen's resolution over the game's own presents, with Direct3D 11 only. The game keeps every rule: what loads, in
 * which order, the progress it reports and when the screen is presented. */
#ifndef LOADING_HD_H
#define LOADING_HD_H

#include <stdint.h>

#include "loading_layout.h"

void loading_hd_install(void);
/* 1 when a present now is one of the blank ones around a level load and the screen keeps its last picture. */
int loading_hd_present_held(void);
/* A UI frame begins: the hold ends, and what a level load kept is freed. */
void loading_hd_ui_frame(void);
/* The game's screen size the HD loading screen is painted at; 0 with the software renderer or no display. */
int loading_hd_screen(int* w, int* h);
/* The loading window as the HD layer shows it, and the HD art slot of its picture ("" when none); 0 when the game's
 * own loading screen must be shown. */
int loading_hd_read(uint32_t loader, int screen_w, int screen_h, ld_scene_t* s, char* slot, int slot_cap);

#endif
