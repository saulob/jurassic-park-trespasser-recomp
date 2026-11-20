/* Guest DLLs the lift carries (tools/lift_module.py): each is the installed DLL mapped at its base in the guest window, verified
 * by SHA-256, with its code run as lifted C through its own dispatch table. A module is identified by its guest base (its HMODULE),
 * never by a host pointer. Nothing is loaded by the host's loader and no DLL byte is executed. */
#ifndef JPT_LINUX_MODULES_H
#define JPT_LINUX_MODULES_H

#include <stddef.h>
#include <stdint.h>

#include "guest_memory.h"
#include "jpt_module_meta.h"
#include "jpt_vpath.h"
#include "linux_spine.h"

typedef struct {
    const jpt_module_meta_t* meta;
    lx_table_t table;
} lx_module_def_t;

typedef struct {
    char name[32]; /* lower case, without .dll */
    const jpt_module_meta_t* meta;
    gm_image_t image;
    lx_table_t table;
    int refcount;
    unsigned attach_count, detach_count;
} lx_module_t;

/* Drops every module (the guest window is about to go). */
void lx_modules_reset(void);
/* Maps each module's installed DLL (versions/<version>/<file>, then the game folder) at its base. 0 with the reason in err. */
int lx_modules_map(const lx_module_def_t* defs, size_t count, jpt_vpath_t* vpath, const char* version, char* err, size_t err_len);
/* Binds each module's imports through the host's import resolver and registers its dispatch table. */
int lx_modules_bind(unsigned* bound, unsigned* implemented);

lx_module_t* lx_module_find(const char* name_or_path);
lx_module_t* lx_module_by_base(uint32_t base);
size_t lx_module_count(void);
lx_module_t* lx_module_at(size_t index);

/* A guest address for a module's export by name or ordinal (name NULL), or 0. Code exports are lifted entry points, data exports
 * addresses inside the mapped image. */
uint32_t lx_module_export(const lx_module_t* m, const char* name, uint16_t ordinal);
/* hle_resolve's view: 1 when dll names a loaded module (*va = its export, 0 when it has none). */
int lx_module_resolve(const char* dll, const char* name, uint16_t ordinal, uint32_t* va);

/* The process's LoadLibrary / FreeLibrary: DllMain(DLL_PROCESS_ATTACH) when the count goes 0 -> 1, DLL_PROCESS_DETACH on 1 -> 0.
 * Modules the executable imports start at 1 and are attached by lx_modules_attach_imported before its entry point runs. */
uint32_t lx_module_load(lx_module_t* m);
int lx_module_free(lx_module_t* m);
int lx_modules_attach_imported(void);

#endif
