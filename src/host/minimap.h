/* The minimap: world bounds, the prepared map geometry per level and the player-centred view, clipping and markers,
 * without the guest or the window. North is world +Y, east +X; world units are metres. The data comes from tools/minimap_bounds.py and
 * tools/minimap_map.py (minimap_bounds.inc, minimap_map.inc). */
#ifndef MINIMAP_H
#define MINIMAP_H

#include <stdint.h>

#include "jpt_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MM_LEVEL_ID_MAX 16
#define MM_LEVEL_COUNT 11

#define MM_KIND_LAND 1
#define MM_KIND_WATER 2
/* Contour level of the smoothed layers: water also keeps streams narrower than a cell. */
#define MM_ISO_LAND 0.5f
#define MM_ISO_WATER 0.38f

#define MM_LM_BUILDING 0
#define MM_LM_WALL 1
#define MM_LM_ROAD 2
#define MM_LM_GATE 3

/* Zoom scales the visible world span around the player: at zoom 1 the minimap diameter covers MM_VIEW_SPAN_M. */
#define MM_VIEW_SPAN_M 800.0f
#define MM_ZOOM_MIN 1.0f
#define MM_ZOOM_MAX 6.0f
#define MM_ZOOM 2.0f

/* Marker sizes and the edge anchor, as fractions of the minimap radius. */
#define MM_PLAYER_K 0.105f
#define MM_WAYPOINT_K 0.07f
#define MM_EDGE_ARROW_K 0.10f
#define MM_WAYPOINT_CLEAR 0.26f
#define MM_EDGE_ANCHOR 0.84f

/* Placement in the gameplay picture: bottom-left, diameter a fraction of the picture height. */
#define MM_HEIGHT_FRACTION 0.25f
#define MM_MARGIN_FRACTION 0.0225f
#define MM_DIAMETER_MIN 96.0f
/* Colour semantics (r, g, b): land dark green, water blue, buildings grey, walls and fences light grey, roads muted
 * sand, the player white, the objective yellow, threatening creatures red, other living creatures bright green. */
#define MM_COLOR_LAND 0.17f, 0.30f, 0.15f
#define MM_COLOR_WATER 0.13f, 0.40f, 0.80f
#define MM_COLOR_BUILDING 0.58f, 0.58f, 0.61f
#define MM_COLOR_WALL 0.86f, 0.86f, 0.89f
#define MM_COLOR_ROAD 0.42f, 0.39f, 0.31f
#define MM_COLOR_LANDMARK_EDGE 0.07f, 0.07f, 0.08f
#define MM_COLOR_PLAYER 1.0f, 1.0f, 1.0f
#define MM_COLOR_OBJECTIVE 1.0f, 0.84f, 0.22f
#define MM_COLOR_VEHICLE 0.96f, 0.46f, 0.06f
#define MM_COLOR_THREAT 0.92f, 0.16f, 0.14f
#define MM_COLOR_PASSIVE 0.42f, 0.88f, 0.30f

#define MM_CREATURES_MAX 6
#define MM_CREATURE_RANGE_MIN_M 35.0f
#define MM_CREATURE_RANGE_MAX_M 90.0f
#define MM_CREATURE_RANGE_VIEW 0.75f
#define MM_CREATURE_K 0.03f

/* A creature's size class comes from its instance scale (retail: raptors 2.5-3.1, Triceratops, Stegosaurus and
 * Parasaurolophus 4.6-5.7, Albertosaurus and T. rex 6.2-7.8, Brachiosaurus 10-15) and scales the marker radius
 * MM_CREATURE_K: about -1, 0, +1 and +2 px at 1080p. */
#define MM_SIZE_CLASSES 4
#define MM_SIZE_STANDARD 1
#define MM_SCALE_STANDARD 2.0f
#define MM_SCALE_MEDIUM 4.0f
#define MM_SCALE_LARGE 6.0f
#define MM_CREATURE_SIZE_FACTORS { 0.75f, 1.0f, 1.25f, 1.5f }
#define MM_CREATURE_OUTLINE_PX 1.0f

/* A vehicle is a rectangle by size class, as fractions of the minimap radius: about 8 x 5, 9 x 6 and 12 x 7 px at 1080p. */
#define MM_VEHICLE_SMALL 0
#define MM_VEHICLE_MEDIUM 1
#define MM_VEHICLE_LARGE 2
#define MM_VEHICLE_CLASSES 3
#define MM_VEHICLE_LENGTH_K { 0.059f, 0.067f, 0.089f }
#define MM_VEHICLE_WIDTH_K { 0.037f, 0.044f, 0.052f }
#define MM_VEHICLE_MIN_LENGTH_PX { 6.0f, 7.0f, 9.0f }
#define MM_VEHICLE_MIN_WIDTH_PX { 3.5f, 4.5f, 5.0f }
#define MM_VEHICLE_OUTLINE_PX 1.0f

/* Landmarks are drawn at least this thick on screen. */
#define MM_LANDMARK_MIN_PX 3.0f
#define MM_GATE_MIN_PX 5.0f

#define MM_CLIP_SIDES 160
#define MM_CLIP_MAX (MM_CLIP_SIDES + 8)

typedef struct {
    const char* id;
    float min_x, min_y, max_x, max_y;
} minimap_bounds_t;

/* One horizontal run of the rasterized map: row and start are cell indices, kind is MM_KIND_*. */
typedef struct {
    uint16_t start, len;
    uint8_t row, kind;
} minimap_run_t;

/* A constructed object's oriented footprint: four corners in world metres, kind is MM_LM_*. */
typedef struct {
    float x[4], y[4];
    uint8_t kind;
} minimap_landmark_t;

/* A ground vehicle: its centre in world metres, the unit direction of its long side and its size class. */
typedef struct {
    float x, y, ax, ay;
    int size;
} minimap_vehicle_t;

typedef struct {
    const char* id;
    uint16_t cols, rows;
    const minimap_run_t* runs;
    uint16_t count;
    const minimap_landmark_t* landmarks;
    uint16_t landmark_count;
    const minimap_vehicle_t* vehicles;
    uint16_t vehicle_count;
} minimap_level_map_t;

typedef enum { MM_CREATURE_NONE, MM_CREATURE_PASSIVE, MM_CREATURE_THREAT } minimap_creature_kind_t;

typedef struct {
    float x, y;
    int threat;
    int size; /* a size class, 0 .. MM_SIZE_CLASSES - 1 */
} minimap_creature_t;

/* The player-centred transform: the player's world position is always the circle's centre (screen y grows down). The
 * world direction drawn straight up: north (+y) until minimap_face sets the player's heading. */
typedef struct {
    float cx, cy, radius;
    float scale; /* screen pixels per world metre */
    float player_x, player_y;
    float up_x, up_y;
} minimap_frame_t;

typedef struct {
    float x, y;
} minimap_point_t;

typedef struct {
    float x, y, w, h;
} minimap_rect_t;

typedef enum { MM_ICON_DIAMOND, MM_ICON_ARROW } minimap_icon_t;

typedef struct {
    minimap_icon_t icon; /* diamond on the map, arrow on the edge: the in-world waypoint's own pair */
    int inside;     /* the objective lies within the visible circle */
    float x, y;     /* marker anchor on screen */
    float dir_x, dir_y; /* unit screen direction from the player to the objective */
} minimap_marker_t;

/* The level's world bounds, or NULL for a level with no map data. */
const minimap_bounds_t* minimap_bounds(const char* level_id);
/* The level's prepared map runs, or NULL for a level with no map data. */
const minimap_level_map_t* minimap_map(const char* level_id);

/* The frame for a gameplay picture and the live player position; 0 when the picture is too small. */
int minimap_place(const minimap_rect_t* picture, float player_x, float player_y, float zoom, minimap_frame_t* out);
void minimap_frame(minimap_frame_t* f, float cx, float cy, float radius, float player_x, float player_y, float zoom);
/* World (x, y) to screen: the player lands on (cx, cy), east grows right, north grows up. */
/* Player-up: the heading (compass degrees, clockwise from north) points straight up; one sine and cosine per frame. */
void minimap_face(minimap_frame_t* f, float heading_deg);
/* minimap_place, then minimap_face. */
int minimap_place_facing(const minimap_rect_t* picture, float player_x, float player_y, float heading_deg, float zoom,
                         minimap_frame_t* out);
void minimap_to_screen(const minimap_frame_t* f, float x, float y, float* sx, float* sy);
/* The world rect of one map run (min_x < max_x, south_y < north_y); 0 with bad input. */
int minimap_run_world(const minimap_bounds_t* b, const minimap_level_map_t* m, const minimap_run_t* run, float* min_x,
                      float* south_y, float* max_x, float* north_y);
typedef void (*minimap_poly_fn)(void* user, const minimap_point_t* points, int count);
/* The layer's smoothed shape (a blurred field contoured at its iso level), as convex polygons clipped to the circle. */
void minimap_layer_visit(const minimap_frame_t* f, const minimap_bounds_t* b, const minimap_level_map_t* m, int kind,
                         minimap_poly_fn fn, void* user);
/* The landmark clipped to the minimap circle as a convex screen polygon; the point count, 0 when nothing is visible. */
int minimap_landmark_polygon(const minimap_frame_t* f, const minimap_landmark_t* landmark, minimap_point_t out[MM_CLIP_MAX]);
/* The vehicle's whole rectangle on screen, along its direction; 4, or 0 when any corner (outline included) is outside the circle. */
int minimap_vehicle_polygon(const minimap_frame_t* f, const minimap_vehicle_t* vehicle, minimap_point_t out[MM_CLIP_MAX]);
/* Convex screen polygon clipped to the circle; the point count, 0 when nothing is visible. */
int minimap_clip_polygon(const minimap_frame_t* f, const minimap_point_t* in, int count, minimap_point_t out[MM_CLIP_MAX]);
/* Screen rect clipped to the circle; the point count, 0 when nothing is visible. */
int minimap_clip_rect(const minimap_frame_t* f, float x0, float y0, float x1, float y1, minimap_point_t out[MM_CLIP_MAX]);
/* The active objective: its own position inside the circle, else an anchor on the edge pointing toward it. Never
 * closer to the player than the marker clearance. */
void minimap_waypoint(const minimap_frame_t* f, float x, float y, minimap_marker_t* out);
/* A living creature is shown red while the game's AI has it awake and engaged, green otherwise; the dead are not shown. */
minimap_creature_kind_t minimap_creature_kind(int dead, int engaged);
/* Engaged: the animal's brain is running and the player is within the radius the game keeps it awake for. Without a
 * player position or a usable radius, running is enough. */
int minimap_creature_engaged(int active, int have_distance, float distance, float keep_radius);
/* How far from the player a creature is shown: a share of the visible radius, kept within the practical bounds. */
float minimap_creature_range(float zoom);
/* The nearest finite candidates within range metres of the player, nearest first, at most max. */
int minimap_creatures_select(float player_x, float player_y, float range, const minimap_creature_t* in, int count,
                             minimap_creature_t* out, int max);
/* The size class of a creature of this instance scale; a scale that is not a positive finite number is the standard class. */
int minimap_creature_size_class(float scale);
/* The marker radius in pixels of a size class on a minimap of this radius (an out-of-range class is the standard one). */
float minimap_creature_marker_radius(float minimap_radius, int size_class);
/* The creature's screen position; 0 when its whole marker, outline included, is not inside the circle (it is then not drawn). */
int minimap_creature_place(const minimap_frame_t* f, const minimap_creature_t* creature, float* sx, float* sy);
/* Unit screen direction of a heading in degrees (0 = north = up, 90 = east = right). */
void minimap_heading_dir(float heading_deg, float* dx, float* dy);
/* Triangle pointing along (dx, dy) around (cx, cy) with tip distance k; out[0] is the tip. */
void minimap_arrow(float cx, float cy, float dx, float dy, float k, minimap_point_t out[3]);
/* The player's arrowhead: tip, left wing, rear notch, right wing, its centroid exactly on (cx, cy). */
void minimap_player_shape(float cx, float cy, float dx, float dy, float k, minimap_point_t out[4]);

/* F5 and L3 + R3 show or hide the minimap, in gameplay only. */
int minimap_toggle_allowed(int gameplay, int menu_open, int card_shown);
/* Flips the minimap and returns whether it is now on. */
int minimap_toggle(void);
#define MM_PAD_CHORD (JPT_PAD_BIT(JPT_PAD_LEFT_STICK) | JPT_PAD_BIT(JPT_PAD_RIGHT_STICK))

void minimap_configure(int enabled);
int minimap_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
