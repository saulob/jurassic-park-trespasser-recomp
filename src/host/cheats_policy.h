/* The F10 cheats without windows or the guest: multipliers, health, camera modes, when values are in the game. */
#ifndef CHEATS_POLICY_H
#define CHEATS_POLICY_H

#include "debug_menu_policy.h"
#include "jpt_cheats.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CH_MULTIPLIER_COUNT JPT_CHEAT_JUMP_COUNT
#define CH_SPEED_COUNT JPT_CHEAT_SPEED_COUNT
#define CH_MULTIPLIER_DEFAULT JPT_CHEAT_MULTIPLIER_DEFAULT
#define CH_SUPER_HEALTH_FACTOR 4.0f
#define CH_FREE_CAMERA_SPEED 4.0f
#define CH_FREE_CAMERA_FAST 4.0f
#define CH_FREE_CAMERA_PITCH_MAX 1.5f

typedef enum { CH_CAMERA_NORMAL, CH_CAMERA_FREE } ch_camera_t;

/* CAIInfo flags bit 2: CInfluenceList::Add drops the object before it enters any brain's influences. */
#define CH_AI_INFO_IGNORE 2u

float ch_multiplier(int index);
const char* ch_multiplier_text(int index);
int ch_multiplier_clamp(int index);
/* Movement Speed has its own, longer list (to 10x); index CH_MULTIPLIER_DEFAULT is 1.0x in both. Both are the lists of
 * the launcher's Cheats page (jpt_cheats.h). */
float ch_speed(int index);
const char* ch_speed_text(int index);
int ch_speed_clamp(int index);
/* A jump reaches the height of its starting speed squared: the speed takes the square root. */
float ch_jump_speed_scale(int index);

/* The host-side cheats: what is in effect, and in which level. */
typedef struct {
    int speed, jump, super_health, stealth, noclip;
    ch_camera_t camera;
    char level[DM_LEVEL_ID_MAX];
} ch_state_t;

void ch_init(ch_state_t* s);
/* The state a session starts from or F10 left it in (jpt_cheat_effective). */
void ch_from_session(ch_state_t* s, const jpt_cheat_session_t* session);
int ch_any(const ch_state_t* s);
/* The AI info flags with the ignore bit set, and 1 when it was not set before (only then it is restored). */
int ch_ai_hide_flags(uint8_t flags, uint8_t* hidden);
/* 1 once when the level seen before was left, for another one or the main menu: the free camera goes back, the other
 * cheats stay for the session. */
int ch_level_seen(ch_state_t* s, dm_place_t place, const char* level);
void ch_set_level(ch_state_t* s, const char* level);
/* Cheat values sit in the game only in a running level with no load or save about to read it, so a save never
 * keeps them and a load never finds them. */
int ch_should_apply(dm_place_t place, int load_pending, int save_pending, int player);

float ch_super_max(float normal_max);
/* Back to the normal maximum without killing: health above it is clamped. */
void ch_super_off(float normal_max, float* hp, float* max);
/* A living player's health to its current maximum; 0 for a dead one, who stays dead. */
int ch_heal(float hp, float max, float* out);

/* "Noclip On", "Stealth Mode Off": the untranslated log line of a toggle. */
int ch_format_toggle(char* out, size_t cap, const char* name, int on);
/* "Movement Speed = 5.0x", "Invulnerability = On", "Camera = Free Camera": a cheat's name and its new value. on is a
 * toggle's state, value a multiplier index or ch_camera_t. */
int ch_format_change(char* out, size_t cap, dm_action_kind_t kind, int on, int value);
/* "movement=5.0x jump=2.0x super health=Off camera=Normal invulnerability=On infinite ammo=Off" (-1: unknown). */
int ch_format_active(char* out, size_t cap, const ch_state_t* s, int invulnerable, int ammo);
const char* ch_camera_name(ch_camera_t camera);

/* Free camera: yaw as dm_yaw, pitch up positive. */
typedef struct {
    float pos[3];
    float yaw, pitch;
} ch_freecam_t;

typedef struct {
    float rotate[2]; /* SInput v2Rotate: radians, x turns right, y looks up */
    float move[2];   /* SInput v2Move: x right, y forward */
    int up, down, fast;
} ch_freecam_input_t;

void ch_freecam_from(const dm_placement_t* at, ch_freecam_t* out);
void ch_freecam_step(ch_freecam_t* c, const ch_freecam_input_t* in, float dt);
void ch_freecam_placement(const ch_freecam_t* c, dm_placement_t* out);

#ifdef __cplusplus
}
#endif

#endif
