#include "host.h"

#include <stdio.h>
#include <string.h>

#include "l10n.h"

#define GUEST_EXE_NAME JPT_GUEST_IMAGE

static char s_guest_exe[MAX_PATH];
static char s_command_line[2 * MAX_PATH];

void guest_env_init(const char* game_dir, const char* guest_args) {
    _snprintf(s_guest_exe, sizeof s_guest_exe - 1, "%s\\" GUEST_EXE_NAME, game_dir);
    _snprintf(s_command_line, sizeof s_command_line - 1, "\"%s\"%s%s", s_guest_exe,
              guest_args[0] ? " " : "", guest_args);
}

const char* guest_env_exe_path(void) { return s_guest_exe; }

#define GAME_INSTANCE GVA(0x0066E05C)

int guest_load_string(unsigned id, char* out, int cap) {
    return LoadStringA((HINSTANCE)(uintptr_t)MEM32(GAME_INSTANCE), id, out, cap) > 0;
}

int guest_data_dir(char* out, size_t cap) {
    const char* slash = strrchr(s_guest_exe, '\\');
    size_t dir = slash ? (size_t)(slash - s_guest_exe) : 0;
    return snprintf(out, cap, "%.*s\\data", (int)dir, s_guest_exe) < (int)cap;
}

/* The guest CRT startup takes WinMain's command line from msvcrt's _acmdln, which holds the host's. */
int guest_env_apply_command_line(void) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    char** acmdln = crt ? (char**)GetProcAddress(crt, "_acmdln") : NULL;
    if (!acmdln) return 0;
    *acmdln = s_command_line;
    return 1;
}

static const char* text_or_placeholder(const char* s) { return s ? s : "(null)"; }

/* The guest's hInstance must be its own image, not the host executable. */
void shim_GetModuleHandleA(void) {
    const char* name = (const char*)GUEST_PTR(0);
    g_eax = name ? (uint32_t)(uintptr_t)GetModuleHandleA(name) : JPT_GUEST_IMAGE_BASE;
    GUEST_RETURN_STDCALL(1);
}

void shim_GetModuleFileNameA(void) {
    uint32_t module = GUEST_ARG(0), size = GUEST_ARG(2);
    char* out = (char*)GUEST_PTR(1);
    if (module == 0 || module == JPT_GUEST_IMAGE_BASE) {
        uint32_t len = (uint32_t)strlen(s_guest_exe);
        if (size == 0) {
            len = 0;
        } else {
            if (len >= size) len = size - 1;
            memcpy(out, s_guest_exe, len);
            out[len] = 0;
        }
        g_eax = len;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)module, out, size);
    }
    GUEST_RETURN_STDCALL(3);
}

/* sub_0040E4D0's own arguments (type, title id, text id) sit above its 0x2100 bytes of locals, esi,
 * the four MessageBoxA arguments and its return slot. */
#define HELPER_ARGS 0x2120u

static int safemode_prompt_site(void) {
    uint32_t type = 0, title_id = 0, text_id = 0;
    if (g_cur_func != SAFEMODE_MSGBOX_HELPER) return 0;
    guest_read32(g_esp + HELPER_ARGS, &type);
    guest_read32(g_esp + HELPER_ARGS + 4, &title_id);
    guest_read32(g_esp + HELPER_ARGS + 8, &text_id);
    return safemode_prompt_args(g_cur_func, type, title_id, text_id);
}

void shim_MessageBoxA(void) {
    HWND owner = (HWND)GUEST_PTR(0);
    const char* text = (const char*)GUEST_PTR(1);
    const char* caption = (const char*)GUEST_PTR(2);
    UINT type = GUEST_ARG(3);
    int answer;
    if (!registry_legacy_safemode_enabled() && safemode_prompt_site()) {
        host_log("host", "legacy Safe Mode dialog suppressed at sub_0040D780: answered No (start normally)");
        g_eax = IDNO;
        GUEST_RETURN_STDCALL(4);
        return;
    }
    host_log("msgbox", "\"%s\": %s (type 0x%X, from sub_%08X)", text_or_placeholder(caption),
             text_or_placeholder(text), type, g_cur_func);
    mach_leave();
    answer = l10n_active() ? l10n_message_box(owner, text, caption, type) : MessageBoxA(owner, text, caption, type);
    mach_enter();
    host_log("msgbox", "answered %d", answer);
    g_eax = (uint32_t)answer;
    GUEST_RETURN_STDCALL(4);
}

void shim_DialogBoxParamA(void) {
    HINSTANCE instance = (HINSTANCE)GUEST_PTR(0);
    uint32_t dialog = GUEST_ARG(1);
    HWND owner = (HWND)GUEST_PTR(2);
    DLGPROC proc = (DLGPROC)GUEST_PTR(3);
    LPARAM param = (LPARAM)GUEST_ARG(4);
    INT_PTR answer;
    if (IS_INTRESOURCE(dialog))
        host_log("dialog", "template %u from sub_%08X", dialog, g_cur_func);
    else
        host_log("dialog", "template \"%s\" from sub_%08X", (const char*)(uintptr_t)dialog, g_cur_func);
    mach_leave();
    answer = DialogBoxParamA(instance, (LPCSTR)(uintptr_t)dialog, owner, proc, param);
    mach_enter();
    host_log("dialog", "answered %d", (int)answer);
    g_eax = (uint32_t)answer;
    GUEST_RETURN_STDCALL(5);
}

#define DEBUG_LINE_LIMIT 500
static int s_debug_lines;

void shim_OutputDebugStringA(void) {
    const char* text = (const char*)GUEST_PTR(0);
    size_t len = text ? strlen(text) : 0;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    if (len && s_debug_lines < DEBUG_LINE_LIMIT) {
        host_log("guestdbg", "%.*s", (int)len, text);
        if (++s_debug_lines == DEBUG_LINE_LIMIT)
            host_log("guestdbg", "limit of %d lines reached; further guest debug output is not logged",
                     DEBUG_LINE_LIMIT);
    }
    OutputDebugStringA(text_or_placeholder(text));
    GUEST_RETURN_STDCALL(1);
}
