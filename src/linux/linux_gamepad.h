/* SDL3 gamepads on Linux through the policy layer the Windows host uses (gamepad_policy.h): connection and hot-plug, the
 * controller in use, gameplay input added to the game's own CInput read, and rumble. Everything runs on the guest thread. */
#ifndef JPT_LINUX_GAMEPAD_H
#define JPT_LINUX_GAMEPAD_H

#include <stdint.h>

#include "gamepad_policy.h"
#include "jpt_gamepad.h"

/* Where controllers come from: SDL3 by default; a suite installs its own to drive the layer without a device. */
typedef struct {
    char name[64];
    jpt_pad_type_t type;
    uint16_t vendor, product;
    int rumble;
} lx_pad_info_t;

typedef struct {
    int (*init)(void);
    void (*quit)(void);
    void (*update)(void);
    int (*list)(uint32_t* ids, int capacity);
    void* (*open)(uint32_t id);
    void (*close)(void* handle);
    void (*read)(void* handle, pad_state_t* out);
    void (*info)(void* handle, lx_pad_info_t* out);
    int (*rumble)(void* handle, uint16_t low, uint16_t high, unsigned ms);
} lx_pad_backend_t;

/* NULL restores SDL3. Set before lx_gamepad_init. */
void lx_gamepad_set_backend(const lx_pad_backend_t* backend);

/* CInput::tinReadDefaultControls: the function the gamepad is added to. */
#define LX_CINPUT_READ_VA 0x005F80B0u

/* Starts SDL's gamepad subsystem; 0 when SDL has none (the build without joystick support). */
int lx_gamepad_init(const jpt_gamepad_settings_t* settings);
void lx_gamepad_shutdown(void);

/* Reads the controllers: new and removed ones are noticed, the one in use is chosen. */
void lx_gamepad_poll(void);

/* The hook of CInput's read, `self` being its this pointer: runs `original` (the lifted function) with the pad's look
 * before it and its commands and move after it. */
void lx_gamepad_read_around(uint32_t self, void (*original)(void));

/* The pad in the game's UI loop (the shared gamepad_ui_step): shortcuts over gameplay, the host menus and the game's own menus. */
void lx_gamepad_ui_loop(void);
void lx_gamepad_overlay_changed(void);

int lx_gamepad_connected(void);
/* The state of the controller in use at the last poll. */
const pad_state_t* lx_gamepad_state(void);
/* The device the player used last. */
pad_device_t lx_gamepad_device(void);
void lx_gamepad_note_keyboard(void);
void lx_gamepad_note_mouse_button(void);
void lx_gamepad_note_mouse_motion(int dx, int dy);
/* 0..1 per motor for ms milliseconds on the controller in use; 0 when there is none or it cannot rumble. */
int lx_gamepad_rumble(float low, float high, unsigned ms);

typedef struct {
    unsigned connections, disconnections, switches, play_frames, rumble_requests, keys_posted;
} lx_gamepad_stats_t;
void lx_gamepad_stats(lx_gamepad_stats_t* out);
void lx_gamepad_log_summary(void);

#endif
