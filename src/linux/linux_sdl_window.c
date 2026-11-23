/* The SDL3 window backend: one window, one renderer, one streaming texture. The guest's RGB565 frame is uploaded unchanged and
 * the renderer places it where jpt_presentation says (letterboxed); SDL chooses the window system (X11 or Wayland). Only the
 * executable installs this backend; the headless suites never open it. */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "jpt_app_identity.h"
#include "jpt_monitors.h"
#include "jpt_port.h"
#include "linux_log.h"
#include "linux_mouse.h"
#include "linux_crash.h"
#include "host_policy.h"
#include "linux_vulkan.h"
#include "linux_window.h"

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    int using_vulkan;
    int tex_w, tex_h;
    lx_pixel_format_t tex_format;
    int fullscreen, out_w, out_h, guest_w, guest_h;
    int mouse_grab, relative;
    uint64_t relative_since_ns;
    motion_counts_t counts;
    int video_up;
} sdl_state_t;

static sdl_state_t s;

static uint32_t vk_of(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (uint32_t)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (uint32_t)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (uint32_t)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (uint32_t)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}

static void refresh_output(void) {
    if (s.using_vulkan && s.window) {
        if (!SDL_GetWindowSizeInPixels(s.window, &s.out_w, &s.out_h)) s.out_w = s.out_h = 0;
        return;
    }
    if (s.renderer && !SDL_GetCurrentRenderOutputSize(s.renderer, &s.out_w, &s.out_h)) s.out_w = s.out_h = 0;
}

/* The player is told on screen, in SDL's own message box (no external tool), and on stderr; the log has the details. */
static void show_graphics_notice(void) {
    const char* notice = lx_vk_graphics_notice();
    const char* driver = SDL_GetCurrentVideoDriver();
    char text[640];
    if (!notice[0]) return;
    snprintf(text, sizeof text, "%s\n\nYour renderer setting was not changed. Details: %s", notice, lx_log_path());
    fprintf(stderr, "trespasser_recomp: %s\n", text);
    if (driver && strcmp(driver, "dummy") && strcmp(driver, "offscreen"))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Graphics", text, NULL);
}

/* The display the Monitor setting names (primary first, as in the launcher), 0 for the window system's choice. */
static SDL_DisplayID display_of(int monitor) {
    jpt_monitor_t m;
    if (monitor < 1 || !jpt_find_monitor(monitor, &m)) return 0;
    return (SDL_DisplayID)(size_t)m.handle;
}

static SDL_Window* create_window(const lx_window_desc_t* d, SDL_WindowFlags flags, SDL_DisplayID display) {
    SDL_PropertiesID props;
    SDL_Window* window;
    if (!display) return SDL_CreateWindow(d->title, d->client_w, d->client_h, flags);
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, d->title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, d->client_w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, d->client_h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

static int sdl_open(lx_window_backend_t* b, const lx_window_desc_t* d) {
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int want_vk = lx_vk_active();
    SDL_DisplayID display;
    (void)b;
    jpt_app_identity_apply(NULL);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) s.video_up = 1;
    else {
        lx_log("Linux presentation", "SDL3 video: %s", SDL_GetError());
        return 0;
    }
    if (d->resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (d->fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (want_vk) flags |= SDL_WINDOW_VULKAN;
    display = display_of(d->monitor);
    if (d->monitor > 0) lx_log("Linux presentation", "Monitor %d: %s", d->monitor, display ? "found" : "not found, the window system chooses");
    s.window = create_window(d, flags, display);
    if (!s.window && want_vk) {
        lx_log("Linux presentation", "SDL3 Vulkan window failed (%s); retrying Software window", SDL_GetError());
        want_vk = 0;
        flags &= ~SDL_WINDOW_VULKAN;
        s.window = create_window(d, flags, display);
    }
    if (!s.window) {
        lx_log("Linux presentation", "SDL3 window: %s", SDL_GetError());
        return 0;
    }
    s.using_vulkan = 0;
    if (want_vk) {
        int pw = d->client_w, ph = d->client_h;
        SDL_GetWindowSizeInPixels(s.window, &pw, &ph);
        if (pw <= 0 || ph <= 0) pw = d->client_w, ph = d->client_h;
        if (lx_vk_attach_window(s.window, (uint32_t)pw, (uint32_t)ph, d->fullscreen)) {
            s.using_vulkan = 1;
        }
    }
    if (!s.using_vulkan) {
        s.renderer = SDL_CreateRenderer(s.window, NULL);
        if (!s.renderer) {
            lx_log("Linux presentation", "SDL3 renderer: %s", SDL_GetError());
            SDL_DestroyWindow(s.window);
            s.window = NULL;
            return 0;
        }
        SDL_SetRenderVSync(s.renderer, 1);
    }
    jpt_app_icon_apply(s.window);
    show_graphics_notice();
    SDL_StartTextInput(s.window);
    s.fullscreen = d->fullscreen;
    refresh_output();
    lx_log("Linux presentation", "SDL3 window %dx%d, renderer %s, video driver %s, gamescope %s", d->client_w, d->client_h,
           s.using_vulkan ? "Vulkan" : SDL_GetRendererName(s.renderer), SDL_GetCurrentVideoDriver(),
           jpt_port_in_gamescope() ? "yes" : "no");
    return 1;
}

static void sdl_close(lx_window_backend_t* b) {
    (void)b;
    if (s.using_vulkan) {
        lx_vk_shutdown();
        s.using_vulkan = 0;
    }
    if (s.texture) SDL_DestroyTexture(s.texture);
    if (s.renderer) SDL_DestroyRenderer(s.renderer);
    if (s.window) SDL_DestroyWindow(s.window);
    s.texture = NULL, s.renderer = NULL, s.window = NULL;
    s.relative = 0;
    s.tex_w = s.tex_h = 0;
}

static void sdl_set_title(lx_window_backend_t* b, const char* title) {
    (void)b;
    if (s.window) SDL_SetWindowTitle(s.window, title);
}

static void sdl_set_client_size(lx_window_backend_t* b, int w, int h) {
    (void)b;
    if (s.window && !s.fullscreen) SDL_SetWindowSize(s.window, w, h);
}

static void sdl_client_size(lx_window_backend_t* b, int* w, int* h) {
    (void)b;
    if (s.window) SDL_GetWindowSizeInPixels(s.window, w, h);
    else *w = *h = 0;
}

static void sdl_set_visible(lx_window_backend_t* b, int visible) {
    (void)b;
    if (!s.window) return;
    if (visible) SDL_ShowWindow(s.window);
    else SDL_HideWindow(s.window);
}

static void to_client(float x, float y, int* cx, int* cy) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s.window, &ww, &wh);
    refresh_output();
    *cx = ww > 0 ? (int)(x * (float)s.out_w / (float)ww) : (int)x;
    *cy = wh > 0 ? (int)(y * (float)s.out_h / (float)wh) : (int)y;
}

int lx_sdl_relative(void) { return s.relative; }

int lx_sdl_fullscreen(void) { return s.window && (SDL_GetWindowFlags(s.window) & SDL_WINDOW_FULLSCREEN) != 0; }

static void sdl_toggle_fullscreen(lx_window_backend_t* b) {
    (void)b;
    if (!s.window) return;
    s.fullscreen = !s.fullscreen;
    SDL_SetWindowFullscreen(s.window, s.fullscreen != 0);
    if (s.using_vulkan) lx_vk_set_fullscreen(s.fullscreen);
}

static int sdl_next_event(lx_window_backend_t* b, int timeout_ms, lx_event_t* ev) {
    SDL_Event e;
    (void)b;
    lx_crash_present_stage(LX_PRESENT_EVENT_PUMP);
    ev->kind = LX_EV_NONE;
    if (!s.window) return 0;
    while (timeout_ms > 0 ? SDL_WaitEventTimeout(&e, timeout_ms) : SDL_PollEvent(&e)) {
        timeout_ms = 0;
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            ev->kind = LX_EV_CLOSE;
            return 1;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint32_t vk = vk_of(e.key.scancode);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F4 && (e.key.mod & SDL_KMOD_ALT)) {
                ev->kind = LX_EV_CLOSE, ev->vk = 1;
                return 1;
            }
            if (!vk) break;
            ev->kind = LX_EV_KEY, ev->vk = vk, ev->scancode = (uint32_t)e.key.scancode, ev->down = e.key.down, ev->repeat = e.key.repeat;
            return 1;
        }
        case SDL_EVENT_TEXT_INPUT:
            if (e.text.text && (unsigned char)e.text.text[0] >= 0x20 && (unsigned char)e.text.text[0] < 0x7F) {
                ev->kind = LX_EV_CHAR, ev->vk = (unsigned char)e.text.text[0];
                return 1;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            lx_mouse_note(LX_MN_RAW_MOTION);
            ev->kind = LX_EV_MOUSE_MOVE;
            if (s.relative) {
                int ww = 0, wh = 0;
                long cx, cy;
                if (!relative_motion_accepted(s.relative, e.motion.timestamp, s.relative_since_ns)) break;
                SDL_GetWindowSize(s.window, &ww, &wh);
                refresh_output();
                motion_counts(&s.counts, ww > 0 ? e.motion.xrel * (float)s.out_w / (float)ww : e.motion.xrel,
                              wh > 0 ? e.motion.yrel * (float)s.out_h / (float)wh : e.motion.yrel, &cx, &cy);
                if (!cx && !cy) break;
                ev->relative = 1, ev->dx = (float)cx, ev->dy = (float)cy;
                return 1;
            }
            to_client(e.motion.x, e.motion.y, &ev->x, &ev->y);
            return 1;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            ev->kind = LX_EV_MOUSE_BUTTON, ev->down = e.button.down;
            ev->vk = e.button.button == SDL_BUTTON_LEFT ? 1u : e.button.button == SDL_BUTTON_RIGHT ? 2u : 4u;
            to_client(e.button.x, e.button.y, &ev->x, &ev->y);
            return 1;
        case SDL_EVENT_MOUSE_WHEEL:
            ev->kind = LX_EV_MOUSE_WHEEL, ev->y = (int)(e.wheel.y * 120.0f);
            return 1;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ev->kind = LX_EV_FOCUS, ev->down = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            return 1;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            if (s.using_vulkan) lx_vk_on_resize(0, 0);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
            refresh_output();
            if (s.using_vulkan && s.out_w > 0 && s.out_h > 0) lx_vk_on_resize((uint32_t)s.out_w, (uint32_t)s.out_h);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            refresh_output();
            ev->kind = LX_EV_RESIZE, ev->x = s.out_w > 0 ? s.out_w : e.window.data1, ev->y = s.out_h > 0 ? s.out_h : e.window.data2;
            if (s.using_vulkan) lx_vk_on_resize((uint32_t)(s.out_w > 0 ? s.out_w : 0), (uint32_t)(s.out_h > 0 ? s.out_h : 0));
            return 1;
        default: break;
        }
    }
    return 0;
}

static int ensure_texture(const lx_frame_t* f) {
    if (s.texture && s.tex_w == f->width && s.tex_h == f->height && s.tex_format == f->format) return 1;
    if (s.texture) SDL_DestroyTexture(s.texture);
    s.texture = SDL_CreateTexture(s.renderer, f->format == LX_PF_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
    s.tex_w = f->width, s.tex_h = f->height, s.tex_format = f->format;
    if (!s.texture) {
        lx_log("Linux presentation", "SDL3 texture: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

static void sdl_present(lx_window_backend_t* b, const lx_frame_t* f) {
    lx_view_t v;
    SDL_FRect dst;
    (void)b;
    if (s.using_vulkan) {
        lx_crash_present_stage(LX_PRESENT_RENDER_PRESENT);
        lx_vk_present_frame(f);
        return;
    }
    if (!s.renderer) return;
    lx_crash_present_stage(LX_PRESENT_TEXTURE_LOCK);
    if (!ensure_texture(f)) return;
    lx_crash_present_stage(LX_PRESENT_TEXTURE_UPLOAD);
    SDL_UpdateTexture(s.texture, NULL, f->pixels, f->pitch);
    refresh_output();
    v = lx_present_view(s.out_w, s.out_h, f->width, f->height, 0);
    SDL_SetTextureScaleMode(s.texture, v.w == f->width && v.h == f->height ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
    lx_crash_present_stage(LX_PRESENT_RENDER_CLEAR);
    SDL_RenderClear(s.renderer);
    dst.x = (float)v.x, dst.y = (float)v.y, dst.w = (float)v.w, dst.h = (float)v.h;
    lx_crash_present_stage(LX_PRESENT_RENDER_TEXTURE);
    SDL_RenderTexture(s.renderer, s.texture, NULL, &dst);
    lx_crash_present_stage(LX_PRESENT_RENDER_PRESENT);
    SDL_RenderPresent(s.renderer);
}

static void sdl_set_cursor_visible(lx_window_backend_t* b, int visible) {
    (void)b;
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

static void sdl_set_mouse_grab(lx_window_backend_t* b, int grab) {
    (void)b;
    s.mouse_grab = grab;
    if (s.window) SDL_SetWindowMouseGrab(s.window, grab != 0);
}

static void sdl_set_relative_mouse(lx_window_backend_t* b, int on) {
    (void)b;
    if (!s.window || s.relative == (on != 0)) return;
    if (!SDL_SetWindowRelativeMouseMode(s.window, on != 0)) {
        lx_log("Linux presentation", "SDL3 relative mouse mode: %s", SDL_GetError());
        return;
    }
    s.relative = on != 0;
    s.relative_since_ns = SDL_GetTicksNS();
    memset(&s.counts, 0, sizeof s.counts);
}

static void sdl_destroy(lx_window_backend_t* b) {
    sdl_close(b);
    if (s.video_up) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    s.video_up = 0;
}

static lx_window_backend_t s_backend = { "SDL3", sdl_open, sdl_close, sdl_set_title, sdl_set_client_size, sdl_client_size, sdl_set_visible,
                                         sdl_next_event, sdl_present, sdl_set_cursor_visible, sdl_set_mouse_grab, sdl_set_relative_mouse, sdl_destroy, sdl_toggle_fullscreen };

lx_window_backend_t* lx_sdl_backend(void) { return &s_backend; }
