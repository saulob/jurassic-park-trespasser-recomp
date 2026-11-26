#include "level_session.h"

#include <stdio.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"

static void log_line(const char* tag, const char* text) { host_log(tag, "%s", text); }

static void capability_names(char* out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    for (int c = 0; c < JPT_LEVEL_CAPS && used < cap; c++) {
        int n;
        if (!jpt_custom_level_requires((jpt_level_cap_t)c)) continue;
        n = snprintf(out + used, cap - used, "%s%s", used ? "," : "", jpt_custom_level_cap_name((jpt_level_cap_t)c));
        if (n < 0) break;
        used += (size_t)n;
    }
}

void level_session_start(const char* build, const char* renderer, const char* language) {
    jpt_session_info_t info;
    char caps[512];
    memset(&info, 0, sizeof info);
    jpt_session_set_log(log_line);
    info.kind = jpt_custom_level_active() ? JPT_SESSION_CUSTOM : JPT_SESSION_CAMPAIGN;
    info.build = build;
    info.renderer = renderer;
    info.language = language;
    if (info.kind == JPT_SESSION_CUSTOM) {
        capability_names(caps, sizeof caps);
        info.package = jpt_custom_level_package();
        info.layout = jpt_custom_level_layout();
        info.start_level = jpt_custom_level_scene();
        info.capabilities = caps;
    }
    jpt_session_begin(&info);
}

jpt_level_source_t level_session_source(const char* scene) {
    char file[JPT_PATH_MAX];
    const char* name = jpt_session_scene_name(scene);
    return jpt_custom_level_active() && *name && jpt_custom_level_find(name, file, sizeof file) ? JPT_SOURCE_PACKAGE : JPT_SOURCE_RETAIL;
}
