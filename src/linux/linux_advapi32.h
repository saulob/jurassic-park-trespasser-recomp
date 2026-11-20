#ifndef JPT_LINUX_ADVAPI32_H
#define JPT_LINUX_ADVAPI32_H

#include <stddef.h>

#include "linux_hle.h"

const hle_entry_t* hle_advapi32_table(size_t* count);
/* The value store lives in saves_dir; the two path values are guest paths. */
void hle_advapi32_init(const char* saves_dir, const char* data_drive, const char* installed_directory);

#endif
