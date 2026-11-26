/* Ends a custom level's load before the engine does what it cannot survive. */
#ifndef JPT_LEVEL_ABORT_H
#define JPT_LEVEL_ABORT_H

#include <stdint.h>

/* used, asked and capacity in bytes. Neither returns. */
void level_limit_exceeded(const char* what, uint32_t at, uint32_t used, uint32_t asked, uint32_t capacity);
void level_allocation_failed(const char* what, uint32_t at, uint32_t bytes);

#endif
