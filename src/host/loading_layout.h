/* The game's loading screen (CLoaderWnd, menu\loader.ddf) as the HD loading layer draws it: the picture the game picks
 * for a level, where each part goes on any screen, and what changed between two of the loader's draws. No Windows
 * calls. */
#ifndef LOADING_LAYOUT_H
#define LOADING_LAYOUT_H

#include <stdint.h>

#include "frontend_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LD_TABLE_ROWS 10
#define LD_MAX_GAUGES 4
#define LD_MAX_NOTES 2
#define LD_TEXT_BYTES 96
#define LD_SLOT_BYTES 32

typedef struct {
    const char* scene;
    const char* image;
} ld_art_t;

/* The retail table at 0x006610D0: scene file and picture base ("<image>_0.tga"); the last row is the catch-all. */
extern const ld_art_t ld_retail_table[LD_TABLE_ROWS];

/* The row CLoaderWnd (0x00415C40) shows for a scene: the first row whose scene matches without case, else the last. */
int ld_select(const char* scene, const char* const* scenes, int count);
/* The HD art slot of a picture base: "menu\\li_be" gives "li_be"; 0 unless it is a plain [A-Za-z0-9_] name. */
int ld_slot_name(const char* image, char* out, int cap);

typedef struct {
    float fraction; /* the part of the bar the game fills, 0..1 */
    uint32_t color; /* 0xRRGGBB, the bar's own colour */
    uint32_t text_color;
    char caption[LD_TEXT_BYTES];
} ld_gauge_t;

enum { LD_SCENE_NONE, LD_SCENE_LOADER, LD_SCENE_MESSAGE };

typedef struct {
    int kind, screen_w, screen_h;
    const uint16_t* art565; /* the game's picture of the level, RGB565 rows art_pitch pixels apart */
    const uint32_t* art32;  /* or an HD picture, opaque BGRA rows art_w pixels apart */
    int art_w, art_h, art_pitch;
    uint32_t art_key;
    ld_gauge_t gauges[LD_MAX_GAUGES];
    int gauge_count;
    char notes[LD_MAX_NOTES][LD_TEXT_BYTES]; /* the window's other lines of text, over the gauges */
    uint32_t note_colors[LD_MAX_NOTES];
    int note_count;
    char message[LD_TEXT_BYTES];
} ld_scene_t;

/* Canvas units (fe_view): the picture fills the 4:3 canvas in an iron frame, the notes and gauges sit on a plate over
 * its lower part, a message plaque at its centre; the rest of the screen is the picture's blurred surroundings. */
typedef struct {
    fe_view_t view;
    fe_rect_t frame, photo, plate, message;
    fe_rect_t notes[LD_MAX_NOTES], captions[LD_MAX_GAUGES], grooves[LD_MAX_GAUGES];
    int gauge_count, note_count;
} ld_layout_t;

void ld_layout_make(ld_layout_t* l, int screen_w, int screen_h, int gauge_count, int note_count);
void ld_layout_scene(ld_layout_t* l, const ld_scene_t* s);
/* The screen pixels a gauge's fill can change. */
fe_rect_t ld_gauge_pixels(const ld_layout_t* l, int gauge);
/* The screen x where a gauge's fill ends. */
int ld_fill_right(const ld_layout_t* l, int gauge, float fraction);

enum { LD_DIFF_SAME, LD_DIFF_GAUGES, LD_DIFF_ALL };
/* What must be painted again: nothing, *dirty (screen pixels, only gauge fills moved) or the whole screen. */
int ld_scene_diff(const ld_scene_t* before, const ld_scene_t* now, const ld_layout_t* l, fe_rect_t* dirty);

#ifdef __cplusplus
}
#endif

#endif
