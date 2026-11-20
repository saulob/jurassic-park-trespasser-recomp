/* The Win32 API as the Linux host answers it for the guest: each entry is an import the retail game binds by name, taken
 * as the guest called it (arguments from the guest's stack, results in eax) and implemented over the host, with guest
 * structures read and written at their 32-bit Win32 layouts.
 *
 * An import with no entry here is not stubbed: it is bound to a thunk that reports "Linux HLE missing: module!function"
 * and ends the run when the guest calls it. */
#ifndef JPT_LINUX_HLE_H
#define JPT_LINUX_HLE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_thunks.h"
#include "jpt_vpath.h"

#define HLE_EXIT_MISSING 5

typedef struct {
    const char* dll;  /* lower case, with .dll */
    const char* name;
    ht_fn_t fn;
    int argc;         /* argument dwords */
    int conv;         /* HT_STDCALL (the default), HT_CDECL or HT_THISCALL */
} hle_entry_t;

/* vpath resolves the guest's file names (the game folder is its C: drive); image_base and exe_guest_path answer
 * GetModuleHandleA(NULL) and GetModuleFileNameA. Both are borrowed. */
void hle_init(jpt_vpath_t* vpath, uint32_t image_base, const char* exe_guest_path);
void hle_shutdown(void);

size_t hle_entry_count(void);
const hle_entry_t* hle_entry_at(size_t index);
const hle_entry_t* hle_find(const char* dll, const char* name);
/* hle_find for an import by name, or by ordinal when name is NULL ("#17"). */
const hle_entry_t* hle_find_import(const char* dll, const char* name, uint16_t ordinal);

/* gm_resolve_fn: the thunk of an implemented import, or the "missing" thunk (counted in hle_missing_count). */
uint32_t hle_resolve(const char* dll, const char* name, uint16_t ordinal, void* user);
unsigned hle_missing_count(void);
/* What happens when a missing import is called, or ExitProcess: the default ends the process with the code. */
void hle_set_exit_handler(void (*handler)(uint32_t code));
/* Ends the guest process through the exit handler (ExitProcess). */
void hle_exit_process(uint32_t code);
/* GetLastError of the calling guest thread, for the Win32 layers that live outside linux_hle.c. */
void hle_set_last_error(uint32_t error);
/* The run is over: every guest thread still alive ends at its next blocking call or never starts. Called before the guest's memory goes. */
void hle_retire_guest_threads(void);
/* Every guest thread of the run ends and is waited for (the caller must not hold the machine). 0 when some did not end in timeout_ms:
 * guest memory must then stay mapped. */
int hle_join_guest_threads(unsigned timeout_ms);
/* Diagnostic mode for headless inventory only: an import with no implementation but a known signature (linux_signatures.c)
 * returns 0 and is logged instead of ending the run. A run that used it proves nothing about the guest's behaviour. */
void hle_set_permissive(int on);
unsigned hle_stubbed_count(void);
int hle_signature(const char* dll, const char* name, int* argc, int* conv);
/* The host file descriptor behind a guest file HANDLE, -1 when it is not one. */
int hle_file_fd(uint32_t handle);
/* The game folder as the guest sees it. */
jpt_vpath_t* hle_vpath(void);

#endif
