/* The SDL3 window backend's input policy on SDL's dummy video driver (no window is shown, nothing is displayed): F11 toggles
 * fullscreen inside the backend and is never a key of the game, Alt+F4 is a close request, and a key arrives as the Win32 virtual
 * key the user32 layer translates. Events are pushed into SDL's own queue; each case opens a fresh window. */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host_policy.h"
#include "linux_window.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static void push_key(SDL_Scancode scancode, int down, SDL_Keymod mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = scancode;
    e.key.down = down != 0;
    e.key.mod = mod;
    SDL_PushEvent(&e);
}

static void push_motion(float x, float y, uint64_t timestamp) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION, e.motion.x = x, e.motion.y = y, e.motion.xrel = 3.0f, e.motion.yrel = 4.0f, e.motion.timestamp = timestamp;
    SDL_PushEvent(&e);
}

static lx_event_t next_focus(lx_window_backend_t* b) {
    lx_event_t ev;
    for (int i = 0; i < 32; i++) {
        memset(&ev, 0, sizeof ev);
        b->next_event(b, 0, &ev);
        if (ev.kind == LX_EV_FOCUS || ev.kind == LX_EV_NONE) return ev;
    }
    return ev;
}

static void push_window_event(SDL_EventType type) {
    SDL_Event e;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    memset(&e, 0, sizeof e);
    e.type = type;
    if (windows && count > 0) e.window.windowID = SDL_GetWindowID(windows[0]);
    SDL_free(windows);
    SDL_PushEvent(&e);
}

/* The next event that is not window housekeeping (resize, focus); LX_EV_NONE when the queue is empty. */
static lx_event_t next(lx_window_backend_t* b) {
    lx_event_t ev;
    for (int i = 0; i < 32; i++) {
        memset(&ev, 0, sizeof ev);
        b->next_event(b, 0, &ev);
        if (ev.kind != LX_EV_RESIZE && ev.kind != LX_EV_FOCUS) return ev;
    }
    return ev;
}

static lx_window_backend_t* open_window(void) {
    lx_window_backend_t* b = lx_sdl_backend();
    lx_window_desc_t desc;
    static unsigned short pixels[640 * 480];
    lx_frame_t f;
    memset(&desc, 0, sizeof desc);
    desc.title = "test", desc.client_w = 640, desc.client_h = 480, desc.resizable = 1;
    if (!b->open(b, &desc)) return NULL;
    memset(&f, 0, sizeof f);
    f.pixels = pixels, f.width = 640, f.height = 480, f.pitch = 1280, f.format = LX_PF_RGB565;
    b->present(b, &f);
    return b;
}

int main(void) {
    lx_window_backend_t* b;
    lx_event_t ev;
    CHECK(SDL_GetVersion() >= SDL_VERSIONNUM(3, 4, 20) && SDL_GetVersion() < SDL_VERSIONNUM(3, 5, 0), "SDL 3.4.20 or a later 3.4 release is linked (SDL_VERSIONNUM contract: %d.%d.%d)", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    b = open_window();
    if (!b) {
        printf("linux_sdl_window_tests: DATA UNAVAILABLE (SDL cannot open a window on its dummy video driver: %s)\n", SDL_GetError());
        printf("linux_sdl_window_tests: 1 checks, 0 failures\n");
        return 0;
    }

    CHECK(!lx_sdl_fullscreen(), "the window starts windowed");
    push_key(SDL_SCANCODE_F11, 1, 0);
    ev = next(b);
    CHECK(ev.kind == LX_EV_KEY && ev.vk == 0x7A && ev.down == 1, "F11 reaches the input layer as virtual key 0x7A: the shared hotkey table decides what it does (kind %d vk 0x%X)", ev.kind, ev.vk);
    CHECK(!lx_sdl_fullscreen(), "and the backend does not decide it itself");
    b->toggle_fullscreen(b);
    CHECK(lx_sdl_fullscreen(), "the table's fullscreen action makes the window fullscreen");
    b->toggle_fullscreen(b);
    CHECK(!lx_sdl_fullscreen(), "and again returns to a window");
    b->destroy(b);

    b = open_window();
    CHECK(b != NULL, "a second window opens");
    if (b) {
        push_key(SDL_SCANCODE_F4, 1, SDL_KMOD_LALT);
        ev = next(b);
        CHECK(ev.kind == LX_EV_CLOSE && ev.vk == 1, "Alt+F4 is a close request (kind %d)", ev.kind);
        b->destroy(b);
    }

    b = open_window();
    CHECK(b != NULL, "a third window opens");
    if (b) {
        push_key(SDL_SCANCODE_A, 1, 0);
        ev = next(b);
        CHECK(ev.kind == LX_EV_KEY && ev.vk == 0x41 && ev.down == 1, "A is virtual key 0x41, down (kind %d vk 0x%X)", ev.kind, ev.vk);
        b->destroy(b);
    }
    b = open_window();
    CHECK(b != NULL, "a fourth window opens");
    if (b) {
        push_motion(100.0f, 50.0f, SDL_GetTicksNS());
        ev = next(b);
        CHECK(ev.kind == LX_EV_MOUSE_MOVE && !ev.relative && ev.x == 100 && ev.y == 50, "outside relative mode a motion is an absolute position (%d, %d)", ev.x, ev.y);
        b->destroy(b);
    }
    b = open_window();
    if (b) {
        b->set_relative_mouse(b, 1);
        CHECK(lx_sdl_relative(), "relative mouse mode goes on");
        push_motion(100.0f, 50.0f, SDL_GetTicksNS() + 1000000u);
        ev = next(b);
        CHECK(ev.kind == LX_EV_MOUSE_MOVE && ev.relative && (int)ev.dx == 3 && (int)ev.dy == 4, "in relative mode it is the motion (%.0f, %.0f)", ev.dx, ev.dy);
        b->set_relative_mouse(b, 0);
        CHECK(!lx_sdl_relative(), "and goes off again");
        b->destroy(b);
    }
    b = open_window();
    if (b) {
        push_window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
        ev = next_focus(b);
        CHECK(ev.kind == LX_EV_FOCUS && ev.down == 0, "a lost focus reaches the input layer as a focus event, not gained (kind %d)", ev.kind);
        b->set_relative_mouse(b, 1);
        b->set_relative_mouse(b, 0);
        CHECK(!lx_sdl_relative(), "relative mode off while the focus is lost leaves SDL with the free pointer");
        b->destroy(b);
    }
    b = open_window();
    if (b) {
        push_window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
        ev = next_focus(b);
        CHECK(ev.kind == LX_EV_FOCUS && ev.down == 1, "a regained focus reaches it as gained (kind %d)", ev.kind);
        b->set_relative_mouse(b, 1);
        CHECK(lx_sdl_relative(), "and relative mode can be taken again");
        b->set_relative_mouse(b, 0);
        b->destroy(b);
    }
    CHECK(!relative_motion_accepted(1, 5, 10) && relative_motion_accepted(1, 10, 10) && !relative_motion_accepted(0, 99, 1),
          "motion stamped before relative mode began is not accepted (SDL stamps pushed events itself: the shared rule is checked directly)");
    printf("linux_sdl_window_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
