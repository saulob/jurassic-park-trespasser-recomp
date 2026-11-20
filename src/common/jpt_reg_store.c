#include "jpt_reg_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_fs.h"

#define MAX_VALUES 160
#define MAX_NAME 64
#define MAX_LINE 8192

typedef struct {
    char name[MAX_NAME];
    uint32_t type, size;
    unsigned char* data;
} value_t;

struct jpt_reg_store {
    value_t values[MAX_VALUES];
    int count;
};

jpt_reg_store_t* jpt_reg_new(void) { return (jpt_reg_store_t*)calloc(1, sizeof(jpt_reg_store_t)); }

void jpt_reg_free(jpt_reg_store_t* r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) free(r->values[i].data);
    free(r);
}

int jpt_reg_count(const jpt_reg_store_t* r) {
    int n = 0;
    for (int i = 0; i < r->count; i++) n += r->values[i].data != NULL;
    return n;
}

static value_t* find(const jpt_reg_store_t* r, const char* name) {
    for (int i = 0; i < r->count; i++)
        if (!_stricmp(r->values[i].name, name)) return (value_t*)&r->values[i];
    return NULL;
}

int jpt_reg_set(jpt_reg_store_t* r, const char* name, uint32_t type, const void* data, uint32_t size) {
    value_t* v = find(r, name);
    unsigned char* copy;
    if (!v) {
        if (r->count == MAX_VALUES || strlen(name) >= MAX_NAME) return 0;
        v = &r->values[r->count++];
        memset(v, 0, sizeof *v);
        strcpy(v->name, name);
    }
    copy = (unsigned char*)malloc(size ? size : 1);
    if (!copy) return 0;
    if (size) memcpy(copy, data, size);
    free(v->data);
    v->data = copy;
    v->type = type;
    v->size = size;
    return 1;
}

int jpt_reg_set_dword(jpt_reg_store_t* r, const char* name, uint32_t value) {
    unsigned char bytes[4] = { (unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16),
                               (unsigned char)(value >> 24) };
    return jpt_reg_set(r, name, JPT_REG_DWORD, bytes, 4);
}

int jpt_reg_set_string(jpt_reg_store_t* r, const char* name, const char* text) {
    return jpt_reg_set(r, name, JPT_REG_SZ, text, (uint32_t)strlen(text) + 1);
}

const void* jpt_reg_get(const jpt_reg_store_t* r, const char* name, uint32_t* type, uint32_t* size) {
    const value_t* v = find(r, name);
    if (!v || !v->data) return NULL;
    if (type) *type = v->type;
    if (size) *size = v->size;
    return v->data;
}

int jpt_reg_get_dword(const jpt_reg_store_t* r, const char* name, uint32_t* value) {
    uint32_t type, size;
    const unsigned char* d = (const unsigned char*)jpt_reg_get(r, name, &type, &size);
    if (!d || type != JPT_REG_DWORD || size != 4) return 0;
    *value = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    return 1;
}

int jpt_reg_delete(jpt_reg_store_t* r, const char* name) {
    value_t* v = find(r, name);
    if (!v || !v->data) return 0;
    free(v->data);
    v->data = NULL;
    v->size = 0;
    return 1;
}

static int parse_hex(const char* text, unsigned char* out, uint32_t* size) {
    uint32_t n = 0;
    unsigned byte;
    while (text[0] && text[1] && sscanf(text, "%2x", &byte) == 1) {
        out[n++] = (unsigned char)byte;
        text += 2;
    }
    *size = n;
    return *text == 0;
}

static int load_line(jpt_reg_store_t* r, char* line) {
    static unsigned char bytes[MAX_LINE / 2];
    char* eq = strchr(line, '=');
    uint32_t size;
    unsigned type;
    if (!eq) return 0;
    *eq++ = 0;
    if (!strncmp(eq, "dword:", 6)) {
        char* end;
        unsigned long v = strtoul(eq + 6, &end, 16);
        return jpt_reg_set_dword(r, line, (uint32_t)v) && end != eq + 6;
    }
    if (!strncmp(eq, "sz:", 3)) return jpt_reg_set_string(r, line, eq + 3);
    if (sscanf(eq, "hex(%u):", &type) == 1 && (eq = strchr(eq, ':')) != NULL && parse_hex(eq + 1, bytes, &size))
        return jpt_reg_set(r, line, type, bytes, size);
    return 0;
}

int jpt_reg_load(jpt_reg_store_t* r, const char* path, int* ignored) {
    static char line[MAX_LINE];
    FILE* f = fopen(path, "r");
    int read = 0;
    if (ignored) *ignored = 0;
    if (!f) return jpt_fs_exists(path) ? -1 : 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        if (load_line(r, line)) read++;
        else if (ignored) (*ignored)++;
    }
    fclose(f);
    return read;
}

int jpt_reg_save(const jpt_reg_store_t* r, const char* path) {
    char tmp[JPT_PATH_MAX + 8];
    FILE* f;
    int ok = 1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    f = fopen(tmp, "w");
    if (!f) return 0;
    for (int i = 0; i < r->count && ok; i++) {
        const value_t* v = &r->values[i];
        if (!v->data) continue;
        if (v->type == JPT_REG_DWORD && v->size == 4) {
            uint32_t d = (uint32_t)v->data[0] | (uint32_t)v->data[1] << 8 | (uint32_t)v->data[2] << 16 |
                         (uint32_t)v->data[3] << 24;
            ok = fprintf(f, "%s=dword:%08" PRIX32 "\n", v->name, d) > 0;
        } else if (v->type == JPT_REG_SZ && v->size && v->data[v->size - 1] == 0 &&
                   strlen((const char*)v->data) == v->size - 1 && !strpbrk((const char*)v->data, "\r\n")) {
            ok = fprintf(f, "%s=sz:%s\n", v->name, (const char*)v->data) > 0;
        } else {
            ok = fprintf(f, "%s=hex(%" PRIu32 "):", v->name, v->type) > 0;
            for (uint32_t k = 0; ok && k < v->size; k++) ok = fprintf(f, "%02x", v->data[k]) > 0;
            ok = ok && fputc('\n', f) != EOF;
        }
    }
    ok = fclose(f) == 0 && ok;
    ok = ok && jpt_fs_replace(tmp, path);
    if (!ok) jpt_fs_remove(tmp);
    return ok;
}
