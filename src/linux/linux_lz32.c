#include "linux_lz32.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "guest_features.h"
#include "guest_ptr.h"
#include "jpt_fs.h"
#include "jpt_level_gate.h"
#include "jpt_level_world.h"
#include "jpt_scene.h"
#include "jpt_szdd.h"
#include "jpt_texhw.h"
#include "linux_log.h"

#define MAX_FILES 16
#define FIRST_HANDLE 0x400
#define HFILE_ERROR 0xFFFFFFFFu
#define OFSTRUCT_BYTES 136
#define OFSTRUCT_PATH 8
#define OFSTRUCT_PATH_MAX 128

typedef struct {
    int used;
    uint8_t* data;
    size_t size, pos;
} lz_file_t;

static lz_file_t s_file[MAX_FILES];

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (uint8_t*)malloc((size_t)n ? (size_t)n : 1);
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) free(data), data = NULL;
        *size = (size_t)n;
    }
    fclose(f);
    return data;
}

static lz_file_t* file_of(uint32_t handle) {
    uint32_t slot = handle - FIRST_HANDLE;
    return slot < MAX_FILES && s_file[slot].used ? &s_file[slot] : NULL;
}

void hle_lz32_shutdown(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        free(s_file[i].data);
        memset(&s_file[i], 0, sizeof s_file[i]);
    }
}

unsigned lx_lz32_open_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_FILES; i++) n += (unsigned)s_file[i].used;
    return n;
}

typedef struct {
    const uint8_t* data;
    size_t size;
} expanded_view_t;

static uint8_t* expanded_copy(void* context, size_t expanded_size) {
    const expanded_view_t* view = (const expanded_view_t*)context;
    uint8_t* copy;
    if (expanded_size != view->size) return NULL;
    copy = (uint8_t*)malloc(expanded_size ? expanded_size : 1);
    if (copy) memcpy(copy, view->data, expanded_size);
    return copy;
}

/* The retail level files of the official Patch 1.1 are served fixed: the same bytes the Windows host serves through its LZ calls. */
static void serve_fixed_level(const char* host_path, const uint8_t* raw, size_t raw_size, uint8_t** data, size_t* size) {
    expanded_view_t view;
    size_t fixed_size = 0;
    uint8_t* fixed;
    if (!patch117_file_wanted(host_path)) return;
    view.data = *data, view.size = *size;
    fixed = patch117_level_bytes(host_path, raw, raw_size, expanded_copy, &view, &fixed_size);
    if (!fixed) return;
    if (*data != raw) free(*data);
    *data = fixed, *size = fixed_size;
}

/* A package's scene of a later format, as the game's own loader reads it; the file stays as it is. 0 when it cannot be. */
static int adapt_scene(const char* host_path, const uint8_t* raw, uint8_t** data, size_t* size) {
    jpt_scene_report_t report;
    size_t adapted_size = 0;
    uint8_t* adapted;
    adapted = jpt_custom_level_adapt_scene(host_path, *data, *size, &adapted_size, &report);
    if (report.problem[0]) {
        lx_log("custom-level", "%s cannot be read: %s", host_path, report.problem);
        return 0;
    }
    if (!adapted) return 1;
    lx_log("custom-level", "%s: scene version %d read as the game's own layout (%zu -> %zu bytes, in memory)", host_path, report.version, *size, adapted_size);
    for (int i = 0; i < report.note_count; i++) lx_log("custom-level", "  %s", report.notes[i]);
    if (*data != raw) free(*data);
    *data = adapted, *size = adapted_size;
    return 1;
}

static void texhw_line(const char* line) { lx_log("custom-level", "%s", line); }

static void adapt_world(const char* name, uint8_t* data, size_t size) {
    jpt_level_world_t world;
    char line[512];
    jpt_texhw_set_log(texhw_line);
    jpt_level_world_prepare(name, data, size, &world);
    for (int i = 0; jpt_level_world_line(&world, i, line, sizeof line); i++) lx_log("custom-level", "%s", line);
    if (world.failed) hle_exit_process(HLE_EXIT_LEVEL);
}

static int adapt(const char* name, const char* host_path, const uint8_t* raw, uint8_t** data, size_t* size) {
    const size_t n = strlen(host_path);
    if (n > 4 && !strcasecmp(host_path + n - 4, ".scn")) return adapt_scene(host_path, raw, data, size);
    if (n > 4 && !strcasecmp(host_path + n - 4, ".grf")) adapt_world(name, *data, *size);
    return 1;
}

static uint64_t z_LZOpenFileA(const uint32_t* a, void* u) {
    const char* name = gp_string(a[0], 260);
    char host[JPT_PATH_MAX];
    uint8_t* raw;
    uint8_t* data;
    size_t size = 0, expanded = 0;
    uint8_t info[OFSTRUCT_BYTES];
    int slot = -1, overlaid = 0, readable;
    (void)u;
    if (!name || !hle_vpath() || jpt_vpath_resolve_read(hle_vpath(), name, host, sizeof host, &overlaid) != JPT_VP_OK)
        return HFILE_ERROR;
    for (int i = 0; i < MAX_FILES && slot < 0; i++)
        if (!s_file[i].used) slot = i;
    if (jpt_save_is_name(name) || jpt_save_is_temporary(name)) {
        char why[160];
        if (!jpt_gate_reads(host, why, sizeof why)) return lx_log("save", "%s is damaged and was not read: %s", name, why), HFILE_ERROR;
    }
    raw = slot < 0 ? NULL : read_file(host, &size);
    if (!raw) return HFILE_ERROR;
    data = jpt_szdd_expand(raw, size, &expanded);
    if (!data) data = raw, expanded = size;
    serve_fixed_level(host, raw, size, &data, &expanded);
    if (!overlaid && strlen(host) > 4 && !strcasecmp(host + strlen(host) - 4, ".grf")) jpt_level_world_original(host);
    readable = !overlaid || adapt(name, host, raw, &data, &expanded);
    if (data != raw) free(raw);
    if (!readable) {
        free(data);
        return HFILE_ERROR;
    }
    s_file[slot].used = 1, s_file[slot].data = data, s_file[slot].size = expanded, s_file[slot].pos = 0;
    if (a[1]) {
        memset(info, 0, sizeof info);
        info[0] = OFSTRUCT_BYTES, info[1] = 1;
        snprintf((char*)info + OFSTRUCT_PATH, OFSTRUCT_PATH_MAX, "%s", name);
        gp_copy_out(a[1], info, sizeof info);
    }
    return FIRST_HANDLE + (uint32_t)slot;
}

static uint64_t z_LZClose(const uint32_t* a, void* u) {
    lz_file_t* f = file_of(a[0]);
    (void)u;
    if (!f) return 0;
    free(f->data);
    memset(f, 0, sizeof *f);
    return 0;
}

static uint64_t z_LZRead(const uint32_t* a, void* u) {
    lz_file_t* f = file_of(a[0]);
    size_t n;
    void* dst;
    (void)u;
    if (!f || (int32_t)a[2] < 0) return (uint32_t)-1;
    n = f->size - f->pos < a[2] ? f->size - f->pos : a[2];
    if (!n) return 0;
    dst = gm_ptr_checked(a[1], (uint32_t)n, 1);
    if (!dst) return (uint32_t)-1;
    memcpy(dst, f->data + f->pos, n);
    f->pos += n;
    return (uint32_t)n;
}

static uint64_t z_LZSeek(const uint32_t* a, void* u) {
    lz_file_t* f = file_of(a[0]);
    int64_t base, target;
    (void)u;
    if (!f || a[2] > 2) return (uint32_t)-1;
    base = a[2] == 0 ? 0 : a[2] == 1 ? (int64_t)f->pos : (int64_t)f->size;
    target = base + (int32_t)a[1];
    if (target < 0) return (uint32_t)-1;
    f->pos = (size_t)target > f->size ? f->size : (size_t)target;
    return (uint32_t)f->pos;
}

#define LZ(fn, argc) { "lz32.dll", #fn, z_##fn, argc, HT_STDCALL }

static const hle_entry_t k_table[] = { LZ(LZOpenFileA, 3), LZ(LZClose, 1), LZ(LZRead, 3), LZ(LZSeek, 3) };

const hle_entry_t* hle_lz32_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
