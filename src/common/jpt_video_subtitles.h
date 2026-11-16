/* Subtitles for the game's videos: the timelines of the retail files, the cue on screen for a Smacker frame, and the
 * layout of the text over the picture. Text is looked up with jpt_tr. No Windows or graphics API calls. */
#ifndef JPT_VIDEO_SUBTITLES_H
#define JPT_VIDEO_SUBTITLES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames are the retail file's own (0-based, the frame on screen); a cue shows from first to last inclusive. */
typedef struct {
    uint16_t first, last;
    const char* key; /* jpt_tr key */
} jvs_cue_t;

#define JVS_FINGERPRINT_BYTES 65536u

typedef struct {
    const char* name; /* "tpassintro.smk" */
    uint64_t size;
    const char* sha256; /* of the first JVS_FINGERPRINT_BYTES bytes (the whole file when shorter) */
    uint32_t frames;
    const jvs_cue_t* cues;
    int cue_count;
} jvs_timeline_t;

extern const jvs_timeline_t jvs_timelines[];
extern const int jvs_timeline_count;

/* By file name, case-insensitive; NULL for a video without dialogue or unknown. */
const jvs_timeline_t* jvs_timeline_named(const char* name);
/* The retail file itself: same size and the same first bytes. */
int jvs_fingerprint_matches(const jvs_timeline_t* t, uint64_t size, const char* sha256_hex);

typedef enum { JVS_SELECTED, JVS_DISABLED, JVS_NO_DIALOGUE, JVS_NOT_RETAIL } jvs_choice_t;
/* The timeline a playback uses: none when the option is Off, the video has no dialogue, or the file is not the retail one. */
jvs_choice_t jvs_select(int enabled, const char* name, uint64_t size, const char* sha256_hex, const jvs_timeline_t** out);

/* The cue showing on a frame, -1 between cues. */
int jvs_cue_at(const jvs_timeline_t* t, int32_t frame);
double jvs_frame_ms(int32_t frame, double ms_per_frame);

/* One playback. A frame number lower than one already shown means the decoder wrapped after the last frame: nothing
 * shows from then on. */
typedef struct {
    const jvs_timeline_t* timeline;
    int playing, ended;
    int32_t last_frame;
} jvs_session_t;

void jvs_session_begin(jvs_session_t* s, const jvs_timeline_t* t);
int jvs_session_cue(jvs_session_t* s, int32_t frame);
void jvs_session_end(jvs_session_t* s);

typedef struct {
    int x, y, w, h;
} jvs_rect_t;

/* Text size from the picture's height on the target, so it follows the render size and not the desktop's DPI. */
int jvs_font_px(const jvs_rect_t* picture);
int jvs_outline_px(int font_px);
/* Widest line: a share of the picture's width, never wider than the target's safe area. */
int jvs_max_line_width(const jvs_rect_t* picture, int target_w);
/* The text block centred under the picture's middle, near its bottom, inside the target's safe area. */
jvs_rect_t jvs_place(int target_w, int target_h, const jvs_rect_t* picture, int block_w, int block_h);

#define JVS_MAX_LINES 4
typedef struct {
    int start, length; /* bytes of the UTF-8 text */
    int width;
} jvs_line_t;

typedef int (*jvs_width_fn)(void* ctx, const char* utf8, int bytes);
/* Lines of at most max_width: one when it fits, else two balanced lines, else as many as needed (JVS_MAX_LINES). */
int jvs_wrap(const char* text, int max_width, jvs_width_fn width, void* ctx, jvs_line_t* lines);

/* Premultiplied BGRA image over a BGRA target, clipped to it. */
void jvs_blend(uint32_t* target, long target_pitch_pixels, int target_w, int target_h, const uint32_t* image, int image_w,
               int image_h, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
