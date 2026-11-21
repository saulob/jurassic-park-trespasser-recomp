/* The queue policy between the mixer and the device (audio_pacer.c), driven by a virtual device on a virtual clock: a device
 * that drains steadily, in bursts, with jitter, with a clock off by half a percent, that stalls for 100 ms and 500 ms, and a
 * producer that wakes late or early. Every frame the simulated mixer makes carries its own number, so a repeated or reordered
 * frame (a stale buffer played again) is seen, and every frame is accounted for: pushed = played + dropped + still queued. SDL's
 * own stream converts 22050 Hz to the device's rate in the last test: the pitch and the length survive. No device, no sound,
 * no Windows header: the same source runs on Windows and Linux. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio_pacer.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

#define RATE 22050u
#define FRAME_BYTES 8u
#define RING 65536u
#define NS 1000000000ll

typedef enum { DEV_STEADY, DEV_BURST, DEV_JITTER, DEV_DRIFT, DEV_STALL, DEV_PRODUCER_LATE, DEV_PRODUCER_EARLY } device_kind_t;

typedef struct {
    device_kind_t kind;
    double seconds;
    double stall_at_s, stall_ms;
    double burst_ms;      /* DEV_BURST: the device pulls this much at once */
    double jitter_ms;     /* DEV_JITTER: each pull is up to this late, and catches up */
    double clock_factor;  /* DEV_DRIFT: the device consumes this many times the nominal rate */
    int naive;            /* the producer pushes one period per wake-up and ignores the queue: the control */
    int late_too;         /* the producer is also late at random */
} scenario_t;

typedef struct {
    uint32_t ring[RING];
    uint32_t head, count;
    uint32_t next_id;
    uint32_t last_played_id;
    uint64_t pushed_frames, played_frames, dropped_frames, silence_pushed;
    uint64_t starved_frames, starved_events, starved_after_warmup;
    uint64_t stale_frames, reordered;
    uint32_t max_queued_bytes;
    uint32_t queued_at_end_bytes;
    double carry;
    uint64_t rng;
} sim_t;

static uint32_t rnd(sim_t* s) {
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 7;
    s->rng ^= s->rng << 17;
    return (uint32_t)(s->rng >> 11);
}

static void ring_push(sim_t* s, uint32_t id) {
    if (s->count >= RING) return;
    s->ring[(s->head + s->count++) % RING] = id;
}

static void push_silence(sim_t* s, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) ring_push(s, 0);
    s->pushed_frames += frames;
    s->silence_pushed += frames;
}

static void push_mix(sim_t* s, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) ring_push(s, ++s->next_id);
    s->pushed_frames += frames;
}

static void consume(sim_t* s, uint32_t frames, int64_t now_ns, int64_t warmup_ns) {
    int starved = 0;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t id;
        if (!s->count) {
            s->starved_frames++;
            starved = 1;
            if (now_ns > warmup_ns) s->starved_after_warmup++;
            continue;
        }
        id = s->ring[s->head];
        s->head = (s->head + 1) % RING;
        s->count--;
        s->played_frames++;
        if (id) {
            if (id == s->last_played_id) s->stale_frames++;
            else if (id < s->last_played_id) s->reordered++;
            s->last_played_id = id;
        }
    }
    if (starved) s->starved_events++;
}

typedef struct {
    sim_t sim;
    audio_pacer_t pacer;
    uint64_t pacer_underruns_after_warmup;
    uint64_t pacer_underruns_before;
    double queued_ms_at_end;
} result_t;

/* One run on the virtual clock. The producer follows exactly the plans the pacer returns; the device is the scenario. */
static void run(const scenario_t* sc, result_t* out) {
    sim_t* s = &out->sim;
    audio_pacer_t* p = &out->pacer;
    int64_t end = (int64_t)(sc->seconds * NS), warmup = 4ll * NS;
    int64_t producer_at = 0, device_at = 0, device_period = 21333333ll, stall_from = -1, stall_to = -1;
    int64_t last_consume = 0;
    uint32_t period_frames;
    int warmup_recorded = 0;
    memset(out, 0, sizeof *out);
    s->rng = 0x9E3779B97F4A7C15ull;
    audio_pacer_init(p, RATE, FRAME_BYTES);
    period_frames = p->period_frames;
    if (sc->kind == DEV_BURST) device_period = (int64_t)(sc->burst_ms * 1e6);
    if (sc->kind == DEV_STALL) stall_from = (int64_t)(sc->stall_at_s * NS), stall_to = stall_from + (int64_t)(sc->stall_ms * 1e6);
    while (producer_at < end || device_at < end) {
        if (producer_at <= device_at && producer_at < end) {
            int64_t now = producer_at;
            audio_plan_t plan = audio_pacer_tick(p, now, s->count * FRAME_BYTES);
            if (sc->naive) {
                plan.drop = 0;
                plan.silence_frames = 0;
            }
            if (plan.drop) {
                s->dropped_frames += s->count;
                s->count = 0;
            }
            push_silence(s, plan.silence_frames);
            push_mix(s, plan.mix_frames);
            if (s->count * FRAME_BYTES > s->max_queued_bytes) s->max_queued_bytes = s->count * FRAME_BYTES;
            producer_at = plan.next_ns;
            if (sc->naive) producer_at = now + p->period_ns;
            if ((sc->kind == DEV_PRODUCER_LATE || sc->late_too) && rnd(s) % 40 == 0) producer_at += (int64_t)(10 + rnd(s) % 25) * 1000000ll;
            else if (sc->kind == DEV_PRODUCER_LATE || sc->late_too) producer_at += (int64_t)(rnd(s) % 4000) * 1000;
            if (sc->kind == DEV_PRODUCER_EARLY) producer_at -= 3000000ll;
            if (producer_at < now) producer_at = now;
        } else if (device_at < end) {
            int64_t now = device_at;
            double wanted = (double)(now - last_consume) * 1e-9 * (double)RATE * (sc->kind == DEV_DRIFT ? sc->clock_factor : 1.0) + s->carry;
            uint32_t frames = (uint32_t)wanted;
            if (stall_from >= 0 && now >= stall_from && now < stall_to) {
                last_consume = now;
                s->carry = 0;
            } else {
                s->carry = wanted - frames;
                last_consume = now;
                consume(s, frames, now, warmup);
            }
            if (!warmup_recorded && now >= warmup) {
                out->pacer_underruns_before = p->stats.underruns;
                warmup_recorded = 1;
            }
            device_at += device_period;
            if (sc->kind == DEV_JITTER) device_at += (int64_t)((double)(rnd(s) % 1000) / 1000.0 * sc->jitter_ms * 1e6);
        } else {
            break;
        }
    }
    out->pacer_underruns_after_warmup = p->stats.underruns - out->pacer_underruns_before;
    s->queued_at_end_bytes = s->count * FRAME_BYTES;
    out->queued_ms_at_end = audio_pacer_bytes_to_ms(p, s->queued_at_end_bytes);
    (void)period_frames;
}

static void invariants(const char* name, const result_t* r, const scenario_t* sc) {
    const sim_t* s = &r->sim;
    char report[400];
    uint64_t accounted = s->played_frames + s->dropped_frames + s->count;
    double expected_mixed = sc->seconds * RATE;
    double slack = (double)r->pacer.period_frames * 2.0 + sc->seconds * RATE * 0.01;
    if (getenv("JPT_TEST_VERBOSE")) {
        audio_pacer_report(&r->pacer, report, sizeof report);
        printf("  %s: starved %llu frames in %llu events, %s\n", name, (unsigned long long)s->starved_frames, (unsigned long long)s->starved_events, report);
    }
    CHECK(s->stale_frames == 0 && s->reordered == 0, "%s: no frame plays twice or out of order (%llu repeated, %llu reordered)", name,
          (unsigned long long)s->stale_frames, (unsigned long long)s->reordered);
    CHECK(accounted == s->pushed_frames, "%s: every pushed frame is played, dropped or still queued (%llu of %llu)", name,
          (unsigned long long)accounted, (unsigned long long)s->pushed_frames);
    CHECK(fabs((double)r->pacer.stats.mixed_frames - expected_mixed) <= slack,
          "%s: the mixer made the right number of frames for the time that passed (%llu, expected %.0f)", name,
          (unsigned long long)r->pacer.stats.mixed_frames, expected_mixed);
    CHECK(s->max_queued_bytes <= audio_pacer_queue_limit_bytes(&r->pacer) + r->pacer.period_frames * FRAME_BYTES * 2u,
          "%s: the queue stays bounded (%.1f ms at most)", name, audio_pacer_bytes_to_ms(&r->pacer, s->max_queued_bytes));
}

static void test_steady_device(void) {
    scenario_t sc = { DEV_STEADY, 30.0, 0, 0, 0, 0, 1.0, 0, 0 };
    result_t r;
    run(&sc, &r);
    invariants("steady 21.3 ms", &r, &sc);
    CHECK(r.sim.starved_after_warmup == 0, "steady 21.3 ms: the device never starves (%llu frames)", (unsigned long long)r.sim.starved_after_warmup);
    CHECK(r.pacer.stats.underruns == 0 && r.pacer.stats.target_periods_max_seen == AUDIO_PACER_MIN_PERIODS,
          "steady 21.3 ms: no underrun, and the target stays at the minimum (%llu underruns, peak target %u)", (unsigned long long)r.pacer.stats.underruns,
          (unsigned)r.pacer.stats.target_periods_max_seen);
    CHECK(audio_pacer_bytes_to_ms(&r.pacer, r.sim.max_queued_bytes) < 60.0, "steady 21.3 ms: latency stays low (%.1f ms queued at most)",
          audio_pacer_bytes_to_ms(&r.pacer, r.sim.max_queued_bytes));
}

static void test_bursty_device(void) {
    static const double bursts[] = { 42.7, 64.0, 85.3 };
    for (unsigned i = 0; i < 3; i++) {
        scenario_t sc = { DEV_BURST, 30.0, 0, 0, bursts[i], 0, 1.0, 0, 0 };
        char name[48];
        result_t r;
        run(&sc, &r);
        snprintf(name, sizeof name, "bursts of %.0f ms", bursts[i]);
        invariants(name, &r, &sc);
        CHECK(r.sim.starved_after_warmup == 0, "%s: after the first seconds the device never starves (%llu frames)", name,
              (unsigned long long)r.sim.starved_after_warmup);
        CHECK(r.pacer_underruns_after_warmup == 0, "%s: the queue grew to absorb the bursts and stopped underrunning (%llu later, %llu at first, target %u)", name,
              (unsigned long long)r.pacer_underruns_after_warmup, (unsigned long long)r.pacer_underruns_before, (unsigned)r.pacer.stats.target_periods);
    }
}

static void test_jitter(void) {
    scenario_t sc = { DEV_JITTER, 40.0, 0, 0, 0, 50.0, 1.0, 0, 0 };
    result_t r;
    run(&sc, &r);
    invariants("50 ms jitter", &r, &sc);
    CHECK(r.sim.starved_after_warmup == 0, "50 ms jitter: no starvation after the first seconds (%llu frames)", (unsigned long long)r.sim.starved_after_warmup);
    CHECK(r.pacer.stats.underruns < 20, "50 ms jitter: the underruns that taught the target are few (%llu)", (unsigned long long)r.pacer.stats.underruns);
}

static void test_clock_drift(void) {
    static const double factors[] = { 1.005, 0.995 };
    for (unsigned i = 0; i < 2; i++) {
        scenario_t sc = { DEV_DRIFT, 120.0, 0, 0, 0, 0, factors[i], 0, 0 };
        char name[48];
        result_t r;
        run(&sc, &r);
        snprintf(name, sizeof name, "device clock x%.3f", factors[i]);
        invariants(name, &r, &sc);
        CHECK(r.sim.starved_after_warmup == 0, "%s: no starvation in two minutes (%llu frames)", name, (unsigned long long)r.sim.starved_after_warmup);
        CHECK(r.queued_ms_at_end < 90.0, "%s: the queue does not drift away (%.1f ms at the end)", name, r.queued_ms_at_end);
    }
}

static void test_stalls(void) {
    scenario_t short_stall = { DEV_STALL, 30.0, 8.0, 100.0, 0, 0, 1.0, 0, 0 };
    scenario_t long_stall = { DEV_STALL, 30.0, 8.0, 500.0, 0, 0, 1.0, 0, 0 };
    result_t r;
    run(&short_stall, &r);
    invariants("100 ms stall", &r, &short_stall);
    CHECK(r.sim.starved_after_warmup == 0, "100 ms stall: the device that stopped taking audio never starves (%llu frames)", (unsigned long long)r.sim.starved_after_warmup);
    CHECK(r.queued_ms_at_end < 90.0, "100 ms stall: the extra latency decays by the end (%.1f ms)", r.queued_ms_at_end);
    run(&long_stall, &r);
    invariants("500 ms stall", &r, &long_stall);
    CHECK(r.pacer.stats.backlog_drops >= 1, "500 ms stall: the backlog is dropped, not played later (%llu drops)", (unsigned long long)r.pacer.stats.backlog_drops);
    CHECK(audio_pacer_bytes_to_ms(&r.pacer, r.sim.max_queued_bytes) < 200.0, "500 ms stall: never more than 200 ms queued (%.1f ms)",
          audio_pacer_bytes_to_ms(&r.pacer, r.sim.max_queued_bytes));
    CHECK(r.sim.starved_after_warmup == 0, "500 ms stall: and nothing starves after it (%llu frames)", (unsigned long long)r.sim.starved_after_warmup);
}

static void test_producer_timing(void) {
    scenario_t late = { DEV_PRODUCER_LATE, 40.0, 0, 0, 0, 0, 1.0, 0, 0 };
    scenario_t early = { DEV_PRODUCER_EARLY, 30.0, 0, 0, 0, 0, 1.0, 0, 0 };
    result_t r;
    run(&late, &r);
    invariants("late producer", &r, &late);
    CHECK(r.sim.starved_after_warmup < 2400, "late producer: a wake-up up to 35 ms late costs little (%llu starved frames in 36 s)", (unsigned long long)r.sim.starved_after_warmup);
    CHECK(r.pacer_underruns_after_warmup <= 3, "late producer: the target adapts, later underruns stay rare (%llu)", (unsigned long long)r.pacer_underruns_after_warmup);
    run(&early, &r);
    invariants("early producer", &r, &early);
    CHECK(r.sim.starved_after_warmup == 0, "early producer: waking 3 ms early changes nothing (%llu starved frames)", (unsigned long long)r.sim.starved_after_warmup);
}

static void test_bursts_with_a_late_producer(void) {
    scenario_t sc = { DEV_BURST, 40.0, 0, 0, 64.0, 0, 1.0, 0, 1 };
    result_t r;
    run(&sc, &r);
    invariants("64 ms bursts and a late producer", &r, &sc);
    CHECK(r.sim.starved_after_warmup < 2400 && r.pacer_underruns_after_warmup <= 3,
          "64 ms bursts and a late producer: the target adapts, the device starves rarely (%llu frames, %llu underruns)", (unsigned long long)r.sim.starved_after_warmup,
          (unsigned long long)r.pacer_underruns_after_warmup);
}

/* The control: one period per wake-up with no queue policy starves when the producer is late, so the simulation does see starvation. */
static void test_control_without_the_queue_policy(void) {
    scenario_t sc = { DEV_STEADY, 20.0, 0, 0, 0, 0, 1.0, 1, 1 };
    result_t r;
    run(&sc, &r);
    CHECK(r.sim.starved_frames > 0, "control: without the queue policy the same late producer starves the device (%llu frames)", (unsigned long long)r.sim.starved_frames);
}

static void test_silence_is_not_a_repeat(void) {
    scenario_t sc = { DEV_BURST, 20.0, 0, 0, 85.3, 0, 1.0, 0, 1 };
    result_t r;
    run(&sc, &r);
    CHECK(r.pacer.stats.silence_frames == r.sim.silence_pushed, "an underrun is filled with silence and counted (%llu frames)", (unsigned long long)r.sim.silence_pushed);
    CHECK(r.sim.stale_frames == 0, "never with the last buffer played again");
}

static void test_period_and_report(void) {
    char line[400];
    audio_pacer_t p;
    CHECK(audio_pacer_period_frames(22050) == 256 && audio_pacer_period_frames(44100) == 512 && audio_pacer_period_frames(48000) == 512 &&
              audio_pacer_period_frames(11025) == 128,
          "the period is a power of two close to 11.6 ms for every rate (%u, %u, %u, %u)", audio_pacer_period_frames(22050), audio_pacer_period_frames(44100),
          audio_pacer_period_frames(48000), audio_pacer_period_frames(11025));
    audio_pacer_init(&p, 22050, FRAME_BYTES);
    audio_pacer_tick(&p, 1000, 0);
    audio_pacer_report(&p, line, sizeof line);
    CHECK(strstr(line, "underruns") && strstr(line, "backlog drops") && strstr(line, "queued min") && strstr(line, "longest wake-up gap"),
          "the report names the target, the queue extremes, the underruns, the drops and the gaps: %s", line);
    audio_pacer_set_rate(&p, 44100);
    CHECK(p.rate == 44100 && p.period_frames == 512 && p.stats.ticks == 1, "a new rate changes the period and keeps the statistics");
}

/* SDL does the conversion the device needs: the stream the backend feeds is 22050 Hz float stereo whatever the device runs at. */
static int count_crossings(const float* samples, int frames) {
    int crossings = 0;
    for (int i = 1; i < frames; i++)
        if ((samples[(i - 1) * 2] < 0.0f) != (samples[i * 2] < 0.0f)) crossings++;
    return crossings;
}

static void test_conversion_keeps_pitch_and_length(void) {
    static const int device_rates[] = { 44100, 48000, 32000, 96000 };
    for (unsigned r = 0; r < 4; r++) {
        SDL_AudioSpec src = { SDL_AUDIO_F32, 2, 22050 }, dst = { SDL_AUDIO_F32, 2, device_rates[r] };
        SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
        enum { SECONDS = 2 };
        float* in = (float*)malloc((size_t)22050 * SECONDS * 2 * sizeof(float));
        float* out = (float*)malloc((size_t)device_rates[r] * (SECONDS + 1) * 2 * sizeof(float));
        int got, frames;
        double hz;
        if (!stream || !in || !out) {
            CHECK(0, "SDL could not make a %d Hz conversion stream", device_rates[r]);
            SDL_DestroyAudioStream(stream);
            free(in);
            free(out);
            continue;
        }
        for (int i = 0; i < 22050 * SECONDS; i++) in[i * 2] = in[i * 2 + 1] = (float)sin(2.0 * 3.14159265358979 * 440.0 * i / 22050.0);
        SDL_PutAudioStreamData(stream, in, 22050 * SECONDS * 2 * (int)sizeof(float));
        SDL_FlushAudioStream(stream);
        got = SDL_GetAudioStreamData(stream, out, device_rates[r] * (SECONDS + 1) * 2 * (int)sizeof(float));
        frames = got / (2 * (int)sizeof(float));
        hz = frames > 0 ? count_crossings(out, frames) / 2.0 / ((double)frames / device_rates[r]) : 0.0;
        CHECK(abs(frames - device_rates[r] * SECONDS) <= device_rates[r] / 50, "22050 Hz to %d Hz keeps the duration (%d frames for %d s)", device_rates[r], frames, SECONDS);
        CHECK(fabs(hz - 440.0) < 5.0, "22050 Hz to %d Hz keeps the pitch (%.1f Hz for 440 Hz)", device_rates[r], hz);
        SDL_DestroyAudioStream(stream);
        free(in);
        free(out);
    }
}

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

int main(void) {
    test_period_and_report();
    test_steady_device();
    test_bursty_device();
    test_jitter();
    test_clock_drift();
    test_stalls();
    test_producer_timing();
    test_bursts_with_a_late_producer();
    test_control_without_the_queue_policy();
    test_silence_is_not_a_repeat();
    test_conversion_keeps_pitch_and_length();
    printf("audio_pacer_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
