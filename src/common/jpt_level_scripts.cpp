#include "jpt_level_scripts.h"

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

jpt_script_session* g_session;
jpt_level_scripts_log_fn g_log;
const jpt_script_engine* g_engine;

void Log(void*, const char* line) {
    if (g_log) g_log(line);
}

char* Read(void*, const char* path, size_t* size) {
    char file[JPT_PATH_MAX];
    *size = 0;
    const int n = snprintf(file, sizeof file, "%s/%s", jpt_custom_level_dir(), path);
    if (n < 0 || static_cast<size_t>(n) >= sizeof file || jpt_fs_is_link(file)) return nullptr;
    FILE* f = fopen(file, "rb");
    if (!f) return nullptr;
    char* bytes = nullptr;
    long length = -1;
    if (!fseek(f, 0, SEEK_END)) length = ftell(f);
    if (length >= 0 && static_cast<unsigned long>(length) <= JPT_SCRIPT_MAX_SOURCE && !fseek(f, 0, SEEK_SET) &&
        (bytes = static_cast<char*>(malloc(static_cast<size_t>(length) + 1))) != nullptr) {
        if (fread(bytes, 1, static_cast<size_t>(length), f) == static_cast<size_t>(length)) {
            *size = static_cast<size_t>(length);
        } else {
            free(bytes);
            bytes = nullptr;
        }
    }
    fclose(f);
    return bytes;
}

uint8_t* Save(size_t* size) { return g_session ? jpt_script_session_save(g_session, size) : nullptr; }

jpt_script_begin BeginOf(jpt_level_reason_t reason) {
    switch (reason) {
    case JPT_REASON_TRANSITION: return JPT_SCRIPT_BEGIN_TRANSITION;
    case JPT_REASON_RESTART: return JPT_SCRIPT_BEGIN_RESTART;
    case JPT_REASON_LOAD_SAVE: return JPT_SCRIPT_BEGIN_SAVE;
    case JPT_REASON_NEW_GAME:
    case JPT_REASON_INITIAL: break;
    }
    return JPT_SCRIPT_BEGIN_NEW_GAME;
}

}  // namespace

extern "C" void jpt_level_scripts_start(jpt_session_kind_t kind, jpt_level_scripts_log_fn log) {
    jpt_level_scripts_end();
    if (kind != JPT_SESSION_CUSTOM || !jpt_custom_level_active()) return;
    g_log = log;
    jpt_script_session_config config{};
    config.log = Log;
    config.read = Read;
    config.per_level_files = 1;
    config.engine = g_engine;
    g_session = jpt_script_session_create(&config);
    for (int i = 0; jpt_custom_level_script_file(i); ++i) jpt_script_session_add_file(g_session, jpt_custom_level_script_file(i));
    const char *scene, *class_name, *object;
    int action;
    for (int i = 0; jpt_custom_level_script_reference(i, &scene, &class_name, &object, &action); ++i)
        jpt_script_session_add_reference(g_session, scene, class_name, object, action);
    const char* file;
    for (int i = 0; jpt_custom_level_script_use(i, &scene, &file); ++i) jpt_script_session_add_level_file(g_session, scene, file);
    jpt_save_set_scripts(Save);
    jpt_custom_level_on_end(jpt_level_scripts_end);
}

extern "C" void jpt_level_scripts_begin(jpt_level_reason_t reason, const char* scene, const char* save) {
    if (!g_session) return;
    uint8_t* state = nullptr;
    size_t size = 0;
    jpt_script_begin why = BeginOf(reason);
    if (save && *save) {
        char problem[128];
        why = JPT_SCRIPT_BEGIN_SAVE;
        state = jpt_save_read_scripts(save, &size, problem, sizeof problem);
        if (problem[0] && g_log) {
            char line[256];
            snprintf(line, sizeof line, "[script] level=%s save state ignored: %s", jpt_session_scene_name(scene), problem);
            g_log(line);
        }
    }
    jpt_script_level_begin(g_session, jpt_session_scene_name(scene), why, state, size);
    free(state);
}

extern "C" void jpt_level_scripts_ready(int ok) {
    if (g_session) jpt_script_level_ready(g_session, ok);
}

extern "C" void jpt_level_scripts_step(float seconds) {
    if (g_session) jpt_script_level_step(g_session, seconds);
}

extern "C" void jpt_level_scripts_act_bite(unsigned animal, unsigned target, const float location[3], float distance, int feint) {
    if (g_session) jpt_script_level_act_bite(g_session, animal, target, location, distance, feint);
}

extern "C" void jpt_level_scripts_set_engine(const jpt_script_engine* engine) { g_engine = engine; }

extern "C" void jpt_level_scripts_paint(void) {
    if (g_session) jpt_script_level_paint(g_session);
}

extern "C" const jpt_draw_frame_t* jpt_level_scripts_frame(void) {
    if (!g_session) return nullptr;
    const jpt_draw_frame_t* frame = jpt_script_level_paint_frame(g_session);
    return frame && frame->count > 0 ? frame : nullptr;
}

extern "C" void jpt_level_scripts_input(const jpt_script_input* in, jpt_script_input_result* out) {
    if (out) *out = jpt_script_input_result{};
    if (g_session) jpt_script_level_input(g_session, in, out);
}

extern "C" int jpt_level_scripts_key(unsigned vk, int down, int shift, int ctrl, int alt) {
    return g_session ? jpt_script_level_key(g_session, vk, down, shift, ctrl, alt) : 0;
}

extern "C" unsigned jpt_level_scripts_user_commands(void) { return g_session ? jpt_script_session_user_commands(g_session) : 0u; }

extern "C" int jpt_level_scripts_drives_player(void) { return g_session && jpt_script_level_drives_player(g_session); }

extern "C" float jpt_level_scripts_speed_factor(unsigned player, float speed) {
    return g_session ? jpt_script_level_speed_factor(g_session, player, speed) : speed;
}

extern "C" float jpt_level_scripts_jump_factor(unsigned player) { return g_session ? jpt_script_level_jump_factor(g_session, player) : 1.0f; }

extern "C" void jpt_level_scripts_jump(unsigned player) {
    if (g_session) jpt_script_level_jump(g_session, player);
}

extern "C" int jpt_level_scripts_console(const char* command, const char* args) {
    return g_session ? jpt_script_level_console(g_session, command, args) : 0;
}

extern "C" void jpt_level_scripts_end(void) {
    if (!g_session) return;
    jpt_script_session* ending = g_session;
    g_session = nullptr;
    jpt_save_set_scripts(nullptr);
    jpt_script_session_destroy(ending);
}

extern "C" jpt_script_session* jpt_level_scripts_session(void) { return g_session; }
