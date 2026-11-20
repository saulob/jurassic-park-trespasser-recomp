/* lz32.dll: the game reads its scene and world files through LZOpenFileA/LZRead/LZSeek/LZClose. A file that starts with the SZDD
 * header is expanded when it is opened; any other file is read as it is, which is what LZOpenFile does. */
#ifndef JPT_LINUX_LZ32_H
#define JPT_LINUX_LZ32_H

#include <stddef.h>

#include "linux_hle.h"

const hle_entry_t* hle_lz32_table(size_t* count);
void hle_lz32_shutdown(void);
unsigned lx_lz32_open_count(void);

#endif
