#ifndef JPT_PERF_STAT_H
#define JPT_PERF_STAT_H

#include <stddef.h>

/* Duration samples in milliseconds: a count, a sum, a maximum and a 0.25 ms histogram whose last bin takes the rest. */
#define PERF_STAT_BINS 480u
#define PERF_STAT_BIN_MS 0.25

typedef struct {
    unsigned long long n;
    double sum, max;
    unsigned bins[PERF_STAT_BINS];
} perf_stat_t;

static inline void perf_stat_add(perf_stat_t* s, double ms) {
    size_t bin;
    if (!(ms >= 0.0) || ms > 1e9) return;
    bin = (size_t)(ms / PERF_STAT_BIN_MS);
    if (bin >= PERF_STAT_BINS) bin = PERF_STAT_BINS - 1u;
    s->bins[bin]++;
    s->n++;
    s->sum += ms;
    if (ms > s->max) s->max = ms;
}

static inline double perf_stat_mean(const perf_stat_t* s) { return s->n ? s->sum / (double)s->n : 0.0; }

/* The upper edge of the bin holding the fraction p of the samples (0 with none). */
static inline double perf_stat_percentile(const perf_stat_t* s, double p) {
    unsigned long long want, seen = 0;
    if (!s->n) return 0.0;
    want = (unsigned long long)(p * (double)s->n);
    if (want >= s->n) want = s->n - 1u;
    for (size_t i = 0; i < PERF_STAT_BINS; i++) {
        seen += s->bins[i];
        if (seen > want) return (double)(i + 1u) * PERF_STAT_BIN_MS;
    }
    return s->max;
}

#endif
