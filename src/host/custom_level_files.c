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
#include "jpt_szdd.h"

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
static size_t s_data_len;
static crt_sopen_t s_sopen;
static int* (__cdecl* s_errno)(void);
static lz_file_t s_lz[LZ_HANDLES];

/* The path below the data folder, whatever the current directory; NULL when it lies elsewhere. */
static const char* below_data(const char* path, char full[MAX_PATH]) {
    DWORD n;
    if (!s_data_len || !path || !path[0]) return NULL;
    n = GetFullPathNameA(path, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH || _strnicmp(full, s_data_dir, s_data_len)) return NULL;
    return full + s_data_len;
}

int custom_level_file(const char* path, char* out, size_t cap) {
    char full[MAX_PATH];
    DWORD error = GetLastError();
    const char* relative = jpt_custom_level_active() ? below_data(path, full) : NULL;
    int found = relative && jpt_custom_level_find(relative, out, cap);
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
    if (flags & (_O_CREAT | _O_TRUNC | _O_WRONLY | _O_RDWR)) {
        if (custom_level_write_refused(path)) return -1;
    } else if (custom_level_file(path, host, sizeof host)) {
        host_log("file", "stream \"%s\" served from %s", path, host);
        path = host;
    }
    return s_sopen(path, flags, share, mode);
}

int custom_level_files_start(const char* game_dir) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (_snprintf(s_data_dir, sizeof s_data_dir - 1, "%s\\data\\", game_dir) < 0) return 0;
    s_data_len = strlen(s_data_dir);
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

/* A package's file, or a path LZ32 refuses for its length; -2 leaves the file to LZ32. */
int custom_level_lz_open(const char* path, int for_reading) {
    char host[MAX_PATH], full[MAX_PATH];
    const char* source = path;
    LARGE_INTEGER size;
    DWORD error;
    int slot = 0;
    if (!path || !for_reading) return -2;
    if (custom_level_file(path, host, sizeof host)) {
        source = host;
    } else {
        DWORD n = GetFullPathNameA(path, MAX_PATH, full, NULL);
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
        if (lz_expand(&s_lz[slot])) return LZ_FIRST + slot;
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
