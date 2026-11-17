/* The F10 Cheats and F3 Objectives menus without windows or the guest. F10's list and labels are the cheats of
 * jpt_cheats.h, the list the launcher's Cheats page shows. */
#ifndef DEBUG_MENU_POLICY_H
#define DEBUG_MENU_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "compass_policy.h"
#include "debug_menu_layout.h"

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

typedef enum {
    DM_LEVEL_SELECT,
    DM_FIELD_X,
    DM_FIELD_Y,
    DM_FIELD_Z,
    DM_TELEPORT,
    DM_FREEZE_AI,
    DM_MOVE_SPEED,
    DM_JUMP_HEIGHT,
    DM_INVULNERABLE,
    DM_INFINITE_AMMO,
    DM_HEAL,
    DM_SUPER_HEALTH,
    DM_TELEPORT_PAGE,
    DM_FREE_CAMERA,
    DM_NOCLIP,
    DM_STEALTH,
    DM_BACK,
    DM_CLOSE,
    DM_ITEM_COUNT
} dm_item_t;

typedef enum { DM_MENU_CHEATS, DM_MENU_OBJECTIVES } dm_menu_t;
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
    int speed, jump, super_health, camera, stealth, noclip; /* the host's cheats (cheats_policy.h) */
} dm_world_t;

typedef enum {
    DM_ACT_NONE,
    DM_ACT_LOAD_LEVEL,
    DM_ACT_TELEPORT,
    DM_ACT_FREEZE_AI,
    DM_ACT_SPEED,
    DM_ACT_JUMP,
    DM_ACT_INVULNERABLE,
    DM_ACT_AMMO,
    DM_ACT_HEAL,
    DM_ACT_SUPER_HEALTH,
    DM_ACT_CAMERA,
    DM_ACT_NOCLIP,
    DM_ACT_STEALTH
} dm_action_kind_t;

typedef struct {
    dm_action_kind_t kind;
    dm_placement_t to; /* TELEPORT: only pos is used */
    char scene[DM_SCENE_MAX];
    int on;    /* toggles: the state asked for */
    int value; /* SPEED, JUMP: the multiplier index; CAMERA: ch_camera_t; NOCLIP, STEALTH: on */
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
    char feedback[DM_TEXT_MAX];
    uint32_t feedback_until;
    dm_action_t pending;
    dm_levels_t levels;
    uint8_t taken[32];
    int char_pending;
    dm_repeat_t repeat;
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
} dm_row_t;

void dm_init(dm_state_t* s);
const char* dm_item_label(dm_menu_t menu, dm_item_t item);
const char* dm_menu_title(dm_menu_t menu);
int dm_item_enabled(const dm_state_t* s, const dm_world_t* w, dm_item_t item);

/* F10 (DM_MENU_CHEATS) and F3 (DM_MENU_OBJECTIVES): 1 when a menu opened, closed or replaced the other one. They open
 * only in a level or with the main menu alone in front; never both. */
int dm_toggle(dm_state_t* s, const dm_world_t* w, dm_menu_t menu);
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

/* Each frame: the menu closes where it cannot act, fields fill on opening. */
void dm_world_seen(dm_state_t* s, const dm_world_t* w);
/* The action the menu asked for, once. */
int dm_take(dm_state_t* s, dm_action_t* out);
void dm_set_levels(dm_state_t* s, const dm_levels_t* levels);
void dm_fill_fields(dm_state_t* s, const dm_placement_t* at);
void dm_feedback(dm_state_t* s, const char* text, uint32_t now_ms);
const char* dm_feedback_text(const dm_state_t* s, uint32_t now_ms);

int dm_rows(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max);
/* What the selected row does, one short line; NULL when it needs none. */
const char* dm_help_text(const dm_state_t* s, const dm_world_t* w);
/* The level as the menus name it: "Beach", the id alone for a level the game did not ship. */
int dm_level_display(const dm_world_t* w, char* out, size_t cap);

/* The pointer over the panel (x, y in the presentation view's pixels, hits from the last draw). A move selects the row
 * under it; a click on the close button closes the menu, on a row selects and uses it. Both are 1 when the menu is open
 * and the pointer event is its own. */
int dm_mouse_move(dm_state_t* s, const dm_world_t* w, const dml_hits_t* hits, float x, float y);
int dm_mouse_click(dm_state_t* s, const dm_world_t* w, const dml_hits_t* hits, float x, float y, uint32_t now_ms);

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
#ifdef __cplusplus
}
#endif

#endif
