#include "window_title.h"

#include <stdio.h>
#include <string.h>

static void append(char* out, size_t cap, const char* text) {
    size_t used = strlen(out), n = strlen(text);
    if (used + n >= cap) n = cap - 1 - used;
    memcpy(out + used, text, n);
    out[used + n] = 0;
}

void title_fps_text(double fps, char* out, size_t cap) {
    if (!cap) return;
    _snprintf(out, cap - 1, "FPS: %.1f", fps);
    out[cap - 1] = 0;
}

void title_compose(const title_content_t* c, char* out, size_t cap) {
    char part[48];
    if (!cap) return;
    out[0] = 0;
    append(out, cap, c->base);
    if (c->show_resolution && c->width > 0 && c->height > 0) {
        _snprintf(part, sizeof part - 1, " | %dx%d", c->width, c->height);
        part[sizeof part - 1] = 0;
        append(out, cap, part);
    }
    if (c->show_fps && c->fps_ready) {
        append(out, cap, " | ");
        title_fps_text(c->fps, part, sizeof part);
        append(out, cap, part);
    }
}
