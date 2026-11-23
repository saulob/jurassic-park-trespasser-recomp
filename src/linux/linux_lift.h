/* The lifted builds linked into the Linux executable: the active version's dispatch tables, metadata, SelfMod table and
 * guest DLLs (the MSVCIRT lift, shared by every version because the installed DLL is the same file). */
#ifndef JPT_LINUX_LIFT_H
#define JPT_LINUX_LIFT_H

#include <stddef.h>

#include "active_profile.h"
#include "jpt_lift_meta.h"
#include "linux_modules.h"
#include "linux_spine.h"

typedef struct {
    jpt_lift_t lift;
    const char* id;
    const jpt_lift_meta_t* meta;
    lx_table_t main;
    lx_table_t selfmod;
    const lx_module_def_t* modules;
    size_t module_count;
} lx_lift_t;

/* The lift a version runs, or NULL for an unknown version or a lift this program does not carry. */
const lx_lift_t* lx_lift_for_version(const jpt_game_version_t* version);
const lx_lift_t* lx_lift_get(jpt_lift_t lift);

#endif
