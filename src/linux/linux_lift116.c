#include "linux_lift116.h"

#include "recomp_types.h"

extern const recomp_dispatch_entry_t lift116_dispatch_table[];
extern const jpt_lift_meta_t lift116_meta;
extern const recomp_dispatch_entry_t lift116_selfmod_dispatch_table[];
extern const unsigned lift116_selfmod_dispatch_count;

typedef char lx_entry_layout_matches_recomp[(sizeof(lx_entry_t) == sizeof(recomp_dispatch_entry_t) &&
                                             offsetof(lx_entry_t, fn) == offsetof(recomp_dispatch_entry_t, func))
                                                ? 1 : -1];

extern const recomp_dispatch_entry_t msvcirt_dispatch_table[];
extern const jpt_module_meta_t msvcirt_meta;

const lx_module_def_t* lx_lift116_modules(size_t* count) {
    static lx_module_def_t defs[1];
    defs[0].meta = &msvcirt_meta;
    defs[0].table.entries = (const lx_entry_t*)(const void*)msvcirt_dispatch_table;
    defs[0].table.count = msvcirt_meta.function_count;
    *count = 1;
    return defs;
}

lx_table_t lx_lift116_main_table(void) {
    lx_table_t t = { (const lx_entry_t*)(const void*)lift116_dispatch_table, lift116_meta.function_count };
    return t;
}

lx_table_t lx_lift116_selfmod_table(void) {
    lx_table_t t = { (const lx_entry_t*)(const void*)lift116_selfmod_dispatch_table, lift116_selfmod_dispatch_count };
    return t;
}

const jpt_lift_meta_t* lx_lift116_meta(void) { return &lift116_meta; }
