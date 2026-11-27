#include "jpt_level_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_alloc.h"
#include "jpt_compat.h"
#include "jpt_fs.h"
#include "jpt_groff.h"
#include "jpt_hash.h"
#include "jpt_szdd.h"
#include "jpt_texhw.h"

#define FILE_LIMIT (128u << 20)

static struct {
    jpt_level_world_t world;
    uint8_t* set;
    size_t set_size;
} W;

static const char* file_name(const char* path) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (!slash || (back && back > slash)) slash = back;
    return slash ? slash + 1 : path;
}

int jpt_level_world_set_name(const char* world, char* out, size_t cap) {
    const char *name = file_name(world), *dot = strrchr(name, '.');
    const int stem = (int)(dot ? (size_t)(dot - name) : strlen(name)), n = snprintf(out, cap, "%.*s-130.pid", stem, name);
    return stem > 0 && n > 0 && (size_t)n < cap;
}

static uint8_t* read_file(const char* path, size_t* size) {
    uint64_t length = 0;
    jpt_fs_file_t* f = jpt_fs_open_read(path, &length);
    uint8_t* bytes = f && length && length <= FILE_LIMIT ? (uint8_t*)jpt_alloc((size_t)length, 1) : NULL;
    if (bytes && !jpt_fs_read_at(f, 0, bytes, (size_t)length)) {
        free(bytes);
        bytes = NULL;
    }
    if (f) jpt_fs_close(f);
    *size = bytes ? (size_t)length : 0;
    return bytes;
}

uint8_t* jpt_level_world_load(const char* path, size_t* size) {
    uint8_t *bytes = read_file(path, size), *expanded;
    if (!bytes || !jpt_szdd_is(bytes, *size)) return bytes;
    expanded = jpt_szdd_expand(bytes, *size, size);
    free(bytes);
    return expanded;
}

/* A world without a texture set still names its textures. */
static void name_textures(const jpt_groff_t* g) {
    jpt_groff_textures_t textures;
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) || !jpt_groff_textures(g, NULL, &textures)) return;
    for (unsigned i = 0; i < textures.count; i++) jpt_texhw_name(textures.items[i].hash, textures.items[i].name);
    jpt_groff_textures_free(&textures);
}

static void compare(const jpt_groff_t* g, jpt_texture_set_t* set, uint8_t* bytes, uint8_t* pid, size_t pid_size) {
    jpt_level_world_t* w = &W.world;
    jpt_groff_textures_t textures;
    char path[JPT_PATH_MAX], problem[128];
    size_t original_size = 0;
    uint8_t* original = NULL;
    jpt_groff_t* original_world = NULL;
    if (w->borrowed) {
        if (jpt_custom_level_game_file(w->world, path, sizeof path)) original = jpt_level_world_load(path, &original_size);
        if (original) original_world = jpt_groff_open(w->world, original, original_size, problem, sizeof problem);
        if (!jpt_texture_set_borrow(set, original_world)) w->failed = 1;
    }
    if (!jpt_groff_textures(g, set, &textures)) {
        w->failed = 1;
    } else {
        w->textures = textures.count;
        w->curved = textures.curved;
        w->curved_missing = textures.curved_missing;
        for (unsigned i = 0; i < textures.count; i++) {
            w->textures_missing += !textures.items[i].in_set;
            jpt_texhw_name(textures.items[i].hash, textures.items[i].name);
        }
        jpt_groff_textures_free(&textures);
    }
    if (w->curved_missing) {
        if (!jpt_groff_hide_unusable(g, set, pid, &w->hidden)) w->failed = 1;
        w->flattened = jpt_groff_flatten_curved(g, set, bytes);
    }
    if (w->hidden) {
        W.set = (uint8_t*)jpt_alloc(pid_size, 1);
        if (W.set) memcpy(W.set, pid, W.set_size = pid_size);
        else w->failed = 1;
    }
    jpt_groff_close(original_world);
    free(original);
}

void jpt_level_world_prepare(const char* path, uint8_t* bytes, size_t size, jpt_level_world_t* out) {
    jpt_level_world_t* w = &W.world;
    char name[JPT_CUSTOM_LEVEL_PATH_MAX], host[JPT_PATH_MAX];
    jpt_groff_report_t report;
    jpt_groff_t* g;
    size_t pid_size = 0;
    uint8_t* pid = NULL;
    jpt_texture_set_t* set;
    jpt_level_world_forget();
    jpt_custom_level_on_end(jpt_level_world_forget);
    snprintf(w->world, sizeof w->world, "%s", file_name(path));
    g = jpt_groff_open(w->world, bytes, size, w->problem, sizeof w->problem);
    if (!g) w->failed = 1;
    if (g) {
        if (!jpt_groff_describe(g, &report)) w->failed = 1;
        if (report.terrain[0]) snprintf(w->terrain, sizeof w->terrain, "%s.wtd", report.terrain);
        if (jpt_level_world_set_name(path, name, sizeof name)) {
            if (jpt_custom_level_find(name, host, sizeof host)) pid = read_file(host, &pid_size);
            else if (jpt_custom_level_game_file(name, host, sizeof host)) w->borrowed = (pid = read_file(host, &pid_size)) != NULL;
        }
        set = pid ? jpt_texture_set_open(pid, pid_size) : NULL;
        if (set) {
            snprintf(w->set, sizeof w->set, "%s", name);
            compare(g, set, bytes, pid, pid_size);
        } else {
            name_textures(g);
        }
        jpt_texture_set_close(set);
        free(pid);
        jpt_groff_close(g);
    }
    if (w->failed && !w->problem[0]) snprintf(w->problem, sizeof w->problem, "out of memory");
    if (out) *out = *w;
}

void jpt_level_world_original(const char* host) {
    char problem[128];
    size_t size = 0;
    uint8_t* bytes = jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) ? jpt_level_world_load(host, &size) : NULL;
    jpt_groff_t* g = bytes ? jpt_groff_open(file_name(host), bytes, size, problem, sizeof problem) : NULL;
    if (g) name_textures(g);
    jpt_groff_close(g);
    free(bytes);
}

uint8_t* jpt_level_world_take_set(const char* path, size_t* size) {
    uint8_t* bytes = W.set;
    if (!bytes || !path || _stricmp(file_name(path), W.world.set)) return NULL;
    *size = W.set_size;
    W.set = NULL;
    return bytes;
}

int jpt_level_world_needs(const char* path) { return path && W.world.terrain[0] && !_stricmp(file_name(path), W.world.terrain); }

int jpt_level_world_line(const jpt_level_world_t* w, int index, char* out, size_t cap) {
    const char* source = w->borrowed ? "the installed game's" : "the package's own";
    if (w->problem[0]) return index == 0 && snprintf(out, cap, "%s: %s", w->world, w->problem) > 0;
    if (!w->set[0]) return index == 0 && snprintf(out, cap, "%s: no texture set", w->world) > 0;
    switch (index) {
    case 0:
        return snprintf(out, cap, "%s: %u of its %u textures are in the texture set %s, %s", w->world, w->textures - w->textures_missing, w->textures, w->set, source) > 0;
    case 1:
        if (!w->curved_missing) return 0;
        return snprintf(out, cap, "%u of its %u polygons with a curved bump map have none in that set, which was made for another build of the world: "
                        "%u are lit flat, %u textures without a picture of their own are read as missing ones",
                        w->curved_missing, w->curved, w->flattened, w->hidden) > 0;
    default: return 0;
    }
}

void jpt_level_world_forget(void) {
    free(W.set);
    memset(&W, 0, sizeof W);
}

uint32_t jpt_level_world_file_hash(const char* path, uint32_t seed) {
    size_t size = 0;
    uint8_t* bytes = read_file(path, &size);
    const uint32_t hash = bytes ? jpt_crc32(seed, bytes, size) : seed;
    free(bytes);
    return hash;
}

/* Of the package's own texture set: the installed game's is as it is. */
static void set_problem(const char* world, jpt_groff_report_t* report) {
    char name[JPT_CUSTOM_LEVEL_PATH_MAX], host[JPT_PATH_MAX];
    size_t size = 0;
    uint8_t* pid = jpt_level_world_set_name(world, name, sizeof name) && jpt_custom_level_find(name, host, sizeof host) ? read_file(host, &size) : NULL;
    jpt_texture_set_t* set = pid ? jpt_texture_set_open(pid, size) : NULL;
    if (set && jpt_texture_set_fault(set, &report->faults[0])) {
        report->fault_count = 1;
        snprintf(report->faults[0].subject, sizeof report->faults[0].subject, "%.*s", JPT_GROFF_TEXT_MAX - 1, name);
    }
    jpt_texture_set_close(set);
    free(pid);
}

int jpt_level_world_read(const char* path, char* terrain, size_t terrain_cap, char* problem, size_t problem_cap) {
    size_t size = 0;
    uint8_t* bytes = jpt_level_world_load(path, &size);
    jpt_groff_t* g = bytes ? jpt_groff_open(file_name(path), bytes, size, problem, problem_cap) : NULL;
    jpt_groff_report_t report;
    if (!bytes) snprintf(problem, problem_cap, "it cannot be read");
    if (g && !jpt_groff_describe(g, &report)) {
        snprintf(problem, problem_cap, "out of memory");
        jpt_groff_close(g);
        g = NULL;
    }
    if (g) {
        snprintf(terrain, terrain_cap, "%s%s", report.terrain, report.terrain[0] ? ".wtd" : "");
        problem[0] = 0;
        if (!report.fault_count) set_problem(path, &report);
        if (report.fault_count)
            snprintf(problem, problem_cap, "%s %s, %u where %u fit", report.faults[0].code, report.faults[0].subject, report.faults[0].value, report.faults[0].limit);
    }
    jpt_groff_close(g);
    free(bytes);
    return g != NULL;
}
