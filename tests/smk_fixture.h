/* A tiny Smacker (SMK2) file built from nothing, for tests that need a real bitstream and no game video.
 * 32x16 pictures of 4x4 solid-fill blocks, 6 frames at 100 ms, per-frame palette updates (new entries, skips and copies,
 * or none), a 16-bit mono 22050 Hz track 0 and an 8-bit mono 11025 Hz track 2, both uncompressed. The expected pixels,
 * palettes and samples come from the functions below, not from any decoder. Plain C11, no library dependency. */
#ifndef TRESPASSER_TESTS_SMK_FIXTURE_H
#define TRESPASSER_TESTS_SMK_FIXTURE_H

#include <stdint.h>
#include <string.h>

#define SMKFX_WIDTH 32u
#define SMKFX_HEIGHT 16u
#define SMKFX_FRAMES 6u
#define SMKFX_FRAME_RATE 100
#define SMKFX_BLOCKS ((SMKFX_WIDTH / 4u) * (SMKFX_HEIGHT / 4u))
#define SMKFX_TRACK_WIDE 0u /* 16 bits, 22050 Hz */
#define SMKFX_TRACK_NARROW 2u /* 8 bits, 11025 Hz */
#define SMKFX_RATE_WIDE 22050u
#define SMKFX_RATE_NARROW 11025u
#define SMKFX_WIDE_SAMPLES_PER_FRAME 2205u
#define SMKFX_NARROW_SAMPLES_PER_FRAME 1102u
#define SMKFX_MAX_BYTES 65536u
#define SMKFX_HEADER_BYTES 104u
#define SMKFX_TREE_BYTES_OFFSET 52u

/* ---- the content the file must decode to ------------------------------------------------------------------------ */

static inline uint8_t smkfx_pixel(unsigned frame, unsigned x, unsigned y) {
    const unsigned block = (y / 4u) * (SMKFX_WIDTH / 4u) + x / 4u;
    return (uint8_t)(frame * 41u + block * 7u + 11u);
}

static inline uint16_t smkfx_sample_wide(uint32_t i) { return (uint16_t)(i * 7u + 1000u); }
static inline uint8_t smkfx_sample_narrow(uint32_t i) { return (uint8_t)(i * 3u + 17u); }

/* ---- byte and bit writers --------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t* data;
    uint32_t size, capacity;
    int overflow;
} smkfx_buf_t;

static inline void smkfx_u8(smkfx_buf_t* b, uint8_t v) {
    if (b->size < b->capacity) b->data[b->size++] = v;
    else b->overflow = 1;
}

static inline void smkfx_u32(smkfx_buf_t* b, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) smkfx_u8(b, (uint8_t)(v >> (8 * i)));
}

static inline void smkfx_put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint8_t bytes[2048];
    uint32_t bits;
} smkfx_bits_t;

/* The decoder reads bits from the least significant end of each byte. */
static inline void smkfx_bit(smkfx_bits_t* t, unsigned v) {
    if (t->bits < sizeof t->bytes * 8u && (v & 1u)) t->bytes[t->bits / 8u] = (uint8_t)(t->bytes[t->bits / 8u] | (1u << (t->bits % 8u)));
    t->bits++;
}

static inline void smkfx_bits_lsb_first(smkfx_bits_t* t, unsigned value, unsigned count) {
    for (unsigned i = 0; i < count; i++) smkfx_bit(t, (value >> i) & 1u);
}

/* A Huffman code is the path from the root, first branch first: for a complete tree the leaf index, top bit first. */
static inline void smkfx_code(smkfx_bits_t* t, unsigned leaf) {
    for (int i = 7; i >= 0; i--) smkfx_bit(t, (leaf >> i) & 1u);
}

/* ---- the trees: only the block-type tree is used, 256 leaves of equal depth ------------------------------------- */

static inline void smkfx_small_tree(smkfx_bits_t* t, unsigned depth, unsigned* next) {
    if (!depth) {
        smkfx_bit(t, 0);
        smkfx_bits_lsb_first(t, (*next)++, 8);
        return;
    }
    smkfx_bit(t, 1);
    smkfx_small_tree(t, depth - 1u, next);
    smkfx_small_tree(t, depth - 1u, next);
}

static inline void smkfx_big_tree(smkfx_bits_t* t, unsigned depth, unsigned* next) {
    if (!depth) {
        smkfx_bit(t, 0);
        smkfx_code(t, (*next)++);
        return;
    }
    smkfx_bit(t, 1);
    smkfx_big_tree(t, depth - 1u, next);
    smkfx_big_tree(t, depth - 1u, next);
}

/* Block types are 16-bit: low byte 0x03 (solid fill, run of one block), high byte the colour. */
static inline uint32_t smkfx_trees(smkfx_bits_t* t) {
    unsigned next = 0;
    memset(t, 0, sizeof *t);
    smkfx_bit(t, 0); /* no MMAP, MCLR or FULL tree */
    smkfx_bit(t, 0);
    smkfx_bit(t, 0);
    smkfx_bit(t, 1);
    smkfx_bit(t, 1); /* low byte: a single leaf 0x03 */
    smkfx_bit(t, 0);
    smkfx_bits_lsb_first(t, 0x03u, 8);
    smkfx_bit(t, 0);
    smkfx_bit(t, 1); /* high byte: leaf i is the byte i */
    smkfx_small_tree(t, 8, &next);
    smkfx_bit(t, 0);
    for (unsigned i = 0; i < 3; i++) smkfx_bits_lsb_first(t, 0xFFFFu, 16); /* escape codes no leaf uses */
    next = 0;
    smkfx_big_tree(t, 8, &next);
    smkfx_bit(t, 0);
    return (t->bits + 31u) / 32u * 4u;
}

/* ---- palettes --------------------------------------------------------------------------------------------------- */

/* The 6-bit levels of a palette entry map to 8 bits by the table Smacker defines. */
static inline uint8_t smkfx_level(unsigned v) { return (uint8_t)(v * 4u + v / 16u); }

static inline void smkfx_pal_new(unsigned frame, uint8_t* pal, unsigned* at, unsigned count, smkfx_buf_t* chunk) {
    for (unsigned n = 0; n < count; n++, (*at)++) {
        const unsigned r = ((*at) * 3u + frame * 5u + 1u) & 63u, g = ((*at) * 5u + frame * 7u + 2u) & 63u, b = ((*at) * 7u + frame * 11u + 3u) & 63u;
        if (chunk) {
            smkfx_u8(chunk, (uint8_t)r);
            smkfx_u8(chunk, (uint8_t)g);
            smkfx_u8(chunk, (uint8_t)b);
        }
        pal[*at * 3u] = smkfx_level(r);
        pal[*at * 3u + 1u] = smkfx_level(g);
        pal[*at * 3u + 2u] = smkfx_level(b);
    }
}

static inline void smkfx_pal_skip(unsigned* at, unsigned count, smkfx_buf_t* chunk) {
    if (chunk) smkfx_u8(chunk, (uint8_t)(0x80u | (count - 1u)));
    *at += count;
}

static inline void smkfx_pal_copy(const uint8_t* old, uint8_t* pal, unsigned* at, unsigned from, unsigned count, smkfx_buf_t* chunk) {
    if (chunk) {
        smkfx_u8(chunk, (uint8_t)(0x40u | (count - 1u)));
        smkfx_u8(chunk, (uint8_t)from);
    }
    memcpy(pal + *at * 3u, old + from * 3u, count * 3u);
    *at += count;
}

/* Applies the palette update of `frame` to `pal` (768 RGB bytes) and, when `chunk` is given, writes it as the file stores
 * it. Returns 0 for a frame without an update. */
static inline int smkfx_palette_update(unsigned frame, uint8_t* pal, smkfx_buf_t* chunk) {
    uint8_t old[768];
    unsigned at = 0;
    memcpy(old, pal, sizeof old);
    if (chunk) smkfx_u8(chunk, 0);
    switch (frame) {
    case 0:
    case 3:
        smkfx_pal_new(frame, pal, &at, 256, chunk);
        break;
    case 2:
        smkfx_pal_skip(&at, 64, chunk);
        smkfx_pal_new(frame, pal, &at, 64, chunk);
        smkfx_pal_copy(old, pal, &at, 0, 64, chunk);
        smkfx_pal_skip(&at, 64, chunk);
        break;
    case 5:
        smkfx_pal_copy(old, pal, &at, 192, 64, chunk);
        smkfx_pal_copy(old, pal, &at, 128, 64, chunk);
        smkfx_pal_new(frame, pal, &at, 64, chunk);
        smkfx_pal_skip(&at, 64, chunk);
        break;
    default:
        if (chunk) chunk->size = 0;
        return 0;
    }
    if (chunk) {
        while (chunk->size % 4u) smkfx_u8(chunk, 0);
        chunk->data[0] = (uint8_t)(chunk->size / 4u);
    }
    return 1;
}

/* The palette in force while `frame` is shown. */
static inline void smkfx_palette(unsigned frame, uint8_t* pal) {
    memset(pal, 0, 768);
    for (unsigned f = 0; f <= frame; f++) smkfx_palette_update(f, pal, NULL);
}

/* ---- the file --------------------------------------------------------------------------------------------------- */

static inline uint32_t smkfx_wide_bytes(void) { return SMKFX_WIDE_SAMPLES_PER_FRAME * 2u; }

/* Writes the file into `out`; returns its size, 0 when `capacity` is too small. */
static inline uint32_t smkfx_build(uint8_t* out, uint32_t capacity) {
    smkfx_bits_t trees;
    smkfx_buf_t file;
    uint32_t sizes[SMKFX_FRAMES], tree_bytes = smkfx_trees(&trees), data_start;
    uint8_t types[SMKFX_FRAMES], pal[768] = { 0 };
    uint32_t wide_at = 0, narrow_at = 0;
    file.data = out;
    file.size = 0;
    file.capacity = capacity;
    file.overflow = 0;
    data_start = SMKFX_HEADER_BYTES + SMKFX_FRAMES * 5u + tree_bytes;
    if (data_start > capacity) return 0;
    memset(out, 0, data_start);
    file.size = data_start;
    for (unsigned f = 0; f < SMKFX_FRAMES; f++) {
        uint8_t chunk_bytes[800];
        smkfx_buf_t chunk;
        smkfx_bits_t picture;
        const uint32_t begin = file.size;
        chunk.data = chunk_bytes;
        chunk.size = 0;
        chunk.capacity = sizeof chunk_bytes;
        chunk.overflow = 0;
        types[f] = (uint8_t)(0x02u | 0x08u);
        if (smkfx_palette_update(f, pal, &chunk)) {
            types[f] |= 1u;
            for (uint32_t i = 0; i < chunk.size; i++) smkfx_u8(&file, chunk_bytes[i]);
        }
        smkfx_u32(&file, 4u + SMKFX_WIDE_SAMPLES_PER_FRAME * 2u);
        for (uint32_t i = 0; i < SMKFX_WIDE_SAMPLES_PER_FRAME; i++, wide_at++) {
            const uint16_t s = smkfx_sample_wide(wide_at);
            smkfx_u8(&file, (uint8_t)s);
            smkfx_u8(&file, (uint8_t)(s >> 8));
        }
        smkfx_u32(&file, 4u + SMKFX_NARROW_SAMPLES_PER_FRAME);
        for (uint32_t i = 0; i < SMKFX_NARROW_SAMPLES_PER_FRAME; i++, narrow_at++) smkfx_u8(&file, smkfx_sample_narrow(narrow_at));
        memset(&picture, 0, sizeof picture);
        for (unsigned block = 0; block < SMKFX_BLOCKS; block++)
            smkfx_code(&picture, smkfx_pixel(f, (block % (SMKFX_WIDTH / 4u)) * 4u, (block / (SMKFX_WIDTH / 4u)) * 4u));
        for (uint32_t i = 0; i < (picture.bits + 7u) / 8u; i++) smkfx_u8(&file, picture.bytes[i]);
        while ((file.size - begin) % 4u) smkfx_u8(&file, 0);
        sizes[f] = (file.size - begin) | ((f == 0 || f == 3) ? 1u : 0u);
    }
    if (file.overflow) return 0;
    memcpy(out, "SMK2", 4);
    smkfx_put32(out + 4, SMKFX_WIDTH);
    smkfx_put32(out + 8, SMKFX_HEIGHT);
    smkfx_put32(out + 12, SMKFX_FRAMES);
    smkfx_put32(out + 16, (uint32_t)SMKFX_FRAME_RATE);
    smkfx_put32(out + 24 + 4u * SMKFX_TRACK_WIDE, smkfx_wide_bytes());
    smkfx_put32(out + 24 + 4u * SMKFX_TRACK_NARROW, SMKFX_NARROW_SAMPLES_PER_FRAME);
    smkfx_put32(out + SMKFX_TREE_BYTES_OFFSET, tree_bytes);
    smkfx_put32(out + 56, 0);
    smkfx_put32(out + 60, 0);
    smkfx_put32(out + 64, 0);
    smkfx_put32(out + 68, 511u * 4u); /* entries of the block-type tree: 256 leaves and 255 nodes */
    smkfx_put32(out + 72 + 4u * SMKFX_TRACK_WIDE, 0x40000000u | 0x20000000u | SMKFX_RATE_WIDE);
    smkfx_put32(out + 72 + 4u * SMKFX_TRACK_NARROW, 0x40000000u | SMKFX_RATE_NARROW);
    for (unsigned f = 0; f < SMKFX_FRAMES; f++) {
        smkfx_put32(out + SMKFX_HEADER_BYTES + 4u * f, sizes[f]);
        out[SMKFX_HEADER_BYTES + 4u * SMKFX_FRAMES + f] = types[f];
    }
    memcpy(out + SMKFX_HEADER_BYTES + 5u * SMKFX_FRAMES, trees.bytes, tree_bytes);
    return file.size;
}

/* Where frame `f` starts in a file built by smkfx_build. */
static inline uint32_t smkfx_frame_start(const uint8_t* file, unsigned f) {
    uint32_t at = SMKFX_HEADER_BYTES + SMKFX_FRAMES * 5u + ((uint32_t)file[SMKFX_TREE_BYTES_OFFSET] | (uint32_t)file[SMKFX_TREE_BYTES_OFFSET + 1] << 8);
    for (unsigned i = 0; i < f; i++) {
        const uint8_t* p = file + SMKFX_HEADER_BYTES + 4u * i;
        at += ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) & ~3u;
    }
    return at;
}

#endif
