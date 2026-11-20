/* Where the Linux executable reads the game and writes everything else. The layout is the Windows one (docs/user-data.md):
 * user_data and logs next to the program, game data from the folder the player installed it to. A caller that needs the
 * data elsewhere (a read-only mount) passes the folders explicitly; nothing falls back to another place on its own. */
#ifndef JPT_LINUX_PATHS_H
#define JPT_LINUX_PATHS_H

#include <stddef.h>

#define LX_PATH_MAX 1024

typedef struct {
    char port_dir[LX_PATH_MAX];     /* the folder of the executable */
    char game_dir[LX_PATH_MAX];     /* the installed game: read only */
    char user_root[LX_PATH_MAX];    /* user_data */
    char launcher_dir[LX_PATH_MAX]; /* user_data/launcher: trespasser.ini */
    char saves_dir[LX_PATH_MAX];    /* user_data/saves: SAVEGAME.NNN, recomp-registry.txt, swap files */
    char cache_dir[LX_PATH_MAX];    /* user_data/cache */
    char logs_dir[LX_PATH_MAX];
} lx_paths_t;

/* Chooses the folders. exe_path is the executable (from /proc/self/exe). Arguments override the environment
 * (TRESPASSER_GAME_DIR, TRESPASSER_USER_DATA_ROOT, TRESPASSER_LOG_DIR), which overrides the defaults next to the program.
 * Returns 0 and fills err when a path is too long. */
int lx_paths_resolve(lx_paths_t* p, const char* exe_path, const char* game_arg, const char* user_root_arg,
                     const char* log_dir_arg, char* err, size_t err_len);

/* Creates the user folders and the log folder and proves each can be written. A folder that cannot is an error; the
 * caller does not choose another one. */
int lx_paths_prepare(const lx_paths_t* p, char* err, size_t err_len);

#endif
