/* John Hammond's narration in Stream.tpa. The level scenes name his voice-over samples VH<number> with an optional
 * letter (VH0 to VH299d); Anne's are VA<number>, and a conversation of both is VC H<number> - A<number>. A sample's ID
 * is the CRC-32 of its lower-case name (jpt_sample_id). */
#ifndef JPT_HAMMOND_H
#define JPT_HAMMOND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the sample ID belongs to one of Hammond's narration samples. */
int jpt_hammond_sample(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif
