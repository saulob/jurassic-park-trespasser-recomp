/*
 * The Windows half of the official Patch 1.1 level fixes: the bytes patch117_level_bytes returns are kept per LZ handle and
 * the game's LZRead, LZSeek and LZClose are served from them (Linux does the same inside its LZ file layer).
 */
#include "host.h"

#include <lzexpand.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VIEWS 4
#define EXPAND_CHUNK (1 << 20)

typedef struct {
    INT handle;
    uint8_t* data;
    LONG size, pos;
} scene_view_t;

static scene_view_t s_views[MAX_VIEWS];

static uint8_t* read_whole(const char* path, LONG* size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER n;
    uint8_t* data = NULL;
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && n.QuadPart < 64 * 1024 * 1024) data = (uint8_t*)malloc((size_t)n.QuadPart);
    if (data && (!ReadFile(f, data, (DWORD)n.QuadPart, &got, NULL) || got != (DWORD)n.QuadPart)) {
        free(data);
        data = NULL;
    }
    CloseHandle(f);
    *size = data ? (LONG)n.QuadPart : 0;
    return data;
}

static scene_view_t* view_of(INT handle) {
    for (int i = 0; i < MAX_VIEWS; i++)
        if (s_views[i].data && s_views[i].handle == handle) return &s_views[i];
    return NULL;
}

static scene_view_t* free_view(void) {
    for (int i = 0; i < MAX_VIEWS; i++)
        if (!s_views[i].data) return &s_views[i];
    return NULL;
}

static void serve(scene_view_t* slot, INT handle, uint8_t* data, LONG size) {
    slot->handle = handle;
    slot->data = data;
    slot->size = size;
    slot->pos = 0;
}

static uint8_t* read_expanded_file(void* context, size_t expanded_size) {
    const char* path = (const char*)context;
    LONG size = (LONG)expanded_size;
    OFSTRUCT opened;
    INT h = LZOpenFileA((LPSTR)path, &opened, OF_READ);
    uint8_t* data = h >= 0 ? (uint8_t*)malloc((size_t)size) : NULL;
    LONG got = 0;
    while (data && got < size) {
        INT n = LZRead(h, (LPSTR)(data + got), size - got < EXPAND_CHUNK ? size - got : EXPAND_CHUNK);
        if (n <= 0) break;
        got += n;
    }
    if (h >= 0) LZClose(h);
    if (data && got != size) {
        free(data);
        data = NULL;
    }
    return data;
}

void patch117_lz_opened(const char* path, int handle) {
    LONG raw_size;
    size_t out_size = 0;
    uint8_t *raw, *out;
    scene_view_t* slot;
    if (handle < 0 || !patch117_file_wanted(path)) return;
    slot = free_view();
    raw = slot ? read_whole(path, &raw_size) : NULL;
    if (!raw) return;
    out = patch117_level_bytes(path, raw, (size_t)raw_size, read_expanded_file, (void*)path, &out_size);
    free(raw);
    if (out) serve(slot, handle, out, (LONG)out_size);
}

void shim_LZRead(void) {
    INT handle = (INT)GUEST_ARG(0);
    char* buffer = (char*)GUEST_PTR(1);
    INT wanted = (INT)GUEST_ARG(2);
    scene_view_t* v = view_of(handle);
    int result;
    if (custom_level_lz_read(handle, buffer, wanted, &result)) {
        g_eax = (uint32_t)result;
    } else if (!v) {
        g_eax = (uint32_t)LZRead(handle, buffer, wanted);
    } else {
        LONG n = wanted < 0 ? 0 : v->size - v->pos < wanted ? v->size - v->pos : wanted;
        memcpy(buffer, v->data + v->pos, (size_t)n);
        v->pos += n;
        g_eax = (uint32_t)n;
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_LZSeek(void) {
    INT handle = (INT)GUEST_ARG(0);
    LONG offset = (LONG)GUEST_ARG(1);
    INT origin = (INT)GUEST_ARG(2);
    scene_view_t* v = view_of(handle);
    int result;
    if (custom_level_lz_seek(handle, offset, origin, &result)) {
        g_eax = (uint32_t)result;
    } else if (!v) {
        g_eax = (uint32_t)LZSeek(handle, offset, origin);
    } else {
        LONG base = origin == 1 ? v->pos : origin == 2 ? v->size : 0, to = base + offset;
        if (origin < 0 || origin > 2 || to < 0 || to > v->size) {
            g_eax = (uint32_t)LZERROR_BADVALUE;
        } else {
            v->pos = to;
            g_eax = (uint32_t)to;
        }
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_LZClose(void) {
    INT handle = (INT)GUEST_ARG(0);
    scene_view_t* v = view_of(handle);
    if (v) {
        free(v->data);
        memset(v, 0, sizeof *v);
    }
    if (!custom_level_lz_close(handle)) LZClose(handle);
    GUEST_RETURN_STDCALL(1);
}
