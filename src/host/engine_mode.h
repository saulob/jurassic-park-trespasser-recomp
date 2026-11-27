/* Whether the engine runs with the modernised limits (the Direct3D 11 renderer) or the original ones (the software
 * renderer, on both hosts). Features that only apply to the modern limits read it. */
#ifndef JPT_ENGINE_MODE_H
#define JPT_ENGINE_MODE_H

#include <stdint.h>

void engine_limits_keep_original(void);
int engine_limits_original(void);
int engine_limits_modern(void);
int engine_heaps_enlarged(void);
void engine_heaps_install(void);
/* Bytes to reserve where the engine asks for this many without an address. */
uint32_t engine_heaps_region_bytes(uint32_t bytes);
/* The same for memory it asks for at once, to use whole. */
uint32_t engine_heaps_buffer_bytes(uint32_t bytes);
/* Whether the address is in the guard behind a heap: only a bug of the port writes there. */
int engine_heaps_guard(uint32_t va);

#endif
