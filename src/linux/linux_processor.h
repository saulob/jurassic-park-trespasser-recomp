/* Processor.dll as guest compatibility over the host's CPUID: the DLL is loaded by name and its one export is answered here. */
#ifndef JPT_LINUX_PROCESSOR_H
#define JPT_LINUX_PROCESSOR_H

#include <stddef.h>

#include "linux_hle.h"

const hle_entry_t* hle_processor_table(size_t* count);

#endif
