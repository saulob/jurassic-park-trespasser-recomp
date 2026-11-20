/* What a lifted guest DLL says about itself (tools/lift_module.py): the DLL it was lifted from, where the lifted code assumes it
 * sits, and its function count. The program maps the installed DLL only when its SHA-256 is the recorded one. */
#ifndef JPT_MODULE_META_H
#define JPT_MODULE_META_H

#include <stdint.h>

typedef struct {
    const char* name;               /* msvcirt */
    const char* file_name;          /* MSVCIRT.DLL */
    const char* sha256;             /* of the supported DLL */
    const char* export_fingerprint; /* SHA-256 of "ordinal name rva" lines */
    uint32_t image_base, image_size, entry_point, text_start, text_end, function_count, export_count;
} jpt_module_meta_t;

#endif
