/* The HD frontend's model: the game's menu windows and controls as read from the guest, in the 640x480 canvas
 * coordinates the game hit-tests in, and their placement on the screen. No Windows calls. */
#ifndef FRONTEND_LAYOUT_H
#define FRONTEND_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_CANVAS_W 640
#define FE_CANVAS_H 480
#define FE_MAX_WINDOWS 8
#define FE_MAX_WIDGETS 160
#define FE_MAX_ITEMS 256
#define FE_TEXT_BYTES 16384

typedef struct {
    int left, top, right, bottom;
} fe_rect_t;

/* Canvas units to screen pixels: the 4:3 canvas rectangle the mouse mapping uses (jpt_ui_canvas), so every widget
 * is drawn exactly where the game hit-tests it. */
typedef struct {
    int screen_w, screen_h;
    double x0, y0, sx, sy;
} fe_view_t;

fe_view_t fe_view_make(int screen_w, int screen_h);
/* Screen pixels covering a canvas rectangle (left/top down, right/bottom up), not clipped. */
fe_rect_t fe_view_rect(const fe_view_t* v, fe_rect_t r);
double fe_view_x(const fe_view_t* v, double u);
double fe_view_y(const fe_view_t* v, double u);
/* The pointer's canvas position for a screen pixel, as the game receives it. */
void fe_view_to_canvas(const fe_view_t* v, int x, int y, int* u, int* w);

fe_rect_t fe_rect(int left, int top, int right, int bottom);
int fe_rect_empty(fe_rect_t r);
fe_rect_t fe_rect_union(fe_rect_t a, fe_rect_t b);
fe_rect_t fe_rect_intersect(fe_rect_t a, fe_rect_t b);
fe_rect_t fe_rect_inflate(fe_rect_t r, int dx, int dy);
fe_rect_t fe_rect_offset(fe_rect_t r, int dx, int dy);
int fe_rect_contains(fe_rect_t r, int x, int y);
int fe_rect_equal(fe_rect_t a, fe_rect_t b);

typedef enum {
    FE_WND_GAME,     /* the gameplay view under a pause menu: drawn by the renderer, not by the frontend */
    FE_WND_MAIN,
    FE_WND_OPTIONS,
    FE_WND_GAME_MENU,
    FE_WND_AUDIO,
    FE_WND_CONTROLS,
    FE_WND_LOAD,
    FE_WND_SAVE,
    FE_WND_QUIT,
    FE_WND_MESSAGE,
    FE_WND_KIND_COUNT
} fe_window_kind_t;

typedef enum {
    FE_W_BUTTON,
    FE_W_CHECK,
    FE_W_SLIDER,
    FE_W_LABEL,
    FE_W_FIELD, /* a bordered text box: a key binding, a value */
    FE_W_LIST,
    FE_W_EDIT,
    FE_W_IMAGE,
    FE_W_KIND_COUNT
} fe_widget_kind_t;

enum {
    FE_ST_HOVER = 1,
    FE_ST_PRESSED = 2,
    FE_ST_DISABLED = 4,
    FE_ST_CHECKED = 8,
    FE_ST_FOCUS = 16,
    FE_ST_ROW_HOVER = 32,
    FE_ST_CARET = 64
};

enum { FE_BUTTON_PLAQUE, FE_BUTTON_LARGE, FE_BUTTON_ARROW_UP, FE_BUTTON_ARROW_DOWN };

/* FE_IMAGE_PHOTO: a save's picture in its own frame bolted over the plate's edge, outside the plate's bounds. */
enum { FE_IMAGE_PLAIN, FE_IMAGE_FRAMED, FE_IMAGE_PHOTO };

enum {
    FE_TEXT_CENTER = 1,
    FE_TEXT_RIGHT = 2,
    FE_TEXT_VCENTER = 4,
    FE_TEXT_BOTTOM = 8,
    FE_TEXT_WRAP = 16,
    FE_TEXT_SHADOW = 32,
    FE_TEXT_ELLIPSIS = 64
};

typedef struct {
    uint8_t kind, style;
    uint16_t state;
    int32_t id;
    fe_rect_t rect; /* canvas */
    int text;       /* offset in the scene's text, -1 for none */
    float text_h;   /* canvas units */
    uint16_t text_flags;
    uint16_t weight;
    uint32_t color; /* 0xRRGGBB */
    int value, range;                       /* slider: thumb centre (canvas x) and units */
    int first_item, item_count, top, selected, row_h, rows; /* list: the shown items from first_item, item_count in all */
    fe_rect_t track;                        /* list: scroll bar column, arrows included (empty when none) */
    int caret;                              /* edit: caret index in the text */
    const uint16_t* image;                  /* RGB565 rows */
    const uint32_t* image32;                /* or opaque BGRA rows (image_pitch pixels apart) */
    int image_w, image_h, image_pitch;
    uint16_t image_key;                     /* the transparent colour when image_keyed */
    uint8_t image_keyed;
    uint32_t image_hash;
} fe_widget_t;

typedef struct {
    uint8_t kind;
    fe_rect_t rect;  /* the game's window rectangle */
    fe_rect_t panel; /* the plate the frontend draws (canvas) */
    int title;       /* offset in the scene's text, -1 for none */
    int first, count;
} fe_window_t;

typedef struct {
    int screen_w, screen_h;
    int window_count, widget_count, item_count, text_used;
    fe_window_t windows[FE_MAX_WINDOWS];
    fe_widget_t widgets[FE_MAX_WIDGETS];
    int items[FE_MAX_ITEMS];
    char text[FE_TEXT_BYTES];
    int cursor_x, cursor_y; /* canvas */
    const uint16_t* backdrop; /* the main screen's 640x480 art, RGB565 */
    int backdrop_pitch;
    uint32_t backdrop_hash;
} fe_scene_t;

void fe_scene_reset(fe_scene_t* s, int screen_w, int screen_h);
/* Offset of a copy of text in the scene, -1 when full. */
int fe_scene_text(fe_scene_t* s, const char* text);
const char* fe_scene_str(const fe_scene_t* s, int offset);
fe_window_t* fe_scene_window(fe_scene_t* s, fe_window_kind_t kind, fe_rect_t rect);
fe_widget_t* fe_scene_widget(fe_scene_t* s, fe_widget_kind_t kind, int32_t id, fe_rect_t rect);
int fe_scene_item(fe_scene_t* s, const char* text);
/* Items a list shows: rows from its top one, fewer at the end of the list. */
int fe_list_shown(const fe_widget_t* w);

/* The plate of a dialog: its window and every widget in it, with a margin for the frame. */
#define FE_PANEL_MARGIN 6
void fe_scene_finish_window(fe_scene_t* s, fe_window_t* w);
/* Labels to the right of a check box are drawn next to it (labels are never hit-tested). */
void fe_scene_tidy_check_rows(fe_scene_t* s, const fe_window_t* w);

enum { FE_DIFF_SAME, FE_DIFF_RECT, FE_DIFF_ALL };
/* What must be painted again: nothing, *dirty (canvas units, glow included) or the whole screen. */
int fe_scene_diff(const fe_scene_t* before, const fe_scene_t* now, fe_rect_t* dirty);

uint32_t fe_hash(uint32_t h, const void* data, size_t bytes);
uint32_t fe_hash_pixels(const uint16_t* rows, int w, int h, int pitch, int step);

/* Options without its Video entry, staggered as options.ddf staggers it: Audio high, Controls and Credits on either
 * side, Close low. Each entry keeps its own size (the game draws its picture at the rectangle's corner); rects receive
 * window-relative canvas rectangles in the order of ids. */
#define FE_OPTIONS_AUDIO_ID 1002
#define FE_OPTIONS_CONTROLS_ID 1000
#define FE_OPTIONS_CREDITS_ID 1003
#define FE_OPTIONS_CLOSE_ID 1004
void fe_layout_options(const int32_t* ids, const int* widths, const int* heights, int count, int window_w, int window_h,
                       fe_rect_t* rects);

/* The main screen's plaques (mainscreen.ddf: 208x81 at 205,70+90k), each sized to its label and a little off a
 * straight column, as the original's torn plates are. */
void fe_layout_main_menu(const int* label_lengths, int count, fe_rect_t* rects);

/* Rows of the audio options spread evenly over the band [top, bottom): the new top of each row. */
void fe_layout_rows(int count, int top, int bottom, int row_h, int* row_tops);

/* The slider thumb's centre as the game places it: from the value at rest, under the pointer while dragged. */
int fe_slider_thumb_x(fe_rect_t track, int thumb_w, int value, int range);
int fe_slider_drag_x(fe_rect_t track, int thumb_w, int pointer_x);

/* The frame of a save's picture in Load and Save (canvas): larger than the game's 100x75 thumbnail, over the
 * plate's left edge, clear of the list and the name field. */
fe_rect_t fe_layout_photo(fe_window_kind_t kind, fe_rect_t window);

/* The part of a picture shown (src, picture pixels) and where it goes inside a frame (dst, from the frame's corner):
 * cropped to the frame's shape by up to FE_PHOTO_CROP in one direction, the rest letterboxed. */
#define FE_PHOTO_CROP 1.34
void fe_photo_fit(int image_w, int image_h, double frame_w, double frame_h, double src[4], double dst[4]);

/* A list's scroll thumb inside the track between its arrows (canvas); empty when every item fits. */
fe_rect_t fe_list_thumb(fe_rect_t column, int arrow_h, int top, int rows, int count);

#ifdef __cplusplus
}
#endif

#endif
