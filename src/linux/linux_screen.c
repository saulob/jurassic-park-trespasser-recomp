#include "linux_screen.h"
#include "debug_menu.h"

#include <string.h>

#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_sync.h"
#include "linux_crash.h"
#include "linux_frontend.h"
#include "linux_log.h"
#include "linux_overlay.h"
#include "linux_quick.h"
#include "linux_settings.h"
#include "linux_spine.h"
#include "linux_video_subtitles.h"

#define DEFAULT_W 640
#define DEFAULT_H 480
#define DEFAULT_BPP 16

static lx_display_mode_t s_mode = { DEFAULT_W, DEFAULT_H, DEFAULT_BPP };
static jpt_presentation_t s_pres;
static int s_pres_ready, s_open;
static unsigned s_frames, s_frame_limit, s_generation;
static lx_frame_hook_fn s_hook;
static void* s_hook_user;

static void ensure_presentation(void) {
    if (s_pres_ready) return;
    jpt_presentation_init(&s_pres, 0);
    s_pres_ready = 1;
}

void lx_screen_reset(void) {
    s_mode.width = DEFAULT_W;
    s_mode.height = DEFAULT_H;
    s_mode.bpp = DEFAULT_BPP;
    s_pres_ready = 0;
    s_open = 0;
    s_frames = s_frame_limit = s_generation = 0;
    s_hook = NULL, s_hook_user = NULL;
}

lx_display_mode_t lx_screen_mode(void) { return s_mode; }

void lx_screen_set_mode(int width, int height, int bpp) {
    if (width <= 0 || height <= 0) return;
    s_mode.width = width;
    s_mode.height = height;
    s_mode.bpp = bpp;
    ensure_presentation();
    s_generation++;
    jpt_presentation_request_guest(&s_pres, width, height, s_generation);
    lx_log("Linux DDRAW", "display mode %dx%dx%d (generation %u)", width, height, bpp, s_generation);
}

int lx_screen_window_open(const char* title, int client_w, int client_h) {
    lx_window_desc_t d;
    if (s_open) return 1;
    d.title = title ? title : "";
    d.client_w = client_w;
    d.client_h = client_h;
    d.resizable = 1;
    d.fullscreen = 0;
    d.monitor = 0;
    if (lx_window_backend()->name && !strcmp(lx_window_backend()->name, "SDL3")) {
        const jpt_config_t* settings = lx_settings_current();
        if (settings->window_w > 0 && settings->window_h > 0) d.client_w = client_w = settings->window_w, d.client_h = client_h = settings->window_h;
        d.fullscreen = settings->borderless;
        d.monitor = settings->monitor;
    }
    if (!lx_window_backend()->open(lx_window_backend(), &d)) return 0;
    ensure_presentation();
    jpt_presentation_set_host(&s_pres, client_w, client_h);
    jpt_presentation_set_guest(&s_pres, s_mode.width, s_mode.height);
    s_open = 1;
    lx_log("Linux presentation", "%s window %dx%d for a %dx%d screen", lx_window_backend()->name, client_w, client_h, s_mode.width,
           s_mode.height);
    return 1;
}

int lx_screen_window_is_open(void) { return s_open; }

void lx_screen_window_close(void) {
    if (!s_open) return;
    lx_window_backend()->close(lx_window_backend());
    s_open = 0;
}

void lx_screen_set_title(const char* title) {
    if (s_open) lx_window_backend()->set_title(lx_window_backend(), title);
}

void lx_screen_set_visible(int visible) {
    if (s_open) lx_window_backend()->set_visible(lx_window_backend(), visible);
}

void lx_screen_window_client(int* w, int* h) {
    if (s_open) lx_window_backend()->client_size(lx_window_backend(), w, h);
    else *w = s_mode.width, *h = s_mode.height;
}

/* A Window size the player chose is a size the guest's own resizing never changes (Window size Auto follows the guest). */
static int fixed_window_size(void) {
    const jpt_config_t* settings = lx_settings_current();
    return lx_window_backend()->name && !strcmp(lx_window_backend()->name, "SDL3") && settings->window_w > 0 && settings->window_h > 0;
}

void lx_screen_set_client_size(int w, int h) {
    if (!s_open || w <= 0 || h <= 0 || fixed_window_size()) return;
    lx_window_backend()->set_client_size(lx_window_backend(), w, h);
    ensure_presentation();
    jpt_presentation_set_host(&s_pres, w, h);
}

static void frame_presented(int width, int height, const char* kind) {
    if (++s_frames == 1) lx_log("Linux presentation", "first frame %dx%d %s presented", width, height, kind);
    if (lx_quick_pending()) lx_quick_pump(lx_quick_guest_ops());
    if (s_hook) {
        lx_crash_present_stage(LX_PRESENT_FRAME_HOOK);
        s_hook(s_frames, s_hook_user);
    }
    lx_crash_present_stage(LX_PRESENT_END);
    if (s_frame_limit && s_frames >= s_frame_limit) lx_abort(LX_STOP, s_frames, "frame presented");
}

void lx_screen_present_scene(int width, int height) {
    lx_scene_frame_t f = { width, height, NULL, 0, 0 };
    int window_w = 0, window_h = 0;
    if (!s_open || !lx_window_backend()->present_scene || debug_menu_start_holds_present()) return;
    ensure_presentation();
    lx_screen_window_client(&window_w, &window_h);
    lx_frontend_frame(width, height, window_w, window_h);
    lx_frontend_frame_done();
    lx_overlay_set_picture(0, 0, 0, 0);
    lx_overlay_frame_presented(jpt_time_ms());
    f.overlay = lx_overlay_compose_layer(width, height, jpt_time_ms());
    f.overlay_w = width, f.overlay_h = height;
    if (width == s_mode.width && height == s_mode.height) jpt_presentation_commit(&s_pres, s_generation);
    lx_crash_present_stage(LX_PRESENT_BEGIN);
    lx_window_backend()->present_scene(lx_window_backend(), &f);
    frame_presented(width, height, "Direct3D scene");
}

void lx_screen_present(const lx_frame_t* frame) {
    lx_frame_t f = *frame, composed;
    if (!s_open || debug_menu_start_holds_present()) return;
    ensure_presentation();
    if (lx_vsub_compose(&f, &composed)) f = composed;
    {
        int window_w = 0, window_h = 0;
        lx_screen_window_client(&window_w, &window_h);
        lx_view_t picture = { 0, 0, 0, 0 };
        if (lx_frontend_frame(frame->width, frame->height, window_w, window_h) && lx_frontend_compose(&f, &composed, window_w, window_h, 0)) {
            f = composed;
            picture = lx_present_view(window_w, window_h, frame->width, frame->height, 0);
        }
        lx_overlay_set_picture(picture.x, picture.y, picture.w, picture.h);
        lx_frontend_frame_done();
    }
    {
        lx_frame_t overlaid;
        lx_overlay_frame_presented(jpt_time_ms());
        if (lx_overlay_compose(&f, &overlaid, jpt_time_ms())) f = overlaid;
    }
    f.generation = s_generation;
    if (frame->width == s_mode.width && frame->height == s_mode.height) jpt_presentation_commit(&s_pres, s_generation);
    lx_crash_present_stage(LX_PRESENT_BEGIN);
    lx_window_backend()->present(lx_window_backend(), &f);
    frame_presented(f.width, f.height, f.format == LX_PF_RGB565 ? "RGB565" : "XRGB8888");
}

unsigned lx_screen_frames(void) { return s_frames; }
void lx_screen_set_frame_limit(unsigned frames) { s_frame_limit = frames; }
void lx_screen_set_frame_hook(lx_frame_hook_fn fn, void* user) { s_hook = fn, s_hook_user = user; }

int lx_screen_client_to_guest(int cx, int cy, int* gx, int* gy) {
    ensure_presentation();
    return jpt_presentation_to_guest(&s_pres, cx, cy, gx, gy);
}
