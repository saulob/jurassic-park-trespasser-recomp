#ifndef JPT_LINUX_MSVCRT_H
#define JPT_LINUX_MSVCRT_H

#include <stddef.h>
#include <stdint.h>

#include "linux_hle.h"

/* The msvcrt.dll imports the guest has reached, over the host's C library, with Win32 32-bit layouts at the boundary. */
const hle_entry_t* hle_msvcrt_table(size_t* count);
void hle_msvcrt_init(const char* command_line);
/* Guest address of an msvcrt data export (`_acmdln`, `_adjust_fdiv`), or 0. */
uint32_t hle_msvcrt_data(const char* name);

/* The file, descriptor and formatting functions (linux_msvcrt_io.c) and the _iob, __pioinfo and __badioinfo data. */
const hle_entry_t* hle_msvcrt_io_table(size_t* count);
void hle_msvcrt_io_init(void);
uint32_t hle_msvcrt_io_data(const char* name);

#endif
