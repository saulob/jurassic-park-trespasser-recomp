#include "jpt_custom_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_fs.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

typedef struct {
    const char* file;
    char name[JPT_CUSTOM_LEVEL_SCENE_CAP];
} scene_t;

static struct {
    int active;
    char package[JPT_CUSTOM_LEVEL_ID_MAX + 1];
    char dir[JPT_PATH_MAX];
    char** files;
    int count, overflow;
    scene_t scenes[JPT_CUSTOM_LEVEL_SCENES_MAX];
    int scene_count, scene;
} C;

static int alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

int jpt_custom_level_id_valid(const char* id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > JPT_CUSTOM_LEVEL_ID_MAX || !alnum(id[0]) || id[n - 1] == '.' || id[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++)
        if (!alnum(id[i]) && !strchr(" ._-", id[i])) return 0;
    return 1;
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char* extension(const char* path) {
    const char* dot = strrchr(base_name(path), '.');
    return dot ? dot : "";
}

static int is_scene(const char* path) { return !_stricmp(extension(path), ".scn"); }

int jpt_custom_level_scene_name(const char* file_name, int index, char* out, size_t cap) {
    size_t n = strlen(file_name);
    int keep = n > 4 && n <= JPT_CUSTOM_LEVEL_SCENE_MAX && is_scene(file_name), wrote;
    for (size_t i = 0; keep && i + 4 < n; i++) keep = alnum(file_name[i]) || file_name[i] == '_' || file_name[i] == '-';
    wrote = keep ? snprintf(out, cap, "%s", file_name) : snprintf(out, cap, "LEVEL%d.SCN", index + 1);
    return wrote > 0 && (size_t)wrote < cap;
}

int jpt_custom_level_saves_dir(const char* saves_dir, const char* package, char* out, size_t cap) {
    int n;
    if (!jpt_custom_level_id_valid(package)) return 0;
    n = snprintf(out, cap, "%s" SEP JPT_CUSTOM_LEVELS_DIR SEP "%s", saves_dir, package);
    return n > 0 && (size_t)n < cap;
}

void jpt_custom_level_deactivate(void) {
    for (int i = 0; i < C.count; i++) free(C.files[i]);
    free(C.files);
    memset(&C, 0, sizeof C);
}

int jpt_custom_level_active(void) { return C.active; }
const char* jpt_custom_level_package(void) { return C.package; }
const char* jpt_custom_level_scene(void) { return C.active ? C.scenes[C.scene].name : ""; }
int jpt_custom_level_scene_count(void) { return C.scene_count; }

const char* jpt_custom_level_scene_at(int index) {
    return index >= 0 && index < C.scene_count ? C.scenes[index].name : "";
}

static int host_path(const char* relative, char* out, size_t cap) {
    int n = snprintf(out, cap, "%s%s%s", C.dir, relative[0] ? SEP : "", relative);
    if (n < 0 || (size_t)n >= cap) return 0;
#ifdef _WIN32
    for (char* c = out + strlen(C.dir); *c; c++)
        if (*c == '/') *c = '\\';
#endif
    return 1;
}

typedef struct {
    const char* prefix;
    int depth;
} walk_t;

static void walk(const char* relative, int depth);

static void visit(const char* name, int is_dir, void* user) {
    const walk_t* w = (const walk_t*)user;
    char path[JPT_CUSTOM_LEVEL_PATH_MAX], host[JPT_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s%s%s", w->prefix, w->prefix[0] ? "/" : "", name);
    if (n < 0 || n >= (int)sizeof path || !host_path(path, host, sizeof host) || jpt_fs_is_link(host)) return;
    if (is_dir) {
        walk(path, w->depth + 1);
        return;
    }
    if (C.count == JPT_CUSTOM_LEVEL_FILES_MAX || (C.files[C.count] = _strdup(path)) == NULL) {
        C.overflow = 1;
        return;
    }
    C.count++;
}

static void walk(const char* relative, int depth) {
    char dir[JPT_PATH_MAX];
    walk_t w;
    if (depth > JPT_CUSTOM_LEVEL_DEPTH_MAX || !host_path(relative, dir, sizeof dir)) return;
    w.prefix = relative;
    w.depth = depth;
    jpt_fs_list(dir, visit, &w);
}

static int by_path(const void* a, const void* b) {
    const char *x = *(char* const*)a, *y = *(char* const*)b;
    int c = _stricmp(x, y);
    return c ? c : strcmp(x, y);
}

static void name_scenes(void) {
    for (int i = 0; i < C.count && C.scene_count < JPT_CUSTOM_LEVEL_SCENES_MAX; i++) {
        scene_t* s = &C.scenes[C.scene_count];
        if (!is_scene(C.files[i])) continue;
        s->file = C.files[i];
        jpt_custom_level_scene_name(base_name(s->file), C.scene_count, s->name, sizeof s->name);
        for (int k = 0; k < C.scene_count; k++)
            if (!_stricmp(C.scenes[k].name, s->name)) snprintf(s->name, sizeof s->name, "LEVEL%d.SCN", C.scene_count + 1);
        C.scene_count++;
    }
}

static int refuse(char* error, size_t cap, const char* text, const char* subject) {
    snprintf(error, cap, "%s: %s", text, subject);
    jpt_custom_level_deactivate();
    return 0;
}

int jpt_custom_level_activate(const char* user_root, const jpt_custom_level_request_t* request, char* error, size_t cap) {
    int n;
    jpt_custom_level_deactivate();
    if (!jpt_custom_level_id_valid(request->package))
        return refuse(error, cap, "invalid custom level package", request->package);
    n = snprintf(C.dir, sizeof C.dir, "%s" SEP JPT_CUSTOM_LEVELS_DIR SEP "%s", user_root, request->package);
    if (n < 0 || n >= (int)sizeof C.dir || !jpt_fs_is_dir(C.dir) || jpt_fs_is_link(C.dir))
        return refuse(error, cap, "the custom level package is not installed", request->package);
    C.files = (char**)calloc(JPT_CUSTOM_LEVEL_FILES_MAX, sizeof *C.files);
    if (!C.files) return refuse(error, cap, "out of memory for the custom level package", request->package);
    walk("", 0);
    if (C.overflow) return refuse(error, cap, "the custom level package has too many files", request->package);
    qsort(C.files, (size_t)C.count, sizeof *C.files, by_path);
    name_scenes();
    for (C.scene = 0; C.scene < C.scene_count; C.scene++)
        if (!_stricmp(C.scenes[C.scene].file, request->scene)) break;
    if (C.scene == C.scene_count) return refuse(error, cap, "the custom level package has no such scene", request->scene);
    snprintf(C.package, sizeof C.package, "%s", request->package);
    C.active = 1;
    return 1;
}

static int existing(const char* relative, char* out, size_t cap) {
    for (int i = 0; i < C.count; i++)
        if (!_stricmp(C.files[i], relative)) return host_path(C.files[i], out, cap);
    return 0;
}

static int beside(const char* scene, const char* relative, char* out, size_t cap) {
    char path[JPT_PATH_MAX];
    int folder = (int)(base_name(scene) - scene), n = snprintf(path, sizeof path, "%.*s%s", folder, scene, relative);
    return n > 0 && n < (int)sizeof path && existing(path, out, cap);
}

static int is_level_data(const char* path) {
    static const char* const k_types[] = { ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".asa", ".asb" };
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++)
        if (!_stricmp(extension(path), k_types[i])) return 1;
    return 0;
}

/* The game takes the first file of a set that exists: an original one must not stand in for the package's. */
static int hidden(const char* path) {
    static const char* const k_sets[][5] = { { ".spz", ".spp", ".swp", ".pid", NULL }, { ".asb", ".asa", NULL } };
    const char* name = base_name(path);
    size_t stem = (size_t)(extension(path) - name);
    for (size_t s = 0; s < sizeof k_sets / sizeof k_sets[0]; s++) {
        int member = 0;
        for (int k = 0; k_sets[s][k]; k++) member |= !_stricmp(extension(path), k_sets[s][k]);
        for (int i = 0; member && i < C.count; i++) {
            const char* file = base_name(C.files[i]);
            if (_strnicmp(file, name, stem) || (size_t)(extension(file) - file) != stem) continue;
            for (int k = 0; k_sets[s][k]; k++)
                if (!_stricmp(extension(file), k_sets[s][k])) return 1;
        }
    }
    return 0;
}

/* Level data a package keeps outside the folder the game expects (an animation script beside the scene). */
static int only_one_named(const char* name, char* out, size_t cap) {
    int found = -1;
    for (int i = 0; i < C.count; i++) {
        if (_stricmp(base_name(C.files[i]), name)) continue;
        if (found >= 0) return 0;
        found = i;
    }
    return found >= 0 && host_path(C.files[found], out, cap);
}

static int clean(const char* path, char* out, size_t cap) {
    size_t n = strlen(path);
    if (!n || n >= cap || strchr(path, ':')) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = path[i] == '\\' ? '/' : path[i];
    for (const char* c = out;; c++) {
        size_t length = strcspn(c, "/");
        if (!length || (length == 1 && c[0] == '.') || (length == 2 && c[0] == '.' && c[1] == '.')) return 0;
        c += length;
        if (!*c) return 1;
    }
}

int jpt_custom_level_find(const char* data_relative, char* out, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    if (!C.active || !clean(data_relative, path, sizeof path) || !_strnicmp(path, "menu/", 5)) return 0;
    if (!strchr(path, '/') && is_scene(path)) {
        for (int i = 0; i < C.scene_count; i++)
            if (!_stricmp(C.scenes[i].name, path)) return host_path(C.scenes[i].file, out, cap);
        return 0;
    }
    if (beside(C.scenes[C.scene].file, path, out, cap)) return 1;
    for (int i = 0; i < C.scene_count; i++)
        if (i != C.scene && beside(C.scenes[i].file, path, out, cap)) return 1;
    if (!is_level_data(path)) return 0;
    return only_one_named(base_name(path), out, cap) || (hidden(path) && host_path(path, out, cap));
}
