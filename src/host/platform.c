#include "platform.h"

#include "gamepad.h"
#include "host.h"

#include <SDL3/SDL.h>

static struct {
    sdl_life_t life;
    int gamepads;
    SDL_Window* window;
    int confined;
} P;

/* Only motion and gamepad connections are read from the queue; gamepad state is read directly. Drop events stay off so
 * SDL does not make the game's window accept dropped files. */
static void quiet_events(void) {
    static const Uint32 unused[] = {
        SDL_EVENT_KEY_DOWN,              SDL_EVENT_KEY_UP,                  SDL_EVENT_TEXT_EDITING,
        SDL_EVENT_TEXT_INPUT,            SDL_EVENT_TEXT_EDITING_CANDIDATES, SDL_EVENT_MOUSE_BUTTON_DOWN,
        SDL_EVENT_MOUSE_BUTTON_UP,       SDL_EVENT_MOUSE_WHEEL,             SDL_EVENT_JOYSTICK_AXIS_MOTION,
        SDL_EVENT_JOYSTICK_BALL_MOTION,  SDL_EVENT_JOYSTICK_HAT_MOTION,     SDL_EVENT_JOYSTICK_BUTTON_DOWN,
        SDL_EVENT_JOYSTICK_BUTTON_UP,    SDL_EVENT_JOYSTICK_BATTERY_UPDATED, SDL_EVENT_GAMEPAD_AXIS_MOTION,
        SDL_EVENT_GAMEPAD_BUTTON_DOWN,   SDL_EVENT_GAMEPAD_BUTTON_UP,       SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN,
        SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION, SDL_EVENT_GAMEPAD_TOUCHPAD_UP,   SDL_EVENT_GAMEPAD_SENSOR_UPDATE,
        SDL_EVENT_FINGER_DOWN,           SDL_EVENT_FINGER_UP,               SDL_EVENT_FINGER_MOTION,
        SDL_EVENT_FINGER_CANCELED,       SDL_EVENT_DROP_FILE,               SDL_EVENT_DROP_TEXT,
        SDL_EVENT_DROP_BEGIN,            SDL_EVENT_DROP_COMPLETE,           SDL_EVENT_DROP_POSITION,
    };
    for (size_t i = 0; i < sizeof unused / sizeof unused[0]; i++) SDL_SetEventEnabled(unused[i], false);
}

static void set_hints(void) {
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
    /* The game's window keeps Alt's system menu (SC_KEYMENU), the screensaver and its own mouse capture. */
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
    /* SDL's raw input joystick driver reads the thread's whole raw input buffer. */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

int platform_start(void) {
    int v = SDL_GetVersion();
    if (!sdl_life_may_start(&P.life)) return P.life.started && !P.life.stopped;
    set_hints();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        host_log("platform", "SDL %d.%d.%d could not start its video subsystem: %s", SDL_VERSIONNUM_MAJOR(v),
                 SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetError());
        return 0;
    }
    sdl_life_started(&P.life, GetCurrentThreadId());
    quiet_events();
    host_log("platform", "SDL %d.%d.%d (static), video driver %s; the game's own loop pumps Windows messages",
             SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetCurrentVideoDriver());
    P.gamepads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (P.gamepads) {
        quiet_events();
        host_log("gamepad", "SDL3 gamepads ready, connections followed while the game runs; the keyboard and mouse stay "
                 "active");
    } else {
        host_log("gamepad", "SDL3 gamepad support could not start (%s): keyboard and mouse only", SDL_GetError());
    }
    host_log("audio", "the game's DirectSound is the system DSound.dll; SDL audio is not started");
    return 1;
}

void platform_stop(void) {
    if (!sdl_life_may_stop(&P.life, GetCurrentThreadId())) return;
    sdl_life_stopped(&P.life);
    P.gamepads = 0;
    P.window = NULL;
    SDL_Quit();
    host_log("platform", "SDL stopped");
}

int platform_gamepads_ready(void) { return P.gamepads && !P.life.stopped; }

int platform_window_attach(HWND window) {
    SDL_PropertiesID props;
    if (!P.life.started || P.life.stopped || P.window) return P.window != NULL;
    props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, window);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    P.window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!P.window) {
        host_log("platform", "the game window could not become an SDL window: %s", SDL_GetError());
        return 0;
    }
    /* SDL registers the window for multi-touch; the game's window keeps Windows' touch-as-mouse. */
    UnregisterTouchWindow(window);
    host_log("platform", "game window 0x%08X is SDL window %u", (uint32_t)(uintptr_t)window,
             (unsigned)SDL_GetWindowID(P.window));
    return 1;
}

void platform_window_detach(void) {
    SDL_Window* window = P.window;
    if (!window || GetCurrentThreadId() != P.life.thread) return;
    P.window = NULL;
    P.confined = 0;
    SDL_DestroyWindow(window);
}

int platform_window_attached(void) { return P.window != NULL; }

int platform_relative_mouse(int on) {
    if (!P.window) return 0;
    return SDL_SetWindowRelativeMouseMode(P.window, on != 0);
}

void platform_confine_mouse(const RECT* rect) {
    SDL_Rect r;
    if (!P.window) return;
    if (!rect) {
        if (P.confined) SDL_SetWindowMouseRect(P.window, NULL);
        P.confined = 0;
        return;
    }
    r.x = rect->left;
    r.y = rect->top;
    r.w = rect->right - rect->left;
    r.h = rect->bottom - rect->top;
    P.confined = SDL_SetWindowMouseRect(P.window, &r);
}

uint64_t platform_ticks_ns(void) { return SDL_GetTicksNS(); }

int platform_mouse_buttons_held(void) {
    const SDL_MouseButtonFlags buttons = SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK;
    if (!P.life.started || P.life.stopped) return 0;
    return (SDL_GetGlobalMouseState(NULL, NULL) & buttons) != 0;
}

int platform_vk_held(unsigned vk) {
    const bool* keys;
    if (!P.life.started || P.life.stopped) return 0;
    keys = SDL_GetKeyboardState(NULL);
    switch (vk) {
    case VK_SPACE: return keys[SDL_SCANCODE_SPACE];
    case VK_CONTROL: return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    case VK_SHIFT: return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        SDL_Keycode key = vk >= 'A' && vk <= 'Z' ? (SDL_Keycode)(SDLK_A + (vk - 'A')) : (SDL_Keycode)(SDLK_0 + (vk - '0'));
        SDL_Scancode code = SDL_GetScancodeFromKey(key, NULL);
        return code != SDL_SCANCODE_UNKNOWN && keys[code];
    }
    return 0;
}

void platform_pump(void) {
    SDL_Event events[32];
    int n;
    if (!P.life.started || P.life.stopped || GetCurrentThreadId() != P.life.thread) return;
    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(events, 32, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0)
        for (int i = 0; i < n; i++) {
            switch (events[i].type) {
            case SDL_EVENT_MOUSE_MOTION:
                window_host_mouse_motion(events[i].motion.xrel, events[i].motion.yrel, events[i].motion.timestamp);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (P.gamepads) gamepad_device_added((uint32_t)events[i].gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (P.gamepads) gamepad_device_removed((uint32_t)events[i].gdevice.which);
                break;
            default: break;
            }
        }
}
