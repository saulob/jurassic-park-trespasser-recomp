#include "stream_audio.h"

#include <stdlib.h>
#include <string.h>

#include "stream_vfile.h"

typedef struct {
    HANDLE handle;
    stream_vfile_t* file;
} virtual_file_t;

static struct {
    int ready;
    CRITICAL_SECTION lock;
    virtual_file_t files[STREAM_PACK_MAX_VIEWS];
} S;

static void log_line(const char* line) { host_log("audio", "%s", line); }

static int read_handle(void* ctx, uint64_t offset, void* buf, size_t n) {
    OVERLAPPED at;
    DWORD got = 0;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    return ReadFile((HANDLE)ctx, buf, (DWORD)n, &got, &at) && got == n;
}

void stream_audio_init(void) {
    if (S.ready) return;
    InitializeCriticalSection(&S.lock);
    S.ready = 1;
    stream_catalog_init(log_line);
}

stream_pack_t* stream_audio_open_view(HANDLE file, uint64_t size) {
    stream_audio_init();
    return stream_catalog_open_pack(read_handle, file, size);
}

static int attach(HANDLE file, stream_vfile_t* virtual_file) {
    int attached = 0;
    EnterCriticalSection(&S.lock);
    for (int i = 0; !attached && i < STREAM_PACK_MAX_VIEWS; i++) {
        if (S.files[i].handle) continue;
        S.files[i].handle = file;
        S.files[i].file = virtual_file;
        attached = 1;
    }
    LeaveCriticalSection(&S.lock);
    if (!attached) stream_vfile_close(virtual_file);
    return attached;
}

void stream_audio_attach(HANDLE file, const char* path, DWORD access, DWORD flags, int package_file) {
    LARGE_INTEGER size;
    const int kind = stream_vfile_kind(path, access, flags, package_file);
    stream_vfile_t* virtual_file;
    if (file == INVALID_HANDLE_VALUE || kind < 0) return;
    if (!GetFileSizeEx(file, &size)) return;
    stream_audio_init();
    virtual_file = stream_vfile_open_kind(kind, package_file, read_handle, file, (uint64_t)size.QuadPart);
    if (virtual_file) attach(file, virtual_file);
}

HANDLE stream_audio_open_memory(uint8_t* bytes, size_t size) {
    stream_vfile_t* virtual_file = stream_vfile_open_memory(bytes, size);
    HANDLE file;
    if (!virtual_file) return INVALID_HANDLE_VALUE;
    stream_audio_init();
    file = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        stream_vfile_close(virtual_file);
        return file;
    }
    if (attach(file, virtual_file)) return file;
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
}

void stream_audio_detach(HANDLE file) {
    stream_vfile_t* virtual_file = NULL;
    if (!S.ready) return;
    EnterCriticalSection(&S.lock);
    for (int i = 0; i < STREAM_PACK_MAX_VIEWS; i++) {
        if (S.files[i].handle != file) continue;
        virtual_file = S.files[i].file;
        memset(&S.files[i], 0, sizeof S.files[i]);
    }
    LeaveCriticalSection(&S.lock);
    stream_vfile_close(virtual_file);
}

static virtual_file_t* find_file(HANDLE file) {
    for (int i = 0; i < STREAM_PACK_MAX_VIEWS; i++)
        if (S.files[i].handle == file) return &S.files[i];
    return NULL;
}

void shim_ReadFile(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    void* buffer = GUEST_PTR(1);
    DWORD wanted = GUEST_ARG(2);
    DWORD* read = (DWORD*)GUEST_PTR(3);
    OVERLAPPED* overlapped = (OVERLAPPED*)GUEST_PTR(4);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = (uint32_t)ReadFile(file, buffer, wanted, read, overlapped);
        GUEST_RETURN_STDCALL(5);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = overlapped ? NULL : find_file(file);
    if (v) {
        uint32_t got = 0;
        int ok = stream_vfile_read(v->file, buffer, wanted, &got);
        LeaveCriticalSection(&S.lock);
        if (read) *read = got;
        if (!ok) SetLastError(ERROR_READ_FAULT);
        g_eax = (uint32_t)ok;
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = (uint32_t)ReadFile(file, buffer, wanted, read, overlapped);
    }
    GUEST_RETURN_STDCALL(5);
}

void shim_SetFilePointer(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    LONG low = (LONG)GUEST_ARG(1);
    LONG* high = (LONG*)GUEST_PTR(2);
    DWORD method = GUEST_ARG(3);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = SetFilePointer(file, low, high, method);
        GUEST_RETURN_STDCALL(4);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = find_file(file);
    if (v) {
        int64_t distance = high ? ((int64_t)*high << 32) | (uint32_t)low : (int64_t)low;
        uint64_t target = 0;
        int ok = stream_vfile_seek(v->file, distance, method, &target);
        LeaveCriticalSection(&S.lock);
        if (!ok) {
            SetLastError(ERROR_NEGATIVE_SEEK);
            g_eax = INVALID_SET_FILE_POINTER;
        } else {
            if (high) *high = (LONG)(target >> 32);
            SetLastError(NO_ERROR);
            g_eax = (uint32_t)target;
        }
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = SetFilePointer(file, low, high, method);
    }
    GUEST_RETURN_STDCALL(4);
}

void shim_GetFileSize(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    DWORD* high = (DWORD*)GUEST_PTR(1);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = GetFileSize(file, high);
        GUEST_RETURN_STDCALL(2);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = find_file(file);
    if (v) {
        uint64_t size = stream_vfile_size(v->file);
        LeaveCriticalSection(&S.lock);
        if (high) *high = (DWORD)(size >> 32);
        SetLastError(NO_ERROR);
        g_eax = (uint32_t)size;
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = GetFileSize(file, high);
    }
    GUEST_RETURN_STDCALL(2);
}
