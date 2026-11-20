/* Display and render-size arithmetic shared by the host, the launcher and the tests. No Windows calls. */
#ifndef JPT_DISPLAY_H
#define JPT_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* key;   /* aspect_ratio in trespasser.ini */
    const char* label; /* launcher text */
    int num, den;
    int compat_w, compat_h; /* gameplay mode of the software renderer and of the Original preset */
} jpt_aspect_t;

#define JPT_ASPECT_COUNT 5
extern const jpt_aspect_t jpt_aspects[JPT_ASPECT_COUNT];
/* Index of the key; 0 (4:3) when unknown. */
int jpt_aspect_index(const char* key);

typedef struct {
    const char* key;
    const char* label;
    int width, height;   /* the 16:9 size the preset names; 0 for original and native */
    int ultrawide_width; /* the 21:9 monitor size of that height (2560x1080, 3440x1440, 5120x2160) */
} jpt_render_preset_t;

enum { JPT_ASPECT_4_3 = 0, JPT_ASPECT_16_9 = 1, JPT_ASPECT_21_9 = 3 };

enum { JPT_RENDER_ORIGINAL = 0, JPT_RENDER_NATIVE = 5 };
#define JPT_RENDER_PRESET_COUNT 6
extern const jpt_render_preset_t jpt_render_presets[JPT_RENDER_PRESET_COUNT];
/* Index of the key; -1 when unknown. */
int jpt_render_preset_index(const char* key);

typedef struct {
    int width, height;
} jpt_size_t;

typedef struct {
    int left, top, right, bottom;
} jpt_rect_t;

/* The render box of a preset for an aspect ratio: wider shapes keep the preset's height (21:9 takes the ultrawide
 * monitor sizes), narrower ones its width. Native fits the monitor. {0,0} for Original (the compatibility size). */
jpt_size_t jpt_render_box(int preset, int aspect, int monitor_w, int monitor_h);

/* The render target for a guest surface: the box itself when their shapes differ by less than 2% (the picture is
 * shown in the guest's shape, so that difference never reaches the screen), else the largest size of the guest's
 * shape inside the box. Even sizes. */
jpt_size_t jpt_render_target(jpt_size_t box, int guest_w, int guest_h);

/* The game's own gameplay screen. Direct3D with a render box plays at the box itself, up to JPT_GAMEPLAY_MAX_PIXELS
 * (larger boxes get the largest mode of their shape within it, a multiple of 8 wide and high); the software renderer
 * and Original keep the aspect ratio's compatibility mode. */
#define JPT_GAMEPLAY_MAX_PIXELS (3840 * 2160)
jpt_size_t jpt_gameplay_mode(jpt_size_t box, int aspect, int hardware);

/* What the title bar names: the selected render size with Direct3D, else the gameplay mode. Menus and videos never
 * change it. */
jpt_size_t jpt_title_size(jpt_size_t box, jpt_size_t gameplay, int hardware);

/* Host memory one gameplay mode needs: front and back RGB565 buffers, a 32-bit z-buffer, and the D3D11 renderer's
 * RGB565 reference and 32-bit overlay copies. */
unsigned long long jpt_gameplay_host_bytes(jpt_size_t mode);

/* The game sizes its gameplay text for screens up to 600 lines (32 px at 800x600); taller screens keep that share of
 * the height. */
int jpt_ui_text_height(int game_height, int screen_h);

#define JPT_UI_CANVAS_W 640
#define JPT_UI_CANVAS_H 480

/* The game's 640x480 UI canvas inside a frame: the largest 4:3 rectangle, centred, scale not rounded. */
typedef struct {
    jpt_rect_t rect;
    double scale;
} jpt_ui_canvas_t;

jpt_ui_canvas_t jpt_ui_canvas(int frame_w, int frame_h);
/* The canvas pixel under the centre of a frame pixel, clamped to the canvas; 0 when the frame pixel is outside the
 * canvas rectangle. */
int jpt_ui_to_canvas(const jpt_ui_canvas_t* canvas, int x, int y, int* canvas_x, int* canvas_y);
/* The frame pixel under the centre of a canvas pixel. */
void jpt_ui_to_frame(const jpt_ui_canvas_t* canvas, int canvas_x, int canvas_y, int* x, int* y);
/* The frame rectangle covering a canvas rectangle: left/top rounded down, right/bottom up, so clips never cut a pixel. */
jpt_rect_t jpt_ui_rect_to_frame(const jpt_ui_canvas_t* canvas, jpt_rect_t r);

/* The canvas rectangle of a frame as normalised device coordinates {left, top, right, bottom}. */
void jpt_ui_canvas_ndc(const jpt_ui_canvas_t* canvas, int frame_w, int frame_h, float out[4]);

/* Horizontal view scale of a Hor+ view over the original 4:3 one (vertical field of view kept). */
double jpt_hor_plus_scale(int guest_w, int guest_h);

/* Where content of content_w x content_h is shown in a client area: letterboxed (centred, shape kept) or stretched. */
jpt_rect_t jpt_present_rect(int client_w, int client_h, int content_w, int content_h, int stretch);

/* How the game's screen is shown in the host window. The host client size belongs to the user and the platform and
 * takes effect at once; a change of the game's screen size only changes the view, and only when the first frame of the
 * new screen (the one carrying its generation) is about to be shown: until then the active screen keeps describing the
 * picture on display. */
typedef struct {
    jpt_size_t host;
    jpt_size_t guest;
    unsigned generation;
    int stretch;
    jpt_rect_t view;
    int has_pending;
    jpt_size_t pending_guest;
    unsigned pending_generation;
} jpt_presentation_t;

void jpt_presentation_init(jpt_presentation_t* p, int stretch);
/* Both return 1 when the view changed; a size that is not positive is ignored. */
int jpt_presentation_set_host(jpt_presentation_t* p, int client_w, int client_h);
int jpt_presentation_set_guest(jpt_presentation_t* p, int guest_w, int guest_h);
/* The game asks for a screen of its own. With nothing shown yet it is active at once (returns 1); the size already
 * active cancels a pending request; any other size becomes pending under this generation, replacing an earlier one. */
int jpt_presentation_request_guest(jpt_presentation_t* p, int guest_w, int guest_h, unsigned generation);
/* A frame of this generation is about to be shown: the pending screen becomes active. Returns 1 when it did. Frames of
 * any other generation change nothing. */
int jpt_presentation_commit(jpt_presentation_t* p, unsigned generation);
/* The client size a window that follows the game's shape (Window size Auto, Letterbox) takes for a screen: the width
 * kept, the height of the screen's shape, reduced to fit the work area with its frame. Unchanged for a size that is not
 * positive. */
jpt_size_t jpt_auto_client(jpt_size_t client, jpt_size_t guest, int frame_w, int frame_h, int work_w, int work_h);

typedef struct {
    int committed; /* the pending screen became active */
    int resize;    /* the host client became `client`: the window must be resized to it */
    jpt_size_t client;
} jpt_commit_t;

/* jpt_presentation_commit, and with follow_guest the host client takes the new screen's shape in the same step, so the
 * view, the mouse transform and the window geometry change together. */
jpt_commit_t jpt_presentation_commit_frame(jpt_presentation_t* p, unsigned generation, int follow_guest, int frame_w,
                                           int frame_h, int work_w, int work_h);
/* The view the next committed screen will have in the current host, the active one when nothing is pending. */
jpt_rect_t jpt_presentation_next_view(const jpt_presentation_t* p);
/* The active screen's pixel under a host client pixel (rounded down); 0 when it is outside the view or nothing is
 * shown. */
int jpt_presentation_to_guest(const jpt_presentation_t* p, int client_x, int client_y, int* guest_x, int* guest_y);

typedef struct {
    const char* key;
    int width, height; /* 0x0 for auto */
} jpt_window_size_t;

#define JPT_WINDOW_SIZE_COUNT 8
extern const jpt_window_size_t jpt_window_sizes[JPT_WINDOW_SIZE_COUNT];

/* A client size that fits the work area with its frame, keeping its shape; unchanged when it already fits. */
jpt_size_t jpt_fit_window(jpt_size_t client, int frame_w, int frame_h, int work_w, int work_h);

/* 0-based index of the 1-based monitor setting among `count` monitors listed primary first; 0 when out of range. */
int jpt_monitor_index(int setting, int count);

/* The sample count used for a request: the largest supported count not above it (bit n of supported = n samples). */
unsigned jpt_msaa_pick(unsigned requested, unsigned supported_mask);

/* "1920x1080" or "1920 x 1080"-style parsing: 1 when both numbers are in 320..16384 x 240..16384. */
int jpt_parse_size(const char* text, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif
