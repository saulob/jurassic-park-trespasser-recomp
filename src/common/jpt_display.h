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
    int guest_w, guest_h; /* gameplay logical resolution the guest runs at */
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

/* Horizontal view scale of a Hor+ view over the original 4:3 one (vertical field of view kept). */
double jpt_hor_plus_scale(int guest_w, int guest_h);

/* Where content of content_w x content_h is shown in a client area: letterboxed (centred, shape kept) or stretched. */
jpt_rect_t jpt_present_rect(int client_w, int client_h, int content_w, int content_h, int stretch);

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
