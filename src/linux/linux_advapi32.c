/* The game's one registry key (HKLM\Software\DreamWorks Interactive\Trespasser) served from the portable value store, in the
 * Windows host's file format, with the same host policy for the values the port decides. Every other key is not found. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "guest_ptr.h"
#include "host_policy.h"
#include "jpt_reg_store.h"
#include "linux_advapi32.h"
#include "linux_log.h"
#include "linux_smack.h"

#define HKLM 0x80000002u
#define GAME_KEY_TOKEN 0x00000A04u
#define ERROR_SUCCESS_G 0u
#define ERROR_FILE_NOT_FOUND_G 2u
#define ERROR_ACCESS_DENIED_G 5u
#define ERROR_INVALID_PARAMETER_G 87u
#define ERROR_MORE_DATA_G 234u
#define GAME_KEY "Software\\DreamWorks Interactive\\Trespasser"
#define STORE_FILE "recomp-registry.txt"

static jpt_reg_store_t* s_store;
static char s_path[1024];

void hle_advapi32_init(const char* saves_dir, const char* data_drive, const char* installed_directory) {
    uint32_t installed = 1;
    jpt_reg_free(s_store);
    s_store = jpt_reg_new();
    snprintf(s_path, sizeof s_path, "%s/" STORE_FILE, saves_dir);
    jpt_reg_load(s_store, s_path, NULL);
    if (!jpt_reg_get_dword(s_store, "Installed", &installed)) jpt_reg_set_dword(s_store, "Installed", 1);
    jpt_reg_set_string(s_store, "Installed Directory", installed_directory);
    jpt_reg_set_string(s_store, "Data Drive", data_drive);
    jpt_reg_save(s_store, s_path);
    lx_log("Linux REG", "%d stored values, Installed Directory %s, Data Drive %s", jpt_reg_count(s_store), installed_directory,
           data_drive);
}

typedef struct {
    uint32_t type, size;
    uint8_t data[8];
} policy_t;

static int s_skip_video;

void guest_skip_video(int on) { s_skip_video = on; }

static int dword_policy(const char* name, int stored, policy_t* p) {
    uint32_t value;
    if (!strcasecmp(name, "Safemode") && safemode_value_virtual(0)) value = 0;
    else if (!strcasecmp(name, "AudioEnable3D") || !strcasecmp(name, "Use D3D")) value = 0;
    else if (!strcasecmp(name, "DDrawCertIgnore") && !stored) value = 1;
    else if (!strcasecmp(name, "UseSubTitles") && !stored) value = 1;
    else if (!strcasecmp(name, "NoVideo") && (s_skip_video || !lx_smack_videos_enabled())) value = 1;
    else return 0;
    p->type = JPT_REG_DWORD;
    p->size = 4;
    for (int i = 0; i < 4; i++) p->data[i] = (uint8_t)(value >> (8 * i));
    return 1;
}

static uint64_t a_RegOpenKeyExA(const uint32_t* a, void* u) {
    const char* subkey = a[1] ? gp_string(a[1], 256) : NULL;
    (void)u;
    if (a[0] == HKLM && subkey && !strcasecmp(subkey, GAME_KEY)) {
        gp_write_u32(a[4], GAME_KEY_TOKEN);
        return ERROR_SUCCESS_G;
    }
    return ERROR_FILE_NOT_FOUND_G;
}

static uint64_t a_RegCreateKeyA(const uint32_t* a, void* u) {
    const char* subkey = a[1] ? gp_string(a[1], 256) : NULL;
    (void)u;
    if (a[0] == HKLM && subkey && !strcasecmp(subkey, GAME_KEY)) {
        gp_write_u32(a[2], GAME_KEY_TOKEN);
        return ERROR_SUCCESS_G;
    }
    return ERROR_ACCESS_DENIED_G;
}

static uint64_t a_RegCloseKey(const uint32_t* a, void* u) {
    (void)u;
    return a[0] == GAME_KEY_TOKEN ? ERROR_SUCCESS_G : ERROR_INVALID_PARAMETER_G;
}

static uint64_t a_RegQueryValueExA(const uint32_t* a, void* u) {
    const char* name = a[1] ? gp_string(a[1], 256) : "";
    uint32_t type = 0, size = 0, capacity = 0;
    const void* bytes;
    policy_t policy;
    (void)u;
    if (a[0] != GAME_KEY_TOKEN) return ERROR_INVALID_PARAMETER_G;
    bytes = jpt_reg_get(s_store, name, &type, &size);
    if (dword_policy(name, bytes != NULL, &policy)) {
        bytes = policy.data;
        type = policy.type;
        size = policy.size;
    }
    if (!bytes) return ERROR_FILE_NOT_FOUND_G;
    if (a[3]) gp_write_u32(a[3], type);
    if (a[5]) gp_read_u32(a[5], &capacity);
    if (a[4]) {
        void* out;
        if (!a[5]) return ERROR_INVALID_PARAMETER_G;
        if (capacity < size) {
            gp_write_u32(a[5], size);
            return ERROR_MORE_DATA_G;
        }
        out = gm_ptr_checked(a[4], size, 1);
        if (!out) return ERROR_INVALID_PARAMETER_G;
        memcpy(out, bytes, size);
    }
    if (a[5]) gp_write_u32(a[5], size);
    return ERROR_SUCCESS_G;
}

static uint64_t a_RegSetValueExA(const uint32_t* a, void* u) {
    const char* name = a[1] ? gp_string(a[1], 256) : "";
    const void* data = a[5] ? gm_ptr_checked(a[4], a[5], 0) : (const void*)"";
    (void)u;
    if (a[0] != GAME_KEY_TOKEN) return ERROR_ACCESS_DENIED_G;
    if (!strcasecmp(name, "Safemode") || !data) return ERROR_SUCCESS_G;
    if (!jpt_reg_set(s_store, name, a[3], data, a[5])) return ERROR_INVALID_PARAMETER_G;
    jpt_reg_save(s_store, s_path);
    return ERROR_SUCCESS_G;
}

static uint64_t a_RegDeleteValueA(const uint32_t* a, void* u) {
    const char* name = a[1] ? gp_string(a[1], 256) : "";
    (void)u;
    if (a[0] != GAME_KEY_TOKEN) return ERROR_ACCESS_DENIED_G;
    if (!jpt_reg_delete(s_store, name)) return ERROR_FILE_NOT_FOUND_G;
    jpt_reg_save(s_store, s_path);
    return ERROR_SUCCESS_G;
}

#define ADV(fn, argc) { "advapi32.dll", #fn, a_##fn, argc, HT_STDCALL }

static const hle_entry_t k_table[] = { ADV(RegOpenKeyExA, 5), ADV(RegCreateKeyA, 3), ADV(RegCloseKey, 1), ADV(RegQueryValueExA, 6),
                                       ADV(RegSetValueExA, 6), ADV(RegDeleteValueA, 2) };

const hle_entry_t* hle_advapi32_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
