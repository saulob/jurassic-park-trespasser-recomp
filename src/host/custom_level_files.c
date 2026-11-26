/* The active custom level on the Windows host: which guest paths its files answer, the level loads that LZ32 cannot take
 * (it stops at 128 characters) and the level files the game opens through MSVCIRT. */
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <lzexpand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/platform/windows_dsound_imports.h"
#include "jpt_custom_level.h"
#include "jpt_level_gate.h"
#include "jpt_level_world.h"
#include "jpt_scene.h"
#include "jpt_szdd.h"
#include "jpt_texhw.h"

#define LZ_HANDLES 8
#define LZ_FIRST 0x4A00
#define LZ_NATIVE_MAX 120
#define LZ_RAW_LIMIT (64 * 1024 * 1024)

typedef int (__cdecl* crt_sopen_t)(const char*, int, int, int);

typedef struct {
    int used;
    HANDLE file;
    uint8_t* data;
    LONG size, pos;
} lz_file_t;

static char s_data_dir[MAX_PATH];
static size_t s_data_len, s_game_len;
static crt_sopen_t s_sopen;
static int* (__cdecl* s_errno)(void);
static lz_file_t s_lz[LZ_HANDLES];

/* The path below the game folder, whatever the current directory; NULL when it lies elsewhere. */
static const char* below_game(const char* path, char full[MAX_PATH]) {
    DWORD n;
    if (!s_data_len || !path || !path[0]) return NULL;
    n = GetFullPathNameA(path, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH || _strnicmp(full, s_data_dir, s_game_len)) return NULL;
    return full + s_game_len;
}

static const char* below_data(const char* path, char full[MAX_PATH]) {
    const char* relative = below_game(path, full);
    return relative && !_strnicmp(full, s_data_dir, s_data_len) ? full + s_data_len : NULL;
}

static int game_file(const char* relative, char* out, size_t cap) {
    for (size_t root = s_game_len; root; root = root == s_game_len ? s_data_len : 0) {
        DWORD attributes;
        int n = snprintf(out, cap, "%.*s%s", (int)root, s_data_dir, relative);
        if (n < 0 || (size_t)n >= cap) return 0;
        attributes = GetFileAttributesA(out);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    return 0;
}

int custom_level_file(const char* path, char* out, size_t cap) {
    char full[MAX_PATH];
    DWORD error = GetLastError();
    const char* relative = jpt_custom_level_active() && !jpt_custom_level_is_texture_request(path) ? below_game(path, full) : NULL;
    int found = relative ? jpt_custom_level_find_in_game(relative, out, cap) : jpt_custom_level_find_relative(path, out, cap);
    SetLastError(error);
    return found;
}

int custom_level_write_refused(const char* path) {
    char full[MAX_PATH];
    if (!jpt_custom_level_active() || !below_data(path, full)) return 0;
    SetLastError(ERROR_ACCESS_DENIED);
    if (s_errno) *s_errno() = EACCES;
    return 1;
}

static int __cdecl sopen_hook(const char* path, int flags, int share, int mode) {
    char host[MAX_PATH];
    int fd;
    if (flags & (_O_CREAT | _O_TRUNC | _O_WRONLY | _O_RDWR)) {
        if (custom_level_write_refused(path)) return -1;
    } else if (custom_level_file(path, host, sizeof host)) {
        host_log("file", "stream \"%s\" served from %s", path, host);
        path = host;
    }
    fd = s_sopen(path, flags, share, mode);
    if (fd < 0 && jpt_custom_level_active() && jpt_level_world_needs(path)) {
        host_log("custom-level", "%s of %s cannot be played: its terrain data %s is neither in the package nor in the installed game, and the engine has no world without it",
                 jpt_custom_level_current_scene(), jpt_custom_level_package(), path);
        host_stop_level(jpt_gate_message(JPT_GATE_MISSING));
    }
    return fd;
}

int custom_level_files_start(const char* game_dir) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (_snprintf(s_data_dir, sizeof s_data_dir - 1, "%s\\data\\", game_dir) < 0) return 0;
    s_data_len = strlen(s_data_dir);
    s_game_len = strlen(game_dir) + 1;
    jpt_custom_level_set_game(game_file);
    s_sopen = crt ? (crt_sopen_t)GetProcAddress(crt, "_sopen") : NULL;
    s_errno = crt ? (int*(__cdecl*)(void))GetProcAddress(crt, "_errno") : NULL;
    return s_sopen && dsound_patch_import(GetModuleHandleA("msvcirt.dll"), "msvcrt.dll", "_sopen", (void*)sopen_hook);
}

static lz_file_t* lz_of(int handle) {
    int slot = handle - LZ_FIRST;
    return slot >= 0 && slot < LZ_HANDLES && s_lz[slot].used ? &s_lz[slot] : NULL;
}

static void lz_release(lz_file_t* f) {
    if (f->file && f->file != INVALID_HANDLE_VALUE) CloseHandle(f->file);
    free(f->data);
    memset(f, 0, sizeof *f);
}

static int lz_expand(lz_file_t* f) {
    uint8_t header[JPT_SZDD_HEADER];
    uint8_t* raw;
    DWORD got = 0;
    size_t expanded = 0;
    if (!ReadFile(f->file, header, sizeof header, &got, NULL)) return 0;
    if (SetFilePointer(f->file, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return 0;
    if (!jpt_szdd_is(header, got)) return 1;
    if (f->size > LZ_RAW_LIMIT || (raw = (uint8_t*)malloc((size_t)f->size)) == NULL) return 0;
    if (ReadFile(f->file, raw, (DWORD)f->size, &got, NULL) && got == (DWORD)f->size)
        f->data = jpt_szdd_expand(raw, (size_t)f->size, &expanded);
    free(raw);
    CloseHandle(f->file);
    f->file = NULL;
    f->size = (LONG)expanded;
    return f->data != NULL;
}

static int is_type(const char* path, const char* type) {
    size_t n = strlen(path);
    return n > 4 && !_stricmp(path + n - 4, type);
}

static int read_all(lz_file_t* f) {
    DWORD got = 0;
    if (f->data) return 1;
    if (f->size <= 0 || f->size > LZ_RAW_LIMIT || (f->data = (uint8_t*)malloc((size_t)f->size)) == NULL) return 0;
    if (ReadFile(f->file, f->data, (DWORD)f->size, &got, NULL) && got == (DWORD)f->size) {
        CloseHandle(f->file);
        f->file = NULL;
        return 1;
    }
    free(f->data);
    f->data = NULL;
    return 0;
}

/* A scene of a later format, as the game's own loader reads it; the file stays as it is. */
static int adapt_scene(lz_file_t* f, const char* path) {
    jpt_scene_report_t report;
    uint8_t* adapted;
    size_t size = 0;
    if (!read_all(f)) return 0;
    adapted = jpt_custom_level_adapt_scene(path, f->data, (size_t)f->size, &size, &report);
    if (report.problem[0]) {
        host_log("custom-level", "%s cannot be read: %s", path, report.problem);
        return 0;
    }
    if (!adapted) return 1;
    host_log("custom-level", "%s: scene version %d read as the game's own layout (%ld -> %u bytes, in memory)", path, report.version, (long)f->size, (unsigned)size);
    for (int i = 0; i < report.note_count; i++) host_log("custom-level", "  %s", report.notes[i]);
    free(f->data);
    f->data = adapted;
    f->size = (LONG)size;
    return 1;
}

static void texhw_line(const char* line) { host_log("custom-level", "%s", line); }

static int adapt_world(lz_file_t* f, const char* path) {
    jpt_level_world_t world;
    char line[512];
    if (!read_all(f)) return 1;
    jpt_texhw_set_log(texhw_line);
    jpt_level_world_prepare(path, f->data, (size_t)f->size, &world);
    for (int i = 0; jpt_level_world_line(&world, i, line, sizeof line); i++) host_log("custom-level", "%s", line);
    if (world.failed) host_stop_level(jpt_gate_message(JPT_GATE_UNSAFE));
    return 1;
}

static int adapt(lz_file_t* f, const char* asked, const char* host) {
    if (is_type(host, ".scn")) return adapt_scene(f, host);
    return !is_type(host, ".grf") || adapt_world(f, asked);
}

/* A package's file, or a path LZ32 refuses for its length; -2 leaves the file to LZ32. */
int custom_level_lz_open(const char* path, int for_reading) {
    char host[MAX_PATH], full[MAX_PATH];
    const char* source = path;
    LARGE_INTEGER size;
    DWORD error;
    int slot = 0;
    if (!path || !for_reading) return -2;
    if ((jpt_save_is_name(path) || jpt_save_is_temporary(path)) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && !jpt_gate_reads(path, full, sizeof full)) {
        host_log("save", "%s is damaged and was not read: %s", path, full);
        SetLastError(ERROR_FILE_CORRUPT);
        return -1;
    }
    if (custom_level_file(path, host, sizeof host)) {
        source = host;
    } else {
        DWORD n = GetFullPathNameA(path, MAX_PATH, full, NULL);
        if (n && n < MAX_PATH && is_type(path, ".grf")) jpt_level_world_original(full);
        if (n < LZ_NATIVE_MAX) return -2;
    }
    while (slot < LZ_HANDLES && s_lz[slot].used) slot++;
    if (slot == LZ_HANDLES) return LZERROR_GLOBALLOC;
    s_lz[slot].file = CreateFileA(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
    error = GetLastError();
    if (s_lz[slot].file != INVALID_HANDLE_VALUE && GetFileSizeEx(s_lz[slot].file, &size) && size.QuadPart <= MAXLONG) {
        s_lz[slot].used = 1;
        s_lz[slot].size = (LONG)size.QuadPart;
        if (lz_expand(&s_lz[slot]) && (source != host || adapt(&s_lz[slot], path, host))) return LZ_FIRST + slot;
    }
    lz_release(&s_lz[slot]);
    SetLastError(error);
    return -1;
}

int custom_level_lz_read(int handle, char* buffer, int wanted, int* result) {
    lz_file_t* f = lz_of(handle);
    DWORD got = 0;
    if (!f) return 0;
    if (wanted < 0) {
        *result = LZERROR_BADVALUE;
    } else if (f->data) {
        LONG n = f->size - f->pos < wanted ? f->size - f->pos : wanted;
        memcpy(buffer, f->data + f->pos, (size_t)n);
        f->pos += n;
        *result = (int)n;
    } else {
        *result = ReadFile(f->file, buffer, (DWORD)wanted, &got, NULL) ? (int)got : LZERROR_READ;
    }
    return 1;
}

int custom_level_lz_seek(int handle, long offset, int origin, int* result) {
    lz_file_t* f = lz_of(handle);
    LONG base, to;
    if (!f) return 0;
    if (!f->data) f->pos = (LONG)SetFilePointer(f->file, 0, NULL, FILE_CURRENT);
    base = origin == 1 ? f->pos : origin == 2 ? f->size : 0;
    to = base + offset;
    *result = LZERROR_BADVALUE;
    if (origin < 0 || origin > 2 || to < 0 || to > f->size) return 1;
    if (!f->data && SetFilePointer(f->file, to, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return 1;
    f->pos = to;
    *result = (int)to;
    return 1;
}

int custom_level_lz_close(int handle) {
    lz_file_t* f = lz_of(handle);
    if (!f) return 0;
    lz_release(f);
    return 1;
}
