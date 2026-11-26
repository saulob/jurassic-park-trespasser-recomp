/* The launcher, called by the host before the guest image is mapped and before the game window exists. */
#ifndef JPT_LAUNCHER_H
#define JPT_LAUNCHER_H

#include "jpt_custom_level.h"
#include "jpt_user_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JPT_LAUNCHER_PLAY = 0, JPT_LAUNCHER_QUIT = 1, JPT_LAUNCHER_FAILED = 2 } jpt_launcher_result_t;

#define JPT_USER_PATH_CAP 260

/* The user data root in use and its folders (docs/user-data.md). */
typedef struct {
    char root[JPT_USER_PATH_CAP];
    char launcher_dir[JPT_USER_PATH_CAP];
    char saves_dir[JPT_USER_PATH_CAP]; /* the game's Installed Directory */
    char cache_dir[JPT_USER_PATH_CAP];
    char settings_file[JPT_USER_PATH_CAP];
    int outcome; /* jpt::UserDataOutcome */
    int flat;    /* the earlier layout is still in use for this run */
    int warning;
    int writable; /* 0: nothing may be written; the host stops */
    char message[4 * JPT_USER_PATH_CAP];
} jpt_user_data_t;

/* Chooses and prepares the root: explicit_root (--user_data_root, else TRESPASSER_USER_DATA_ROOT) when given, else
 * <port folder>\user_data, and never anywhere else. Folders of the earlier flat layout (<port folder>\user, then
 * legacy_dir) are migrated into it first. out->writable is 0 when the root cannot be written. 0 when a path does not
 * fit. */
int jpt_user_data_select(const char* explicit_root, const char* legacy_dir, jpt_user_data_t* out);

/* PLAY's handoff, called while the launcher's window is still shown; the window closes once it returns. */
typedef struct {
    void* window;      /* the launcher's HWND on Windows; NULL elsewhere */
    int custom;
    const char* level; /* the custom level's package, or "" */
    unsigned background, text, dim, accent; /* the launcher's theme, 0xRRGGBB */
} jpt_launch_handoff_request_t;
typedef void (*jpt_launch_handoff_fn)(const jpt_launch_handoff_request_t* request);

typedef struct {
    const char* game_dir;      /* the game folder: versions\<id>, or an older installation directly in it */
    const char* user_dir;      /* the user data root */
    const char* saves_dir;     /* the English 1.0.116 saves and registry store; other versions keep theirs apart */
    const char* logs_dir;
    const char* log_file;      /* this run's log, or NULL */
    const char* settings_file; /* <user_dir>\launcher\trespasser.ini */
    int user_data_outcome;
    int force_show;            /* --launcher */
    int skip_once;             /* --skip-launcher: not shown unless the game files are not ready */
    const char* launcher_renderer; /* --launcher-renderer (jpt_launcher_renderer_valid); NULL is auto. Never saved */
    jpt_log_fn log;
    jpt_custom_level_request_t* custom_level; /* out: the custom level to start, an empty scene for the game itself */
    jpt_launch_handoff_fn handoff;            /* NULL: the launcher just closes */
} jpt_launcher_config_t;

/* Shows the launcher when it is wanted and returns once its window and every resource of it are gone. PLAY when the
 * game should start (also when the launcher was not wanted). FAILED when its window could not open: the player has been
 * told and the game must not start. */
jpt_launcher_result_t jpt_launcher_run(const jpt_launcher_config_t* config);
/* The launcher's own translation of an English text, in its language; the text itself when it has none. It outlives the call. */
const char* jpt_launcher_text(const char* english);

/* Prints what the Custom Levels import makes of an archive or a folder, as text or as the manifest it would write. Nothing is
 * installed or opened. 0 when it could be installed. data_dir: the installed game's data folder, or NULL. The paths are in the program's own
 * code page (UTF-8 on Linux). */
int jpt_analyze_custom_level(const char* path, const char* data_dir, int json);
/* Every package below a folder. report_dir may be NULL; with one, the text and the JSON are written there. */
int jpt_analyze_custom_levels(const char* root, const char* data_dir, const char* report_dir, int json);

#ifdef __cplusplus
}
#endif

#endif
