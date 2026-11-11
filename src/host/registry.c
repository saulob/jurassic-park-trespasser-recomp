/*
 * The game keeps its install paths and settings under one HKLM key that only its installer
 * creates. That key is served from a text file in the game directory, so nothing is written
 * to the real registry. Every other key is forwarded read-only.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_KEY "Software\\DreamWorks Interactive\\Trespasser"
#define STORE_FILE "recomp-registry.txt"
#define MAX_VALUES 160
#define MAX_NAME 64
#define MAX_LINE 8192

typedef struct {
    char name[MAX_NAME];
    DWORD type, size;
    unsigned char* data;
    int reported;
} reg_value_t;

static reg_value_t s_values[MAX_VALUES];
static int s_count;
static char s_store_path[MAX_PATH];
static char s_game_key_token;

#define GAME_HKEY ((uint32_t)(uintptr_t)&s_game_key_token)

static reg_value_t* find_value(const char* name) {
    for (int i = 0; i < s_count; i++)
        if (!_stricmp(s_values[i].name, name)) return &s_values[i];
    return NULL;
}

static reg_value_t* slot_for(const char* name) {
    reg_value_t* v = find_value(name);
    if (v || s_count == MAX_VALUES || strlen(name) >= MAX_NAME) return v;
    v = &s_values[s_count++];
    memset(v, 0, sizeof *v);
    strcpy(v->name, name);
    return v;
}

static int set_data(reg_value_t* v, DWORD type, const void* data, DWORD size) {
    unsigned char* copy = (unsigned char*)malloc(size ? size : 1);
    if (!copy) return 0;
    if (size) memcpy(copy, data, size);
    free(v->data);
    v->data = copy;
    v->type = type;
    v->size = size;
    return 1;
}

static int set_value(const char* name, DWORD type, const void* data, DWORD size) {
    reg_value_t* v = slot_for(name);
    return v && set_data(v, type, data, size);
}

static void set_string(const char* name, const char* text) {
    set_value(name, REG_SZ, text, (DWORD)strlen(text) + 1);
}

static void save_store(void) {
    FILE* f = fopen(s_store_path, "w");
    if (!f) {
        host_log("registry", "cannot write %s", s_store_path);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        const reg_value_t* v = &s_values[i];
        if (!v->data) continue;
        if (v->type == REG_DWORD && v->size == 4) {
            fprintf(f, "%s=dword:%08lX\n", v->name, *(DWORD*)v->data);
        } else if (v->type == REG_SZ && v->size && v->data[v->size - 1] == 0 &&
                   strlen((const char*)v->data) == v->size - 1 && !strpbrk((const char*)v->data, "\r\n")) {
            fprintf(f, "%s=sz:%s\n", v->name, (const char*)v->data);
        } else {
            fprintf(f, "%s=hex(%lu):", v->name, v->type);
            for (DWORD k = 0; k < v->size; k++) fprintf(f, "%02x", v->data[k]);
            fputc('\n', f);
        }
    }
    fclose(f);
}

static int parse_hex(const char* text, unsigned char* out, DWORD* size) {
    DWORD n = 0;
    unsigned byte;
    while (text[0] && text[1] && sscanf(text, "%2x", &byte) == 1) {
        out[n++] = (unsigned char)byte;
        text += 2;
    }
    *size = n;
    return *text == 0;
}

static void load_line(char* line) {
    static unsigned char bytes[MAX_LINE / 2];
    char* eq = strchr(line, '=');
    DWORD type, size;
    if (!eq) return;
    *eq++ = 0;
    if (!strncmp(eq, "dword:", 6)) {
        DWORD v = strtoul(eq + 6, NULL, 16);
        set_value(line, REG_DWORD, &v, 4);
    } else if (!strncmp(eq, "sz:", 3)) {
        set_string(line, eq + 3);
    } else if (sscanf(eq, "hex(%lu):", &type) == 1 && (eq = strchr(eq, ':')) != NULL &&
               parse_hex(eq + 1, bytes, &size)) {
        set_value(line, type, bytes, size);
    } else {
        host_log("registry", "ignored malformed line for \"%s\" in %s", line, s_store_path);
    }
}

static void load_store(void) {
    static char line[MAX_LINE];
    FILE* f = fopen(s_store_path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) load_line(line);
    }
    fclose(f);
}

void guest_registry_init(const char* game_dir) {
    char root[MAX_PATH];
    DWORD installed = 1;
    _snprintf(s_store_path, sizeof s_store_path - 1, "%s\\" STORE_FILE, game_dir);
    _snprintf(root, sizeof root - 1, "%s\\", game_dir);
    root[sizeof root - 1] = 0;
    load_store();
    host_log("registry", "%d stored values loaded from %s", s_count, s_store_path);
    if (!find_value("Installed")) set_value("Installed", REG_DWORD, &installed, 4);
    set_string("Installed Directory", root);
    set_string("Data Drive", root);
    save_store();
    host_log("registry", "Installed Directory = Data Drive = %s", root);
}

static int is_game_key(uint32_t root, const char* subkey) {
    return root == (uint32_t)(uintptr_t)HKEY_LOCAL_MACHINE && subkey && !_stricmp(subkey, GAME_KEY);
}

static LONG query_value(const char* name, DWORD* type, BYTE* data, DWORD* size) {
    reg_value_t* v = find_value(name ? name : "");
    if (!v || !v->data) return ERROR_FILE_NOT_FOUND;
    if (type) *type = v->type;
    if (data) {
        if (!size) return ERROR_INVALID_PARAMETER;
        if (*size < v->size) {
            *size = v->size;
            return ERROR_MORE_DATA;
        }
        memcpy(data, v->data, v->size);
    }
    if (size) *size = v->size;
    return ERROR_SUCCESS;
}

/* One line per distinct value name: enough to see what the game asked for, never a flood. */
static void report_query_once(const char* name, LONG result) {
    reg_value_t* v = slot_for(name ? name : "");
    if (!v || v->reported) return;
    v->reported = 1;
    host_log("registry", "query \"%s\" -> %s", v->name,
             result == ERROR_SUCCESS ? "stored value" : "not set (game default applies)");
}

void shim_RegOpenKeyExA(void) {
    uint32_t root = GUEST_ARG(0);
    const char* subkey = (const char*)GUEST_PTR(1);
    HKEY* out = (HKEY*)GUEST_PTR(4);
    if (is_game_key(root, subkey)) {
        *out = (HKEY)(uintptr_t)GAME_HKEY;
        g_eax = ERROR_SUCCESS;
    } else {
        g_eax = (uint32_t)RegOpenKeyExA((HKEY)(uintptr_t)root, subkey, 0, KEY_READ, out);
        host_log("registry", "read-only open of %08X\\%s -> %u", root, subkey ? subkey : "", g_eax);
    }
    GUEST_RETURN_STDCALL(5);
}

void shim_RegCreateKeyA(void) {
    uint32_t root = GUEST_ARG(0);
    const char* subkey = (const char*)GUEST_PTR(1);
    HKEY* out = (HKEY*)GUEST_PTR(2);
    if (is_game_key(root, subkey)) {
        *out = (HKEY)(uintptr_t)GAME_HKEY;
        g_eax = ERROR_SUCCESS;
    } else {
        host_log("registry", "refused to create %08X\\%s", root, subkey ? subkey : "");
        g_eax = ERROR_ACCESS_DENIED;
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_RegQueryValueExA(void) {
    uint32_t key = GUEST_ARG(0);
    const char* name = (const char*)GUEST_PTR(1);
    if (key == GAME_HKEY) {
        LONG result = query_value(name, (DWORD*)GUEST_PTR(3), (BYTE*)GUEST_PTR(4), (DWORD*)GUEST_PTR(5));
        report_query_once(name, result);
        g_eax = (uint32_t)result;
    } else {
        g_eax = (uint32_t)RegQueryValueExA((HKEY)(uintptr_t)key, name, NULL, (DWORD*)GUEST_PTR(3),
                                           (BYTE*)GUEST_PTR(4), (DWORD*)GUEST_PTR(5));
    }
    GUEST_RETURN_STDCALL(6);
}

void shim_RegSetValueExA(void) {
    uint32_t key = GUEST_ARG(0);
    const char* name = (const char*)GUEST_PTR(1);
    if (key != GAME_HKEY) {
        host_log("registry", "refused to write \"%s\" outside the game key", name ? name : "");
        g_eax = ERROR_ACCESS_DENIED;
    } else if (set_value(name ? name : "", GUEST_ARG(3), GUEST_PTR(4), GUEST_ARG(5))) {
        save_store();
        g_eax = ERROR_SUCCESS;
    } else {
        host_log("registry", "cannot store \"%s\" (%u bytes)", name ? name : "", GUEST_ARG(5));
        g_eax = ERROR_OUTOFMEMORY;
    }
    GUEST_RETURN_STDCALL(6);
}

void shim_RegDeleteValueA(void) {
    uint32_t key = GUEST_ARG(0);
    const char* name = (const char*)GUEST_PTR(1);
    reg_value_t* v = key == GAME_HKEY ? find_value(name ? name : "") : NULL;
    if (key != GAME_HKEY) {
        g_eax = ERROR_ACCESS_DENIED;
    } else if (v && v->data) {
        free(v->data);
        v->data = NULL;
        v->size = 0;
        save_store();
        g_eax = ERROR_SUCCESS;
    } else {
        g_eax = ERROR_FILE_NOT_FOUND;
    }
    GUEST_RETURN_STDCALL(2);
}

void shim_RegCloseKey(void) {
    uint32_t key = GUEST_ARG(0);
    g_eax = key == GAME_HKEY ? ERROR_SUCCESS : (uint32_t)RegCloseKey((HKEY)(uintptr_t)key);
    GUEST_RETURN_STDCALL(1);
}
