#include "jpt_display.h"

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
