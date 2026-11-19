#ifndef TRESPASSER_WINDOWS_DSOUND_IMPORTS_H
#define TRESPASSER_WINDOWS_DSOUND_IMPORTS_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The game and SMACKW32.DLL both load DSound.dll and take DirectSoundCreate from it at run time. The host answers both
 * with a module handle of its own and its DirectSoundCreate, so the system DSound.dll is never loaded. */

/* The module handle every request for DSound.dll gets. */
HMODULE dsound_host_module(void);
/* GetProcAddress for that handle: DirectSoundCreate, NULL for anything else. */
FARPROC dsound_host_proc(const char* name);

/* The three kernel32 calls SMACKW32.DLL resolves DirectSound with, filtered for DSound.dll and passed on otherwise. */
HMODULE WINAPI dsound_load_library_a(LPCSTR name);
FARPROC WINAPI dsound_get_proc_address(HMODULE module, LPCSTR name);
BOOL WINAPI dsound_free_library(HMODULE module);

/* Points `module`'s kernel32 imports of LoadLibraryA, GetProcAddress and FreeLibrary at the filtered versions.
 * Returns how many were patched (3 when all are imported). */
int dsound_patch_imports(HMODULE module);
/* One import of `dll` named `function` pointed at `replacement`; 1 when found and written. */
int dsound_patch_import(HMODULE module, const char* dll, const char* function, void* replacement);

#endif
