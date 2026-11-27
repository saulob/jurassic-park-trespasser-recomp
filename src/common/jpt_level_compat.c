#include "jpt_level_compat.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_level_features.h"

/* Features of jpt_level_features that the runtime leaves out when it does not have them: each was checked where it would take
 * effect. ActionType 99 makes an action that does nothing; scripts the port cannot plan get no VM; texhw over a CPU-drawn or
 * odd-sized texture only changes how a GPU picture looks; STARS and METEORS layers are not drawn; the settings and files of
 * ATX and Trespasser CE named here are not read. */
static const char* const k_left_out[] = {
    "ATX_ACTION_99",   "ATX_JUMPS",          "ATX_PROJECTILES",       "ATX_PLUGIN",       "ATX_SETTINGS",     "CE_END_GAME_DDF",
    "CE_SCRIPTS",      "CE_SCRIPT_API",      "CE_SCRIPT_FILES",       "CE_EXTENDED_AI",   "CE_AI_ACTIVATE_DISTANCE",
    "CE_WOOD_SAP",     "CE_TEXHW_ODD_SIZE",  "CE_TEXHW_BUMPED",       "CE_SKY_STARS",
};

/* Configuration warnings whose setting is ignored or whose file is not used. */
static const char* const k_unconfirmed_config[] = {
    "config_active_fm_missing", "config_invalid_value", "config_loading_image", "config_mod_path_missing", "config_several_engine_files",
    "config_several_fan_missions", "config_several_tp_mod", "config_tpa_twice", "config_tpa_unusable", "config_twice", "config_unreadable",
};

static const struct {
    const char* code;
    jpt_issue_kind_t kind;
} k_codes[] = {
    { "MISSING_REQUIRED_WORLD", JPT_ISSUE_MISSING },
    { "MISSING_REQUIRED_TERRAIN", JPT_ISSUE_MISSING },
    { "MISSING_REQUIRED_TEXTURE_SET", JPT_ISSUE_MISSING },
    { "MISSING_TEXTURES", JPT_ISSUE_MISSING },
    { "CURVED_BUMP_NOT_IN_SET", JPT_ISSUE_INCOMPATIBLE },
    { "UNSUPPORTED_SCENE_FORMAT", JPT_ISSUE_INCOMPATIBLE },
    { "MALFORMED_WORLD", JPT_ISSUE_DAMAGED },
    { "MALFORMED_SCENE", JPT_ISSUE_DAMAGED },
    { "NOT_A_SCENE", JPT_ISSUE_DAMAGED },
    { "UNREAD_SCENE", JPT_ISSUE_DAMAGED },
    { "CASE_COLLISION", JPT_ISSUE_DAMAGED },
    { "README_FILE_ABSENT", JPT_ISSUE_UNCONFIRMED },
    { "README_NOT_FOLLOWED", JPT_ISSUE_UNCONFIRMED },
    { "TARGET_TAKEN", JPT_ISSUE_UNCONFIRMED },
    { "TEXTURE_UNUSABLE", JPT_ISSUE_UNCONFIRMED },
};

static int listed(const char* name, const char* const* list, size_t count) {
    for (size_t i = 0; name && i < count; i++)
        if (!strcmp(name, list[i])) return 1;
    return 0;
}

static jpt_issue_kind_t feature_kind(const jpt_level_feature_t* feature) {
    if (!feature) return JPT_ISSUE_UNKNOWN;
    return listed(feature->name, k_left_out, sizeof k_left_out / sizeof k_left_out[0]) ? JPT_ISSUE_FEATURE : JPT_ISSUE_INCOMPATIBLE;
}

static jpt_issue_kind_t action_kind(const char* subject) {
    static const char prefix[] = "ActionType ";
    char* end = NULL;
    long type;
    if (!subject || strncmp(subject, prefix, sizeof prefix - 1)) return JPT_ISSUE_UNKNOWN;
    type = strtol(subject + sizeof prefix - 1, &end, 10);
    if (end == subject + sizeof prefix - 1 || *end) return JPT_ISSUE_UNKNOWN;
    return feature_kind(jpt_level_action_feature((int)type));
}

static jpt_issue_kind_t config_kind(const char* subject) {
    if (subject && !strcmp(subject, "config_unsafe_path")) return JPT_ISSUE_SECURITY;
    return listed(subject, k_unconfirmed_config, sizeof k_unconfirmed_config / sizeof k_unconfirmed_config[0]) ? JPT_ISSUE_UNCONFIRMED : JPT_ISSUE_UNKNOWN;
}

jpt_issue_kind_t jpt_level_issue_kind(const char* code, const char* subject, int note) {
    if (note) return JPT_ISSUE_NOTE;
    if (!code || !*code) return JPT_ISSUE_UNKNOWN;
    if (!strcmp(code, "UNSUPPORTED_FEATURE")) return feature_kind(subject ? jpt_level_feature(subject) : NULL);
    if (!strcmp(code, "UNSUPPORTED_ACTION")) return action_kind(subject);
    if (!strcmp(code, "CONFIG_WARNING")) return config_kind(subject);
    for (size_t i = 0; i < sizeof k_codes / sizeof k_codes[0]; i++)
        if (!strcmp(code, k_codes[i].code)) return k_codes[i].kind;
    /* The world reader's faults and the engine limits: an index past its table, or a count past what the engine holds. */
    if (strstr(code, "EXCEED")) return JPT_ISSUE_CAPACITY;
    if (!strncmp(code, "INVALID_", 8) || !strncmp(code, "MALFORMED_", 10)) return JPT_ISSUE_DAMAGED;
    return JPT_ISSUE_UNKNOWN;
}

jpt_compat_t jpt_issue_compat(jpt_issue_kind_t kind) {
    switch (kind) {
    case JPT_ISSUE_NOTE: return JPT_COMPAT_READY;
    case JPT_ISSUE_FEATURE:
    case JPT_ISSUE_UNCONFIRMED: return JPT_COMPAT_LIMITED;
    default: break;
    }
    return JPT_COMPAT_INVALID;
}

jpt_compat_t jpt_compat_worst(jpt_compat_t a, jpt_compat_t b) { return a > b ? a : b; }

const char* jpt_compat_name(jpt_compat_t compat) {
    switch (compat) {
    case JPT_COMPAT_READY: return "ready";
    case JPT_COMPAT_LIMITED: return "limited";
    case JPT_COMPAT_INVALID: break;
    }
    return "invalid";
}

int jpt_compat_parse(const char* name, jpt_compat_t* out) {
    for (int c = JPT_COMPAT_READY; c <= JPT_COMPAT_INVALID; c++)
        if (name && !strcmp(name, jpt_compat_name((jpt_compat_t)c))) return *out = (jpt_compat_t)c, 1;
    return 0;
}

const char* jpt_compat_title(jpt_compat_t compat) {
    switch (compat) {
    case JPT_COMPAT_READY: return "READY";
    case JPT_COMPAT_LIMITED: return "LIMITED COMPATIBILITY";
    case JPT_COMPAT_INVALID: break;
    }
    return "INVALID / UNSAFE";
}

const char* jpt_issue_kind_name(jpt_issue_kind_t kind) {
    static const char* const k_names[JPT_ISSUE_KINDS] = { "note",         "missing feature", "unconfirmed",     "missing file", "damaged file",
                                                          "incompatible", "engine limit",    "security",        "unclassified" };
    return (unsigned)kind < JPT_ISSUE_KINDS ? k_names[kind] : k_names[JPT_ISSUE_UNKNOWN];
}
