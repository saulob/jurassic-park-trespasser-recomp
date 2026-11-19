#ifndef WINDOW_TITLE_H
#define WINDOW_TITLE_H

#include <stddef.h>

typedef struct {
    const char* base;     /* the product, architecture and renderer */
    int show_resolution;
    int width, height;    /* the render resolution; 0 until it is known */
    int show_fps;
    int fps_ready;        /* a rate has been measured */
    double fps;
} title_content_t;

/* "FPS: 59.9": the title and the in-game counter read the same measured rate. */
void title_fps_text(double fps, char* out, size_t cap);

/* base, then " | WxH" and " | FPS: n" when shown and known, never a separator without text after it. */
void title_compose(const title_content_t* content, char* out, size_t cap);

#endif
