#include "audio_pacer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PERIOD_SECONDS_TIMES_10000 116u
#define CADENCE_STEP_DIVISOR 32

uint32_t audio_pacer_period_frames(uint32_t rate) {
    double want = (double)rate * PERIOD_SECONDS_TIMES_10000 / 10000.0;
    uint32_t best = 64, candidate;
    double best_ratio = 1e30;
    for (candidate = 64; candidate <= 4096; candidate *= 2) {
        double ratio = (double)candidate > want ? (double)candidate / want : want / (double)candidate;
        if (ratio < best_ratio) best_ratio = ratio, best = candidate;
    }
    return best;
}

static void configure(audio_pacer_t* p, uint32_t rate) {
    p->rate = rate ? rate : 22050u;
    p->period_frames = audio_pacer_period_frames(p->rate);
    p->period_ns = (int64_t)p->period_frames * 1000000000ll / (int64_t)p->rate;
    p->started = 0;
    p->adjust_ns = 0;
    p->window_ticks = 0;
    p->window_underruns = 0;
}

void audio_pacer_init(audio_pacer_t* p, uint32_t rate, uint32_t frame_bytes) {
    memset(p, 0, sizeof *p);
    p->frame_bytes = frame_bytes ? frame_bytes : 8u;
    p->target_periods = AUDIO_PACER_MIN_PERIODS;
    p->stats.target_periods = p->stats.target_periods_max_seen = p->target_periods;
    p->stats.queue_min_bytes = 0xFFFFFFFFu;
    configure(p, rate);
}

void audio_pacer_set_rate(audio_pacer_t* p, uint32_t rate) {
    if (rate) configure(p, rate);
}

uint32_t audio_pacer_queue_limit_bytes(const audio_pacer_t* p) {
    return (AUDIO_PACER_MAX_PERIODS + AUDIO_PACER_BACKLOG_EXTRA_PERIODS) * p->period_frames * p->frame_bytes;
}

double audio_pacer_bytes_to_ms(const audio_pacer_t* p, uint32_t bytes) {
    return (double)bytes * 1000.0 / ((double)p->rate * (double)p->frame_bytes);
}

static void grow_target(audio_pacer_t* p) {
    if (p->target_periods < AUDIO_PACER_MAX_PERIODS) p->target_periods++;
    p->stats.target_periods = p->target_periods;
    if (p->target_periods > p->stats.target_periods_max_seen) p->stats.target_periods_max_seen = p->target_periods;
    p->calm_windows = 0;
}

static void steer_window(audio_pacer_t* p, uint32_t queued, uint32_t low, uint32_t period_bytes) {
    if (p->window_ticks == 0 || queued < p->window_min) p->window_min = queued;
    if (++p->window_ticks < AUDIO_PACER_WINDOW_TICKS) return;
    p->adjust_ns = p->window_min > low + 2u * period_bytes ? p->period_ns / CADENCE_STEP_DIVISOR
                   : p->window_min < low                    ? -(p->period_ns / CADENCE_STEP_DIVISOR)
                                                            : 0;
    if (p->window_underruns) p->calm_windows = 0;
    else if (++p->calm_windows >= AUDIO_PACER_SHRINK_WINDOWS && p->target_periods > AUDIO_PACER_MIN_PERIODS) {
        p->target_periods--;
        p->stats.target_periods = p->target_periods;
        p->calm_windows = 0;
    }
    p->window_ticks = 0;
    p->window_underruns = 0;
}

audio_plan_t audio_pacer_tick(audio_pacer_t* p, int64_t now, uint32_t queued) {
    audio_plan_t plan;
    uint32_t period_bytes = p->period_frames * p->frame_bytes, low;
    memset(&plan, 0, sizeof plan);
    if (!p->started) {
        p->started = 1;
        p->next_ns = now;
        p->warmup = AUDIO_PACER_WARMUP_TICKS;
    } else {
        int64_t gap = now - p->last_ns;
        if (gap > p->stats.max_gap_ns) p->stats.max_gap_ns = gap;
        if (now > p->next_ns + AUDIO_PACER_STALL_PERIODS * p->period_ns) {
            p->next_ns = now;
            p->adjust_ns = 0;
            p->window_ticks = 0;
            p->stats.producer_stalls++;
        }
    }
    p->last_ns = now;
    p->stats.ticks++;
    if (queued > audio_pacer_queue_limit_bytes(p)) {
        plan.drop = 1;
        p->stats.backlog_drops++;
        p->stats.backlog_dropped_frames += queued / p->frame_bytes;
        queued = 0;
        p->warmup = AUDIO_PACER_WARMUP_TICKS;
        p->window_ticks = 0;
    }
    if (queued > p->stats.queue_max_bytes) p->stats.queue_max_bytes = queued;
    if (!p->warmup && queued < p->stats.queue_min_bytes) p->stats.queue_min_bytes = queued;
    if (queued < period_bytes) {
        if (!p->warmup) {
            p->stats.underruns++;
            p->window_underruns++;
            grow_target(p);
        }
    }
    low = p->target_periods * period_bytes;
    if (queued < period_bytes) {
        plan.silence_frames = (low - queued) / p->frame_bytes;
        p->stats.silence_frames += plan.silence_frames;
        queued = low;
    }
    if (p->warmup) p->warmup--;
    steer_window(p, queued, low, period_bytes);
    plan.mix_frames = p->period_frames;
    p->stats.mixed_frames += plan.mix_frames;
    p->next_ns += p->period_ns + p->adjust_ns;
    plan.next_ns = p->next_ns;
    return plan;
}

int audio_pacer_report(const audio_pacer_t* p, char* out, unsigned capacity) {
    const audio_pacer_stats_t* s = &p->stats;
    uint32_t low = s->queue_min_bytes == 0xFFFFFFFFu ? 0u : s->queue_min_bytes;
    return snprintf(out, capacity,
                    "queue target %u periods (%.1f ms), peak target %u, queued min %.1f ms max %.1f ms, underruns %" PRIu64 ", backlog drops %" PRIu64 " (%.0f ms), "
                    "producer stalls %" PRIu64 ", longest wake-up gap %.1f ms, %" PRIu64 " wake-ups",
                    (unsigned)s->target_periods, audio_pacer_bytes_to_ms(p, s->target_periods * p->period_frames * p->frame_bytes),
                    (unsigned)s->target_periods_max_seen, audio_pacer_bytes_to_ms(p, low), audio_pacer_bytes_to_ms(p, s->queue_max_bytes),
                    s->underruns, s->backlog_drops,
                    (double)s->backlog_dropped_frames * 1000.0 / (double)p->rate,
                    s->producer_stalls, (double)s->max_gap_ns / 1e6, s->ticks);
}
