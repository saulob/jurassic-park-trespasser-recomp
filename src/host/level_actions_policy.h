/* The ActionTypes of ATX and Trespasser CE that custom levels use: what each does, apart from the guest. */
#ifndef LEVEL_ACTIONS_POLICY_H
#define LEVEL_ACTIONS_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define LA_FIRST 99
#define LA_LAST 121
#define LA_TEXT_MAX 512
#define LA_ENTRIES_MAX 199
#define LA_RUNS_MAX 256
#define LA_MAP_NAME_MAX 81
#define LA_FIXED_MAX 64
#define LA_SWAPS_MAX 64
#define LA_RESETS_MAX 32
#define LA_STATE_MAX (256 + LA_TEXT_MAX + LA_RUNS_MAX * 4 + LA_FIXED_MAX * 20 + LA_SWAPS_MAX * 12 + LA_RESETS_MAX * 8)

enum {
    LA_SLOW = 100,
    LA_SPEED = 101,
    LA_MAP = 102,
    LA_RELOAD = 103,
    LA_HEALTH = 104,
    LA_VIDEO = 105,
    LA_HINT = 106,
    LA_COLOUR = 109,
    LA_FIX_SPEED = 110,
    LA_END_SPEED = 111,
    LA_CONTROLS = 112,
    LA_PLAYER_AI = 113,
    LA_TELEPORT = 114,
    LA_USE = 115,
    LA_JUMP = 116,
    LA_PARTICLES = 117,
    LA_TEXTURE = 118,
    LA_TRIGGER = 120,
    LA_RELOAD_GUNS = 121
};

/* Whether Trespasser CE gives a trigger an action for the type, and the bytes its scenes of that version keep of it. */
int la_creates(int type);
uint32_t la_scene_bytes(int type, int scene_version);

/* The game's speed: 101 sets it, 100 lays a slower one over it for a while. */
typedef struct {
    float base, current;
    float slow, start, duration, enter, leave, elapsed; /* seconds at the base speed */
    int active;
} la_speed_t;

void la_speed_reset(la_speed_t* s);
float la_speed_clamp(float speed);
void la_speed_set(la_speed_t* s, float speed);
void la_speed_slow(la_speed_t* s, float speed, float duration_ms, float enter_ms, float leave_ms);
void la_speed_step(la_speed_t* s, float game_seconds);
float la_speed_factor(const la_speed_t* s);

typedef struct {
    uint32_t mask;
    int mouse_off;
} la_controls_t;

void la_controls_reset(la_controls_t* c);
/* 112: keys 0 off, 1 on, 2 on without moving and jumping; mouse 0 off, 1 on; anything else leaves that half as it is. */
void la_controls_set(la_controls_t* c, int keys, int mouse);
int la_controls_active(const la_controls_t* c);
void la_controls_apply(const la_controls_t* c, float rotate[2], float move[2], uint32_t* state, uint32_t* hit);

/* 106: the text of its two properties; 0 when it has none. */
int la_hint_join(const char* sample, const char* frame, char* out, size_t cap);
/* The text hint_strings.txt has for a key; 0 when it has none. */
int la_hint_lookup(const char* strings, size_t size, const char* key, char* out, size_t cap);

/* 103: the rounds a gun gets. */
int la_reload_rounds(int ammo, int alt_count, int most);

/* 114: the group a draw falls on, -1 without any weight. */
int la_pick_group(const int* weights, int count, uint32_t draw);
/* Takes one of the first `left` entries out of a list. */
int la_shuffle(uint8_t* list, int left, uint32_t draw);
/* Degrees about x, then y, then z, as w, x, y, z. */
void la_quaternion(float x, float y, float z, float out[4]);
uint32_t la_random(uint32_t* seed);

/* 102: the file of its Sample, which ends at a quote; 0 when it has none. */
int la_map_name(const char* sample, char out[LA_MAP_NAME_MAX]);

/* A picture (blue, green, red, alpha) as at most side x side colours, 0xAARRGGBB. */
void la_map_cells(const uint8_t* bgra, int width, int height, int side, int* cols, int* rows, uint32_t* cells);

/* 117: where an instance's +Y axis points, of length 1. */
void la_forward(const float quaternion[4], float out[3]);

/* 104: the health after losing that much; losing 0 is a whole cure. */
float la_health(float now, float most, float lost);

/* 105: the name the game's video window takes, below the data folder and without ".smk"; 0 when it is none. */
#define LA_VIDEO_MAX 48
int la_video_name(const char* sample, char out[LA_VIDEO_MAX]);

/* 109: 0x00RRGGBB. */
uint32_t la_colour(int r, int g, int b);

/* 110: an object that keeps a velocity, and the seconds left of it. */
#define LA_UNLIMITED (-100.0f)
typedef struct {
    uint32_t instance;
    float velocity[3], left;
} la_fixed_t;

float la_fixed_seconds(int interval_ms);
/* Takes time off; 0 when it is over. */
int la_fixed_step(la_fixed_t* f, float seconds);

/* 113: the AI type of a part that has the type now; -1 goes between seen (5) and unseen (0). */
#define LA_AI_NONE (-2)
int la_ai_valid(int type);
int la_ai_type(int type, int now);

/* 115: the rounds after one is taken by hand; -1 is empty and clicked. */
int la_rounds_left(int rounds, int no_click);

/* 116: an orientation pitched up by degrees. */
void la_pitched(const float quaternion[4], float degrees, float out[4]);

/* 118 and 120: what a save keeps of them. */
typedef struct {
    uint32_t instance;
    int32_t material, frame; /* from 0 */
} la_swap_t;

typedef struct {
    uint32_t trigger;
    int32_t count;
} la_reset_t;

/* 120: the engine's number for a probability in percent. */
uint32_t la_probability(int percent);

/* What a save keeps; runs: of each action, by its place in the level. */
typedef struct {
    la_speed_t speed;
    la_controls_t controls;
    char hint[LA_TEXT_MAX];
    char map[LA_MAP_NAME_MAX];
    float map_corners[4];
    int32_t runs[LA_RUNS_MAX];
    uint32_t run_count;
    la_fixed_t fixed[LA_FIXED_MAX];
    la_swap_t swaps[LA_SWAPS_MAX];
    la_reset_t resets[LA_RESETS_MAX];
    uint32_t fixed_count, swap_count, reset_count;
    int32_t player_ai;
} la_state_t;

/* 0 when the object already keeps one and replace is not set, or there are too many. */
int la_fixed_set(la_state_t* s, uint32_t instance, const float velocity[3], float seconds, int replace);
void la_fixed_end(la_state_t* s, uint32_t instance);
/* In the order they were set. */
void la_swap_set(la_state_t* s, uint32_t instance, int material, int frame);
void la_reset_set(la_state_t* s, uint32_t trigger, int count);

size_t la_state_write(const la_state_t* s, uint8_t* out, size_t cap);
int la_state_read(la_state_t* s, const uint8_t* bytes, size_t size);

#endif
