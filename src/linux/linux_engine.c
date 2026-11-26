#include "engine_mode.h"

/* The software renderer keeps the original engine limits on both hosts; the modern ones belong to Direct3D 11. */
void engine_limits_keep_original(void) {}

int engine_limits_original(void) { return 1; }

int engine_limits_modern(void) { return 0; }
