#ifndef TRESPASSER_AUDIO_PACER_H
#define TRESPASSER_AUDIO_PACER_H

#include <stdint.h>

/* The cadence and queue policy between the mixer and the device. A producer thread wakes once per period on a steady
 * clock, mixes one period and pushes it to a queue the device drains in its own quanta; this keeps the queue between a
 * low mark (the target) and a backlog limit, so a device that pulls in bursts, a late wake-up or a short stall never
 * reaches silence, and a device that stops pulling never leaves seconds of audio behind. No SDL, no clock of its own:
 * the caller passes the time and the queue size, so the same code runs in the backend and in the stress tests. */

#define AUDIO_PACER_POLICY_VERSION 1

enum {
    AUDIO_PACER_MIN_PERIODS = 2,
    AUDIO_PACER_MAX_PERIODS = 8,
    AUDIO_PACER_BACKLOG_EXTRA_PERIODS = 5,
    AUDIO_PACER_WINDOW_TICKS = 32,
    AUDIO_PACER_WARMUP_TICKS = 4,
    AUDIO_PACER_STALL_PERIODS = 8,
    AUDIO_PACER_SHRINK_WINDOWS = 160
};

typedef struct {
    uint64_t ticks;
    uint64_t underruns;
    uint64_t backlog_drops;
    uint64_t backlog_dropped_frames;
    uint64_t producer_stalls;
    uint64_t silence_frames;
    uint64_t mixed_frames;
    int64_t max_gap_ns;
    uint32_t queue_min_bytes;
    uint32_t queue_max_bytes;
    uint32_t target_periods;
    uint32_t target_periods_max_seen;
} audio_pacer_stats_t;

typedef struct {
    uint32_t rate;
    uint32_t frame_bytes;
    uint32_t period_frames;
    int64_t period_ns;
    uint32_t target_periods;
    int started;
    int64_t next_ns;
    int64_t last_ns;
    int64_t adjust_ns;
    uint32_t warmup;
    uint32_t window_ticks;
    uint32_t window_min;
    uint32_t window_underruns;
    uint32_t calm_windows;
    audio_pacer_stats_t stats;
} audio_pacer_t;

typedef struct {
    int drop;                /* clear the device queue before anything else */
    uint32_t silence_frames; /* then push this many frames of silence */
    uint32_t mix_frames;     /* then mix and push this many frames */
    int64_t next_ns;         /* wake up again at this time */
} audio_plan_t;

/* The frames mixed per wake-up for a mixer rate: a power of two close to 11 ms. */
uint32_t audio_pacer_period_frames(uint32_t rate);

void audio_pacer_init(audio_pacer_t* pacer, uint32_t rate, uint32_t frame_bytes);
/* A new mixer rate: the period follows it, the queue is rebuilt from the next wake-up, the statistics stay. */
void audio_pacer_set_rate(audio_pacer_t* pacer, uint32_t rate);
/* One wake-up: `queued_bytes` is what the device has not taken yet. */
audio_plan_t audio_pacer_tick(audio_pacer_t* pacer, int64_t now_ns, uint32_t queued_bytes);

uint32_t audio_pacer_queue_limit_bytes(const audio_pacer_t* pacer);
double audio_pacer_bytes_to_ms(const audio_pacer_t* pacer, uint32_t bytes);
/* One log line: target, queue extremes, underruns, backlog drops, the longest wake-up gap. */
int audio_pacer_report(const audio_pacer_t* pacer, char* out, unsigned capacity);

#endif
