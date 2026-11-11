/*
 * File and module lookups are forwarded unchanged and logged: a game that cannot find
 * its data says nothing about why. Successes are capped; failures have a higher cap.
 */
#include "host.h"

#include <lzexpand.h>
#include <stdio.h>
#include <string.h>

#define SUCCESS_LINE_LIMIT 300
#define FAILURE_LINE_LIMIT 1000

typedef void* (__cdecl* crt_fopen_t)(const char*, const char*);
typedef int (__cdecl* crt_open_t)(const char*, int, int);

static crt_fopen_t s_crt_fopen;
static crt_open_t s_crt_open;
static LONG s_success_lines, s_failure_lines;

int io_trace_init(void) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (!crt) return 0;
    s_crt_fopen = (crt_fopen_t)GetProcAddress(crt, "fopen");
    s_crt_open = (crt_open_t)GetProcAddress(crt, "_open");
    return s_crt_fopen && s_crt_open;
}

static void report(const char* api, const char* path, int ok, DWORD error) {
    LONG* lines = ok ? &s_success_lines : &s_failure_lines;
    LONG limit = ok ? SUCCESS_LINE_LIMIT : FAILURE_LINE_LIMIT;
    LONG n = InterlockedIncrement(lines);
    if (n > limit) return;
    if (ok)
        host_log("file", "%s(\"%s\") ok", api, path ? path : "(null)");
    else
        host_log("file", "%s(\"%s\") FAILED, error %lu, from sub_%08X", api, path ? path : "(null)",
                 error, g_cur_func);
    if (n == limit) host_log("file", "%s line limit reached; later ones are not logged", ok ? "success" : "failure");
    SetLastError(error);
}

void shim_CreateFileA(void) {
    const char* path = (const char*)GUEST_PTR(0);
    HANDLE h = CreateFileA(path, GUEST_ARG(1), GUEST_ARG(2), (LPSECURITY_ATTRIBUTES)GUEST_PTR(3),
                           GUEST_ARG(4), GUEST_ARG(5), (HANDLE)GUEST_PTR(6));
    report("CreateFileA", path, h != INVALID_HANDLE_VALUE, GetLastError());
    g_eax = (uint32_t)(uintptr_t)h;
    GUEST_RETURN_STDCALL(7);
}

void shim_GetFileAttributesA(void) {
    const char* path = (const char*)GUEST_PTR(0);
    DWORD attributes = GetFileAttributesA(path);
    report("GetFileAttributesA", path, attributes != INVALID_FILE_ATTRIBUTES, GetLastError());
    g_eax = attributes;
    GUEST_RETURN_STDCALL(1);
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
    INT h = LZOpenFileA(path, (OFSTRUCT*)GUEST_PTR(1), (WORD)GUEST_ARG(2));
    report("LZOpenFileA", path, h >= 0, GetLastError());
    g_eax = (uint32_t)h;
    GUEST_RETURN_STDCALL(3);
}

void shim_LoadLibraryA(void) {
    const char* name = (const char*)GUEST_PTR(0);
    HMODULE module = LoadLibraryA(name);
    report("LoadLibraryA", name, module != NULL, GetLastError());
    g_eax = (uint32_t)(uintptr_t)module;
    GUEST_RETURN_STDCALL(1);
}

/* cdecl: the lifted caller removes the arguments. The FILE* and descriptor belong to
 * msvcrt.dll, so msvcrt's own functions must create them. */
void shim_fopen(void) {
    const char* path = (const char*)GUEST_PTR(0);
    void* file = s_crt_fopen(path, (const char*)GUEST_PTR(1));
    report("fopen", path, file != NULL, GetLastError());
    g_eax = (uint32_t)(uintptr_t)file;
    g_esp += 4;
}

void shim_open(void) {
    const char* path = (const char*)GUEST_PTR(0);
    int fd = s_crt_open(path, (int)GUEST_ARG(1), (int)GUEST_ARG(2));
    report("_open", path, fd >= 0, GetLastError());
    g_eax = (uint32_t)fd;
    g_esp += 4;
}
