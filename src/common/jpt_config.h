/* user_data\launcher\trespasser.ini: the port's own settings, read by the host and written by the launcher and F11.
 * `key = value` lines; lines a writer does not change (comments, other keys) are kept. */
#ifndef JPT_CONFIG_H
#define JPT_CONFIG_H

#include <stddef.h>

#include "jpt_cheats.h"
#include "jpt_game_version.h"
#include "jpt_gamepad.h"
#include "jpt_gpu.h"
#include "jpt_l10n.h"
#include "jpt_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_SETTINGS_FILE "trespasser.ini"

#define JPT_KEY_SHOW_LAUNCHER "show_launcher"
#define JPT_KEY_DISPLAY_MODE "display_mode"
#define JPT_KEY_MONITOR "monitor"
#define JPT_KEY_WINDOW_SIZE "window_size"
#define JPT_KEY_PRESENTATION "presentation"
#define JPT_KEY_RENDERER "renderer"
/* An adapter label (jpt_gpu.h); empty or not found: the first usable adapter. */
#define JPT_KEY_GRAPHICS_CARD "graphics_card"
#define JPT_KEY_RENDER_RESOLUTION "render_resolution"
#define JPT_KEY_ASPECT_RATIO "aspect_ratio"
#define JPT_KEY_TEXTURE_FILTER "texture_filter"
#define JPT_KEY_MSAA "msaa"
#define JPT_KEY_VSYNC "vsync"
#define JPT_KEY_AMBIENT_OCCLUSION "ambient_occlusion"
#define JPT_KEY_SHOW_FPS "show_fps"
/* The frame counter over the game, shown from the start; F6 toggles it for the session only. */
#define JPT_KEY_SHOW_FRAME_COUNTER "show_frame_counter"
#define JPT_KEY_SKIP_INTRO "skip_intro"
#define JPT_KEY_VIDEO_SUBTITLES "video_subtitles"
#define JPT_KEY_SHOW_RESOLUTION_TITLE "show_resolution_title"
/* A language code (jpt_l10n_languages): the game's text and subtitles where it has them, English elsewhere. Each game
 * version keeps its own choice under language_<id>; this key is the choice made before that (and --language). */
#define JPT_KEY_LANGUAGE "language"
#define JPT_KEY_LANGUAGE_PREFIX "language_"
/* The two settings it replaced: read to choose the language, then removed. */
#define JPT_KEY_LEGACY_GAME_TEXT_LANGUAGE "game_text_language"
#define JPT_KEY_LEGACY_SUBTITLE_LANGUAGE "subtitle_language"
/* Trespasser CE features reimplemented natively; Off is the retail behaviour. */
#define JPT_KEY_DUAL_STOW "dual_stow"
#define JPT_KEY_MAX_OBJECT_DETAIL "max_object_detail"
#define JPT_KEY_ACTIVE_AI "active_ai"
#define JPT_KEY_WATER_REFRACTION "water_refraction"
#define JPT_KEY_ENHANCED_SKY "enhanced_sky"
#define JPT_KEY_MINIMAP "minimap"
#define JPT_KEY_COMPASS "compass"
#define JPT_KEY_WAYPOINTS "waypoints"
#define JPT_KEY_QUICK_SAVE "quick_save_key"
#define JPT_KEY_QUICK_LOAD "quick_load_key"
/* DreamWorks Interactive's official v1.1 build 117 fixes reimplemented natively; Off is the retail 1.0 build 116. */
#define JPT_KEY_OFFICIAL_PATCH_117 "official_patch_117"
/* Degrees across the 4:3 picture, as Trespasser CE's FOV; wider shapes add view to the sides (Hor+). */
#define JPT_KEY_FOV "fov"
/* Mouse-look factor for both axes, as Trespasser CE's MouseSensX/MouseSensY. */
#define JPT_KEY_MOUSE_SENSITIVITY "mouse_sensitivity"
/* The installed game version to play (jpt_game_version.h); empty or not installed: the first installed one. */
#define JPT_KEY_GAME_VERSION "game_version"
/* The cheats the launcher saves (jpt_cheats.h): the values F10 starts each session from. */
/* Written with every change; a file without it was written by the first, experimental launcher. */
#define JPT_KEY_SETTINGS_VERSION "settings_version"
#define JPT_SETTINGS_VERSION "3"

#define JPT_FOV_DEFAULT 75
#define JPT_FOV_MIN 60
#define JPT_FOV_MAX 110
int jpt_fov_clamp(int degrees);

#define JPT_MOUSE_SENSITIVITY_DEFAULT 1.0f
#define JPT_MOUSE_SENSITIVITY_MIN 0.25f
#define JPT_MOUSE_SENSITIVITY_MAX 4.0f
#define JPT_MOUSE_SENSITIVITY_STEP 0.05f
/* Inside the range, on the step; anything not finite is the default. */
float jpt_mouse_sensitivity_clamp(float value);
/* "1.00": the text the settings file keeps. */
void jpt_mouse_sensitivity_text(float value, char* out, size_t cap);

enum { JPT_FILTER_ORIGINAL, JPT_FILTER_BILINEAR, JPT_FILTER_ANISO4, JPT_FILTER_ANISO8, JPT_FILTER_ANISO16, JPT_FILTER_COUNT };
extern const char* const jpt_filter_keys[JPT_FILTER_COUNT];

enum { JPT_ACTIVE_AI_ORIGINAL, JPT_ACTIVE_AI_ENHANCED, JPT_ACTIVE_AI_COUNT };
extern const char* const jpt_active_ai_keys[JPT_ACTIVE_AI_COUNT];
/* Active animals at once for each choice. */
extern const int jpt_active_ai_limits[JPT_ACTIVE_AI_COUNT];

enum { JPT_AO_OFF, JPT_AO_LOW, JPT_AO_MEDIUM, JPT_AO_HIGH, JPT_AO_ULTRA, JPT_AO_COUNT };
extern const char* const jpt_ao_keys[JPT_AO_COUNT];
extern const char* const jpt_ao_names[JPT_AO_COUNT];

/* The port's rebindable hotkeys. They never reach the game, so they are limited to keys the game itself leaves unused
 * and that never type text. */
enum { JPT_HOTKEY_QUICK_SAVE, JPT_HOTKEY_QUICK_LOAD, JPT_HOTKEY_COUNT };
typedef struct {
    const char* setting;
    const char* name;
    const char* default_key;
} jpt_hotkey_t;
extern const jpt_hotkey_t jpt_hotkeys[JPT_HOTKEY_COUNT];

typedef struct {
    const char* key;
    uint32_t vk;
} jpt_hotkey_choice_t;
#define JPT_HOTKEY_CHOICE_COUNT 10
extern const jpt_hotkey_choice_t jpt_hotkey_choices[JPT_HOTKEY_CHOICE_COUNT];
/* 0 when the name is not one of the choices. */
uint32_t jpt_hotkey_vk(const char* key);

typedef struct {
    int show_launcher;
    int borderless;
    int monitor;              /* 1-based, primary first */
    int window_w, window_h;   /* 0 x 0: auto */
    int stretch;
    int renderer;             /* jpt_renderer_t */
    int renderer_from_ini;
    int hardware;
    char graphics_card[JPT_GPU_LABEL_MAX];
    int render_preset;        /* jpt_render_presets index, -1 with render_w x render_h */
    int render_w, render_h;
    int aspect;               /* jpt_aspects index */
    int texture_filter;
    unsigned msaa;            /* 1, 2, 4 or 8 */
    int vsync;
    int ambient_occlusion;    /* JPT_AO_* */
    int show_fps;
    int show_frame_counter;
    int show_resolution_title;
    int skip_intro;
    int video_subtitles;
    int dual_stow;
    int max_object_detail;
    int active_ai_enhanced; /* 0: the retail 3 active animals, 1: 6 */
    int water_refraction;
    int enhanced_sky;
    int minimap;
    int compass;
    int waypoints;
    int official_patch_117;
    int fov;
    float mouse_sensitivity;
    int cheat[JPT_CHEAT_COUNT]; /* a toggle's 0 or 1, a choice's index; only the saved cheats are read */
    char language[JPT_L10N_CODE_MAX];
    char version_language[JPT_GAME_VERSION_COUNT][JPT_L10N_CODE_MAX]; /* "" until the version's own choice is stored */
    char earlier_language[JPT_L10N_CODE_MAX];                           /* "" without a stored language_ before versions */
    int language_forced;                                                /* --language: every version plays in it */
    char game_version[JPT_GAME_VERSION_ID_MAX];
    uint32_t hotkey_vk[JPT_HOTKEY_COUNT];
    jpt_gamepad_settings_t gamepad;
} jpt_config_t;

typedef struct {
    const char* key;
    const char* default_value;
} jpt_setting_t;

#define JPT_SETTING_COUNT (33 + JPT_CHEAT_PERSISTENT_COUNT + JPT_GAMEPAD_SETTING_COUNT)
extern const jpt_setting_t jpt_settings[JPT_SETTING_COUNT];

void jpt_config_defaults(jpt_config_t* c);
/* 1 when the value is valid for the key and was applied; otherwise the config keeps what it had. */
int jpt_config_apply(jpt_config_t* c, const char* key, const char* value);
int jpt_config_valid(const char* key, const char* value);
/* Defaults, then every valid line of the file. 1 when the file was read. A file with the two earlier language
 * settings and no language gets the language they gave. */
int jpt_config_load(const char* path, jpt_config_t* c);
const char* jpt_default_value(const char* key);

/* The key that stores the language chosen for a game version: language_<id>. 0 when it does not fit. */
int jpt_language_key(const jpt_game_version_t* v, char* out, size_t cap);
/* The language a version plays in: its own stored choice, else (English 1.0.116 only) the choice stored before choices
 * were kept per version, else the language of the release itself. Either argument may be NULL or "". */
const char* jpt_language_for(const jpt_game_version_t* v, const char* chosen, const char* earlier);
/* Sets c->language for the version about to run (unchanged under --language). */
void jpt_config_select_language(jpt_config_t* c, const jpt_game_version_t* v);

/* One `key = value` line (also `# key = value` with allow_comment). The value stops at ` #`. */
int jpt_ini_parse_line(const char* line, int allow_comment, char* key, size_t key_cap, char* value, size_t value_cap);
/* The value of the key in the file, else 0. */
int jpt_ini_get(const char* path, const char* key, char* value, size_t cap);
/* Sets the keys in the file (replacing their lines, else a commented example, else appending; a NULL value removes the
 * key) and keeps every other line; settings_version is written too. The file is replaced in one step. 1 on success. */
int jpt_ini_set(const char* path, const char* const* keys, const char* const* values, int count);

enum {
    JPT_MIGRATED_NOTHING,
    JPT_MIGRATED_EXPERIMENTAL,
    JPT_MIGRATED_GRAPHICS_DEFAULTS,
    JPT_MIGRATED_VERSION_ONLY,
};
/* Runs once per file. No settings_version: the experimental launcher's display and graphics keys are removed, as its
 * choices cannot be told apart from its defaults. Version 2: texture_filter original, msaa 0/1 and vsync off were
 * that launcher's defaults, so those values are removed and the current defaults apply; other values are kept. */
int jpt_config_migrate(const char* path);
/* Writes the language the two earlier language settings gave and removes them; 1 when the file was changed. */
int jpt_config_migrate_language(const char* path);

int jpt_parse_bool(const char* text, int* out);

#ifdef __cplusplus
}
#endif

#endif
