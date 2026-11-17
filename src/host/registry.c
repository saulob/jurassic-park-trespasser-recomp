/*
 * The game keeps its install paths and settings under one HKLM key that only its installer
 * creates. That key is served from a text file in the user directory, so nothing is written
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
#define SAFEMODE_VALUE "Safemode"
#define SUBTITLES_VALUE "UseSubTitles"
#define SUBTITLES_MARKER "JPT UseSubTitles Default"

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
static int s_show_ddraw_cert_dialog;
static int s_software_water;
static int s_skip_video;
static int s_legacy_safemode;
static int s_safemode_logged;
static int s_gameplay_w[2], s_gameplay_h[2];

void registry_gameplay_sizes(int hardware_w, int hardware_h, int software_w, int software_h) {
    s_gameplay_w[1] = hardware_w;
    s_gameplay_h[1] = hardware_h;
    s_gameplay_w[0] = software_w;
    s_gameplay_h[0] = software_h;
}

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

static void clear_value(reg_value_t* v) {
    free(v->data);
    v->data = NULL;
    v->size = 0;
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

static void migrate_subtitles_default(void) {
    reg_value_t* marker = find_value(SUBTITLES_MARKER);
    reg_value_t* value = find_value(SUBTITLES_VALUE);
    DWORD on = 1, replacement;
    int value_present = value && value->data && value->type == REG_DWORD && value->size == 4;
    if (registry_subtitles_migration(marker && marker->data, value_present, value_present ? *(DWORD*)value->data : 0, &replacement)) {
        set_value(SUBTITLES_VALUE, REG_DWORD, &replacement, 4);
        host_log("registry", "stored \"" SUBTITLES_VALUE "\" was the game's own Off: On, the port's default");
    }
    if (!marker || !marker->data) set_value(SUBTITLES_MARKER, REG_DWORD, &on, 4);
}

void guest_registry_init(const char* game_dir, const char* user_dir) {
    char data_root[MAX_PATH], user_root[MAX_PATH];
    DWORD installed = 1;
    _snprintf(s_store_path, sizeof s_store_path - 1, "%s\\" STORE_FILE, user_dir);
    _snprintf(data_root, sizeof data_root - 1, "%s\\", game_dir);
    _snprintf(user_root, sizeof user_root - 1, "%s\\", user_dir);
    data_root[sizeof data_root - 1] = 0;
    user_root[sizeof user_root - 1] = 0;
    load_store();
    host_log("registry", "%d stored values loaded from %s", s_count, s_store_path);
    if (safemode_value_virtual(s_legacy_safemode) && find_value(SAFEMODE_VALUE) && find_value(SAFEMODE_VALUE)->data) {
        clear_value(find_value(SAFEMODE_VALUE));
        host_log("registry", "stored \"" SAFEMODE_VALUE "\" from an earlier run dropped: the recompilation has no Safe "
                 "Mode prompt");
    }
    if (!find_value("Installed")) set_value("Installed", REG_DWORD, &installed, 4);
    migrate_subtitles_default();
    set_string("Installed Directory", user_root);
    set_string("Data Drive", data_root);
    save_store();
    host_log("registry", "Installed Directory (saves, level swap files) = %s, Data Drive = %s", user_root, data_root);
}

static int is_game_key(uint32_t root, const char* subkey) {
    return root == (uint32_t)(uintptr_t)HKEY_LOCAL_MACHINE && subkey && !_stricmp(subkey, GAME_KEY);
}

void registry_show_ddraw_cert_dialog(void) { s_show_ddraw_cert_dialog = 1; }

void registry_software_water(void) { s_software_water = 1; }

void registry_skip_video(int on) { s_skip_video = on; }

void registry_legacy_safemode_dialog(void) { s_legacy_safemode = 1; }

int registry_legacy_safemode_enabled(void) { return s_legacy_safemode; }

static int is_safemode(const char* name) { return name && !_stricmp(name, SAFEMODE_VALUE); }

typedef struct {
    DWORD type, size;
    BYTE data[16];
    const char* text;
} policy_t;

/* {84E63DE0-46AA-11CF-816F-0000C020156E}, IID_IDirect3DHALDevice, as the game stores it. */
static const BYTE k_hal_guid[16] = { 0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                                     0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E };

static void policy_dword(policy_t* p, DWORD value, const char* text) {
    p->type = REG_DWORD;
    p->size = sizeof value;
    memcpy(p->data, &value, sizeof value);
    p->text = text;
}

/*
 * Host policy, never stored. "Safemode" is the game's "still running" flag: WinMain sets it to 1 at
 * start and back to 0 at its end (0x005F70E0 / 0x005F70B0), and a 1 at start-up asks for Safe Mode
 * (0x0040D780). It reads as 0 and is not written unless --legacy-safemode-dialog is given.
 * "DDrawCertIgnore": the "NOT certified" dialog speaks of legacy display
 * hardware, so the game's own opt-out is answered as set unless a stored value exists or the dialog
 * was asked for. "Use D3D" and "D3D GUID" follow --renderer and override stored values: d3d11 selects
 * the game's Direct3D path on the compatibility layer's hardware device, software keeps it off.
 * "Hardware Water": the game's video setup (0x0040AA80) checks it for every device that can draw alpha
 * water. Unset, water keeps its software texture of alpha codes, which the full-texturing path then
 * draws as an opaque colour texture; d3d11 answers 1 unless a value is stored or --software-water.
 * "AudioEnable3D" stays 0 whatever is stored: a DirectSound wrapper that reported hardware 3D voices (0x00435BE2)
 * would otherwise send the game down its 3D buffer path.
 */
static int policy_value(const char* name, int stored, policy_t* p) {
    if (is_safemode(name) && safemode_value_virtual(s_legacy_safemode)) {
        if (!s_safemode_logged) host_log("host", "legacy Safe Mode dialog suppressed");
        s_safemode_logged = 1;
        policy_dword(p, 0, "host policy: 0 (the last run is never reported unstable; --legacy-safemode-dialog keeps "
                           "the game's own crash flag)");
        return 1;
    }
    if (s_skip_video && name && !_stricmp(name, "NoVideo")) {
        policy_dword(p, 1, "host policy: 1 for this opening only (Skip Intro): CVideoWnd::Play takes its own no-video "
                           "branch");
        return 1;
    }
    if (name && !stored && !_stricmp(name, SUBTITLES_VALUE)) {
        policy_dword(p, REGISTRY_SUBTITLES_ON, "host policy: On until the game's options store a choice");
        return 1;
    }
    if (name && !_stricmp(name, "AudioEnable3D")) {
        policy_dword(p, 0, "host policy: 0 (hardware 3D sound buffers are not supported)");
        return 1;
    }
    if (!name || !ddraw_compat_active()) return 0;
    if (s_gameplay_w[0] && (!_stricmp(name, "Width") || !_stricmp(name, "Height"))) {
        int width = !_stricmp(name, "Width"), hw = d3dr_hardware() ? 1 : 0;
        policy_dword(p, (DWORD)(width ? s_gameplay_w[hw] : s_gameplay_h[hw]),
                     hw ? "host policy: the D3D11 gameplay resolution (the launcher's Render Resolution and Aspect "
                          "Ratio; the in-game video options cannot change it)"
                        : "host policy: the software renderer's gameplay mode of the launcher's Aspect Ratio");
        return 1;
    }
    if (registry_view_margin_name(name)) {
        policy_dword(p, REGISTRY_VIEW_MARGIN, "host policy: 0 (the 3D image fills the launcher's resolution; a stored Screen "
                                              "Size margin is ignored)");
        return 1;
    }
    if (!_stricmp(name, "DDrawCertIgnore")) {
        if (stored || s_show_ddraw_cert_dialog) return 0;
        policy_dword(p, 1, "host policy: 1 (legacy DirectDraw certification dialog skipped; --ddraw-cert-dialog shows it)");
        return 1;
    }
    if (!_stricmp(name, "Use D3D")) {
        policy_dword(p, d3dr_hardware() ? 1 : 0,
                     d3dr_hardware() ? "host policy: 1 (--renderer d3d11)" : "host policy: 0 (--renderer software)");
        return 1;
    }
    if (!_stricmp(name, "D3D GUID") && d3dr_hardware()) {
        p->type = REG_BINARY;
        p->size = sizeof k_hal_guid;
        memcpy(p->data, k_hal_guid, sizeof k_hal_guid);
        p->text = "host policy: IID_IDirect3DHALDevice (--renderer d3d11)";
        return 1;
    }
    if (!_stricmp(name, "Max Recommended Tex Dim") && engine_texture_dim_policy(engine_limits_modern())) {
        policy_dword(p, engine_texture_dim_policy(1),
                     "host policy: 256 with d3d11 (textures and impostors at their full size; the 1998 setup offered "
                     "32 to 128; --original-engine-limits keeps the stored value)");
        return 1;
    }
    if (!_stricmp(name, "RenderQuality") && engine_render_quality_policy(d3dr_hardware(), stored)) {
        policy_dword(p, engine_render_quality_policy(1, 0),
                     "host policy: 4, the top render quality, with d3d11 until the game's options store a choice");
        return 1;
    }
    if (!_stricmp(name, "Hardware Water") && hardware_water_policy(d3dr_hardware(), stored, s_software_water) == 1) {
        policy_dword(p, 1, "host policy: 1 (--renderer d3d11, as the game's video setup sets it for alpha-capable "
                           "devices; --software-water or a stored value keeps the software water)");
        return 1;
    }
    return 0;
}

static LONG query_value(const char* name, DWORD* type, BYTE* data, DWORD* size, const char** policy_text) {
    reg_value_t* v = find_value(name ? name : "");
    policy_t policy;
    *policy_text = NULL;
    if (policy_value(name, v && v->data, &policy)) {
        *policy_text = policy.text;
        if (type) *type = policy.type;
        if (data) {
            if (!size) return ERROR_INVALID_PARAMETER;
            if (*size < policy.size) {
                *size = policy.size;
                return ERROR_MORE_DATA;
            }
            memcpy(data, policy.data, policy.size);
        }
        if (size) *size = policy.size;
        return ERROR_SUCCESS;
    }
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
static void report_query_once(const char* name, LONG result, const char* policy_text) {
    reg_value_t* v = slot_for(name ? name : "");
    if (!v || v->reported) return;
    v->reported = 1;
    host_log("registry", "query \"%s\" -> %s", v->name,
             policy_text ? policy_text : result == ERROR_SUCCESS ? "stored value" : "not set (game default applies)");
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
        const char* policy_text;
        LONG result = query_value(name, (DWORD*)GUEST_PTR(3), (BYTE*)GUEST_PTR(4), (DWORD*)GUEST_PTR(5), &policy_text);
        report_query_once(name, result, policy_text);
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
    } else if (is_safemode(name) && safemode_value_virtual(s_legacy_safemode)) {
        g_eax = ERROR_SUCCESS;
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
        clear_value(v);
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
