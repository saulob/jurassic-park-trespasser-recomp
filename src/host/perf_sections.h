#ifndef JPT_PERF_SECTIONS_H
#define JPT_PERF_SECTIONS_H

#include <stdint.h>

/* DEBUG_SAULO: host work timed with the performance counter and summed per section. */
typedef enum {
    PS_UI_LOOP,
    PS_OBJECTIVES,
    PS_MINIMAP_STATE,
    PS_CHEATS,
    PS_HUD_DRAW,
    PS_AI,
    PS_TERRAIN_UPDATE,
    PS_CPU_POLYGONS,
    PS_TEXTURE_UPLOADS,
    PS_PHYSICS_HUMAN_IDOF,
    PS_PHYSICS_ANIMAL_IDOF,
    PS_COUNT
} perf_section_t;

int64_t perf_section_begin(void);
void perf_section_end(perf_section_t section, int64_t begin);
/* A section timed on the guest thread only, from two stamps already taken: no atomics, no second counter read. */
void perf_section_commit(perf_section_t section, int64_t begin, int64_t end);
double perf_section_total_ms(perf_section_t section);
unsigned long long perf_section_calls(perf_section_t section);
const char* perf_section_name(perf_section_t section);

#endif
