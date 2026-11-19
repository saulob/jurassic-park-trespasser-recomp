#include "jpt_display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_TARGET 16384

const jpt_aspect_t jpt_aspects[JPT_ASPECT_COUNT] = {
    { "4:3", "4:3 (original)", 4, 3, 800, 600 },
    { "16:9", "16:9", 16, 9, 1024, 576 },
    { "16:10", "16:10", 16, 10, 960, 600 },
    { "21:9", "21:9 Ultrawide", 64, 27, 1280, 540 },
    { "32:9", "32:9 Super Ultrawide", 32, 9, 2048, 576 },
};

const jpt_render_preset_t jpt_render_presets[JPT_RENDER_PRESET_COUNT] = {
    { "original", "Original", 0, 0, 0 },
    { "720p", "720p", 1280, 720, 1720 },
    { "1080p", "1080p", 1920, 1080, 2560 },
    { "1440p", "1440p", 2560, 1440, 3440 },
    { "4k", "4K", 3840, 2160, 5120 },
    { "native", "Native", 0, 0, 0 },
};

const jpt_window_size_t jpt_window_sizes[JPT_WINDOW_SIZE_COUNT] = {
    { "auto", 0, 0 },          { "1280x720", 1280, 720 },   { "1600x900", 1600, 900 },   { "1920x1080", 1920, 1080 },
    { "2560x1440", 2560, 1440 }, { "3440x1440", 3440, 1440 }, { "3840x2160", 3840, 2160 }, { "5120x1440", 5120, 1440 },
};

int jpt_aspect_index(const char* key) {
    for (int i = 0; key && i < JPT_ASPECT_COUNT; i++)
        if (!strcmp(key, jpt_aspects[i].key)) return i;
    return 0;
}

int jpt_render_preset_index(const char* key) {
    for (int i = 0; key && i < JPT_RENDER_PRESET_COUNT; i++)
        if (!_stricmp(key, jpt_render_presets[i].key)) return i;
    return -1;
}

static int even(double v) {
    int n = (int)(v + 0.5);
    return n & 1 ? n + 1 : n;
}

static jpt_size_t clamp_size(jpt_size_t s) {
    if (s.width > MAX_TARGET) {
        s.height = even((double)s.height * MAX_TARGET / s.width);
        s.width = MAX_TARGET;
    }
    if (s.height > MAX_TARGET) {
        s.width = even((double)s.width * MAX_TARGET / s.height);
        s.height = MAX_TARGET;
    }
    return s;
}

static jpt_size_t contain(int box_w, int box_h, int num, int den) {
    jpt_size_t s;
    if ((long long)box_w * den >= (long long)box_h * num) {
        s.height = box_h;
        s.width = even((double)box_h * num / den);
        if (s.width > box_w) s.width = box_w & ~1;
    } else {
        s.width = box_w;
        s.height = even((double)box_w * den / num);
        if (s.height > box_h) s.height = box_h & ~1;
    }
    return s;
}

jpt_size_t jpt_render_box(int preset, int aspect, int monitor_w, int monitor_h) {
    jpt_size_t s = { 0, 0 };
    const jpt_aspect_t* a;
    if (aspect < 0 || aspect >= JPT_ASPECT_COUNT) aspect = 0;
    a = &jpt_aspects[aspect];
    if (preset == JPT_RENDER_NATIVE) {
        if (monitor_w <= 0 || monitor_h <= 0) return s;
        return clamp_size(contain(monitor_w, monitor_h, a->num, a->den));
    }
    if (preset <= JPT_RENDER_ORIGINAL || preset >= JPT_RENDER_PRESET_COUNT) return s;
    if (aspect == JPT_ASPECT_21_9) {
        s.width = jpt_render_presets[preset].ultrawide_width;
        s.height = jpt_render_presets[preset].height;
    } else if ((long long)a->num * 9 >= (long long)a->den * 16) {
        s.height = jpt_render_presets[preset].height;
        s.width = even((double)s.height * a->num / a->den);
    } else {
        s.width = jpt_render_presets[preset].width;
        s.height = even((double)s.width * a->den / a->num);
    }
    return clamp_size(s);
}

jpt_size_t jpt_render_target(jpt_size_t box, int guest_w, int guest_h) {
    jpt_size_t s = { 0, 0 };
    double shape;
    if (box.width <= 0 || box.height <= 0 || guest_w <= 0 || guest_h <= 0) return s;
    shape = ((double)box.width / box.height) / ((double)guest_w / guest_h);
    if (shape > 0.98 && shape < 1.02) return box;
    return contain(box.width, box.height, guest_w, guest_h);
}

jpt_size_t jpt_gameplay_mode(jpt_size_t box, int aspect, int hardware) {
    const jpt_aspect_t* a = &jpt_aspects[aspect >= 0 && aspect < JPT_ASPECT_COUNT ? aspect : 0];
    jpt_size_t s = { a->compat_w, a->compat_h };
    long long pixels = (long long)box.width * box.height;
    double f;
    if (!hardware || box.width < 320 || box.height < 240) return s;
    if (pixels <= JPT_GAMEPLAY_MAX_PIXELS) return box;
    f = sqrt((double)JPT_GAMEPLAY_MAX_PIXELS / (double)pixels);
    s.width = (int)(box.width * f) & ~7;
    s.height = (int)(box.height * f) & ~7;
    return s;
}

jpt_size_t jpt_title_size(jpt_size_t box, jpt_size_t gameplay, int hardware) {
    return hardware && box.width > 0 && box.height > 0 ? box : gameplay;
}

unsigned long long jpt_gameplay_host_bytes(jpt_size_t mode) {
    return (unsigned long long)mode.width * (unsigned long long)mode.height * (2 + 2 + 4 + 2 + 4);
}

int jpt_ui_text_height(int game_height, int screen_h) {
    if (screen_h <= 600 || game_height <= 0) return game_height;
    return (int)(((long long)game_height * screen_h + 300) / 600);
}

jpt_ui_canvas_t jpt_ui_canvas(int frame_w, int frame_h) {
    jpt_ui_canvas_t c = { { 0, 0, 0, 0 }, 1.0 };
    double sx, sy;
    int w, h;
    if (frame_w <= 0 || frame_h <= 0) return c;
    sx = (double)frame_w / JPT_UI_CANVAS_W;
    sy = (double)frame_h / JPT_UI_CANVAS_H;
    c.scale = sx < sy ? sx : sy;
    w = (int)(JPT_UI_CANVAS_W * c.scale + 0.5);
    h = (int)(JPT_UI_CANVAS_H * c.scale + 0.5);
    if (w > frame_w) w = frame_w;
    if (h > frame_h) h = frame_h;
    c.rect.left = (frame_w - w) / 2;
    c.rect.top = (frame_h - h) / 2;
    c.rect.right = c.rect.left + w;
    c.rect.bottom = c.rect.top + h;
    return c;
}

int jpt_ui_to_canvas(const jpt_ui_canvas_t* canvas, int x, int y, int* canvas_x, int* canvas_y) {
    long long w = canvas->rect.right - canvas->rect.left, h = canvas->rect.bottom - canvas->rect.top;
    long long cx, cy;
    int inside;
    if (w <= 0 || h <= 0) return 0;
    inside = x >= canvas->rect.left && x < canvas->rect.right && y >= canvas->rect.top && y < canvas->rect.bottom;
    cx = (((long long)(x - canvas->rect.left) * 2 + 1) * JPT_UI_CANVAS_W) / (2 * w);
    cy = (((long long)(y - canvas->rect.top) * 2 + 1) * JPT_UI_CANVAS_H) / (2 * h);
    *canvas_x = (int)(cx < 0 ? 0 : cx >= JPT_UI_CANVAS_W ? JPT_UI_CANVAS_W - 1 : cx);
    *canvas_y = (int)(cy < 0 ? 0 : cy >= JPT_UI_CANVAS_H ? JPT_UI_CANVAS_H - 1 : cy);
    return inside;
}

void jpt_ui_to_frame(const jpt_ui_canvas_t* canvas, int canvas_x, int canvas_y, int* x, int* y) {
    long long w = canvas->rect.right - canvas->rect.left, h = canvas->rect.bottom - canvas->rect.top;
    *x = canvas->rect.left + (int)((((long long)canvas_x * 2 + 1) * w) / (2 * JPT_UI_CANVAS_W));
    *y = canvas->rect.top + (int)((((long long)canvas_y * 2 + 1) * h) / (2 * JPT_UI_CANVAS_H));
}

jpt_rect_t jpt_ui_rect_to_frame(const jpt_ui_canvas_t* canvas, jpt_rect_t r) {
    long long w = canvas->rect.right - canvas->rect.left, h = canvas->rect.bottom - canvas->rect.top;
    jpt_rect_t out;
    out.left = canvas->rect.left + (int)(((long long)r.left * w) / JPT_UI_CANVAS_W);
    out.top = canvas->rect.top + (int)(((long long)r.top * h) / JPT_UI_CANVAS_H);
    out.right = canvas->rect.left + (int)(((long long)r.right * w + JPT_UI_CANVAS_W - 1) / JPT_UI_CANVAS_W);
    out.bottom = canvas->rect.top + (int)(((long long)r.bottom * h + JPT_UI_CANVAS_H - 1) / JPT_UI_CANVAS_H);
    return out;
}

void jpt_ui_canvas_ndc(const jpt_ui_canvas_t* canvas, int frame_w, int frame_h, float out[4]) {
    out[0] = (float)canvas->rect.left / (float)frame_w * 2.0f - 1.0f;
    out[1] = 1.0f - (float)canvas->rect.top / (float)frame_h * 2.0f;
    out[2] = (float)canvas->rect.right / (float)frame_w * 2.0f - 1.0f;
    out[3] = 1.0f - (float)canvas->rect.bottom / (float)frame_h * 2.0f;
}

double jpt_hor_plus_scale(int guest_w, int guest_h) {
    if (guest_w <= 0 || guest_h <= 0) return 1.0;
    return ((double)guest_w / guest_h) / (4.0 / 3.0);
}

jpt_rect_t jpt_present_rect(int client_w, int client_h, int content_w, int content_h, int stretch) {
    jpt_rect_t r = { 0, 0, client_w, client_h };
    long long vw = client_w, vh = client_h;
    if (stretch || client_w <= 0 || client_h <= 0 || content_w <= 0 || content_h <= 0) return r;
    vh = ((long long)client_w * content_h + content_w / 2) / content_w;
    if (vh > client_h) {
        vh = client_h;
        vw = ((long long)client_h * content_w + content_h / 2) / content_h;
    }
    if (vh >= client_h - 1) vh = client_h;
    if (vw >= client_w - 1) vw = client_w;
    r.left = (int)((client_w - vw) / 2);
    r.top = (int)((client_h - vh) / 2);
    r.right = r.left + (int)vw;
    r.bottom = r.top + (int)vh;
    return r;
}

void jpt_presentation_init(jpt_presentation_t* p, int stretch) {
    memset(p, 0, sizeof *p);
    p->stretch = stretch != 0;
}

static int presentation_relayout(jpt_presentation_t* p) {
    jpt_rect_t view = jpt_present_rect(p->host.width, p->host.height, p->guest.width, p->guest.height, p->stretch);
    int changed = memcmp(&view, &p->view, sizeof view) != 0;
    p->view = view;
    return changed;
}

int jpt_presentation_set_host(jpt_presentation_t* p, int client_w, int client_h) {
    if (client_w <= 0 || client_h <= 0) return 0;
    p->host.width = client_w;
    p->host.height = client_h;
    return presentation_relayout(p);
}

int jpt_presentation_set_guest(jpt_presentation_t* p, int guest_w, int guest_h) {
    if (guest_w <= 0 || guest_h <= 0) return 0;
    p->guest.width = guest_w;
    p->guest.height = guest_h;
    return presentation_relayout(p);
}

int jpt_presentation_request_guest(jpt_presentation_t* p, int guest_w, int guest_h, unsigned generation) {
    if (guest_w <= 0 || guest_h <= 0) return 0;
    if (p->guest.width <= 0) {
        p->generation = generation;
        p->has_pending = 0;
        jpt_presentation_set_guest(p, guest_w, guest_h);
        return 1;
    }
    if (p->guest.width == guest_w && p->guest.height == guest_h) {
        p->has_pending = 0;
        p->generation = generation;
        return 0;
    }
    p->has_pending = 1;
    p->pending_guest.width = guest_w;
    p->pending_guest.height = guest_h;
    p->pending_generation = generation;
    return 0;
}

int jpt_presentation_commit(jpt_presentation_t* p, unsigned generation) {
    if (!p->has_pending || p->pending_generation != generation) return 0;
    p->has_pending = 0;
    p->generation = generation;
    jpt_presentation_set_guest(p, p->pending_guest.width, p->pending_guest.height);
    return 1;
}

jpt_size_t jpt_auto_client(jpt_size_t client, jpt_size_t guest, int frame_w, int frame_h, int work_w, int work_h) {
    jpt_size_t out = client;
    if (client.width <= 0 || client.height <= 0 || guest.width <= 0 || guest.height <= 0) return client;
    out.height = (int)(((long long)client.width * guest.height + guest.width / 2) / guest.width);
    if (out.height < 1) out.height = 1;
    if (out.height == client.height) return client;
    return jpt_fit_window(out, frame_w, frame_h, work_w, work_h);
}

jpt_commit_t jpt_presentation_commit_frame(jpt_presentation_t* p, unsigned generation, int follow_guest, int frame_w,
                                           int frame_h, int work_w, int work_h) {
    jpt_commit_t result = { 0, 0, { 0, 0 } };
    if (!p->has_pending || p->pending_generation != generation) return result;
    if (follow_guest) {
        jpt_size_t target = jpt_auto_client(p->host, p->pending_guest, frame_w, frame_h, work_w, work_h);
        if (target.width != p->host.width || target.height != p->host.height) {
            result.resize = 1;
            result.client = target;
            p->host = target;
        }
    }
    result.committed = jpt_presentation_commit(p, generation);
    return result;
}

jpt_rect_t jpt_presentation_next_view(const jpt_presentation_t* p) {
    jpt_presentation_t next = *p;
    if (next.has_pending) jpt_presentation_commit(&next, next.pending_generation);
    return next.view;
}

int jpt_presentation_to_guest(const jpt_presentation_t* p, int client_x, int client_y, int* guest_x, int* guest_y) {
    long long view_w = (long long)p->view.right - p->view.left, view_h = (long long)p->view.bottom - p->view.top;
    long long nx, ny, x, y;
    if (view_w <= 0 || view_h <= 0 || p->guest.width <= 0 || p->guest.height <= 0) return 0;
    nx = ((long long)client_x - p->view.left) * p->guest.width;
    ny = ((long long)client_y - p->view.top) * p->guest.height;
    x = nx / view_w - ((nx % view_w != 0 && nx < 0) ? 1 : 0);
    y = ny / view_h - ((ny % view_h != 0 && ny < 0) ? 1 : 0);
    *guest_x = (int)x;
    *guest_y = (int)y;
    return x >= 0 && y >= 0 && x < p->guest.width && y < p->guest.height;
}

jpt_size_t jpt_fit_window(jpt_size_t client, int frame_w, int frame_h, int work_w, int work_h) {
    long long fit_w = (long long)work_w - frame_w, fit_h = (long long)work_h - frame_h;
    if (client.width <= 0 || client.height <= 0 || fit_w <= 0 || fit_h <= 0) return client;
    if (client.width <= fit_w && client.height <= fit_h) return client;
    if (fit_w * client.height < fit_h * client.width) {
        client.height = (int)((fit_w * client.height + client.width / 2) / client.width);
        client.width = (int)fit_w;
    } else {
        client.width = (int)((fit_h * client.width + client.height / 2) / client.height);
        client.height = (int)fit_h;
    }
    return client;
}

int jpt_monitor_index(int setting, int count) {
    if (count <= 0 || setting < 1 || setting > count) return 0;
    return setting - 1;
}

unsigned jpt_msaa_pick(unsigned requested, unsigned supported_mask) {
    for (unsigned n = requested > 16 ? 16 : requested; n > 1; n--)
        if (supported_mask & (1u << n)) return n;
    return 1;
}

int jpt_parse_size(const char* text, int* width, int* height) {
    int w = 0, h = 0;
    char extra;
    if (!text || sscanf(text, "%dx%d%c", &w, &h, &extra) != 2) return 0;
    if (w < 320 || h < 240 || w > MAX_TARGET || h > MAX_TARGET) return 0;
    *width = w;
    *height = h;
    return 1;
}
