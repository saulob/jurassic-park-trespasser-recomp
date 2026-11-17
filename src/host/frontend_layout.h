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
    int guest_w, guest_h;
    double x0, y0, sx, sy;
} fe_view_t;

fe_view_t fe_view_make(int screen_w, int screen_h);
/* The view of a layer of screen_w x screen_h pixels laid over a game screen of guest_w x guest_h: the canvas rectangle
 * of the game's screen (the one its pointer mapping uses), at the layer's resolution. */
fe_view_t fe_view_make_for(int guest_w, int guest_h, int screen_w, int screen_h);
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
    FE_WND_DIRECT_LOAD,
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

/* FE_IMAGE_PHOTO: a save's or a level's picture, taken from a file or the frozen game picture instead of a game raster. */
enum { FE_IMAGE_PLAIN, FE_IMAGE_FRAMED, FE_IMAGE_PHOTO };

enum {
    FE_TEXT_CENTER = 1,
    FE_TEXT_RIGHT = 2,
    FE_TEXT_VCENTER = 4,
    FE_TEXT_BOTTOM = 8,
    FE_TEXT_WRAP = 16,
    FE_TEXT_SHADOW = 32,
    FE_TEXT_ELLIPSIS = 64,
    FE_TEXT_EXACT = 128, /* as the game draws it: left/top at the box, no fitting, the shadow its own offset copy */
    FE_TEXT_FIT = 256    /* one line inset from its box's sides, made smaller when it would not fit */
};

/* A translated label set over a picture whose own lettering was taken out (frontend_plaque.h). */
enum { FE_LABEL_NONE, FE_LABEL_STENCIL, FE_LABEL_ENGRAVED };

/* A button prompt: a gamepad control in a family's glyphs, or a key. */
enum { FE_PROMPT_NONE, FE_PROMPT_PAD, FE_PROMPT_KEY };
typedef struct {
    uint8_t kind, family, control, dir, tilt;
    uint32_t vk;
} fe_prompt_t;

#define FE_LEGEND_MAX 4

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
    int arrow_w;                            /* list: width of the scroll column */
    const uint16_t* image;                  /* RGB565 rows */
    const uint32_t* image32;                /* or opaque BGRA rows (image_pitch pixels apart) */
    int image_w, image_h, image_pitch;
    uint16_t image_key;                     /* the transparent colour when image_keyed */
    uint8_t image_keyed;
    uint32_t image_hash;
    const uint16_t* normal_image;           /* hover picture: the normal one, for FA_HOVER_LIFT */
    uint8_t label;                          /* FE_LABEL_*: text is the label, color its ink */
    fe_rect_t label_box;                    /* canvas: the picture's own lettering */
    fe_rect_t label_room;                   /* canvas: where the label may go */
    int normal_pitch;
    float image_zoom;                       /* 0: native size */
    uint32_t shadow_color;                 /* 0xRRGGBB, the text's offset copy (shadow_offset canvas units; 0: none) */
    uint8_t shadow_offset;
    uint8_t border;                         /* a field's sunken edge */
    uint8_t filled;                         /* a text box that paints its own background */
    uint32_t fill_color;                    /* 0xRRGGBB */
    uint32_t selected_color;                /* list: the text colour on the selected row's band */
    fe_prompt_t prompt;                     /* field: drawn in place of its text */
} fe_widget_t;

typedef struct {
    uint8_t kind;
    fe_rect_t rect;  /* the game's window rectangle */
    fe_rect_t panel; /* the plate the frontend draws (canvas) */
    int title;       /* offset in the scene's text, -1 for none */
    int first, count;
} fe_window_t;

typedef struct {
    int screen_w, screen_h; /* the layer */
    int guest_w, guest_h;   /* the game's screen the layer covers */
    int window_count, widget_count, item_count, text_used;
    fe_window_t windows[FE_MAX_WINDOWS];
    fe_widget_t widgets[FE_MAX_WIDGETS];
    int items[FE_MAX_ITEMS];
    char text[FE_TEXT_BYTES];
    int cursor_x, cursor_y; /* canvas */
    const uint16_t* backdrop; /* the main screen's 640x480 art, RGB565 */
    int footer_text;          /* the backdrop's legal lines when they were taken out of it ('\n' between lines), -1 */
    fe_rect_t footer_box;     /* canvas */
    int backdrop_pitch;
    uint32_t backdrop_hash;
    int legend_count;         /* prompts in the screen's lower right corner */
    fe_prompt_t legend[FE_LEGEND_MAX];
    int legend_text[FE_LEGEND_MAX];
} fe_scene_t;

void fe_scene_reset(fe_scene_t* s, int screen_w, int screen_h);
void fe_scene_reset_for(fe_scene_t* s, int guest_w, int guest_h, int screen_w, int screen_h);
/* Offset of a copy of text in the scene, -1 when full. */
int fe_scene_text(fe_scene_t* s, const char* text);
const char* fe_scene_str(const fe_scene_t* s, int offset);
fe_window_t* fe_scene_window(fe_scene_t* s, fe_window_kind_t kind, fe_rect_t rect);
fe_widget_t* fe_scene_widget(fe_scene_t* s, fe_widget_kind_t kind, int32_t id, fe_rect_t rect);
int fe_scene_item(fe_scene_t* s, const char* text);
/* 0 when the legend is full or the text does not fit. */
int fe_scene_legend(fe_scene_t* s, fe_prompt_t prompt, const char* text);
/* Items a list shows: rows from its top one, fewer at the end of the list. */
int fe_list_shown(const fe_widget_t* w);

/* The plate of a dialog: its window and every widget in it, with a margin for the frame. */
#define FE_PANEL_MARGIN 6
void fe_scene_finish_window(fe_scene_t* s, fe_window_t* w);
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

/* Rows of the audio options spread evenly over the band [top, bottom): the new top of each row. */
void fe_layout_rows(int count, int top, int bottom, int row_h, int* row_tops);

/* The lettering a release's own picture gets wrong, in the game's display code page: the Brazilian Portuguese Options
 * button that opens the credits is lettered "AREA DE TRABALHO". NULL for every other version and button. */
const char* fe_release_label_fix(const char* version_id, int window_kind, int button_id);

/* The text a button's lettering is replaced by: in the release's own language only the release's correction, else the
 * language's label. NULL leaves the picture as it is. */
const char* fe_button_text(int release_language, const char* translated, const char* release_fix);

/* Controls (controls.ddf, 320x200): the host's larger layout with wider label columns. */
#define FE_CONTROLS_W 320
#define FE_CONTROLS_H 200
#define FE_CONTROLS_ZOOM 1.75
fe_rect_t fe_controls_window(fe_rect_t window, int canvas_w, int canvas_h);
fe_rect_t fe_controls_rect(fe_rect_t r, int picture);
void fe_controls_polish(fe_scene_t* s, const fe_window_t* w, double zoom);

/* The slider thumb's centre as the game places it: from the value at rest, under the pointer while dragged. */
int fe_slider_thumb_x(fe_rect_t track, int thumb_w, int value, int range);
int fe_slider_drag_x(fe_rect_t track, int thumb_w, int pointer_x);

/* The part of a picture shown (src, picture pixels) and where it goes inside a frame (dst, from the frame's corner):
 * cropped to the frame's shape by up to FE_PHOTO_CROP in one direction, the rest letterboxed. */
#define FE_PHOTO_CROP 1.34
void fe_photo_fit(int image_w, int image_h, double frame_w, double frame_h, double src[4], double dst[4]);

/* Direct Load: how far its window and controls move (canvas, to the left) so that a picture of the selected level fits
 * on its right, and where that picture goes for the moved window. */
#define FE_DIRECT_PHOTO_W 212
#define FE_DIRECT_PHOTO_H 159
#define FE_DIRECT_GAP 8
int fe_layout_direct_dx(fe_rect_t window);
fe_rect_t fe_layout_direct_photo(fe_rect_t window);

/* The loading picture the game shows for a level file ("as.scn": li_a1_0.tga), the largest picture a level has. */
const char* fe_level_picture(const char* scene_file);

/* A list's scroll thumb inside the track between its arrows (canvas); empty when every item fits. */
fe_rect_t fe_list_thumb(fe_rect_t column, int arrow_h, int top, int rows, int count);

#ifdef __cplusplus
}
#endif

#endif
