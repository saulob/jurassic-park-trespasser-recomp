/* The FFmpeg Smacker backend: which libraries and licences are behind it, that they carry the Smacker demuxer and
 * decoders, that a file that is not a decodable Smacker file is refused without leaving anything open, and that a
 * synthetic Smacker file (tests/smk_fixture.h, built here from nothing) decodes through the real libraries to the
 * pictures, palettes and PCM it was built from, directly and through the HLE. No game file and no downloaded asset is
 * used. Without FFmpeg in the build the suite reports DATA UNAVAILABLE for what it cannot check; it never turns that
 * into a pass. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smacker_backend.h"
#include "smacker_compat.h"
#include "smk_fixture.h"

#ifdef JPT_SMACKER_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif

static int g_checks, g_failures;

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

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

#ifdef JPT_SMACKER_FFMPEG
static void* arena_resolve(void* context, uint32_t address, uint32_t size) {
    static uint8_t memory[1 << 16];
    (void)context;
    if (address < 0x1000u || address - 0x1000u > sizeof memory || size > sizeof memory - (address - 0x1000u)) return NULL;
    return memory + (address - 0x1000u);
}

static uint32_t g_arena_used;

static uint32_t arena_alloc(void* context, uint32_t size) {
    uint32_t at = 0x1000u + g_arena_used;
    (void)context;
    if (size > 0x8000u) return 0;
    g_arena_used = (g_arena_used + size + 15u) & ~15u;
    if (g_arena_used > 0x8000u) g_arena_used = 0;
    return at;
}

static void arena_release(void* context, uint32_t address) {
    (void)context;
    (void)address;
}

static smk_memory_t g_file;
static int g_source_closes;

static int host_open_handle(void* context, uint32_t handle, smk_source_t* out) {
    (void)context;
    (void)handle;
    *out = smk_memory_source(&g_file);
    return 1;
}

static void host_close_source(void* context, smk_source_t* source) {
    (void)context;
    (void)source;
    g_source_closes++;
}

static void make_header(uint8_t* h) {
    memset(h, 0, SMK_HEADER_BYTES);
    memcpy(h, "SMK2", 4);
    h[4] = 64;
    h[8] = 40;
    h[12] = 5;
    h[16] = 67;
}

/* ---- the synthetic file, decoded for real ----------------------------------------------------------------------- */

#define PICTURE_BYTES (SMKFX_WIDTH * SMKFX_HEIGHT)
#define WIDE_BYTES (SMKFX_FRAMES * SMKFX_WIDE_SAMPLES_PER_FRAME * 2u)
#define NARROW_BYTES (SMKFX_FRAMES * SMKFX_NARROW_SAMPLES_PER_FRAME)

static uint8_t g_fixture[SMKFX_MAX_BYTES], g_work[SMKFX_MAX_BYTES];
static uint8_t g_expected_wide[WIDE_BYTES], g_expected_narrow[NARROW_BYTES];

static int picture_matches(unsigned frame, const uint8_t* picture) {
    for (unsigned y = 0; y < SMKFX_HEIGHT; y++)
        for (unsigned x = 0; x < SMKFX_WIDTH; x++)
            if (picture[y * SMKFX_WIDTH + x] != smkfx_pixel(frame, x, y)) return 0;
    return 1;
}

static int palette_matches(unsigned frame, const uint8_t* palette) {
    uint8_t expected[768];
    smkfx_palette(frame, expected);
    return !memcmp(expected, palette, sizeof expected);
}

typedef struct {
    unsigned decoded, good;
} decode_result_t;

/* Pulls up to `limit` frames; a frame is good when it is the expected picture and palette of its position. */
static decode_result_t decode_frames(const smk_backend_ops_t* backend, smk_backend_t* b, unsigned limit) {
    uint8_t picture[PICTURE_BYTES], palette[768];
    decode_result_t r = { 0, 0 };
    while (r.decoded < limit && backend->next_video(b, picture, palette)) {
        if (picture_matches(r.decoded, picture) && palette_matches(r.decoded, palette)) r.good++;
        r.decoded++;
    }
    return r;
}

/* Reads a track to its end in requests of changing sizes; returns the bytes taken, stored up to `capacity`. */
static uint32_t drain_track(const smk_backend_ops_t* backend, smk_backend_t* b, uint32_t track, uint8_t* out, uint32_t capacity) {
    static const uint32_t k_requests[] = { 1000u, 7u, 4096u, 2u, 300u };
    uint8_t piece[4096];
    uint32_t total = 0;
    for (unsigned turn = 0;; turn++) {
        const uint32_t asked = k_requests[turn % (sizeof k_requests / sizeof k_requests[0])];
        const uint32_t got = backend->next_audio(b, track, piece, asked);
        if (!got) return total;
        if (got > asked) return 0xFFFFFFFFu;
        if (total + got <= capacity) memcpy(out + total, piece, got);
        total += got;
    }
}

/* The backend keeps the pointer to its source: the source lives as long as the decoder. */
typedef struct {
    smk_memory_t memory;
    smk_source_t source;
} held_source_t;

static smk_backend_t* open_memory(const smk_backend_ops_t* backend, const uint8_t* bytes, uint32_t size, unsigned streams, held_source_t* held) {
    smk_header_t header;
    held->memory.bytes = bytes;
    held->memory.size = size;
    held->source = smk_memory_source(&held->memory);
    if (!smk_header_parse(bytes, size, &header)) return NULL;
    return backend->open(&held->source, &header, streams);
}

/* Opens the file with a change at `at` and drains whatever it gives: it must neither crash nor loop. */
static void abuse(const smk_backend_ops_t* backend, uint32_t size, unsigned* opened) {
    for (unsigned streams = SMK_STREAM_VIDEO; streams <= SMK_STREAM_AUDIO; streams++) {
        held_source_t held;
        smk_backend_t* b = open_memory(backend, g_work, size, streams, &held);
        uint8_t picture[PICTURE_BYTES], palette[768], piece[4096];
        if (!b) continue;
        (*opened)++;
        for (unsigned i = 0; i < 12 && backend->next_video(b, picture, palette); i++) {
        }
        for (unsigned i = 0; i < 64 && backend->next_audio(b, SMKFX_TRACK_WIDE, piece, sizeof piece); i++) {
        }
        for (unsigned i = 0; i < 64 && backend->next_audio(b, SMKFX_TRACK_NARROW, piece, sizeof piece); i++) {
        }
        backend->close(b);
    }
}

static uint16_t pack565(const uint8_t* rgb) { return (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3); }

static uint8_t* guest_bytes(uint32_t address, uint32_t size) { return (uint8_t*)arena_resolve(NULL, address, size); }

static uint32_t guest_u32(uint32_t address) {
    const uint8_t* p = guest_bytes(address, 4);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0xCDCDCDCDu;
}

/* SmackOpen, SmackToBuffer, then SmackDoFrame and SmackNextFrame over the whole file through the real decoder; `real` is
 * how many frames the file really holds (the header always says SMKFX_FRAMES). */
static void hle_play(const char* label, const uint8_t* bytes, uint32_t size, unsigned real, uint32_t format, const smk_backend_ops_t* backend) {
    const uint32_t bpp = format ? 2u : 1u, left = 3, top = 2, columns = SMKFX_WIDTH + 8u, rows = SMKFX_HEIGHT + 4u, pitch = columns * bpp;
    smk_host_t host;
    uint32_t open_args[3] = { 1u, SMK_OPEN_FILEHANDLE, 0xFFFFFFFFu }, smk, buffer, closes = (uint32_t)g_source_closes;
    uint8_t shown[768] = { 0 };
    int pixels_ok = 1, palettes_ok = 1, flags_ok = 1, border_ok = 1;
    unsigned frames_ok = 0;
    memset(&host, 0, sizeof host);
    host.guest.resolve = arena_resolve;
    host.guest.alloc = arena_alloc;
    host.guest.release = arena_release;
    host.backend = backend;
    host.open_handle = host_open_handle;
    host.close_source = host_close_source;
    g_file.bytes = bytes;
    g_file.size = size;
    g_arena_used = 0;
    CHECK(smacker_hle_install(&host), "%s: the HLE is installed over the FFmpeg backend", label);
    smk = smacker_hle_call(SMKX_SmackOpen, open_args);
    CHECK(smk != 0 && smacker_open_count() == 1, "%s: SmackOpen succeeds on the synthetic file", label);
    if (!smk) {
        smacker_hle_shutdown();
        return;
    }
    CHECK(guest_u32(smk + SMK_OFF_WIDTH) == SMKFX_WIDTH && guest_u32(smk + SMK_OFF_HEIGHT) == SMKFX_HEIGHT && guest_u32(smk + SMK_OFF_FRAMES) == SMKFX_FRAMES &&
              guest_u32(smk + SMK_OFF_RATE) == (uint32_t)SMKFX_FRAME_RATE,
          "%s: the Smack structure carries the file's width, height, frame count and frame time", label);
    buffer = arena_alloc(NULL, pitch * rows);
    memset(guest_bytes(buffer, pitch * rows), 0xAB, pitch * rows);
    {
        uint32_t args[7] = { smk, left, top, pitch, rows, buffer, format };
        smacker_hle_call(SMKX_SmackToBuffer, args);
    }
    for (unsigned f = 0; f < SMKFX_FRAMES; f++) {
        const unsigned shows = f < real ? f : real - 1u;
        const uint32_t args[1] = { smk };
        uint8_t palette[768];
        const uint8_t* dest = guest_bytes(buffer, pitch * rows);
        int frame_ok = 1;
        smkfx_palette(shows, palette);
        smacker_hle_call(SMKX_SmackDoFrame, args);
        flags_ok = flags_ok && guest_u32(smk + SMK_OFF_NEW_PALETTE) == (uint32_t)(f == 0 || (f < real && memcmp(palette, shown, sizeof palette) != 0));
        memcpy(shown, palette, sizeof shown);
        palettes_ok = palettes_ok && !memcmp(guest_bytes(smk + SMK_OFF_PALETTE, 768), palette, 768);
        for (unsigned y = 0; y < SMKFX_HEIGHT; y++)
            for (unsigned x = 0; x < SMKFX_WIDTH; x++) {
                const uint8_t index = smkfx_pixel(shows, x, y);
                const uint8_t* at = dest + (size_t)(top + y) * pitch + (size_t)(left + x) * bpp;
                if (format) {
                    const uint16_t v = (uint16_t)(at[0] | at[1] << 8);
                    frame_ok = frame_ok && v == pack565(palette + index * 3u);
                } else {
                    frame_ok = frame_ok && at[0] == index;
                }
            }
        pixels_ok = pixels_ok && frame_ok;
        if (frame_ok) frames_ok++;
        border_ok = border_ok && dest[0] == 0xAB && dest[(size_t)(top - 1u) * pitch + (size_t)(left + 5u) * bpp] == 0xAB &&
                    dest[(size_t)(top + SMKFX_HEIGHT) * pitch + (size_t)(left + 5u) * bpp] == 0xAB;
        smacker_hle_call(SMKX_SmackNextFrame, args);
        CHECK(guest_u32(smk + SMK_OFF_FRAME_NUMBER) == (f + 1u < SMKFX_FRAMES ? f + 1u : SMKFX_FRAMES - 1u), "%s: frame number after SmackNextFrame %u", label, f);
    }
    CHECK(pixels_ok && frames_ok == SMKFX_FRAMES, "%s: every SmackDoFrame converted the decoder's picture (%u of %u frames)", label, frames_ok, SMKFX_FRAMES);
    CHECK(palettes_ok, "%s: the palette in the Smack structure is the file's palette of each frame", label);
    CHECK(flags_ok, "%s: the new-palette flag is set when the palette changed and only then", label);
    CHECK(border_ok, "%s: nothing is written outside the picture's rectangle", label);
    {
        const uint32_t args[1] = { smk };
        smacker_hle_call(SMKX_SmackClose, args);
    }
    CHECK(smacker_open_count() == 0 && (uint32_t)g_source_closes == closes + 1u, "%s: SmackClose releases the decoder and the source", label);
    smacker_hle_shutdown();
}
#endif

int main(void) {
    const smk_backend_ops_t* ffmpeg = smk_ffmpeg_backend();
    char version[512];
    int described = smk_ffmpeg_describe(version, sizeof version);
#ifndef JPT_SMACKER_FFMPEG
    CHECK(ffmpeg == NULL && described == 0 && version[0] == 0, "a build without FFmpeg has no FFmpeg backend");
    printf("smacker_ffmpeg_tests: skipped - DATA UNAVAILABLE (FFmpeg is not part of this build: no libavformat and libavcodec development files)\n");
#else
    uint8_t header[SMK_HEADER_BYTES];
    smk_header_t parsed;
    smk_source_t source;
    smk_host_t host;
    CHECK(ffmpeg != NULL && ffmpeg->open && ffmpeg->close && ffmpeg->next_video && ffmpeg->next_audio && !strcmp(ffmpeg->name, "ffmpeg"), "the backend is complete");
    CHECK(described > 0 && strstr(version, "libavformat") && strstr(version, "libavcodec") && strstr(version, "libavutil"), "versions: %s", version);
    CHECK(!strstr(version, "nonfree"), "no non-free component is linked");
    if (strstr(version, "(GPL") || !strstr(version, "LGPL"))
        printf("note: this FFmpeg is not an LGPL-only build (%s): fine for development, a release must ship an LGPL build (docs/ARCHITECTURE.md)\n", version);
    CHECK(avcodec_find_decoder(AV_CODEC_ID_SMACKVIDEO) != NULL, "libavcodec has the Smacker video decoder");
    CHECK(avcodec_find_decoder(AV_CODEC_ID_SMACKAUDIO) != NULL, "libavcodec has the Smacker audio decoder");
    CHECK(av_find_input_format("smk") != NULL, "libavformat has the Smacker demuxer");
    CHECK(avformat_version() >> 16 >= 58 && avcodec_version() >> 16 >= 58, "libraries from FFmpeg 4 on");

    make_header(header);
    CHECK(smk_header_parse(header, sizeof header, &parsed), "the synthetic header parses");
    g_file.bytes = header;
    g_file.size = sizeof header;
    source = smk_memory_source(&g_file);
    CHECK(ffmpeg->open(&source, &parsed, SMK_STREAM_VIDEO) == NULL, "a header with no frame data is not decodable: refused, nothing leaks");
    CHECK(ffmpeg->open(&source, &parsed, SMK_STREAM_AUDIO) == NULL, "the same for an audio decoder");
    {
        uint8_t noise[400];
        smk_memory_t memory = { noise, sizeof noise };
        smk_source_t bad;
        for (unsigned i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)(i * 37u + 11u);
        bad = smk_memory_source(&memory);
        CHECK(ffmpeg->open(&bad, &parsed, SMK_STREAM_VIDEO) == NULL, "noise is refused");
        memory.size = 0;
        bad = smk_memory_source(&memory);
        CHECK(ffmpeg->open(&bad, &parsed, SMK_STREAM_VIDEO) == NULL, "an empty file is refused");
    }
    memset(&host, 0, sizeof host);
    host.guest.resolve = arena_resolve;
    host.guest.alloc = arena_alloc;
    host.guest.release = arena_release;
    host.backend = ffmpeg;
    host.open_handle = host_open_handle;
    host.close_source = host_close_source;
    CHECK(smacker_hle_install(&host), "the HLE accepts the FFmpeg backend");
    {
        uint32_t args[3] = { 1u, SMK_OPEN_FILEHANDLE, 0u };
        CHECK(smacker_hle_call(SMKX_SmackOpen, args) == 0 && smacker_open_count() == 0, "SmackOpen on an undecodable file returns NULL and keeps nothing open");
        CHECK(g_source_closes == 1, "and the source it opened is closed again");
    }
    smacker_hle_shutdown();

    /* The synthetic file. */
    {
        smk_header_t h;
        held_source_t memory;
        smk_backend_t *video, *audio, *second;
        uint32_t size = smkfx_build(g_fixture, sizeof g_fixture);
        uint8_t picture[PICTURE_BYTES], palette[768];
        static uint8_t got_wide[WIDE_BYTES + 16], got_narrow[NARROW_BYTES + 16];
        uint32_t got;
        decode_result_t result;
        unsigned opened = 0;
        for (uint32_t i = 0; i < SMKFX_FRAMES * SMKFX_WIDE_SAMPLES_PER_FRAME; i++) {
            g_expected_wide[i * 2u] = (uint8_t)smkfx_sample_wide(i);
            g_expected_wide[i * 2u + 1u] = (uint8_t)(smkfx_sample_wide(i) >> 8);
        }
        for (uint32_t i = 0; i < NARROW_BYTES; i++) g_expected_narrow[i] = smkfx_sample_narrow(i);

        CHECK(size > SMKFX_HEADER_BYTES && size < SMKFX_MAX_BYTES, "the fixture builds (%u bytes)", size);
        CHECK(smkfx_build(g_work, 100) == 0, "a buffer too small for the fixture is refused");
        CHECK(smk_header_parse(g_fixture, size, &h) && h.version == 2 && h.width == SMKFX_WIDTH && h.height == SMKFX_HEIGHT && h.frames == SMKFX_FRAMES &&
                  h.frame_rate == SMKFX_FRAME_RATE && h.flags == 0 && smk_frame_duration_us(h.frame_rate) == 100000u,
              "the fixture's header: 32x16, 6 frames, 100 ms");
        CHECK(h.track[SMKFX_TRACK_WIDE].rate == SMKFX_RATE_WIDE && h.track[SMKFX_TRACK_WIDE].bits == 16 && h.track[SMKFX_TRACK_WIDE].channels == 1 &&
                  h.track[1].rate == 0 && h.track[SMKFX_TRACK_NARROW].rate == SMKFX_RATE_NARROW && h.track[SMKFX_TRACK_NARROW].bits == 8 &&
                  h.track[SMKFX_TRACK_NARROW].channels == 1,
              "tracks: 0 = 16-bit mono 22050 Hz, 1 absent, 2 = 8-bit mono 11025 Hz");
        {
            uint8_t first[768], last[768];
            smkfx_palette(0, first);
            smkfx_palette(1, last);
            CHECK(!memcmp(first, last, 768), "the model: frame 1 keeps the palette of frame 0");
            smkfx_palette(2, last);
            CHECK(memcmp(first, last, 768) != 0, "the model: frame 2 changes the palette");
        }

        /* Pictures and palettes, in order, then the end. */
        video = open_memory(ffmpeg, g_fixture, size, SMK_STREAM_VIDEO, &memory);
        CHECK(video != NULL, "a video decoder opens on the fixture");
        if (video) {
            result = decode_frames(ffmpeg, video, SMKFX_FRAMES);
            CHECK(result.decoded == SMKFX_FRAMES, "the decoder yields exactly %u frames (%u)", SMKFX_FRAMES, result.decoded);
            CHECK(result.good == SMKFX_FRAMES, "every frame is the picture and palette it was built from, in file order (%u of %u)", result.good, SMKFX_FRAMES);
            CHECK(ffmpeg->next_video(video, picture, palette) == 0 && ffmpeg->next_video(video, picture, palette) == 0, "after the last frame next_video returns 0, again and again");
            CHECK(ffmpeg->next_audio(video, SMKFX_TRACK_WIDE, g_work, sizeof g_work) == 0, "a video-only decoder has no audio");
            ffmpeg->close(video);
        }

        /* Audio of each track, read apart from the other and in odd request sizes. */
        audio = open_memory(ffmpeg, g_fixture, size, SMK_STREAM_AUDIO, &memory);
        CHECK(audio != NULL, "an audio decoder opens on the fixture");
        if (audio) {
            CHECK(ffmpeg->next_video(audio, picture, palette) == 0, "an audio-only decoder has no pictures");
            memset(got_narrow, 0, sizeof got_narrow);
            got = drain_track(ffmpeg, audio, SMKFX_TRACK_NARROW, got_narrow, NARROW_BYTES);
            CHECK(got == NARROW_BYTES, "track 2 yields %u bytes of 8-bit PCM (%u)", NARROW_BYTES, got);
            CHECK(got == NARROW_BYTES && !memcmp(got_narrow, g_expected_narrow, NARROW_BYTES), "track 2's samples are the ones the file holds");
            memset(got_wide, 0, sizeof got_wide);
            got = drain_track(ffmpeg, audio, SMKFX_TRACK_WIDE, got_wide, WIDE_BYTES);
            CHECK(got == WIDE_BYTES, "track 0, read after track 2 has been read to its end, yields %u bytes of 16-bit PCM (%u)", WIDE_BYTES, got);
            CHECK(got == WIDE_BYTES && !memcmp(got_wide, g_expected_wide, WIDE_BYTES), "track 0's samples are the ones the file holds, little-endian");
            CHECK(ffmpeg->next_audio(audio, SMKFX_TRACK_WIDE, g_work, 64) == 0 && ffmpeg->next_audio(audio, SMKFX_TRACK_NARROW, g_work, 64) == 0, "both tracks end");
            CHECK(ffmpeg->next_audio(audio, 1, g_work, 64) == 0 && ffmpeg->next_audio(audio, SMK_TRACKS, g_work, 64) == 0 && ffmpeg->next_audio(audio, 0xFFFFFFFFu, g_work, 64) == 0,
                  "an absent track and a track out of range give nothing");
            ffmpeg->close(audio);
        }
        audio = open_memory(ffmpeg, g_fixture, size, SMK_STREAM_AUDIO, &memory);
        if (audio) {
            memset(got_wide, 0, sizeof got_wide);
            got = drain_track(ffmpeg, audio, SMKFX_TRACK_WIDE, got_wide, WIDE_BYTES);
            CHECK(got == WIDE_BYTES && !memcmp(got_wide, g_expected_wide, WIDE_BYTES), "track 0 read first is the same");
            memset(got_narrow, 0, sizeof got_narrow);
            got = drain_track(ffmpeg, audio, SMKFX_TRACK_NARROW, got_narrow, NARROW_BYTES);
            CHECK(got == NARROW_BYTES && !memcmp(got_narrow, g_expected_narrow, NARROW_BYTES), "and track 2 read after it");
            ffmpeg->close(audio);
        }

        /* Video and audio asked of one decoder: the pictures are still right. */
        video = open_memory(ffmpeg, g_fixture, size, SMK_STREAM_VIDEO | SMK_STREAM_AUDIO, &memory);
        CHECK(video != NULL, "a decoder for video and audio opens");
        if (video) {
            result = decode_frames(ffmpeg, video, SMKFX_FRAMES + 2u);
            CHECK(result.decoded == SMKFX_FRAMES && result.good == SMKFX_FRAMES, "its pictures are right with the audio streams present (%u of %u)", result.good, result.decoded);
            ffmpeg->close(video);
        }

        /* Two instances over one source, interleaved. */
        {
            smk_memory_t shared = { g_fixture, size };
            smk_source_t shared_source = smk_memory_source(&shared);
            video = ffmpeg->open(&shared_source, &h, SMK_STREAM_VIDEO);
            second = ffmpeg->open(&shared_source, &h, SMK_STREAM_VIDEO);
            audio = ffmpeg->open(&shared_source, &h, SMK_STREAM_AUDIO);
            CHECK(video && second && audio && video != second, "three decoders over the same source");
            if (video && second && audio) {
                uint8_t scratch[PICTURE_BYTES], scratch_palette[768];
                int independent = 1;
                for (unsigned i = 0; i < 3; i++) {
                    independent = independent && ffmpeg->next_video(video, scratch, scratch_palette) && picture_matches(i, scratch) && palette_matches(i, scratch_palette);
                }
                for (unsigned i = 0; i < SMKFX_FRAMES; i++) {
                    independent = independent && ffmpeg->next_video(second, scratch, scratch_palette) && picture_matches(i, scratch) && palette_matches(i, scratch_palette);
                    if (i == 1) {
                        uint8_t head[64];
                        independent = independent && ffmpeg->next_audio(audio, SMKFX_TRACK_WIDE, head, sizeof head) == sizeof head && !memcmp(head, g_expected_wide, sizeof head);
                    }
                }
                for (unsigned i = 3; i < SMKFX_FRAMES; i++)
                    independent = independent && ffmpeg->next_video(video, scratch, scratch_palette) && picture_matches(i, scratch) && palette_matches(i, scratch_palette);
                CHECK(independent && ffmpeg->next_video(video, scratch, scratch_palette) == 0 && ffmpeg->next_video(second, scratch, scratch_palette) == 0,
                      "two video decoders and an audio decoder over one source run independently and each reaches the end");
                CHECK(drain_track(ffmpeg, audio, SMKFX_TRACK_WIDE, g_work, sizeof g_work) == WIDE_BYTES - 64u, "the audio decoder kept its own position");
            }
            ffmpeg->close(audio);
            ffmpeg->close(second);
            ffmpeg->close(video);
        }

        /* A file that ends in the middle of a frame. */
        {
            const uint32_t starts[] = { smkfx_frame_start(g_fixture, 3) + 20u, smkfx_frame_start(g_fixture, 4) - 10u, smkfx_frame_start(g_fixture, 3) };
            for (unsigned i = 0; i < 3; i++) {
                video = open_memory(ffmpeg, g_fixture, starts[i], SMK_STREAM_VIDEO, &memory);
                CHECK(video != NULL, "cut %u: a file cut at byte %u still opens", i, starts[i]);
                if (video) {
                    result = decode_frames(ffmpeg, video, SMKFX_FRAMES);
                    CHECK(result.decoded == 3 && result.good == 3, "cut %u: the three whole frames decode, nothing after them (%u frames, %u good)", i, result.decoded, result.good);
                    CHECK(ffmpeg->next_video(video, picture, palette) == 0, "cut %u: and then the end", i);
                    ffmpeg->close(video);
                }
                audio = open_memory(ffmpeg, g_fixture, starts[i], SMK_STREAM_AUDIO, &memory);
                CHECK(audio != NULL, "cut %u: the audio opens too", i);
                if (audio) {
                    got = drain_track(ffmpeg, audio, SMKFX_TRACK_WIDE, got_wide, WIDE_BYTES);
                    CHECK(got >= 3u * smkfx_wide_bytes() && got <= 4u * smkfx_wide_bytes() && !memcmp(got_wide, g_expected_wide, got),
                          "cut %u: the audio of the whole frames is intact and ends (%u bytes)", i, got);
                    ffmpeg->close(audio);
                }
            }
            CHECK(open_memory(ffmpeg, g_fixture, 60, SMK_STREAM_VIDEO, &memory) == NULL, "a file cut inside the header is refused");
            CHECK(open_memory(ffmpeg, g_fixture, SMKFX_HEADER_BYTES + 5u * SMKFX_FRAMES + 40u, SMK_STREAM_VIDEO, &memory) == NULL, "a file cut inside the trees is refused");
        }

        /* Corrupted data. */
        memcpy(g_work, g_fixture, size);
        g_work[SMKFX_HEADER_BYTES + 5u * SMKFX_FRAMES] = 0;
        CHECK(open_memory(ffmpeg, g_work, size, SMK_STREAM_VIDEO, &memory) == NULL, "trees that are all absent are refused");
        memcpy(g_work, g_fixture, size);
        smkfx_put32(g_work + SMKFX_TREE_BYTES_OFFSET, 0xFFFFFFFFu);
        CHECK(open_memory(ffmpeg, g_work, size, SMK_STREAM_VIDEO, &memory) == NULL, "an absurd tree size is refused");
        memcpy(g_work, g_fixture, size);
        smkfx_put32(g_work + 68, 8u);
        CHECK(open_memory(ffmpeg, g_work, size, SMK_STREAM_VIDEO, &memory) == NULL, "a block-type tree larger than its declared size is refused");
        memcpy(g_work, g_fixture, size);
        {
            const uint32_t trees = SMKFX_HEADER_BYTES + 5u * SMKFX_FRAMES, end = smkfx_frame_start(g_fixture, 0);
            for (uint32_t at = trees; at < end; at++)
                for (unsigned mask = 0; mask < 3; mask++) {
                    g_work[at] = (uint8_t)(mask == 0 ? g_fixture[at] ^ 0xFFu : mask == 1 ? 0u : 0xFFu);
                    abuse(ffmpeg, size, &opened);
                    g_work[at] = g_fixture[at];
                }
            CHECK(opened > 0, "every one-byte change in the trees (%u..%u, three kinds) was survived (%u opened)", trees, end, opened);
        }
        opened = 0;
        for (uint32_t at = 0; at < size; at += 11u) {
            g_work[at] = (uint8_t)(g_fixture[at] ^ 0x5Au);
            abuse(ffmpeg, size, &opened);
            g_work[at] = g_fixture[at];
        }
        CHECK(opened > 0, "a change every 11 bytes through the whole file was survived (%u opened)", opened);
        for (uint32_t at = 0; at < size; at += 97u) {
            g_work[at] = 0xFFu;
            g_work[at + 1u < size ? at + 1u : at] = 0xFFu;
            g_work[at + 2u < size ? at + 2u : at] = 0xFFu;
            g_work[at + 3u < size ? at + 3u : at] = 0xFFu;
            abuse(ffmpeg, size, &opened);
            memcpy(g_work + at, g_fixture + at, size - at < 4u ? size - at : 4u);
        }
        CHECK(!memcmp(g_work, g_fixture, size), "the corruption sweep restored its file");

        /* The HLE over the real decoder. */
        hle_play("hle rgb565", g_fixture, size, SMKFX_FRAMES, SMK_BUFFER_565, ffmpeg);
        hle_play("hle indexes", g_fixture, size, SMKFX_FRAMES, 0, ffmpeg);
        hle_play("hle cut", g_fixture, smkfx_frame_start(g_fixture, 3) + 20u, 3, SMK_BUFFER_565, ffmpeg);
    }
#endif
    printf("smacker_ffmpeg_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
