#include "stream_pack.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_hash.h"
#include "jpt_l10n.h"

#define TPA_DESCRIPTOR_FIELD 0x14u
#define TPA_HEADER_FIELD 0x1Cu
#define TPA_BLOCK_START 0x20u /* the descriptor's last field: 0x24 where a caption block follows it, 0 in a sample without one */
#define TPA_COUNT_COPY 0x14u /* the sample count again: the game sizes the directory it reads from this one */
#define TPA_PAYLOAD_LIMIT (64u << 20)
#define TPA_FILE_LIMIT 0xFFFFFFF0ull

typedef struct {
    uint64_t start, size;
    const uint8_t* mem; /* NULL: the real file from `real` */
    uint64_t real;
} segment_t;

struct stream_pack {
    stream_read_fn read;
    void* ctx;
    segment_t* segments;
    int segment_count, segment_cap;
    uint8_t** owned;
    int owned_count;
    uint64_t total;
};

typedef struct {
    uint32_t id;
    int index;                           /* directory entry */
    const uint8_t* head;                 /* descriptor and caption block, or a whole record */
    size_t head_size;
    uint64_t payload_real, payload_size; /* a real-file payload that follows the head, or none */
} placed_t;

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int add_segment(stream_pack_t* p, uint64_t size, const uint8_t* mem, uint64_t real) {
    segment_t* s;
    if (!size) return 1;
    if (p->segment_count == p->segment_cap) {
        int cap = p->segment_cap ? p->segment_cap * 2 : 16;
        segment_t* grown = (segment_t*)realloc(p->segments, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        p->segments = grown;
        p->segment_cap = cap;
    }
    s = &p->segments[p->segment_count++];
    s->start = p->total;
    s->size = size;
    s->mem = mem;
    s->real = real;
    p->total += size;
    return 1;
}

static uint8_t* own(stream_pack_t* p, size_t n) {
    uint8_t** grown = (uint8_t**)realloc(p->owned, (size_t)(p->owned_count + 1) * sizeof *grown);
    uint8_t* mem;
    if (!grown) return NULL;
    p->owned = grown;
    mem = (uint8_t*)malloc(n ? n : 1);
    if (!mem) return NULL;
    p->owned[p->owned_count++] = mem;
    return mem;
}

void stream_pack_close(stream_pack_t* p) {
    if (!p) return;
    for (int i = 0; i < p->owned_count; i++) free(p->owned[i]);
    free(p->owned);
    free(p->segments);
    free(p);
}

uint64_t stream_pack_size(const stream_pack_t* p) { return p->total; }

long stream_pack_read(const stream_pack_t* p, uint64_t offset, void* buf, size_t n) {
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    int lo = 0, hi = p->segment_count - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (offset < p->segments[mid].start) hi = mid - 1;
        else if (offset >= p->segments[mid].start + p->segments[mid].size) lo = mid + 1;
        else {
            at = mid;
            break;
        }
    }
    if (at < 0) return 0;
    for (; at < p->segment_count && done < n; at++) {
        const segment_t* s = &p->segments[at];
        uint64_t in_segment = offset + done - s->start, take = s->size - in_segment;
        if (take > n - done) take = n - done;
        if (s->mem) memcpy(out + done, s->mem + in_segment, (size_t)take);
        else if (!p->read(p->ctx, s->real + in_segment, out + done, (size_t)take)) return -1;
        done += (size_t)take;
    }
    return (long)done;
}

static int find_first(const uint8_t* dir, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++)
        if (rd32(dir + (size_t)i * JPT_TPA_ENTRY) == id) return (int)i;
    return -1;
}

static int record_valid(const stream_audio_t* a) {
    return a->record && a->size > JPT_TPA_DESCRIPTOR && rd32(a->record + 8) >= JPT_TPA_DESCRIPTOR && rd32(a->record + 8) <= a->size;
}

static int payload_matches(const stream_pack_t* p, const uint8_t* entry, const char* sha256) {
    uint32_t offset = rd32(entry + 4), size = rd32(entry + 8), header = rd32(entry + TPA_HEADER_FIELD);
    uint8_t* payload;
    char hex[65];
    int match;
    if (!sha256 || strlen(sha256) != 64 || header > size || size - header > TPA_PAYLOAD_LIMIT) return 0;
    payload = (uint8_t*)malloc(size - header ? size - header : 1);
    if (!payload) return 0;
    match = p->read(p->ctx, (uint64_t)offset + header, payload, size - header);
    if (match) {
        jpt_sha256_buffer_hex(payload, size - header, hex);
        match = !strcmp(hex, sha256);
    }
    free(payload);
    return match;
}

static int caption_current(const stream_pack_t* p, const uint8_t* entry, const stream_caption_t* c) {
    uint8_t* old;
    int same;
    if (rd32(entry + TPA_HEADER_FIELD) != JPT_TPA_DESCRIPTOR + c->size) return 0;
    old = (uint8_t*)malloc(c->size);
    if (!old) return 0;
    same = p->read(p->ctx, (uint64_t)rd32(entry + 4) + JPT_TPA_DESCRIPTOR, old, c->size) && !memcmp(old, c->block, c->size);
    free(old);
    return same;
}

stream_pack_t* stream_pack_open(stream_read_fn read, void* ctx, uint64_t size, const stream_audio_t* audio, int audio_count,
                                const stream_caption_t* captions, int caption_count, stream_pack_stats_t* stats) {
    stream_pack_t* p = NULL;
    stream_pack_stats_t st;
    uint8_t head[JPT_TPA_HEADER], *dir = NULL, *out_dir;
    placed_t* placed = NULL;
    int placed_count = 0, added = 0;
    uint32_t count;
    uint64_t old_dir_end, new_dir_end, at;
    memset(&st, 0, sizeof st);
    if (stats) *stats = st;
    if (size < JPT_TPA_HEADER || size > TPA_FILE_LIMIT || !read(ctx, 0, head, sizeof head) || rd32(head) != JPT_TPA_MAGIC ||
        rd32(head + 8) != JPT_TPA_HEADER)
        return NULL;
    count = rd32(head + 4);
    if (!count || count > JPT_TPA_MAX_SAMPLES || JPT_TPA_HEADER + (uint64_t)count * JPT_TPA_ENTRY > size) return NULL;
    p = (stream_pack_t*)calloc(1, sizeof *p);
    dir = (uint8_t*)malloc((size_t)count * JPT_TPA_ENTRY);
    placed = (placed_t*)calloc((size_t)(audio_count + caption_count + 1), sizeof *placed);
    if (!p || !dir || !placed) goto fail;
    p->read = read;
    p->ctx = ctx;
    if (!read(ctx, JPT_TPA_HEADER, dir, (size_t)count * JPT_TPA_ENTRY)) goto fail;
    old_dir_end = JPT_TPA_HEADER + (uint64_t)count * JPT_TPA_ENTRY;

    for (int i = 0; i < audio_count; i++) {
        const stream_audio_t* a = &audio[i];
        int index = find_first(dir, count, a->id);
        if (!record_valid(a)) continue;
        if (index >= 0) {
            if (!a->replaces || !payload_matches(p, dir + (size_t)index * JPT_TPA_ENTRY, a->replaces)) {
                st.audio_kept++;
                continue;
            }
            st.audio_replaced++;
        } else {
            if (count + (uint32_t)added >= JPT_TPA_MAX_SAMPLES) continue;
            index = (int)count + added++;
            st.audio_added++;
        }
        placed[placed_count].id = a->id;
        placed[placed_count].index = index;
        placed[placed_count].head = a->record;
        placed[placed_count].head_size = a->size;
        placed_count++;
    }

    for (int i = 0; i < caption_count; i++) {
        const stream_caption_t* c = &captions[i];
        int index = find_first(dir, count, c->id), taken = 0;
        const uint8_t* entry;
        uint8_t* chunk;
        uint32_t offset, entry_size, header;
        if (index < 0 || !c->block || c->size < 8 || rd32(c->block) != c->size) continue;
        for (int k = 0; k < placed_count; k++) taken |= placed[k].index == index;
        if (taken) continue;
        entry = dir + (size_t)index * JPT_TPA_ENTRY;
        offset = rd32(entry + 4);
        entry_size = rd32(entry + 8);
        header = rd32(entry + TPA_HEADER_FIELD);
        if (header > entry_size || (uint64_t)offset + entry_size > size || caption_current(p, entry, c)) continue;
        if (!(chunk = own(p, JPT_TPA_DESCRIPTOR + c->size))) goto fail;
        memcpy(chunk, entry + TPA_DESCRIPTOR_FIELD, JPT_TPA_DESCRIPTOR);
        wr32(chunk + 8, (uint32_t)(JPT_TPA_DESCRIPTOR + c->size));
        wr32(chunk + TPA_BLOCK_START, JPT_TPA_DESCRIPTOR);
        memcpy(chunk + JPT_TPA_DESCRIPTOR, c->block, c->size);
        placed[placed_count].id = c->id;
        placed[placed_count].index = index;
        placed[placed_count].head = chunk;
        placed[placed_count].head_size = JPT_TPA_DESCRIPTOR + c->size;
        placed[placed_count].payload_real = (uint64_t)offset + header;
        placed[placed_count].payload_size = entry_size - header;
        placed_count++;
        st.captions_applied++;
    }

    new_dir_end = JPT_TPA_HEADER + (uint64_t)(count + (uint32_t)added) * JPT_TPA_ENTRY;
    if (!(out_dir = own(p, (size_t)new_dir_end))) goto fail;
    memset(out_dir, 0, (size_t)new_dir_end);
    memcpy(out_dir, head, JPT_TPA_HEADER);
    wr32(out_dir + 4, count + (uint32_t)added);
    if (rd32(head + TPA_COUNT_COPY) == count) wr32(out_dir + TPA_COUNT_COPY, count + (uint32_t)added);
    memcpy(out_dir + JPT_TPA_HEADER, dir, (size_t)count * JPT_TPA_ENTRY);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = out_dir + JPT_TPA_HEADER + (size_t)i * JPT_TPA_ENTRY;
        wr32(e + 4, rd32(e + 4) + (uint32_t)(new_dir_end - old_dir_end));
    }

    at = new_dir_end + (size - old_dir_end);
    for (int k = 0; k < placed_count; k++) {
        const placed_t* pl = &placed[k];
        uint8_t* e = out_dir + JPT_TPA_HEADER + (size_t)pl->index * JPT_TPA_ENTRY;
        uint64_t record_size = pl->head_size + pl->payload_size;
        if (at + record_size > TPA_FILE_LIMIT) goto fail;
        wr32(e, pl->id);
        wr32(e + 4, (uint32_t)at);
        wr32(e + 8, (uint32_t)record_size);
        memcpy(e + TPA_DESCRIPTOR_FIELD, pl->head, JPT_TPA_DESCRIPTOR);
        at += record_size;
    }

    if (!add_segment(p, new_dir_end, out_dir, 0) || !add_segment(p, size - old_dir_end, NULL, old_dir_end)) goto fail;
    for (int k = 0; k < placed_count; k++)
        if (!add_segment(p, placed[k].head_size, placed[k].head, 0) ||
            !add_segment(p, placed[k].payload_size, NULL, placed[k].payload_real))
            goto fail;
    free(dir);
    free(placed);
    if (stats) *stats = st;
    return p;

fail:
    free(dir);
    free(placed);
    stream_pack_close(p);
    return NULL;
}
