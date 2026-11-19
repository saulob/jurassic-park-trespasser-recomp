/* Voice-over playback through the DirectSound layer, as the retail game drives it for a Hammond narration: a looping
 * streaming buffer of 22050 Hz 16-bit mono (the format of the Stream.tpa voice-over records, descriptor 0x10110) with
 * software location and volume control, filled by the streaming thread from the play cursor, at a volume of -5 dB.
 * The audio is synthetic; nothing of the game is used and nothing is played. */
#include "dsound_guest_harness.h"

#define STREAM_FLAGS 0x88u /* DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME */
#define VOICE_BYTES 0x18150u /* the payload size of a real Hammond record (VH117) */
#define BUFFER_BYTES 0x8000u
#define RATE 22050u
#define POLL_FRAMES 1764u /* 80 ms: the streaming thread's poll */

static int16_t voice_sample(uint32_t frame) {
    const double phase = 6.283185307179586 * 440.0 * (double)frame / (double)RATE;
    return (int16_t)(12000.0 * sin(phase) + 4000.0 * sin(phase * 0.37));
}

static void write_voice(uint32_t address, uint32_t first_frame, uint32_t bytes, uint32_t total_frames) {
    int16_t* out = (int16_t*)arena_resolve(NULL, address, bytes);
    for (uint32_t i = 0; i < bytes / 2; i++) out[i] = first_frame + i < total_frames ? voice_sample(first_frame + i) : 0;
}

static float energy(const float* out, unsigned frames) {
    float sum = 0.0f;
    for (unsigned i = 0; i < frames * 2; i++) sum += out[i] * out[i];
    return sum;
}

/* The streaming thread: the buffer is filled once, played looping, and each 80 ms poll writes the bytes the play cursor
 * has passed with the next frames of the voice; it stops the voice when the cursor has covered the whole payload. */
static void test_streaming_voice_over(void) {
    uint32_t ds = make_device(), hr, b, p1, n1, p2, n2;
    const uint32_t total_frames = VOICE_BYTES / 2;
    float chunk[2 * POLL_FRAMES];
    const double gain = pow(10.0, -5.0 / 20.0);
    uint32_t frames_out = 0, silent_chunks = 0, wrong_samples = 0, written = 0, played = 0, previous = 0, next_frame = BUFFER_BYTES / 2;
    int stopped = 0;
    b = make_secondary(ds, STREAM_FLAGS, RATE, 1, 16, BUFFER_BYTES, &hr);
    CHECK(hr == DS_OK && b, "the voice-over buffer (LOCSOFTWARE | CTRLVOLUME, 22050 Hz mono 16-bit) is created: %08X", hr);
    CHECK(lock_buffer(b, 0, BUFFER_BYTES, 0, &p1, &n1, &p2, &n2) == DS_OK && n1 == BUFFER_BYTES && p2 == 0, "the first Lock takes the whole buffer");
    write_voice(p1, 0, BUFFER_BYTES, total_frames);
    CHECK(vcall(b, BUF_UNLOCK, p1, n1, p2, n2) == DS_OK, "Unlock");
    CHECK(vcall(b, BUF_SET_VOLUME, (uint32_t)-500) == DS_OK, "SetVolume(-5 dB)");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK && status_of(b) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "Play(looping)");
    for (int poll = 0; poll < 400 && !stopped; poll++) {
        uint32_t play, at = scratch(8), delta, fp1, fn1, fp2, fn2;
        mix(chunk, POLL_FRAMES);
        if (energy(chunk, POLL_FRAMES) == 0.0f && frames_out < total_frames) silent_chunks++;
        for (uint32_t i = 0; i < POLL_FRAMES && frames_out + i < total_frames; i++) {
            const float expected = (float)(gain * (double)voice_sample(frames_out + i) / 32768.0);
            if (!near_to(chunk[2 * i], expected, 2e-4f) || !near_to(chunk[2 * i + 1], expected, 2e-4f)) wrong_samples++;
        }
        frames_out += POLL_FRAMES;
        CHECK(vcall(b, BUF_GET_POSITION, at, at + 4) == DS_OK, "GetCurrentPosition with both pointers");
        play = get32(at);
        played += (play + BUFFER_BYTES - previous) % BUFFER_BYTES;
        previous = play;
        delta = (play + BUFFER_BYTES - written) % BUFFER_BYTES;
        if (delta) {
            CHECK(lock_buffer(b, written, delta, 0, &fp1, &fn1, &fp2, &fn2) == DS_OK && fn1 + fn2 == delta, "the refill Lock");
            write_voice(fp1, next_frame, fn1, total_frames);
            if (fp2) write_voice(fp2, next_frame + fn1 / 2, fn2, total_frames);
            vcall(b, BUF_UNLOCK, fp1, fn1, fp2, fn2);
            next_frame += delta / 2;
            written = (written + delta) % BUFFER_BYTES;
        }
        if (played > VOICE_BYTES) {
            CHECK(vcall(b, BUF_STOP) == DS_OK, "the thread stops the finished voice");
            stopped = 1;
        }
    }
    CHECK(wrong_samples == 0, "every sample that reaches the output is the voice at -5 dB, in order across the refills (%u wrong)", wrong_samples);
    CHECK(silent_chunks == 0, "no 80 ms poll of the narration is silent (%u)", silent_chunks);
    CHECK(stopped && frames_out >= total_frames && frames_out < total_frames + 3 * POLL_FRAMES, "the end is detected from the play cursor as the narration ends (%u of %u frames)", frames_out, total_frames);
    mix(chunk, POLL_FRAMES);
    CHECK(energy(chunk, POLL_FRAMES) == 0.0f && status_of(b) == 0, "after Stop the mixer is silent");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_voice_over_with_music_and_effects(void) {
    uint32_t ds = make_device(), hr, voice, music, effect, p1, n1, p2, n2;
    float out[2 * 64];
    voice = make_secondary(ds, STREAM_FLAGS, RATE, 1, 16, BUFFER_BYTES, &hr);
    CHECK(hr == DS_OK && voice, "voice-over");
    music = make_secondary(ds, STREAM_FLAGS, RATE, 2, 16, BUFFER_BYTES * 2, &hr);
    CHECK(hr == DS_OK && music, "stereo music at the same time");
    effect = make_secondary(ds, DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME, RATE, 1, 16, 4096, &hr);
    CHECK(hr == DS_OK && effect, "an effect");
    for (int k = 0; k < 3; k++) {
        uint32_t target = k == 0 ? voice : k == 1 ? music : effect, bytes = k == 0 ? BUFFER_BYTES : k == 1 ? BUFFER_BYTES * 2 : 4096;
        CHECK(lock_buffer(target, 0, bytes, 0, &p1, &n1, &p2, &n2) == DS_OK, "Lock %d", k);
        {
            int16_t* w = (int16_t*)arena_resolve(NULL, p1, n1);
            for (uint32_t i = 0; i < n1 / 2; i++) w[i] = k == 0 ? 6000 : k == 1 ? -3000 : 1000;
        }
        vcall(target, BUF_UNLOCK, p1, n1, p2, n2);
        CHECK(vcall(target, BUF_PLAY, 0u, 0u, k == 2 ? 0u : DSBPLAY_LOOPING) == DS_OK, "Play %d", k);
    }
    mix(out, 64);
    CHECK(near_to(out[0], (6000.0f - 3000.0f + 1000.0f) / 32768.0f, 1e-5f),
          "the three sources add: mono voice on both channels, stereo music, effect (%f)", out[0]);
    vcall(voice, BUF_STOP);
    mix(out, 64);
    CHECK(near_to(out[0], (-3000.0f + 1000.0f) / 32768.0f, 1e-5f), "stopping the narration leaves the others playing (%f)", out[0]);
    vcall(voice, BUF_RELEASE);
    vcall(music, BUF_RELEASE);
    vcall(effect, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_voice_over_buffer_contract(void) {
    uint32_t ds = make_device(), hr, b, q = scratch(4);
    static const struct {
        uint32_t flags;
        uint16_t channels, bits;
        uint32_t rate, bytes;
        uint32_t expected;
        const char* what;
    } k_cases[] = {
        { 0x88, 1, 16, 22050, 0x18150, DS_OK, "a streaming voice-over: software, volume" },
        { 0x88, 1, 16, 11025, 0x6db0, DS_OK, "the 11025 Hz voice-over records" },
        { 0x88, 2, 16, 22050, 0x20000, DS_OK, "music: stereo" },
        { 0x88, 1, 8, 22050, 0x4000, DS_OK, "8-bit mono" },
        { 0xA8, 1, 16, 22050, 0x8000, DS_OK, "with frequency control" },
        { 0x98, 1, 16, 22050, 0x8000, DS_OK, "a 3D mono voice" },
        { 0x82, 1, 16, 22050, 0x2000, DS_OK, "a static effect with volume" },
        { 0x88, 1, 16, 22050, 0x18151, DSERR_INVALIDPARAM, "an odd size is not a whole number of frames" },
        { 0x98, 2, 16, 22050, 0x8000, DSERR_INVALIDPARAM, "a 3D buffer must be mono" },
        { 0xD8, 1, 16, 22050, 0x8000, DSERR_INVALIDPARAM, "3D with pan control" },
        { 0x84, 1, 16, 22050, 0x8000, DSERR_ALLOCATED, "a hardware buffer is not available" },
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        b = make_secondary(ds, k_cases[i].flags, k_cases[i].rate, k_cases[i].channels, k_cases[i].bits, k_cases[i].bytes, &hr);
        CHECK(hr == k_cases[i].expected && (hr == DS_OK) == (b != 0), "%s: %08X", k_cases[i].what, hr);
        if (b) vcall(b, BUF_RELEASE);
    }
    b = make_secondary(ds, 0x98, RATE, 1, 16, 0x8000, &hr);
    CHECK(vcall(b, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q) == DS_OK && get32(q) != 0 && get32(q) != b, "a 3D voice-over has its 3D interface");
    CHECK(vcall(get32(q), BUF_ADDREF) == 3 && vcall(get32(q), BUF_RELEASE) == 2 && vcall(get32(q), BUF_RELEASE) == 1 && vcall(b, BUF_RELEASE) == 0,
          "AddRef and Release on the faces share one count and free the buffer once");
    (void)k_iid_unknown;
    (void)k_iid_buffer;
    (void)k_iid_listener;
    (void)k_iid_ks_property_set;
    (void)k_iid_notify;
    vcall(ds, DEV_RELEASE);
}

static void test_volume_and_frequency_of_a_voice_over(void) {
    uint32_t ds = make_device(), hr, b, p1, n1, p2, n2, at = scratch(4);
    float out[2 * 8];
    b = make_secondary(ds, 0xA8, RATE, 1, 16, 8, &hr);
    CHECK(hr == DS_OK, "a buffer with volume and frequency control");
    CHECK(lock_buffer(b, 0, 8, 0, &p1, &n1, &p2, &n2) == DS_OK, "Lock");
    {
        int16_t* w = (int16_t*)arena_resolve(NULL, p1, 8);
        w[0] = 16384;
        w[1] = 16384;
        w[2] = 16384;
        w[3] = 16384;
    }
    vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    CHECK(vcall(b, BUF_SET_VOLUME, (uint32_t)-500) == DS_OK && vcall(b, BUF_GET_VOLUME, at) == DS_OK && (int32_t)get32(at) == -500, "the narration volume -5 dB");
    mix(out, 2);
    CHECK(near_to(out[0], 0.5f * (float)pow(10.0, -0.25), 1e-4f) && out[0] > 0.2f && out[0] == out[0], "its gain is 0.5623 of the sample, not 0 or NaN (%f)", out[0]);
    CHECK(vcall(b, BUF_SET_VOLUME, (uint32_t)-602) == DS_OK, "the menu click level");
    mix(out, 2);
    CHECK(near_to(out[0], 0.25f, 0.002f), "-6.02 dB is half the amplitude as before (%f)", out[0]);
    CHECK(vcall(b, BUF_SET_VOLUME, (uint32_t)-10000) == DS_OK, "DSBVOLUME_MIN");
    mix(out, 2);
    CHECK(out[0] == 0.0f, "is silence");
    vcall(b, BUF_SET_VOLUME, 0u);
    CHECK(vcall(b, BUF_SET_FREQUENCY, DSBFREQUENCY_ORIGINAL) == DS_OK, "frequency 0 means the sample's own rate");
    mix(out, 4);
    CHECK(out[0] == 0.5f && out[6] == 0.5f, "and keeps playing at that rate");
    CHECK(vcall(b, BUF_SET_FREQUENCY, RATE) == DS_OK, "22050 Hz");
    mix(out, 4);
    CHECK(out[0] == 0.5f, "audible");
    CHECK(vcall(b, BUF_SET_FREQUENCY, 11025u) == DS_OK && vcall(b, BUF_SET_FREQUENCY, 44100u) == DS_OK, "11025 and 44100 Hz");
    mix(out, 4);
    CHECK(out[0] == 0.5f, "audible at 44100 Hz");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_one_shot_voice_over(void) {
    uint32_t ds = make_device(), hr, b, p1, n1, p2, n2, play = 7;
    float out[2 * 16];
    b = make_secondary(ds, 0x82, RATE, 1, 16, 16, &hr);
    CHECK(hr == DS_OK, "a static buffer");
    lock_buffer(b, 0, 16, 0, &p1, &n1, &p2, &n2);
    {
        int16_t* w = (int16_t*)arena_resolve(NULL, p1, 16);
        for (int i = 0; i < 8; i++) w[i] = 8192;
    }
    vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, 0u) == DS_OK && status_of(b) == DSBSTATUS_PLAYING, "Play without looping");
    mix(out, 1);
    CHECK(out[0] == 0.25f, "the first frame is the first sample, not silence");
    mix(out, 6);
    CHECK(status_of(b) == DSBSTATUS_PLAYING, "it is still playing after seven of eight frames");
    mix(out, 4);
    CHECK(status_of(b) == 0 && out[0] == 0.25f && out[2 * 1] == 0.0f, "it ends after the eighth frame: the last frame sounds, then silence");
    play = position_of(b, NULL);
    CHECK(play == 0, "and the cursor is back at the start");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, 0u) == DS_OK, "it can play again");
    mix(out, 1);
    CHECK(out[0] == 0.25f, "from the first sample");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_three_d_voice_over_is_audible(void) {
    uint32_t ds = make_device(), hr, b, q = scratch(4), p1, n1, p2, n2, s3d;
    float out[2 * 4];
    b = make_secondary(ds, 0x98, RATE, 1, 16, 8, &hr);
    CHECK(hr == DS_OK, "a 3D voice-over buffer");
    lock_buffer(b, 0, 8, 0, &p1, &n1, &p2, &n2);
    {
        int16_t* w = (int16_t*)arena_resolve(NULL, p1, 8);
        for (int i = 0; i < 4; i++) w[i] = 16384;
    }
    vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
    vcall(b, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q);
    s3d = get32(q);
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    mix(out, 1);
    CHECK(near_to(out[0], 0.5f, 1e-5f) && near_to(out[1], 0.5f, 1e-5f), "at the listener's position a 3D voice is at full level (%f)", out[0]);
    CHECK(vcall(s3d, S3D_SET_POSITION, float_bits(0), float_bits(0), float_bits(10), DS3D_IMMEDIATE) == DS_OK, "the game places it");
    mix(out, 1);
    CHECK(out[0] > 0.0f && near_to(out[0], out[1], 1e-6f), "ten metres in front it is quieter but not silent, and centred (%f)", out[0]);
    vcall(s3d, BUF_RELEASE);
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

int main(void) {
    G.memory = (uint8_t*)calloc(1, ARENA_BYTES);
    if (!G.memory) return 2;
    CHECK(dsound_compat_install(&k_backend, &(dsound_binding_t){ { NULL, arena_resolve, arena_alloc, arena_release }, thunk_address, NULL }) == 1, "the layer installs");
    test_voice_over_buffer_contract();
    test_streaming_voice_over();
    test_voice_over_with_music_and_effects();
    test_volume_and_frequency_of_a_voice_over();
    test_one_shot_voice_over();
    test_three_d_voice_over_is_audible();
    printf("hammond_voiceover_tests: %d checks, %d failures\n", g_checks, g_failures);
    free(G.memory);
    return g_failures ? 1 : 0;
}
