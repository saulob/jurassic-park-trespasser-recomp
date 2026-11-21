#include "host.h"
#include "host_input.h"

#include "gamepad.h"
#include "platform.h"

void host_post_key(unsigned vk) { window_host_post_key(vk); }

void host_post_key_up(unsigned vk) { window_host_post_key_up(vk); }

void host_pad_cursor_set(int x, int y) { window_host_set_pad_cursor(x, y); }

int host_game_cursor(int* x, int* y) { return window_host_game_cursor(x, y); }

void host_post_mouse(int kind) { window_host_post_mouse(kind); }

void host_post_char(char c) { window_host_post_char(c); }

void host_overlay_changed(void) { gamepad_overlay_changed(); }

int overlay_key(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return 0;
    return overlay_key_event((unsigned)wparam, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, (lparam & 0x40000000) != 0);
}

int host_key_held(unsigned vk) { return platform_vk_held(vk); }

uint32_t host_pad_buttons(void) { return gamepad_buttons(); }

int host_input_focused(void) { return window_host_input_focused(); }

int host_panel_visible(void) { return debug_menu_visible(); }
