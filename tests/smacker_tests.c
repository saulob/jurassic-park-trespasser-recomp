/* The Smacker HLE: the 14 exports the retail executable imports from SMACKW32.DLL, called with the stack words of the
 * original calls, over a synthetic decoder, a fake clock and the real mixer. Guest memory is an arena whose addresses
 * are not host pointers. No Windows header, no FFmpeg, no game file, no audio device: the same source runs on Windows and
 * Linux. Nothing is played. */
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_sync.h"
#include "dsound_compat.h"
#include "smacker_compat.h"

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

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

/* ---- guest memory ----------------------------------------------------------------------------------------------- */

#define GUEST_BASE 0x30000000u
#define ARENA_BYTES (16u << 20)
#define BLOCK_LIMIT 4096

static struct {
    uint8_t* memory;
    uint32_t used;
    struct {
        uint32_t address;
        int live;
    } blocks[BLOCK_LIMIT];
    int block_count, live_blocks;
} G;

static void* arena_resolve(void* context, uint32_t address, uint32_t size) {
    (void)context;
    if (address < GUEST_BASE || address - GUEST_BASE > ARENA_BYTES || size > ARENA_BYTES - (address - GUEST_BASE)) return NULL;
    return G.memory + (address - GUEST_BASE);
}

static uint32_t arena_alloc(void* context, uint32_t size) {
    uint32_t at;
    (void)context;
    size = (size + 15u) & ~15u;
    if (G.block_count >= BLOCK_LIMIT || size > ARENA_BYTES - G.used) return 0;
    at = G.used;
    G.used += size;
    memset(G.memory + at, 0, size);
    G.blocks[G.block_count].address = GUEST_BASE + at;
    G.blocks[G.block_count].live = 1;
    G.block_count++;
    G.live_blocks++;
    return GUEST_BASE + at;
}

static void arena_release(void* context, uint32_t address) {
    (void)context;
    for (int i = 0; i < G.block_count; i++)
        if (G.blocks[i].address == address && G.blocks[i].live) {
            G.blocks[i].live = 0;
            G.live_blocks--;
            return;
        }
}

static uint32_t get32(uint32_t address) {
    uint32_t value = 0xCDCDCDCDu;
    void* host = arena_resolve(NULL, address, 4);
    if (host) memcpy(&value, host, 4);
    return value;
}

static uint16_t get16(uint32_t address) {
    uint16_t value = 0xCDCDu;
    void* host = arena_resolve(NULL, address, 2);
    if (host) memcpy(&value, host, 2);
    return value;
}

static uint8_t get8(uint32_t address) {
    const uint8_t* host = (const uint8_t*)arena_resolve(NULL, address, 1);
    return host ? *host : 0xCD;
}

static void put_text(uint32_t address, const char* text) { memcpy(arena_resolve(NULL, address, (uint32_t)strlen(text) + 1), text, strlen(text) + 1); }

/* ---- the synthetic file and decoder ----------------------------------------------------------------------------- */

#define HANDLE_FILE 0x1111u
#define FAKE_DEVICE 0xD5D50u
#define RATE 22050u

static uint8_t g_header[SMK_HEADER_BYTES];
static smk_memory_t g_file;

static void le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_header(uint32_t width, uint32_t height, uint32_t frames, int32_t rate, uint32_t flags, int audio) {
    memset(g_header, 0, sizeof g_header);
    memcpy(g_header, "SMK2", 4);
    le32(g_header + 4, width);
    le32(g_header + 8, height);
    le32(g_header + 12, frames);
    le32(g_header + 16, (uint32_t)rate);
    le32(g_header + 20, flags);
    if (audio) le32(g_header + 0x48, 0x40000000u | 0x20000000u | 0x10000000u | RATE);
    g_file.bytes = g_header;
    g_file.size = sizeof g_header;
}

static struct {
    int opens, closes, video_opens, audio_opens, fail_open;
    uint32_t video_frames, audio_chunks[SMK_TRACKS];
    uint32_t order_errors;
    uint32_t anim_x0, anim_y0, anim_x1, anim_y1;
    uint32_t palette_change_frame;
    int source_closes, path_opens, handle_opens;
    char last_path[600];
} K;

struct smk_backend {
    unsigned streams;
    smk_header_t header;
    uint32_t video_index, audio_index[SMK_TRACKS];
};

static uint8_t pixel_at(uint32_t frame, uint32_t x, uint32_t y) {
    if (x >= K.anim_x0 && x < K.anim_x1 && y >= K.anim_y0 && y < K.anim_y1) return (uint8_t)(frame * 7u + x);
    return (uint8_t)(x + y * 3u);
}

static void palette_for(uint32_t frame, uint8_t* palette) {
    for (unsigned i = 0; i < 256; i++) {
        palette[i * 3] = (uint8_t)i;
        palette[i * 3 + 1] = (uint8_t)(i * 2);
        palette[i * 3 + 2] = (uint8_t)(255 - i);
    }
    if (frame >= K.palette_change_frame)
        for (unsigned i = 0; i < 256; i++) palette[i * 3] = (uint8_t)(palette[i * 3] + 8);
}

static smk_backend_t* synth_open(const smk_source_t* source, const smk_header_t* header, unsigned streams) {
    smk_backend_t* b;
    (void)source;
    if (K.fail_open) return NULL;
    b = (smk_backend_t*)calloc(1, sizeof *b);
    b->streams = streams;
    b->header = *header;
    K.opens++;
    if (streams & SMK_STREAM_VIDEO) K.video_opens++;
    if (streams & SMK_STREAM_AUDIO) K.audio_opens++;
    return b;
}

static void synth_close(smk_backend_t* b) {
    K.closes++;
    free(b);
}

static int synth_next_video(smk_backend_t* b, uint8_t* picture, uint8_t* palette) {
    if (!(b->streams & SMK_STREAM_VIDEO) || b->video_index >= b->header.frames) return 0;
    for (uint32_t y = 0; y < b->header.height; y++)
        for (uint32_t x = 0; x < b->header.width; x++) picture[y * b->header.width + x] = pixel_at(b->video_index, x, y);
    palette_for(b->video_index, palette);
    b->video_index++;
    K.video_frames++;
    return 1;
}

static uint32_t synth_next_audio(smk_backend_t* b, uint32_t track, uint8_t* out, uint32_t capacity) {
    const uint32_t frames_per_chunk = RATE * (uint32_t)(b->header.frame_rate > 0 ? b->header.frame_rate : 67) / 1000u, bytes = frames_per_chunk * 4u;
    if (!(b->streams & SMK_STREAM_AUDIO) || track >= SMK_TRACKS || !b->header.track[track].rate || b->audio_index[track] >= b->header.frames || bytes > capacity) return 0;
    for (uint32_t i = 0; i < frames_per_chunk; i++) {
        int16_t v = 8192;
        memcpy(out + i * 4u, &v, 2);
        memcpy(out + i * 4u + 2u, &v, 2);
    }
    b->audio_index[track]++;
    K.audio_chunks[track]++;
    return bytes;
}

static const smk_backend_ops_t k_synthetic = { "synthetic", synth_open, synth_close, synth_next_video, synth_next_audio };

/* ---- the host ---------------------------------------------------------------------------------------------------- */

static uint64_t g_now;
static audio_mixer_t* g_mixer;
static int g_use_dsound_glue;

static int host_open_handle(void* context, uint32_t handle, smk_source_t* out) {
    (void)context;
    if (handle != HANDLE_FILE) return 0;
    K.handle_opens++;
    *out = smk_memory_source(&g_file);
    return 1;
}

static int host_open_path(void* context, const char* path, smk_source_t* out) {
    (void)context;
    K.path_opens++;
    snprintf(K.last_path, sizeof K.last_path, "%s", path);
    if (strcmp(path, "data/menu/test.smk")) return 0;
    *out = smk_memory_source(&g_file);
    return 1;
}

static void host_close_source(void* context, smk_source_t* source) {
    (void)context;
    (void)source;
    K.source_closes++;
}

static audio_mixer_t* host_mixer_for_device(void* context, uint32_t device) {
    (void)context;
    return device == FAKE_DEVICE ? g_mixer : NULL;
}

static uint64_t host_now(void* context) {
    (void)context;
    return g_now;
}

static smk_host_t make_host(void) {
    smk_host_t h;
    memset(&h, 0, sizeof h);
    h.guest.resolve = arena_resolve;
    h.guest.alloc = arena_alloc;
    h.guest.release = arena_release;
    h.backend = &k_synthetic;
    h.open_handle = host_open_handle;
    h.open_path = host_open_path;
    h.close_source = host_close_source;
    h.mixer_for_device = g_use_dsound_glue ? smk_dsound_mixer_for_device : host_mixer_for_device;
    h.now_us = host_now;
    return h;
}

static void reset(void) {
    smacker_hle_shutdown();
    audio_mixer_destroy(g_mixer);
    g_mixer = audio_mixer_create(RATE, NULL);
    memset(&K, 0, sizeof K);
    K.palette_change_frame = 0xFFFFFFFFu;
    g_now = 1000000;
    g_use_dsound_glue = 0;
    {
        smk_host_t h = make_host();
        smacker_hle_install(&h);
    }
}

static uint32_t call(smk_export_t id, uint32_t first, ...) {
    uint32_t words[8] = { first };
    va_list ap;
    uint32_t count = smacker_export_words(id);
    va_start(ap, first);
    for (uint32_t i = 1; i < count; i++) words[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return smacker_hle_call(id, words);
}

static uint32_t open_smk(uint32_t flags) { return call(SMKX_SmackOpen, HANDLE_FILE, flags | SMK_OPEN_FILEHANDLE, 0xFFFFFFFFu); }

static uint16_t pack565(const uint8_t* rgb) { return (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3); }

static void pull(uint32_t frames) {
    float out[1024];
    while (frames) {
        uint32_t n = frames < 512 ? frames : 512;
        audio_mixer_mix(g_mixer, out, n);
        frames -= n;
    }
}

typedef struct {
    uint32_t buffer, pitch, height, left, top;
} dest_t;

static dest_t set_destination(uint32_t smk, uint32_t pixels_wide, uint32_t rows, int32_t left, int32_t top, uint32_t format) {
    dest_t d;
    const uint32_t bpp = format ? 2u : 1u;
    d.pitch = pixels_wide * bpp;
    d.height = rows;
    d.left = (uint32_t)left;
    d.top = (uint32_t)top;
    d.buffer = arena_alloc(NULL, d.pitch * rows);
    call(SMKX_SmackToBuffer, smk, d.left, d.top, d.pitch, d.height, d.buffer, format);
    return d;
}

/* ---- exports and binding ---------------------------------------------------------------------------------------- */

static void test_exports(void) {
    static const char* const k_imported[] = { "_SmackBufferClose@4", "_SmackClose@4", "_SmackSoundOnOff@8", "_SmackWait@4", "_SmackBufferOpen@24",
                                              "_SmackNextFrame@4", "_SmackOpen@12", "_SmackColorRemapWithTrans@20", "_SmackDoFrame@4", "_SmackToBufferRect@8",
                                              "_SmackSoundUseDirectSound@4", "_SmackToBuffer@28", "_SmackBufferNewPalette@12", "_SmackBufferBlit@32" };
    smk_export_t found;
    int all = 1;
    CHECK(SMKX_COUNT == 14, "the executable imports exactly 14 Smacker functions (%d)", SMKX_COUNT);
    for (size_t i = 0; i < sizeof k_imported / sizeof k_imported[0]; i++) {
        all = all && smacker_find_export(k_imported[i], &found) && !strcmp(smacker_export_name(found), k_imported[i]);
        CHECK(smacker_find_export(k_imported[i], &found), "%s is an export of the HLE", k_imported[i]);
    }
    CHECK(all, "every import of the retail executable resolves to the export of that name");
    for (int i = 0; i < SMKX_COUNT; i++) {
        const char* name = smacker_export_name((smk_export_t)i);
        const char* at = strchr(name, '@');
        CHECK(name[0] == '_' && at && (uint32_t)atoi(at + 1) == smacker_export_words((smk_export_t)i) * 4u, "%s: stdcall bytes are four per stack word", name);
    }
    CHECK(smacker_find_export("SmackOpen", &found) && found == SMKX_SmackOpen && smacker_find_export("_SmackWait@4", &found) && found == SMKX_SmackWait,
          "found by plain or decorated name");
    CHECK(!smacker_find_export("SmackGoto", NULL) && !smacker_find_export("_SmackOpen@8", NULL) && !smacker_find_export("", NULL) && !smacker_find_export(NULL, NULL) &&
              !smacker_find_export("smackopen", NULL),
          "exports the game does not import are not provided; matching is exact");
    CHECK(smacker_is_library("SMACKW32.DLL") && smacker_is_library("smackw32.dll") && smacker_is_library("C:\\Game\\Smackw32.dll") && smacker_is_library("./x/SMACKW32.DLL") &&
              !smacker_is_library("smackw32.dll.bak") && !smacker_is_library("xsmackw32.dll") && !smacker_is_library(NULL) && !smacker_is_library("dsound.dll"),
          "the pseudo-module answers SMACKW32.DLL by any path and case, nothing else");
    CHECK(smacker_hle_call((smk_export_t)99, (const uint32_t[]){ 0 }) == 0 && smacker_hle_call((smk_export_t)-1, (const uint32_t[]){ 0 }) == 0, "an unknown export does nothing");
}

static void test_binding_policy(void) {
    CHECK(smk_binding_for_platform(JPT_PLATFORM_WINDOWS) == SMK_BINDING_ORIGINAL_DLL, "Windows keeps the original SMACKW32.DLL");
    CHECK(smk_binding_for_platform(JPT_PLATFORM_LINUX) == SMK_BINDING_HLE && smk_binding_for_platform(JPT_PLATFORM_OTHER) == SMK_BINDING_HLE,
          "Linux and every other platform use the HLE");
    CHECK(smk_binding_needs_dll(SMK_BINDING_ORIGINAL_DLL) && !smk_binding_needs_dll(SMK_BINDING_HLE), "only the Windows binding needs the DLL");
    CHECK(smk_binding_for_platform(jpt_current_platform()) == (jpt_current_platform() == JPT_PLATFORM_WINDOWS ? SMK_BINDING_ORIGINAL_DLL : SMK_BINDING_HLE),
          "this build picks the binding of its own platform");
}

static void test_header(void) {
    smk_header_t h;
    make_header(640, 174, 100, -6667, SMK_FLAG_Y_INTERLACED, 1);
    CHECK(smk_header_parse(g_header, sizeof g_header, &h) && h.width == 640 && h.height == 174 && h.frames == 100 && h.frame_rate == -6667 &&
              h.flags == SMK_FLAG_Y_INTERLACED && h.version == 2,
          "the file header");
    CHECK(h.track[0].rate == RATE && h.track[0].bits == 16 && h.track[0].channels == 2 && h.track[1].rate == 0, "track 0: 22050 Hz, 16 bits, stereo; no other track");
    CHECK(smk_shown_height(&h) == 348, "Y-interlaced pictures are shown at twice their stored height");
    make_header(640, 240, 10, 67, 0, 0);
    smk_header_parse(g_header, sizeof g_header, &h);
    CHECK(smk_shown_height(&h) == 240 && h.track[0].rate == 0, "a progressive picture, no audio");
    make_header(640, 240, 10, 67, SMK_FLAG_Y_DOUBLED, 0);
    smk_header_parse(g_header, sizeof g_header, &h);
    CHECK(smk_shown_height(&h) == 480, "Y-doubled pictures too");
    CHECK(smk_frame_duration_us(67) == 67000u && smk_frame_duration_us(-6667) == 66670u && smk_frame_duration_us(0) == 100000u,
          "frame times: positive is milliseconds, negative tens of microseconds, zero ten frames per second");
    memcpy(g_header, "SMK4", 4);
    CHECK(smk_header_parse(g_header, sizeof g_header, &h) && h.version == 4, "SMK4");
    memcpy(g_header, "SMK3", 4);
    CHECK(!smk_header_parse(g_header, sizeof g_header, &h), "another signature");
    memcpy(g_header, "SMK2", 4);
    CHECK(!smk_header_parse(g_header, SMK_HEADER_BYTES - 1, &h) && !smk_header_parse(NULL, 0, &h), "a short header");
    le32(g_header + 4, 0);
    CHECK(!smk_header_parse(g_header, sizeof g_header, &h), "no picture");
    le32(g_header + 4, 640);
    le32(g_header + 12, 0);
    CHECK(!smk_header_parse(g_header, sizeof g_header, &h), "no frames");
    le32(g_header + 12, 10);
    le32(g_header + 4, 100000);
    CHECK(!smk_header_parse(g_header, sizeof g_header, &h), "an absurd width");
}

/* ---- open and close --------------------------------------------------------------------------------------------- */

static void test_open_close(void) {
    uint32_t smk, name, live;
    reset();
    make_header(64, 40, 20, 67, 0, 0);
    live = (uint32_t)G.live_blocks;
    smk = open_smk(SMK_OPEN_NEEDVOLUME | SMK_OPEN_TRACKS);
    CHECK(smk >= GUEST_BASE && smk < GUEST_BASE + ARENA_BYTES && smacker_open_count() == 1, "SmackOpen returns the structure in guest memory (%08X)", smk);
    CHECK(K.handle_opens == 1 && K.path_opens == 0 && K.video_opens == 1 && K.audio_opens == 0, "a handle flag opens the handle, one video decoder, no audio decoder for a file without audio");
    CHECK(get32(smk + SMK_OFF_VERSION) == 0x324B4D53u && get32(smk + SMK_OFF_WIDTH) == 64 && get32(smk + SMK_OFF_HEIGHT) == 40 && get32(smk + SMK_OFF_FRAMES) == 20 &&
              get32(smk + SMK_OFF_RATE) == 67 && get32(smk + SMK_OFF_FLAGS) == 0 && get32(smk + SMK_OFF_FRAME_NUMBER) == 0,
          "the Smack structure holds SMK2, width, height, frames, the raw frame rate, flags and frame 0 at the offsets the game reads");
    call(SMKX_SmackClose, smk);
    CHECK(smacker_open_count() == 0 && K.closes == 1 && K.source_closes == 1 && (uint32_t)G.live_blocks == live, "SmackClose frees the decoder, the source and the structure");
    call(SMKX_SmackClose, smk);
    call(SMKX_SmackClose, 0);
    call(SMKX_SmackClose, 0x12345678u);
    CHECK(K.closes == 1 && K.source_closes == 1, "closing twice, NULL or a made-up pointer does nothing");

    name = arena_alloc(NULL, 64);
    put_text(name, "data/menu/test.smk");
    smk = call(SMKX_SmackOpen, name, 0u, 0xFFFFFFFFu);
    CHECK(smk != 0 && K.path_opens == 1 && K.handle_opens == 1 && !strcmp(K.last_path, "data/menu/test.smk"), "without the handle flag the first argument is a guest string");
    call(SMKX_SmackClose, smk);
    put_text(name, "data/menu/missing.smk");
    CHECK(call(SMKX_SmackOpen, name, 0u, 0u) == 0 && smacker_open_count() == 0, "an unknown file");
    CHECK(call(SMKX_SmackOpen, 0xFFFFFFF0u, 0u, 0u) == 0, "a name outside guest memory");
    CHECK(call(SMKX_SmackOpen, 0x2222u, SMK_OPEN_FILEHANDLE, 0u) == 0, "an unknown handle");

    le32(g_header + 4, 0);
    CHECK(open_smk(0) == 0 && K.source_closes == 3, "a file that is not Smacker is refused and its source closed again");
    make_header(64, 40, 20, 67, 0, 0);
    K.fail_open = 1;
    CHECK(open_smk(0) == 0 && smacker_open_count() == 0 && (uint32_t)G.live_blocks == live + 1, "a decoder that fails to open leaves nothing behind");
    K.fail_open = 0;

    smk = open_smk(0);
    call(SMKX_SmackOpen, HANDLE_FILE, SMK_OPEN_FILEHANDLE, 0u);
    CHECK(smacker_open_count() == 2, "two videos can be open");
    smacker_hle_shutdown();
    CHECK(smacker_open_count() == 0 && K.closes == K.opens, "the HLE shutdown closes every open video");
    {
        smk_host_t h = make_host();
        smacker_hle_install(&h);
    }
    CHECK(call(SMKX_SmackOpen, HANDLE_FILE, SMK_OPEN_FILEHANDLE, 0u) != 0, "and the HLE can be installed again");
    smacker_hle_shutdown();
    CHECK(call(SMKX_SmackOpen, HANDLE_FILE, SMK_OPEN_FILEHANDLE, 0u) == 0, "without a host nothing opens");
}

/* ---- frames ----------------------------------------------------------------------------------------------------- */

static void test_frame_progression_and_pixels(void) {
    uint32_t smk, rect_results = 0;
    dest_t d;
    uint8_t palette[768];
    reset();
    make_header(64, 40, 5, 67, 0, 0);
    smk = open_smk(0);
    d = set_destination(smk, 100, 60, 10, 5, SMK_BUFFER_565);
    CHECK(call(SMKX_SmackDoFrame, smk) == 0, "SmackDoFrame returns 0");
    palette_for(0, palette);
    {
        int ok = 1;
        for (uint32_t y = 0; y < 40; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint8_t index = pixel_at(0, x, y);
                ok = ok && get16(d.buffer + (d.top + y) * d.pitch + (d.left + x) * 2) == pack565(palette + index * 3u);
            }
        CHECK(ok, "frame 0 is converted to RGB565 through the palette and placed at left and top");
    }
    CHECK(get16(d.buffer + d.top * d.pitch + (d.left - 1) * 2) == 0 && get16(d.buffer + (d.top - 1) * d.pitch + d.left * 2) == 0 &&
              get16(d.buffer + (d.top + 40) * d.pitch + d.left * 2) == 0 && get16(d.buffer + d.top * d.pitch + (d.left + 64) * 2) == 0,
          "nothing outside the picture is written");
    CHECK(get32(smk + SMK_OFF_NEW_PALETTE) == 1 && get8(smk + SMK_OFF_PALETTE + 3) == 1 && get8(smk + SMK_OFF_PALETTE + 4) == 2 && get8(smk + SMK_OFF_PALETTE + 5) == 254,
          "the first frame reports a new palette and the Smack structure holds its RGB bytes");
    while (call(SMKX_SmackToBufferRect, smk, 1u)) rect_results++;
    CHECK(rect_results == 1, "the first frame has one changed rectangle");
    for (uint32_t frame = 1; frame < 5; frame++) {
        call(SMKX_SmackNextFrame, smk);
        CHECK(get32(smk + SMK_OFF_FRAME_NUMBER) == frame, "SmackNextFrame moves to frame %u", frame);
        call(SMKX_SmackDoFrame, smk);
    }
    CHECK(K.video_frames == 5, "five frames decoded, each once and in order (%u)", K.video_frames);
    call(SMKX_SmackNextFrame, smk);
    CHECK(get32(smk + SMK_OFF_FRAME_NUMBER) == 4, "the last frame stays the last: the player stops there");
    call(SMKX_SmackDoFrame, smk);
    CHECK(K.video_frames == 5, "a frame is not decoded twice");
    call(SMKX_SmackClose, smk);
}

static void test_destination_formats_and_clipping(void) {
    uint32_t smk;
    dest_t d;
    uint8_t palette[768];
    reset();
    make_header(16, 8, 3, 67, 0, 0);
    smk = open_smk(0);
    palette_for(0, palette);
    d = set_destination(smk, 20, 12, 2, 1, SMK_BUFFER_555);
    call(SMKX_SmackDoFrame, smk);
    {
        const uint8_t* rgb = palette + pixel_at(0, 3, 2) * 3u;
        uint16_t expected = (uint16_t)((rgb[0] >> 3) << 10 | (rgb[1] >> 3) << 5 | rgb[2] >> 3);
        CHECK(get16(d.buffer + (1 + 2) * d.pitch + (2 + 3) * 2) == expected, "RGB555 when the game asks for 0x80000000");
    }
    d = set_destination(smk, 20, 12, 2, 1, 0);
    call(SMKX_SmackDoFrame, smk);
    CHECK(get8(d.buffer + (1 + 2) * d.pitch + (2 + 3)) == pixel_at(0, 3, 2), "8-bit destinations get the palette indexes");
    d = set_destination(smk, 10, 6, -4, -2, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    {
        int ok = 1;
        for (uint32_t y = 0; y < 6; y++)
            for (uint32_t x = 0; x < 10; x++) {
                int sx = (int)x + 4, sy = (int)y + 2;
                uint16_t expected = (sx < 16 && sy < 8) ? pack565(palette + pixel_at(0, (uint32_t)sx, (uint32_t)sy) * 3u) : 0;
                ok = ok && get16(d.buffer + y * d.pitch + x * 2) == expected;
            }
        CHECK(ok, "a negative left and top crop the picture, a destination smaller than the picture clips it");
    }
    {
        uint32_t guard;
        d = set_destination(smk, 8, 4, 6, 2, SMK_BUFFER_565);
        guard = arena_alloc(NULL, 64);
        memset(arena_resolve(NULL, guard, 64), 0xEE, 64);
        call(SMKX_SmackDoFrame, smk);
        CHECK(get16(d.buffer + 2 * d.pitch + 6 * 2) != 0 && get16(d.buffer + 2 * d.pitch + 7 * 2) != 0 && get8(guard) == 0xEE && get8(guard + 63) == 0xEE,
              "the last columns of the destination are written and nothing past its pitch");
    }
    call(SMKX_SmackToBuffer, smk, 0u, 0u, 32u, 8u, 0xFFFFFFF0u, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    CHECK(1, "a destination outside guest memory is ignored, not written");
    call(SMKX_SmackToBuffer, smk, 0u, 0u, 0u, 8u, d.buffer, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    call(SMKX_SmackClose, smk);
    smk = open_smk(0);
    call(SMKX_SmackDoFrame, smk);
    CHECK(call(SMKX_SmackToBufferRect, smk, 1u) == 0, "a frame decoded before any destination exists reports no rectangle");
    call(SMKX_SmackClose, smk);
}

static void rect_of(uint32_t smk, int32_t* x, int32_t* y, int32_t* w, int32_t* h) {
    *x = (int32_t)get32(smk + SMK_OFF_RECT_X);
    *y = (int32_t)get32(smk + SMK_OFF_RECT_Y);
    *w = (int32_t)get32(smk + SMK_OFF_RECT_W);
    *h = (int32_t)get32(smk + SMK_OFF_RECT_H);
}

static void test_changed_rectangles(void) {
    uint32_t smk;
    int32_t x, y, w, h;
    reset();
    make_header(64, 40, 8, 67, 0, 0);
    K.anim_x0 = 8;
    K.anim_y0 = 4;
    K.anim_x1 = 16;
    K.anim_y1 = 10;
    K.palette_change_frame = 4;
    smk = open_smk(0);
    set_destination(smk, 100, 60, 10, 5, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    CHECK(call(SMKX_SmackToBufferRect, smk, 1u) == 1, "first frame: a rectangle");
    rect_of(smk, &x, &y, &w, &h);
    CHECK(x == 10 && y == 5 && w == 64 && h == 40, "the whole picture, in destination coordinates (%d,%d %dx%d)", x, y, w, h);
    CHECK(call(SMKX_SmackToBufferRect, smk, 1u) == 0, "one rectangle per frame, then 0");
    call(SMKX_SmackNextFrame, smk);
    call(SMKX_SmackDoFrame, smk);
    CHECK(call(SMKX_SmackToBufferRect, smk, 1u) == 1, "frame 1: the animated part changed");
    rect_of(smk, &x, &y, &w, &h);
    CHECK(x == 18 && y == 9 && w == 8 && h == 8, "its 4x4 blocks only (%d,%d %dx%d)", x, y, w, h);
    K.anim_x1 = K.anim_x0;
    call(SMKX_SmackNextFrame, smk);
    call(SMKX_SmackDoFrame, smk);
    call(SMKX_SmackToBufferRect, smk, 1u);
    call(SMKX_SmackNextFrame, smk);
    call(SMKX_SmackDoFrame, smk);
    CHECK(call(SMKX_SmackToBufferRect, smk, 1u) == 0 && get32(smk + SMK_OFF_NEW_PALETTE) == 0, "a frame identical to the last one has no rectangle and no new palette");
    call(SMKX_SmackNextFrame, smk);
    call(SMKX_SmackDoFrame, smk);
    CHECK(get32(smk + SMK_OFF_NEW_PALETTE) == 1 && call(SMKX_SmackToBufferRect, smk, 1u) == 0,
          "a palette change alone is reported but repaints nothing: the original rewrites only blocks whose pixels changed");
    call(SMKX_SmackClose, smk);

    reset();
    make_header(64, 40, 3, 67, 0, 0);
    smk = open_smk(0);
    set_destination(smk, 50, 30, 10, 5, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    call(SMKX_SmackToBufferRect, smk, 1u);
    rect_of(smk, &x, &y, &w, &h);
    CHECK(x == 10 && y == 5 && w == 40 && h == 25, "the rectangle is clipped to the destination (%d,%d %dx%d)", x, y, w, h);
    call(SMKX_SmackClose, smk);
}

static void test_blocks_keep_their_colours_after_a_palette_change(void) {
    uint32_t smk;
    dest_t d;
    uint8_t old_palette[768], new_palette[768];
    reset();
    make_header(16, 8, 4, 67, 0, 0);
    K.anim_x0 = 0;
    K.anim_y0 = 0;
    K.anim_x1 = 1;
    K.anim_y1 = 1;
    K.palette_change_frame = 2;
    palette_for(0, old_palette);
    palette_for(2, new_palette);
    smk = open_smk(0);
    d = set_destination(smk, 16, 8, 0, 0, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    for (uint32_t frame = 1; frame <= 2; frame++) {
        call(SMKX_SmackNextFrame, smk);
        call(SMKX_SmackDoFrame, smk);
    }
    CHECK(get32(smk + SMK_OFF_NEW_PALETTE) == 1, "frame 2 brings a new palette");
    CHECK(get16(d.buffer + 1 * 2) == pack565(new_palette + pixel_at(2, 1, 0) * 3u),
          "a pixel in a block that changed is converted with the new palette, its unchanged neighbours with it");
    CHECK(get16(d.buffer + 8 * 2) == pack565(old_palette + pixel_at(2, 8, 0) * 3u) && get16(d.buffer + 8 * 2) != pack565(new_palette + pixel_at(2, 8, 0) * 3u),
          "a block that did not change keeps the colour it was written with: the original leaves stale blocks behind a palette change");
    CHECK(get16(d.buffer + 4 * d.pitch) == pack565(old_palette + pixel_at(2, 0, 4) * 3u), "the block below it too");
    call(SMKX_SmackClose, smk);
}

static void test_y_interlaced_and_doubled(void) {
    uint32_t smk;
    dest_t d;
    uint8_t palette[768];
    reset();
    make_header(16, 10, 2, 67, SMK_FLAG_Y_INTERLACED, 0);
    palette_for(0, palette);
    smk = open_smk(0);
    CHECK(get32(smk + SMK_OFF_HEIGHT) == 20 && get32(smk + SMK_OFF_FLAGS) == SMK_FLAG_Y_INTERLACED, "a Y-interlaced file is reported at twice its stored rows");
    d = set_destination(smk, 16, 24, 0, 2, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    {
        int even = 1, odd_black = 1;
        for (uint32_t r = 0; r < 10; r++) {
            even = even && get16(d.buffer + (2 + 2 * r) * d.pitch + 4 * 2) == pack565(palette + pixel_at(0, 4, r) * 3u);
            for (uint32_t x = 0; x < 16; x++) odd_black = odd_black && get16(d.buffer + (2 + 2 * r + 1) * d.pitch + x * 2) == 0;
        }
        CHECK(even, "stored row r lands on row 2r");
        CHECK(odd_black, "every second row stays untouched, as the original leaves it");
    }
    call(SMKX_SmackToBufferRect, smk, 1u);
    CHECK((int32_t)get32(smk + SMK_OFF_RECT_H) == 20, "the rectangle covers the shown rows");
    call(SMKX_SmackClose, smk);

    reset();
    make_header(16, 10, 2, 67, SMK_FLAG_Y_DOUBLED, 0);
    smk = open_smk(0);
    d = set_destination(smk, 16, 24, 0, 0, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    {
        int same = 1;
        for (uint32_t r = 0; r < 10; r++)
            for (uint32_t x = 0; x < 16; x++) same = same && get16(d.buffer + 2 * r * d.pitch + x * 2) == get16(d.buffer + (2 * r + 1) * d.pitch + x * 2) && get16(d.buffer + 2 * r * d.pitch + x * 2) != 0xCDCD;
        CHECK(same, "a Y-doubled file repeats every stored row");
    }
    call(SMKX_SmackClose, smk);
}

/* ---- the clock -------------------------------------------------------------------------------------------------- */

static void test_wait_without_audio(void) {
    uint32_t smk;
    reset();
    make_header(16, 8, 100, 66, 0, 0);
    smk = open_smk(SMK_OPEN_NEEDVOLUME | SMK_OPEN_TRACKS);
    CHECK(call(SMKX_SmackWait, 0u) == 0, "no video, nothing to wait for");
    CHECK(call(SMKX_SmackWait, smk) == 1, "the first call starts the clock and waits one frame time, as the original does");
    g_now += 60000;
    CHECK(call(SMKX_SmackWait, smk) == 1, "60 ms: not yet (the frame time is 66 ms)");
    g_now += 6000;
    CHECK(call(SMKX_SmackWait, smk) == 0, "66 ms: the next frame is due");
    CHECK(call(SMKX_SmackWait, smk) == 1, "and the one after it is not");
    g_now += 65000;
    CHECK(call(SMKX_SmackWait, smk) == 1, "131 ms: not yet");
    g_now += 1000;
    CHECK(call(SMKX_SmackWait, smk) == 0 && call(SMKX_SmackWait, smk) == 1, "132 ms: due once");
    g_now += 400000;
    CHECK(call(SMKX_SmackWait, smk) == 0, "after a long stall the next frame is due at once");
    CHECK(call(SMKX_SmackWait, smk) == 1, "but the lateness is dropped, not repaid with a burst of frames");
    g_now += 66000;
    CHECK(call(SMKX_SmackWait, smk) == 0, "and the schedule goes on from the stall");
    call(SMKX_SmackClose, smk);
}

/* ---- audio ------------------------------------------------------------------------------------------------------ */

static void test_audio_enters_the_mixer(void) {
    uint32_t smk;
    float out[64];
    reset();
    make_header(16, 8, 30, 67, 0, 1);
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(SMK_OPEN_NEEDVOLUME | SMK_OPEN_TRACKS);
    CHECK(smk != 0 && K.audio_opens == 1 && K.video_opens == 1 && audio_mixer_stream_count(g_mixer) == 1, "an audio decoder and one PCM stream in the game's mixer, no second output");
    pull(64);
    audio_mixer_mix(g_mixer, out, 8);
    CHECK(out[0] == 0.0f && K.audio_chunks[0] == 0, "nothing plays or is decoded before the clock starts");
    CHECK(call(SMKX_SmackWait, smk) == 1, "the first wait starts the clock");
    audio_mixer_mix(g_mixer, out, 8);
    CHECK(out[0] == 0.25f && out[1] == 0.25f, "after it the PCM plays through the mixer (0.25 of full scale)");
    CHECK(K.audio_chunks[0] >= 8 && K.audio_chunks[0] <= 9, "about half a second is queued ahead: %u chunks of 1477 frames", K.audio_chunks[0]);
    call(SMKX_SmackClose, smk);
    CHECK(audio_mixer_stream_count(g_mixer) == 0 && K.closes == 2, "closing the video closes its stream and both decoders");
}

static void test_audio_is_the_clock(void) {
    uint32_t smk, zeros = 0;
    reset();
    make_header(16, 8, 60, 66, 0, 1);
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(SMK_OPEN_TRACKS);
    CHECK(call(SMKX_SmackWait, smk) == 1, "armed");
    pull(1000);
    CHECK(call(SMKX_SmackWait, smk) == 1, "1000 frames of audio are 45 ms: the video waits although the wall clock stood still");
    pull(500);
    CHECK(call(SMKX_SmackWait, smk) == 0, "1500 frames are 68 ms: the frame is due, by the audio clock alone");
    CHECK(call(SMKX_SmackWait, smk) == 1, "and the next is not");
    pull(1455);
    CHECK(call(SMKX_SmackWait, smk) == 0, "one more frame time of audio, one more frame");
    g_now += 2000000;
    for (int i = 0; i < 40; i++) zeros += call(SMKX_SmackWait, smk) == 0;
    CHECK(zeros >= 20 && zeros < 40, "an audio device that stopped for two seconds does not freeze the picture: it follows the wall clock (%u due)", zeros);
    call(SMKX_SmackClose, smk);
}

static void test_audio_end_and_sound_switch(void) {
    uint32_t smk, due = 0;
    reset();
    make_header(16, 8, 3, 66, 0, 1);
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(SMK_OPEN_TRACKS);
    call(SMKX_SmackWait, smk);
    CHECK(K.audio_chunks[0] == 3, "all three frames of audio are decoded and queued at once");
    pull(2000);
    for (int i = 0; i < 6; i++) due += call(SMKX_SmackWait, smk) == 0;
    pull(10000);
    for (int i = 0; i < 200; i++) {
        g_now += 10000;
        due += call(SMKX_SmackWait, smk) == 0;
    }
    CHECK(due >= 10, "when the audio ends the picture goes on by the wall clock: no wait for a clock that stopped (%u frames due)", due);
    call(SMKX_SmackClose, smk);

    reset();
    make_header(16, 8, 30, 66, 0, 1);
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(SMK_OPEN_TRACKS);
    call(SMKX_SmackWait, smk);
    pull(100);
    call(SMKX_SmackSoundOnOff, smk, 0u);
    {
        float out[16];
        audio_mixer_mix(g_mixer, out, 4);
        CHECK(out[0] == 0.0f, "SmackSoundOnOff(0) silences the video at once and drops what was queued");
    }
    {
        uint32_t chunks = K.audio_chunks[0];
        call(SMKX_SmackWait, smk);
        call(SMKX_SmackDoFrame, smk);
        CHECK(K.audio_chunks[0] == chunks, "and no more audio is decoded while it is off");
    }
    call(SMKX_SmackSoundOnOff, smk, 1u);
    call(SMKX_SmackWait, smk);
    {
        float out[16];
        audio_mixer_mix(g_mixer, out, 4);
        CHECK(out[0] == 0.25f, "SmackSoundOnOff(1) brings the sound back");
    }
    call(SMKX_SmackClose, smk);
}

static void test_audio_selection(void) {
    uint32_t smk;
    reset();
    make_header(16, 8, 5, 66, 0, 1);
    smk = open_smk(SMK_OPEN_TRACKS);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 0 && K.audio_opens == 0, "without SmackSoundUseDirectSound the video has no sound and no audio decoder");
    call(SMKX_SmackClose, smk);
    call(SMKX_SmackSoundUseDirectSound, 0xBADBAD);
    smk = open_smk(SMK_OPEN_TRACKS);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 0, "a DirectSound pointer that is not the game's gives no sound");
    call(SMKX_SmackClose, smk);
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(0);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 0, "a video opened without track flags has no sound");
    call(SMKX_SmackClose, smk);
    smk = open_smk(0x2000u);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 1, "track 1's own flag opens track 0 of the file");
    call(SMKX_SmackClose, smk);
    smk = open_smk(0x4000u);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 0, "a track the file does not have opens nothing");
    call(SMKX_SmackClose, smk);
    {
        audio_mixer_t* none = g_mixer;
        const int before = K.audio_opens;
        g_mixer = NULL;
        smk = open_smk(SMK_OPEN_TRACKS);
        CHECK(smk != 0 && K.audio_opens == before, "no output device open: the video plays silently");
        call(SMKX_SmackClose, smk);
        g_mixer = none;
    }
    make_header(16, 8, 5, 66, 0, 0);
    smk = open_smk(SMK_OPEN_TRACKS);
    CHECK(smk != 0 && audio_mixer_stream_count(g_mixer) == 0, "a file without audio");
    call(SMKX_SmackClose, smk);
}

static void test_the_player_loop(void) {
    /* CVideoWnd::Play: wait, decode a frame, copy its rectangles, step, until the last frame; the host's own message
     * pump is the 4 ms tick, with the output pulling the same time of audio. */
    const uint32_t frames = 45, duration_ms = 66;
    uint32_t smk, shown = 0, rects = 0, iterations = 0, max_skew = 0, last_frame = 0, mixed_frames = 0;
    double carry = 0;
    int started = 0;
    dest_t d;
    reset();
    make_header(64, 40, frames, (int32_t)duration_ms, 0, 1);
    K.anim_x0 = 0;
    K.anim_y0 = 0;
    K.anim_x1 = 64;
    K.anim_y1 = 4;
    call(SMKX_SmackSoundUseDirectSound, FAKE_DEVICE);
    smk = open_smk(SMK_OPEN_NEEDVOLUME | SMK_OPEN_TRACKS);
    d = set_destination(smk, 100, 60, 18, 10, SMK_BUFFER_565);
    (void)d;
    call(SMKX_SmackSoundOnOff, smk, 1u);
    while (iterations++ < 20000) {
        double frames_float = 0.004 * RATE + carry;
        uint32_t whole = (uint32_t)frames_float;
        carry = frames_float - whole;
        if (started) {
            pull(whole);
            mixed_frames += whole;
        }
        g_now += 4000;
        if (call(SMKX_SmackWait, smk) != 0) {
            started = 1;
            continue;
        }
        {
            uint64_t media_us = (uint64_t)mixed_frames * 1000000u / RATE;
            uint64_t expected = (uint64_t)(shown + 1) * duration_ms * 1000u;
            uint64_t skew = media_us > expected ? media_us - expected : expected - media_us;
            if (skew > max_skew) max_skew = (uint32_t)skew;
        }
        call(SMKX_SmackDoFrame, smk);
        while (call(SMKX_SmackToBufferRect, smk, 1u)) rects++;
        shown++;
        last_frame = get32(smk + SMK_OFF_FRAME_NUMBER);
        if (last_frame == get32(smk + SMK_OFF_FRAMES) - 1) break;
        call(SMKX_SmackNextFrame, smk);
    }
    CHECK(iterations < 20000, "the loop reaches the last frame: no deadlock (%u iterations)", iterations);
    CHECK(shown == frames && K.video_frames == frames && last_frame == frames - 1, "every frame is shown exactly once, in order (%u of %u)", shown, frames);
    CHECK(rects == frames, "one changed rectangle per frame");
    CHECK(max_skew <= 2u * duration_ms * 1000u, "the picture never runs ahead of or behind its audio by more than two frames (%u us)", max_skew);
    {
        double seconds = (double)(g_now - 1000000u) / 1e6;
        CHECK(fabs(seconds - (double)frames * duration_ms / 1000.0) < 0.2, "the whole video takes its own length: %.2f s for %.2f s", seconds, (double)frames * duration_ms / 1000.0);
    }
    CHECK(K.audio_chunks[0] <= frames, "no audio frame is decoded twice (%u of %u)", K.audio_chunks[0], frames);
    call(SMKX_SmackClose, smk);
    CHECK(audio_mixer_stream_count(g_mixer) == 0 && smacker_open_count() == 0, "closing leaves no stream and no video");
}

static void test_buffer_api_is_unavailable(void) {
    uint32_t smk;
    reset();
    make_header(16, 8, 5, 66, 0, 0);
    smk = open_smk(0);
    CHECK(call(SMKX_SmackBufferOpen, 0x1000u, 0u, 16u, 8u, 16u, 8u) == 0, "SmackBufferOpen fails: the player only uses it when the primary surface has no memory");
    CHECK(call(SMKX_SmackBufferClose, 0u) == 0 && call(SMKX_SmackBufferNewPalette, 0u, 0u, 0u) == 0 &&
              call(SMKX_SmackBufferBlit, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u) == 0 && call(SMKX_SmackColorRemapWithTrans, smk, 0u, 0u, 0u, 0u) == 0,
          "the other buffer functions do nothing and return 0");
    call(SMKX_SmackClose, smk);
}

static void test_robustness(void) {
    uint32_t smk;
    reset();
    make_header(16, 8, 3, 66, 0, 0);
    smk = open_smk(0);
    CHECK(call(SMKX_SmackDoFrame, 0u) == 0 && call(SMKX_SmackNextFrame, 0u) == 0 && call(SMKX_SmackWait, 0xDEADu) == 0 && call(SMKX_SmackToBufferRect, 0u, 1u) == 0 &&
              call(SMKX_SmackToBuffer, 0u, 0u, 0u, 0u, 0u, 0u, 0u) == 0 && call(SMKX_SmackSoundOnOff, 0u, 1u) == 0,
          "calls on a missing video are harmless");
    call(SMKX_SmackDoFrame, smk);
    call(SMKX_SmackToBuffer, smk, 0u, 0u, 100000000u, 100000000u, GUEST_BASE, SMK_BUFFER_565);
    call(SMKX_SmackDoFrame, smk);
    CHECK(get32(smk + SMK_OFF_FRAME_NUMBER) == 0, "a destination larger than guest memory is ignored");
    call(SMKX_SmackClose, smk);
    reset();
    make_header(16, 8, 3, 66, 0, 0);
    smk = open_smk(0);
    {
        smk_host_t h = make_host();
        h.guest.resolve = NULL;
        CHECK(!smacker_hle_install(&h), "a host without guest memory is refused");
        h = make_host();
        h.backend = NULL;
        CHECK(!smacker_hle_install(&h), "a host without a decoder is refused");
        CHECK(!smacker_hle_install(NULL), "no host");
    }
    CHECK(smacker_open_count() == 1, "a refused install leaves the running one untouched");
    call(SMKX_SmackClose, smk);
}

static uint32_t layer_thunk(void* context, dsound_method_t method) {
    (void)context;
    return 0x7F000000u + (uint32_t)method * 16u;
}

static audio_mixer_t* layer_open(uint32_t rate) { return audio_mixer_create(rate, NULL); }
static void layer_set_rate(uint32_t rate) { audio_mixer_set_rate(dsound_compat_mixer(), rate); }
static void layer_close(void) { audio_mixer_destroy(dsound_compat_mixer()); }
static const char* layer_name(void) { return "test"; }

static void test_with_the_directsound_layer(void) {
    /* The same HLE over the real DirectSound layer's mixer: the game's IDirectSound is the device that switches the
     * sound on, and the layer's shutdown comes after the HLE's. */
    static const audio_backend_t backend = { layer_open, layer_set_rate, layer_close, layer_name };
    dsound_binding_t binding;
    uint32_t out, device, smk;
    float samples[8];
    reset();
    g_use_dsound_glue = 1;
    {
        smk_host_t h = make_host();
        smacker_hle_install(&h);
    }
    memset(&binding, 0, sizeof binding);
    binding.guest.resolve = arena_resolve;
    binding.guest.alloc = arena_alloc;
    binding.guest.release = arena_release;
    binding.method_address = layer_thunk;
    CHECK(dsound_compat_install(&backend, &binding) == 1, "the DirectSound layer installs over the same guest memory");
    CHECK(smk_dsound_mixer_for_device(NULL, 0) == NULL && smk_dsound_mixer_for_device(NULL, 0x1234) == NULL, "without a DirectSound device the glue offers no mixer");
    out = arena_alloc(NULL, 16);
    CHECK(dsound_compat_call(DSM_Create, (const uint32_t[]){ 0u, out, 0u }) == DS_OK, "the game creates its DirectSound device");
    device = get32(out);
    CHECK(device != 0 && smk_dsound_mixer_for_device(NULL, device) == dsound_compat_mixer() && dsound_compat_mixer() != NULL,
          "that device leads to the layer's own mixer: the video audio has no output of its own");
    CHECK(smk_dsound_mixer_for_device(NULL, GUEST_BASE) == NULL, "another guest address is no device");
    make_header(16, 8, 30, 66, 0, 1);
    call(SMKX_SmackSoundUseDirectSound, device);
    smk = open_smk(SMK_OPEN_TRACKS);
    CHECK(smk != 0 && audio_mixer_stream_count(dsound_compat_mixer()) == 1, "the video's PCM stream lives in the game's mixer");
    call(SMKX_SmackWait, smk);
    audio_mixer_mix(dsound_compat_mixer(), samples, 4);
    CHECK(samples[0] == 0.25f, "and plays through it");
    call(SMKX_SmackClose, smk);
    CHECK(audio_mixer_stream_count(dsound_compat_mixer()) == 0, "closing the video closes the stream");
    smk = open_smk(SMK_OPEN_TRACKS);
    smacker_hle_shutdown();
    dsound_compat_shutdown();
    CHECK(smacker_open_count() == 0 && dsound_compat_mixer() == NULL, "at exit the HLE closes first, then the layer's output, with a video still open");
    g_use_dsound_glue = 0;
}

int main(void) {
    G.memory = (uint8_t*)calloc(1, ARENA_BYTES);
    if (!G.memory) return 2;
    test_exports();
    test_binding_policy();
    test_header();
    test_open_close();
    test_frame_progression_and_pixels();
    test_destination_formats_and_clipping();
    test_changed_rectangles();
    test_blocks_keep_their_colours_after_a_palette_change();
    test_y_interlaced_and_doubled();
    test_wait_without_audio();
    test_audio_enters_the_mixer();
    test_audio_is_the_clock();
    test_audio_end_and_sound_switch();
    test_audio_selection();
    test_the_player_loop();
    test_buffer_api_is_unavailable();
    test_robustness();
    test_with_the_directsound_layer();
    smacker_hle_shutdown();
    audio_mixer_destroy(g_mixer);
    printf("smacker_tests: %d checks, %d failures\n", g_checks, g_failures);
    free(G.memory);
    return g_failures ? 1 : 0;
}
