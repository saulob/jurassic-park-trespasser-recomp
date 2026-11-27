#include "jpt_level_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_groff.h"
#include "jpt_level_world.h"
#include "jpt_scene.h"

#define ENGINE_PATH_BYTES 260u

static int s_failed;

static const char* file_name(const char* path) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (!slash || (back && back > slash)) slash = back;
    return slash ? slash + 1 : path;
}

const char* jpt_gate_message(jpt_gate_t gate) {
    switch (gate) {
    case JPT_GATE_OPEN: break;
    case JPT_GATE_DAMAGED: return "This save is damaged and was not loaded";
    case JPT_GATE_INCOMPATIBLE: return "This save is not compatible with the current level";
    case JPT_GATE_MISSING: return "This level is missing required files";
    case JPT_GATE_UNSAFE: return "This level cannot be loaded safely";
    case JPT_GATE_NOT_INCLUDED: return "The next level is not included in this package";
    case JPT_GATE_NOT_AUTHORIZED: return "This level needs Play Anyway in the launcher";
    }
    return NULL;
}

/* Of the package's own worlds: the installed game's stay as they are. */
static uint32_t content_of(const jpt_save_info_t* save) {
    uint32_t content = 0;
    for (int i = 0; i < JPT_SAVE_WORLDS; i++) {
        char file[JPT_PATH_MAX];
        if (save->worlds[i][0] && jpt_custom_level_find(save->worlds[i], file, sizeof file)) content = jpt_level_world_file_hash(file, content);
    }
    return content;
}

static jpt_gate_t world_gate(const char* world, char* why, size_t cap) {
    char file[JPT_PATH_MAX], terrain[JPT_CUSTOM_LEVEL_PATH_MAX], problem[128];
    if (!jpt_custom_level_find(world, file, sizeof file)) {
        if (jpt_custom_level_game_file(world, file, sizeof file)) return JPT_GATE_OPEN;
        snprintf(why, cap, "its world %s is neither in the package nor in the installed game", world);
        return JPT_GATE_MISSING;
    }
    if (!jpt_level_world_read(file, terrain, sizeof terrain, problem, sizeof problem)) {
        snprintf(why, cap, "its world %s cannot be read: %s", world, problem);
        return JPT_GATE_UNSAFE;
    }
    if (problem[0]) {
        snprintf(why, cap, "engine limit exceeded in %s: %s", world, problem);
        return JPT_GATE_UNSAFE;
    }
    if (!terrain[0]) return JPT_GATE_OPEN;
    if (jpt_custom_level_find(terrain, file, sizeof file) ? jpt_fs_exists(file) : jpt_custom_level_game_file(terrain, file, sizeof file)) return JPT_GATE_OPEN;
    snprintf(why, cap, "the terrain data %s of %s is neither in the package nor in the installed game", terrain, world);
    return JPT_GATE_MISSING;
}

/* The engine looks for each world by one path that it never shortens. */
static int paths_fit(char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX], const char* asked) {
    size_t length = (size_t)(file_name(asked) - asked);
    for (int i = 0; i < JPT_SAVE_WORLDS; i++) length += strlen(worlds[i]);
    return length < ENGINE_PATH_BYTES;
}

static jpt_gate_t worlds_gate(char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX], const char* asked, char* why, size_t cap) {
    if (!paths_fit(worlds, asked)) {
        snprintf(why, cap, "engine limit exceeded: the paths of its worlds are longer than %u characters together", ENGINE_PATH_BYTES - 1u);
        return JPT_GATE_UNSAFE;
    }
    for (int i = 0; i < JPT_SAVE_WORLDS; i++) {
        const jpt_gate_t gate = worlds[i][0] ? world_gate(worlds[i], why, cap) : JPT_GATE_OPEN;
        if (gate != JPT_GATE_OPEN) return gate;
    }
    return JPT_GATE_OPEN;
}

/* The compatibility of a scene of the package, as recorded for this launch: Play Anyway covers only the levels it was confirmed for. */
static jpt_gate_t admission(const char* scene, char* why, size_t cap) {
    switch (jpt_custom_level_admits(file_name(scene))) {
    case JPT_ADMIT_OPEN: return JPT_GATE_OPEN;
    case JPT_ADMIT_NEEDS_CONSENT:
        snprintf(why, cap, "%s has limited compatibility and this launch's Play Anyway does not cover it", file_name(scene));
        return JPT_GATE_NOT_AUTHORIZED;
    case JPT_ADMIT_INVALID: snprintf(why, cap, "%s is INVALID / UNSAFE", file_name(scene)); break;
    case JPT_ADMIT_UNRECORDED: snprintf(why, cap, "no compatibility was recorded for %s", file_name(scene)); break;
    }
    return JPT_GATE_UNSAFE;
}

static jpt_gate_t save_gate(const char* file, const char* asked, char* why, size_t cap) {
    jpt_save_info_t save;
    if (!jpt_save_inspect_file(file, &save)) {
        snprintf(why, cap, "%s", save.problem);
        return JPT_GATE_DAMAGED;
    }
    if (!jpt_save_belongs(&save, jpt_custom_level_active() ? jpt_custom_level_package() : "", content_of(&save), why, cap)) return JPT_GATE_INCOMPATIBLE;
    if (!jpt_custom_level_active()) return JPT_GATE_OPEN;
    const jpt_gate_t admitted = save.scene[0] ? admission(save.scene, why, cap) : JPT_GATE_OPEN;
    return admitted != JPT_GATE_OPEN ? admitted : worlds_gate(save.worlds, asked, why, cap);
}

static jpt_gate_t scene_gate(const char* file, const char* asked, char* why, size_t cap) {
    char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX];
    jpt_scene_report_t report;
    size_t size = 0;
    uint8_t* bytes = jpt_level_world_load(file, &size);
    jpt_gate_t gate = JPT_GATE_UNSAFE;
    memset(worlds, 0, sizeof worlds);
    if (!bytes) snprintf(why, cap, "it cannot be read");
    else if (!jpt_scene_readable(bytes, size, &report)) snprintf(why, cap, "%s", report.problem);
    else if (!jpt_save_worlds(bytes, size, worlds)) snprintf(why, cap, "its header is damaged");
    else gate = worlds_gate(worlds, asked, why, cap);
    free(bytes);
    return gate;
}

jpt_gate_t jpt_gate_load(const char* file, const char* asked, char* why, size_t cap) {
    const char* name = file_name(asked);
    char own[JPT_PATH_MAX];
    if (cap) why[0] = 0;
    if (jpt_save_is_name(name) || jpt_save_is_temporary(name)) return save_gate(file, asked, why, cap);
    if (!jpt_custom_level_active()) return JPT_GATE_OPEN;
    if (strlen(name) > JPT_SAVE_LEVEL_NAME_MAX) {
        snprintf(why, cap, "the game keeps a level's name of %d characters at most", JPT_SAVE_LEVEL_NAME_MAX);
        return JPT_GATE_UNSAFE;
    }
    if (!jpt_custom_level_find(name, own, sizeof own)) {
        if (!jpt_custom_level_has_game() || jpt_custom_level_game_file(name, own, sizeof own)) return JPT_GATE_OPEN;
        snprintf(why, cap, "neither the package nor the installed game has %s: the game stays in the current level", name);
        return JPT_GATE_NOT_INCLUDED;
    }
    const jpt_gate_t admitted = admission(name, why, cap);
    return admitted != JPT_GATE_OPEN ? admitted : scene_gate(own, asked, why, cap);
}

int jpt_gate_lists(const char* file, char* why, size_t cap) {
    if (!jpt_save_is_name(file)) {
        snprintf(why, cap, "it is not a save's name");
        return 0;
    }
    return jpt_gate_reads(file, why, cap);
}

int jpt_gate_reads(const char* file, char* why, size_t cap) {
    jpt_save_info_t save;
    if (jpt_save_inspect_file(file, &save)) return 1;
    snprintf(why, cap, "%s", save.problem);
    return 0;
}

int jpt_gate_save_begin(const char* path, char* temp, size_t cap) { return jpt_save_begin(path, temp, cap); }

int jpt_gate_save_end(const char* path, const char* temp, char* why, size_t cap) {
    const int ok = jpt_save_finish(path, temp, jpt_custom_level_active() ? jpt_custom_level_package() : "", content_of, why, cap);
    if (!ok) s_failed = 1;
    return ok;
}

int jpt_gate_save_failed(void) {
    const int failed = s_failed;
    s_failed = 0;
    return failed;
}
