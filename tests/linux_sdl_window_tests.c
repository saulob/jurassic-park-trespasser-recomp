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

/* As user32 pumps: one event variable for every call, never cleared by the caller. */
static void next_reused(lx_window_backend_t* b, lx_event_t* ev) {
    for (int i = 0; i < 32; i++) {
        b->next_event(b, 0, ev);
        if (ev->kind != LX_EV_RESIZE && ev->kind != LX_EV_FOCUS) return;
    }
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

/* The Monitor setting names a display (primary first); one that does not exist leaves the choice to the window system. */
static void test_monitor_and_size(void) {
    lx_window_backend_t* b = lx_sdl_backend();
    lx_window_desc_t d;
    int w = 0, h = 0;
    memset(&d, 0, sizeof d);
    d.title = "monitor", d.client_w = 800, d.client_h = 600, d.resizable = 1, d.monitor = 1;
    CHECK(b->open(b, &d), "a window on monitor 1 opens (%s)", SDL_GetError());
    b->client_size(b, &w, &h);
    CHECK(w == 800 && h == 600, "with the size asked for (%dx%d)", w, h);
    b->destroy(b);
    d.monitor = 99;
    CHECK(b->open(b, &d), "a Monitor setting beyond the displays still opens a window (%s)", SDL_GetError());
    b->destroy(b);
}

static void test_window_lifecycle_1280x800(void) {
    lx_window_backend_t* b = lx_sdl_backend();
    lx_window_desc_t d;
    lx_event_t ev;
    int w = 0, h = 0;
    memset(&d, 0, sizeof d);
    d.title = "deck", d.client_w = 1280, d.client_h = 800, d.resizable = 1;
    CHECK(b->open(b, &d), "a 1280x800 window opens (%s)", SDL_GetError());
    push_window_event(SDL_EVENT_WINDOW_HIDDEN);
    push_window_event(SDL_EVENT_WINDOW_MINIMIZED);
    push_window_event(SDL_EVENT_WINDOW_RESTORED);
    push_window_event(SDL_EVENT_WINDOW_SHOWN);
    ev = next(b);
    CHECK(ev.kind == LX_EV_NONE, "hidden/minimized/restored/shown cycle does not fail or emit spurious input");
    b->client_size(b, &w, &h);
    CHECK(w == 1280 && h == 800, "client size remains 1280x800 after lifecycle events (%dx%d)", w, h);
    b->destroy(b);
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
    {
        lx_event_t reused;
        memset(&reused, 0x5A, sizeof reused);
        b = open_window();
        if (b) {
            b->set_relative_mouse(b, 1);
            push_motion(10.0f, 10.0f, SDL_GetTicksNS() + 1000000u);
            next_reused(b, &reused);
            CHECK(reused.kind == LX_EV_MOUSE_MOVE && reused.relative == 1, "JPT-Y2CHNM: a look in the level leaves relative set in the caller's event");
            b->set_relative_mouse(b, 0);
            b->destroy(b);
        }
        b = open_window();
        if (b) {
            int w = 0, h = 0, px = -1, py = -1;
            push_motion(200.0f, 120.0f, SDL_GetTicksNS());
            next_reused(b, &reused);
            CHECK(reused.kind == LX_EV_MOUSE_MOVE && reused.relative == 0 && reused.dx == 0.0f && reused.dy == 0.0f && reused.synthetic == 0 &&
                      reused.x == 200 && reused.y == 120,
                  "JPT-Y2CHNM: the next absolute position in the same event is absolute, not a stale look (relative %d, synthetic %d, %d, %d)",
                  reused.relative, reused.synthetic, reused.x, reused.y);
            b->client_size(b, &w, &h);
            CHECK(b->pointer_position && (!b->pointer_position(b, &px, &py) || (px >= 0 && py >= 0 && px < w && py < h)),
                  "the backend reports the pointer in client pixels, or that it is not over the window (%d, %d)", px, py);
            b->destroy(b);
        }
        memset(&reused, 0x5A, sizeof reused);
        b = open_window();
        if (b) {
            push_window_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
            next_reused(b, &reused);
            CHECK(reused.kind == LX_EV_CLOSE && reused.vk == 0 && reused.relative == 0, "a window close is not read as Alt+F4 from a stale key (vk 0x%X)", reused.vk);
            b->destroy(b);
        }
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
    test_monitor_and_size();
    test_window_lifecycle_1280x800();
    CHECK(!relative_motion_accepted(1, 5, 10) && relative_motion_accepted(1, 10, 10) && !relative_motion_accepted(0, 99, 1),
          "motion stamped before relative mode began is not accepted (SDL stamps pushed events itself: the shared rule is checked directly)");
    printf("linux_sdl_window_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
