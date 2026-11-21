#include "sdl_audio.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "audio_log.h"
#include "audio_pacer.h"
#include "audio_policy.h"

#define CHUNK_FRAMES 1024
#define FRAME_BYTES ((uint32_t)(2 * sizeof(float)))
#define DEVICE_SAMPLE_FRAMES "1024"
#define QUIET_LOG_INTERVAL_NS (30ull * 1000000000ull)
#define TROUBLE_LOG_INTERVAL_NS (5ull * 1000000000ull)

static struct {
    SDL_Mutex* mutex;
    SDL_Mutex* control;
    SDL_AudioStream* stream;
    audio_mixer_t* mixer;
    SDL_Thread* thread;
    SDL_AtomicInt run;
    audio_pacer_t pacer;
    int subsystem;
    int stats_every_interval;
    uint64_t logged_underruns, logged_drops, last_log_ns;
    float chunk[CHUNK_FRAMES * 2];
} A;

static void mixer_lock(void* context) {
    (void)context;
    SDL_LockMutex(A.mutex);
}

static void mixer_unlock(void* context) {
    (void)context;
    SDL_UnlockMutex(A.mutex);
}

static SDL_AudioSpec stream_spec(uint32_t rate) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)rate;
    return spec;
}

static void push_frames(uint32_t frames, int silence) {
    while (frames > 0) {
        uint32_t n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        if (silence) memset(A.chunk, 0, n * FRAME_BYTES);
        else audio_mixer_mix(A.mixer, A.chunk, n);
        if (!SDL_PutAudioStreamData(A.stream, A.chunk, (int)(n * FRAME_BYTES))) return;
        frames -= n;
    }
}

static void log_pacer(const char* when) {
    char line[320];
    audio_pacer_report(&A.pacer, line, sizeof line);
    host_log("audio", "%s: %s", when, line);
    A.logged_underruns = A.pacer.stats.underruns;
    A.logged_drops = A.pacer.stats.backlog_drops;
}

static void log_when_due(uint64_t now) {
    const audio_pacer_stats_t* s = &A.pacer.stats;
    uint64_t interval = A.stats_every_interval ? TROUBLE_LOG_INTERVAL_NS : QUIET_LOG_INTERVAL_NS;
    int trouble = s->underruns != A.logged_underruns || s->backlog_drops != A.logged_drops;
    if (now - A.last_log_ns < interval) return;
    if (!trouble && !A.stats_every_interval) return;
    A.last_log_ns = now;
    log_pacer(trouble ? "queue trouble" : "queue");
}

static int SDLCALL producer(void* data) {
    (void)data;
    while (SDL_GetAtomicInt(&A.run)) {
        int64_t wake;
        uint64_t now = SDL_GetTicksNS();
        int queued;
        audio_plan_t plan;
        SDL_LockMutex(A.control);
        queued = SDL_GetAudioStreamQueued(A.stream);
        plan = audio_pacer_tick(&A.pacer, (int64_t)now, queued > 0 ? (uint32_t)queued : 0u);
        if (plan.drop) SDL_ClearAudioStream(A.stream);
        push_frames(plan.silence_frames, 1);
        push_frames(plan.mix_frames, 0);
        wake = plan.next_ns;
        log_when_due(now);
        SDL_UnlockMutex(A.control);
        now = SDL_GetTicksNS();
        if ((int64_t)now < wake) SDL_DelayPrecise((uint64_t)(wake - (int64_t)now));
    }
    return 0;
}

static void log_formats(void) {
    SDL_AudioSpec source, device;
    if (!SDL_GetAudioStreamFormat(A.stream, &source, &device)) return;
    host_log("audio", "SDL3 pacer active (policy %d): source %d Hz %d ch %s, device %d Hz %d ch %s, driver %s, %u-frame periods, queue target %u periods (%.1f ms), device buffer %s frames",
             AUDIO_PACER_POLICY_VERSION, source.freq, source.channels, SDL_GetAudioFormatName(source.format), device.freq, device.channels,
             SDL_GetAudioFormatName(device.format), SDL_GetCurrentAudioDriver(), (unsigned)A.pacer.period_frames,
             (unsigned)A.pacer.target_periods,
             audio_pacer_bytes_to_ms(&A.pacer, A.pacer.target_periods * A.pacer.period_frames * A.pacer.frame_bytes), DEVICE_SAMPLE_FRAMES);
}

static void sdl_close(void) {
    if (A.thread) {
        SDL_SetAtomicInt(&A.run, 0);
        SDL_WaitThread(A.thread, NULL);
        A.thread = NULL;
        if (A.stream && A.pacer.stats.ticks) log_pacer("shutdown");
    }
    if (A.stream) SDL_DestroyAudioStream(A.stream);
    audio_mixer_destroy(A.mixer);
    if (A.control) SDL_DestroyMutex(A.control);
    if (A.mutex) SDL_DestroyMutex(A.mutex);
    if (A.subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_zero(A);
}

static void apply_driver_policy(void) {
    const char* explicit_driver = SDL_getenv("SDL_AUDIODRIVER");
    audio_driver_choice_t choice =
        audio_choose_driver(jpt_current_platform(), explicit_driver, audio_headless_requested(SDL_getenv("JPT_AUDIO_HEADLESS")));
    if (choice.hint) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, choice.hint);
}

static audio_mixer_t* sdl_open(uint32_t rate) {
    audio_lock_ops_t ops = { NULL, mixer_lock, mixer_unlock };
    SDL_AudioSpec spec = stream_spec(rate);
    const char* stats = SDL_getenv("JPT_AUDIO_STATS");
    if (!rate || A.mixer) return NULL;
    apply_driver_policy();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, DEVICE_SAMPLE_FRAMES);
    A.subsystem = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!A.subsystem) {
        host_log("audio", "SDL3 audio could not start: %s", SDL_GetError());
        return NULL;
    }
    A.mutex = SDL_CreateMutex();
    A.control = SDL_CreateMutex();
    A.mixer = A.mutex && A.control ? audio_mixer_create(rate, &ops) : NULL;
    if (A.mixer) A.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!A.stream || !SDL_ResumeAudioStreamDevice(A.stream)) {
        host_log("audio", "SDL3 audio device could not be opened (driver %s): %s", SDL_GetCurrentAudioDriver(), SDL_GetError());
        sdl_close();
        return NULL;
    }
    A.stats_every_interval = stats && *stats && *stats != '0';
    audio_pacer_init(&A.pacer, rate, FRAME_BYTES);
    SDL_SetAtomicInt(&A.run, 1);
    A.thread = SDL_CreateThread(producer, "jpt-audio", NULL);
    if (!A.thread) {
        host_log("audio", "SDL3 audio thread could not start: %s", SDL_GetError());
        sdl_close();
        return NULL;
    }
    log_formats();
    return A.mixer;
}

static void sdl_set_rate(uint32_t rate) {
    SDL_AudioSpec spec = stream_spec(rate);
    if (!A.stream || !rate) return;
    SDL_LockMutex(A.control);
    if (SDL_SetAudioStreamFormat(A.stream, &spec, NULL)) {
        audio_mixer_set_rate(A.mixer, rate);
        audio_pacer_set_rate(&A.pacer, rate);
        SDL_ClearAudioStream(A.stream);
    }
    SDL_UnlockMutex(A.control);
}

static const char* sdl_name(void) {
    const char* name = SDL_GetCurrentAudioDriver();
    return name ? name : "none";
}

void sdl_audio_stats(audio_pacer_stats_t* out) {
    memset(out, 0, sizeof *out);
    if (!A.control) return;
    SDL_LockMutex(A.control);
    *out = A.pacer.stats;
    SDL_UnlockMutex(A.control);
}

const audio_backend_t* sdl_audio_backend(void) {
    static const audio_backend_t backend = { sdl_open, sdl_set_rate, sdl_close, sdl_name };
    return &backend;
}
