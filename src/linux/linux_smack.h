/* smackw32.dll on Linux: the portable Smacker HLE (src/host/video) bound to the guest's imports. The original DLL is never
 * loaded; pictures and sound come from FFmpeg's Smacker decoder when the build has it. */
#ifndef JPT_LINUX_SMACK_H
#define JPT_LINUX_SMACK_H

#include <stddef.h>

#include "linux_hle.h"

const hle_entry_t* hle_smack_table(size_t* count);
void hle_smack_init(void);
void hle_smack_shutdown(void);
/* 1 when the build can decode Smacker video (FFmpeg linked in). */
int lx_smack_can_decode(void);

#endif
