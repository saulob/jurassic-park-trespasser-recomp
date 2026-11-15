/* user_data\launcher\trespasser.ini: the port's own settings, read by the host and written by the launcher and F11.
 * `key = value` lines; lines a writer does not change (comments, other keys) are kept. */
#ifndef JPT_CONFIG_H
#define JPT_CONFIG_H

#include <stddef.h>

#include "jpt_gamepad.h"

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
#define JPT_KEY_RENDER_RESOLUTION "render_resolution"
#define JPT_KEY_ASPECT_RATIO "aspect_ratio"
#define JPT_KEY_TEXTURE_FILTER "texture_filter"
#define JPT_KEY_MSAA "msaa"
#define JPT_KEY_VSYNC "vsync"
#define JPT_KEY_AMBIENT_OCCLUSION "ambient_occlusion"
#define JPT_KEY_SHOW_FPS "show_fps"
#define JPT_KEY_SKIP_INTRO "skip_intro"
/* Written with every change; a file without it was written by the first, experimental launcher. */
#define JPT_KEY_SETTINGS_VERSION "settings_version"
#define JPT_SETTINGS_VERSION "3"

enum { JPT_FILTER_ORIGINAL, JPT_FILTER_BILINEAR, JPT_FILTER_ANISO4, JPT_FILTER_ANISO8, JPT_FILTER_ANISO16, JPT_FILTER_COUNT };
extern const char* const jpt_filter_keys[JPT_FILTER_COUNT];

enum { JPT_AO_OFF, JPT_AO_LOW, JPT_AO_MEDIUM, JPT_AO_HIGH, JPT_AO_ULTRA, JPT_AO_COUNT };
extern const char* const jpt_ao_keys[JPT_AO_COUNT];
extern const char* const jpt_ao_names[JPT_AO_COUNT];

typedef struct {
    int show_launcher;
    int borderless;
    int monitor;              /* 1-based, primary first */
    int window_w, window_h;   /* 0 x 0: auto */
    int stretch;
    int hardware;
    int render_preset;        /* jpt_render_presets index, -1 with render_w x render_h */
    int render_w, render_h;
    int aspect;               /* jpt_aspects index */
    int texture_filter;
    unsigned msaa;            /* 1, 2, 4 or 8 */
    int vsync;
    int ambient_occlusion;    /* JPT_AO_* */
    int show_fps;
    int skip_intro;
    jpt_gamepad_settings_t gamepad;
} jpt_config_t;

typedef struct {
    const char* key;
    const char* default_value;
} jpt_setting_t;

#define JPT_SETTING_COUNT (14 + JPT_GAMEPAD_SETTING_COUNT)
extern const jpt_setting_t jpt_settings[JPT_SETTING_COUNT];

void jpt_config_defaults(jpt_config_t* c);
/* 1 when the value is valid for the key and was applied; otherwise the config keeps what it had. */
int jpt_config_apply(jpt_config_t* c, const char* key, const char* value);
int jpt_config_valid(const char* key, const char* value);
/* Defaults, then every valid line of the file. 1 when the file was read. */
int jpt_config_load(const char* path, jpt_config_t* c);
const char* jpt_default_value(const char* key);

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

int jpt_parse_bool(const char* text, int* out);

#ifdef __cplusplus
}
#endif

#endif
