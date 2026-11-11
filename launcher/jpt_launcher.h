/* The launcher, called by the host before the guest image is mapped and before the game window exists. */
#ifndef JPT_LAUNCHER_H
#define JPT_LAUNCHER_H

#include "jpt_user_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JPT_LAUNCHER_PLAY = 0, JPT_LAUNCHER_QUIT = 1 } jpt_launcher_result_t;

typedef struct {
    const char* game_dir;      /* the installed game (trespass.exe, its DLLs, data\) */
    const char* user_dir;      /* settings, the game's registry store, saves */
    const char* logs_dir;
    const char* settings_file; /* <user_dir>\trespasser.ini */
    int force_show;            /* --launcher */
    int skip_once;             /* --skip-launcher: not shown unless the game files are not ready */
    jpt_log_fn log;
} jpt_launcher_config_t;

/* Shows the launcher when it is wanted and returns once its window and every resource of it are gone. PLAY when the
 * game should start (also when the launcher was not wanted, or its window could not open with valid files). */
jpt_launcher_result_t jpt_launcher_run(const jpt_launcher_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
