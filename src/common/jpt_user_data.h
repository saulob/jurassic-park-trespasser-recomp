/* The player's files of an older layout, kept in the installed game folder, and folder creation. The user data root
 * itself is chosen in launcher/user_data.cpp. */
#ifndef JPT_USER_DATA_H
#define JPT_USER_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_REGISTRY_FILE "recomp-registry.txt"

typedef void (*jpt_log_fn)(const char* line);

/* Creates the folder and its parents. 1 when it exists afterwards. */
int jpt_make_dirs(const char* path);

/* Copies the registry store and the saves (SAVEGAME.*) from an old game folder into user_dir when user_dir has no file
 * of that name yet; nothing is removed from `from`. Returns the number of files copied, -1 when a copy failed. */
int jpt_carry_user_files(const char* from, const char* user_dir, jpt_log_fn log);

/* 1 for a file name that belongs to the player rather than to the installed game. */
int jpt_is_user_file(const char* name);

#ifdef __cplusplus
}
#endif

#endif
