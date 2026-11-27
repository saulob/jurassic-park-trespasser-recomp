#include "jpt_custom_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_fs.h"
#include "jpt_level_compat.h"
#include "jpt_scene.h"
#include "jpt_szdd.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define MAP_LIMIT (4 * 1024 * 1024)
#define SCENE_LIMIT (64 * 1024 * 1024)
#define BMP_HEAD 54
#define BMP_PIXELS (BMP_HEAD + 256u * 4u)
#define TEXTURE_SIDE_MAX 2048u
#define MISSING_SIDE 4u
#define MISSING_GREY 0x7F7F7Fu
#define TEXHW "texhw/"
#define TEXHW_SHARED "texhw/Map/"
#define SHARED_FOLDER_MIN 4 /* Trespasser CE: the folder in front of the name is more than "Map" */
#define SETTINGS_MAX 8
#define TPA_MAX 64
#define LOADING_MAX JPT_CUSTOM_LEVEL_SCENES_MAX
#define LIMITS_MAX 4
#define ENDS_MAX 12
#define SCRIPTS_MAX 256
#define SCRIPT_REFS_MAX 1024
#define ACTION_FIRST 99
#define ACTION_LAST 121

typedef struct {
    const char* file;
    char name[JPT_CUSTOM_LEVEL_SCENE_CAP];
    int configured; /* in the active mod: the map's files come first, and its sounds and pictures apply */
    int compat;     /* jpt_compat_t the launcher recorded; -1 for none */
    int authorized; /* covered by this launch's Play Anyway */
} scene_t;

typedef struct {
    char* target;
    const char* file;
} mapped_t;

typedef struct {
    jpt_level_tpa_t kind;
    int replaces;
    const char* scene;
    const char* file;
} tpa_t;

typedef struct {
    const char* scene;
    const char* file;
} loading_t;

static struct {
    int active;
    char package[JPT_CUSTOM_LEVEL_ID_MAX + 1];
    char layout[24];
    char dir[JPT_PATH_MAX];
    char records[JPT_PATH_MAX];
    int linked;
    char* link;
    const char* scopes[JPT_CUSTOM_LEVEL_LINK_SCOPES_MAX];
    int scope_count;
    char** files;
    int count, overflow;
    scene_t scenes[JPT_CUSTOM_LEVEL_SCENES_MAX];
    int scene_count, scene, current; /* current: -1 for a level of the original game */
    int context;                     /* the package's scene whose files and settings hold: the last one loaded */
    char current_name[JPT_CUSTOM_LEVEL_PATH_MAX];
    char video[JPT_CUSTOM_LEVEL_PATH_MAX]; /* below the data folder */
    char* map;
    mapped_t* mapped;
    int mapped_count;
    tpa_t tpas[TPA_MAX];
    int tpa_count;
    loading_t loading[LOADING_MAX];
    int loading_count;
    const char* settings[SETTINGS_MAX][2];
    int setting_count, skip_win;
    struct {
        const char* name;
        unsigned value;
    } limits[LIMITS_MAX];
    int limit_count;
    unsigned map_caps;    /* asked for by the package's map: of its configured levels */
    unsigned format_caps; /* of the scenes read so far */
    uint32_t actions;     /* bit 0: ActionType 99 */
    int scene_version;
    const char* scripts[SCRIPTS_MAX];
    int script_count;
    struct {
        const char *scene, *class_name, *object;
        int action;
    } script_refs[SCRIPT_REFS_MAX];
    int script_ref_count;
    struct {
        const char *scene, *file;
    } script_uses[SCRIPT_REFS_MAX];
    int script_use_count;
    int compat_known, experimental;
} C;

static jpt_custom_level_game_fn s_game;
static jpt_custom_level_end_fn s_ends[ENDS_MAX];
static int s_end_count;
static unsigned s_serial;

static unsigned bit(jpt_level_cap_t cap) { return 1u << cap; }

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

const char* jpt_custom_level_cap_name(jpt_level_cap_t cap) {
    static const char* const k_names[JPT_LEVEL_CAPS] = { "scene_format", "script_state",     "texhw",        "sound_added", "sound_replaced",
                                                         "loading_image", "skip_ending",     "dual_stow",    "water_refraction", "enhanced_sky",
                                                         "active_ai",    "mesh_faces",       "mesh_heap_mb", "terrain_nodes", "texture_pool_mb", "terrain_cull_mm", "full_restart", "sky" };
    return (unsigned)cap < JPT_LEVEL_CAPS ? k_names[cap] : "";
}

void jpt_custom_level_on_end(jpt_custom_level_end_fn ended) {
    for (int i = 0; i < s_end_count; i++)
        if (s_ends[i] == ended) return;
    if (s_end_count < ENDS_MAX) s_ends[s_end_count++] = ended;
}

unsigned jpt_custom_level_caps(void) {
    if (!C.active) return 0;
    return C.format_caps | (C.scenes[C.scene].configured ? C.map_caps : 0);
}

int jpt_custom_level_requires(jpt_level_cap_t cap) { return (unsigned)cap < JPT_LEVEL_CAPS && (jpt_custom_level_caps() & bit(cap)) != 0; }

int jpt_custom_level_action(int type) {
    return C.active && type >= ACTION_FIRST && type <= ACTION_LAST && (C.actions >> (type - ACTION_FIRST) & 1u) != 0;
}

void jpt_custom_level_deactivate(void) {
    const int ended = C.active;
    C.active = 0;
    for (int i = 0; ended && i < s_end_count; i++) s_ends[i]();
    for (int i = 0; i < C.count; i++) free(C.files[i]);
    free(C.files);
    free(C.mapped);
    free(C.map);
    free(C.link);
    memset(&C, 0, sizeof C);
    s_game = NULL;
}

int jpt_custom_level_active(void) { return C.active; }
const char* jpt_custom_level_package(void) { return C.package; }
const char* jpt_custom_level_layout(void) { return C.layout; }
const char* jpt_custom_level_scene(void) { return C.active ? C.scenes[C.scene].name : ""; }
int jpt_custom_level_scene_count(void) { return C.scene_count; }

const char* jpt_custom_level_script_file(int index) { return C.active && index >= 0 && index < C.script_count ? C.scripts[index] : NULL; }

int jpt_custom_level_script_reference(int index, const char** scene, const char** class_name, const char** object, int* action) {
    if (!C.active || index < 0 || index >= C.script_ref_count) return 0;
    *scene = C.script_refs[index].scene;
    *class_name = C.script_refs[index].class_name;
    *object = C.script_refs[index].object;
    *action = C.script_refs[index].action;
    return 1;
}

int jpt_custom_level_script_use(int index, const char** scene, const char** file) {
    if (!C.active || index < 0 || index >= C.script_use_count) return 0;
    *scene = C.script_uses[index].scene;
    *file = C.script_uses[index].file;
    return 1;
}

const char* jpt_custom_level_scene_at(int index) {
    return index >= 0 && index < C.scene_count ? C.scenes[index].name : "";
}

const char* jpt_custom_level_scene_file_at(int index) {
    return index >= 0 && index < C.scene_count ? C.scenes[index].file : "";
}

const char* jpt_custom_level_current_scene(void) { return C.active && C.current >= 0 ? C.scenes[C.current].file : ""; }
const char* jpt_custom_level_dir(void) { return C.dir; }
const char* jpt_custom_level_records_dir(void) { return C.records; }
int jpt_custom_level_linked(void) { return C.active && C.linked; }

/* What the launcher writes about a package. A linked package's copy of these names is never its own file. */
static int generated(const char* relative) {
    const size_t n = sizeof JPT_CUSTOM_LEVEL_GENERATED_DIR - 1;
    if (!_strnicmp(relative, JPT_CUSTOM_LEVEL_GENERATED_DIR, n) && (relative[n] == 0 || relative[n] == '/')) return 1;
    return !_stricmp(relative, JPT_CUSTOM_LEVEL_RUNTIME_MAP) || !_stricmp(relative, "manifest.json") ||
           !_stricmp(relative, JPT_CUSTOM_LEVEL_LINK_FILE);
}

/* Programs and scripts are never level data; the importer leaves them out, and a linked folder's are not listed. */
static int program(const char* name) {
    static const char* const k_types[] = { ".exe", ".dll", ".com", ".scr", ".msi", ".asi", ".so",  ".sys", ".ocx", ".cpl", ".pif", ".bat",
                                           ".cmd", ".ps1", ".vbs", ".js",  ".wsf", ".hta", ".sh",  ".lnk", ".reg", ".url", ".jar", ".py" };
    const char* dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof k_types / sizeof k_types[0]; i++)
        if (!_stricmp(dot, k_types[i])) return 1;
    return 0;
}

static int host_path(const char* relative, char* out, size_t cap) {
    const char* base = C.linked && generated(relative) ? C.records : C.dir;
    int n = snprintf(out, cap, "%s%s%s", base, relative[0] ? SEP : "", relative);
    if (n < 0 || (size_t)n >= cap) return 0;
#ifdef _WIN32
    for (char* c = out + strlen(base); *c; c++)
        if (*c == '/') *c = '\\';
#endif
    return 1;
}

typedef struct {
    const char* prefix;
    int depth;
    int records; /* the records folder's own files, of a linked package */
} walk_t;

static void walk(const char* relative, int depth, int records);

static void add_file(const char* path) {
    if (C.count == JPT_CUSTOM_LEVEL_FILES_MAX || (C.files[C.count] = _strdup(path)) == NULL) {
        C.overflow = 1;
        return;
    }
    C.count++;
}

static void visit(const char* name, int is_dir, void* user) {
    const walk_t* w = (const walk_t*)user;
    char path[JPT_CUSTOM_LEVEL_PATH_MAX], host[JPT_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s%s%s", w->prefix, w->prefix[0] ? "/" : "", name);
    if (n < 0 || n >= (int)sizeof path || !host_path(path, host, sizeof host) || jpt_fs_is_link(host)) return;
    if (C.linked && !w->records && generated(path)) return;
    if (is_dir) {
        walk(path, w->depth + 1, w->records);
        return;
    }
    if (!program(name)) add_file(path);
}

static void walk(const char* relative, int depth, int records) {
    char dir[JPT_PATH_MAX];
    walk_t w;
    if (depth > JPT_CUSTOM_LEVEL_DEPTH_MAX || !host_path(relative, dir, sizeof dir)) return;
    w.prefix = relative;
    w.depth = depth;
    w.records = records;
    jpt_fs_list(dir, visit, &w);
}

/* A linked package is the parts of its folder the link names, or all of it, and the records' own generated files. */
static void walk_package(void) {
    if (!C.linked || !C.scope_count) walk("", 0, 0);
    if (!C.linked) return;
    for (int i = 0; i < C.scope_count; i++) {
        char host[JPT_PATH_MAX];
        int depth = 1;
        for (const char* c = C.scopes[i]; *c; c++) depth += *c == '/';
        if (!host_path(C.scopes[i], host, sizeof host) || jpt_fs_is_link(host) || generated(C.scopes[i])) continue;
        if (jpt_fs_is_dir(host)) walk(C.scopes[i], depth, 0);
        else if (jpt_fs_exists(host) && !program(C.scopes[i])) add_file(C.scopes[i]);
    }
    walk(JPT_CUSTOM_LEVEL_GENERATED_DIR, 1, 1);
}

static int by_path(const void* a, const void* b) {
    const char *x = *(char* const*)a, *y = *(char* const*)b;
    int c = _stricmp(x, y);
    return c ? c : strcmp(x, y);
}

/* Scopes that hold one another would list a file twice. */
static void drop_repeats(void) {
    int kept = 0;
    for (int i = 0; i < C.count; i++) {
        if (kept && !strcmp(C.files[kept - 1], C.files[i])) {
            free(C.files[i]);
            continue;
        }
        C.files[kept++] = C.files[i];
    }
    C.count = kept;
}

static void name_scenes(void) {
    for (int i = 0; i < C.count && C.scene_count < JPT_CUSTOM_LEVEL_SCENES_MAX; i++) {
        scene_t* s = &C.scenes[C.scene_count];
        if (!is_scene(C.files[i])) continue;
        s->file = C.files[i];
        s->compat = -1;
        jpt_custom_level_scene_name(base_name(s->file), C.scene_count, s->name, sizeof s->name);
        for (int k = 0; k < C.scene_count; k++)
            if (!_stricmp(C.scenes[k].name, s->name)) snprintf(s->name, sizeof s->name, "LEVEL%d.SCN", C.scene_count + 1);
        C.scene_count++;
    }
}

/* The package's own spelling of a path; NULL when it is absent or only told apart by case. */
static const char* own_file(const char* relative) {
    const char* found = NULL;
    for (int i = 0; i < C.count; i++) {
        if (!strcmp(C.files[i], relative)) return C.files[i];
        if (_stricmp(C.files[i], relative)) continue;
        if (found) return NULL;
        found = C.files[i];
    }
    return found;
}

static char* next_field(char** rest) {
    char *field = *rest, *bar = field ? strchr(field, '|') : NULL;
    if (bar) *bar = 0;
    *rest = bar ? bar + 1 : NULL;
    return field;
}

static int tpa_kind(const char* name, jpt_level_tpa_t* kind) {
    static const char* const k_names[JPT_LEVEL_TPA_KINDS] = { "stream", "effects", "ambient" };
    for (int i = 0; i < JPT_LEVEL_TPA_KINDS; i++)
        if (!strcmp(name, k_names[i])) return *kind = (jpt_level_tpa_t)i, 1;
    return 0;
}

/* The capability a setting or a limit of that name is; JPT_LEVEL_CAPS for none. */
static jpt_level_cap_t cap_of(const char* name, jpt_level_cap_t first, jpt_level_cap_t last) {
    for (int cap = first; cap <= (int)last; cap++)
        if (!strcmp(name, jpt_custom_level_cap_name((jpt_level_cap_t)cap))) return (jpt_level_cap_t)cap;
    return JPT_LEVEL_CAPS;
}

static jpt_level_cap_t setting_cap(const char* key) { return cap_of(key, JPT_LEVEL_CAP_DUAL_STOW, JPT_LEVEL_CAP_ACTIVE_AI); }
static jpt_level_cap_t limit_cap(const char* name) { return cap_of(name, JPT_LEVEL_CAP_MESH_FACES, JPT_LEVEL_CAP_TERRAIN_CULL); }

static int limit_allowed(const char* name, unsigned value) {
    static const struct {
        jpt_level_cap_t cap;
        unsigned lowest, highest;
    } k[] = { { JPT_LEVEL_CAP_MESH_FACES, 2049, 8192 }, { JPT_LEVEL_CAP_MESH_HEAP, 33, 128 }, { JPT_LEVEL_CAP_TERRAIN_NODES, 1, 300 }, { JPT_LEVEL_CAP_TEXTURE_POOL, 129, 256 }, { JPT_LEVEL_CAP_TERRAIN_CULL, 1, 10000 } };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (limit_cap(name) == k[i].cap) return value >= k[i].lowest && value <= k[i].highest;
    return 0;
}

static int is_clean_path(const char* path) {
    if (!path[0] || strchr(path, '\\') || strchr(path, ':')) return 0;
    for (const char* c = path;; c++) {
        size_t length = strcspn(c, "/");
        if (!length || (length == 1 && c[0] == '.') || (length == 2 && c[0] == '.' && c[1] == '.')) return 0;
        c += length;
        if (!*c) return 1;
    }
}

static void map_line(const char* kind, char* rest) {
    char *a = next_field(&rest), *b = next_field(&rest), *c = next_field(&rest), *d = next_field(&rest);
    const char* file;
    jpt_level_tpa_t pack;
    jpt_compat_t compat;
    if (!strcmp(kind, "scene") && a) {
        for (int i = 0; i < C.scene_count; i++)
            if (!strcmp(C.scenes[i].file, a)) C.scenes[i].configured = 1;
    } else if (!strcmp(kind, "compat") && a && b && jpt_compat_parse(b, &compat)) {
        C.compat_known = 1;
        for (int i = 0; i < C.scene_count; i++)
            if (!strcmp(C.scenes[i].file, a)) C.scenes[i].compat = (int)compat;
    } else if (!strcmp(kind, "file") && a && b && (file = own_file(b)) != NULL && is_clean_path(a)) {
        C.mapped[C.mapped_count].target = a;
        C.mapped[C.mapped_count++].file = file;
        if (!_strnicmp(a, "data/" TEXHW, sizeof "data/" TEXHW - 1)) C.map_caps |= bit(JPT_LEVEL_CAP_TEXHW);
    } else if ((!strcmp(kind, "tpa+") || !strcmp(kind, "tpa=")) && a && b && c && tpa_kind(a, &pack) && (file = own_file(c)) != NULL) {
        if (C.tpa_count == TPA_MAX) return;
        C.tpas[C.tpa_count].kind = pack;
        C.tpas[C.tpa_count].replaces = kind[3] == '=';
        C.map_caps |= bit(kind[3] == '=' ? JPT_LEVEL_CAP_SOUND_REPLACED : JPT_LEVEL_CAP_SOUND_ADDED);
        C.tpas[C.tpa_count].scene = strcmp(b, "*") ? b : NULL;
        C.tpas[C.tpa_count++].file = file;
    } else if (!strcmp(kind, "loading") && a && b && (file = own_file(b)) != NULL) {
        if (C.loading_count == LOADING_MAX) return;
        C.loading[C.loading_count].scene = a;
        C.loading[C.loading_count++].file = file;
        C.map_caps |= bit(JPT_LEVEL_CAP_LOADING_IMAGE);
    } else if (!strcmp(kind, "set") && a && b && setting_cap(a) != JPT_LEVEL_CAPS && C.setting_count < SETTINGS_MAX) {
        C.map_caps |= bit(setting_cap(a));
        C.settings[C.setting_count][0] = a;
        C.settings[C.setting_count++][1] = b;
    } else if (!strcmp(kind, "limit") && a && b && C.limit_count < LIMITS_MAX && limit_allowed(a, (unsigned)strtoul(b, NULL, 10))) {
        C.map_caps |= bit(limit_cap(a));
        C.limits[C.limit_count].name = a;
        C.limits[C.limit_count++].value = (unsigned)strtoul(b, NULL, 10);
    } else if (!strcmp(kind, "need") && a && cap_of(a, JPT_LEVEL_CAP_FULL_RESTART, JPT_LEVEL_CAP_SKY) != JPT_LEVEL_CAPS) {
        C.map_caps |= bit(cap_of(a, JPT_LEVEL_CAP_FULL_RESTART, JPT_LEVEL_CAP_SKY));
    } else if (!strcmp(kind, "action") && a) {
        const long type = strtol(a, NULL, 10);
        if (type >= ACTION_FIRST && type <= ACTION_LAST) C.actions |= 1u << (type - ACTION_FIRST);
    } else if (!strcmp(kind, "nut") && a && (file = own_file(a)) != NULL && C.script_count < SCRIPTS_MAX) {
        C.scripts[C.script_count++] = file;
    } else if (!strcmp(kind, "script") && a && b && *b && c && d && C.script_ref_count < SCRIPT_REFS_MAX) {
        C.script_refs[C.script_ref_count].scene = a;
        C.script_refs[C.script_ref_count].class_name = b;
        C.script_refs[C.script_ref_count].object = c;
        C.script_refs[C.script_ref_count++].action = !strcmp(d, "1");
    } else if (!strcmp(kind, "uses") && a && b && (file = own_file(b)) != NULL && C.script_use_count < SCRIPT_REFS_MAX) {
        C.script_uses[C.script_use_count].scene = a;
        C.script_uses[C.script_use_count++].file = file;
    } else if (!strcmp(kind, "skip") && a && !strcmp(a, "win")) {
        C.skip_win = 1;
        C.map_caps |= bit(JPT_LEVEL_CAP_SKIP_ENDING);
    }
}

static int by_target(const void* a, const void* b) { return _stricmp(((const mapped_t*)a)->target, ((const mapped_t*)b)->target); }

/* A package without a map, or with one of another version, is read by its folders alone. */
static void read_map(void) {
    char path[JPT_PATH_MAX], head[48];
    FILE* f;
    long size = 0;
    int lines = 0;
    if (!host_path(JPT_CUSTOM_LEVEL_RUNTIME_MAP, path, sizeof path) || jpt_fs_is_link(path) || (f = fopen(path, "rb")) == NULL) return;
    if (!fseek(f, 0, SEEK_END)) size = ftell(f);
    if (size > 0 && size < MAP_LIMIT && !fseek(f, 0, SEEK_SET) && (C.map = (char*)malloc((size_t)size + 1)) != NULL) {
        size_t got = fread(C.map, 1, (size_t)size, f);
        C.map[got] = 0;
    }
    fclose(f);
    if (!C.map) return;
    for (const char* c = C.map; *c; c++) lines += *c == '\n';
    C.mapped = (mapped_t*)calloc((size_t)lines + 1, sizeof *C.mapped);
    snprintf(head, sizeof head, "trespasser-runtime %d", JPT_CUSTOM_LEVEL_RUNTIME_VERSION);
    for (char *line = C.map, *next; line && C.mapped; line = next) {
        char* space;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (line == C.map) {
            if (strcmp(line, head)) return;
            continue;
        }
        if ((space = strchr(line, ' ')) == NULL) continue;
        *space = 0;
        map_line(line, space + 1);
    }
    if (C.mapped_count) qsort(C.mapped, (size_t)C.mapped_count, sizeof *C.mapped, by_target);
}

static uint8_t* read_whole(const char* path, long limit, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* bytes = NULL;
    long length = 0;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END)) length = ftell(f);
    if (length > 0 && length <= limit && !fseek(f, 0, SEEK_SET) && (bytes = (uint8_t*)malloc((size_t)length)) != NULL &&
        fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return bytes;
}

static void note_version(int version) {
    if (version > JPT_SCENE_VERSION_OWN) C.format_caps |= bit(JPT_LEVEL_CAP_SCENE_FORMAT);
    if (version >= JPT_SCENE_VERSION_SCRIPTS) C.format_caps |= bit(JPT_LEVEL_CAP_SCRIPT_STATE);
}

static int scene_readable(const char* file) {
    char path[JPT_PATH_MAX];
    size_t size = 0;
    uint8_t* bytes = host_path(file, path, sizeof path) ? read_whole(path, SCENE_LIMIT, &size) : NULL;
    int readable;
    if (bytes && jpt_szdd_is(bytes, size)) {
        uint8_t* expanded = jpt_szdd_expand(bytes, size, &size);
        free(bytes);
        bytes = expanded;
    }
    readable = bytes && (!jpt_scene_version(bytes, size) || jpt_scene_readable(bytes, size, NULL));
    if (readable) note_version(jpt_scene_version(bytes, size));
    free(bytes);
    return readable;
}

static int refuse(char* error, size_t cap, const char* text, const char* subject) {
    snprintf(error, cap, "%s: %s", text, subject);
    jpt_custom_level_deactivate();
    return 0;
}

/* The package's own folder, or the folder its link names. 0 for a link that cannot be followed. */
static int read_link(void) {
    char file[JPT_PATH_MAX];
    size_t size = 0;
    uint8_t* bytes;
    int n = snprintf(file, sizeof file, "%s" SEP JPT_CUSTOM_LEVEL_LINK_FILE, C.records), head = 1, found = 0;
    if (n < 0 || n >= (int)sizeof file || !jpt_fs_exists(file)) {
        memcpy(C.dir, C.records, sizeof C.dir);
        return 1;
    }
    if (jpt_fs_is_link(file) || (bytes = read_whole(file, MAP_LIMIT, &size)) == NULL) return 0;
    C.link = (char*)malloc(size + 1);
    if (C.link) {
        memcpy(C.link, bytes, size);
        C.link[size] = 0;
    }
    free(bytes);
    if (!C.link) return 0;
    C.linked = 1;
    for (char *line = C.link, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (head) {
            if (strcmp(line, JPT_CUSTOM_LEVEL_LINK_HEAD)) return 0;
            head = 0;
        } else if (!strncmp(line, "path ", 5)) {
            found = jpt_fs_from_utf8(line + 5, C.dir, sizeof C.dir);
        } else if (!strncmp(line, "scope ", 6)) {
            if (!is_clean_path(line + 6) || C.scope_count == JPT_CUSTOM_LEVEL_LINK_SCOPES_MAX) return 0;
            C.scopes[C.scope_count++] = line + 6;
        }
    }
    return found && jpt_fs_is_dir(C.dir) && !jpt_fs_is_link(C.dir);
}

static int scene_index(const char* name);

static void authorize(const char* list, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    const char* end = memchr(list, 0, cap);
    for (const char* line = list; end && line < end;) {
        const char* next = memchr(line, '\n', (size_t)(end - line));
        const size_t length = (size_t)((next ? next : end) - line);
        if (length < sizeof path) {
            memcpy(path, line, length);
            path[length] = 0;
            for (int i = 0; i < C.scene_count; i++)
                if (!strcmp(C.scenes[i].file, path)) C.scenes[i].authorized = 1;
        }
        line += length + 1;
    }
}

static jpt_admit_t admit_index(int index) {
    const scene_t* s = &C.scenes[index];
    if (!C.compat_known) return JPT_ADMIT_OPEN;
    switch (s->compat) {
    case JPT_COMPAT_READY: return JPT_ADMIT_OPEN;
    case JPT_COMPAT_LIMITED: return C.experimental && s->authorized ? JPT_ADMIT_OPEN : JPT_ADMIT_NEEDS_CONSENT;
    case JPT_COMPAT_INVALID: return JPT_ADMIT_INVALID;
    default: break;
    }
    return JPT_ADMIT_UNRECORDED;
}

int jpt_custom_level_admit_start(char* line, size_t cap) {
    int covered = 0;
    const char* file;
    if (!C.active) return snprintf(line, cap, "launch blocked: no custom level is active"), 0;
    file = C.scenes[C.scene].file;
    for (int i = 0; i < C.scene_count; i++) covered += C.scenes[i].authorized && C.scenes[i].compat == JPT_COMPAT_LIMITED;
    if (!C.compat_known || C.scenes[C.scene].compat < 0) {
        snprintf(line, cap, "launch blocked: no compatibility was recorded for %s; open Custom Levels to check it again", file);
    } else if (C.scenes[C.scene].compat == JPT_COMPAT_INVALID) {
        snprintf(line, cap, "launch blocked: structural validation failed, %s is INVALID / UNSAFE", file);
    } else if (admit_index(C.scene) != JPT_ADMIT_OPEN) {
        snprintf(line, cap, "launch blocked: %s has limited compatibility and Play Anyway was not confirmed for this launch", file);
    } else {
        if (C.scenes[C.scene].compat == JPT_COMPAT_LIMITED)
            snprintf(line, cap, "experimental launch authorized: Play Anyway for %s, %d limited level(s) of the package covered", file, covered);
        else if (cap)
            line[0] = 0;
        return 1;
    }
    jpt_custom_level_deactivate();
    return 0;
}

jpt_admit_t jpt_custom_level_admits(const char* scene_name) {
    const int index = C.active && scene_name ? scene_index(scene_name) : -1;
    return index < 0 ? JPT_ADMIT_OPEN : admit_index(index);
}

const char* jpt_custom_level_compat_name(void) {
    if (!C.active || !C.compat_known || C.scenes[C.scene].compat < 0) return "unknown";
    return jpt_compat_name((jpt_compat_t)C.scenes[C.scene].compat);
}

int jpt_custom_level_experimental(void) { return C.active && C.experimental && C.scenes[C.scene].compat == JPT_COMPAT_LIMITED; }

int jpt_custom_level_activate(const char* user_root, const jpt_custom_level_request_t* request, char* error, size_t cap) {
    int n;
    jpt_custom_level_deactivate();
    if (!jpt_custom_level_id_valid(request->package))
        return refuse(error, cap, "invalid custom level package", request->package);
    n = snprintf(C.records, sizeof C.records, "%s" SEP JPT_CUSTOM_LEVELS_DIR SEP "%s", user_root, request->package);
    if (n < 0 || n >= (int)sizeof C.records || !jpt_fs_is_dir(C.records) || jpt_fs_is_link(C.records))
        return refuse(error, cap, "the custom level package is not installed", request->package);
    if (!read_link()) return refuse(error, cap, "the linked folder of the custom level is not available", request->package);
    C.files = (char**)calloc(JPT_CUSTOM_LEVEL_FILES_MAX, sizeof *C.files);
    if (!C.files) return refuse(error, cap, "out of memory for the custom level package", request->package);
    walk_package();
    if (C.overflow) return refuse(error, cap, "the custom level package has too many files", request->package);
    qsort(C.files, (size_t)C.count, sizeof *C.files, by_path);
    drop_repeats();
    name_scenes();
    for (C.scene = 0; C.scene < C.scene_count; C.scene++)
        if (!_stricmp(C.scenes[C.scene].file, request->scene)) break;
    if (C.scene == C.scene_count) return refuse(error, cap, "the custom level package has no such scene", request->scene);
    if (!scene_readable(C.scenes[C.scene].file))
        return refuse(error, cap, "unsupported level format", request->scene);
    read_map();
    C.experimental = request->experimental != 0;
    if (C.experimental) authorize(request->authorized, sizeof request->authorized);
    C.current = C.context = C.scene;
    snprintf(C.current_name, sizeof C.current_name, "%s", base_name(C.scenes[C.scene].file));
    snprintf(C.package, sizeof C.package, "%s", request->package);
    snprintf(C.layout, sizeof C.layout, "%s", request->layout);
    C.active = 1;
    s_serial++;
    return 1;
}

static int scene_index(const char* name) {
    const char *slash = strrchr(name, '\\'), *base = base_name(slash ? slash + 1 : name);
    for (int i = 0; i < C.scene_count; i++)
        if (!_stricmp(C.scenes[i].name, base)) return i;
    for (int i = 0; i < C.scene_count; i++)
        if (!_stricmp(base_name(C.scenes[i].file), base)) return i;
    return -1;
}

void jpt_custom_level_scene_loading(const char* scene) {
    const char* slash = scene ? strrchr(scene, '\\') : NULL;
    if (!C.active || !scene || !is_scene(scene)) return;
    C.scene_version = 0;
    s_serial++;
    C.current = scene_index(scene);
    if (C.current >= 0) C.context = C.current;
    snprintf(C.current_name, sizeof C.current_name, "%s", base_name(C.current >= 0 ? C.scenes[C.current].file : slash ? slash + 1 : scene));
}

int jpt_custom_level_restart(const char* scene, char* out, size_t cap) {
    const char* slash = scene ? strrchr(scene, '\\') : NULL;
    const char* name = slash ? slash + 1 : scene;
    if (!C.active || !name || !is_scene(name)) return 0;
    jpt_custom_level_scene_loading(name);
    return jpt_custom_level_find(name, out, cap);
}

uint8_t* jpt_custom_level_adapt_scene(const char* host, const uint8_t* bytes, size_t size, size_t* out_size, jpt_scene_report_t* report) {
    char own[JPT_PATH_MAX];
    memset(report, 0, sizeof *report);
    *out_size = 0;
    for (int i = 0; C.active && i < C.scene_count; i++) {
        if (!host_path(C.scenes[i].file, own, sizeof own) || strcmp(own, host)) continue;
        C.scene_version = jpt_scene_version(bytes, size);
        note_version(C.scene_version);
        return jpt_scene_adapt(bytes, size, out_size, report);
    }
    return NULL;
}

static int configured(void) { return C.scenes[C.context].configured; }

static int of_current(const char* scene) {
    return configured() && !_stricmp(C.current_name, scene);
}

int jpt_custom_level_tpa(jpt_level_tpa_t kind, int index, char* out, size_t cap) {
    for (int i = 0; jpt_custom_level_requires(JPT_LEVEL_CAP_SOUND_ADDED) && configured() && i < C.tpa_count; i++) {
        const tpa_t* t = &C.tpas[i];
        if (t->kind != kind || t->replaces || (t->scene && !of_current(t->scene))) continue;
        if (index-- == 0) return host_path(t->file, out, cap);
    }
    return 0;
}

int jpt_custom_level_setting(int index, const char** key, const char** value) {
    if (index < 0 || index >= C.setting_count || !jpt_custom_level_requires(setting_cap(C.settings[index][0]))) return 0;
    *key = C.settings[index][0];
    *value = C.settings[index][1];
    return 1;
}

unsigned jpt_custom_level_limit(const char* name) {
    for (int i = 0; jpt_custom_level_requires(limit_cap(name)) && i < C.limit_count; i++)
        if (!strcmp(C.limits[i].name, name)) return C.limits[i].value;
    return 0;
}

int jpt_custom_level_skips_ending(void) { return jpt_custom_level_requires(JPT_LEVEL_CAP_SKIP_ENDING) && C.skip_win && configured(); }

int jpt_custom_level_scene_version(void) { return C.active && C.scene_version > JPT_SCENE_VERSION_OWN ? C.scene_version : 0; }

static int ending_with(const char* name, char* out, size_t cap) {
    const size_t length = strlen(name);
    for (int i = 0; C.active && i < C.count; i++) {
        const size_t n = strlen(C.files[i]);
        if (n < length || _stricmp(C.files[i] + n - length, name) || (n > length && C.files[i][n - length - 1] != '/')) continue;
        return host_path(C.files[i], out, cap);
    }
    return 0;
}

int jpt_custom_level_hint_strings(char* out, size_t cap) { return ending_with("menu/hint_strings.txt", out, cap); }

static int existing(const char* relative, char* out, size_t cap) {
    const char* file = own_file(relative);
    return file && host_path(file, out, cap);
}

/* "EffectsTPA=x.tpa": the file that stands for the game's own, the current level's before the mission's. */
static int replaced_tpa(const char* path, char* out, size_t cap) {
    static const char* const k_names[JPT_LEVEL_TPA_KINDS] = { "stream.tpa", "effects.tpa", "ambient.tpa" };
    const char* file = NULL;
    for (int i = 0; jpt_custom_level_requires(JPT_LEVEL_CAP_SOUND_REPLACED) && configured() && i < C.tpa_count; i++) {
        const tpa_t* t = &C.tpas[i];
        if (!t->replaces || _stricmp(path, k_names[t->kind]) || (t->scene && !of_current(t->scene))) continue;
        if (t->scene || !file) file = t->file;
    }
    return file && host_path(file, out, cap);
}

static int mapped(const char* folder, const char* path, char* out, size_t cap) {
    char target[JPT_CUSTOM_LEVEL_PATH_MAX + 8];
    int lo = 0, hi = C.mapped_count - 1, n = snprintf(target, sizeof target, "%s%s", folder, path);
    if (n < 0 || n >= (int)sizeof target) return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, order = _stricmp(target, C.mapped[mid].target);
        if (!order) return host_path(C.mapped[mid].file, out, cap);
        if (order < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

/* The game asks for the original picture of a level, menu\li_<name>_0.tga. */
static int loading_picture(const char* path, char* out, size_t cap) {
    size_t n = strlen(path);
    if (_strnicmp(path, "menu/li_", 8) || n < 14 || _stricmp(path + n - 6, "_0.tga")) return 0;
    for (int i = 0; jpt_custom_level_requires(JPT_LEVEL_CAP_LOADING_IMAGE) && i < C.loading_count; i++)
        if (of_current(C.loading[i].scene)) return host_path(C.loading[i].file, out, cap);
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

/* The game takes the first file of a set that exists: an original one must not stand in for the package's. A package with the
 * directory of its textures alone uses the game's own pixels. */
static int hidden(const char* path) {
    static const char* const k_sets[][5] = { { ".spz", ".spp", ".swp", ".pid", NULL }, { ".asb", ".asa", NULL } };
    const char* name = base_name(path);
    size_t stem = (size_t)(extension(path) - name);
    for (size_t s = 0; s < sizeof k_sets / sizeof k_sets[0]; s++) {
        int member = 0;
        const int pixels = s == 0 && _stricmp(extension(path), ".pid");
        for (int k = 0; k_sets[s][k]; k++) member |= !_stricmp(extension(path), k_sets[s][k]);
        for (int i = 0; member && i < C.count; i++) {
            const char* file = base_name(C.files[i]);
            if (_strnicmp(file, name, stem) || (size_t)(extension(file) - file) != stem) continue;
            for (int k = 0; k_sets[s][k]; k++)
                if (!_stricmp(extension(file), k_sets[s][k]) && !(pixels && !strcmp(k_sets[s][k], ".pid"))) return 1;
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
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = path[i] == '\\' ? '/' : path[i];
    return is_clean_path(out);
}

int jpt_custom_level_map_path(const char* name, char* out, size_t cap) {
    size_t n = 0;
    if (!name || cap < 6) return 0;
    memcpy(out, "maps/", 5);
    n = 5;
    for (const char* c = name; *c; c++) {
        const char ch = *c == '\\' ? '/' : *c;
        if (ch == '/' && (n == 5 || out[n - 1] == '/')) continue;
        if (n + 1 >= cap) return 0;
        out[n++] = ch;
    }
    if (n > 5 && out[n - 1] == '/') n--;
    out[n] = 0;
    return n > 5 && is_clean_path(out);
}

int jpt_custom_level_map(const char* name, char* out, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    return C.active && jpt_custom_level_map_path(name, path, sizeof path) && mapped("", path, out, cap);
}

int jpt_custom_level_play_video(const char* data_relative) {
    char file[JPT_PATH_MAX];
    if (data_relative && C.active && clean(data_relative, C.video, sizeof C.video) && ending_with(C.video, file, sizeof file)) return 1;
    C.video[0] = 0;
    return 0;
}

int jpt_custom_level_find(const char* data_relative, char* out, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    const scene_t* scene;
    if (!C.active || !clean(data_relative, path, sizeof path)) return 0;
    if (C.video[0] && !_stricmp(path, C.video)) return ending_with(path, out, cap);
    if (!_strnicmp(path, "menu/", 5)) return loading_picture(path, out, cap);
    if (!strchr(path, '/') && is_scene(path)) {
        int index = scene_index(path);
        return index >= 0 && host_path(C.scenes[index].file, out, cap);
    }
    if (replaced_tpa(path, out, cap)) return 1;
    scene = &C.scenes[C.context];
    if (scene->configured && mapped("data/", path, out, cap)) return 1;
    if (beside(scene->file, path, out, cap)) return 1;
    for (int i = 0; i < C.scene_count; i++)
        if (&C.scenes[i] != scene && beside(C.scenes[i].file, path, out, cap)) return 1;
    if (!is_level_data(path)) return 0;
    return only_one_named(base_name(path), out, cap) || (hidden(path) && host_path(path, out, cap));
}

static int is_picture(const char* path) {
    static const char* const k_types[] = { ".bmp", ".tga", ".dds", ".pcx", ".png", ".jpg", ".jpeg", ".gif", ".tif" };
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++)
        if (!_stricmp(extension(path), k_types[i])) return 1;
    return 0;
}

static uint32_t le(const uint8_t* p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

int jpt_custom_level_texture_ok(const void* head, size_t head_size, uint64_t file_size) {
    const uint8_t* p = (const uint8_t*)head;
    uint32_t pixels, width, height;
    if (head_size < BMP_HEAD || p[0] != 'B' || p[1] != 'M') return 0;
    pixels = le(p + 0x0A, 4);
    width = le(p + 0x12, 4);
    height = le(p + 0x16, 4);
    if (height > 0x80000000u) height = 0u - height;
    if (!width || !height || width > TEXTURE_SIDE_MAX || height > TEXTURE_SIDE_MAX) return 0;
    if (le(p + 0x1C, 2) != 8 || le(p + 0x1E, 4) || le(p + 0x2E, 4) > 256u || pixels < BMP_HEAD) return 0;
    return (uint64_t)pixels + (uint64_t)((width + 3u) & ~3u) * height <= file_size;
}

static void put(uint8_t* p, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(value >> (8 * i));
}

uint8_t* jpt_custom_level_missing_texture(int height_map, size_t* size) {
    const uint32_t side = height_map ? TEXTURE_SIDE_MAX : MISSING_SIDE;
    uint8_t* p = (uint8_t*)calloc(1, *size = BMP_PIXELS + (size_t)side * side);
    if (!p) return NULL;
    p[0] = 'B', p[1] = 'M';
    put(p + 0x02, (uint32_t)*size, 4), put(p + 0x0A, BMP_PIXELS, 4), put(p + 0x0E, 40, 4), put(p + 0x12, side, 4), put(p + 0x16, side, 4);
    put(p + 0x1A, 1, 2), put(p + 0x1C, 8, 2), put(p + 0x22, side * side, 4), put(p + 0x2E, 256, 4);
    for (uint32_t i = 0; i < 256; i++) put(p + BMP_HEAD + i * 4, MISSING_GREY, 4);
    memset(p + BMP_PIXELS, 1, (size_t)side * side);
    return p;
}

static int is_usable_texture(const char* host) {
    uint8_t head[BMP_HEAD];
    uint64_t size = 0;
    jpt_fs_file_t* f = jpt_fs_open_read(host, &size);
    int ok = f && size >= sizeof head && jpt_fs_read_at(f, 0, head, sizeof head) && jpt_custom_level_texture_ok(head, sizeof head, size);
    if (f) jpt_fs_close(f);
    return ok;
}

/* The loader takes any name for a bitmap: a package's picture in another format is no answer. */
static int bmp_name(const char* prefix, const char* path, char* out, size_t cap) {
    const char* type = extension(path);
    int n = snprintf(out, cap, "%s%.*s.bmp", prefix, (int)(strlen(path) - strlen(type)), path);
    return n > 0 && (size_t)n < cap;
}

static int in_package(const char* relative, char* out, size_t cap) {
    if (!configured()) return 0;
    return mapped("data/", relative, out, cap) || (strchr(relative, '/') && mapped("", relative, out, cap));
}

/* path: clean, relative. */
static int texture_try(const char* path, int index, jpt_texture_try_t* t) {
    const char *name = base_name(path), *places[3];
    const int texhw = jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW);
    char own[sizeof t->relative], shared[sizeof t->relative];
    int count = 0;
    memset(t, 0, sizeof *t);
    if (texhw && _strnicmp(path, TEXHW, sizeof TEXHW - 1) && bmp_name(TEXHW, path, own, sizeof own)) places[count++] = own;
    else if (!_stricmp(extension(path), ".bmp")) places[count++] = path;
    if (texhw && name - path > SHARED_FOLDER_MIN && bmp_name(TEXHW_SHARED, name, shared, sizeof shared) && _stricmp(shared, path)) places[count++] = shared;
    if (!_stricmp(extension(path), ".bmp") && (!count || places[0] != path)) places[count++] = path;
    if (index < 0 || index >= 2 * count) return 0;
    t->in_game = index & 1;
    snprintf(t->relative, sizeof t->relative, "%s", places[index / 2]);
    if (t->in_game ? !s_game || !s_game(t->relative, t->file, sizeof t->file) : !in_package(t->relative, t->file, sizeof t->file)) t->file[0] = 0;
    else t->result = is_usable_texture(t->file) ? JPT_TEXTURE_FOUND : JPT_TEXTURE_UNUSABLE;
    return 1;
}

static int override_at(const char* prefix, const char* path, const char* suffix, char* out, size_t cap) {
    static const char* const k_types[] = { ".dds", ".tga", ".bmp" };
    const char* type = extension(path);
    char relative[JPT_CUSTOM_LEVEL_PATH_MAX + 32];
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        const int n = snprintf(relative, sizeof relative, "%s%.*s%s%s", prefix, (int)(strlen(path) - strlen(type)), path, suffix, k_types[i]);
        if (n < 0 || n >= (int)sizeof relative) return 0;
        if (in_package(relative, out, cap) || (s_game && s_game(relative, out, cap))) return 1;
    }
    return 0;
}

int jpt_custom_level_texhw(const char* texture, const char* suffix, char* out, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) || !clean(texture, path, sizeof path)) return 0;
    return override_at(TEXHW, path, suffix, out, cap) || override_at(TEXHW_SHARED, base_name(path), suffix, out, cap);
}

static int texture(const char* path, char* out, size_t cap) {
    jpt_texture_try_t t;
    for (int i = 0; texture_try(path, i, &t); i++) {
        if (t.result != JPT_TEXTURE_FOUND) continue;
        snprintf(out, cap, "%s", t.file);
        return strlen(t.file) < cap;
    }
    return 0;
}

int jpt_custom_level_find_in_game(const char* game_relative, char* out, size_t cap) {
    char path[JPT_CUSTOM_LEVEL_PATH_MAX];
    if (!C.active || !clean(game_relative, path, sizeof path)) return 0;
    if (!_strnicmp(path, "data/", 5)) return jpt_custom_level_find(path + 5, out, cap);
    if (is_picture(path)) return texture(path, out, cap);
    return strchr(path, '/') && in_package(path, out, cap);
}

static int separator(char c) { return c == '/' || c == '\\'; }

/* Without a drive or a share; the separator in front of it is the loader's. */
static const char* driveless(const char* path) {
    if (!path || !path[0] || strchr(path, ':') || (separator(path[0]) && separator(path[1]))) return NULL;
    return separator(path[0]) ? path + 1 : path;
}

int jpt_custom_level_find_relative(const char* path, char* out, size_t cap) {
    const char* relative = C.active ? driveless(path) : NULL;
    if (!relative || (!_strnicmp(relative, "data", 4) && separator(relative[4]))) return 0;
    return jpt_custom_level_find_in_game(relative, out, cap);
}

void jpt_custom_level_set_game(jpt_custom_level_game_fn find) { s_game = find; }
int jpt_custom_level_game_file(const char* relative, char* out, size_t cap) { return s_game && s_game(relative, out, cap); }
int jpt_custom_level_has_game(void) { return s_game != NULL; }

static int texture_request(const char* path, char* name, size_t cap) {
    const char* relative = C.active ? driveless(path) : NULL;
    return relative && clean(relative, name, cap) && is_picture(name);
}

int jpt_custom_level_is_texture_request(const char* path) {
    char name[JPT_CUSTOM_LEVEL_PATH_MAX];
    return texture_request(path, name, sizeof name);
}

int jpt_custom_level_texture_try(const char* path, int index, jpt_texture_try_t* out) {
    char name[JPT_CUSTOM_LEVEL_PATH_MAX];
    return texture_request(path, name, sizeof name) && texture_try(name, index, out);
}

int jpt_custom_level_texture_other(const char* path, char* out, size_t cap) {
    char name[JPT_CUSTOM_LEVEL_PATH_MAX];
    size_t stem;
    if (!texture_request(path, name, sizeof name)) return 0;
    stem = (size_t)(extension(name) - base_name(name));
    for (int i = 0; i < C.count; i++) {
        const char* file = base_name(C.files[i]);
        if ((size_t)(extension(file) - file) != stem || _strnicmp(file, base_name(name), stem) || !is_picture(file)) continue;
        snprintf(out, cap, "%s", C.files[i]);
        return 1;
    }
    return 0;
}

unsigned jpt_custom_level_serial(void) { return s_serial; }

/* A file of the active mod as the game would find it: beside the scene, in the data folder, in the mod's own. */
static int mod_file(const char* relative, char* out, size_t cap) {
    const char* scene = C.scenes[C.context].file;
    char path[JPT_PATH_MAX];
    int folder = (int)(base_name(scene) - scene), n;
    if (beside(scene, relative, out, cap)) return 1;
    if (configured() && (mapped("data/", relative, out, cap) || mapped("", relative, out, cap))) return 1;
    for (folder--; folder > 0 && scene[folder - 1] != '/'; folder--) {}
    if (folder < 0) return 0;
    n = snprintf(path, sizeof path, "%.*s%s", folder, scene, relative);
    return n > 0 && n < (int)sizeof path && existing(path, out, cap);
}

int jpt_custom_level_sky(char* out, size_t cap) {
    char name[JPT_CUSTOM_LEVEL_PATH_MAX];
    const char* type = extension(C.current_name);
    int n;
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_SKY) || !C.current_name[0]) return 0;
    n = snprintf(name, sizeof name, "%.*s.sky", (int)(strlen(C.current_name) - strlen(type)), C.current_name);
    return n > 0 && n < (int)sizeof name && mod_file(name, out, cap);
}

int jpt_custom_level_sky_texture(const char* name, char* out, size_t cap) {
    static const char* const k_types[] = { ".dds", ".tga", ".bmp" };
    char path[JPT_CUSTOM_LEVEL_PATH_MAX], file[JPT_CUSTOM_LEVEL_PATH_MAX + 8];
    const char* relative = path;
    size_t stem;
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_SKY) || !clean(name, path, sizeof path)) return 0;
    if (!_strnicmp(relative, "data/", 5)) relative += 5;
    stem = strlen(relative) - (is_picture(relative) ? strlen(extension(relative)) : 0);
    for (int game = 0; game < 2; game++)
        for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
            snprintf(file, sizeof file, "%.*s%s", (int)stem, relative, k_types[i]);
            if (game ? s_game && s_game(file, out, cap) : mod_file(file, out, cap)) return 1;
        }
    return 0;
}
