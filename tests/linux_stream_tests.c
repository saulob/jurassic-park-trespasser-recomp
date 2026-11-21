/* The virtual Stream.tpa on Linux, through the kernel32 handle table the way the guest uses it: every handle of the
 * game's pool (one read-only directory handle, then read-write stream handles) reads the same corrected view with its own
 * position, a write never reaches the retail file, and every view is released with its handle. A synthetic TPA covers
 * the rules; the installed retail file (game/data/Stream.tpa) covers the Beach samples and the restored records, and the
 * retail checks report DATA UNAVAILABLE without it. Headless. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_fs.h"
#include "jpt_hash.h"
#include "jpt_l10n.h"
#include "jpt_restored_audio.h"
#include "jpt_vpath.h"
#include "l10n_assets.h"
#include "linux_hle.h"
#include "stream_vfile.h"

static int g_checks, g_failures, g_unavailable;

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

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define OPEN_EXISTING 3u
#define FLAG_SEQUENTIAL 0x08000000u
#define FLAG_OVERLAPPED 0x40000000u
#define INVALID_HANDLE 0xFFFFFFFFu
#define POOL_HANDLES 8u

static uint32_t s_stack, s_scratch;
static char s_root[256];

static uint32_t call_n(uint32_t thunk, int argc, const uint32_t* args) {
    uint32_t esp = s_stack + 0x8000 - 4 * (uint32_t)(argc + 1), eax = 0, edx = 0;
    gp_write_u32(esp, 0x00401000u);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    if (!ht_invoke(thunk, &esp, &eax, &edx)) return 0xBAD0BAD0u;
    return eax;
}

static uint32_t api(const char* name, int argc, ...) {
    uint32_t args[16], thunk;
    va_list list;
    va_start(list, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(list, uint32_t);
    va_end(list);
    thunk = hle_resolve("KERNEL32.dll", name, 0, NULL);
    return thunk ? call_n(thunk, argc, args) : 0xBAD1BAD1u;
}

#define K32(name, argc, ...) api(#name, argc, ##__VA_ARGS__)

static uint32_t put_string(uint32_t where, const char* text) {
    gp_copy_out(where, text, (uint32_t)strlen(text) + 1);
    return where;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t open_stream(const char* name, uint32_t access, uint32_t flags) {
    return K32(CreateFileA, 7, put_string(s_scratch, name), access, 1u, 0u, OPEN_EXISTING, flags, 0u);
}

/* Bytes read through a handle at an absolute position; -1 when the call failed. */
static long read_at(uint32_t handle, uint64_t position, void* out, uint32_t n) {
    uint32_t buffer = s_scratch + 0x4000, count = s_scratch + 0x3F00, high = s_scratch + 0x3F10;
    gp_write_u32(high, (uint32_t)(position >> 32));
    if (K32(SetFilePointer, 4, handle, (uint32_t)position, high, 0u) == INVALID_HANDLE && K32(GetLastError, 0) != 0) return -1;
    gp_write_u32(count, 0xDEADBEEFu);
    if (n > 0x40000u || !K32(ReadFile, 5, handle, buffer, n, count, 0u)) return -1;
    gp_copy_in(out, buffer, *(uint32_t*)gm_ptr(count, 4));
    return (long)*(uint32_t*)gm_ptr(count, 4);
}

static uint64_t size_of(uint32_t handle) {
    uint32_t high = s_scratch + 0x3F20;
    uint32_t low;
    gp_write_u32(high, 0);
    low = K32(GetFileSize, 2, handle, high);
    return (uint64_t)*(uint32_t*)gm_ptr(high, 4) << 32 | low;
}

/* ---- a retail Stream.tpa made up on disk */

static size_t build_record(uint8_t* out, const uint8_t* block, size_t block_size, const uint8_t* payload, size_t payload_size) {
    size_t header = 0x24 + block_size;
    memset(out, 0, header);
    wr32(out, 0x524F4257);
    wr32(out + 4, 110);
    wr32(out + 8, (uint32_t)header);
    wr32(out + 16, (uint32_t)payload_size);
    wr32(out + 20, (uint32_t)payload_size);
    wr32(out + 24, 22050);
    wr32(out + 28, 0x110);
    if (block_size) wr32(out + 32, 0x24);
    if (block_size) memcpy(out + 0x24, block, block_size);
    memcpy(out + header, payload, payload_size);
    return header + payload_size;
}

static size_t write_file(const char* path, const uint8_t* bytes, size_t n) {
    FILE* f = fopen(path, "wb");
    size_t written;
    if (!f) return 0;
    written = fwrite(bytes, 1, n, f);
    fclose(f);
    return written;
}

static uint64_t hash_of_file(const char* path) {
    FILE* f = fopen(path, "rb");
    uint64_t h = 1469598103934665603ull;
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) h = (h ^ (uint64_t)c) * 1099511628211ull;
    fclose(f);
    return h;
}

typedef struct {
    uint32_t id;
    uint8_t block[0x2000];
    size_t block_size;
    int found;
} first_caption_t;

static void take_first_caption(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    first_caption_t* f = (first_caption_t*)ctx;
    char converted[JPT_CAPTION_MAX_LINES][512];
    const char* pointers[JPT_CAPTION_MAX_LINES];
    if (f->found || strcmp(pack, "stream") || count <= 0 || count > JPT_CAPTION_MAX_LINES) return;
    for (int i = 0; i < count; i++) {
        if (jpt_utf8_to_cp(1252, lines[i], converted[i], sizeof converted[i]) == (size_t)-1) return;
        pointers[i] = converted[i];
    }
    f->block_size = jpt_caption_build(count, units, pointers, f->block, sizeof f->block);
    f->id = sample;
    f->found = f->block_size != 0;
}

static char s_tpa_path[400];
static uint8_t s_tpa[8192];
static size_t s_tpa_size;
static uint32_t s_ids[3];
static first_caption_t s_caption;

static void make_synthetic_tpa(void) {
    uint8_t record[512], payload[40];
    size_t at = 0x18 + 3 * 0x38;
    const char* english;
    size_t english_size;
    memset(s_tpa, 0, sizeof s_tpa);
    if (l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size)) jpt_l10n_read_subtitles(english, english_size, take_first_caption, &s_caption);
    s_ids[0] = s_caption.found ? s_caption.id : 0x1111u;
    s_ids[1] = 0x2222u;
    s_ids[2] = 0x3333u;
    wr32(s_tpa, 0x150);
    wr32(s_tpa + 4, 3);
    wr32(s_tpa + 8, 0x18);
    wr32(s_tpa + 0x14, 3);
    for (int i = 0; i < 3; i++) {
        uint8_t* entry = s_tpa + 0x18 + (size_t)i * 0x38;
        size_t size;
        for (int k = 0; k < 40; k++) payload[k] = (uint8_t)(i * 31 + k);
        size = build_record(record, NULL, 0, payload, sizeof payload);
        wr32(entry, s_ids[i]);
        wr32(entry + 4, (uint32_t)at);
        wr32(entry + 8, (uint32_t)size);
        memcpy(entry + 0x14, record, 0x24);
        memcpy(s_tpa + at, record, size);
        at += size;
    }
    s_tpa_size = at;
}

static const uint8_t* directory_entry(const uint8_t* directory, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++)
        if (rd32(directory + (size_t)i * 0x38) == id) return directory + (size_t)i * 0x38;
    return NULL;
}

static void test_synthetic_pool(void) {
    uint32_t handles[2 * POOL_HANDLES];
    uint8_t head[0x18], directory[0x18 + 16 * 0x38];
    uint64_t real_hash = hash_of_file(s_tpa_path), view_size;
    hle_handle_stats_t stats;
    uint32_t view_count;

    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++)
        handles[k] = open_stream("DATA\\stream.TPA", k % POOL_HANDLES == 0 ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE, FLAG_SEQUENTIAL);
    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) CHECK(handles[k] != INVALID_HANDLE, "pool handle %u opened", k);
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 2 * POOL_HANDLES && stats.files == 2 * POOL_HANDLES, "every pool handle holds a view (%u views, %u files)",
          stats.stream_views, stats.files);
    CHECK(stream_vfile_live() == 2 * POOL_HANDLES, "the portable layer counts the same views (%u)", stream_vfile_live());

    CHECK(read_at(handles[0], 0, head, sizeof head) == (long)sizeof head && rd32(head) == 0x150, "the directory handle reads the header");
    view_count = rd32(head + 4);
    CHECK(view_count == 3 + 5, "the view adds the five restored samples to the three of the file (%u)", view_count);
    CHECK(rd32(head + 0x14) == view_count, "the count copy the game sizes its directory from follows it");
    view_size = size_of(handles[0]);
    CHECK(view_size > s_tpa_size, "the view is longer than the physical file (%llu > %zu)", (unsigned long long)view_size, s_tpa_size);
    for (uint32_t k = 1; k < 2 * POOL_HANDLES; k++) CHECK(size_of(handles[k]) == view_size, "handle %u reports the same virtual size", k);
    CHECK(read_at(handles[0], 0x18, directory, view_count * 0x38) == (long)(view_count * 0x38), "the whole virtual directory reads");

    {
        const uint8_t* restored_entry = NULL;
        const uint8_t* manifest;
        size_t manifest_size;
        jpt_restored_entry_t entries[JPT_RESTORED_MAX];
        int count;
        CHECK(restored_audio_asset(JPT_RESTORED_MANIFEST, &manifest, &manifest_size), "the restored manifest is embedded");
        count = jpt_restored_parse((const char*)manifest, manifest_size, entries, JPT_RESTORED_MAX);
        CHECK(count == 5, "five restored records (%d)", count);
        for (int i = 0; i < count; i++) {
            const uint8_t* embedded;
            size_t embedded_size;
            restored_entry = directory_entry(directory, view_count, entries[i].id);
            CHECK(restored_entry != NULL, "%s is in the virtual directory", entries[i].name);
            if (!restored_entry || !restored_audio_asset(entries[i].file, &embedded, &embedded_size)) continue;
            CHECK(rd32(restored_entry + 4) >= s_tpa_size, "%s lies beyond the physical end of the file", entries[i].name);
            for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) {
                uint8_t* got = (uint8_t*)malloc(embedded_size);
                long n = got ? read_at(handles[k], rd32(restored_entry + 4), got, (uint32_t)embedded_size > 0x40000u ? 0x40000u : (uint32_t)embedded_size) : -1;
                uint32_t want = embedded_size > 0x40000u ? 0x40000u : (uint32_t)embedded_size;
                CHECK(n == (long)want && !memcmp(got, embedded, want), "%s through handle %u is the embedded record", entries[i].name, k);
                free(got);
            }
        }
    }

    if (s_caption.found) {
        const uint8_t* entry = directory_entry(directory, view_count, s_caption.id);
        CHECK(entry && rd32(entry + 0x1C) == 0x24 + s_caption.block_size, "the sample that lacked a caption block now has one");
        if (entry && rd32(entry + 0x1C) == 0x24 + s_caption.block_size)
            for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) {
                uint8_t* got = (uint8_t*)malloc(s_caption.block_size);
                CHECK(got && read_at(handles[k], rd32(entry + 4) + 0x24, got, (uint32_t)s_caption.block_size) == (long)s_caption.block_size &&
                          !memcmp(got, s_caption.block, s_caption.block_size),
                      "the corrected caption through handle %u", k);
                free(got);
            }
    } else {
        printf("linux_stream_tests: DATA UNAVAILABLE (no English caption to correct a sample with)\n");
        g_unavailable++;
    }

    {
        uint32_t position_a, position_b;
        uint8_t first[16], second[16];
        K32(SetFilePointer, 4, handles[2], 0u, 0u, 0u);
        K32(SetFilePointer, 4, handles[3], 0x18u, 0u, 0u);
        CHECK(read_at(handles[2], 0, first, 0) == 0, "a zero-length read");
        K32(SetFilePointer, 4, handles[2], 0u, 0u, 0u);
        K32(ReadFile, 5, handles[2], s_scratch + 0x4000, 16u, s_scratch + 0x3F00, 0u);
        K32(ReadFile, 5, handles[3], s_scratch + 0x5000, 16u, s_scratch + 0x3F04, 0u);
        gp_copy_in(first, s_scratch + 0x4000, 16);
        gp_copy_in(second, s_scratch + 0x5000, 16);
        CHECK(rd32(first) == 0x150 && !memcmp(second, directory, 16), "two handles read from their own positions");
        position_a = K32(SetFilePointer, 4, handles[2], 0u, 0u, 1u);
        position_b = K32(SetFilePointer, 4, handles[3], 0u, 0u, 1u);
        CHECK(position_a == 16 && position_b == 0x18 + 16, "each handle's position advanced by its own read (%u, %u)", position_a, position_b);
        CHECK(K32(SetFilePointer, 4, handles[4], (uint32_t)-8, 0u, 2u) == (uint32_t)view_size - 8, "a distance from the virtual end");
        CHECK(read_at(handles[4], view_size, first, 16) == 0, "a read at the virtual end returns 0 bytes and succeeds");
        CHECK(read_at(handles[4], view_size + 100, first, 16) == 0, "a read past the virtual end returns 0 bytes");
        CHECK(K32(SetFilePointer, 4, handles[4], (uint32_t)-1, 0u, 0u) == INVALID_HANDLE && K32(GetLastError, 0) == 131, "a negative target: ERROR_NEGATIVE_SEEK");
        CHECK(K32(SetFilePointer, 4, handles[4], 0u, 0u, 7u) == INVALID_HANDLE, "an unknown method fails");
    }

    CHECK(K32(WriteFile, 5, handles[1], put_string(s_scratch + 0x6000, "XXXX"), 4u, s_scratch + 0x3F00, 0u) == 0 && K32(GetLastError, 0) == 5,
          "a write through a view handle is refused (ERROR_ACCESS_DENIED)");
    CHECK(hash_of_file(s_tpa_path) == real_hash, "the retail file is unchanged after the refused write");

    CHECK(open_stream("data\\Stream.tpa", GENERIC_READ, FLAG_OVERLAPPED) != INVALID_HANDLE, "an overlapped open is served by the real file");
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 2 * POOL_HANDLES, "and holds no view");
    {
        uint32_t raw = open_stream("data\\Stream.tpa", GENERIC_READ, FLAG_OVERLAPPED);
        CHECK(size_of(raw) == s_tpa_size, "the unserved handle sees the physical file");
        K32(CloseHandle, 1, raw);
    }
    CHECK(open_stream("data\\Effects.tpa", GENERIC_READ, 0) != INVALID_HANDLE, "another TPA is left alone");
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 2 * POOL_HANDLES, "and holds no view either");

    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) CHECK(K32(CloseHandle, 1, handles[k]) == 1, "pool handle %u closed", k);
    CHECK(K32(CloseHandle, 1, handles[0]) == 0 && K32(GetLastError, 0) == 6, "a closed handle is invalid");
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 0 && stream_vfile_live() == 0, "no view is left after the handles are closed");
}

static void test_limits_and_lifetime(void) {
    uint32_t handles[STREAM_PACK_MAX_VIEWS + 2];
    hle_handle_stats_t before, after;
    uint32_t extra, raw_size;
    hle_handle_stats(&before);
    for (uint32_t i = 0; i < STREAM_PACK_MAX_VIEWS; i++) handles[i] = open_stream("data\\stream.tpa", GENERIC_READ | GENERIC_WRITE, 0);
    CHECK(stream_vfile_live() == STREAM_PACK_MAX_VIEWS, "the limit of views is reached");
    extra = open_stream("data\\stream.tpa", GENERIC_READ | GENERIC_WRITE, 0);
    CHECK(extra != INVALID_HANDLE, "the handle past the limit still opens");
    raw_size = (uint32_t)size_of(extra);
    CHECK(raw_size == s_tpa_size, "and reads the player's file as it is (%u)", raw_size);
    CHECK(stream_vfile_live() == STREAM_PACK_MAX_VIEWS, "without a view of its own");
    K32(CloseHandle, 1, extra);
    for (uint32_t i = 0; i < STREAM_PACK_MAX_VIEWS; i++) K32(CloseHandle, 1, handles[i]);
    for (int round = 0; round < 500; round++) {
        uint32_t h = open_stream("data\\stream.tpa", GENERIC_READ, 0);
        if (h == INVALID_HANDLE) break;
        K32(CloseHandle, 1, h);
    }
    hle_handle_stats(&after);
    CHECK(after.files == before.files && after.stream_views == 0 && stream_vfile_live() == 0, "500 open/close cycles leave nothing (files %u -> %u)", before.files, after.files);
    CHECK(stream_vfile_opened_total() > STREAM_PACK_MAX_VIEWS, "the views really were made (%u)", stream_vfile_opened_total());
}

static void test_read_only_file_and_foreign_files(void) {
    uint32_t h;
    uint8_t garbage[64];
    char path[400];
    snprintf(path, sizeof path, "%s/data/Other.tpa", s_root);
    chmod(s_tpa_path, 0444);
    h = open_stream("data\\stream.tpa", GENERIC_READ | GENERIC_WRITE, FLAG_SEQUENTIAL);
    CHECK(h != INVALID_HANDLE, "a read-write open of a read-only retail file succeeds: the view never needs write access");
    K32(CloseHandle, 1, h);
    chmod(s_tpa_path, 0644);

    for (unsigned i = 0; i < sizeof garbage; i++) garbage[i] = (uint8_t)(i * 7 + 1);
    snprintf(path, sizeof path, "%s/data/Stream.tpa", s_root);
    {
        uint8_t keep[8192];
        memcpy(keep, s_tpa, sizeof keep);
        write_file(path, garbage, sizeof garbage);
        h = open_stream("data\\stream.tpa", GENERIC_READ, 0);
        CHECK(h != INVALID_HANDLE && size_of(h) == sizeof garbage && stream_vfile_live() == 0, "a Stream.tpa that is not a TPA is read as it is");
        K32(CloseHandle, 1, h);
        write_file(path, keep, s_tpa_size);
    }
}

/* ---- the player's retail Stream.tpa */

typedef struct {
    uint32_t id;
    uint8_t* block;
    size_t size;
} expected_caption_t;

static expected_caption_t s_expected[1024];
static int s_expected_count;

static void collect_expected(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    char converted[JPT_CAPTION_MAX_LINES][512];
    const char* pointers[JPT_CAPTION_MAX_LINES];
    uint8_t block[0x2000];
    size_t size;
    (void)ctx;
    if (strcmp(pack, "stream") || count <= 0 || count > JPT_CAPTION_MAX_LINES || s_expected_count == 1024) return;
    for (int i = 0; i < count; i++) {
        if (jpt_utf8_to_cp(1252, lines[i], converted[i], sizeof converted[i]) == (size_t)-1) return;
        pointers[i] = converted[i];
    }
    size = jpt_caption_build(count, units, pointers, block, sizeof block);
    s_expected[s_expected_count].id = sample;
    s_expected[s_expected_count].block = (uint8_t*)malloc(size);
    memcpy(s_expected[s_expected_count].block, block, size);
    s_expected[s_expected_count].size = size;
    s_expected_count++;
}

static int pread_reader(void* context, uint64_t offset, void* buffer, size_t n) {
    return pread((int)(intptr_t)context, buffer, n, (off_t)offset) == (ssize_t)n;
}

static int read_pack(void* context, uint64_t offset, void* buffer, size_t n) {
    return stream_pack_read((const stream_pack_t*)context, offset, buffer, n) == (long)n;
}

typedef struct {
    int seen, differing;
} played_t;

static void check_played(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    played_t* p = (played_t*)ctx;
    for (int i = 0; i < s_expected_count; i++) {
        if (s_expected[i].id != sample) continue;
        p->seen++;
        p->differing += s_expected[i].size != n || memcmp(s_expected[i].block, block, n);
    }
}

static void test_retail(const char* game_dir) {
    char retail[1024];
    struct stat st;
    uint32_t handles[2 * POOL_HANDLES];
    uint8_t head[0x18], *directory = NULL, *real_directory = NULL;
    uint32_t view_count, real_count;
    const char* english;
    size_t english_size;
    static const char* const beach[] = { "VH116", "VH117", "VH10", "VH16" };
    hle_handle_stats_t stats;
    snprintf(retail, sizeof retail, "%s/data/Stream.tpa", game_dir);
    if (stat(retail, &st) != 0) {
        printf("linux_stream_tests: DATA UNAVAILABLE (no retail Stream.tpa at %s: the retail checks are not run)\n", retail);
        g_unavailable++;
        return;
    }
    CHECK(l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size), "the English captions are embedded");
    s_expected_count = 0;
    jpt_l10n_read_subtitles(english, english_size, collect_expected, NULL);
    CHECK(s_expected_count == 410, "410 English caption samples (%d)", s_expected_count);

    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++)
        handles[k] = open_stream("data\\Stream.tpa", k % POOL_HANDLES == 0 ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE, FLAG_SEQUENTIAL);
    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) CHECK(handles[k] != INVALID_HANDLE, "retail pool handle %u opened", k);
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 2 * POOL_HANDLES, "all %u handles of two packs hold the view (%u)", 2 * POOL_HANDLES, stats.stream_views);

    CHECK(read_at(handles[0], 0, head, sizeof head) == (long)sizeof head && rd32(head) == 0x150, "the retail view's header");
    view_count = rd32(head + 4);
    {
        FILE* f = fopen(retail, "rb");
        uint8_t real_head[0x18];
        CHECK(f && fread(real_head, 1, sizeof real_head, f) == sizeof real_head, "the real header reads");
        real_count = f ? rd32(real_head + 4) : 0;
        real_directory = (uint8_t*)malloc((size_t)real_count * 0x38);
        CHECK(f && real_directory && fseek(f, 0x18, SEEK_SET) == 0 && fread(real_directory, 1, (size_t)real_count * 0x38, f) == (size_t)real_count * 0x38, "the real directory reads");
        if (f) fclose(f);
    }
    CHECK(view_count == real_count + 4, "four restored samples are added and one replaced (%u over %u)", view_count, real_count);
    directory = (uint8_t*)malloc((size_t)view_count * 0x38);
    CHECK(directory && read_at(handles[0], 0x18, directory, view_count * 0x38) == (long)(view_count * 0x38), "the virtual directory reads");
    if (!directory || !real_directory) {
        free(directory);
        free(real_directory);
        for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) K32(CloseHandle, 1, handles[k]);
        return;
    }

    for (size_t b = 0; b < sizeof beach / sizeof beach[0]; b++) {
        uint32_t id = jpt_sample_id(beach[b]);
        const uint8_t *entry = directory_entry(directory, view_count, id), *real_entry = directory_entry(real_directory, real_count, id);
        uint8_t *reference = NULL, *got;
        uint32_t size, header, real_size, real_header;
        const expected_caption_t* caption = NULL;
        for (int i = 0; i < s_expected_count; i++)
            if (s_expected[i].id == id) caption = &s_expected[i];
        CHECK(entry != NULL && real_entry != NULL, "%s is in the retail directory and in the view", beach[b]);
        if (!entry || !real_entry) continue;
        size = rd32(entry + 8);
        header = rd32(entry + 0x1C);
        real_size = rd32(real_entry + 8);
        real_header = rd32(real_entry + 0x1C);
        CHECK(size > 0x24 && size < (32u << 20), "%s: record size %u", beach[b], size);
        CHECK(size - header == real_size - real_header, "%s: payload size is the retail one", beach[b]);
        got = (uint8_t*)malloc(size);
        reference = (uint8_t*)malloc(size);
        if (!got || !reference) {
            free(got);
            free(reference);
            continue;
        }
        CHECK(read_at(handles[0], rd32(entry + 4), reference, size > 0x40000u ? 0x40000u : size) >= 0, "%s read through the directory handle", beach[b]);
        for (uint32_t k = 1; k < 2 * POOL_HANDLES; k++) {
            long n = read_at(handles[k], rd32(entry + 4), got, size > 0x40000u ? 0x40000u : size);
            CHECK(n == (long)(size > 0x40000u ? 0x40000u : size) && !memcmp(got, reference, (size_t)n), "%s through pool handle %u equals handle 0", beach[b], k);
        }
        if (caption)
            CHECK(header == 0x24 + caption->size && !memcmp(reference + 0x24, caption->block, caption->size), "%s: the corrected caption is in its record", beach[b]);
        {
            FILE* f = fopen(retail, "rb");
            uint8_t* real = (uint8_t*)malloc(real_size);
            uint32_t compare = size - header < 0x40000u ? size - header : 0x40000u;
            CHECK(f && real && fseek(f, (long)(rd32(real_entry + 4) + real_header), SEEK_SET) == 0 && fread(real, 1, compare, f) == compare, "%s payload read from the real file", beach[b]);
            CHECK(real && header + compare <= (size > 0x40000u ? 0x40000u : size) + 0 && !memcmp(real, reference + header, compare), "%s payload is byte-identical to the retail audio", beach[b]);
            free(real);
            if (f) fclose(f);
        }
        free(got);
        free(reference);
    }

    {
        const uint8_t* manifest;
        size_t manifest_size;
        jpt_restored_entry_t entries[JPT_RESTORED_MAX];
        int count;
        restored_audio_asset(JPT_RESTORED_MANIFEST, &manifest, &manifest_size);
        count = jpt_restored_parse((const char*)manifest, manifest_size, entries, JPT_RESTORED_MAX);
        for (int i = 0; i < count; i++) {
            const uint8_t* entry = directory_entry(directory, view_count, entries[i].id);
            const uint8_t* embedded;
            size_t embedded_size;
            CHECK(entry != NULL && restored_audio_asset(entries[i].file, &embedded, &embedded_size), "restored %s is in the retail view", entries[i].name);
            if (!entry || !restored_audio_asset(entries[i].file, &embedded, &embedded_size)) continue;
            CHECK(rd32(entry + 8) == embedded_size && rd32(entry + 4) >= (uint32_t)st.st_size - 0x1000000u, "%s record size and place", entries[i].name);
            for (uint32_t k = 0; k < 2 * POOL_HANDLES; k += 5) {
                uint32_t want = embedded_size > 0x40000u ? 0x40000u : (uint32_t)embedded_size;
                uint8_t* got = (uint8_t*)malloc(want);
                CHECK(got && read_at(handles[k], rd32(entry + 4), got, want) == (long)want && !memcmp(got, embedded, want), "restored %s through handle %u", entries[i].name, k);
                free(got);
            }
            {
                uint8_t* tail = (uint8_t*)malloc(256);
                uint64_t last = (uint64_t)rd32(entry + 4) + embedded_size - 256;
                CHECK(tail && read_at(handles[7], last, tail, 256) == 256 && !memcmp(tail, embedded + embedded_size - 256, 256), "the end of restored %s past the physical EOF", entries[i].name);
                free(tail);
            }
        }
    }

    {
        int fd = open(retail, O_RDONLY);
        stream_pack_t* pack = fd >= 0 ? stream_catalog_open_pack(pread_reader, (void*)(intptr_t)fd, (uint64_t)st.st_size) : NULL;
        played_t played = { 0, 0 };
        CHECK(pack != NULL, "the caption scan opens the retail file through the catalog");
        if (pack) {
            CHECK(jpt_tpa_captions(read_pack, pack, stream_pack_size(pack), check_played, &played, NULL), "the scan walks the virtual directory");
            CHECK(played.seen == s_expected_count && played.differing == 0, "every one of the %d English captions reads back as given (%d seen, %d differing)",
                  s_expected_count, played.seen, played.differing);
            stream_pack_close(pack);
        }
        if (fd >= 0) close(fd);
    }

    free(directory);
    free(real_directory);
    for (uint32_t k = 0; k < 2 * POOL_HANDLES; k++) K32(CloseHandle, 1, handles[k]);
    hle_handle_stats(&stats);
    CHECK(stats.stream_views == 0 && stream_vfile_live() == 0, "the retail views are released with their handles");
}

int main(int argc, char** argv) {
    char tree[400];
    jpt_vpath_t* vp;
    char retail_dir[400];
    (void)argc;
    (void)argv;
    snprintf(s_root, sizeof s_root, "%s/jpt_linux_stream_%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    jpt_fs_mkdir(s_root);
    snprintf(tree, sizeof tree, "%s/data", s_root);
    jpt_fs_mkdir(tree);
    snprintf(s_tpa_path, sizeof s_tpa_path, "%s/data/Stream.tpa", s_root);
    snprintf(tree, sizeof tree, "%s/data/Effects.tpa", s_root);
    write_file(tree, (const uint8_t*)"effects", 7);
    make_synthetic_tpa();
    write_file(s_tpa_path, s_tpa, s_tpa_size);

    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    s_stack = gm_alloc_pages(0, 0x10000, "stack");
    s_scratch = gm_alloc_pages(0, 0x100000, "scratch");
    vp = jpt_vpath_new();
    jpt_vpath_mount(vp, 'C', s_root);
    hle_init(vp, 0x00400000u, "C:\\trespass.exe");
    CHECK(stream_catalog_audio_count() == 5, "the catalog holds the five restored records (%d)", stream_catalog_audio_count());
    CHECK(stream_catalog_caption_count() == 410, "and the 410 corrected captions (%d)", stream_catalog_caption_count());

    test_synthetic_pool();
    test_limits_and_lifetime();
    test_read_only_file_and_foreign_files();
    hle_shutdown();
    jpt_vpath_free(vp);

    snprintf(retail_dir, sizeof retail_dir, "%s/game", JPT_SOURCE_DIR);
    vp = jpt_vpath_new();
    jpt_vpath_mount(vp, 'C', retail_dir);
    hle_init(vp, 0x00400000u, "C:\\trespass.exe");
    test_retail(retail_dir);
    hle_shutdown();
    jpt_vpath_free(vp);
    gm_shutdown();

    snprintf(tree, sizeof tree, "%s/data/Effects.tpa", s_root);
    jpt_fs_remove(tree);
    jpt_fs_remove(s_tpa_path);
    snprintf(tree, sizeof tree, "%s/data", s_root);
    jpt_fs_remove_dir(tree);
    jpt_fs_remove_dir(s_root);
    printf("linux_stream_tests: %d checks, %d failures%s\n", g_checks, g_failures, g_unavailable ? " (some checks DATA UNAVAILABLE)" : "");
    return g_failures ? 1 : 0;
}
