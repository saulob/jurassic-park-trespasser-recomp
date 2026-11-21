#include "linux_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_display.h"

#define VIRTUAL_QUEUE 64

static lx_window_backend_t* s_backend;
static lx_virtual_state_t s_state;
static lx_event_t s_queue[VIRTUAL_QUEUE];
static int s_head, s_count;
static uint8_t* s_last_pixels;
static size_t s_last_cap;

void lx_window_set_backend(lx_window_backend_t* b) { s_backend = b; }
lx_window_backend_t* lx_window_backend(void) { return s_backend ? s_backend : lx_virtual_backend(); }

static int v_open(lx_window_backend_t* b, const lx_window_desc_t* d) {
    (void)b;
    s_state.opened = 1;
    s_state.width = d->client_w;
    s_state.height = d->client_h;
    snprintf(s_state.title, sizeof s_state.title, "%s", d->title ? d->title : "");
    s_state.cursor_visible = 1;
    return 1;
}

static void v_close(lx_window_backend_t* b) {
    (void)b;
    s_state.opened = 0;
    s_state.visible = 0;
}

static void v_set_title(lx_window_backend_t* b, const char* title) {
    (void)b;
    snprintf(s_state.title, sizeof s_state.title, "%s", title ? title : "");
}

static void v_set_client_size(lx_window_backend_t* b, int w, int h) {
    (void)b;
    s_state.width = w;
    s_state.height = h;
}

static void v_client_size(lx_window_backend_t* b, int* w, int* h) {
    (void)b;
    *w = s_state.width;
    *h = s_state.height;
}

static void v_set_visible(lx_window_backend_t* b, int visible) {
    (void)b;
    s_state.visible = visible;
}

static int v_next_event(lx_window_backend_t* b, int timeout_ms, lx_event_t* ev) {
    (void)b;
    (void)timeout_ms;
    if (!s_count) {
        ev->kind = LX_EV_NONE;
        return 0;
    }
    *ev = s_queue[s_head];
    s_head = (s_head + 1) % VIRTUAL_QUEUE;
    s_count--;
    s_state.events_returned++;
    return 1;
}

static void v_present(lx_window_backend_t* b, const lx_frame_t* f) {
    uint32_t sum = 2166136261u;
    (void)b;
    for (int y = 0; y < f->height; y++) {
        const uint8_t* row = (const uint8_t*)f->pixels + (size_t)y * (size_t)f->pitch;
        size_t n = (size_t)f->width * (f->format == LX_PF_RGB565 ? 2u : 4u);
        for (size_t i = 0; i < n; i++) sum = (sum ^ row[i]) * 16777619u;
    }
    {
        size_t row = (size_t)f->width * (f->format == LX_PF_RGB565 ? 2u : 4u), need = row * (size_t)f->height;
        if (s_last_cap < need) {
            uint8_t* p = (uint8_t*)realloc(s_last_pixels, need);
            if (p) s_last_pixels = p, s_last_cap = need;
        }
        if (s_last_cap >= need)
            for (int y = 0; y < f->height; y++) memcpy(s_last_pixels + (size_t)y * row, (const uint8_t*)f->pixels + (size_t)y * (size_t)f->pitch, row);
    }
    s_state.presents++;
    s_state.last = *f;
    s_state.last.pixels = NULL;
    s_state.last_checksum = sum;
}

static void v_set_cursor_visible(lx_window_backend_t* b, int visible) {
    (void)b;
    if (s_state.cursor_visible != visible) s_state.cursor_changes++;
    s_state.cursor_visible = visible;
}

static void v_set_mouse_grab(lx_window_backend_t* b, int grab) {
    (void)b;
    s_state.grabbed = grab;
    s_state.grab_calls++;
}

static void v_set_relative_mouse(lx_window_backend_t* b, int on) {
    (void)b;
    if (s_state.relative != (on != 0)) s_state.relative_changes++;
    s_state.relative = on != 0;
}

static void v_destroy(lx_window_backend_t* b) { (void)b; }

static void v_toggle_fullscreen(lx_window_backend_t* b) {
    (void)b;
    s_state.fullscreen_toggles++;
}

static lx_window_backend_t s_virtual = { "virtual", v_open, v_close, v_set_title, v_set_client_size, v_client_size, v_set_visible,
                                         v_next_event, v_present, v_set_cursor_visible, v_set_mouse_grab, v_set_relative_mouse, v_destroy, v_toggle_fullscreen };

lx_window_backend_t* lx_virtual_backend(void) { return &s_virtual; }
const lx_virtual_state_t* lx_virtual_state(void) { return &s_state; }

int lx_virtual_dump_ppm(const char* path) {
    FILE* out;
    const lx_frame_t* f = &s_state.last;
    if (!s_state.presents || !s_last_pixels || !(out = fopen(path, "wb"))) return 0;
    fprintf(out, "P6\n%d %d\n255\n", f->width, f->height);
    for (int y = 0; y < f->height; y++)
        for (int x = 0; x < f->width; x++) {
            uint8_t rgb[3];
            if (f->format == LX_PF_RGB565) {
                const uint8_t* p = s_last_pixels + ((size_t)y * (size_t)f->width + (size_t)x) * 2;
                unsigned v = (unsigned)(p[0] | (p[1] << 8)), r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                rgb[0] = (uint8_t)(r << 3 | r >> 2), rgb[1] = (uint8_t)(g << 2 | g >> 4), rgb[2] = (uint8_t)(b << 3 | b >> 2);
            } else {
                const uint8_t* p = s_last_pixels + ((size_t)y * (size_t)f->width + (size_t)x) * 4;
                rgb[0] = p[2], rgb[1] = p[1], rgb[2] = p[0];
            }
            fwrite(rgb, 1, 3, out);
        }
    fclose(out);
    return 1;
}

void lx_virtual_reset(void) {
    memset(&s_state, 0, sizeof s_state);
    s_head = s_count = 0;
}

void lx_virtual_queue_event(const lx_event_t* ev) {
    if (s_count == VIRTUAL_QUEUE) return;
    s_queue[(s_head + s_count) % VIRTUAL_QUEUE] = *ev;
    s_count++;
}

lx_view_t lx_present_view(int client_w, int client_h, int guest_w, int guest_h, int stretch) {
    jpt_rect_t r = jpt_present_rect(client_w, client_h, guest_w, guest_h, stretch);
    lx_view_t v;
    v.x = r.left;
    v.y = r.top;
    v.w = r.right - r.left;
    v.h = r.bottom - r.top;
    return v;
}

int lx_present_to_guest(int client_w, int client_h, int guest_w, int guest_h, int stretch, int cx, int cy, int* gx, int* gy) {
    jpt_presentation_t p;
    jpt_presentation_init(&p, stretch);
    jpt_presentation_set_host(&p, client_w, client_h);
    jpt_presentation_set_guest(&p, guest_w, guest_h);
    return jpt_presentation_to_guest(&p, cx, cy, gx, gy);
}
