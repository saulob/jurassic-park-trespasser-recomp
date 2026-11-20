#include "linux_modules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_ptr.h"
#include "jpt_fs.h"
#include "jpt_hash.h"
#include "linux_hle.h"
#include "linux_log.h"

#define MAX_MODULES 8
#define DLL_PROCESS_DETACH 0u
#define DLL_PROCESS_ATTACH 1u

static lx_module_t s_modules[MAX_MODULES];
static size_t s_count;

void lx_modules_reset(void) {
    memset(s_modules, 0, sizeof s_modules);
    s_count = 0;
}

size_t lx_module_count(void) { return s_count; }
lx_module_t* lx_module_at(size_t index) { return index < s_count ? &s_modules[index] : NULL; }

static void base_name(const char* path, char* out, size_t cap) {
    const char* start = path;
    size_t n = 0;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/') start = p + 1;
    for (; start[n] && n + 1 < cap; n++) out[n] = (char)((start[n] >= 'A' && start[n] <= 'Z') ? start[n] + 32 : start[n]);
    out[n] = 0;
    if (n > 4 && !strcmp(out + n - 4, ".dll")) out[n - 4] = 0;
}

lx_module_t* lx_module_find(const char* name_or_path) {
    char name[32];
    if (!name_or_path) return NULL;
    base_name(name_or_path, name, sizeof name);
    for (size_t i = 0; i < s_count; i++)
        if (!strcmp(s_modules[i].name, name)) return &s_modules[i];
    return NULL;
}

lx_module_t* lx_module_by_base(uint32_t base) {
    for (size_t i = 0; i < s_count; i++)
        if (s_modules[i].image.base == base) return &s_modules[i];
    return NULL;
}

static int read_installed(jpt_vpath_t* vp, const char* version, const char* file, uint8_t** data, size_t* size) {
    char guest[JPT_PATH_MAX], host[JPT_PATH_MAX];
    FILE* f;
    long n;
    snprintf(guest, sizeof guest, "C:\\versions\\%s\\%s", version, file);
    if (jpt_vpath_resolve(vp, guest, 0, host, sizeof host) != JPT_VP_OK) {
        snprintf(guest, sizeof guest, "C:\\%s", file);
        if (jpt_vpath_resolve(vp, guest, 0, host, sizeof host) != JPT_VP_OK) return 0;
    }
    f = fopen(host, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    *data = (uint8_t*)malloc((size_t)n);
    if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
        free(*data);
        fclose(f);
        return 0;
    }
    fclose(f);
    *size = (size_t)n;
    return 1;
}

int lx_modules_map(const lx_module_def_t* defs, size_t count, jpt_vpath_t* vpath, const char* version, char* err, size_t err_len) {
    for (size_t i = 0; i < count; i++) {
        const jpt_module_meta_t* meta = defs[i].meta;
        uint8_t* bytes = NULL;
        size_t size = 0;
        char hash[65], reason[128];
        lx_module_t* m;
        if (s_count == MAX_MODULES) return snprintf(err, err_len, "too many modules"), 0;
        if (!read_installed(vpath, version ? version : "en-116", meta->file_name, &bytes, &size))
            return snprintf(err, err_len, "%s is not in the game folder", meta->file_name), 0;
        jpt_sha256_buffer_hex(bytes, size, hash);
        if (strcmp(hash, meta->sha256) != 0) {
            free(bytes);
            return snprintf(err, err_len, "%s is not the DLL this port was lifted from", meta->file_name), 0;
        }
        m = &s_modules[s_count];
        memset(m, 0, sizeof *m);
        if (gm_pe_map_memory(bytes, size, GM_PE_PREFERRED, 0, &m->image, reason, sizeof reason) != 0) {
            free(bytes);
            return snprintf(err, err_len, "%s cannot be mapped at 0x%08X: %s", meta->file_name, meta->image_base, reason), 0;
        }
        free(bytes);
        if (m->image.base != meta->image_base || m->image.span < meta->image_size - 0xFFF ||
            m->image.entry != meta->entry_point)
            return snprintf(err, err_len, "%s does not sit where the lift expects", meta->file_name), 0;
        snprintf(m->name, sizeof m->name, "%s", meta->name);
        m->meta = meta;
        m->table = defs[i].table;
        m->refcount = 1;
        s_count++;
    }
    return 1;
}

static uint32_t bind_module_import(const char* dll, const char* name, uint16_t ordinal, void* user) {
    unsigned* counts = (unsigned*)user;
    counts[0]++;
    if (name && hle_find(dll, name)) counts[1]++;
    return hle_resolve(dll, name, ordinal, NULL);
}

int lx_modules_bind(unsigned* bound, unsigned* implemented) {
    unsigned counts[2] = { 0, 0 };
    int unresolved = 0;
    for (size_t i = 0; i < s_count; i++) {
        unresolved += gm_pe_bind(&s_modules[i].image, bind_module_import, counts, NULL);
        lx_spine_add_table(s_modules[i].table);
    }
    if (bound) *bound = counts[0];
    if (implemented) *implemented = counts[1];
    return unresolved == 0;
}

uint32_t lx_module_export(const lx_module_t* m, const char* name, uint16_t ordinal) {
    return gm_pe_export(&m->image, name, name ? 0 : ordinal);
}

int lx_module_resolve(const char* dll, const char* name, uint16_t ordinal, uint32_t* va) {
    lx_module_t* m = lx_module_find(dll);
    if (!m) return 0;
    *va = lx_module_export(m, name, ordinal);
    return 1;
}

static int dll_main(lx_module_t* m, uint32_t reason) {
    uint32_t args[3] = { m->image.base, reason, 0 };
    if (!m->image.entry) return 1;
    if (reason == DLL_PROCESS_ATTACH) m->attach_count++;
    else m->detach_count++;
    return lx_call_guest(m->image.entry, LX_STDCALL, 3, args, 0, NULL) != 0;
}

uint32_t lx_module_load(lx_module_t* m) {
    if (m->refcount++ == 0 && !dll_main(m, DLL_PROCESS_ATTACH)) {
        m->refcount = 0;
        return 0;
    }
    return m->image.base;
}

int lx_module_free(lx_module_t* m) {
    if (m->refcount <= 0) return 0;
    if (--m->refcount == 0) dll_main(m, DLL_PROCESS_DETACH);
    return 1;
}

int lx_modules_attach_imported(void) {
    for (size_t i = 0; i < s_count; i++)
        if (s_modules[i].attach_count == 0 && !dll_main(&s_modules[i], DLL_PROCESS_ATTACH)) {
            lx_log("modules", "%s: DllMain(DLL_PROCESS_ATTACH) failed", s_modules[i].name);
            return 0;
        }
    return 1;
}
