#include "windows_dsound_imports.h"

#include <string.h>

#include "../audio_policy.h"
#include "windows_dsound_bridge.h"

static const char s_module_token = 0;

HMODULE dsound_host_module(void) { return (HMODULE)&s_module_token; }

FARPROC dsound_host_proc(const char* name) {
    return audio_is_dsound_create(name) ? (FARPROC)jpt_dsound_create : NULL;
}

HMODULE WINAPI dsound_load_library_a(LPCSTR name) {
    if (audio_is_dsound_library(name)) return dsound_host_module();
    return LoadLibraryA(name);
}

FARPROC WINAPI dsound_get_proc_address(HMODULE module, LPCSTR name) {
    if (module == dsound_host_module()) return dsound_host_proc(name);
    return GetProcAddress(module, name);
}

BOOL WINAPI dsound_free_library(HMODULE module) {
    if (module == dsound_host_module()) return TRUE;
    return FreeLibrary(module);
}

static int write_thunk(PIMAGE_THUNK_DATA slot, void* replacement) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    slot->u1.Function = (DWORD_PTR)replacement;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

int dsound_patch_import(HMODULE module, const char* dll, const char* function, void* replacement) {
    BYTE* base = (BYTE*)module;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_DATA_DIRECTORY dir;
    PIMAGE_IMPORT_DESCRIPTOR desc;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    for (desc = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir->VirtualAddress); desc->Name; desc++) {
        PIMAGE_THUNK_DATA names, slots;
        if (_stricmp((const char*)(base + desc->Name), dll)) continue;
        names = (PIMAGE_THUNK_DATA)(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        slots = (PIMAGE_THUNK_DATA)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            PIMAGE_IMPORT_BY_NAME by_name;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            by_name = (PIMAGE_IMPORT_BY_NAME)(base + names->u1.AddressOfData);
            if (!strcmp((const char*)by_name->Name, function)) return write_thunk(slots, replacement);
        }
    }
    return 0;
}

int dsound_patch_imports(HMODULE module) {
    return dsound_patch_import(module, "kernel32.dll", "LoadLibraryA", (void*)dsound_load_library_a) +
           dsound_patch_import(module, "kernel32.dll", "GetProcAddress", (void*)dsound_get_proc_address) +
           dsound_patch_import(module, "kernel32.dll", "FreeLibrary", (void*)dsound_free_library);
}
