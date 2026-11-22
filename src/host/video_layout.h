#ifndef TRESPASSER_VIDEO_LAYOUT_H
#define TRESPASSER_VIDEO_LAYOUT_H

#include <stdint.h>

#define VL_SMK_HEADER_BYTES 104
#define VL_SMK_Y_INTERLACED 2u
#define VL_SMK_Y_DOUBLED 4u

typedef struct {
    uint32_t width, height, frames; /* height as stored in the file */
    int32_t frame_rate;             /* the header's field: > 0 ms per frame, < 0 tens of microseconds, 0 = 10 fps */
    uint32_t flags;
    uint32_t audio_rate, audio_bits, audio_channels; /* track 0; rate 0 without audio */
    int audio_compressed;
} vl_smk_info_t;

int vl_smk_parse(const uint8_t* header, uint32_t size, vl_smk_info_t* out);
double vl_smk_ms_per_frame(int32_t frame_rate);
/* Rows the decoder fills for a file's stored height (Y-interlaced and Y-doubled files are shown at twice it). */
uint32_t vl_smk_shown_rows(uint32_t stored_rows, uint32_t flags);

typedef enum { VL_ROWS_PROGRESSIVE, VL_ROWS_REBUILD } vl_rows_t;

/* Which of the decoder's rows hold the picture. Y-interlaced files leave every second row unwritten and Y-doubled
 * ones repeat each row: both carry shown_rows / 2 rows of picture, on the shown rows parity + 2k. */
typedef struct {
    vl_rows_t rows;
    int stored_rows;
    int shown_rows;
    int parity;
} vl_policy_t;

vl_policy_t vl_policy_for(uint32_t flags, uint32_t shown_rows);
const char* vl_policy_text(const vl_policy_t* p);

typedef struct {
    int x, y, w, h;
} vl_rect_t;

/* The picture (square pixels) as large as it fits in the output, centred: black bars above and below when the
 * output is narrower than the picture, side extensions when it is wider. */
typedef struct {
    vl_rect_t video;
    int side; /* width of each side extension */
    int bars; /* height of the bar above the picture */
} vl_layout_t;

vl_layout_t vl_layout(int out_w, int out_h, int content_w, int content_h);

/* The overlay's render size: the client's shape inside the render box (0x0: none), never larger than the client. */
void vl_output_size(int client_w, int client_h, int box_w, int box_h, int* w, int* h);

/* "menu\\newgame" -> "newgame.smk" */
void vl_display_name(const char* guest_name, char* out, int cap);

uint16_t vl_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* The palette's own colours for the RGB565 codes the decoder writes: map[code] is 0xFF000000 | RGB of the first
 * entry with that code, 0 for a code no entry has. */
typedef struct {
    uint32_t map[65536];
    uint16_t codes[256];
    int count;
    uint8_t palette[768];
    int valid;
} vl_palette_map_t;

/* 1 when the palette differs from the one the map was built for (and the map now follows it). */
int vl_palette_map_update(vl_palette_map_t* m, const uint8_t* palette);

/* One row of decoder pixels as RGBA bytes. A code the palette map does not hold (the palette changed since the block
 * was drawn) is the code's own colour, never black. gamma is the three 256-entry 16-bit ramps, or NULL. */
void vl_convert_row_rgba(const uint16_t* row, int count, int rgb565, const vl_palette_map_t* map, const uint16_t* gamma,
                         uint8_t* out);

/* AUTO is the game's own choice; the Video Test forces one playback either way. */
typedef enum { VL_PLAY_AUTO, VL_PLAY_ORIGINAL, VL_PLAY_HD } vl_play_mode_t;

const char* vl_play_mode_name(vl_play_mode_t mode);
/* The player takes the forced mode once: every later playback is AUTO again. */
vl_play_mode_t vl_play_mode_take(vl_play_mode_t* forced);
/* ORIGINAL plays without an HD session: every present is the game's own. */
int vl_play_uses_session(vl_play_mode_t mode);

typedef enum { VL_SHOW_CLASSIC, VL_SHOW_BLACK, VL_SHOW_FRAME } vl_show_t;

typedef struct {
    int playing;    /* inside the game's player */
    int overlay;    /* the HD picture covers the game's own */
    int failed;     /* the HD picture failed: the game's own playback until the player returns */
    int started;    /* the first HD frame was shown */
    int stray_line; /* the picture's last row carries a stray line */
    unsigned frames, dropped;
    int32_t last_frame;
    double max_ms;
} vl_session_t;

void vl_session_begin(vl_session_t* s);
/* A present inside the player: raster_ready once its picture buffer exists, first_pending until its first frame is
 * drawn, buffer_path when the decoder blits to a window DC instead of the screen. */
vl_show_t vl_session_route(const vl_session_t* s, int hardware, int raster_ready, int first_pending, int buffer_path);
void vl_session_shown(vl_session_t* s, vl_show_t what, int32_t frame_number, double ms);
void vl_session_failed(vl_session_t* s);
void vl_session_end(vl_session_t* s);
/* After the game's own picture reached the screen: 1 when the overlay must go now. */
int vl_session_main_presented(vl_session_t* s);

/* Pixels of a decoded picture's last row (RGB565 or XRGB1555 codes) that are lit while the three rows above them, the
 * rest of their 4x4 codec block, are black. */
int vl_lone_bottom_pixels(const uint8_t* rows, long stride, int width, int height);

/* A progressive frame (rows of decoder codes, stride in bytes) as RGBA bytes, on the CPU. stray_bottom_line applies the
 * rule the GPU converter has (stray_row): in the last row a lit code whose three codes above are all black takes the
 * code above. */
void vl_convert_frame_rgba(const uint8_t* rows, long stride, int width, int height, int rgb565, const vl_palette_map_t* map,
                           const uint16_t* gamma, int stray_bottom_line, uint8_t* out);
/* Which passes a frame goes through, from its row policy, its layout and what the session has learned: the one policy
 * both backends' video pipelines read. */
typedef struct {
    int smooth, deblock, glow, stray;
} vl_stages_t;
vl_stages_t vl_video_stages(const vl_policy_t* policy, const vl_layout_t* layout, int stray_latched);

/* credits.smk's stray line shows 359-365 lone pixels at full brightness; the other videos never more than 35. */
#define VL_STRAY_LINE_MIN 128
/* One frame's lone pixels: 1 when they mark the last row as a stray line for the rest of the playback (once). */
int vl_session_stray_line(vl_session_t* s, int lone_pixels);

#endif
