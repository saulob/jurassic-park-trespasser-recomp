#include "linux_lift.h"

#include <string.h>

#include "recomp_types.h"

extern const recomp_dispatch_entry_t lift116_dispatch_table[];
extern const jpt_lift_meta_t lift116_meta;
extern const recomp_dispatch_entry_t lift116_selfmod_dispatch_table[];
extern const unsigned lift116_selfmod_dispatch_count;

extern const recomp_dispatch_entry_t lift_es117_dispatch_table[];
extern const jpt_lift_meta_t lift_es117_meta;
extern const recomp_dispatch_entry_t lift_es117_selfmod_dispatch_table[];
extern const unsigned lift_es117_selfmod_dispatch_count;

extern const recomp_dispatch_entry_t lift_ptbr117_dispatch_table[];
extern const jpt_lift_meta_t lift_ptbr117_meta;
extern const recomp_dispatch_entry_t lift_ptbr117_selfmod_dispatch_table[];
extern const unsigned lift_ptbr117_selfmod_dispatch_count;

extern const recomp_dispatch_entry_t msvcirt_dispatch_table[];
extern const jpt_module_meta_t msvcirt_meta;

static lx_lift_t s_current;

static const lx_module_def_t* msvcirt_modules(size_t* count) {
    static lx_module_def_t defs[1];
    defs[0].meta = &msvcirt_meta;
    defs[0].table.entries = (const lx_entry_t*)(const void*)msvcirt_dispatch_table;
    defs[0].table.count = msvcirt_meta.function_count;
    *count = 1;
    return defs;
}

static int fill(jpt_lift_t lift, lx_lift_t* out) {
    memset(out, 0, sizeof *out);
    out->lift = lift;
    switch (lift) {
    case JPT_LIFT_116:
        out->id = "lift116";
        out->meta = &lift116_meta;
        out->main = (lx_table_t){ (const lx_entry_t*)(const void*)lift116_dispatch_table, lift116_meta.function_count };
        out->selfmod = (lx_table_t){ (const lx_entry_t*)(const void*)lift116_selfmod_dispatch_table,
                                     lift116_selfmod_dispatch_count };
        break;
    case JPT_LIFT_ES117:
        out->id = "lift_es117";
        out->meta = &lift_es117_meta;
        out->main = (lx_table_t){ (const lx_entry_t*)(const void*)lift_es117_dispatch_table, lift_es117_meta.function_count };
        out->selfmod = (lx_table_t){ (const lx_entry_t*)(const void*)lift_es117_selfmod_dispatch_table,
                                     lift_es117_selfmod_dispatch_count };
        break;
    case JPT_LIFT_PTBR117:
        out->id = "lift_ptbr117";
        out->meta = &lift_ptbr117_meta;
        out->main = (lx_table_t){ (const lx_entry_t*)(const void*)lift_ptbr117_dispatch_table,
                                  lift_ptbr117_meta.function_count };
        out->selfmod = (lx_table_t){ (const lx_entry_t*)(const void*)lift_ptbr117_selfmod_dispatch_table,
                                     lift_ptbr117_selfmod_dispatch_count };
        break;
    default: return 0;
    }
    out->modules = msvcirt_modules(&out->module_count);
    return 1;
}

const lx_lift_t* lx_lift_get(jpt_lift_t lift) { return fill(lift, &s_current) ? &s_current : NULL; }

const lx_lift_t* lx_lift_for_version(const jpt_game_version_t* version) {
    jpt_lift_t lift;
    if (!version || !jpt_lift_find(version->lift, &lift)) return NULL;
    return lx_lift_get(lift);
}
