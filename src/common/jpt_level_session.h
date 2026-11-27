/* What a run plays and why each level loads: a [session] block when the game starts, then [level] and [restart] lines at
 * every load. Lifecycle events only, never per frame. The world generation counts the worlds built from nothing that the
 * host knows of: the first level, and every purge. */
#ifndef JPT_LEVEL_SESSION_H
#define JPT_LEVEL_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JPT_SESSION_NONE, JPT_SESSION_CAMPAIGN, JPT_SESSION_CUSTOM } jpt_session_kind_t;
typedef enum { JPT_SOURCE_RETAIL, JPT_SOURCE_PACKAGE } jpt_level_source_t;
typedef enum { JPT_REASON_NEW_GAME, JPT_REASON_INITIAL, JPT_REASON_TRANSITION, JPT_REASON_RESTART, JPT_REASON_LOAD_SAVE } jpt_level_reason_t;
/* The campaign keeps the game's own Restart Level, which reads the scene over the world it has; a custom session purges the
 * world and loads the level whole, as when it entered it. */
typedef enum { JPT_RESTART_SCENE_RELOAD, JPT_RESTART_FULL } jpt_restart_mode_t;

typedef struct {
    jpt_session_kind_t kind;
    const char* build;
    const char* renderer;
    const char* language;
    const char* package;      /* custom only */
    const char* layout;       /* custom only */
    const char* start_level;  /* custom only */
    const char* capabilities; /* custom only: names joined by ',' */
    const char* compatibility; /* custom only: "ready", "limited" */
    int experimental;          /* custom only: started by Play Anyway */
} jpt_session_info_t;

typedef void (*jpt_session_log_fn)(const char* tag, const char* text);

void jpt_session_set_log(jpt_session_log_fn log);
void jpt_session_begin(const jpt_session_info_t* info);
void jpt_session_end(void);
jpt_session_kind_t jpt_session_kind(void);

const char* jpt_session_reason_name(jpt_level_reason_t reason);
/* The file name of a scene the game names by its path. */
const char* jpt_session_scene_name(const char* path);
jpt_level_reason_t jpt_session_reason(int save_file, int from_main_menu);

void jpt_session_level_begin(jpt_level_reason_t reason, const char* scene, jpt_level_source_t source);
void jpt_session_level_end(int ok, int code);
void jpt_session_world_purged(void);

jpt_restart_mode_t jpt_session_restart_mode(void);
jpt_restart_mode_t jpt_session_restart_begin(const char* scene, jpt_level_source_t source);
/* 1 inside a full restart until its world was purged. */
int jpt_session_restart_purges(void);
int jpt_session_restarting(void);
void jpt_session_restart_end(int ok);

unsigned jpt_session_world_generation(void);
unsigned jpt_session_levels(void);
const char* jpt_session_current_scene(void);
jpt_level_source_t jpt_session_current_source(void);

#ifdef __cplusplus
}
#endif

#endif
