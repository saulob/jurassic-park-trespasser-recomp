/* The installed custom level packages, user_data/custom_levels/<package>, and the one a run plays. While a package is
 * active its files answer lookups in the Data Drive's data folder; everything else stays the original game's. */
#ifndef JPT_CUSTOM_LEVEL_H
#define JPT_CUSTOM_LEVEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_CUSTOM_LEVELS_DIR "custom_levels"
#define JPT_CUSTOM_LEVEL_ID_MAX 48
#define JPT_CUSTOM_LEVEL_PATH_MAX 260
#define JPT_CUSTOM_LEVEL_FILES_MAX 8192
#define JPT_CUSTOM_LEVEL_DEPTH_MAX 16
#define JPT_CUSTOM_LEVEL_SCENES_MAX 64
/* The game copies a save's scene name into a field that holds 16 characters, extension included. */
#define JPT_CUSTOM_LEVEL_SCENE_MAX 16
#define JPT_CUSTOM_LEVEL_SCENE_CAP (JPT_CUSTOM_LEVEL_SCENE_MAX + 8)

typedef struct {
    char package[JPT_CUSTOM_LEVEL_ID_MAX + 1];
    char scene[JPT_CUSTOM_LEVEL_PATH_MAX]; /* relative to the package, '/' separators; empty: no custom level */
} jpt_custom_level_request_t;

int jpt_custom_level_id_valid(const char* id);
/* The name the game loads a scene file by: its own when the game can keep it, else LEVEL<index + 1>.SCN. */
int jpt_custom_level_scene_name(const char* file_name, int index, char* out, size_t cap);
/* <saves>/custom_levels/<package>: the Installed Directory while the package is active. */
int jpt_custom_level_saves_dir(const char* saves_dir, const char* package, char* out, size_t cap);

/* 1 when the package and its scene exist; the error says why otherwise. Replaces an earlier activation. */
int jpt_custom_level_activate(const char* user_root, const jpt_custom_level_request_t* request, char* error, size_t cap);
void jpt_custom_level_deactivate(void);
int jpt_custom_level_active(void);
const char* jpt_custom_level_package(void);
const char* jpt_custom_level_scene(void);
int jpt_custom_level_scene_count(void);
const char* jpt_custom_level_scene_at(int index);

/* The file that answers a path relative to the data folder ('/' or '\\', any case); 0 leaves it to the original game.
 * A package with its own textures or animation script for a name never mixes them with the original's: the rest of
 * that set is answered by a path that does not exist. */
int jpt_custom_level_find(const char* data_relative, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
