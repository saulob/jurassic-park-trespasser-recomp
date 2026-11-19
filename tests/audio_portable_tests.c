/* The audio layer's platform-independent decisions and PCM paths: which SDL3 audio driver each platform asks for, the
 * single start-up log line, the mixer's PCM streams (the videos' audio) and voices over memory the caller owns (guest
 * memory). No Windows header: the same source runs on Windows and Linux. Nothing is played. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_mixer.h"
#include "audio_policy.h"

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

static int near_to(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

/* ---- driver policy ---------------------------------------------------------------------------------------------- */

static void test_driver_policy(void) {
    audio_driver_choice_t c;
    c = audio_choose_driver(JPT_PLATFORM_WINDOWS, NULL, 0);
    CHECK(c.hint && !strcmp(c.hint, "wasapi") && c.source == AUDIO_DRIVER_PLATFORM, "Windows by default: WASAPI only");
    c = audio_choose_driver(JPT_PLATFORM_WINDOWS, "", 0);
    CHECK(c.hint && !strcmp(c.hint, "wasapi"), "an empty SDL_AUDIODRIVER is not a choice");
    c = audio_choose_driver(JPT_PLATFORM_LINUX, NULL, 0);
    CHECK(c.hint == NULL && c.source == AUDIO_DRIVER_AUTOMATIC, "Linux by default: no hint, SDL picks PipeWire, PulseAudio or ALSA by itself");
    c = audio_choose_driver(JPT_PLATFORM_OTHER, NULL, 0);
    CHECK(c.hint == NULL && c.source == AUDIO_DRIVER_AUTOMATIC, "any other platform: SDL's automatic choice");
    for (int p = 0; p < 3; p++) {
        c = audio_choose_driver((jpt_platform_t)p, "pipewire", 0);
        CHECK(c.hint == NULL && c.source == AUDIO_DRIVER_EXPLICIT, "an explicit SDL_AUDIODRIVER is left to SDL on every platform (%d)", p);
        c = audio_choose_driver((jpt_platform_t)p, "pulseaudio", 1);
        CHECK(c.hint == NULL && c.source == AUDIO_DRIVER_EXPLICIT, "and wins over a headless request (%d)", p);
        c = audio_choose_driver((jpt_platform_t)p, NULL, 1);
        CHECK(c.hint && !strcmp(c.hint, "dummy") && c.source == AUDIO_DRIVER_HEADLESS, "a headless run asks for SDL's dummy driver (%d)", p);
    }
    c = audio_choose_driver(JPT_PLATFORM_WINDOWS, "dummy", 0);
    CHECK(c.hint == NULL && c.source == AUDIO_DRIVER_EXPLICIT, "the gate's SDL_AUDIODRIVER=dummy is respected on Windows");
    for (int p = 0; p < 3; p++)
        for (int headless = 0; headless < 2; headless++) {
            c = audio_choose_driver((jpt_platform_t)p, NULL, headless);
            CHECK(!c.hint || (strcmp(c.hint, "directsound") && strcmp(c.hint, "winmm")), "the host never asks for SDL's DirectSound or WinMM driver (%d, %d)", p, headless);
            CHECK(!c.hint || !strcmp(c.hint, "dummy") || (p == JPT_PLATFORM_WINDOWS && !strcmp(c.hint, "wasapi")),
                  "the only drivers the host names are dummy and, on Windows, wasapi (%d, %d)", p, headless);
        }
    CHECK(audio_headless_requested("1") && audio_headless_requested("true") && audio_headless_requested("TRUE") && !audio_headless_requested("0") &&
              !audio_headless_requested("") && !audio_headless_requested(NULL) && !audio_headless_requested("yes"),
          "JPT_AUDIO_HEADLESS is 1 or true");
    CHECK(jpt_current_platform() == JPT_PLATFORM_WINDOWS || jpt_current_platform() == JPT_PLATFORM_LINUX || jpt_current_platform() == JPT_PLATFORM_OTHER,
          "this build knows its platform");
    {
        char line[128];
        int n = audio_backend_line(line, sizeof line, 22050, "wasapi");
        CHECK(n > 0 && !strcmp(line, "Audio: SDL3 backend initialized, 22050 Hz, stereo, driver wasapi"), "the start-up line: %s", line);
        audio_backend_line(line, sizeof line, 44100, "pipewire");
        CHECK(!strcmp(line, "Audio: SDL3 backend initialized, 44100 Hz, stereo, driver pipewire"), "with the driver SDL chose");
        audio_backend_line(line, sizeof line, 22050, NULL);
        CHECK(strstr(line, "driver unknown") != NULL, "and a driver name when SDL has none");
    }
}

/* ---- PCM streams (the videos' audio) ---------------------------------------------------------------------------- */

static void test_streams(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_format_t f;
    audio_stream_t* s;
    int16_t pcm[8];
    float out[32];
    uint32_t taken;
    f.rate = 22050;
    f.channels = 2;
    f.bits = 16;
    CHECK(audio_stream_open(m, &f, 0) == NULL && audio_stream_open(NULL, &f, 100) == NULL, "no capacity, no mixer");
    f.channels = 3;
    CHECK(audio_stream_open(m, &f, 100) == NULL, "a bad format");
    f.channels = 2;
    s = audio_stream_open(m, &f, 100);
    CHECK(s != NULL && audio_mixer_stream_count(m) == 1 && audio_stream_paused(s), "a stream starts paused");
    for (int i = 0; i < 8; i++) pcm[i] = (int16_t)(8192 * ((i & 1) ? 2 : 1));
    CHECK(audio_stream_push(s, pcm, sizeof pcm) == sizeof pcm && audio_stream_queued(s) == sizeof pcm, "PCM is queued");
    audio_mixer_mix(m, out, 4);
    CHECK(out[0] == 0.0f && audio_stream_queued(s) == sizeof pcm && audio_stream_played_frames(s) == 0, "a paused stream is silent and keeps its data");
    audio_stream_pause(s, 0);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.25f && out[1] == 0.5f && out[2] == 0.25f && out[3] == 0.5f, "interleaved stereo plays in order");
    CHECK(audio_stream_queued(s) == 8 && audio_stream_played_frames(s) == 2, "queued falls by what the output took, the stream's clock follows");
    audio_stream_set_gain(s, 0.5f);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.125f && out[1] == 0.25f, "the stream's own gain");
    audio_mixer_mix(m, out, 4);
    CHECK(out[0] == 0.125f && out[2] == 0.0f && out[6] == 0.0f && audio_stream_queued(s) == 0 && audio_stream_played_frames(s) == 4,
          "a drained stream falls silent and stops counting");
    CHECK(audio_stream_push(s, pcm, 6) == 4, "only whole frames are taken");
    audio_stream_flush(s);
    CHECK(audio_stream_queued(s) == 0, "flush drops the queue");
    audio_mixer_set_master_gain(m, 0.5f);
    audio_stream_set_gain(s, 1.0f);
    audio_stream_push(s, pcm, 4);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.125f, "the master gain applies to streams as to voices");
    audio_mixer_set_master_gain(m, 1.0f);
    audio_stream_pause(s, 1);
    audio_stream_flush(s);
    {
        uint8_t* block = (uint8_t*)calloc(1, 22050u * 4u);
        taken = audio_stream_push(s, block, 22050u * 4u);
        CHECK(taken == audio_stream_queued(s) && taken == 2205u * 4u, "the queue holds its capacity (100 ms) and no more (%u bytes)", taken);
        CHECK(audio_stream_push(s, block, 4) == 0, "a full queue takes nothing");
        free(block);
    }
    audio_stream_flush(s);
    for (int round = 0; round < 3; round++) {
        uint8_t chunk[2000];
        memset(chunk, 0, sizeof chunk);
        audio_stream_pause(s, 0);
        CHECK(audio_stream_push(s, chunk, sizeof chunk) == sizeof chunk, "the ring wraps without losing frames (%d)", round);
        audio_mixer_mix(m, out, 16);
        audio_stream_pause(s, 1);
        audio_stream_flush(s);
    }
    audio_stream_close(s);
    CHECK(audio_mixer_stream_count(m) == 0, "closing removes the stream");
    audio_mixer_mix(m, out, 4);
    CHECK(out[0] == 0.0f, "and the mixer plays nothing of it");

    f.channels = 1;
    f.bits = 8;
    f.rate = 11025;
    s = audio_stream_open(m, &f, 200);
    {
        const uint8_t pcm8[4] = { 0xC0, 0xC0, 0x40, 0x40 };
        audio_stream_push(s, pcm8, 4);
        audio_stream_pause(s, 0);
        audio_mixer_mix(m, out, 8);
        CHECK(out[0] == 0.5f && out[1] == 0.5f && audio_stream_played_frames(s) == 4,
              "an 8-bit mono stream at half the output rate plays each frame for two output frames");
        CHECK(near_to(out[8], -0.5f, 1e-6f) || near_to(out[7], -0.5f, 1e-6f) || near_to(out[6], -0.5f, 1e-6f), "unsigned 8-bit samples centre on 128");
    }
    audio_mixer_destroy(m);
}

static void count_release(void* context) { ++*(int*)context; }

static void test_voice_over_external_memory(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_format_t f = { 22050, 1, 16 }, bad = { 22050, 3, 16 };
    static uint8_t block[16], other[16];
    int released = 0, released_other = 0;
    audio_memory_t memory;
    audio_voice_t *a, *b;
    float out[4];
    int16_t sample = 16384;
    memory.bytes = block;
    memory.release = count_release;
    memory.context = &released;
    memset(block, 0xAA, sizeof block);
    CHECK(audio_voice_create_in(m, &f, 16, NULL) != NULL, "no memory given: the voice allocates its own");
    memory.bytes = NULL;
    CHECK(audio_voice_create_in(m, &f, 16, &memory) == NULL, "memory without bytes is refused");
    memory.bytes = block;
    CHECK(audio_voice_create_in(m, &f, 15, &memory) == NULL && audio_voice_create_in(m, &f, 0, &memory) == NULL && audio_voice_create_in(m, &bad, 16, &memory) == NULL,
          "a size that is not a whole number of frames, or a bad format");
    CHECK(released == 0, "a refused voice leaves the memory to its owner");
    a = audio_voice_create_in(m, &f, 16, &memory);
    CHECK(a != NULL && audio_voice_data(a) == block && block[0] == 0 && block[15] == 0, "the voice plays from the caller's memory, silence filled");
    memcpy(block, &sample, 2);
    audio_voice_play(a, 1);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.5f, "what is written into the memory is what plays");
    b = audio_voice_duplicate(m, a);
    CHECK(b != NULL && audio_voice_data(b) == block, "a duplicate shares it");
    audio_voice_destroy(a);
    CHECK(released == 0, "the memory stays while a duplicate uses it");
    audio_voice_destroy(b);
    CHECK(released == 1, "and goes back to its owner exactly once with the last voice");
    memory.bytes = other;
    memory.context = &released_other;
    a = audio_voice_create_in(m, &f, 16, &memory);
    CHECK(a != NULL, "another block");
    audio_mixer_destroy(m);
    CHECK(released_other == 1 && released == 1, "destroying the mixer returns the memory of the voices it still holds");
}

int main(void) {
    test_driver_policy();
    test_streams();
    test_voice_over_external_memory();
    printf("audio_portable_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
