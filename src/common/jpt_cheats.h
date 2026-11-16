/* The cheats: one list for the launcher's Cheats page, the F10 menu and the saved settings, and the session layer F10
 * changes. Logic only: no game memory, no window. */
#ifndef JPT_CHEATS_H
#define JPT_CHEATS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In the order F10 lists them. */
typedef enum {
    JPT_CHEAT_SPEED,
    JPT_CHEAT_JUMP,
    JPT_CHEAT_INVULNERABLE,
    JPT_CHEAT_AMMO,
    JPT_CHEAT_FREEZE_AI,
    JPT_CHEAT_HEAL,
    JPT_CHEAT_SUPER_HEALTH,
    JPT_CHEAT_TELEPORT,
    JPT_CHEAT_LEVEL_SELECT,
    JPT_CHEAT_FREE_CAMERA,
    JPT_CHEAT_COUNT
} jpt_cheat_t;

/* A toggle holds 0 (Off) or 1 (On), a choice the index of its list, an action nothing. */
typedef enum { JPT_CHEAT_TOGGLE, JPT_CHEAT_CHOICE, JPT_CHEAT_ACTION } jpt_cheat_kind_t;

typedef struct {
    const char* saved; /* the text the settings file keeps */
    const char* label;
    float value;
} jpt_cheat_choice_t;

typedef struct {
    jpt_cheat_t id;
    jpt_cheat_kind_t kind;
    const char* key; /* the settings key; NULL: not saved, F10 only */
    const char* label;
    const char* help;
    int default_value;
    const jpt_cheat_choice_t* choices;
    int choice_count;
} jpt_cheat_info_t;

#define JPT_KEY_CHEAT_MOVEMENT_SPEED "cheat_movement_speed"
#define JPT_KEY_CHEAT_JUMP_HEIGHT "cheat_jump_height"
#define JPT_KEY_CHEAT_INVULNERABILITY "cheat_invulnerability"
#define JPT_KEY_CHEAT_INFINITE_AMMO "cheat_infinite_ammo"
#define JPT_KEY_CHEAT_FREEZE_DINOSAURS "cheat_freeze_dinosaurs"
#define JPT_KEY_CHEAT_SUPER_HEALTH "cheat_super_health"
#define JPT_CHEAT_PERSISTENT_COUNT 6

#define JPT_CHEAT_MULTIPLIER_DEFAULT 1 /* the index of 1.0x in both multiplier lists */
#define JPT_CHEAT_SPEED_COUNT 8
#define JPT_CHEAT_JUMP_COUNT 6

extern const jpt_cheat_info_t jpt_cheats[JPT_CHEAT_COUNT];

/* The text a saved value is written as ("1", "2.5", "0"). */
const char* jpt_cheat_saved_text(const jpt_cheat_info_t* info, int value);
/* 1 when the text names a value of the cheat ("1.0" is the 1.0x choice, "yes" is On); a bad text leaves *value. */
int jpt_cheat_parse(const jpt_cheat_info_t* info, const char* text, int* value);
int jpt_cheat_valid_value(const jpt_cheat_info_t* info, int value);
/* "1.0x", "Off", "On"; the untranslated text. */
const char* jpt_cheat_value_text(const jpt_cheat_info_t* info, int value);
/* The next value in list order; after the last one it is the first again. */
int jpt_cheat_next(const jpt_cheat_info_t* info, int value);
/* The cheats the launcher saves. */
int jpt_cheat_is_saved(const jpt_cheat_info_t* info);

/* The launcher's values for the next start, then what F10 changed over them. F10 only records differences from the
 * saved value, so the saved value is never altered and a cheat cycled back follows it again. */
typedef struct {
    int saved[JPT_CHEAT_COUNT];
    int changed[JPT_CHEAT_COUNT]; /* -1: follows the saved value */
} jpt_cheat_session_t;

/* Starts a session: the saved values (a value the list does not have is the default) and no change. */
void jpt_cheat_session_start(jpt_cheat_session_t* s, const int saved[JPT_CHEAT_COUNT]);
int jpt_cheat_effective(const jpt_cheat_session_t* s, jpt_cheat_t cheat);
/* Returns the value now in effect; a value the list does not have changes nothing. */
int jpt_cheat_set(jpt_cheat_session_t* s, jpt_cheat_t cheat, int value);
int jpt_cheat_cycle(jpt_cheat_session_t* s, jpt_cheat_t cheat);
void jpt_cheat_clear_changes(jpt_cheat_session_t* s);

/* "Movement Speed 1.0x, Jump Height 1.0x, Invulnerability Off, ...": the saved cheats, for the log. */
int jpt_cheat_format_saved(char* out, size_t cap, const jpt_cheat_session_t* s);

#ifdef __cplusplus
}
#endif

#endif
