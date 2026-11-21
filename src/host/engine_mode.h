/* Whether the engine runs with the modernised limits (the Direct3D 11 renderer) or the original ones (the software
 * renderer, on both hosts). Features that only apply to the modern limits read it. */
#ifndef JPT_ENGINE_MODE_H
#define JPT_ENGINE_MODE_H

void engine_limits_keep_original(void);
int engine_limits_original(void);
int engine_limits_modern(void);
int engine_heaps_enlarged(void);

#endif
