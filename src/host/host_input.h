/* What the host features read from the keyboard, the gamepad and the panels, behind one interface for both hosts. */
#ifndef JPT_HOST_INPUT_H
#define JPT_HOST_INPUT_H

#include <stdint.h>

/* Windows virtual-key codes, the numbering both hosts use for keys. */
enum { HOST_VK_ESCAPE = 0x1B, HOST_VK_SHIFT = 0x10, HOST_VK_CONTROL = 0x11, HOST_VK_SPACE = 0x20, HOST_VK_F1 = 0x70 };

/* Whether the key (a Windows virtual-key code) is held now. */
int host_key_held(unsigned vk);
/* The gamepad's buttons held now (JPT_PAD_BIT). */
uint32_t host_pad_buttons(void);
/* The game window has the keyboard. */
int host_input_focused(void);
/* An F3 or F10 panel is open: it takes the keyboard and the pad. */
int host_panel_visible(void);

/* The pad's pointer and clicks in the game's menus: the game's own pointer is placed on a control and a click is the mouse message its
 * window expects. */
enum { HOST_MOUSE_DOWN, HOST_MOUSE_UP, HOST_MOUSE_DOUBLE, HOST_MOUSE_MOVE };
void host_pad_cursor_set(int x, int y);
/* The game's pointer, in its own pixels; 0 when it cannot be read yet. */
int host_game_cursor(int* x, int* y);
void host_post_mouse(int kind);
void host_post_char(char c);

/* A key press and release the game reads as typed (the game's own Hint on F1). */
void host_post_key(unsigned vk);
/* Only the release of a key (a video takes it as skip). */
void host_post_key_up(unsigned vk);
/* An interface of the host opened or closed: gamepad buttons held now wait for their release. */
void host_overlay_changed(void);

/* overlay.c: the one owner of the host's interfaces over gameplay (overlay_policy.h kinds). */
void overlay_request(int kind, const char* source);
void overlay_ui_loop(void);
int overlay_blocks_gameplay(void);
/* Every key first: 1 when the game must not see it. */
int overlay_key_event(unsigned vk, int down, int repeat);
/* F5 and L3 + R3: flips the minimap in gameplay, saves it to the settings file (NULL: not saved) and says so on the HUD. */
void overlay_toggle_minimap(const char* source, const char* settings_path);

#endif
