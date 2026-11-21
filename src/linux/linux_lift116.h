#ifndef JPT_LINUX_LIFT116_H
#define JPT_LINUX_LIFT116_H

#include "jpt_lift_meta.h"
#include "linux_modules.h"
#include "linux_spine.h"

lx_table_t lx_lift116_main_table(void);
lx_table_t lx_lift116_selfmod_table(void);
const jpt_lift_meta_t* lx_lift116_meta(void);
/* The guest DLLs lift116 carries. */
const lx_module_def_t* lx_lift116_modules(size_t* count);

#endif
