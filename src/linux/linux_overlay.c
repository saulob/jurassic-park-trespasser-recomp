#include "linux_overlay.h"

#include <stdlib.h>
#include <string.h>

#include "host_policy.h"
#include "linux_card.h"
#include "linux_panel.h"
#include "linux_text.h"
#include "window_title.h"

#define NOTICE_MS 2500u
#define TEXT_HEIGHT (-18)
#define MARGIN 8

static struct {
    int frame_counter;
    fps_counter_t fps;
    int fps_ready, fps_started;
    char notice[96];
    uint64_t notice_until;
    uint32_t* buffer;
    size_t capacity;
    int pic_x, pic_y, pic_w, pic_h;
} O;

void lx_overlay_reset(void) {
    free(O.buffer);
    memset(&O, 0, sizeof O);
}

void lx_overlay_configure(int frame_counter_on) { O.frame_counter = frame_counter_on != 0; }

void lx_overlay_toggle_frame_counter(void) { O.frame_counter = !O.frame_counter; }

int lx_overlay_frame_counter_on(void) { return O.frame_counter; }

void lx_overlay_frame_presented(uint64_t now_ms) {
    if (!O.fps_started) {
        fps_counter_init(&O.fps, 1000, (int64_t)now_ms);
        O.fps_started = 1;
    }
    if (fps_counter_present(&O.fps, (int64_t)now_ms)) O.fps_ready = 1;
}

int lx_overlay_frame_counter_text(char* out, unsigned capacity) {
    if (!O.frame_counter || !O.fps_ready || !capacity) return 0;
    title_fps_text(O.fps.fps, out, capacity);
    return 1;
}

void lx_overlay_notice(const char* text, uint64_t now_ms) {
    if (!text || !*text) return;
    strncpy(O.notice, text, sizeof O.notice - 1);
    O.notice[sizeof O.notice - 1] = 0;
    O.notice_until = now_ms + NOTICE_MS;
}

typedef struct {
    uint32_t* pixels;
    int width, height;
    uint32_t color;
} canvas_t;

static void put(void* context, int x, int y, int coverage) {
    canvas_t* c = (canvas_t*)context;
    uint32_t* p;
    uint32_t old, mix = 0;
    if (x < 0 || y < 0 || x >= c->width || y >= c->height || coverage <= 0) return;
    if (coverage > 255) coverage = 255;
    p = c->pixels + (size_t)y * (size_t)c->width + (size_t)x;
    old = *p;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t a = (old >> shift) & 0xFFu, b = (c->color >> shift) & 0xFFu;
        mix |= ((a * (uint32_t)(255 - coverage) + b * (uint32_t)coverage) / 255u) << shift;
    }
    *p = mix;
}

static int text_width(float scale, const char* s) { return lx_text_width(scale, s, (int)strlen(s), 0); }

static void draw_text(canvas_t* c, float scale, const char* s, int x, int baseline) {
    static const int outline[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 } };
    for (int pass = 0; pass < 2; pass++) {
        c->color = pass == 0 ? 0x000000u : 0xFFFFFFu;
        for (int shadow = 0; shadow < (pass == 0 ? 8 : 1); shadow++) {
            int pen = x;
            int dx = pass == 0 ? outline[shadow][0] : 0, dy = pass == 0 ? outline[shadow][1] : 0;
            for (const char* p = s; *p; p++) {
                lx_text_glyph(scale, (uint8_t)*p, 0, pen + dx, baseline + dy, put, c);
                pen += lx_text_advance(scale, (uint8_t)*p, 0);
            }
        }
    }
}

static uint32_t from_565(uint16_t v) {
    uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

void lx_overlay_set_picture(int x, int y, int w, int h) { O.pic_x = x, O.pic_y = y, O.pic_w = w, O.pic_h = h; }

int lx_overlay_compose(const lx_frame_t* in, lx_frame_t* out, uint64_t now_ms) {
    char counter[32];
    int counter_on = lx_overlay_frame_counter_text(counter, sizeof counter);
    int notice_on = O.notice[0] && now_ms < O.notice_until;
    size_t pixels;
    canvas_t canvas;
    float scale;
    lx_text_metrics_t m;
    int panel_on = lx_panel_wanted(O.frame_counter);
    const int card_on = lx_card_shown() || lx_card_hint_shown();
    if ((!counter_on && !notice_on && !panel_on && !card_on) || !in->pixels || in->width <= 0 || in->height <= 0) return 0;
    pixels = (size_t)in->width * (size_t)in->height;
    if (O.capacity < pixels) {
        uint32_t* grown = (uint32_t*)realloc(O.buffer, pixels * 4u);
        if (!grown) return 0;
        O.buffer = grown;
        O.capacity = pixels;
    }
    for (int y = 0; y < in->height; y++) {
        const uint8_t* row = (const uint8_t*)in->pixels + (size_t)y * (size_t)in->pitch;
        uint32_t* dst = O.buffer + (size_t)y * (size_t)in->width;
        if (in->format == LX_PF_RGB565)
            for (int x = 0; x < in->width; x++) dst[x] = from_565(((const uint16_t*)row)[x]);
        else
            memcpy(dst, row, (size_t)in->width * 4u);
    }
    canvas.pixels = O.buffer, canvas.width = in->width, canvas.height = in->height, canvas.color = 0;
    scale = lx_text_scale(TEXT_HEIGHT * (in->height >= 720 ? in->height / 480 : 1));
    lx_text_metrics(scale, &m);
    if (panel_on) {
        lx_panel_draw(O.buffer, in->width, in->height, in->width, counter_on ? counter : NULL);
        counter_on = 0;
    }
    if (card_on) {
        const int whole = O.pic_w <= 0 || O.pic_h <= 0;
        lx_card_draw(O.buffer, in->width, in->width, in->height, whole ? 0 : O.pic_x, whole ? 0 : O.pic_y, whole ? in->width : O.pic_w, whole ? in->height : O.pic_h);
    }
    if (counter_on) draw_text(&canvas, scale, counter, in->width - MARGIN - text_width(scale, counter), MARGIN + m.ascent);
    if (notice_on) draw_text(&canvas, scale, O.notice, (in->width - text_width(scale, O.notice)) / 2, in->height - MARGIN - m.descent);
    *out = *in;
    out->pixels = O.buffer;
    out->pitch = in->width * 4;
    out->format = LX_PF_XRGB8888;
    return 1;
}
