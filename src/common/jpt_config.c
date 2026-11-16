#define WIN32_LEAN_AND_MEAN
#include "jpt_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "jpt_display.h"

#define MAX_LINE 1024
#define APPEND_HEADER "# Set by Trespasser Recompiled"

const char* const jpt_filter_keys[JPT_FILTER_COUNT] = { "original", "bilinear", "anisotropic4", "anisotropic8",
                                                        "anisotropic16" };
const char* const jpt_active_ai_keys[JPT_ACTIVE_AI_COUNT] = { "original", "enhanced" };
const int jpt_active_ai_limits[JPT_ACTIVE_AI_COUNT] = { 3, 6 };
const char* const jpt_ao_keys[JPT_AO_COUNT] = { "off", "low", "medium", "high", "ultra" };
const char* const jpt_ao_names[JPT_AO_COUNT] = { "Off", "Low", "Medium", "High", "Ultra" };

const jpt_hotkey_t jpt_hotkeys[JPT_HOTKEY_COUNT] = {
    { JPT_KEY_QUICK_SAVE, "Quick Save", "F7" },
    { JPT_KEY_QUICK_LOAD, "Quick Load", "F8" },
};

const jpt_hotkey_choice_t jpt_hotkey_choices[JPT_HOTKEY_CHOICE_COUNT] = {
    { "F6", 0x75 },     { "F7", 0x76 },  { "F8", 0x77 },   { "F9", 0x78 },     { "Insert", 0x2D },
    { "Delete", 0x2E }, { "Home", 0x24 }, { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
};

uint32_t jpt_hotkey_vk(const char* key) {
    for (int i = 0; key && i < JPT_HOTKEY_CHOICE_COUNT; i++)
        if (!_stricmp(key, jpt_hotkey_choices[i].key)) return jpt_hotkey_choices[i].vk;
    return 0;
}

const jpt_setting_t jpt_settings[JPT_SETTING_COUNT] = {
    { JPT_KEY_SHOW_LAUNCHER, "1" },
    { JPT_KEY_DISPLAY_MODE, "windowed" },
    { JPT_KEY_MONITOR, "1" },
    { JPT_KEY_WINDOW_SIZE, "auto" },
    { JPT_KEY_PRESENTATION, "letterbox" },
    { JPT_KEY_RENDERER, "d3d11" },
    { JPT_KEY_GRAPHICS_CARD, "" },
    { JPT_KEY_RENDER_RESOLUTION, "native" },
    { JPT_KEY_ASPECT_RATIO, "16:9" },
    { JPT_KEY_TEXTURE_FILTER, "anisotropic16" },
    { JPT_KEY_MSAA, "4" },
    { JPT_KEY_VSYNC, "1" },
    { JPT_KEY_AMBIENT_OCCLUSION, "medium" },
    { JPT_KEY_SHOW_FPS, "1" },
    { JPT_KEY_SKIP_INTRO, "0" },
    { JPT_KEY_VIDEO_SUBTITLES, "1" },
    { JPT_KEY_SHOW_RESOLUTION_TITLE, "0" },
    { JPT_KEY_LANGUAGE, JPT_L10N_ENGLISH },
    { JPT_KEY_DUAL_STOW, "1" },
    { JPT_KEY_MAX_OBJECT_DETAIL, "1" },
    { JPT_KEY_ACTIVE_AI, "enhanced" },
    { JPT_KEY_WATER_REFRACTION, "1" },
    { JPT_KEY_ENHANCED_SKY, "1" },
    { JPT_KEY_QUICK_SAVE, "F7" },
    { JPT_KEY_QUICK_LOAD, "F8" },
    { JPT_KEY_OFFICIAL_PATCH_117, "1" },
    { JPT_KEY_FOV, "75" },
    { JPT_KEY_MOUSE_SENSITIVITY, "1.00" },
    { JPT_KEY_GAMEPAD_GLYPHS, "automatic" },
    { JPT_KEY_GAMEPAD_LOOK_X, "100" },
    { JPT_KEY_GAMEPAD_LOOK_Y, "100" },
    { JPT_KEY_GAMEPAD_INVERT_Y, "0" },
    { JPT_KEY_GAMEPAD_STICK_DEADZONE, "15" },
    { JPT_KEY_GAMEPAD_TRIGGER_DEADZONE, "12" },
    { "gamepad_bind_jump", "south" },
    { "gamepad_bind_crouch", "east" },
    { "gamepad_bind_crouch_toggle", "leftstick" },
    { "gamepad_bind_use", "righttrigger" },
    { "gamepad_bind_hand", "lefttrigger" },
    { "gamepad_bind_grab", "west" },
    { "gamepad_bind_throw", "north" },
    { "gamepad_bind_stow", "dpup" },
    { "gamepad_bind_wrist", "leftshoulder" },
    { "gamepad_bind_arm", "rightshoulder" },
};

const char* jpt_default_value(const char* key) {
    for (int i = 0; i < JPT_SETTING_COUNT; i++)
        if (!strcmp(key, jpt_settings[i].key)) return jpt_settings[i].default_value;
    return NULL;
}

int jpt_parse_bool(const char* text, int* out) {
    static const char* const on[] = { "1", "on", "yes", "true" };
    static const char* const off[] = { "0", "off", "no", "false" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(text, on[i])) return *out = 1, 1;
        if (!_stricmp(text, off[i])) return *out = 0, 1;
    }
    return 0;
}

int jpt_fov_clamp(int degrees) {
    return degrees < JPT_FOV_MIN ? JPT_FOV_MIN : degrees > JPT_FOV_MAX ? JPT_FOV_MAX : degrees;
}

float jpt_mouse_sensitivity_clamp(float value) {
    float steps;
    if (!isfinite(value)) return JPT_MOUSE_SENSITIVITY_DEFAULT;
    if (value < JPT_MOUSE_SENSITIVITY_MIN) value = JPT_MOUSE_SENSITIVITY_MIN;
    if (value > JPT_MOUSE_SENSITIVITY_MAX) value = JPT_MOUSE_SENSITIVITY_MAX;
    steps = floorf(value / JPT_MOUSE_SENSITIVITY_STEP + 0.5f);
    return steps * JPT_MOUSE_SENSITIVITY_STEP;
}

void jpt_mouse_sensitivity_text(float value, char* out, size_t cap) {
    if (cap) _snprintf(out, cap - 1, "%.2f", jpt_mouse_sensitivity_clamp(value)), out[cap - 1] = 0;
}

static int parse_number(const char* text, double* out) {
    char* end;
    double v = strtod(text, &end);
    if (end == text || *end || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

static int parse_choice(const char* text, const char* const* names, int count) {
    for (int i = 0; i < count; i++)
        if (!_stricmp(text, names[i])) return i;
    return -1;
}

static int apply(jpt_config_t* c, const char* key, const char* value) {
    static const char* const modes[] = { "windowed", "borderless" };
    static const char* const presentations[] = { "letterbox", "stretch" };
    static const char* const renderers[] = { "software", "d3d11" };
    int n, w, h;
    char* end;
    if (jpt_gamepad_is_key(key)) return jpt_gamepad_apply(&c->gamepad, key, value);
    for (n = 0; n < JPT_HOTKEY_COUNT; n++)
        if (!strcmp(key, jpt_hotkeys[n].setting)) {
            uint32_t vk = jpt_hotkey_vk(value);
            return vk ? (c->hotkey_vk[n] = vk, 1) : 0;
        }
    if (!strcmp(key, JPT_KEY_SHOW_LAUNCHER)) return jpt_parse_bool(value, &c->show_launcher);
    if (!strcmp(key, JPT_KEY_VSYNC)) return jpt_parse_bool(value, &c->vsync);
    if (!strcmp(key, JPT_KEY_SHOW_FPS)) return jpt_parse_bool(value, &c->show_fps);
    if (!strcmp(key, JPT_KEY_SKIP_INTRO)) return jpt_parse_bool(value, &c->skip_intro);
    if (!strcmp(key, JPT_KEY_VIDEO_SUBTITLES)) return jpt_parse_bool(value, &c->video_subtitles);
    if (!strcmp(key, JPT_KEY_SHOW_RESOLUTION_TITLE)) return jpt_parse_bool(value, &c->show_resolution_title);
    if (!strcmp(key, JPT_KEY_DUAL_STOW)) return jpt_parse_bool(value, &c->dual_stow);
    if (!strcmp(key, JPT_KEY_MAX_OBJECT_DETAIL)) return jpt_parse_bool(value, &c->max_object_detail);
    if (!strcmp(key, JPT_KEY_WATER_REFRACTION)) return jpt_parse_bool(value, &c->water_refraction);
    if (!strcmp(key, JPT_KEY_ENHANCED_SKY)) return jpt_parse_bool(value, &c->enhanced_sky);
    if (!strcmp(key, JPT_KEY_OFFICIAL_PATCH_117)) return jpt_parse_bool(value, &c->official_patch_117);
    if (!strcmp(key, JPT_KEY_ACTIVE_AI))
        return (n = parse_choice(value, jpt_active_ai_keys, JPT_ACTIVE_AI_COUNT)) >= 0 ? (c->active_ai_enhanced = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_DISPLAY_MODE)) return (n = parse_choice(value, modes, 2)) >= 0 ? (c->borderless = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_PRESENTATION))
        return (n = parse_choice(value, presentations, 2)) >= 0 ? (c->stretch = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_RENDERER)) return (n = parse_choice(value, renderers, 2)) >= 0 ? (c->hardware = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_TEXTURE_FILTER))
        return (n = parse_choice(value, jpt_filter_keys, JPT_FILTER_COUNT)) >= 0 ? (c->texture_filter = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_AMBIENT_OCCLUSION))
        return (n = parse_choice(value, jpt_ao_keys, JPT_AO_COUNT)) >= 0 ? (c->ambient_occlusion = n, 1) : 0;
    if (!strcmp(key, JPT_KEY_GRAPHICS_CARD)) {
        if (strlen(value) >= sizeof c->graphics_card) return 0;
        strcpy(c->graphics_card, value);
        return 1;
    }
    if (!strcmp(key, JPT_KEY_LANGUAGE)) {
        if (!jpt_l10n_language(value)) return 0;
        strcpy(c->language, value);
        return 1;
    }
    if (!strcmp(key, JPT_KEY_FOV)) {
        double v;
        if (!parse_number(value, &v)) return 0;
        c->fov = jpt_fov_clamp(v > 1000.0 ? 1000 : v < -1000.0 ? -1000 : (int)floor(v + 0.5));
        return 1;
    }
    if (!strcmp(key, JPT_KEY_MOUSE_SENSITIVITY)) {
        double v;
        if (!parse_number(value, &v)) return 0;
        c->mouse_sensitivity = jpt_mouse_sensitivity_clamp((float)(v > 1000.0 ? 1000.0 : v));
        return 1;
    }
    if (!strcmp(key, JPT_KEY_MONITOR)) {
        long v = strtol(value, &end, 10);
        if (*end || v < 1 || v > 16) return 0;
        c->monitor = (int)v;
        return 1;
    }
    if (!strcmp(key, JPT_KEY_MSAA)) {
        long v = strtol(value, &end, 10);
        if (*end || (v != 0 && v != 1 && v != 2 && v != 4 && v != 8)) return 0;
        c->msaa = v ? (unsigned)v : 1;
        return 1;
    }
    if (!strcmp(key, JPT_KEY_WINDOW_SIZE)) {
        if (!_stricmp(value, "auto")) return c->window_w = c->window_h = 0, 1;
        if (!jpt_parse_size(value, &w, &h)) return 0;
        c->window_w = w;
        c->window_h = h;
        return 1;
    }
    if (!strcmp(key, JPT_KEY_RENDER_RESOLUTION)) {
        if ((n = jpt_render_preset_index(value)) >= 0) {
            c->render_preset = n;
            c->render_w = c->render_h = 0;
            return 1;
        }
        if (!jpt_parse_size(value, &w, &h)) return 0;
        c->render_preset = -1;
        c->render_w = w;
        c->render_h = h;
        return 1;
    }
    if (!strcmp(key, JPT_KEY_ASPECT_RATIO)) {
        for (n = 0; n < JPT_ASPECT_COUNT; n++)
            if (!strcmp(value, jpt_aspects[n].key)) return c->aspect = n, 1;
        return 0;
    }
    return 0;
}

int jpt_config_apply(jpt_config_t* c, const char* key, const char* value) {
    jpt_config_t next = *c;
    if (!key || !value || !apply(&next, key, value)) return 0;
    *c = next;
    return 1;
}

int jpt_config_valid(const char* key, const char* value) {
    jpt_config_t scratch;
    jpt_config_defaults(&scratch);
    return jpt_config_apply(&scratch, key, value);
}

void jpt_config_defaults(jpt_config_t* c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < JPT_SETTING_COUNT; i++) apply(c, jpt_settings[i].key, jpt_settings[i].default_value);
}

static char* trim(char* s) {
    char* end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

int jpt_ini_parse_line(const char* line, int allow_comment, char* key, size_t key_cap, char* value, size_t value_cap) {
    char buf[MAX_LINE];
    char *p, *eq, *k, *v, *hash;
    if (!line || strlen(line) >= sizeof buf) return 0;
    strcpy(buf, line);
    p = trim(buf);
    if (*p == '#' || *p == ';') {
        if (!allow_comment) return 0;
        p = trim(p + 1);
    }
    eq = strchr(p, '=');
    if (!eq) return 0;
    *eq = 0;
    k = trim(p);
    v = eq + 1;
    hash = strstr(v, " #");
    if (hash) *hash = 0;
    v = trim(v);
    if (!*k || strpbrk(k, " \t#;") || strlen(k) >= key_cap || strlen(v) >= value_cap) return 0;
    strcpy(key, k);
    strcpy(value, v);
    return 1;
}

typedef struct {
    int has_language;
    char game_text[JPT_L10N_CODE_MAX], subtitles[JPT_L10N_CODE_MAX];
} earlier_language_t;

static void note_earlier_language(earlier_language_t* e, const char* key, const char* value) {
    if (!strcmp(key, JPT_KEY_LANGUAGE)) e->has_language = jpt_l10n_language(value) != NULL;
    else if (!strcmp(key, JPT_KEY_LEGACY_GAME_TEXT_LANGUAGE) && strlen(value) < sizeof e->game_text) strcpy(e->game_text, value);
    else if (!strcmp(key, JPT_KEY_LEGACY_SUBTITLE_LANGUAGE) && strlen(value) < sizeof e->subtitles) strcpy(e->subtitles, value);
}

int jpt_config_load(const char* path, jpt_config_t* c) {
    char line[MAX_LINE], key[128], value[MAX_LINE];
    earlier_language_t earlier;
    FILE* f;
    memset(&earlier, 0, sizeof earlier);
    jpt_config_defaults(c);
    f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!jpt_ini_parse_line(line, 0, key, sizeof key, value, sizeof value)) continue;
        note_earlier_language(&earlier, key, value);
        jpt_config_apply(c, key, value);
    }
    fclose(f);
    if (!earlier.has_language) strcpy(c->language, jpt_l10n_infer_language(earlier.game_text, earlier.subtitles));
    return 1;
}

int jpt_ini_get(const char* path, const char* key, char* value, size_t cap) {
    char line[MAX_LINE], k[128], v[MAX_LINE];
    int found = 0;
    FILE* f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (jpt_ini_parse_line(line, 0, k, sizeof k, v, sizeof v) && !strcmp(k, key) && strlen(v) < cap) {
            strcpy(value, v);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

typedef struct {
    char** lines;
    int count, cap;
} lines_t;

static int push_line(lines_t* l, const char* text) {
    char* copy;
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char** grown = (char**)realloc(l->lines, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        l->lines = grown;
        l->cap = cap;
    }
    copy = _strdup(text);
    if (!copy) return 0;
    l->lines[l->count++] = copy;
    return 1;
}

static void free_lines(lines_t* l) {
    for (int i = 0; i < l->count; i++) free(l->lines[i]);
    free(l->lines);
}

static int find_line(const lines_t* l, const char* key, int commented) {
    char k[128], v[MAX_LINE];
    for (int i = l->count - 1; i >= 0; i--) {
        int is_comment = l->lines[i][strspn(l->lines[i], " \t")] == '#';
        if (is_comment != commented) continue;
        if (jpt_ini_parse_line(l->lines[i], commented, k, sizeof k, v, sizeof v) && !strcmp(k, key)) return i;
    }
    return -1;
}

static int read_lines(const char* path, lines_t* l) {
    char line[MAX_LINE];
    FILE* f = fopen(path, "r");
    int ok = 1;
    if (!f) return 0;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        ok = push_line(l, line);
    }
    fclose(f);
    return ok ? 1 : -1;
}

static int set_line(lines_t* l, const char* key, const char* value, int* header) {
    char line[MAX_LINE];
    int at = find_line(l, key, 0);
    if (!value) {
        if (at < 0) return 1;
        free(l->lines[at]);
        memmove(&l->lines[at], &l->lines[at + 1], (size_t)(l->count - at - 1) * sizeof *l->lines);
        l->count--;
        return 1;
    }
    if (at < 0) at = find_line(l, key, 1);
    _snprintf(line, sizeof line - 1, "%s = %s", key, value);
    line[sizeof line - 1] = 0;
    if (at >= 0) {
        char* copy = _strdup(line);
        if (!copy) return 0;
        free(l->lines[at]);
        l->lines[at] = copy;
        return 1;
    }
    if (!*header && !((l->count == 0 || push_line(l, "")) && push_line(l, APPEND_HEADER))) return 0;
    *header = 1;
    return push_line(l, line);
}

int jpt_ini_set(const char* path, const char* const* keys, const char* const* values, int count) {
    char tmp[MAX_PATH + 8];
    lines_t l = { 0 };
    FILE* f;
    int ok = read_lines(path, &l) >= 0, header = 0;
    for (int i = 0; ok && i < l.count; i++)
        if (!strcmp(l.lines[i], APPEND_HEADER)) header = 1;
    for (int i = 0; ok && i < count; i++) ok = set_line(&l, keys[i], values[i], &header);
    ok = ok && set_line(&l, JPT_KEY_SETTINGS_VERSION, JPT_SETTINGS_VERSION, &header);
    if (ok) {
        _snprintf(tmp, sizeof tmp - 1, "%s.tmp", path);
        tmp[sizeof tmp - 1] = 0;
        f = fopen(tmp, "w");
        ok = f != NULL;
        for (int i = 0; ok && i < l.count; i++) ok = fprintf(f, "%s\n", l.lines[i]) >= 0;
        if (f) ok = fclose(f) == 0 && ok;
        ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileA(tmp);
    }
    free_lines(&l);
    return ok;
}

static int reset_experimental(const char* path) {
    static const char* const keys[] = {
        JPT_KEY_DISPLAY_MODE, JPT_KEY_MONITOR,        JPT_KEY_WINDOW_SIZE, JPT_KEY_PRESENTATION,
        JPT_KEY_RENDERER,     JPT_KEY_RENDER_RESOLUTION, JPT_KEY_ASPECT_RATIO, JPT_KEY_TEXTURE_FILTER,
        JPT_KEY_MSAA,         JPT_KEY_VSYNC,          JPT_KEY_SHOW_FPS,
    };
    const char* removed[sizeof keys / sizeof keys[0]] = { 0 };
    return jpt_ini_set(path, keys, removed, (int)(sizeof keys / sizeof keys[0]));
}

static int is_old_graphics_default(const char* key, const char* value) {
    int on;
    if (!strcmp(key, JPT_KEY_TEXTURE_FILTER)) return !_stricmp(value, "original");
    if (!strcmp(key, JPT_KEY_MSAA)) return !strcmp(value, "0") || !strcmp(value, "1");
    if (!strcmp(key, JPT_KEY_VSYNC)) return jpt_parse_bool(value, &on) && !on;
    return 0;
}

static int reset_old_graphics_defaults(const char* path, int* reset) {
    static const char* const keys[] = { JPT_KEY_TEXTURE_FILTER, JPT_KEY_MSAA, JPT_KEY_VSYNC };
    const char* picked[3];
    const char* values[3];
    char value[64];
    int count = 0;
    for (int i = 0; i < 3; i++)
        if (jpt_ini_get(path, keys[i], value, sizeof value) && is_old_graphics_default(keys[i], value)) {
            picked[count] = keys[i];
            values[count++] = NULL;
        }
    *reset = count;
    return jpt_ini_set(path, picked, values, count);
}

int jpt_config_migrate_language(const char* path) {
    const char* keys[3] = { JPT_KEY_LANGUAGE, JPT_KEY_LEGACY_GAME_TEXT_LANGUAGE, JPT_KEY_LEGACY_SUBTITLE_LANGUAGE };
    const char* values[3] = { NULL, NULL, NULL };
    char text[JPT_L10N_CODE_MAX], subtitles[JPT_L10N_CODE_MAX], language[JPT_L10N_CODE_MAX];
    int has_text, has_subtitles, first = 0;
    if (!path || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    has_text = jpt_ini_get(path, keys[1], text, sizeof text);
    has_subtitles = jpt_ini_get(path, keys[2], subtitles, sizeof subtitles);
    if (!has_text && !has_subtitles) return 0;
    if (jpt_ini_get(path, JPT_KEY_LANGUAGE, language, sizeof language) && jpt_l10n_language(language)) {
        first = 1;
    } else {
        strcpy(language, jpt_l10n_infer_language(has_text ? text : "", has_subtitles ? subtitles : ""));
        values[0] = language;
    }
    return jpt_ini_set(path, keys + first, values + first, 3 - first);
}

int jpt_config_migrate(const char* path) {
    char version[16];
    int reset = 0;
    if (!path || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return JPT_MIGRATED_NOTHING;
    if (!jpt_ini_get(path, JPT_KEY_SETTINGS_VERSION, version, sizeof version))
        return reset_experimental(path) ? JPT_MIGRATED_EXPERIMENTAL : JPT_MIGRATED_NOTHING;
    if (atoi(version) >= atoi(JPT_SETTINGS_VERSION)) return JPT_MIGRATED_NOTHING;
    if (!reset_old_graphics_defaults(path, &reset)) return JPT_MIGRATED_NOTHING;
    return reset ? JPT_MIGRATED_GRAPHICS_DEFAULTS : JPT_MIGRATED_VERSION_ONLY;
}
