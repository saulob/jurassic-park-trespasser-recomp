#include "jpt_texhw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_custom_level.h"

#define NAMES_MAX 16384
#define SIDE_MAX 2048u
#define BUDGET ((size_t)256 << 20)
#define FRAME_BYTES ((size_t)16 << 20)
#define LOG_LIMIT 16

typedef struct {
    uint32_t hash;
    int id, normal;
} name_t;

typedef struct {
    char* path;
    uint8_t failed[JPT_TEXHW_CLASSES], counted[JPT_TEXHW_CLASSES];
    uint32_t side; /* the largest one that fits the budget */
} file_t;

static struct {
    name_t* names;
    int name_count, name_cap, sorted;
    file_t* files;
    int file_count, logged, normals, fresh;
    size_t used, in_frame;
} T;
static unsigned s_generation = 1;
static jpt_texhw_log_fn s_log;

void jpt_texhw_set_log(jpt_texhw_log_fn log) { s_log = log; }

static void say(const char* what, const char* path) {
    char line[1200];
    if (!s_log || T.logged >= LOG_LIMIT) return;
    T.logged++;
    snprintf(line, sizeof line, "%s%s%s", what, path, T.logged == LOG_LIMIT ? " (later ones are not logged)" : "");
    s_log(line);
}

void jpt_texhw_reset(void) {
    for (int i = 0; i < T.file_count; i++) free(T.files[i].path);
    free(T.files);
    free(T.names);
    memset(&T, 0, sizeof T);
    s_generation++;
}

unsigned jpt_texhw_generation(void) { return s_generation; }
int jpt_texhw_active(void) { return T.name_count != 0; }

static int id_of(const char* path) {
    file_t* f;
    for (int i = 0; i < T.file_count; i++)
        if (!strcmp(T.files[i].path, path)) return i + 1;
    if (T.file_count == JPT_TEXHW_FILES_MAX) return 0;
    if (!T.files && (T.files = (file_t*)calloc(JPT_TEXHW_FILES_MAX, sizeof *T.files)) == NULL) return 0;
    f = &T.files[T.file_count];
    f->path = (char*)malloc(strlen(path) + 1);
    if (!f->path) return 0;
    strcpy(f->path, path);
    f->side = SIDE_MAX;
    return ++T.file_count;
}

void jpt_texhw_name(uint32_t hash, const char* texture) {
    char path[1024], normal[1024];
    int id;
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW)) return;
    jpt_custom_level_on_end(jpt_texhw_reset);
    if (!jpt_custom_level_texhw(texture, "", path, sizeof path) || (id = id_of(path)) == 0) return;
    if (T.name_count == T.name_cap) {
        const int cap = T.name_cap ? T.name_cap * 2 : 256;
        name_t* grown = cap <= NAMES_MAX ? (name_t*)realloc(T.names, (size_t)cap * sizeof *grown) : NULL;
        if (!grown) return;
        T.names = grown;
        T.name_cap = cap;
    }
    T.names[T.name_count].hash = hash;
    T.names[T.name_count].normal = jpt_custom_level_texhw(texture, ".norm", normal, sizeof normal) ? id_of(normal) : 0;
    T.normals += T.names[T.name_count].normal != 0;
    T.names[T.name_count++].id = id;
    T.sorted = 0;
    T.fresh = 1;
}

static int by_hash(const void* a, const void* b) {
    const uint32_t x = ((const name_t*)a)->hash, y = ((const name_t*)b)->hash;
    return x < y ? -1 : x > y;
}

static const name_t* name_of(uint32_t hash) {
    int lo = 0, hi = T.name_count - 1;
    if (!T.sorted && T.name_count) {
        qsort(T.names, (size_t)T.name_count, sizeof *T.names, by_hash);
        T.sorted = 1;
    }
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (T.names[mid].hash == hash) return &T.names[mid];
        if (T.names[mid].hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int jpt_texhw_find(uint32_t hash) {
    const name_t* n = name_of(hash);
    return n ? n->id : 0;
}

int jpt_texhw_normal(uint32_t hash) {
    const name_t* n = name_of(hash);
    return n ? n->normal : 0;
}

int jpt_texhw_normals(void) { return T.normals != 0; }

/* Worlds were read since the pictures in use were loaded: those that are still drawn load again. */
static void start_over(void) {
    for (int i = 0; i < T.file_count; i++) {
        memset(T.files[i].counted, 0, sizeof T.files[i].counted);
        T.files[i].side = SIDE_MAX;
    }
    T.used = 0;
    T.fresh = 0;
    s_generation++;
}

static void fit(file_t* f, jpt_texhw_class_t as, jpt_picture_t* p) {
    if (f->counted[as]) {
        jpt_picture_limit(p, f->side);
        return;
    }
    if (T.used + p->size > BUDGET && T.used && T.fresh) start_over();
    while (f->side > 64u && T.used + p->size > BUDGET) {
        f->side /= 2;
        jpt_picture_limit(p, f->side);
    }
    jpt_picture_limit(p, f->side);
}

void jpt_texhw_frame(void) { T.in_frame = 0; }

int jpt_texhw_picture(int id, jpt_texhw_class_t as, jpt_picture_t* out) {
    file_t* f = id >= 1 && id <= T.file_count ? &T.files[id - 1] : NULL;
    memset(out, 0, sizeof *out);
    if (!f || f->failed[as]) return 0;
    if (T.in_frame >= FRAME_BYTES) return -1;
    if (!jpt_picture_load(f->path, out)) {
        say("texhw: the picture cannot be read and the level's own texture stays: ", f->path);
        memset(f->failed, 1, sizeof f->failed);
        return 0;
    }
    if (as == JPT_TEXHW_OPAQUE)
        for (size_t i = 3; i < out->size; i += 4) out->bgra[i] = 255;
    T.in_frame += out->size;
    if (jpt_picture_complete(out, as == JPT_TEXHW_CUTOUT)) fit(f, as, out);
    if (!f->counted[as] && T.used + out->size > BUDGET) {
        say("texhw: no room is left for ", f->path);
        f->failed[as] = 1;
        jpt_picture_free(out);
        return 0;
    }
    if (!f->counted[as]) T.used += out->size;
    f->counted[as] = 1;
    return 1;
}
