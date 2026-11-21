/* The real SDL3 audio backend on SDL's dummy driver: the driver policy as SDL sees it (explicit SDL_AUDIODRIVER, headless
 * request, an unknown driver), the device stream pulling the mixer on SDL's audio thread, the rate change, close and
 * reopen. The dummy driver opens no device and makes no sound. No Windows header: the same source runs on Windows and
 * Linux. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio_backend.h"
#include "audio_mixer.h"
#include "audio_policy.h"
#include "sdl_audio.h"

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

static char g_log[1024];

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
    g_log[0] = 'x';
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

static void set_env(const char* name, const char* value) {
    if (value) SDL_setenv_unsafe(name, value, 1);
    else SDL_unsetenv_unsafe(name);
}

/* The mixer is pulled by SDL's own audio thread: a stream's clock moves without any call from here. */
static int plays_through_the_device(audio_mixer_t* mixer) {
    audio_format_t f = { 22050, 2, 16 };
    audio_stream_t* s = audio_stream_open(mixer, &f, 1000);
    uint8_t* pcm = (uint8_t*)calloc(1, 22050u / 2u * 4u);
    int moved = 0;
    if (!s || !pcm) {
        free(pcm);
        return 0;
    }
    audio_stream_push(s, pcm, 22050u / 2u * 4u);
    audio_stream_pause(s, 0);
    for (int i = 0; i < 150 && !moved; i++) {
        SDL_Delay(20);
        moved = audio_stream_played_frames(s) > 0;
    }
    audio_stream_close(s);
    free(pcm);
    return moved;
}

static void test_explicit_driver(void) {
    const audio_backend_t* b = sdl_audio_backend();
    audio_mixer_t* mixer;
    set_env("JPT_AUDIO_HEADLESS", NULL);
    set_env("SDL_AUDIODRIVER", "dummy");
    mixer = b->open(22050);
    CHECK(mixer != NULL && audio_mixer_rate(mixer) == 22050, "the backend opens on an explicit SDL_AUDIODRIVER");
    CHECK(!strcmp(b->name(), "dummy"), "and SDL reports the driver it was asked for (%s)", b->name());
    CHECK(b->open(22050) == NULL, "a second open while one is running is refused");
    CHECK(plays_through_the_device(mixer), "the device consumes the mixer's output: a PCM stream moves with no call from the test");
    {
        audio_pacer_stats_t st;
        SDL_Delay(500);
        sdl_audio_stats(&st);
        CHECK(st.ticks > 20 && st.mixed_frames >= st.ticks * 128u && st.target_periods >= AUDIO_PACER_MIN_PERIODS,
              "the producer thread runs on its own steady clock (%llu wake-ups, %llu frames mixed)", (unsigned long long)st.ticks, (unsigned long long)st.mixed_frames);
        CHECK(st.underruns <= 1 && st.backlog_drops == 0, "and the dummy device it feeds sees at most the start-up underrun (%llu) and no backlog drop (%llu)",
              (unsigned long long)st.underruns, (unsigned long long)st.backlog_drops);
    }
    b->set_rate(44100);
    CHECK(audio_mixer_rate(mixer) == 44100, "the rate follows the primary format");
    b->set_rate(0);
    CHECK(audio_mixer_rate(mixer) == 44100, "a zero rate is ignored");
    b->close();
    CHECK(!strcmp(b->name(), "none"), "closed: SDL has no driver any more (%s)", b->name());
    mixer = b->open(22050);
    CHECK(mixer != NULL && !strcmp(b->name(), "dummy"), "and the backend can open again");
    b->close();
}

static void test_headless_request(void) {
    const audio_backend_t* b = sdl_audio_backend();
    audio_mixer_t* mixer;
    char env_before[64];
    const char* previous = SDL_getenv("SDL_AUDIODRIVER");
    snprintf(env_before, sizeof env_before, "%s", previous ? previous : "");
    set_env("SDL_AUDIODRIVER", NULL);
    set_env("JPT_AUDIO_HEADLESS", "1");
    mixer = b->open(22050);
    CHECK(mixer != NULL && !strcmp(b->name(), "dummy"), "without SDL_AUDIODRIVER, JPT_AUDIO_HEADLESS=1 selects the dummy driver (%s)", b->name());
    b->close();
    set_env("JPT_AUDIO_HEADLESS", NULL);
    set_env("SDL_AUDIODRIVER", env_before[0] ? env_before : "dummy");
}

static void test_unknown_driver_is_not_replaced(void) {
    const audio_backend_t* b = sdl_audio_backend();
    set_env("SDL_AUDIODRIVER", "no-such-driver");
    g_log[0] = 0;
    CHECK(b->open(22050) == NULL, "an explicit driver SDL does not have fails: nothing silently replaces it");
    CHECK(g_log[0] != 0, "and the failure is logged");
    CHECK(!strcmp(b->name(), "none"), "no driver is left running");
    set_env("SDL_AUDIODRIVER", "dummy");
    CHECK(b->open(22050) != NULL, "the next open works");
    b->close();
}

static void test_line_names_the_driver(void) {
    const audio_backend_t* b = sdl_audio_backend();
    char line[128];
    audio_mixer_t* mixer;
    set_env("SDL_AUDIODRIVER", "dummy");
    mixer = b->open(22050);
    audio_backend_line(line, sizeof line, audio_mixer_rate(mixer), b->name());
    CHECK(!strcmp(line, "Audio: SDL3 backend initialized, 22050 Hz, stereo, driver dummy"), "the start-up line carries the driver SDL chose: %s", line);
    b->close();
}

int main(void) {
    test_explicit_driver();
    test_headless_request();
    test_unknown_driver_is_not_replaced();
    test_line_names_the_driver();
    printf("audio_backend_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
