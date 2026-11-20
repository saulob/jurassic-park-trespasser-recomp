#include "linux_screen.h"

#include <string.h>

#include "jpt_display.h"
#include "linux_crash.h"
#include "linux_log.h"
#include "linux_spine.h"

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

void lx_screen_set_client_size(int w, int h) {
    if (!s_open || w <= 0 || h <= 0) return;
    lx_window_backend()->set_client_size(lx_window_backend(), w, h);
    ensure_presentation();
    jpt_presentation_set_host(&s_pres, w, h);
}

void lx_screen_present(const lx_frame_t* frame) {
    lx_frame_t f = *frame;
    if (!s_open) return;
    ensure_presentation();
    f.generation = s_generation;
    if (f.width == s_mode.width && f.height == s_mode.height) jpt_presentation_commit(&s_pres, s_generation);
    lx_crash_present_stage(LX_PRESENT_BEGIN);
    lx_window_backend()->present(lx_window_backend(), &f);
    if (++s_frames == 1)
        lx_log("Linux presentation", "first frame %dx%d %s presented", f.width, f.height, f.format == LX_PF_RGB565 ? "RGB565" : "XRGB8888");
    if (s_hook) {
        lx_crash_present_stage(LX_PRESENT_FRAME_HOOK);
        s_hook(s_frames, s_hook_user);
    }
    lx_crash_present_stage(LX_PRESENT_END);
    if (s_frame_limit && s_frames >= s_frame_limit) lx_abort(LX_STOP, s_frames, "frame presented");
}

unsigned lx_screen_frames(void) { return s_frames; }
void lx_screen_set_frame_limit(unsigned frames) { s_frame_limit = frames; }
void lx_screen_set_frame_hook(lx_frame_hook_fn fn, void* user) { s_hook = fn, s_hook_user = user; }

int lx_screen_client_to_guest(int cx, int cy, int* gx, int* gy) {
    ensure_presentation();
    return jpt_presentation_to_guest(&s_pres, cx, cy, gx, gy);
}
