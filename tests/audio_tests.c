/* The SDL3 audio path without a device: DirectSound volume and frequency semantics, the mixer on synthetic buffers, the
 * 3D math, the PCM formats the game uses, the menu click level, and the static rules that keep DirectSound out of the
 * backend. Nothing is played. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "audio_mixer.h"
#include "audio_policy.h"
#include "audio_spatial.h"
#include "menu_audio_policy.h"

#ifdef JPT_SOURCE_DIR
#define SOURCE_DIR JPT_SOURCE_DIR "/"
#else
#define SOURCE_DIR ""
#endif

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

static audio_voice_t* make_voice(audio_mixer_t* m, uint32_t rate, uint16_t channels, uint16_t bits, uint32_t bytes) {
    audio_format_t f;
    f.rate = rate;
    f.channels = channels;
    f.bits = bits;
    return audio_voice_create(m, &f, bytes);
}

static void put16(audio_voice_t* v, const int16_t* samples, unsigned count) {
    memcpy(audio_voice_data(v), samples, count * sizeof(int16_t));
}

/* ---- volume ----------------------------------------------------------------------------------------------------- */

static void test_volume(void) {
    CHECK(audio_volume_gain(0) == 1.0f, "0 dB is full level");
    CHECK(near_to(audio_volume_gain(-602), 0.5f, 0.001f), "-6.02 dB is half the amplitude");
    CHECK(near_to(audio_volume_gain(-2000), 0.1f, 0.0001f), "-20 dB is a tenth");
    CHECK(near_to(audio_volume_gain(-600), 0.5012f, 0.0005f), "-6 dB");
    CHECK(near_to(audio_volume_gain(-1000), 0.3162f, 0.0005f), "-10 dB");
    CHECK(audio_volume_gain(DS_VOLUME_MIN) == 0.0f, "DSBVOLUME_MIN mutes");
    CHECK(audio_volume_gain(-20000) == 0.0f, "below the minimum clamps to mute");
    CHECK(audio_volume_gain(-9999) > 0.0f && audio_volume_gain(-9999) < 1e-4f, "one step above the minimum is not mute");
    CHECK(audio_volume_gain(500) == 1.0f, "above full level clamps to full");
    CHECK(audio_clamp_volume(-20000) == DS_VOLUME_MIN && audio_clamp_volume(500) == 0 && audio_clamp_volume(-77) == -77,
          "clamping");
    CHECK(near_to(audio_db_gain(-6.0206f), 0.5f, 0.0001f), "-6.0206 dB is 0.5");
    CHECK(audio_db_gain(0.0f) == 1.0f && audio_db_gain(3.0f) == 1.0f, "dB never amplifies");
    {
        float previous = 2.0f;
        for (int v = 0; v >= -10000; v -= 25) {
            float g = audio_volume_gain(v);
            CHECK(g <= previous && g >= 0.0f && g <= 1.0f, "gain falls monotonically at %d", v);
            previous = g;
        }
    }
}

/* The game's sample volume setter (0x0043D7E0) adds the sample's attenuation to its argument, clamps to [-100, 0] dB and
 * gives DirectSound (int)(dB * 100). The menu click's argument comes from menu_effect_volume_arg. */
static int guest_set_volume(float arg_db, float attenuation_db) {
    float v = arg_db + attenuation_db;
    if (v > 0.0f) v = 0.0f;
    if (v < -100.0f) v = -100.0f;
    return (int)(v * 100.0f);
}

static void test_menu_click(void) {
    static const float k_levels[] = { 0.0f, -3.0f, -10.0f, -17.5f };
    for (size_t i = 0; i < sizeof k_levels / sizeof k_levels[0]; i++) {
        float attenuation = 1.25f, current = k_levels[i];
        float arg = menu_effect_volume_arg(current, attenuation);
        int plain = guest_set_volume(current - attenuation, attenuation), quiet = guest_set_volume(arg, attenuation);
        float ratio = audio_volume_gain(quiet) / audio_volume_gain(plain);
        CHECK(near_to(ratio, MENU_EFFECT_GAIN, 0.002f), "the menu click keeps half its amplitude at %.1f dB (got %.4f)",
              current, ratio);
    }
    CHECK(near_to(audio_volume_gain(guest_set_volume(menu_effect_volume_arg(0.0f, 0.0f), 0.0f)), 0.5f, 0.001f),
          "a click at 0 dB plays at 0.5");
    CHECK(MENU_EFFECT_GAIN == 0.5f && near_to(20.0f * log10f(MENU_EFFECT_GAIN), -6.0206f, 0.0001f), "the policy is -6.0206 dB");
}

static void test_frequency(void) {
    CHECK(audio_resolve_frequency(0, 22050) == 22050, "0 is the sample's own rate");
    CHECK(audio_resolve_frequency(44100, 22050) == 44100, "a rate in range");
    CHECK(audio_resolve_frequency(DS_FREQUENCY_MIN, 22050) == DS_FREQUENCY_MIN, "the minimum");
    CHECK(audio_resolve_frequency(DS_FREQUENCY_MIN - 1, 22050) == -1, "below the minimum");
    CHECK(audio_resolve_frequency(DS_FREQUENCY_MAX, 22050) == DS_FREQUENCY_MAX, "the maximum");
    CHECK(audio_resolve_frequency(DS_FREQUENCY_MAX + 1, 22050) == -1, "above the maximum");
}

static void test_import_names(void) {
    CHECK(audio_is_dsound_library("DSound.dll") && audio_is_dsound_library("DSOUND.DLL") && audio_is_dsound_library("dsound.dll"),
          "the library name is case-insensitive");
    CHECK(audio_is_dsound_library("C:\\Windows\\System32\\dsound.dll") && audio_is_dsound_library("x/dsound.dll"), "any path");
    CHECK(!audio_is_dsound_library("dsound.dll.bak") && !audio_is_dsound_library("xdsound.dll") &&
              !audio_is_dsound_library("A3d.dll") && !audio_is_dsound_library(NULL) && !audio_is_dsound_library(""),
          "other names");
    CHECK(audio_is_dsound_create("DirectSoundCreate") && !audio_is_dsound_create("DirectSoundCreate8") &&
              !audio_is_dsound_create("directsoundcreate") && !audio_is_dsound_create(NULL) &&
              !audio_is_dsound_create((const char*)(uintptr_t)1),
          "the one entry point, not by ordinal");
}

/* ---- mixer ------------------------------------------------------------------------------------------------------ */

static void test_silence_and_one_shot(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    float out[16];
    audio_voice_t* v;
    const int16_t samples[4] = { 16384, -16384, 8192, 0 };
    audio_mixer_mix(m, out, 8);
    for (int i = 0; i < 16; i++) CHECK(out[i] == 0.0f, "no voice, no sound");
    v = make_voice(m, 22050, 1, 16, 8);
    put16(v, samples, 4);
    audio_mixer_mix(m, out, 8);
    CHECK(out[0] == 0.0f, "a voice that was never played is silent");
    audio_voice_play(v, 0);
    CHECK(audio_voice_status(v) == AUDIO_STATUS_PLAYING, "playing");
    audio_mixer_mix(m, out, 6);
    CHECK(out[0] == 0.5f && out[1] == 0.5f && out[2] == -0.5f && out[3] == -0.5f && out[4] == 0.25f && out[6] == 0.0f,
          "one-shot samples, mono to both channels");
    CHECK(out[8] == 0.0f && out[11] == 0.0f, "silence after the end");
    CHECK(audio_voice_status(v) == 0, "a finished one-shot is no longer playing");
    {
        uint32_t play, write;
        audio_voice_positions(v, &play, &write);
        CHECK(play == 0 && write == 0, "and its cursor is back at the start");
    }
    audio_voice_play(v, 0);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.5f, "replay starts from the beginning");
    audio_mixer_destroy(m);
}

static void test_gain_pan_clip(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t *a, *b;
    float out[8];
    const int16_t half[2] = { 16384, 16384 }, loud[2] = { 24576, -24576 };
    a = make_voice(m, 22050, 1, 16, 4);
    put16(a, half, 2);
    audio_voice_play(a, 1);
    audio_voice_set_gain(a, 0.5f);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.25f && out[1] == 0.25f, "voice gain");
    audio_voice_set_spatial(a, 1.0f, 0.0f);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.25f && out[1] == 0.0f, "per-channel gain silences one side");
    audio_voice_set_spatial(a, 1.0f, 1.0f);
    audio_mixer_set_master_gain(m, 0.5f);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.125f, "master gain");
    audio_mixer_set_master_gain(m, 1.0f);
    audio_voice_set_gain(a, 1.0f);
    b = make_voice(m, 22050, 1, 16, 4);
    put16(b, half, 2);
    audio_voice_play(b, 1);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 1.0f && out[1] == 1.0f, "two voices add (0.5 + 0.5)");
    put16(a, loud, 2);
    put16(b, loud, 2);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 1.0f && out[2] == -1.0f, "the sum clips at +1 and -1");
    audio_voice_set_gain(a, 0.0f);
    audio_voice_set_gain(b, 0.0f);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.0f, "muted voices add nothing");
    audio_mixer_destroy(m);
}

static void test_loop_stop_restart(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t* v = make_voice(m, 22050, 1, 16, 6);
    const int16_t ramp[3] = { 8192, 16384, 24576 };
    float out[32];
    uint32_t play, write;
    put16(v, ramp, 3);
    audio_voice_play(v, 1);
    CHECK(audio_voice_status(v) == (AUDIO_STATUS_PLAYING | AUDIO_STATUS_LOOPING), "looping status");
    audio_mixer_mix(m, out, 7);
    CHECK(out[0] == 0.25f && out[2] == 0.5f && out[4] == 0.75f && out[6] == 0.25f && out[8] == 0.5f && out[10] == 0.75f &&
              out[12] == 0.25f,
          "the loop wraps without a gap");
    audio_voice_positions(v, &play, NULL);
    CHECK(play == 2, "after seven frames the cursor is at frame 1 (%u)", play);
    audio_voice_stop(v);
    CHECK(audio_voice_status(v) == 0, "stopped");
    audio_mixer_mix(m, out, 4);
    CHECK(out[0] == 0.0f && out[6] == 0.0f, "a stopped loop is silent");
    audio_voice_positions(v, &play, &write);
    CHECK(play == 2 && write == 2, "stopping keeps the position, and write follows play");
    audio_voice_play(v, 1);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.5f, "playing again resumes where it stopped");
    CHECK(audio_voice_set_position(v, 0) == 1, "set position");
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.25f, "a restart from position 0");
    CHECK(audio_voice_set_position(v, 6) == 0 && audio_voice_set_position(v, 100) == 0, "a position outside the buffer is refused");
    audio_voice_play(v, 0);
    CHECK(audio_voice_status(v) == AUDIO_STATUS_PLAYING, "replay as a one-shot drops the loop");
    audio_mixer_mix(m, out, 16);
    CHECK(audio_voice_status(v) == 0, "which then ends");
    audio_mixer_destroy(m);
}

static void test_resampling(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t* v = make_voice(m, 11025, 1, 16, 8);
    const int16_t wave[4] = { 0, 16384, 0, -16384 };
    static const float expect[8] = { 0.0f, 0.25f, 0.5f, 0.25f, 0.0f, -0.25f, -0.5f, -0.5f };
    float out[16];
    put16(v, wave, 4);
    audio_voice_play(v, 0);
    audio_mixer_mix(m, out, 8);
    for (int i = 0; i < 8; i++) CHECK(near_to(out[2 * i], expect[i], 1e-6f), "11025 Hz to 22050 Hz, frame %d: %f", i, out[2 * i]);
    CHECK(audio_voice_status(v) == 0, "the resampled one-shot ends");

    {
        const int16_t wave2[4] = { 8192, 16384, 24576, -8192 };
        put16(v, wave2, 4);
    }
    audio_voice_set_frequency(v, 44100);
    CHECK(audio_voice_frequency(v) == 44100, "frequency readback");
    audio_voice_play(v, 1);
    audio_mixer_mix(m, out, 3);
    CHECK(out[0] == 0.25f && out[2] == 0.75f && out[4] == 0.25f, "twice the rate takes every second frame and wraps");
    audio_voice_set_frequency(v, 0);
    CHECK(audio_voice_frequency(v) == 11025, "frequency 0 is the sample's own");
    audio_mixer_destroy(m);
}

static void test_formats(void) {
    static const struct {
        uint32_t rate;
        uint16_t channels, bits;
    } k_game[] = { { 22050, 1, 16 }, { 22050, 2, 16 }, { 11025, 1, 16 }, { 11025, 2, 16 }, { 22050, 1, 8 }, { 44100, 1, 16 } };
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    float out[8];
    for (size_t i = 0; i < sizeof k_game / sizeof k_game[0]; i++) {
        audio_format_t f = { k_game[i].rate, k_game[i].channels, k_game[i].bits };
        audio_voice_t* v = make_voice(m, f.rate, f.channels, f.bits, audio_format_block(&f) * 8);
        CHECK(audio_format_valid(&f) && v != NULL, "game format %u Hz %u ch %u bit", f.rate, f.channels, f.bits);
        CHECK(v && audio_voice_size(v) == audio_format_block(&f) * 8, "size");
        if (v) {
            audio_voice_play(v, 1);
            audio_mixer_mix(m, out, 4);
            CHECK(out[0] == 0.0f && out[1] == 0.0f, "a fresh buffer is silent in every format");
            audio_voice_destroy(v);
        }
    }
    {
        audio_format_t bad[] = { { 22050, 0, 16 }, { 22050, 3, 16 }, { 22050, 1, 24 }, { 22050, 1, 32 }, { 0, 1, 16 } };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(!audio_format_valid(&bad[i]) && !audio_voice_create(m, &bad[i], 64), "format %zu is refused", i);
    }
    CHECK(make_voice(m, 22050, 1, 16, 0) == NULL && make_voice(m, 22050, 2, 16, 6) == NULL, "empty and misaligned sizes are refused");
    audio_mixer_destroy(m);
}

static void test_stereo_and_8bit(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t* st = make_voice(m, 22050, 2, 8, 4);
    float out[4];
    audio_voice_data(st)[0] = 0xC0;
    audio_voice_data(st)[1] = 0x40;
    audio_voice_data(st)[2] = 0x80;
    audio_voice_data(st)[3] = 0x80;
    audio_voice_play(st, 0);
    audio_mixer_mix(m, out, 2);
    CHECK(out[0] == 0.5f && out[1] == -0.5f && out[2] == 0.0f && out[3] == 0.0f, "8-bit stereo, unsigned around 0x80");
    audio_mixer_destroy(m);
}

static void test_lock(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t* v = make_voice(m, 22050, 1, 16, 16);
    uint8_t *p1, *p2;
    uint32_t n1, n2;
    uint8_t* base = audio_voice_data(v);
    CHECK(audio_voice_lock(v, 12, 8, &p1, &n1, &p2, &n2) && p1 == base + 12 && n1 == 4 && p2 == base && n2 == 4, "a wrapping range gives two regions");
    CHECK(audio_voice_lock(v, 0, 16, &p1, &n1, &p2, &n2) && p1 == base && n1 == 16 && p2 == NULL && n2 == 0, "the whole buffer");
    CHECK(audio_voice_lock(v, 4, 4, &p1, &n1, &p2, &n2) && n1 == 4 && p2 == NULL, "a range inside");
    CHECK(audio_voice_lock(v, 5, 4, &p1, &n1, &p2, &n2) && p1 == base + 5 && n1 == 4, "unaligned offsets are accepted");
    CHECK(audio_voice_lock(v, 4, 0, &p1, &n1, &p2, &n2) && n1 == 0, "an empty range");
    CHECK(!audio_voice_lock(v, 16, 4, &p1, &n1, &p2, &n2) && !audio_voice_lock(v, 0, 17, &p1, &n1, &p2, &n2), "outside the buffer");
    audio_mixer_destroy(m);
}

static void test_cursors_and_streaming(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t* v = make_voice(m, 22050, 1, 16, 44100);
    float* out = (float*)malloc(2 * 2000 * sizeof(float));
    uint32_t play, write;
    audio_voice_positions(v, &play, &write);
    CHECK(play == 0 && write == 0, "a new buffer's cursors");
    audio_voice_play(v, 1);
    audio_mixer_mix(m, out, 1000);
    audio_voice_positions(v, &play, &write);
    CHECK(play == 2000, "1000 frames advance the play cursor by 2000 bytes (%u)", play);
    CHECK(write == 2000 + 440, "the write cursor leads by 10 ms (%u)", write);
    audio_mixer_destroy(m);

    /* a ring refilled behind the play cursor, as the game's streaming thread does it */
    m = audio_mixer_create(22050, NULL);
    v = make_voice(m, 22050, 1, 16, 16);
    {
        int16_t ring[8];
        uint8_t *p1, *p2;
        uint32_t n1, n2, filled = 0;
        int16_t next = 1;
        float o[2 * 6];
        for (int i = 0; i < 8; i++) ring[i] = (int16_t)(i + 1);
        put16(v, ring, 8);
        audio_voice_play(v, 1);
        for (int round = 0; round < 4; round++) {
            audio_mixer_mix(m, o, 6);
            audio_voice_positions(v, &play, NULL);
            {
                uint32_t delta = (play + 16 - filled) % 16;
                CHECK(audio_voice_lock(v, filled, delta, &p1, &n1, &p2, &n2), "refill lock");
                for (uint32_t i = 0; i < n1 / 2; i++) ((int16_t*)p1)[i] = next++;
                for (uint32_t i = 0; i < n2 / 2; i++) ((int16_t*)p2)[i] = next++;
                filled = (filled + delta) % 16;
            }
        }
        CHECK(play == (6u * 4 % 8) * 2, "the play cursor wrapped (%u)", play);
    }
    audio_mixer_destroy(m);
    free(out);
}

static void test_duplicate_and_lifetime(void) {
    audio_mixer_t* m = audio_mixer_create(22050, NULL);
    audio_voice_t *a = make_voice(m, 22050, 1, 16, 8), *b;
    const int16_t s[4] = { 16384, 16384, 16384, 16384 };
    float out[4];
    put16(a, s, 4);
    b = audio_voice_duplicate(m, a);
    CHECK(b && audio_voice_data(b) == audio_voice_data(a) && audio_voice_size(b) == 8, "a duplicate shares the data");
    audio_voice_play(a, 0);
    CHECK(audio_voice_status(a) == AUDIO_STATUS_PLAYING && audio_voice_status(b) == 0, "and has its own state");
    audio_voice_set_gain(b, 0.5f);
    audio_voice_play(b, 0);
    audio_mixer_mix(m, out, 1);
    CHECK(out[0] == 0.75f, "independent volumes add (0.5 + 0.25)");
    CHECK(audio_mixer_voice_count(m) == 2, "two voices");
    audio_voice_destroy(a);
    CHECK(audio_mixer_voice_count(m) == 1 && audio_voice_data(b)[1] == 0x40, "the duplicate outlives its source");
    audio_voice_destroy(b);
    CHECK(audio_mixer_voice_count(m) == 0, "no voice left");
    audio_voice_destroy(NULL);
    a = make_voice(m, 22050, 1, 16, 8);
    audio_voice_play(a, 1);
    audio_mixer_destroy(m);
    audio_mixer_destroy(NULL);
}

static void test_determinism(void) {
    float first[64], second[64];
    for (int pass = 0; pass < 2; pass++) {
        audio_mixer_t* m = audio_mixer_create(22050, NULL);
        audio_voice_t* v = make_voice(m, 11025, 2, 16, 64);
        int16_t* d = (int16_t*)audio_voice_data(v);
        for (int i = 0; i < 32; i++) d[i] = (int16_t)(i * 700 - 9000);
        audio_voice_play(v, 1);
        audio_mixer_mix(m, pass ? second : first, 32);
        audio_mixer_destroy(m);
    }
    CHECK(!memcmp(first, second, sizeof first), "the same input mixes to the same bits");
}

/* ---- 3D --------------------------------------------------------------------------------------------------------- */

static void test_spatial(void) {
    audio_listener_t l;
    audio_source_t s;
    float left, right;
    audio_listener_default(&l);
    audio_source_default(&s);
    s.min_distance = 2.0f;
    s.position.z = 1.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(left == 1.0f && right == 1.0f, "inside the minimum distance and ahead: full level");
    s.position.z = 4.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(near_to(left, 0.5f, 1e-5f) && near_to(right, 0.5f, 1e-5f), "twice the minimum distance: half (6 dB)");
    s.position.z = 8.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(near_to(left, 0.25f, 1e-5f), "four times: a quarter");
    s.max_distance = 5.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(near_to(left, 0.4f, 1e-5f), "held from the maximum distance");
    CHECK(audio_distance_gain(10.0f, 0.0f, 100.0f) == 1.0f, "no minimum distance, no rolloff");

    audio_source_default(&s);
    s.position.x = 5.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(left < 1e-3f && near_to(right, 0.2f, 1e-4f), "a source on the right: the left ear is silent");
    s.position.x = -5.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(right < 1e-3f && near_to(left, 0.2f, 1e-4f), "a source on the left");
    s.position.x = 0.0f;
    s.position.z = 5.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(near_to(left, right, 1e-6f), "straight ahead is centred");
    s.position.z = -5.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(near_to(left, right, 1e-6f), "straight behind is centred");
    s.position.x = 3.0f;
    s.position.z = 4.0f;
    audio_spatial_gains(&l, &s, &left, &right);
    CHECK(left < right && left > 0.0f, "ahead and to the right: left is quieter, not silent");

    audio_source_default(&s);
    s.position.x = 5.0f;
    l.front.x = 1.0f;
    l.front.z = 0.0f;
    audio_pan_for_bearing(&l, &s.position, &left, &right);
    CHECK(near_to(left, right, 1e-6f), "facing the source it is centred");
    l.front.x = 0.0f;
    l.front.z = 1.0f;
    s.position.x = 0.0f;
    s.position.z = 5.0f;
    audio_pan_for_bearing(&l, &s.position, &left, &right);
    CHECK(left == 1.0f && right == 1.0f, "the default orientation");

    audio_source_default(&s);
    s.position.z = 5.0f;
    s.cone_orientation.z = -1.0f;
    s.inside_angle = 60;
    s.outside_angle = 120;
    s.outside_volume = -2000;
    CHECK(near_to(audio_cone_gain(&s, &l.position), 1.0f, 1e-6f), "a source pointing at the listener is inside the cone");
    s.cone_orientation.z = 1.0f;
    CHECK(near_to(audio_cone_gain(&s, &l.position), 0.1f, 1e-4f), "pointing away: the outside volume");
    s.cone_orientation.x = 0.70710678f;
    s.cone_orientation.z = -0.70710678f;
    CHECK(near_to(audio_cone_gain(&s, &l.position), audio_volume_gain(-1000), 1e-3f), "between the cones: dB-linear");
    s.inside_angle = s.outside_angle = 360;
    s.cone_orientation.z = 1.0f;
    CHECK(audio_cone_gain(&s, &l.position) == 1.0f, "360/360 is no cone");
}

/* ---- static rules ----------------------------------------------------------------------------------------------- */

static char* read_text(const char* relative) {
    char path[MAX_PATH];
    FILE* f;
    long n;
    char* text;
    snprintf(path, sizeof path, SOURCE_DIR "%s", relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)n + 1, 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

static int contains_nocase(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!_strnicmp(hay, needle, n)) return 1;
    return 0;
}

static void check_file_free_of(const char* relative, const char* const* forbidden, size_t count) {
    char* text = read_text(relative);
    CHECK(text != NULL, "%s is readable", relative);
    if (!text) return;
    for (size_t i = 0; i < count; i++) CHECK(!contains_nocase(text, forbidden[i]), "%s does not mention %s", relative, forbidden[i]);
    free(text);
}

static void scan_tree(const char* relative_dir, char names[][MAX_PATH], int* count) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE h;
    snprintf(pattern, sizeof pattern, SOURCE_DIR "%s/*", relative_dir);
    h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char child[MAX_PATH];
        const char* ext = strrchr(data.cFileName, '.');
        if (data.cFileName[0] == '.') continue;
        snprintf(child, sizeof child, "%s/%s", relative_dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_tree(child, names, count);
        } else if (ext && (!_stricmp(ext, ".c") || !_stricmp(ext, ".cpp") || !_stricmp(ext, ".h") || !_stricmp(ext, ".inc")) &&
                   *count < 1024) {
            strcpy(names[(*count)++], child);
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
}

static int allowed_to_name_dsound(const char* path) {
    static const char* const allowed[] = { "src/host/audio/dsound_compat.c", "src/host/audio/dsound_compat.h",
                                           "src/host/audio/dsound_abi.h", "src/host/audio/dsound_methods.h",
                                           "src/host/audio/platform/windows_dsound_bridge.c", "src/host/audio/platform/windows_dsound_bridge.h",
                                           "src/host/audio/platform/windows_dsound_imports.c", "src/host/audio/platform/windows_dsound_imports.h",
                                           "src/host/audio/audio_policy.c", "src/host/audio/audio_policy.h" };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (!_stricmp(path, allowed[i])) return 1;
    return 0;
}

static void test_no_directsound_in_backend(void) {
    static const char* const portable[] = { "src/host/audio/audio_mixer.c", "src/host/audio/audio_mixer.h",
                                            "src/host/audio/audio_spatial.c", "src/host/audio/audio_spatial.h",
                                            "src/host/audio/audio_backend.h", "src/host/audio/sdl_audio.c",
                                            "src/host/audio/sdl_audio.h", "src/host/audio/audio_sync.c",
                                            "src/host/audio/audio_sync.h" };
    static const char* const forbidden_in_backend[] = { "dsound", "DirectSound", "windows.h", "<dsound", "XAudio", "OpenAL",
                                                        "miniaudio", "FMOD", "waveOut", "winmm" };
    static const char* const forbidden_everywhere[] = { "DirectSoundCreate", "dsound.dll", "dsound.lib", "dsound.h" };
    static char names[1024][MAX_PATH];
    int count = 0;
    for (size_t i = 0; i < sizeof portable / sizeof portable[0]; i++)
        check_file_free_of(portable[i], forbidden_in_backend, sizeof forbidden_in_backend / sizeof forbidden_in_backend[0]);
    scan_tree("src/host", names, &count);
    scan_tree("src/common", names, &count);
    scan_tree("src/host64", names, &count);
    CHECK(count > 150, "the source scan found the tree (%d files)", count);
    for (int i = 0; i < count; i++) {
        char* text;
        if (allowed_to_name_dsound(names[i])) continue;
        text = read_text(names[i]);
        if (!text) continue;
        for (size_t j = 0; j < sizeof forbidden_everywhere / sizeof forbidden_everywhere[0]; j++)
            CHECK(!contains_nocase(text, forbidden_everywhere[j]), "%s does not name %s", names[i], forbidden_everywhere[j]);
        free(text);
    }
}

static void test_sdl_backend_contract(void) {
    char* sdl = read_text("src/host/audio/sdl_audio.c");
    char* cmake = read_text("CMakeLists.txt");
    CHECK(sdl != NULL && cmake != NULL, "sources readable");
    if (sdl) {
        CHECK(strstr(sdl, "wasapi") == NULL && strstr(sdl, "pipewire") == NULL && strstr(sdl, "alsa") == NULL && strstr(sdl, "pulse") == NULL,
              "sdl_audio.c names no driver: the policy (audio_policy.c) does");
        CHECK(strstr(sdl, "audio_choose_driver(") != NULL && strstr(sdl, "SDL_getenv(\"SDL_AUDIODRIVER\")") != NULL && strstr(sdl, "if (choice.hint) SDL_SetHint(SDL_HINT_AUDIO_DRIVER") != NULL,
              "the driver comes from the policy, and an explicit SDL_AUDIODRIVER is never overridden");
        CHECK(strstr(sdl, "SDL_AUDIO_F32") && strstr(sdl, "SDL_OpenAudioDeviceStream") && strstr(sdl, "SDL_INIT_AUDIO"), "one SDL3 device stream of float samples");
        CHECK(strstr(sdl, "SDL_DestroyAudioStream(A.stream);\n    audio_mixer_destroy") != NULL,
              "the device stream is destroyed before the mixer its callback reads");
        CHECK(strstr(sdl, "SDL_malloc") == NULL && strstr(sdl, "SDL_calloc") == NULL && strstr(sdl, "malloc(") == NULL, "no allocation in the backend");
    }
    if (cmake) {
        CHECK(!contains_nocase(cmake, "dsound.lib") && !contains_nocase(cmake, "target_link_libraries(trespasser_recomp PRIVATE dsound"),
              "the program links no dsound import library");
        CHECK(strstr(cmake, "JPT_AUDIO_PORTABLE_SOURCES") != NULL && strstr(cmake, "JPT_AUDIO_PLATFORM_SOURCES") != NULL,
              "the program builds the portable audio sources and the platform bridge from the lists of cmake/JptAudio.cmake");
    }
    free(sdl);
    free(cmake);
}

static void test_wiring(void) {
    char* platform = read_text("src/host/platform.c");
    char* io = read_text("src/host/io_trace.c");
    char* main_c = read_text("src/host/main.c");
    char* exit_c = read_text("src/host/exit_policy.c");
    CHECK(platform && !strstr(platform, "SDL audio is not started") && !strstr(platform, "SDL_INIT_AUDIO"), "platform.c no longer claims the system DSound");
    CHECK(io && strstr(io, "audio_is_dsound_library(name)") && strstr(io, "dsound_host_module()"), "the game's LoadLibraryA gets the host's DSound");
    CHECK(io && strstr(io, "shim_GetProcAddress") && strstr(io, "dsound_get_proc_address"), "the game's GetProcAddress gets the host's DirectSoundCreate");
    CHECK(main_c && strstr(main_c, "jpt_dsound_bridge_install(sdl_audio_backend())") && strstr(main_c, "dsound_patch_imports"),
          "main installs the SDL backend through the Windows bridge and patches SMACKW32's imports");
    CHECK(main_c && strstr(main_c, "{ \"GetProcAddress\", shim_GetProcAddress }") && strstr(main_c, "{ \"FreeLibrary\", shim_FreeLibrary }"), "both shims are bound");
    CHECK(exit_c && strstr(exit_c, "dsound_compat_shutdown();") != NULL, "exit stops the audio");
    if (exit_c) {
        const char* a = strstr(exit_c, "dsound_compat_shutdown();");
        const char* b = strstr(exit_c, "platform_stop();");
        CHECK(a && b && a < b, "the audio stops before SDL quits");
    }
    free(platform);
    free(io);
    free(main_c);
    free(exit_c);
}

int main(void) {
    test_volume();
    test_menu_click();
    test_frequency();
    test_import_names();
    test_silence_and_one_shot();
    test_gain_pan_clip();
    test_loop_stop_restart();
    test_resampling();
    test_formats();
    test_stereo_and_8bit();
    test_lock();
    test_cursors_and_streaming();
    test_duplicate_and_lifetime();
    test_determinism();
    test_spatial();
    test_no_directsound_in_backend();
    test_sdl_backend_contract();
    test_wiring();
    printf("audio_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
