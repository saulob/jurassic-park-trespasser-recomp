/*
 * Headless checks of the restored audio and the corrected English captions: the embedded records and their manifest, the
 * virtual Stream.tpa built over a retail file, the captions' timings in every language, and the lint that keeps any
 * development path out of the program. Checks that need the player's retail Stream.tpa skip themselves without it.
 * Build: CMake target stream_tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "jpt_hash.h"
#include "jpt_l10n.h"
#include "jpt_restored_audio.h"
#include "l10n_assets.h"
#include "stream_pack.h"

static int s_failures, s_checks, s_skipped;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ---- a retail TPA made up in memory */

typedef struct {
    uint8_t* data;
    size_t size;
} blob_t;

static int read_blob(void* ctx, uint64_t offset, void* buf, size_t n) {
    const blob_t* b = (const blob_t*)ctx;
    if (offset + n > b->size) return 0;
    memcpy(buf, b->data + offset, n);
    return 1;
}

static size_t build_record(uint8_t* out, uint32_t id_hint, const uint32_t* units, const char* const* lines, int line_count, const uint8_t* payload,
                           size_t payload_size, uint32_t format) {
    uint8_t block[256];
    size_t block_size = line_count ? jpt_caption_build(line_count, units, lines, block, sizeof block) : 0, header = 0x24 + block_size;
    (void)id_hint;
    memset(out, 0, header);
    wr32(out, 0x524F4257);
    wr32(out + 4, 110);
    wr32(out + 8, (uint32_t)header);
    wr32(out + 16, (uint32_t)payload_size);
    wr32(out + 20, (uint32_t)payload_size);
    wr32(out + 24, 22050);
    wr32(out + 28, format);
    if (block_size) wr32(out + 32, 0x24);
    if (block_size) memcpy(out + 0x24, block, block_size);
    memcpy(out + header, payload, payload_size);
    return header + payload_size;
}

static blob_t make_retail(const uint32_t* ids, int count, const int* with_caption) {
    static const uint32_t units[2] = {5, 9};
    static const char* const lines[2] = {"One", "Two"};
    uint8_t* file = (uint8_t*)calloc(1, 4096);
    size_t at = 0x18 + (size_t)count * 0x38;
    blob_t b;
    wr32(file, 0x150);
    wr32(file + 4, (uint32_t)count);
    wr32(file + 8, 0x18);
    wr32(file + 0x14, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        uint8_t payload[40], record[400], *entry = file + 0x18 + (size_t)i * 0x38;
        size_t size;
        for (int k = 0; k < 40; k++) payload[k] = (uint8_t)(i * 31 + k);
        size = build_record(record, ids[i], units, lines, with_caption[i] ? 2 : 0, payload, sizeof payload, 0x110);
        wr32(entry, ids[i]);
        wr32(entry + 4, (uint32_t)at);
        wr32(entry + 8, (uint32_t)size);
        memcpy(entry + 0x14, record, 0x24);
        memcpy(file + at, record, size);
        at += size;
    }
    b.data = file;
    b.size = at;
    return b;
}

static void block_of(const char* const* lines, const uint32_t* units, int count, uint8_t* out, size_t* size) { *size = jpt_caption_build(count, units, lines, out, 256); }

static void test_synthetic_pack(void) {
    static const uint32_t ids[3] = {0x1111, 0x2222, 0x3333};
    static const int with_caption[3] = {1, 0, 1};
    blob_t retail = make_retail(ids, 3, with_caption);
    static const uint32_t new_units[1] = {7};
    static const char* const new_lines[1] = {"Fixed"};
    uint8_t new_block[256], same_block[256], added_payload[16], added_record[300], replaced_payload[24], replaced_record[300];
    size_t new_size, same_size, added_size, replaced_size;
    uint8_t original_payload_1[40], dir[0x18 + 5 * 0x38], buf[64];
    char sha[65], wrong[65];
    stream_audio_t audio[3];
    stream_caption_t captions[3];
    stream_pack_stats_t stats;
    stream_pack_t* pack;
    static const uint32_t same_units[2] = {5, 9};
    static const char* const same_lines[2] = {"One", "Two"};

    block_of(new_lines, new_units, 1, new_block, &new_size);
    block_of(same_lines, same_units, 2, same_block, &same_size);
    for (int i = 0; i < 16; i++) added_payload[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 24; i++) replaced_payload[i] = (uint8_t)(0x50 + i);
    added_size = build_record(added_record, 0x4444, new_units, new_lines, 1, added_payload, sizeof added_payload, 0x110);
    replaced_size = build_record(replaced_record, 0x2222, NULL, NULL, 0, replaced_payload, sizeof replaced_payload, 0x110);
    memcpy(original_payload_1, retail.data + rd32(retail.data + 0x18 + 0x38 + 4) + 0x24, 40);
    jpt_sha256_buffer_hex(original_payload_1, 40, sha);
    memset(wrong, '0', 64);
    wrong[64] = 0;

    audio[0] = (stream_audio_t){0x4444, added_record, added_size, "-"};
    audio[1] = (stream_audio_t){0x2222, replaced_record, replaced_size, sha};
    audio[2] = (stream_audio_t){0x3333, replaced_record, replaced_size, wrong};
    captions[0] = (stream_caption_t){0x1111, new_block, new_size};
    captions[1] = (stream_caption_t){0x3333, same_block, same_size};
    captions[2] = (stream_caption_t){0x9999, new_block, new_size};
    pack = stream_pack_open(read_blob, &retail, retail.size, audio, 3, captions, 3, &stats);
    CHECK(pack != NULL);
    if (!pack) return;
    CHECK(stats.audio_added == 1 && stats.audio_replaced == 1 && stats.audio_kept == 1);
    CHECK(stats.captions_applied == 1);

    CHECK(stream_pack_read(pack, 0, dir, sizeof dir) == (long)sizeof dir);
    CHECK(rd32(dir) == 0x150 && rd32(dir + 4) == 4 && rd32(dir + 8) == 0x18 && rd32(dir + 0x14) == 4);
    for (int i = 0; i < 4; i++) CHECK(rd32(dir + 0x18 + (size_t)i * 0x38) == (i < 3 ? ids[i] : 0x4444));

    {
        const uint8_t* e = dir + 0x18 + 2 * 0x38; /* 0x3333: its audio is not the one the record replaces: kept, caption already current */
        uint32_t offset = rd32(e + 4), size = rd32(e + 8), header = rd32(e + 0x1C);
        uint8_t record[400];
        CHECK(stream_pack_read(pack, offset, record, size) == (long)size);
        CHECK(header == 0x24 + same_size && !memcmp(record + 0x24, same_block, same_size));
        CHECK(rd32(record + 8) == header);
    }
    {
        const uint8_t* e = dir + 0x18; /* 0x1111: caption corrected, payload still the retail bytes */
        uint32_t offset = rd32(e + 4), size = rd32(e + 8), header = rd32(e + 0x1C);
        uint8_t record[400];
        CHECK(stream_pack_read(pack, offset, record, size) == (long)size);
        CHECK(header == 0x24 + new_size && !memcmp(record + 0x24, new_block, new_size) && rd32(record + 8) == header);
        CHECK(size == header + 40);
        {
            uint32_t retail_offset = rd32(retail.data + 0x18 + 4), retail_header = rd32(retail.data + 0x18 + 0x1C);
            CHECK(!memcmp(record + header, retail.data + retail_offset + retail_header, 40));
        }
    }
    {
        const uint8_t* e = dir + 0x18 + 0x38; /* 0x2222: replaced by the restored record */
        uint32_t offset = rd32(e + 4), size = rd32(e + 8);
        uint8_t record[400];
        CHECK(size == replaced_size && stream_pack_read(pack, offset, record, size) == (long)size && !memcmp(record, replaced_record, size));
    }
    {
        const uint8_t* e = dir + 0x18 + 3 * 0x38; /* 0x4444: appended */
        uint32_t offset = rd32(e + 4), size = rd32(e + 8);
        uint8_t record[400];
        CHECK(size == added_size && stream_pack_read(pack, offset, record, size) == (long)size && !memcmp(record, added_record, size));
        CHECK(stream_pack_read(pack, (uint64_t)offset + size, buf, sizeof buf) == 0 || stream_pack_read(pack, (uint64_t)offset + size, buf, sizeof buf) > 0);
    }
    {
        uint64_t size = stream_pack_size(pack);
        CHECK(size > retail.size);
        CHECK(stream_pack_read(pack, size, buf, 8) == 0);
        CHECK(stream_pack_read(pack, size - 4, buf, 64) == 4);
    }
    stream_pack_close(pack);

    pack = stream_pack_open(read_blob, &retail, retail.size, NULL, 0, NULL, 0, &stats);
    CHECK(pack != NULL && stats.captions_applied == 0 && stream_pack_size(pack) == retail.size);
    if (pack) {
        uint8_t* whole = (uint8_t*)malloc(retail.size);
        CHECK(stream_pack_read(pack, 0, whole, retail.size) == (long)retail.size && !memcmp(whole, retail.data, retail.size));
        free(whole);
        stream_pack_close(pack);
    }
    {
        blob_t not_tpa = {(uint8_t*)"JUNKJUNKJUNKJUNKJUNKJUNKJUNK", 28};
        CHECK(stream_pack_open(read_blob, &not_tpa, not_tpa.size, NULL, 0, NULL, 0, &stats) == NULL);
    }
    free(retail.data);
}

/* ---- the game's handles on Stream.tpa */

#define POOL_HANDLES 8u

/* What ReadFile gives the game on a handle the view does not serve: the real file's bytes, short at its end; the audio
 * thread pads a short read with silence. */
static void read_real(const blob_t* file, uint64_t offset, uint8_t* out, size_t n) {
    size_t got = offset < file->size ? (size_t)(file->size - offset) : 0;
    if (got > n) got = n;
    memset(out, 0, n);
    if (got) memcpy(out, file->data + offset, got);
}

/* The CAudio pack loader opens Stream.tpa read-only for the directory (0x0043A9C1), then seven more times read-write for
 * its stream handle pool (0x0043AF91); a voice-over playing while music holds the first handle streams through the next. */
static void test_game_handle_pool(void) {
    static const uint32_t ids[2] = {0x1111, 0x2222};
    static const int with_caption[2] = {1, 0};
    static const uint32_t units[1] = {7};
    static const char* const lines[1] = {"Fixed"};
    blob_t retail = make_retail(ids, 2, with_caption);
    uint8_t block[256], added_payload[16], added_record[300], dir[0x18 + 3 * 0x38];
    size_t block_size, added_size;
    stream_audio_t audio[1];
    stream_caption_t caption;
    stream_pack_t* views[2 * POOL_HANDLES];
    stream_pack_stats_t stats;
    const char* path = "S:\\game\\versions\\en-116\\data\\Stream.tpa";

    block_of(lines, units, 1, block, &block_size);
    for (int i = 0; i < 16; i++) added_payload[i] = (uint8_t)(0xA0 + i);
    added_size = build_record(added_record, 0x4444, units, lines, 1, added_payload, sizeof added_payload, 0x110);
    audio[0] = (stream_audio_t){0x4444, added_record, added_size, "-"};
    caption = (stream_caption_t){0x1111, block, block_size};

    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) {
        const uint32_t access = k % POOL_HANDLES == 0 ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
        views[k] = stream_pack_serves_open(path, access, FILE_FLAG_SEQUENTIAL_SCAN)
                       ? stream_pack_open(read_blob, &retail, retail.size, audio, 1, &caption, 1, &stats)
                       : NULL;
    }
    CHECK(2 * POOL_HANDLES <= STREAM_PACK_MAX_VIEWS);
    CHECK(views[0] != NULL && stats.captions_applied == 1 && stats.audio_added == 1);
    if (!views[0]) {
        free(retail.data);
        return;
    }
    CHECK(stream_pack_read(views[0], 0, dir, sizeof dir) == (long)sizeof dir);
    for (uint32_t s = 0; s < 2; s++) {
        const uint8_t* e = dir + 0x18 + (size_t)s * 0x38;
        const uint32_t offset = rd32(e + 4), header = rd32(e + 0x1C);
        const uint8_t* expected = retail.data + rd32(retail.data + 0x18 + (size_t)s * 0x38 + 4) + rd32(retail.data + 0x18 + (size_t)s * 0x38 + 0x1C);
        for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) {
            uint8_t payload[40];
            if (views[k]) CHECK(stream_pack_read(views[k], (uint64_t)offset + header, payload, sizeof payload) == (long)sizeof payload);
            else read_real(&retail, (uint64_t)offset + header, payload, sizeof payload);
            if (memcmp(payload, expected, sizeof payload)) {
                s_failures++;
                printf("FAIL %s:%d: sample %04X streamed through pool handle %u is not its audio (%s)\n", __FILE__, __LINE__, ids[s], k,
                       views[k] ? "virtual view" : "real file at the view's offset");
            }
            s_checks++;
        }
    }
    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) stream_pack_close(views[k]);

    CHECK(stream_pack_serves_open("data/STREAM.TPA", GENERIC_READ, 0));
    CHECK(stream_pack_serves_open("Stream.tpa", GENERIC_READ | GENERIC_WRITE, FILE_FLAG_RANDOM_ACCESS));
    CHECK(!stream_pack_serves_open(path, GENERIC_READ, FILE_FLAG_OVERLAPPED));
    CHECK(!stream_pack_serves_open(path, GENERIC_WRITE, 0));
    CHECK(!stream_pack_serves_open("S:\\data\\Effects.tpa", GENERIC_READ, 0));
    CHECK(!stream_pack_serves_open("S:\\data\\Stream.tpa.bak", GENERIC_READ, 0));
    CHECK(!stream_pack_serves_open(NULL, GENERIC_READ, 0));
    free(retail.data);
}

/* ---- the embedded restored audio */

static const uint8_t* s_manifest;
static size_t s_manifest_size;
static jpt_restored_entry_t s_entries[JPT_RESTORED_MAX];
static int s_entry_count;

typedef struct {
    uint32_t id;
    int lines;
    uint32_t units[JPT_CAPTION_MAX_LINES];
    char text[JPT_CAPTION_MAX_LINES][256];
    int found;
} english_t;

static void find_english(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    english_t* e = (english_t*)ctx;
    if (strcmp(pack, "stream") || sample != e->id) return;
    e->found = 1;
    e->lines = count;
    for (int i = 0; i < count; i++) {
        e->units[i] = units[i];
        snprintf(e->text[i], sizeof e->text[i], "%s", lines[i]);
    }
}

typedef struct {
    int lines;
    uint32_t units[JPT_CAPTION_MAX_LINES];
    char text[JPT_CAPTION_MAX_LINES][256];
} record_caption_t;

static void record_line(void* ctx, uint32_t units, const char* text, size_t n) {
    record_caption_t* c = (record_caption_t*)ctx;
    if (c->lines == JPT_CAPTION_MAX_LINES) return;
    c->units[c->lines] = units;
    jpt_cp_to_utf8(1252, text, n, c->text[c->lines], sizeof c->text[0]);
    c->lines++;
}

/* A sample the release shipped without captions (the Spanish and Brazilian files have none): the descriptor must say a
 * block follows it, as every captioned retail sample does, or the game does not read the block. */
static void test_caption_into_a_sample_without_one(void) {
    static const uint32_t ids[2] = {0x1111, 0x2222};
    static const int none[2] = {0, 0};
    static const uint32_t units[1] = {7};
    static const char* const lines[1] = {"Fixed"};
    blob_t retail = make_retail(ids, 2, none);
    uint8_t block[256], dir[0x18 + 2 * 0x38], record[400];
    size_t size;
    stream_caption_t caption;
    stream_pack_stats_t stats;
    stream_pack_t* pack;
    block_of(lines, units, 1, block, &size);
    caption = (stream_caption_t){0x2222, block, size};
    CHECK(rd32(retail.data + 0x18 + 0x38 + 0x34) == 0 && rd32(retail.data + 0x18 + 0x38 + 0x1C) == 0x24);
    pack = stream_pack_open(read_blob, &retail, retail.size, NULL, 0, &caption, 1, &stats);
    CHECK(pack != NULL && stats.captions_applied == 1);
    if (pack) {
        const uint8_t* entry = dir + 0x18 + 0x38;
        uint32_t offset, total;
        CHECK(stream_pack_read(pack, 0, dir, sizeof dir) == (long)sizeof dir);
        offset = rd32(entry + 4);
        total = rd32(entry + 8);
        CHECK(rd32(entry + 0x1C) == 0x24 + size && rd32(entry + 0x34) == 0x24);
        CHECK(stream_pack_read(pack, offset, record, total) == (long)total);
        CHECK(rd32(record + 8) == 0x24 + size && rd32(record + 0x20) == 0x24 && !memcmp(record + 0x24, block, size));
        CHECK(!memcmp(record, entry + 0x14, 0x24));
        CHECK(rd32(dir + 0x18 + 0x34) == 0 && rd32(dir + 0x18 + 0x1C) == 0x24);
        stream_pack_close(pack);
    }
    free(retail.data);
}

static void test_embedded_audio(void) {
    const char* english;
    size_t english_size;
    CHECK(restored_audio_asset(JPT_RESTORED_MANIFEST, &s_manifest, &s_manifest_size));
    if (!s_manifest) return;
    s_entry_count = jpt_restored_parse((const char*)s_manifest, s_manifest_size, s_entries, JPT_RESTORED_MAX);
    CHECK(s_entry_count == 5);
    CHECK(l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size));
    for (int i = 0; i < s_entry_count; i++) {
        const jpt_restored_entry_t* m = &s_entries[i];
        const uint8_t* record;
        size_t record_size;
        char hex[65];
        uint32_t header;
        english_t en;
        record_caption_t rc;
        CHECK(jpt_sample_id(m->name) == m->id);
        CHECK(restored_audio_asset(m->file, &record, &record_size));
        if (!restored_audio_asset(m->file, &record, &record_size)) continue;
        CHECK(record_size == m->record_size);
        CHECK(record_size > 0x24 && rd32(record) == 0x524F4257 && rd32(record + 4) == 110);
        header = rd32(record + 8);
        CHECK(header >= 0x24 && header < record_size && record_size - header == m->payload_size);
        jpt_sha256_buffer_hex(record + header, record_size - header, hex);
        CHECK(!strcmp(hex, m->sha256));
        CHECK(rd32(record + 16) == m->payload_size && rd32(record + 20) == m->payload_size);
        CHECK(rd32(record + 24) == m->rate && rd32(record + 28) == m->format);
        CHECK(!strcmp(m->codec, "pcm") && m->bits == 16 && (m->format & 0x10000) == 0);
        CHECK(m->channels == ((m->format & 0x200) ? 2 : 1));
        CHECK(m->duration_ms == (uint32_t)((double)m->payload_size * 1000.0 / ((double)m->rate * 2.0 * m->channels) + 0.5));
        CHECK(m->replaces[0] == '-' ? m->replaces[1] == 0 : strlen(m->replaces) == 64);

        memset(&rc, 0, sizeof rc);
        CHECK(header > 0x24 && jpt_caption_lines(record + 0x24, rd32(record + 0x24), (jpt_caption_line_fn)NULL, NULL) > 0);
        jpt_caption_lines(record + 0x24, rd32(record + 0x24), record_line, &rc);
        memset(&en, 0, sizeof en);
        en.id = m->id;
        jpt_l10n_read_subtitles(english, english_size, find_english, &en);
        CHECK(en.found && en.lines == rc.lines);
        for (int k = 0; k < rc.lines && en.found && k < en.lines; k++) CHECK(en.units[k] == rc.units[k] && !strcmp(en.text[k], rc.text[k]));
    }
    CHECK(!strstr((const char*)s_manifest, ":\\") && !strstr((const char*)s_manifest, "Decomp"));
}

/* ---- the player's retail file */

static int read_file(void* ctx, uint64_t offset, void* buf, size_t n) {
    LARGE_INTEGER at;
    DWORD got = 0;
    at.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx((HANDLE)ctx, at, NULL, FILE_BEGIN) && ReadFile((HANDLE)ctx, buf, (DWORD)n, &got, NULL) && got == n;
}

/* The reader the host gives the virtual file: an offset in an OVERLAPPED on a synchronous handle. */
static int read_at_offset(void* ctx, uint64_t offset, void* buf, size_t n) {
    OVERLAPPED at;
    DWORD got = 0;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    return ReadFile((HANDLE)ctx, buf, (DWORD)n, &got, &at) && got == n;
}

static int read_view(void* ctx, uint64_t offset, void* buf, size_t n) { return stream_pack_read((const stream_pack_t*)ctx, offset, buf, n) == (long)n; }

typedef struct {
    int count;
    uint32_t id[512];
    uint8_t* block[512];
    size_t size[512];
} expected_t;

static void expect_caption(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    expected_t* e = (expected_t*)ctx;
    char converted[JPT_CAPTION_MAX_LINES][512];
    const char* pointers[JPT_CAPTION_MAX_LINES];
    uint8_t block[0x2000];
    if (strcmp(pack, "stream") || e->count == 512 || count > JPT_CAPTION_MAX_LINES) return;
    for (int i = 0; i < count; i++) {
        CHECK(jpt_utf8_to_cp(1252, lines[i], converted[i], sizeof converted[i]) != (size_t)-1);
        pointers[i] = converted[i];
    }
    e->size[e->count] = jpt_caption_build(count, units, pointers, block, sizeof block);
    e->block[e->count] = (uint8_t*)malloc(e->size[e->count]);
    memcpy(e->block[e->count], block, e->size[e->count]);
    e->id[e->count++] = sample;
}

typedef struct {
    const expected_t* expected;
    int seen, differing;
} played_t;

static void check_played(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    played_t* p = (played_t*)ctx;
    for (int i = 0; i < p->expected->count; i++) {
        if (p->expected->id[i] != sample) continue;
        p->seen++;
        p->differing += p->expected->size[i] != n || memcmp(p->expected->block[i], block, n);
    }
}

static int has_sample(const uint8_t* dir, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++)
        if (rd32(dir + (size_t)i * 0x38) == id) return 1;
    return 0;
}

static void test_retail_view(void) {
    char path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    stream_audio_t audio[JPT_RESTORED_MAX];
    static expected_t expected;
    stream_caption_t captions[512];
    stream_pack_stats_t stats;
    stream_pack_t* pack;
    uint8_t head[0x18], *dir, *view_dir;
    uint32_t count;
    int audio_n = 0;
    const char* english;
    size_t english_size;
    snprintf(path, sizeof path, "%s/game/data/Stream.tpa", JPT_SOURCE_DIR);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
        printf("stream_tests: no retail Stream.tpa at %s: the retail checks are skipped\n", path);
        s_skipped++;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return;
    }
    for (int i = 0; i < s_entry_count; i++) {
        const uint8_t* record;
        size_t record_size;
        if (restored_audio_asset(s_entries[i].file, &record, &record_size))
            audio[audio_n++] = (stream_audio_t){s_entries[i].id, record, record_size, s_entries[i].replaces};
    }
    CHECK(l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size));
    jpt_l10n_read_subtitles(english, english_size, expect_caption, &expected);
    for (int i = 0; i < expected.count; i++) captions[i] = (stream_caption_t){expected.id[i], expected.block[i], expected.size[i]};
    pack = stream_pack_open(read_file, file, (uint64_t)size.QuadPart, audio, audio_n, captions, expected.count, &stats);
    CHECK(pack != NULL);
    if (!pack) {
        CloseHandle(file);
        return;
    }
    CHECK(stats.audio_added == 4 && stats.audio_replaced == 1 && stats.audio_kept == 0);
    {
        uint8_t first[4096], second[4096];
        uint64_t middle = (uint64_t)size.QuadPart / 2;
        CHECK(read_file(file, middle, first, sizeof first) && read_at_offset(file, middle, second, sizeof second) && !memcmp(first, second, sizeof first));
        CHECK(read_file(file, 0, first, sizeof first) && read_at_offset(file, 0, second, sizeof second) && !memcmp(first, second, sizeof first));
    }
    CHECK(read_file(file, 0, head, sizeof head) && rd32(head) == 0x150);
    count = rd32(head + 4);
    dir = (uint8_t*)malloc((size_t)count * 0x38);
    view_dir = (uint8_t*)malloc((size_t)(count + 8) * 0x38);
    CHECK(dir && view_dir && read_file(file, 0x18, dir, (size_t)count * 0x38));
    if (dir && view_dir) {
        uint32_t view_count;
        uint8_t view_head[0x18];
        CHECK(stream_pack_read(pack, 0, view_head, 0x18) == 0x18);
        view_count = rd32(view_head + 4);
        CHECK(view_count == count + (uint32_t)stats.audio_added && rd32(view_head + 0x14) == view_count);
        CHECK(stream_pack_read(pack, 0x18, view_dir, (size_t)view_count * 0x38) == (long)((size_t)view_count * 0x38));
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *r = dir + (size_t)i * 0x38, *v = view_dir + (size_t)i * 0x38;
            int restored = 0;
            for (int k = 0; k < audio_n; k++) restored |= audio[k].id == rd32(r);
            CHECK(rd32(r) == rd32(v));
            if (!restored) {
                uint32_t size_r = rd32(r + 8), offset_r = rd32(r + 4), header_r = rd32(r + 0x1C), size_v = rd32(v + 8), header_v = rd32(v + 0x1C);
                uint8_t *a = (uint8_t*)malloc(size_r), *b = (uint8_t*)malloc(size_v);
                int ok = a && b && read_file(file, offset_r, a, size_r) && stream_pack_read(pack, rd32(v + 4), b, size_v) == (long)size_v;
                CHECK(ok);
                CHECK(ok && size_r - header_r == size_v - header_v && !memcmp(a + header_r, b + header_v, size_r - header_r));
                free(a);
                free(b);
            }
        }
        for (int k = 0; k < audio_n; k++) {
            int found = 0;
            for (uint32_t i = 0; i < view_count; i++) {
                const uint8_t* v = view_dir + (size_t)i * 0x38;
                uint8_t* b;
                if (rd32(v) != audio[k].id) continue;
                found = 1;
                b = (uint8_t*)malloc(rd32(v + 8));
                CHECK(b && stream_pack_read(pack, rd32(v + 4), b, rd32(v + 8)) == (long)rd32(v + 8) && rd32(v + 8) == audio[k].size &&
                      !memcmp(b, audio[k].record, audio[k].size));
                free(b);
                break;
            }
            CHECK(found);
        }
        for (int i = 0; i < expected.count; i++) {
            int restored = 0;
            for (int k = 0; k < audio_n; k++) restored |= audio[k].id == expected.id[i];
            CHECK(restored || has_sample(dir, count, expected.id[i]));
        }
    }
    {
        played_t played = {&expected, 0, 0};
        CHECK(jpt_tpa_captions(read_view, pack, stream_pack_size(pack), check_played, &played, NULL));
        CHECK(played.seen == expected.count && played.differing == 0);
    }
    free(dir);
    free(view_dir);
    stream_pack_close(pack);
    CloseHandle(file);
}

/* ---- timings in every language */

typedef struct {
    int lines;
    uint32_t units[JPT_CAPTION_MAX_LINES];
} timing_t;

typedef struct {
    int count;
    uint32_t id[600];
    timing_t timing[600];
} timings_t;

static void collect_timing(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    timings_t* t = (timings_t*)ctx;
    (void)lines;
    int at = t->count;
    if (strcmp(pack, "stream")) return;
    for (int i = 0; i < t->count; i++)
        if (t->id[i] == sample) at = i;
    if (at == t->count && t->count == 600) return;
    t->id[at] = sample;
    t->timing[at].lines = count;
    memcpy(t->timing[at].units, units, (size_t)count * sizeof *units);
    t->count += at == t->count;
}

static timings_t s_english, s_other;

static const timing_t* timing_of(const timings_t* t, uint32_t id) {
    for (int i = 0; i < t->count; i++)
        if (t->id[i] == id) return &t->timing[i];
    return NULL;
}

/* Each installed release's own Stream.tpa with the English captions given to it: every sample with a block says so in its
 * descriptor, the other samples do not, and the game reads back the block that was given. */
static void test_release_views(void) {
    static const char* const files[][2] = { { "en-116", "Stream.tpa" }, { "ru-116", "Stream.tpa" }, { "es-117", "stream.tpa" },
                                            { "ptbr-117", "Stream.tpa" } };
    for (size_t f = 0; f < sizeof files / sizeof files[0]; f++) {
        char path[MAX_PATH];
        HANDLE file;
        LARGE_INTEGER size;
        static expected_t expected;
        stream_caption_t captions[512];
        stream_pack_stats_t stats;
        stream_pack_t* pack;
        const char* english;
        size_t english_size;
        uint8_t view_head[0x18], *view_dir;
        uint32_t count, with_block = 0, flagged = 0, mismatched = 0;
        snprintf(path, sizeof path, "%s/game/versions/%s/data/%s", JPT_SOURCE_DIR, files[f][0], files[f][1]);
        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
            printf("stream_tests: no Stream.tpa for %s: skipped\n", files[f][0]);
            s_skipped++;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            continue;
        }
        memset(&expected, 0, sizeof expected);
        CHECK(l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size));
        jpt_l10n_read_subtitles(english, english_size, expect_caption, &expected);
        for (int i = 0; i < expected.count; i++) captions[i] = (stream_caption_t){expected.id[i], expected.block[i], expected.size[i]};
        pack = stream_pack_open(read_file, file, (uint64_t)size.QuadPart, NULL, 0, captions, expected.count, &stats);
        CHECK(pack != NULL);
        if (pack) {
            CHECK(stream_pack_read(pack, 0, view_head, 0x18) == 0x18);
            count = rd32(view_head + 4);
            view_dir = (uint8_t*)malloc((size_t)count * 0x38);
            CHECK(view_dir && stream_pack_read(pack, 0x18, view_dir, (size_t)count * 0x38) == (long)((size_t)count * 0x38));
            for (uint32_t i = 0; view_dir && i < count; i++) {
                const uint8_t* e = view_dir + (size_t)i * 0x38;
                const int block = rd32(e + 0x1C) > 0x24;
                with_block += (uint32_t)block;
                flagged += rd32(e + 0x34) == 0x24;
                mismatched += block != (rd32(e + 0x34) == 0x24);
            }
            CHECK(mismatched == 0 && with_block == flagged);
            CHECK(with_block >= 390);
            {
                played_t played = {&expected, 0, 0};
                CHECK(jpt_tpa_captions(read_view, pack, stream_pack_size(pack), check_played, &played, NULL));
                CHECK(played.seen > 390 && played.differing == 0);
            }
            free(view_dir);
            stream_pack_close(pack);
        }
        CloseHandle(file);
    }
}

static void test_timing_parity(void) {
    static const char* const kinds[] = {JPT_L10N_SUBTITLES_ASSET, JPT_L10N_SUBTITLES_PORT_ASSET};
    const char* data;
    size_t n;
    int compared = 0;
    memset(&s_english, 0, sizeof s_english);
    CHECK(l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &data, &n));
    jpt_l10n_read_subtitles(data, n, collect_timing, &s_english);
    CHECK(s_english.count > 400);
    for (int l = 0; l < JPT_L10N_LANGUAGE_COUNT; l++) {
        const char* code = jpt_l10n_languages[l].code;
        if (!strcmp(code, JPT_L10N_ENGLISH)) continue;
        memset(&s_other, 0, sizeof s_other);
        for (int k = 0; k < 2; k++)
            if (l10n_asset(code, kinds[k], &data, &n)) jpt_l10n_read_subtitles(data, n, collect_timing, &s_other);
        for (int i = 0; i < s_other.count; i++) {
            const timing_t* canon = timing_of(&s_english, s_other.id[i]);
            if (!canon) continue;
            compared++;
            if (canon->lines != s_other.timing[i].lines || memcmp(canon->units, s_other.timing[i].units, (size_t)canon->lines * sizeof canon->units[0])) {
                printf("timing differs: %s sample %08x\n", code, s_other.id[i]);
                CHECK(0);
            }
        }
    }
    CHECK(compared > 1500);
}

/* ---- no development path anywhere in the project */

static int scan_file(const char* path, const char* needle_a, const char* needle_b) {
    FILE* f = fopen(path, "rb");
    long size;
    char* data;
    int hit = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)size + 1);
    if (data && fread(data, 1, (size_t)size, f) == (size_t)size) {
        data[size] = 0;
        hit = strstr(data, needle_a) != NULL || strstr(data, needle_b) != NULL;
    }
    free(data);
    fclose(f);
    return hit;
}

static int scannable(const char* name) {
    static const char* const extensions[] = {".c", ".cpp", ".h", ".hpp", ".cmake", ".txt", ".md", ".ini", ".json", ".ps1", ".py", ".inc", ".rc", ".sample"};
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; i++)
        if (!_stricmp(dot, extensions[i])) return 1;
    return 0;
}

static void scan_directory(const char* dir, const char* needle_a, const char* needle_b, int* files, int* hits) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        snprintf(path, sizeof path, "%s\\%s", dir, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory(path, needle_a, needle_b, files, hits);
        } else if (scannable(found.cFileName) && _stricmp(found.cFileName, "local.json") && found.nFileSizeLow < (64u << 20)) {
            (*files)++;
            if (scan_file(path, needle_a, needle_b)) {
                printf("development path in %s\n", path);
                (*hits)++;
            }
        }
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

static void test_no_development_path(void) {
    static const char* const folders[] = {"src", "launcher", "tools", "config", "assets", "packaging", "docs", "patches"};
    char needle_a[32], needle_b[32], path[MAX_PATH];
    int files = 0, hits = 0;
    snprintf(needle_a, sizeof needle_a, "%s%s", "Decomp_", "Downloads");
    snprintf(needle_b, sizeof needle_b, "%s%s", "Trespasser_English_", "captions_v1");
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) {
        snprintf(path, sizeof path, "%s\\%s", JPT_SOURCE_DIR, folders[i]);
        scan_directory(path, needle_a, needle_b, &files, &hits);
    }
    snprintf(path, sizeof path, "%s\\README.md", JPT_SOURCE_DIR);
    files++;
    hits += scan_file(path, needle_a, needle_b);
    snprintf(path, sizeof path, "%s\\CMakeLists.txt", JPT_SOURCE_DIR);
    files++;
    hits += scan_file(path, needle_a, needle_b);
    CHECK(files > 50);
    CHECK(hits == 0);
}

int main(void) {
    test_synthetic_pack();
    test_caption_into_a_sample_without_one();
    test_game_handle_pool();
    test_release_views();
    test_embedded_audio();
    test_retail_view();
    test_timing_parity();
    test_no_development_path();
    printf("stream_tests: %d checks, %d failures%s\n", s_checks, s_failures, s_skipped ? " (retail checks skipped)" : "");
    return s_failures ? 1 : 0;
}
