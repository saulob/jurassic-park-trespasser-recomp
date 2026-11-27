#include "tpa_merge.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_l10n.h"

#define TPA_COLLISIONS 0x0Cu
#define TPA_COLLISIONS_AT 0x10u
#define TPA_ENTRIES 0x14u /* the directory's own length; +4 counts different samples */
#define TPA_HEADER_FIELD 0x1Cu
#define COLLISION_BYTES 0x98u
#define COLLISIONS_MAX 65536u
#define FILE_LIMIT 0xFFFFFFF0ull

typedef struct {
    uint64_t start, size;
    const uint8_t* mem; /* NULL: the source's own bytes from `at` */
    const tpa_source_t* source;
    uint64_t at;
} part_t;

struct tpa_merge {
    tpa_source_t* sources;
    part_t* parts;
    int part_count;
    uint8_t* directory;
    uint8_t* collisions;
    uint64_t total;
};

typedef struct {
    uint32_t entries, unique, collisions, collisions_at;
    uint8_t* directory;
} pack_t;

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int read_pack(const tpa_source_t* s, pack_t* p) {
    uint8_t head[JPT_TPA_HEADER];
    memset(p, 0, sizeof *p);
    if (s->size < JPT_TPA_HEADER || s->size > FILE_LIMIT || !s->read(s->ctx, 0, head, sizeof head) || rd32(head) != JPT_TPA_MAGIC ||
        rd32(head + 8) != JPT_TPA_HEADER)
        return 0;
    p->unique = rd32(head + 4);
    p->collisions = rd32(head + TPA_COLLISIONS);
    p->collisions_at = rd32(head + TPA_COLLISIONS_AT);
    p->entries = rd32(head + TPA_ENTRIES);
    if (!p->entries || p->entries > JPT_TPA_MAX_SAMPLES || JPT_TPA_HEADER + (uint64_t)p->entries * JPT_TPA_ENTRY > s->size) return 0;
    if (p->collisions > COLLISIONS_MAX || (p->collisions && p->collisions_at + (uint64_t)p->collisions * COLLISION_BYTES > s->size)) return 0;
    p->directory = (uint8_t*)malloc((size_t)p->entries * JPT_TPA_ENTRY);
    return p->directory && s->read(s->ctx, JPT_TPA_HEADER, p->directory, (size_t)p->entries * JPT_TPA_ENTRY);
}

static int entry_valid(const uint8_t* entry, uint64_t size) {
    uint32_t offset = rd32(entry + 4), bytes = rd32(entry + 8), header = rd32(entry + TPA_HEADER_FIELD);
    return header >= JPT_TPA_DESCRIPTOR && header <= bytes && (uint64_t)offset + bytes <= size;
}

static void add_part(tpa_merge_t* m, uint64_t size, const uint8_t* mem, const tpa_source_t* source, uint64_t at) {
    part_t* p;
    if (!size) return;
    p = &m->parts[m->part_count++];
    p->start = m->total;
    p->size = size;
    p->mem = mem;
    p->source = source;
    p->at = at;
    m->total += size;
}

/* Every entry of the sample's ID takes the new record; 0 when there is none. */
static int replace_sample(uint8_t* directory, uint32_t entries, const uint8_t* entry) {
    int found = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t* e = directory + (size_t)i * JPT_TPA_ENTRY;
        if (rd32(e) != rd32(entry)) continue;
        memcpy(e, entry, JPT_TPA_ENTRY);
        found = 1;
    }
    return found;
}

tpa_merge_t* tpa_merge_open(const tpa_source_t* sources, int count, tpa_merge_stats_t* stats) {
    tpa_merge_t* m = (tpa_merge_t*)calloc(1, sizeof *m);
    pack_t* packs = (pack_t*)calloc((size_t)(count > 0 ? count : 1), sizeof *packs);
    tpa_merge_stats_t st;
    uint32_t entries = 0, unique, most = 0, collisions = 0;
    uint64_t directory_end, old_end, at;
    uint8_t *out, *next;
    memset(&st, 0, sizeof st);
    if (stats) *stats = st;
    if (!m || !packs || count < 1 || !read_pack(&sources[0], &packs[0])) goto fail;
    for (int i = 0; i < count; i++) {
        if (i && !read_pack(&sources[i], &packs[i])) {
            free(packs[i].directory);
            memset(&packs[i], 0, sizeof packs[i]);
            st.refused++;
        }
        most += packs[i].entries;
        collisions += packs[i].collisions;
    }
    m->sources = (tpa_source_t*)malloc((size_t)count * sizeof *sources);
    m->parts = (part_t*)calloc((size_t)count + 3, sizeof *m->parts);
    m->directory = (uint8_t*)calloc(1, JPT_TPA_HEADER + (size_t)most * JPT_TPA_ENTRY);
    if (!m->sources || !m->parts || !m->directory) goto fail;
    memcpy(m->sources, sources, (size_t)count * sizeof *sources);
    if (!sources[0].read(sources[0].ctx, 0, m->directory, JPT_TPA_HEADER)) goto fail;
    out = m->directory + JPT_TPA_HEADER;
    entries = packs[0].entries;
    unique = packs[0].unique;
    memcpy(out, packs[0].directory, (size_t)entries * JPT_TPA_ENTRY);

    /* The directory's length decides where every record lies, so the samples are counted first. */
    for (int i = 1; i < count; i++)
        for (uint32_t k = 0; k < packs[i].entries; k++) {
            const uint8_t* e = packs[i].directory + (size_t)k * JPT_TPA_ENTRY;
            if (!entry_valid(e, sources[i].size)) {
                st.refused++;
            } else if (replace_sample(out, entries, e)) {
                st.replaced++;
            } else {
                memcpy(out + (size_t)entries++ * JPT_TPA_ENTRY, e, JPT_TPA_ENTRY);
                unique++;
                st.added++;
            }
        }
    old_end = JPT_TPA_HEADER + (uint64_t)packs[0].entries * JPT_TPA_ENTRY;
    directory_end = JPT_TPA_HEADER + (uint64_t)entries * JPT_TPA_ENTRY;
    memcpy(out, packs[0].directory, (size_t)packs[0].entries * JPT_TPA_ENTRY);
    for (uint32_t k = 0; k < packs[0].entries; k++) {
        uint8_t* e = out + (size_t)k * JPT_TPA_ENTRY;
        wr32(e + 4, rd32(e + 4) + (uint32_t)(directory_end - old_end));
    }
    at = directory_end + (sources[0].size - old_end);
    next = out + (size_t)packs[0].entries * JPT_TPA_ENTRY;
    for (int i = 1; i < count; i++) {
        if (!packs[i].entries) continue;
        if (at + sources[i].size > FILE_LIMIT) goto fail;
        for (uint32_t k = 0; k < packs[i].entries; k++) {
            const uint8_t* e = packs[i].directory + (size_t)k * JPT_TPA_ENTRY;
            uint8_t placed[JPT_TPA_ENTRY];
            if (!entry_valid(e, sources[i].size)) continue;
            memcpy(placed, e, JPT_TPA_ENTRY);
            wr32(placed + 4, rd32(placed + 4) + (uint32_t)at);
            if (replace_sample(out, (uint32_t)((size_t)(next - out) / JPT_TPA_ENTRY), placed)) continue;
            memcpy(next, placed, JPT_TPA_ENTRY);
            next += JPT_TPA_ENTRY;
        }
        at += sources[i].size;
    }
    if ((size_t)(next - out) / JPT_TPA_ENTRY != entries) goto fail;
    if (at + (uint64_t)collisions * COLLISION_BYTES > FILE_LIMIT) goto fail;
    wr32(m->directory + 4, unique);
    wr32(m->directory + TPA_ENTRIES, entries);
    wr32(m->directory + TPA_COLLISIONS, collisions);
    wr32(m->directory + TPA_COLLISIONS_AT, packs[0].collisions_at + (uint32_t)(directory_end - old_end));
    if (collisions != packs[0].collisions) {
        size_t done = 0;
        if (!(m->collisions = (uint8_t*)malloc((size_t)collisions * COLLISION_BYTES))) goto fail;
        for (int i = 0; i < count; i++) {
            size_t bytes = (size_t)packs[i].collisions * COLLISION_BYTES;
            if (bytes && !sources[i].read(sources[i].ctx, packs[i].collisions_at, m->collisions + done, bytes)) goto fail;
            done += bytes;
        }
        st.collisions = (int)(collisions - packs[0].collisions);
        wr32(m->directory + TPA_COLLISIONS_AT, (uint32_t)at);
    }

    add_part(m, directory_end, m->directory, NULL, 0);
    add_part(m, sources[0].size - old_end, NULL, &m->sources[0], old_end);
    for (int i = 1; i < count; i++)
        if (packs[i].entries) add_part(m, sources[i].size, NULL, &m->sources[i], 0);
    if (m->collisions) add_part(m, (uint64_t)collisions * COLLISION_BYTES, m->collisions, NULL, 0);
    for (int i = 0; i < count; i++) free(packs[i].directory);
    free(packs);
    if (stats) *stats = st;
    return m;

fail:
    if (packs)
        for (int i = 0; i < count; i++) free(packs[i].directory);
    free(packs);
    tpa_merge_close(m);
    return NULL;
}

void tpa_merge_close(tpa_merge_t* m) {
    if (!m) return;
    free(m->sources);
    free(m->parts);
    free(m->directory);
    free(m->collisions);
    free(m);
}

uint64_t tpa_merge_size(const tpa_merge_t* m) { return m->total; }

long tpa_merge_read(const tpa_merge_t* m, uint64_t offset, void* buf, size_t n) {
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    for (int i = 0; i < m->part_count && done < n; i++) {
        const part_t* p = &m->parts[i];
        uint64_t in_part, take;
        if (offset + done >= p->start + p->size || offset + done < p->start) continue;
        in_part = offset + done - p->start;
        take = p->size - in_part;
        if (take > n - done) take = n - done;
        if (p->mem) memcpy(out + done, p->mem + in_part, (size_t)take);
        else if (!p->source->read(p->source->ctx, p->at + in_part, out + done, (size_t)take)) return -1;
        done += (size_t)take;
    }
    return (long)done;
}
