#ifndef TRESPASSER_HOST_H
#define TRESPASSER_HOST_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "native32.h"

/* Emitted by tools/run_lift.py into recomp_dispatch.c. */
extern const uint32_t trespasser_entry_va;
extern const uint32_t trespasser_image_base;
extern const uint32_t trespasser_text_start, trespasser_text_end;
extern const uint32_t trespasser_selfmod_start, trespasser_selfmod_end;
extern const char trespasser_expected_sha256[];

/* A shim is entered with g_esp at [return slot][arg0][arg1]... */
#define GUEST_ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define GUEST_PTR(n) ((void*)(uintptr_t)GUEST_ARG(n))
#define GUEST_RETURN_STDCALL(nargs) (g_esp += 4 + 4 * (nargs))

int host_log_open(const char* dir, const char* file);
const char* host_log_path(void);
void host_log(const char* tag, const char* fmt, ...);
void host_log_raw(const char* text, unsigned len);

int host_sha256_file(const char* path, char hex[65]);

void guest_env_init(const char* game_dir, const char* guest_args);
const char* guest_env_exe_path(void);
int guest_env_apply_command_line(void);
void shim_GetModuleHandleA(void);
void shim_GetModuleFileNameA(void);
void shim_MessageBoxA(void);
void shim_DialogBoxParamA(void);
void shim_OutputDebugStringA(void);

void guest_registry_init(const char* game_dir);
void shim_RegOpenKeyExA(void);
void shim_RegCreateKeyA(void);
void shim_RegQueryValueExA(void);
void shim_RegSetValueExA(void);
void shim_RegDeleteValueA(void);
void shim_RegCloseKey(void);

int io_trace_init(void);
void shim_CreateFileA(void);
void shim_GetFileAttributesA(void);
void shim_FindFirstFileA(void);
void shim_LZOpenFileA(void);
void shim_LoadLibraryA(void);
void shim_fopen(void);
void shim_open(void);

void milestones_init(void);
void milestones_observe(uint32_t target);

void ddraw_trace_init(void);
recomp_func_t ddraw_trace_lookup(uint32_t va);
int ddraw_trace_name(uint32_t va, char* out, int cap);
int ddraw_trace_last_call(char* out, int cap);
const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap);

int ddraw_compat_init(int enabled);
recomp_func_t ddraw_compat_lookup(uint32_t va);
int ddraw_compat_last_call(char* out, int cap);

int selfmod_init(void);
recomp_func_t selfmod_lookup(uint32_t va);
unsigned long selfmod_entries(void);

void diagnostics_install(unsigned watchdog_seconds);

enum {
    HOST_EXIT_SETUP = 1,
    HOST_EXIT_NOT_LIFTED = 2,
    HOST_EXIT_FAULT = 3,
    HOST_EXIT_WATCHDOG = 4,
    HOST_EXIT_UNRESOLVED = 5
};

#endif
