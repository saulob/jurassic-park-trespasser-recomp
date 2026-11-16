/* The F5 Debug / Test Menu and the F10 Cheats menu without windows or the guest: two item lists over one state, so
 * the items both show (Teleport, Level Select, Show Coordinates, Freeze AI) are the same ones. */
#ifndef DEBUG_MENU_POLICY_H
#define DEBUG_MENU_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "compass_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DM_SCENE_MAX 20 /* the game keeps the current scene name in 20 bytes (0x0066E342) */
#define DM_LEVEL_ID_MAX 16
#define DM_LEVELS_MAX 32
#define DM_EDIT_MAX 16
#define DM_TEXT_MAX 64
#define DM_ROWS_MAX 48
#define DM_COORD_LIMIT 100000.0f
#define DM_FEEDBACK_MS 2500u
#define DM_AO_LEVELS 5

typedef enum {
    DM_LEVEL_SELECT,
    DM_RESTART,
    DM_RELOAD,
    DM_FIELD_X,
    DM_FIELD_Y,
    DM_FIELD_Z,
    DM_TELEPORT,
    DM_PREV_KEY,
    DM_NEXT_KEY,
    DM_SET_CHECKPOINT,
    DM_RETURN_CHECKPOINT,
    DM_SHOW_COORDS,
    DM_COPY_COORDS,
    DM_FREEZE_AI,
    DM_MOVE_SPEED,
    DM_JUMP_HEIGHT,
    DM_INVULNERABLE,
    DM_INFINITE_AMMO,
    DM_HEAL,
    DM_SUPER_HEALTH,
    DM_TELEPORT_PAGE,
    DM_THIRD_PERSON,
    DM_FREE_CAMERA,
    DM_COMPASS,
    DM_FIELD_OF_VIEW,
    DM_AMBIENT_OCCLUSION,
    DM_WATER_REFRACTION,
    DM_ENHANCED_SKY,
    DM_CLOSE,
    DM_ITEM_COUNT
} dm_item_t;

typedef enum { DM_MENU_DEBUG, DM_MENU_CHEATS, DM_MENU_OBJECTIVES } dm_menu_t;
typedef enum { DM_GOAL_DONE, DM_GOAL_CURRENT, DM_GOAL_UNKNOWN } dm_goal_status_t;

#define DM_GOALS_MAX 12
#define DM_LINE_MAX 160
typedef enum { DM_PLACE_NONE, DM_PLACE_MAIN_MENU, DM_PLACE_LEVEL } dm_place_t;
typedef enum { DM_PAGE_MAIN, DM_PAGE_LEVELS, DM_PAGE_TELEPORT } dm_page_t;

/* CPlacement3<>: position, then the rotation quaternion as the game stores it (scalar first). */
typedef struct {
    float pos[3];
    float rot[4];
} dm_placement_t;

/* What the guest shows this frame; nothing in it outlives the frame. */
typedef struct {
    dm_place_t place;
    char scene[DM_SCENE_MAX];
    char level[DM_LEVEL_ID_MAX];
    int player; /* the camera's mover was found and the player is alive */
    dm_placement_t at;
    int key_count; /* teleport key points; 0 when there are none or the list cannot be read */
    int key_index; /* the one TNEXT reached last; -1 before the first */
    int ai;        /* -1 unknown, else the DINOS flag */
    int invulnerable, ammo; /* -1 unknown, else the game's INVUL and WOO flags */
    float hp, hp_max;
    int camera_head; /* the active camera follows an animate's head (or is free of it by the Free Camera) */
    int eye;         /* eye_at is the active camera's presence: where the picture is seen from */
    dm_placement_t eye_at;
    float tan_half_h; /* the active camera's vertical view; 0 unknown */
    struct {
        int available; /* the level has objectives the port knows */
        int count;
        struct {
            dm_goal_status_t status;
            char title[DM_LINE_MAX];
            char hint[DM_LINE_MAX]; /* the game's own hint text, for the current one */
        } item[DM_GOALS_MAX];
    } goals;
    int speed, jump, super_health, camera; /* the host's cheats (cheats_policy.h) */
    int ambient_occlusion; /* JPT_AO_* (DM_AO_LEVELS of them); -1 when the renderer cannot draw it */
    int water_refraction, enhanced_sky; /* the running state; -1 when the renderer cannot draw it */
    int fov; /* this session's field of view in degrees; 0 unknown */
} dm_world_t;

typedef enum {
    DM_ACT_NONE,
    DM_ACT_LOAD_LEVEL,
    DM_ACT_RESTART,
    DM_ACT_RELOAD,
    DM_ACT_TELEPORT,
    DM_ACT_PREV_KEY,
    DM_ACT_NEXT_KEY,
    DM_ACT_RETURN_CHECKPOINT,
    DM_ACT_COPY,
    DM_ACT_FREEZE_AI,
    DM_ACT_SPEED,
    DM_ACT_JUMP,
    DM_ACT_INVULNERABLE,
    DM_ACT_AMMO,
    DM_ACT_HEAL,
    DM_ACT_SUPER_HEALTH,
    DM_ACT_CAMERA,
    DM_ACT_AMBIENT_OCCLUSION,
    DM_ACT_WATER_REFRACTION,
    DM_ACT_ENHANCED_SKY,
    DM_ACT_FIELD_OF_VIEW
} dm_action_kind_t;

typedef struct {
    dm_action_kind_t kind;
    dm_placement_t to; /* TELEPORT: only pos is used; RETURN_CHECKPOINT: all of it */
    char scene[DM_SCENE_MAX];
    int on;    /* toggles: the state asked for */
    int value; /* SPEED, JUMP: the multiplier index; CAMERA: ch_camera_t; AMBIENT_OCCLUSION: the level; FIELD_OF_VIEW: degrees */
} dm_action_t;

typedef struct {
    int count;
    char scene[DM_LEVELS_MAX][DM_SCENE_MAX];
} dm_levels_t;

typedef struct {
    uint32_t bit, next_ms;
} dm_repeat_t;

typedef struct {
    int open;
    dm_menu_t menu;
    dm_page_t page;
    int cursor;
    int level_cursor;
    float field[3];
    int fill_fields;
    int editing;
    char edit[DM_EDIT_MAX];
    int show_coords;
    struct {
        int set;
        char level[DM_LEVEL_ID_MAX];
        dm_placement_t at;
    } checkpoint;
    char feedback[DM_TEXT_MAX];
    uint32_t feedback_until;
    dm_action_t pending;
    dm_levels_t levels;
    uint8_t taken[32];
    int char_pending;
    dm_repeat_t repeat;
    int compass_off;
    char notice[DM_TEXT_MAX]; /* the HUD's line under the compass */
    uint32_t notice_until;
} dm_state_t;

typedef enum { DM_ROW_TITLE, DM_ROW_INFO, DM_ROW_SECTION, DM_ROW_ITEM, DM_ROW_GOAL, DM_ROW_NOTE } dm_row_kind_t;

typedef struct {
    dm_row_kind_t kind;
    int item; /* dm_item_t, a level's index on the levels page, a dm_goal_status_t; -1 for other rows */
    int enabled, selected;
    char text[DM_LINE_MAX];
    char value[DM_TEXT_MAX];
    float bar; /* a value row's position in its range, 0..1; negative for none */
} dm_row_t;

void dm_init(dm_state_t* s);
const char* dm_item_label(dm_menu_t menu, dm_item_t item);
const char* dm_menu_title(dm_menu_t menu);
int dm_item_enabled(const dm_state_t* s, const dm_world_t* w, dm_item_t item);

/* F5 (DM_MENU_DEBUG) and F10 (DM_MENU_CHEATS): 1 when a menu opened, closed or replaced the other one. They open
 * only in a level or with the main menu alone in front; never both. */
int dm_toggle(dm_state_t* s, const dm_world_t* w, dm_menu_t menu);
int dm_f5(dm_state_t* s, const dm_world_t* w);
void dm_close(dm_state_t* s);

/* Every key message; 1 when the game must not see it. A key the menu took keeps its repeats and its key-up, so the Esc
 * that closed the menu never reaches a window of the game. */
int dm_key(dm_state_t* s, const dm_world_t* w, unsigned vk, int down, int repeat, int shift, int ctrl,
           uint32_t now_ms);
int dm_char(dm_state_t* s, const dm_world_t* w, char c);
/* Gamepad presses (JPT_PAD_BIT) while the menu is open. */
void dm_pad(dm_state_t* s, const dm_world_t* w, uint32_t pressed, uint32_t now_ms);
/* The D-pad repeats while held: the presses to add to this sample's. */
uint32_t dm_pad_repeat(dm_repeat_t* r, uint32_t held, uint32_t pressed, uint32_t now_ms);

/* Each frame: the menu closes where it cannot act, the checkpoint dies with its level, fields fill on opening. */
void dm_world_seen(dm_state_t* s, const dm_world_t* w);
/* The action the menu asked for, once. */
int dm_take(dm_state_t* s, dm_action_t* out);
void dm_set_levels(dm_state_t* s, const dm_levels_t* levels);
void dm_fill_fields(dm_state_t* s, const dm_placement_t* at);
void dm_feedback(dm_state_t* s, const char* text, uint32_t now_ms);
const char* dm_feedback_text(const dm_state_t* s, uint32_t now_ms);

int dm_rows(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max);

/* Each sample: what the HUD draws. Distance is Anne's; the compass, the markers' directions and the projection are the
 * active camera's. */
void dm_navigate(const dm_state_t* s, const dm_world_t* w, const cp_scene_t* scene, const cp_goal_t* goal,
                 uint32_t now_ms, cp_hud_t* hud);
void dm_notice(dm_state_t* s, const char* text, uint32_t now_ms);
void dm_notice_for(dm_state_t* s, const char* text, uint32_t now_ms, uint32_t duration_ms);

/* "be.scn" -> "BE"; 0 for a name that is not a scene file. */
int dm_level_id(const char* scene, char* out, size_t cap);
int dm_scene_valid(const char* scene);
/* The retail name of a scene's level ("be.scn" -> "Beach"); NULL for a scene the game did not ship. */
const char* dm_level_name(const char* scene);
/* "BE - Beach", or the level id alone for an unknown scene. */
int dm_level_title(const char* scene, char* out, size_t cap);
/* A scene file of the data folder, once per name: the campaign in play order, the test scenes, then any other scene by
 * name. */
int dm_levels_add(dm_levels_t* l, const char* scene);
int dm_teleport_valid(const float pos[3]);
/* Radians about Z, counter-clockwise from +Y: the game's facing axis turned by the LOC cheat's own rotation. */
float dm_yaw(const float rot[4]);
/* "Level=BE X=123.456 Y=78.900 Z=-45.120 Yaw=1.570" */
int dm_format_coords(char* out, size_t cap, const char* level, const dm_placement_t* at);

/* The CTeleport list as std::list<CTeleport*> keeps it: a sentinel node, nodes {next, prev, value}, and the iterator
 * TNEXT returns from and advances. */
typedef int (*dm_read32_t)(uint32_t va, uint32_t* out);
#define DM_KEYS_MAX 1024

typedef struct {
    int count;
    int index; /* the node before the iterator: TNEXT's last answer; -1 when none */
    uint32_t prev_target;
} dm_keys_t;

int dm_keys_walk(dm_read32_t read32, uint32_t head, uint32_t iterator, dm_keys_t* out);
/* Where Previous puts the iterator so that TNEXT itself answers the key point before the last one. */
int dm_prev_key_index(int count, int index);


#ifdef __cplusplus
}
#endif

#endif
