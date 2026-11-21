#include "stream_vfile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_l10n.h"
#include "jpt_restored_audio.h"
#include "l10n_assets.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define COUNTER_ADD(counter, delta) ((unsigned)_InterlockedExchangeAdd((volatile long*)&(counter), (long)(delta)) + (unsigned)(delta))
#define COUNTER_GET(counter) ((unsigned)(counter))
#else
#define COUNTER_ADD(counter, delta) __atomic_add_fetch(&(counter), (unsigned)(delta), __ATOMIC_SEQ_CST)
#define COUNTER_GET(counter) __atomic_load_n(&(counter), __ATOMIC_SEQ_CST)
#endif

struct stream_vfile {
    stream_pack_t* pack;
    uint64_t position;
};

static struct {
    int ready;
    stream_log_fn log;
    jpt_restored_entry_t entries[JPT_RESTORED_MAX];
    stream_audio_t audio[JPT_RESTORED_MAX];
    int audio_count;
    stream_caption_t* captions;
    uint8_t** caption_blocks;
    int caption_count, caption_cap;
    unsigned live, opened;
} S;

static void say(const char* fmt, ...) {
    char line[256];
    va_list list;
    if (!S.log) return;
    va_start(list, fmt);
    vsnprintf(line, sizeof line, fmt, list);
    va_end(list);
    S.log(line);
}

static void add_caption(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    char converted[JPT_CAPTION_MAX_LINES][512];
    const char* pointers[JPT_CAPTION_MAX_LINES];
    uint8_t block[0x2000], *copy;
    size_t size;
    (void)ctx;
    if (strcmp(pack, "stream") || count <= 0 || count > JPT_CAPTION_MAX_LINES) return;
    for (int i = 0; i < count; i++) {
        if (jpt_utf8_to_cp(1252, lines[i], converted[i], sizeof converted[i]) == (size_t)-1) return;
        pointers[i] = converted[i];
    }
    if (!(size = jpt_caption_build(count, units, pointers, block, sizeof block))) return;
    if (S.caption_count == S.caption_cap) {
        int cap = S.caption_cap ? S.caption_cap * 2 : 512;
        stream_caption_t* grown = (stream_caption_t*)realloc(S.captions, (size_t)cap * sizeof *grown);
        uint8_t** blocks = (uint8_t**)realloc(S.caption_blocks, (size_t)cap * sizeof *blocks);
        if (grown) S.captions = grown;
        if (blocks) S.caption_blocks = blocks;
        if (!grown || !blocks) return;
        S.caption_cap = cap;
    }
    if (!(copy = (uint8_t*)malloc(size))) return;
    memcpy(copy, block, size);
    S.caption_blocks[S.caption_count] = copy;
    S.captions[S.caption_count].id = sample;
    S.captions[S.caption_count].block = copy;
    S.captions[S.caption_count].size = size;
    S.caption_count++;
}

void stream_catalog_init(stream_log_fn log) {
    const uint8_t* manifest;
    size_t manifest_size;
    int count;
    const char* english;
    size_t english_size;
    if (S.ready) return;
    S.log = log;
    S.ready = 1;
    if (restored_audio_asset(JPT_RESTORED_MANIFEST, &manifest, &manifest_size)) {
        count = jpt_restored_parse((const char*)manifest, manifest_size, S.entries, JPT_RESTORED_MAX);
        for (int i = 0; i < count; i++) {
            const uint8_t* record;
            size_t record_size;
            if (!restored_audio_asset(S.entries[i].file, &record, &record_size) || record_size != S.entries[i].record_size) {
                say("restored sample %s: its record is missing or the wrong size", S.entries[i].name);
                continue;
            }
            S.audio[S.audio_count].id = S.entries[i].id;
            S.audio[S.audio_count].record = record;
            S.audio[S.audio_count].size = record_size;
            S.audio[S.audio_count].replaces = S.entries[i].replaces;
            S.audio_count++;
        }
        if (count < 0) say("the restored audio manifest is malformed: nothing is restored");
    }
    if (l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size))
        jpt_l10n_read_subtitles(english, english_size, add_caption, NULL);
}

stream_pack_t* stream_catalog_open_pack(stream_read_fn read, void* context, uint64_t real_size) {
    stream_catalog_init(NULL);
    return stream_pack_open(read, context, real_size, S.audio, S.audio_count, S.captions, S.caption_count, NULL);
}

stream_vfile_t* stream_vfile_open(stream_read_fn read, void* context, uint64_t real_size) {
    stream_pack_stats_t stats;
    stream_vfile_t* file;
    stream_pack_t* pack;
    stream_catalog_init(NULL);
    if (COUNTER_GET(S.live) >= STREAM_PACK_MAX_VIEWS) {
        say("Stream.tpa: more than %d handles open, this one reads the player's file as it is", STREAM_PACK_MAX_VIEWS);
        return NULL;
    }
    pack = stream_pack_open(read, context, real_size, S.audio, S.audio_count, S.captions, S.caption_count, &stats);
    if (!pack) {
        say("Stream.tpa is not a readable TPA: it plays as it is");
        return NULL;
    }
    file = (stream_vfile_t*)calloc(1, sizeof *file);
    if (!file) {
        stream_pack_close(pack);
        return NULL;
    }
    file->pack = pack;
    COUNTER_ADD(S.live, 1);
    if (COUNTER_ADD(S.opened, 1) == 1)
        say("Stream.tpa: %d captions corrected, %d restored samples added, %d replaced, %d left as in the player's file",
            stats.captions_applied, stats.audio_added, stats.audio_replaced, stats.audio_kept);
    return file;
}

void stream_vfile_close(stream_vfile_t* file) {
    if (!file) return;
    stream_pack_close(file->pack);
    free(file);
    COUNTER_ADD(S.live, -1);
}

int stream_vfile_read(stream_vfile_t* file, void* buffer, uint32_t wanted, uint32_t* got) {
    long n = stream_pack_read(file->pack, file->position, buffer, wanted);
    if (n < 0) {
        *got = 0;
        return 0;
    }
    file->position += (uint64_t)n;
    *got = (uint32_t)n;
    return 1;
}

int stream_vfile_seek(stream_vfile_t* file, int64_t distance, uint32_t method, uint64_t* position) {
    int64_t base = 0, target;
    if (method > 2) return 0;
    if (method == 1) base = (int64_t)file->position;
    else if (method == 2) base = (int64_t)stream_pack_size(file->pack);
    target = base + distance;
    if (target < 0) return 0;
    file->position = (uint64_t)target;
    if (position) *position = file->position;
    return 1;
}

uint64_t stream_vfile_size(const stream_vfile_t* file) { return stream_pack_size(file->pack); }
uint64_t stream_vfile_position(const stream_vfile_t* file) { return file->position; }
unsigned stream_vfile_live(void) { return COUNTER_GET(S.live); }
unsigned stream_vfile_opened_total(void) { return COUNTER_GET(S.opened); }
int stream_catalog_audio_count(void) { return S.audio_count; }
int stream_catalog_caption_count(void) { return S.caption_count; }
