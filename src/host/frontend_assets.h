/* The HD frontend's painter: the game's own menu pictures scaled to the screen (frontend_art.c) and its text set at the
 * screen's resolution from the system's TrueType fonts (the embedded face where there is no GDI); the loading screen's
 * scene is painted here too. Works on memory only (GDI for glyphs on Windows), so it runs headless. */
#ifndef FRONTEND_ASSETS_H
#define FRONTEND_ASSETS_H

#include "frontend_layout.h"
#include "loading_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_painter fe_painter_t;

/* NULL when the fonts or the noise tables cannot be made. */
fe_painter_t* fe_painter_create(void);
void fe_painter_destroy(fe_painter_t* p);
/* Frees the screen-sized caches (kept while menus are shown). */
void fe_painter_trim(fe_painter_t* p);
/* The face each font role resolved to, for the log. */
const char* fe_painter_face(const fe_painter_t* p, int role);
/* FE_FONT_UI: the face the game itself draws its menu text in (Arial), which every Windows install has. FE_FONT_TITLE:
 * the slab face of the engraved window titles. Each role has a list of faces; a text uses the first one that has all of
 * its characters. */
enum { FE_FONT_PLAQUE, FE_FONT_LABEL, FE_FONT_BODY, FE_FONT_UI, FE_FONT_TITLE, FE_FONT_COUNT };
/* The face a role would draw text (in the display code page) with; "" when no face of the role has all its characters. */
const char* fe_painter_face_for(fe_painter_t* p, int role, const char* text);
/* How a translated label (FE_LABEL_*, display code page) fits a room (pixels) over lettering cap pixels tall: the
 * scale of that size it is drawn at and its lines. 0 when it cannot be measured. */
double fe_painter_label_fit(fe_painter_t* p, int label, const char* text, double cap, double room_w, double room_h,
                            int* lines);
/* The width in pixels of one line of text (display code page) in a role's face at a pixel height. */
double fe_painter_text_width(fe_painter_t* p, int role, int weight, double px, const char* text);

/* Paints the scene into premultiplied BGRA rows of the scene's screen size, only inside clip (screen pixels); the
 * pixels outside are left untouched. threads <= 1 paints on the calling thread. 0 on failure. */
int fe_paint(fe_painter_t* p, const fe_scene_t* s, uint32_t* pixels, long pitch, fe_rect_t clip, int threads);

/* A loading scene the same way: opaque for the loading screen, transparent around a message's plaque. The level's
 * picture is prepared once per art_key; an art32 picture must stay valid until the painter is trimmed. */
int fe_paint_loading(fe_painter_t* p, const ld_scene_t* s, uint32_t* pixels, long pitch, fe_rect_t clip, int threads);

/* The pointer: a square premultiplied BGRA sprite whose side fe_cursor_size gives for a view scale. */
int fe_cursor_size(double scale);
void fe_paint_cursor(double scale, uint32_t* pixels, int size, int* hot_x, int* hot_y);

#ifdef __cplusplus
}
#endif

#endif
