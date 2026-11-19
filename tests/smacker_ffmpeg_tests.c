/* The boundary of the FFmpeg Smacker backend: which libraries and licences are behind it, that they carry the Smacker
 * demuxer and decoders, and that a file that is not a decodable Smacker file is refused without leaving anything open.
 * No game file and no downloaded asset is used. Without FFmpeg in the build the suite reports DATA UNAVAILABLE for
 * what it cannot check; it never turns that into a pass. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smacker_backend.h"
#include "smacker_compat.h"

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

static uint32_t arena_alloc(void* context, uint32_t size) {
    static uint32_t used;
    uint32_t at = 0x1000u + used;
    (void)context;
    if (size > 0x8000u) return 0;
    used = (used + size + 15u) & ~15u;
    if (used > 0x8000u) used = 0;
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
    if (strstr(version, "(GPL") || strstr(version, "GPL version") || !strstr(version, "LGPL"))
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
#endif
    printf("smacker_ffmpeg_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
