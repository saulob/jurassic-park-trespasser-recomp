/* The gameplay compass and the objective markers without windows or the guest: angles, distance, visibility, the
 * strip's layout and the camera's projection. North is world +Y, east +X; world units are metres. */
#ifndef COMPASS_POLICY_H
#define COMPASS_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "minimap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP_TEXT_MAX 48
#define CP_LINE_MAX 160
#define CP_METRES_PER_UNIT 1.0f
#define CP_HALF_SPAN_DEG 100.0f
#define CP_TICK_DEG 15
#define CP_TICKS_MAX 16
#define CP_TRAIL_MAX 8
#define CP_TRAIL_RANGE_M 150.0f
#define CP_ROUTE_REACH_M 8.0f
#define CP_SCALE 1.1f
#define CP_OBJECTIVE_SCALE 1.35f
#define CP_REMINDER_MS 4000u
#define CP_REMINDER_FADE_MS 400u
#define CP_NO_OBJECTIVE_MS 2000u
/* The map picture of Show Objective: its longer side as a fraction of the picture height. */
#define CP_MAP_SIDE 0.42f

float cp_wrap360(float deg);
/* (-180, 180] */
float cp_wrap180(float deg);
/* dm_yaw (radians counter-clockwise from +Y) as a compass heading: degrees clockwise from north. */
float cp_heading(float yaw);
float cp_bearing(const float from[3], const float to[3]);
/* Where the target is from the heading: negative left, positive right. */
float cp_relative(float bearing, float heading);
/* On the ground: height differences do not count. */
float cp_distance_m(const float from[3], const float to[3]);
/* "N", "NE", ... for a multiple of 45 degrees; NULL otherwise. */
const char* cp_direction_name(int deg);
/* "8 m", "124 m", "1.2 km" */
int cp_format_distance(float metres, char* out, size_t cap);

/* What is in front of the player this frame. */
typedef struct {
    int level;     /* a running level, no window of the game over it */
    int menu;      /* F3 or F10 open */
    int card;      /* the F2 controls card */
    int loading;   /* a level load is pending */
    int player;    /* Anne found and alive */
    int game_text; /* text that occupies the HUD's part of the picture: gameplay prompts, host prompts (cp_text_hides_hud) */
} cp_scene_t;

/* What an entry of the game's text overlay is. Voice subtitles (the game adds them with category 2, anchored to the
 * bottom of the picture) never reach the HUD; trigger texts and the port's own prompts are laid out from the top and
 * hide it; anything else is told apart by where its first line landed. */
#define CP_TEXT_CATEGORY_SUBTITLE 2u
#define CP_TEXT_ZONE 0.30f /* the top share of the picture the HUD occupies */
typedef enum { CP_TEXT_SUBTITLE, CP_TEXT_HOST_PROMPT, CP_TEXT_GAME_PROMPT, CP_TEXT_MESSAGE } cp_text_kind_t;

/* first_y and raster_h: the entry's first laid-out line and the raster's height, both from the raster's top. */
cp_text_kind_t cp_text_kind(uint32_t category, int host_prompt, int first_y, int raster_h);
int cp_text_hides_hud(cp_text_kind_t kind);

/* Keys compass and waypoints (default on): the strip, and every objective marker (3D diamond, arrow, trail, strip and minimap)
 * with the objective notices. */
void cp_configure(int compass, int waypoints);
int cp_compass_enabled(void);
int cp_waypoints_enabled(void);

int cp_hud_allowed(const cp_scene_t* s);
/* The minimap and its waypoint: gameplay only, but unlike the compass they survive gameplay text (subtitles, narration,
 * tutorial prompts, messages). */
int cp_minimap_allowed(const cp_scene_t* s);
/* The objective marker in the 3D picture (diamond, arrow, trail): the same rule as the minimap, so it also survives text. */
int cp_world_marker_allowed(const cp_scene_t* s);

/* The active camera as the renderer uses it: CPlacement3 (rotation scalar first) and tan of half the vertical view. */
typedef struct {
    float pos[3];
    float rot[4];
    float tan_half_h;
} cp_camera_t;

/* What the HUD draws this frame; empty when nothing. World points are projected at draw time. */
typedef struct {
    int compass;
    float heading;
    float alpha;
    int marker;            /* the current objective has a target */
    float relative;
    float target[3];
    char distance[CP_TEXT_MAX];
    char objective[CP_LINE_MAX]; /* Show Objective: the current objective's title while its reminder runs */
    float objective_alpha;
    int view;              /* camera is valid for projection */
    cp_camera_t camera;
    int trail_count;
    float trail[CP_TRAIL_MAX][3];
    char notice[CP_TEXT_MAX];
    int minimap;                     /* draw the minimap this frame */
    char minimap_level[MM_LEVEL_ID_MAX];
    char minimap_note[CP_TEXT_MAX];  /* a custom level without map data: the minimap says so */
    const minimap_picture_t* level_map; /* Show Objective: the map picture the level itself names, below the title */
    float level_map_alpha;
    float level_map_cursor[2];          /* where Anne is on it, 0..1 across and down */
    int level_map_outside;              /* she is beyond its edge: the cursor stays on the edge */
    float player_pos[3];
    int waypoint;                    /* draw the active objective on the minimap */
    float waypoint_pos[3];
    int creature_count;              /* living creatures near the player, nearest first */
    minimap_creature_t creatures[MM_CREATURES_MAX];
} cp_hud_t;

/* The current objective as the objectives module resolved it: where it is and the real anchors leading there. */
#define CP_ROUTE_MAX 16
typedef struct {
    int set;
    char title[CP_LINE_MAX];
    int target;
    float pos[3];
    int route_count;
    float route[CP_ROUTE_MAX][3];
} cp_goal_t;

/* Show Objective: the current objective's title for CP_REMINDER_MS, fading over the last CP_REMINDER_FADE_MS. */
typedef struct {
    int active;
    uint32_t start_ms;
    char title[CP_LINE_MAX];
} cp_reminder_t;

/* A press: 1 starts (or restarts) the reminder for the current objective; 0 when there is none to show. */
int cp_reminder_press(cp_reminder_t* r, uint32_t now_ms, const cp_goal_t* goal);
/* How visible the title is now (0 when hidden). A changed objective restarts the timer at once (ends it while Waypoints
 * is off); losing it ends it. */
float cp_reminder_step(cp_reminder_t* r, uint32_t now_ms, const cp_goal_t* goal);
/* The title into the frame's HUD at that visibility; nothing while the HUD itself is not allowed. */
void cp_hud_reminder(cp_hud_t* hud, const cp_goal_t* goal, float visibility);

/* Show Objective also shows the map picture a level names (ATX's map), for as long as the title, whether or not there is an
 * objective. */
typedef struct {
    int active;
    uint32_t start_ms;
} cp_map_view_t;

int cp_map_view_press(cp_map_view_t* v, uint32_t now_ms, int has_map);
float cp_map_view_step(cp_map_view_t* v, uint32_t now_ms, int has_map);
/* Where a point is on a picture laid over the world from (x0, y0) at its first corner to (x1, y1) at its last: 0..1 across and
 * down, held at the edge when the point is outside. 0 when the picture has no extent. */
int cp_map_cursor(const minimap_picture_t* map, float x, float y, float out[2], int* outside);

/* The first anchor still ahead of Anne: past the one she is nearest to once she is within reach of it, or once the
 * next one is nearer to her than to it. */
int cp_route_next(const float route[][3], int count, const float at[3]);

/* The strip at the top centre of the picture, sized by its height so wide screens keep it short. */
typedef struct {
    float left, right, centre, top, height;
    float label_px, small_px, objective_px, line_px;
    float text_y, objective_y, notice_y;
} cp_layout_t;

void cp_layout(float view_w, float view_h, cp_layout_t* out);
/* The same for the 3D image inside the presentation surface (x, y, w, h in the surface's pixels): sized by the image and
 * glued to its top edge, whatever margin the game's view-size setting leaves around it. */
void cp_layout_in(float x, float y, float w, float h, cp_layout_t* out);
/* The picture's box below the objective's title: its side scaled to fit, keeping the picture's shape. */
void cp_map_box(const cp_layout_t* l, float view_h, int cols, int rows, float* x, float* y, float* w, float* h);
/* The objective title's outline: a pixel at the base scale, growing with the picture. */
float cp_outline_px(const cp_layout_t* l);
/* A relative direction on the strip; side -1 or 1 when it is past an end and pinned there. */
float cp_strip_x(const cp_layout_t* l, float relative, int* side);
/* 1 in the middle of the strip, down to 0 at its ends. */
float cp_edge_fade(float relative);

typedef struct {
    float relative;
    const char* name; /* N, NE, ... in the language in use; NULL for a plain tick */
    int cardinal;
    int north;
} cp_tick_t;

int cp_ticks(float heading, cp_tick_t* out, int max);

/* A world point on the picture. Off screen (behind included) it is pinned to the picture's edge, inset by margin,
 * along the direction it lies in; dir is that direction in screen space (x right, y down). */
typedef struct {
    float x, y;
    float dir_x, dir_y;
    float depth; /* metres in front of the camera; negative behind */
    int on_screen;
} cp_screen_t;

/* Camera space: x right, y forward, z up. */
void cp_to_camera(const cp_camera_t* c, const float world[3], float out[3]);
int cp_project(const cp_camera_t* c, float view_w, float view_h, float margin, const float world[3], cp_screen_t* out);
/* The same on the 3D image at (x, y) in the surface's pixels. */
int cp_project_in(const cp_camera_t* c, float x, float y, float w, float h, float margin, const float world[3],
                  cp_screen_t* out);

#ifdef __cplusplus
}
#endif

#endif
