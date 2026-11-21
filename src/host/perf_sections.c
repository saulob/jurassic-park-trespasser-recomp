#include "perf_sections.h"

#ifdef _WIN32
#include <windows.h>

static LONG64 s_ticks[PS_COUNT];
static LONG64 s_calls[PS_COUNT];

int64_t perf_section_begin(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void perf_section_end(perf_section_t section, int64_t begin) {
    InterlockedExchangeAdd64(&s_ticks[section], perf_section_begin() - begin);
    InterlockedIncrement64(&s_calls[section]);
}

double perf_section_total_ms(perf_section_t section) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)s_ticks[section] * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>

static int64_t s_ticks[PS_COUNT];
static int64_t s_calls[PS_COUNT];

int64_t perf_section_begin(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

void perf_section_end(perf_section_t section, int64_t begin) {
    __atomic_fetch_add(&s_ticks[section], perf_section_begin() - begin, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_calls[section], 1, __ATOMIC_RELAXED);
}

double perf_section_total_ms(perf_section_t section) { return (double)s_ticks[section] / 1e6; }
#endif

void perf_section_commit(perf_section_t section, int64_t begin, int64_t end) {
    s_ticks[section] += end - begin;
    s_calls[section]++;
}

unsigned long long perf_section_calls(perf_section_t section) { return (unsigned long long)s_calls[section]; }

const char* perf_section_name(perf_section_t section) {
    static const char* const names[PS_COUNT] = { "UI loop (gamepad, overlay, debug menu, CE)", "objectives state",
                                                 "minimap state", "cheats state",
                                                 "HUD/debug overlay draw (minimap, compass, hints, panels)",
                                                 "AI system (CAISystem::Process, brains thinking)",
                                                 "terrain preparation (CQuadRootTIN::Update)",
                                                 "CPU polygons (DrawPolygon: setup, walkers, spans, Locks)",
                                                 "texture management (UploadPolygonArray)",
                                                 "physics: Anne's skeleton step (Pelvic_IDOF)",
                                                 "physics: dinosaurs' skeleton steps (Pelvic_IDOF)" };
    return names[section];
}
