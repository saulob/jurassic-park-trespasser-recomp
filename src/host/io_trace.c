/*
 * File and module lookups are forwarded unchanged and logged: a game that cannot find
 * its data says nothing about why. Successes are capped; failures have a higher cap.
 * The game tries <game>\X before <game>\data\X; a miss that data\X answers is the game's own
 * path search working, so it is counted, not logged, and a repeat of the previous success is
 * counted too. Every other miss is logged.
 */
#include "host.h"
#include "ce_policy.h"

#include <fcntl.h>
#include <lzexpand.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_policy.h"
#include "audio/platform/windows_dsound_imports.h"
#include "frontend_hd.h"
#include "l10n.h"
#include "menu_audio_policy.h"
#include "stream_audio.h"

#define SUCCESS_LINE_LIMIT 300
#define FAILURE_LINE_LIMIT 1000
#define MAX_SWAP_FILES 16

typedef void* (__cdecl* crt_fopen_t)(const char*, const char*);
typedef int (__cdecl* crt_open_t)(const char*, int, int);
typedef int (__cdecl* crt_path_t)(const char*);
typedef int (__cdecl* crt_rename_t)(const char*, const char*);

static crt_fopen_t s_crt_fopen;
static crt_open_t s_crt_open;
static crt_path_t s_crt_mkdir, s_crt_remove;
static crt_rename_t s_crt_rename;
static LONG s_success_lines, s_failure_lines, s_resolved_misses, s_repeated_successes, s_expected_misses;
static char s_game_dir[MAX_PATH];
static char s_user_dir[MAX_PATH];
static char s_last_success[2 * MAX_PATH + 32];
static struct {
    HANDLE handle;
    char path[MAX_PATH];
} s_swap[MAX_SWAP_FILES];

int io_trace_init(void) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    char* slash;
    if (!crt) return 0;
    strncpy(s_game_dir, guest_env_exe_path(), sizeof s_game_dir - 1);
    slash = strrchr(s_game_dir, '\\');
    if (slash) *slash = 0;
    s_crt_fopen = (crt_fopen_t)GetProcAddress(crt, "fopen");
    s_crt_open = (crt_open_t)GetProcAddress(crt, "_open");
    s_crt_mkdir = (crt_path_t)GetProcAddress(crt, "_mkdir");
    s_crt_remove = (crt_path_t)GetProcAddress(crt, "remove");
    s_crt_rename = (crt_rename_t)GetProcAddress(crt, "rename");
    return s_crt_fopen && s_crt_open && s_crt_mkdir && s_crt_remove && s_crt_rename;
}

static int is_relative(const char* path) { return path[0] != '\\' && path[0] != '/' && !strchr(path, ':'); }

void io_trace_user_dir(const char* dir) { strncpy(s_user_dir, dir, sizeof s_user_dir - 1); }

const char* io_trace_user_folder(void) { return s_user_dir; }

static size_t under(const char* path, const char* dir) {
    size_t n = strlen(dir);
    return n && !_strnicmp(path, dir, n) && path[n] == '\\' ? n : 0;
}

/* The working directory is the game folder, so a relative miss is the same search step; so is a miss under the user
 * folder, which the game knows as its Installed Directory. */
static int answered_by_data_folder(const char* path, DWORD error) {
    char alternative[2 * MAX_PATH];
    size_t dir;
    if (!s_game_dir[0] || !path || !path[0] || strpbrk(path, "*?")) return 0;
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return 0;
    if (is_relative(path)) {
        if (!_strnicmp(path, "data\\", 5)) return 0;
        _snprintf(alternative, sizeof alternative - 1, "%s\\data\\%s", s_game_dir, path);
    } else {
        dir = under(path, s_game_dir);
        if (!dir) dir = under(path, s_user_dir);
        if (!dir || !_strnicmp(path + dir + 1, "data\\", 5)) return 0;
        _snprintf(alternative, sizeof alternative - 1, "%s\\data%s", s_game_dir, path + dir);
    }
    alternative[sizeof alternative - 1] = 0;
    return GetFileAttributesA(alternative) != INVALID_FILE_ATTRIBUTES;
}

static int repeats_last_success(const char* api, const char* path) {
    char key[sizeof s_last_success];
    _snprintf(key, sizeof key - 1, "%s|%s", api, path ? path : "");
    key[sizeof key - 1] = 0;
    if (!strcmp(key, s_last_success)) return 1;
    strcpy(s_last_success, key);
    return 0;
}

static void report(const char* api, const char* path, int ok, DWORD error) {
    LONG* lines = ok ? &s_success_lines : &s_failure_lines;
    LONG limit = ok ? SUCCESS_LINE_LIMIT : FAILURE_LINE_LIMIT;
    LONG n;
    if (!ok && host_expected_miss(api, path, error)) {
        InterlockedIncrement(&s_expected_misses);
        SetLastError(error);
        return;
    }
    if (ok ? repeats_last_success(api, path) : answered_by_data_folder(path, error)) {
        InterlockedIncrement(ok ? &s_repeated_successes : &s_resolved_misses);
        SetLastError(error);
        return;
    }
    n = InterlockedIncrement(lines);
    if (n > limit) return;
    if (ok)
        host_log("file", "%s(\"%s\") ok", api, path ? path : "(null)");
    else
        host_log("file", "%s(\"%s\") FAILED, error %lu, from sub_%08X", api, path ? path : "(null)",
                 error, g_cur_func);
    if (n == limit) host_log("file", "%s line limit reached; later ones are not logged", ok ? "success" : "failure");
    SetLastError(error);
}

void io_trace_summary(void) {
    if (s_expected_misses)
        host_log("file", "%ld expected misses not listed (no *.swp swap file yet)",
                 s_expected_misses);
    if (s_resolved_misses || s_repeated_successes)
        host_log("file", "not listed above: %ld lookups under the game or user folder answered by the data subfolder, %ld "
                 "repeats of the previous successful open", s_resolved_misses, s_repeated_successes);
}

static int is_swap_file(const char* path) {
    size_t n = path ? strlen(path) : 0;
    return n > 4 && n < MAX_PATH && !_stricmp(path + n - 4, ".swp");
}

static void note_swap_file(const char* path, HANDLE h) {
    for (int i = 0; i < MAX_SWAP_FILES; i++) {
        if (!s_swap[i].handle) {
            s_swap[i].handle = h;
            strcpy(s_swap[i].path, path);
            return;
        }
    }
}

/* Only for a host shutdown: the game, which normally removes its level swap file, never runs again. */
void io_trace_release_swap_files(void) {
    for (int i = 0; i < MAX_SWAP_FILES; i++) {
        if (!s_swap[i].handle) continue;
        CloseHandle(s_swap[i].handle);
        s_swap[i].handle = NULL;
        if (GetFileAttributesA(s_swap[i].path) == INVALID_FILE_ATTRIBUTES) continue;
        host_log("file", "level swap file %s %s", s_swap[i].path,
                 DeleteFileA(s_swap[i].path) ? "removed, as the game does when it ends normally" : "could not be removed");
    }
}

void shim_CloseHandle(void) {
    HANDLE h = (HANDLE)GUEST_PTR(0);
    for (int i = 0; i < MAX_SWAP_FILES; i++)
        if (s_swap[i].handle == h) s_swap[i].handle = NULL;
    stream_audio_detach(h);
    g_eax = (uint32_t)CloseHandle(h);
    GUEST_RETURN_STDCALL(1);
}

static const char* menu_audio_file(const char* path, char* out, size_t cap) {
    static LONG logged;
    if (!menu_audio_source(path, s_user_dir, s_game_dir, out, cap) || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return path;
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
        if (!InterlockedExchange(&logged, 1)) host_log("menu-audio", "%s is missing: the menus stay silent", out);
        return path;
    }
    if (!InterlockedExchange(&logged, 1))
        host_log("menu-audio", "original selection sound restored: menu.tpa served from %s", out);
    return out;
}

void shim_CreateFileA(void) {
    const char* path = (const char*)GUEST_PTR(0);
    char translated[MAX_PATH];
    char menu_audio[MAX_PATH];
    char level[MAX_PATH];
    DWORD access = GUEST_ARG(1);
    int from_level = path && GUEST_ARG(4) == OPEN_EXISTING && custom_level_file(path, level, sizeof level);
    HANDLE h;
    if (path && GUEST_ARG(4) != OPEN_EXISTING && custom_level_write_refused(path)) {
        g_eax = (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE;
        GUEST_RETURN_STDCALL(7);
        return;
    }
    if (from_level) {
        path = level;
        access &= ~(DWORD)GENERIC_WRITE;
    }
    if (path && GUEST_ARG(4) == OPEN_EXISTING && l10n_menu_file(path, translated, sizeof translated)) path = translated;
    if (path && GUEST_ARG(4) == OPEN_EXISTING) path = menu_audio_file(path, menu_audio, sizeof menu_audio);
    h = CreateFileA(path, access, GUEST_ARG(2) | (from_level ? FILE_SHARE_READ : 0), (LPSECURITY_ATTRIBUTES)GUEST_PTR(3),
                           GUEST_ARG(4), GUEST_ARG(5), (HANDLE)GUEST_PTR(6));
    DWORD error = GetLastError();
    if (h != INVALID_HANDLE_VALUE && !from_level && is_swap_file(path)) note_swap_file(path, h);
    if (h != INVALID_HANDLE_VALUE && !from_level && GUEST_ARG(4) == OPEN_EXISTING) stream_audio_attach(h, path, GUEST_ARG(1), GUEST_ARG(5));
    SetLastError(error);
    report("CreateFileA", path, h != INVALID_HANDLE_VALUE, GetLastError());
    g_eax = (uint32_t)(uintptr_t)h;
    GUEST_RETURN_STDCALL(7);
}

void shim_GetFileAttributesA(void) {
    const char* path = (const char*)GUEST_PTR(0);
    char level[MAX_PATH];
    DWORD attributes;
    if (custom_level_file(path, level, sizeof level)) path = level;
    attributes = GetFileAttributesA(path);
    report("GetFileAttributesA", path, attributes != INVALID_FILE_ATTRIBUTES, GetLastError());
    g_eax = attributes;
    GUEST_RETURN_STDCALL(1);
}

void shim_GetDiskFreeSpaceA(void) {
    const char* root = (const char*)GUEST_PTR(0);
    ULARGE_INTEGER available, total, free_total;
    uint32_t geometry[4];
    BOOL ok = GetDiskFreeSpaceExA(root, &available, &total, &free_total);
    legacy_disk_geometry(ok ? available.QuadPart : 0x7FFFF000ull, ok ? total.QuadPart : 0xFFFFF000ull, geometry);
    if (GUEST_ARG(1)) MEM32(GUEST_ARG(1)) = geometry[0];
    if (GUEST_ARG(2)) MEM32(GUEST_ARG(2)) = geometry[1];
    if (GUEST_ARG(3)) MEM32(GUEST_ARG(3)) = geometry[2];
    if (GUEST_ARG(4)) MEM32(GUEST_ARG(4)) = geometry[3];
    g_eax = 1;
    GUEST_RETURN_STDCALL(5);
}

void shim_FindFirstFileA(void) {
    const char* pattern = (const char*)GUEST_PTR(0);
    HANDLE h = FindFirstFileA(pattern, (WIN32_FIND_DATAA*)GUEST_PTR(1));
    report("FindFirstFileA", pattern, h != INVALID_HANDLE_VALUE, GetLastError());
    g_eax = (uint32_t)(uintptr_t)h;
    GUEST_RETURN_STDCALL(2);
}

void shim_LZOpenFileA(void) {
    char* path = (char*)GUEST_PTR(0);
    OFSTRUCT* opened = (OFSTRUCT*)GUEST_PTR(1);
    const WORD mode = (WORD)GUEST_ARG(2);
    INT h = custom_level_lz_open(path, !(mode & (OF_WRITE | OF_READWRITE | OF_CREATE | OF_DELETE | OF_EXIST | OF_PARSE)));
    DWORD error = GetLastError();
    if (h != -2) {
        if (opened) {
            memset(opened, 0, sizeof *opened);
            opened->cBytes = sizeof *opened;
            opened->fFixedDisk = 1;
            _snprintf(opened->szPathName, sizeof opened->szPathName - 1, "%s", path);
        }
        report("LZOpenFileA", path, h >= 0, error);
        g_eax = (uint32_t)h;
        GUEST_RETURN_STDCALL(3);
        return;
    }
    h = LZOpenFileA(path, opened, mode);
    error = GetLastError();
    if (h >= 0) patch117_lz_opened(opened && opened->szPathName[0] ? opened->szPathName : path, h);
    report("LZOpenFileA", path, h >= 0, error);
    g_eax = (uint32_t)h;
    GUEST_RETURN_STDCALL(3);
}

void shim_LoadLibraryA(void) {
    const char* name = (const char*)GUEST_PTR(0);
    HMODULE module;
    if (host_unavailable_library(name)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        g_eax = 0;
        GUEST_RETURN_STDCALL(1);
        return;
    }
    if (audio_is_dsound_library(name)) {
        g_eax = (uint32_t)(uintptr_t)dsound_host_module();
        GUEST_RETURN_STDCALL(1);
        return;
    }
    module = LoadLibraryA(name);
    report("LoadLibraryA", name, module != NULL, GetLastError());
    g_eax = (uint32_t)(uintptr_t)module;
    GUEST_RETURN_STDCALL(1);
}

void shim_GetProcAddress(void) {
    HMODULE module = (HMODULE)GUEST_PTR(0);
    const char* name = (const char*)GUEST_PTR(1);
    g_eax = (uint32_t)(uintptr_t)dsound_get_proc_address(module, name);
    GUEST_RETURN_STDCALL(2);
}

void shim_FreeLibrary(void) {
    g_eax = (uint32_t)dsound_free_library((HMODULE)GUEST_PTR(0));
    GUEST_RETURN_STDCALL(1);
}

/* cdecl: the lifted caller removes the arguments. The FILE* and descriptor belong to
 * msvcrt.dll, so msvcrt's own functions must create them. */
void shim_fopen(void) {
    const char* path = (const char*)GUEST_PTR(0);
    const char* mode = (const char*)GUEST_PTR(1);
    char level[MAX_PATH];
    void* file = NULL;
    if (mode[0] == 'r' && !strchr(mode, '+') && custom_level_file(path, level, sizeof level)) path = level;
    else if ((mode[0] != 'r' || strchr(mode, '+')) && custom_level_write_refused(path)) path = NULL;
    if (path) file = s_crt_fopen(path, mode);
    report("fopen", path, file != NULL, GetLastError());
    g_eax = (uint32_t)(uintptr_t)file;
    g_esp += 4;
}

void shim_open(void) {
    const char* path = (const char*)GUEST_PTR(0);
    char level[MAX_PATH];
    int flags = (int)GUEST_ARG(1), fd = -1;
    DWORD error;
    const int writes = flags & (_O_CREAT | _O_TRUNC | _O_WRONLY | _O_RDWR);
    if (!writes && custom_level_file(path, level, sizeof level)) path = level;
    if (!writes || !custom_level_write_refused(path)) fd = s_crt_open(path, flags, (int)GUEST_ARG(2));
    error = GetLastError();
    report("_open", path, fd >= 0, error);
    if (fd >= 0 && (flags & (_O_CREAT | _O_TRUNC)) == (_O_CREAT | _O_TRUNC)) frontend_hd_save_opened(path, s_user_dir);
    SetLastError(error);
    g_eax = (uint32_t)fd;
    g_esp += 4;
}

void shim_DeleteFileA(void) {
    const char* path = (const char*)GUEST_PTR(0);
    BOOL ok = !custom_level_write_refused(path) && DeleteFileA(path);
    DWORD error = GetLastError();
    report("DeleteFileA", path, ok, error);
    if (ok) frontend_hd_save_deleted(path, s_user_dir);
    SetLastError(error);
    g_eax = (uint32_t)ok;
    GUEST_RETURN_STDCALL(1);
}

void shim_mkdir(void) {
    const char* path = (const char*)GUEST_PTR(0);
    g_eax = (uint32_t)(custom_level_write_refused(path) ? -1 : s_crt_mkdir(path));
    g_esp += 4;
}

void shim_remove(void) {
    const char* path = (const char*)GUEST_PTR(0);
    g_eax = (uint32_t)(custom_level_write_refused(path) ? -1 : s_crt_remove(path));
    g_esp += 4;
}

void shim_rename(void) {
    const char* from = (const char*)GUEST_PTR(0);
    const char* to = (const char*)GUEST_PTR(1);
    g_eax = (uint32_t)(custom_level_write_refused(from) || custom_level_write_refused(to) ? -1 : s_crt_rename(from, to));
    g_esp += 4;
}
