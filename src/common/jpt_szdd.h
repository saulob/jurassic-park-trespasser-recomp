/* The SZDD compression of expand.exe, which the game's LZ calls read. */
#ifndef JPT_SZDD_H
#define JPT_SZDD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_SZDD_HEADER 14

int jpt_szdd_is(const uint8_t* in, size_t in_size);
/* The expanded bytes (to free); NULL when the data is not SZDD or there is no memory for it. */
uint8_t* jpt_szdd_expand(const uint8_t* in, size_t in_size, size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
