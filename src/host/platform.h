#ifndef TRESPASSER_PLATFORM_H
#define TRESPASSER_PLATFORM_H

/* SDL3 behind the host: the game window as an SDL window, the mouse's relative motion and the gamepads. SDL starts on
 * the guest thread, which creates and pumps the game's window, so that thread is SDL's video thread; SDL never pumps
 * Windows messages itself (the game's own loop dispatches them through SDL's window procedure), and its events are
 * drained by platform_pump on the same thread. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* 0 when SDL's video could not start (logged with SDL's error). */
int platform_start(void);
/* Idempotent; only the starting thread stops SDL. */
void platform_stop(void);
int platform_gamepads_ready(void);

/* The game's window becomes an SDL window. SDL's procedure goes in front of the current one and passes every message on
 * to it; detaching puts that procedure back. */
int platform_window_attach(HWND window);
void platform_window_detach(void);
int platform_window_attached(void);

/* Gameplay mouse-look: SDL hides the cursor, confines it and reports device motion. 0 when SDL refused. */
int platform_relative_mouse(int on);
/* Without relative mode: the cursor kept inside rect (client pixels), or freed with NULL. */
void platform_confine_mouse(const RECT* rect);
uint64_t platform_ticks_ns(void);
int platform_mouse_buttons_held(void);
/* A key held, by Windows virtual-key code (letters, digits, Space, Ctrl, Shift), as SDL's keyboard state has it. */
int platform_vk_held(unsigned vk);

/* SDL's queue: device motion to window_host_mouse_motion, gamepad connections to gamepad.c. */
void platform_pump(void);

#endif
