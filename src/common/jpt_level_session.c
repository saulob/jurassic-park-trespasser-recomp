#include "jpt_level_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SCENE_CAP 64

static struct {
    jpt_session_log_fn log;
    jpt_session_kind_t kind;
    unsigned generation, levels;
    int loading, restarting, purged;
    jpt_restart_mode_t mode;
    unsigned restart_from;
    char scene[SCENE_CAP];
    jpt_level_source_t source;
} S;

static void say(const char* tag, const char* fmt, ...) {
    char text[512];
    va_list ap;
    if (!S.log) return;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    S.log(tag, text);
}

static const char* text_or(const char* s, const char* other) { return s && *s ? s : other; }
static const char* kind_name(void) { return S.kind == JPT_SESSION_CUSTOM ? "custom" : "campaign"; }
static const char* source_name(jpt_level_source_t source) { return source == JPT_SOURCE_PACKAGE ? "package" : "retail"; }
static const char* mode_name(jpt_restart_mode_t mode) { return mode == JPT_RESTART_FULL ? "full" : "scene_reload"; }

void jpt_session_set_log(jpt_session_log_fn log) { S.log = log; }

void jpt_session_begin(const jpt_session_info_t* info) {
    const jpt_session_log_fn log = S.log;
    memset(&S, 0, sizeof S);
    S.log = log;
    S.kind = info->kind == JPT_SESSION_CUSTOM ? JPT_SESSION_CUSTOM : JPT_SESSION_CAMPAIGN;
    say("session", "type=%s", kind_name());
    if (S.kind == JPT_SESSION_CUSTOM) {
        say("session", "package=%s layout=%s", text_or(info->package, "?"), text_or(info->layout, "unknown"));
        say("session", "start_level=%s", jpt_session_scene_name(text_or(info->start_level, "?")));
    }
    say("session", "game_build=%s", text_or(info->build, "unknown"));
    say("session", "renderer=%s", text_or(info->renderer, "unknown"));
    if (info->language && *info->language) say("session", "language=%s", info->language);
    say("session", "saves=%s", S.kind == JPT_SESSION_CUSTOM ? "custom package namespace" : "game");
    if (S.kind == JPT_SESSION_CUSTOM) {
        say("session", "capabilities=%s", text_or(info->capabilities, "none"));
        say("session", "compatibility=%s experimental=%d", text_or(info->compatibility, "unknown"), info->experimental ? 1 : 0);
    }
}

void jpt_session_end(void) {
    if (S.kind == JPT_SESSION_NONE) return;
    say("session", "end type=%s levels=%u world_generation=%u", kind_name(), S.levels, S.generation);
    S.kind = JPT_SESSION_NONE;
    S.loading = S.restarting = S.purged = 0;
}

jpt_session_kind_t jpt_session_kind(void) { return S.kind; }

const char* jpt_session_reason_name(jpt_level_reason_t reason) {
    switch (reason) {
    case JPT_REASON_NEW_GAME: return "new_game";
    case JPT_REASON_INITIAL: return "initial";
    case JPT_REASON_TRANSITION: return "transition";
    case JPT_REASON_RESTART: return "restart";
    case JPT_REASON_LOAD_SAVE: return "load_save";
    }
    return "unknown";
}

const char* jpt_session_scene_name(const char* path) {
    const char* name = path;
    for (const char* p = path; p && *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name ? name : "";
}

jpt_level_reason_t jpt_session_reason(int save_file, int from_main_menu) {
    if (S.restarting) return JPT_REASON_RESTART;
    if (save_file) return JPT_REASON_LOAD_SAVE;
    if (from_main_menu || !S.levels) return S.kind == JPT_SESSION_CUSTOM ? JPT_REASON_INITIAL : JPT_REASON_NEW_GAME;
    return JPT_REASON_TRANSITION;
}

void jpt_session_level_begin(jpt_level_reason_t reason, const char* scene, jpt_level_source_t source) {
    if (!S.levels) S.generation++;
    S.levels++;
    S.loading = 1;
    S.source = source;
    snprintf(S.scene, sizeof S.scene, "%s", jpt_session_scene_name(scene));
    say("level", "begin reason=%s scene=%s source=%s session=%s", jpt_session_reason_name(reason), S.scene, source_name(source), kind_name());
}

void jpt_session_level_end(int ok, int code) {
    if (!S.loading) return;
    S.loading = 0;
    if (ok) say("level", "ready scene=%s source=%s world_generation=%u", S.scene, source_name(S.source), S.generation);
    else say("level", "failed scene=%s source=%s reason=the game's load returned %d", S.scene, source_name(S.source), code);
}

void jpt_session_world_purged(void) {
    S.generation++;
    if (S.restarting) S.purged = 1;
}

jpt_restart_mode_t jpt_session_restart_mode(void) { return S.kind == JPT_SESSION_CUSTOM ? JPT_RESTART_FULL : JPT_RESTART_SCENE_RELOAD; }

jpt_restart_mode_t jpt_session_restart_begin(const char* scene, jpt_level_source_t source) {
    S.mode = jpt_session_restart_mode();
    S.restarting = 1;
    S.purged = 0;
    S.restart_from = S.generation;
    say("restart", "requested scene=%s session=%s mode=%s source=%s", jpt_session_scene_name(scene), kind_name(), mode_name(S.mode), source_name(source));
    return S.mode;
}

int jpt_session_restart_purges(void) { return S.restarting && S.mode == JPT_RESTART_FULL && !S.purged; }
int jpt_session_restarting(void) { return S.restarting; }

void jpt_session_restart_end(int ok) {
    const char* result = !ok ? "failed" : S.mode == JPT_RESTART_FULL && !S.purged ? "failed (the world was not rebuilt)" : "ready";
    if (!S.restarting) return;
    S.restarting = 0;
    say("restart", "result=%s old_world_generation=%u new_world_generation=%u", result, S.restart_from, S.generation);
}

unsigned jpt_session_world_generation(void) { return S.generation; }
unsigned jpt_session_levels(void) { return S.levels; }
const char* jpt_session_current_scene(void) { return S.scene; }
jpt_level_source_t jpt_session_current_source(void) { return S.source; }
