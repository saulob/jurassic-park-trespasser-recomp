/* The installed custom level packages, user_data/custom_levels/<package>, and the one a run plays. While a package is
 * active its files answer lookups in the Data Drive's data folder; everything else stays the original game's.
 *
 * What Trespasser CE and ATX add to the original game exists here and nowhere else, as capabilities of the running session:
 * each one is asked for by what the package holds, checked where it takes effect, and gone when the session ends. Without a
 * custom level there is none. A session is custom from its start to its end, whatever files it loads: a level of the original
 * game that the package leads to is played in it, and the original game's own session never sees any of this. */
#ifndef JPT_CUSTOM_LEVEL_H
#define JPT_CUSTOM_LEVEL_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_scene.h"

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
/* What the launcher generates in a package's folder: the map the game reads it through and the pictures made for it. */
#define JPT_CUSTOM_LEVEL_RUNTIME_MAP "runtime.map"
#define JPT_CUSTOM_LEVEL_RUNTIME_VERSION 1
#define JPT_CUSTOM_LEVEL_GENERATED_DIR ".runtime"

typedef enum { JPT_LEVEL_TPA_STREAM, JPT_LEVEL_TPA_EFFECTS, JPT_LEVEL_TPA_AMBIENT, JPT_LEVEL_TPA_KINDS } jpt_level_tpa_t;

typedef struct {
    char package[JPT_CUSTOM_LEVEL_ID_MAX + 1];
    char scene[JPT_CUSTOM_LEVEL_PATH_MAX]; /* relative to the package, '/' separators; empty: no custom level */
    char layout[24];                       /* the launcher's analysis: loose, fan_mission, ...; may be empty */
} jpt_custom_level_request_t;

typedef enum {
    JPT_LEVEL_CAP_SCENE_FORMAT, /* a scene of version 23 to 26 */
    JPT_LEVEL_CAP_SCRIPT_STATE, /* of version 26: the states of object scripts are skipped */
    JPT_LEVEL_CAP_TEXHW,
    JPT_LEVEL_CAP_SOUND_ADDED,
    JPT_LEVEL_CAP_SOUND_REPLACED,
    JPT_LEVEL_CAP_LOADING_IMAGE,
    JPT_LEVEL_CAP_SKIP_ENDING,
    JPT_LEVEL_CAP_DUAL_STOW,
    JPT_LEVEL_CAP_WATER_REFRACTION,
    JPT_LEVEL_CAP_ENHANCED_SKY,
    JPT_LEVEL_CAP_ACTIVE_AI,
    JPT_LEVEL_CAP_MESH_FACES,
    JPT_LEVEL_CAP_MESH_HEAP,
    JPT_LEVEL_CAP_TERRAIN_NODES,
    JPT_LEVEL_CAP_TEXTURE_POOL,
    JPT_LEVEL_CAP_TERRAIN_CULL,
    JPT_LEVEL_CAP_FULL_RESTART,
    JPT_LEVEL_CAP_SKY,
    JPT_LEVEL_CAPS
} jpt_level_cap_t;

const char* jpt_custom_level_cap_name(jpt_level_cap_t cap);
/* Whether a custom level is running and its package needs the capability: the only way to one. */
int jpt_custom_level_requires(jpt_level_cap_t cap);
/* The same for an ActionType of ATX or Trespasser CE (99 to 121). */
int jpt_custom_level_action(int type);
/* One bit per capability; 0 without a session. */
unsigned jpt_custom_level_caps(void);
/* Called once when the session ends, before another one or none; a function that is there already is not added again. */
typedef void (*jpt_custom_level_end_fn)(void);
void jpt_custom_level_on_end(jpt_custom_level_end_fn ended);

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
const char* jpt_custom_level_layout(void);
const char* jpt_custom_level_scene(void);
int jpt_custom_level_scene_count(void);
const char* jpt_custom_level_scene_at(int index);
/* Its file, relative to the package. */
const char* jpt_custom_level_scene_file_at(int index);
/* The package's Trespasser CE scripts in load order, and the script classes its worlds name (runtime.map); NULL or 0 past the
 * last one. */
const char* jpt_custom_level_script_file(int index);
int jpt_custom_level_script_reference(int index, const char** scene, const char** class_name, const char** object, int* action);
/* The script files a level's VM loads, in load order, as the launcher found them: a level without any reads none. */
int jpt_custom_level_script_use(int index, const char** scene, const char** file);
/* The scene being loaded or played, a file of the package; "" for a level of the original game. */
const char* jpt_custom_level_current_scene(void);
const char* jpt_custom_level_dir(void);

/* The level the game starts to load, by its file or path: loading pictures and sound files of one level follow it. */
void jpt_custom_level_scene_loading(const char* scene);
/* Restart Level reads the scene the game names again, without a loading screen: the session follows it as a level load. The
 * package's file of that scene, or 0 when the game reads its own. */
int jpt_custom_level_restart(const char* scene, char* out, size_t cap);
/* The index-th sound file the package adds to the game's Stream, Effects or Ambient.tpa in the current level; 0 past the
 * last one. */
int jpt_custom_level_tpa(jpt_level_tpa_t kind, int index, char* out, size_t cap);
/* The package's settings for this session, never saved: 0 past the last one. */
int jpt_custom_level_setting(int index, const char** key, const char** value);
int jpt_custom_level_skips_ending(void);
/* The format of the package's scene that is being read, when it is not the game's own; 0 otherwise. */
int jpt_custom_level_scene_version(void);
/* A picture of the package's MAPS folder. */
int jpt_custom_level_map(const char* name, char* out, size_t cap);
/* The package path of a picture that ATX reads as MAPS\<name>: "maps/..." with '/' separators. 0 when the name leaves the folder. */
int jpt_custom_level_map_path(const char* name, char* out, size_t cap);
/* The package's menu\hint_strings.txt. */
int jpt_custom_level_hint_strings(char* out, size_t cap);
/* ActionType 105: a video of the package, by its name below the data folder, answers the game's lookups until NULL; 0 when it has
 * none of that name. */
int jpt_custom_level_play_video(const char* data_relative);
/* An engine limit the package asks for ("mesh_faces", "mesh_heap_mb", "terrain_nodes", "texture_pool_mb", "terrain_cull_mm"); 0 when it does not. */
unsigned jpt_custom_level_limit(const char* name);

/* A scene of the package that the game's own loader does not read, as one it reads (from malloc); NULL when it stays as it is.
 * host: the file, which must be the package's. Only a problem in the report refuses it. */
uint8_t* jpt_custom_level_adapt_scene(const char* host, const uint8_t* bytes, size_t size, size_t* out_size, jpt_scene_report_t* report);

/* The file that answers a path relative to the data folder ('/' or '\\', any case); 0 leaves it to the original game.
 * A package with its own textures or animation script for a name never mixes them with the original's: the rest of
 * that set is answered by a path that does not exist. */
int jpt_custom_level_find(const char* data_relative, char* out, size_t cap);
/* The same for a path relative to the game folder itself ("Map\\as2\\x.bmp"). */
int jpt_custom_level_find_in_game(const char* game_relative, char* out, size_t cap);
/* The same for a path the game gives without a drive. Its current directory is the Installed Directory, so "Map\\as2\\x.bmp"
 * is outside the game folder; "\\as2\\x.bmp" and "x.bmp" are the texture loader's next attempts. */
int jpt_custom_level_find_relative(const char* path, char* out, size_t cap);
/* The installed game's own file for a path relative to it or to its data folder: the root behind every package's. */
typedef int (*jpt_custom_level_game_fn)(const char* relative, char* out, size_t cap);
void jpt_custom_level_set_game(jpt_custom_level_game_fn find);
int jpt_custom_level_game_file(const char* relative, char* out, size_t cap);
/* Whether the installed game's files can be told: a finder was set. */
int jpt_custom_level_has_game(void);

/* A picture asked for without a drive, whatever comes of it. */
int jpt_custom_level_is_texture_request(const char* path);
typedef enum { JPT_TEXTURE_ABSENT, JPT_TEXTURE_UNUSABLE, JPT_TEXTURE_FOUND } jpt_texture_result_t;
typedef struct {
    int in_game; /* the installed game's root, else the package's */
    char relative[JPT_CUSTOM_LEVEL_PATH_MAX + 16];
    char file[1024]; /* empty when absent */
    jpt_texture_result_t result;
} jpt_texture_try_t;
/* The places a texture is looked for, as Trespasser CE orders them: texhw\<path>, texhw\Map\<file>, then <path>, each in the
 * package and then in the game. 0 past the last one. The first that is found answers. */
int jpt_custom_level_texture_try(const char* path, int index, jpt_texture_try_t* out);
/* Trespasser CE's hardware override of a texture: texhw\<path>, then texhw\Map\<file>, each as .dds, .tga and .bmp and those in the
 * package and then in the game. suffix stands before the type (".norm"). */
int jpt_custom_level_texhw(const char* texture, const char* suffix, char* out, size_t cap);
/* A picture of that name somewhere in the package, in any format and folder. */
int jpt_custom_level_texture_other(const char* path, char* out, size_t cap);
/* Changes with every level the session starts to load. */
unsigned jpt_custom_level_serial(void);
/* The custom sky of the level that is loaded, <scene>.sky, when the package needs skies. */
int jpt_custom_level_sky(char* out, size_t cap);
/* A picture a sky names ("data\\texhw\\backdrop\\sun"): .dds, .tga, then .bmp, of the package and then of the game. */
int jpt_custom_level_sky_texture(const char* name, char* out, size_t cap);
/* Trespasser CE's picture for a texture that is nowhere, as a bitmap in memory (from malloc): 4 x 4 of one grey. A height map is
 * read by its texture's size, so the flat one that stands for it is as large as a texture can be. */
uint8_t* jpt_custom_level_missing_texture(int height_map, size_t* size);
/* What the texture loader takes: 8 bits a pixel, uncompressed, whole. head: the file's first 54 bytes. */
int jpt_custom_level_texture_ok(const void* head, size_t head_size, uint64_t file_size);

#ifdef __cplusplus
}
#endif

#endif
