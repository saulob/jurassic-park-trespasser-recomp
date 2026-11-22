/* The host window the guest's windows are shown in. The guest's windows are logical objects (linux_user32.c); a backend owns the
 * one real window and the pictures presented into it. The virtual backend keeps every headless run windowless; the SDL3 backend
 * (linux_sdl_window.c) opens the real window in the executable. Nothing in the guest sees a backend pointer. */
#ifndef JPT_LINUX_WINDOW_H
#define JPT_LINUX_WINDOW_H

#include <stddef.h>
#include <stdint.h>

typedef enum { LX_PF_RGB565, LX_PF_XRGB8888 } lx_pixel_format_t;

typedef struct {
    const void* pixels; /* host memory; valid only for the duration of present() */
    int width, height, pitch;
    lx_pixel_format_t format;
    unsigned generation; /* the guest screen this frame belongs to (jpt_presentation_commit) */
} lx_frame_t;

typedef enum {
    LX_EV_NONE,
    LX_EV_CLOSE,
    LX_EV_KEY,         /* vk = Win32 virtual key, scancode, down, repeat */
    LX_EV_CHAR,        /* vk = UTF-16 unit */
    LX_EV_MOUSE_MOVE,  /* x, y in client pixels */
    LX_EV_MOUSE_BUTTON,/* vk = 1 left, 2 right, 4 middle; down */
    LX_EV_MOUSE_WHEEL, /* y = wheel delta (120 per notch) */
    LX_EV_FOCUS,       /* down = focus gained */
    LX_EV_RESIZE,      /* x, y = new client size */
    LX_EV_TOGGLE_FULLSCREEN
} lx_event_kind_t;

typedef struct {
    lx_event_kind_t kind;
    int x, y;
    uint32_t vk, scancode;
    int down, repeat;
    int relative;    /* LX_EV_MOUSE_MOVE in relative mode: dx and dy are the motion, x and y mean nothing */
    int synthetic;   /* made by the host (the gamepad), not by the user's keyboard or mouse: no device switch */
    float dx, dy;
} lx_event_t;

typedef struct {
    const char* title;
    int client_w, client_h;
    int resizable, fullscreen;
    int monitor; /* 1-based, primary first (the Monitor setting); 0: where the window system puts it */
} lx_window_desc_t;

typedef struct lx_window_backend lx_window_backend_t;

struct lx_window_backend {
    const char* name;
    int (*open)(lx_window_backend_t* b, const lx_window_desc_t* desc);
    void (*close)(lx_window_backend_t* b);
    void (*set_title)(lx_window_backend_t* b, const char* title);
    void (*set_client_size)(lx_window_backend_t* b, int w, int h);
    void (*client_size)(lx_window_backend_t* b, int* w, int* h);
    void (*set_visible)(lx_window_backend_t* b, int visible);
    /* Waits up to timeout_ms (0 = poll) for one event; LX_EV_NONE when none came. */
    int (*next_event)(lx_window_backend_t* b, int timeout_ms, lx_event_t* ev);
    void (*present)(lx_window_backend_t* b, const lx_frame_t* frame);
    void (*set_cursor_visible)(lx_window_backend_t* b, int visible);
    void (*set_mouse_grab)(lx_window_backend_t* b, int grab);
    /* Relative mouse mode: the pointer stays inside and hidden and motion arrives as deltas (gameplay's mouse look). */
    void (*set_relative_mouse)(lx_window_backend_t* b, int on);
    void (*destroy)(lx_window_backend_t* b);
    /* F11, a key of the shared hotkey table: windowed and fullscreen swap. */
    void (*toggle_fullscreen)(lx_window_backend_t* b);
};

/* The window the guest's windows use. A backend is installed before the guest runs; with none, the virtual one is. */
void lx_window_set_backend(lx_window_backend_t* b);
lx_window_backend_t* lx_window_backend(void);

/* A backend with no window and no display. It counts what it was asked to do and returns the events a test queued. */
typedef struct {
    int opened, visible, width, height, cursor_visible, grabbed, relative;
    unsigned grab_calls, relative_changes, cursor_changes;
    char title[128];
    unsigned presents;
    lx_frame_t last; /* the pixels pointer is cleared: only the geometry is kept */
    uint32_t last_checksum;
    unsigned events_returned;
    unsigned fullscreen_toggles;
} lx_virtual_state_t;

lx_window_backend_t* lx_virtual_backend(void);
const lx_virtual_state_t* lx_virtual_state(void);
void lx_virtual_reset(void);
/* Writes the last presented frame as a binary PPM (P6); 0 when there is none or the file cannot be written. */
int lx_virtual_dump_ppm(const char* path);
void lx_virtual_queue_event(const lx_event_t* ev);

/* Where the guest's frame lands in a client area, and what a client pixel means in the guest: jpt_presentation over the frame. */
typedef struct {
    int x, y, w, h;
} lx_view_t;
lx_view_t lx_present_view(int client_w, int client_h, int guest_w, int guest_h, int stretch);
int lx_present_to_guest(int client_w, int client_h, int guest_w, int guest_h, int stretch, int cx, int cy, int* gx, int* gy);

#ifdef JPT_LINUX_SDL_VIDEO
lx_window_backend_t* lx_sdl_backend(void);
/* Whether the SDL window is fullscreen now. */
int lx_sdl_fullscreen(void);
/* Whether the backend is in relative mouse mode. */
int lx_sdl_relative(void);
#endif

#endif
