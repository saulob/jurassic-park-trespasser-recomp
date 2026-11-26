#include "jpt_picture.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"

#define FILE_MAX (96u << 20)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define DDS_HEADER 128u
#define DDS_FOURCC 0x4u
#define DDS_RGB 0x40u
#define TGA_HEADER 18u
#define BMP_HEADER 54u
#define CUTOUT_ALPHA 128u

typedef struct {
    const uint8_t* p;
    size_t size;
} bytes_t;

static uint32_t le(const bytes_t* b, size_t at, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count && at + (size_t)i < b->size; i++) v |= (uint32_t)b->p[at + (size_t)i] << (8 * i);
    return v;
}

static size_t level_bytes(uint32_t w, uint32_t h) { return (size_t)w * h * 4u; }
static uint32_t half(uint32_t side) { return side > 1 ? side / 2 : 1; }

static int levels_of(uint32_t w, uint32_t h) {
    int n = 1;
    for (; w > 1 || h > 1; n++) w = half(w), h = half(h);
    return n;
}

static size_t chain_bytes(uint32_t w, uint32_t h, int levels) {
    size_t total = 0;
    for (int i = 0; i < levels; i++, w = half(w), h = half(h)) total += level_bytes(w, h);
    return total;
}

static int begin(jpt_picture_t* out, uint32_t w, uint32_t h, int levels) {
    if (!w || !h || w > JPT_PICTURE_SIDE_MAX || h > JPT_PICTURE_SIDE_MAX) return 0;
    out->width = w;
    out->height = h;
    out->levels = levels;
    out->size = chain_bytes(w, h, levels);
    out->bgra = (uint8_t*)malloc(out->size);
    return out->bgra != NULL;
}

static void put(uint8_t* d, uint32_t b, uint32_t g, uint32_t r, uint32_t a) {
    d[0] = (uint8_t)b;
    d[1] = (uint8_t)g;
    d[2] = (uint8_t)r;
    d[3] = (uint8_t)a;
}

/* ---- BMP ---- */

/* A channel of a BI_BITFIELDS mask, scaled to 8 bits. */
static uint32_t bmp_channel(uint32_t value, uint32_t mask) {
    uint32_t shift = 0, bits = 0;
    if (!mask) return 255u;
    while (!(mask >> shift & 1u)) shift++;
    while (shift + bits < 32u && (mask >> (shift + bits) & 1u)) bits++;
    value = (value & mask) >> shift;
    return bits >= 8u ? value >> (bits - 8u) : value * 255u / ((1u << bits) - 1u);
}

/* Every uncompressed layout GDI draws: the 12-byte OS/2 header and the 40-byte one or later, 1 to 32 bits. */
static int decode_bmp(const bytes_t* b, jpt_picture_t* out) {
    const uint32_t pixels = le(b, 0x0A, 4), header = le(b, 0x0E, 4);
    const int core = header == 12u;
    const uint32_t width = core ? le(b, 0x12, 2) : le(b, 0x12, 4), bits = le(b, core ? 0x18 : 0x1C, 2), packing = core ? 0u : le(b, 0x1E, 4);
    uint32_t height = core ? le(b, 0x14, 2) : le(b, 0x16, 4), colours = core ? 0u : le(b, 0x2E, 4);
    uint32_t mask[4] = { 0x7C00u, 0x3E0u, 0x1Fu, 0u };
    const int top_down = !core && height > 0x80000000u, indexed = bits <= 8u;
    const size_t entry = core ? 3u : 4u;
    size_t palette = 14u + (size_t)header, stride;
    if (b->size < 26u || (!core && (b->size < BMP_HEADER || header < 40u || header > 256u))) return 0;
    if (top_down) height = 0u - height;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return 0;
    if (packing && !(packing == 3 && (bits == 16 || bits == 32))) return 0;
    if (packing == 3) {
        for (int i = 0; i < 3; i++) mask[i] = le(b, 0x36 + 4u * (size_t)i, 4);
        mask[3] = header >= 56u ? le(b, 0x42, 4) : bits == 32 ? ~(mask[0] | mask[1] | mask[2]) & 0xFF000000u : 0u;
        if (header == 40u) palette += 12u;
        if (!mask[0] || !mask[1] || !mask[2] || palette > b->size) return 0;
    }
    if (indexed) {
        if (!colours || colours > (1u << bits)) colours = 1u << bits;
        if (palette > b->size || b->size - palette < (size_t)colours * entry) return 0;
    }
    if (!width || width > JPT_PICTURE_SIDE_MAX || !height || height > JPT_PICTURE_SIDE_MAX) return 0;
    stride = (((size_t)width * bits + 31u) / 32u) * 4u;
    if (pixels > b->size || b->size - pixels < stride * height || !begin(out, width, height, 1)) return 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* s = b->p + pixels + stride * (top_down ? y : height - 1u - y);
        uint8_t* d = out->bgra + level_bytes(width, y);
        for (uint32_t x = 0; x < width; x++, d += 4) {
            if (indexed) {
                const size_t at = (size_t)x * bits;
                const uint32_t index = (uint32_t)(s[at / 8u] >> (8u - bits - (uint32_t)(at % 8u))) & ((1u << bits) - 1u);
                const size_t colour = palette + entry * (size_t)(index < colours ? index : 0u);
                put(d, b->p[colour], b->p[colour + 1], b->p[colour + 2], 255);
            } else if (bits == 24 || (bits == 32 && !packing)) {
                const uint8_t* q = s + (size_t)x * (bits / 8u);
                put(d, q[0], q[1], q[2], bits == 32 ? q[3] : 255u);
            } else {
                const uint8_t* q = s + (size_t)x * (bits / 8u);
                const uint32_t v = bits == 16 ? (uint32_t)q[0] | (uint32_t)q[1] << 8 : (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
                put(d, bmp_channel(v, mask[2]), bmp_channel(v, mask[1]), bmp_channel(v, mask[0]), bmp_channel(v, mask[3]));
            }
        }
    }
    return 1;
}

/* A 32 bit BMP without an alpha channel has zeros there. */
static void opaque_when_empty(jpt_picture_t* out) {
    for (size_t i = 3; i < out->size; i += 4)
        if (out->bgra[i]) return;
    for (size_t i = 3; i < out->size; i += 4) out->bgra[i] = 255;
}

/* ---- TGA ---- */

typedef struct {
    uint32_t type, bits, map_first, map_count, map_bits, alpha_bits;
    const uint8_t* map;
} tga_t;

static int tga_colour(const tga_t* t, const uint8_t* s, uint32_t bits, uint8_t* d) {
    switch (bits) {
    case 8: put(d, s[0], s[0], s[0], 255); return 1;
    case 15:
    case 16: {
        const uint32_t v = (uint32_t)s[0] | (uint32_t)s[1] << 8, r = v >> 10 & 31u, g = v >> 5 & 31u, b = v & 31u;
        put(d, b << 3 | b >> 2, g << 3 | g >> 2, r << 3 | r >> 2, bits == 16 && t->alpha_bits ? (v >> 15) * 255u : 255u);
        return 1;
    }
    case 24: put(d, s[0], s[1], s[2], 255); return 1;
    case 32: put(d, s[0], s[1], s[2], s[3]); return 1;
    default: return 0;
    }
}

static int tga_pixel(const tga_t* t, const uint8_t* s, uint8_t* d) {
    uint32_t index;
    if ((t->type & 7u) != 1) return tga_colour(t, s, t->bits, d);
    index = t->bits == 8 ? s[0] : (uint32_t)s[0] | (uint32_t)s[1] << 8;
    index = index >= t->map_first && index - t->map_first < t->map_count ? index - t->map_first : 0;
    return tga_colour(t, t->map + (size_t)index * ((t->map_bits + 7u) / 8u), t->map_bits, d);
}

static int decode_tga(const bytes_t* b, jpt_picture_t* out) {
    const uint32_t width = le(b, 12, 2), height = le(b, 14, 2), described = le(b, 17, 1);
    tga_t t = { le(b, 2, 1), le(b, 16, 1), le(b, 3, 2), le(b, 5, 2), le(b, 7, 1), described & 15u, NULL };
    const uint32_t base = t.type & 7u, step = (t.bits + 7u) / 8u;
    const int packed = (t.type & 8u) != 0;
    size_t at = TGA_HEADER + (size_t)le(b, 0, 1), map_bytes = le(b, 1, 1) ? (size_t)t.map_count * ((t.map_bits + 7u) / 8u) : 0;
    uint64_t done = 0, total;
    if (b->size < TGA_HEADER || (t.type & ~8u) != base || base < 1 || base > 3) return 0;
    if (base == 1 && ((t.bits != 8 && t.bits != 16) || le(b, 1, 1) != 1 || !t.map_count || (t.map_bits != 15 && t.map_bits != 16 && t.map_bits != 24 && t.map_bits != 32))) return 0;
    if (base == 2 && t.bits != 15 && t.bits != 16 && t.bits != 24 && t.bits != 32) return 0;
    if (base == 3 && t.bits != 8) return 0;
    if (!width || width > JPT_PICTURE_SIDE_MAX || !height || height > JPT_PICTURE_SIDE_MAX) return 0;
    if (at > b->size || map_bytes > b->size - at) return 0;
    t.map = b->p + at;
    at += map_bytes;
    total = (uint64_t)width * height;
    if (!packed && b->size - at < total * step) return 0;
    if (packed && (uint64_t)(b->size - at) * 128u < total) return 0;
    if (!begin(out, width, height, 1)) return 0;
    while (done < total) {
        uint32_t run = 1;
        int repeat = 0;
        if (packed) {
            if (at >= b->size) return 0;
            run = (b->p[at] & 127u) + 1u;
            repeat = (b->p[at++] & 128u) != 0;
        }
        for (uint32_t i = 0; i < run && done < total; i++, done++) {
            const uint32_t x = (uint32_t)(done % width), y = (uint32_t)(done / width);
            uint8_t* d = out->bgra + level_bytes(width, described & 0x20u ? y : height - 1u - y) + 4u * (size_t)(described & 0x10u ? width - 1u - x : x);
            if (b->size - at < step || !tga_pixel(&t, b->p + at, d)) return 0;
            if (!repeat || i + 1 == run || done + 1 == total) at += step;
        }
    }
    return 1;
}

/* ---- DDS ---- */

static void expand565(uint32_t v, uint32_t c[3]) {
    const uint32_t r = v >> 11 & 31u, g = v >> 5 & 63u, b = v & 31u;
    c[0] = b << 3 | b >> 2;
    c[1] = g << 2 | g >> 4;
    c[2] = r << 3 | r >> 2;
}

static void block_colours(const uint8_t* s, int dxt1, uint8_t out[16][4]) {
    const uint32_t v0 = (uint32_t)s[0] | (uint32_t)s[1] << 8, v1 = (uint32_t)s[2] | (uint32_t)s[3] << 8;
    const int four = !dxt1 || v0 > v1;
    uint32_t c[4][3], bits = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    expand565(v0, c[0]);
    expand565(v1, c[1]);
    for (int i = 0; i < 3; i++) {
        c[2][i] = four ? (2 * c[0][i] + c[1][i]) / 3 : (c[0][i] + c[1][i]) / 2;
        c[3][i] = four ? (c[0][i] + 2 * c[1][i]) / 3 : 0;
    }
    for (int i = 0; i < 16; i++, bits >>= 2) {
        const uint32_t k = bits & 3u;
        put(out[i], c[k][0], c[k][1], c[k][2], four || k != 3 ? 255u : 0);
    }
}

static void block_alpha_explicit(const uint8_t* s, uint8_t out[16][4]) {
    for (int i = 0; i < 16; i++) out[i][3] = (uint8_t)((s[i / 2] >> (i & 1 ? 4 : 0) & 15u) * 17u);
}

static void block_alpha_interpolated(const uint8_t* s, uint8_t out[16][4]) {
    uint32_t a[8] = { s[0], s[1], 0, 0, 0, 0, 0, 255 };
    uint64_t bits = 0;
    const int eight = a[0] > a[1];
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (uint32_t i = 1; i < (eight ? 7u : 5u); i++) a[1 + i] = eight ? ((7 - i) * a[0] + i * a[1]) / 7 : ((5 - i) * a[0] + i * a[1]) / 5;
    for (int i = 0; i < 16; i++, bits >>= 3) out[i][3] = (uint8_t)a[bits & 7u];
}

static void unpremultiply(uint8_t p[4]) {
    for (int i = 0; i < 3 && p[3]; i++) {
        const uint32_t v = ((uint32_t)p[i] * 255u + p[3] / 2u) / p[3];
        p[i] = (uint8_t)(v < 255u ? v : 255u);
    }
}

static void decode_blocks(const uint8_t* s, uint32_t w, uint32_t h, int kind, uint8_t* out) {
    for (uint32_t y = 0; y < h; y += 4)
        for (uint32_t x = 0; x < w; x += 4) {
            uint8_t block[16][4];
            block_colours(kind == 1 ? s : s + 8, kind == 1, block);
            if (kind == 2 || kind == 3) block_alpha_explicit(s, block);
            if (kind == 4 || kind == 5) block_alpha_interpolated(s, block);
            s += kind == 1 ? 8 : 16;
            for (uint32_t i = 0; i < 16; i++) {
                const uint32_t px = x + i % 4, py = y + i / 4;
                if (kind == 2 || kind == 4) unpremultiply(block[i]);
                if (px < w && py < h) memcpy(out + level_bytes(w, py) + (size_t)px * 4u, block[i], 4);
            }
        }
}

typedef struct {
    uint32_t shift, most;
} channel_t;

static int channel(uint32_t mask, channel_t* c) {
    c->shift = 0;
    c->most = mask;
    if (!mask) return 1;
    for (; !(c->most & 1u); c->most >>= 1) c->shift++;
    return !(c->most & (c->most + 1u));
}

static uint32_t channel_of(const channel_t* c, uint32_t v, uint32_t absent) {
    return c->most ? (uint32_t)(((uint64_t)(v >> c->shift & c->most) * 255u + c->most / 2u) / c->most) : absent;
}

static int decode_dds(const bytes_t* b, jpt_picture_t* out) {
    const uint32_t width = le(b, 16, 4), height = le(b, 12, 4), flags = le(b, 80, 4), fourcc = le(b, 84, 4), bits = le(b, 88, 4);
    uint32_t count = le(b, 28, 4), w = width, h = height;
    size_t at = DDS_HEADER, to = 0;
    channel_t c[4];
    int kind = 0, levels, whole;
    if (b->size < DDS_HEADER || le(b, 4, 4) != 124u || le(b, 76, 4) != 32u) return 0;
    if (!width || width > JPT_PICTURE_SIDE_MAX || !height || height > JPT_PICTURE_SIDE_MAX) return 0;
    if (flags & DDS_FOURCC) {
        for (int i = 1; i <= 5; i++)
            if (fourcc == FOURCC('D', 'X', 'T', '0' + i)) kind = i;
        if (!kind) return 0;
    } else {
        if (!(flags & DDS_RGB) || (bits != 16 && bits != 24 && bits != 32)) return 0;
        for (size_t i = 0; i < 4; i++)
            if (!channel(le(b, 92 + 4 * i, 4), &c[i])) return 0;
        if (!(flags & 1u)) c[3].most = 0;
    }
    whole = levels_of(width, height);
    levels = !(le(b, 8, 4) & 0x20000u) || !count ? 1 : count < (uint32_t)whole ? (int)count : whole;
    for (int i = 0; i < levels; i++, w = half(w), h = half(h)) {
        const size_t size = kind ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (kind == 1 ? 8u : 16u) : (size_t)w * h * (bits / 8u);
        if (b->size - at < size) {
            if (!i) return 0;
            levels = i;
            break;
        }
        at += size;
    }
    if (!begin(out, width, height, levels)) return 0;
    for (w = width, h = height, at = DDS_HEADER; levels--; w = half(w), h = half(h)) {
        if (kind) {
            decode_blocks(b->p + at, w, h, kind, out->bgra + to);
            at += (size_t)((w + 3) / 4) * ((h + 3) / 4) * (kind == 1 ? 8u : 16u);
        } else {
            for (size_t i = 0; i < (size_t)w * h; i++, at += bits / 8u) {
                const uint32_t v = le(b, at, (int)bits / 8);
                put(out->bgra + to + 4 * i, channel_of(&c[2], v, 0), channel_of(&c[1], v, 0), channel_of(&c[0], v, 0), channel_of(&c[3], v, 255));
            }
        }
        to += level_bytes(w, h);
    }
    return 1;
}

int jpt_picture_decode(const void* bytes, size_t size, jpt_picture_t* out) {
    const bytes_t b = { (const uint8_t*)bytes, size };
    int ok;
    memset(out, 0, sizeof *out);
    if (size < 4) return 0;
    if (!memcmp(bytes, "DDS ", 4)) ok = decode_dds(&b, out);
    else if (!memcmp(bytes, "BM", 2)) {
        ok = decode_bmp(&b, out);
        if (ok) opaque_when_empty(out);
    } else ok = decode_tga(&b, out);
    if (!ok) {
        jpt_picture_free(out);
        return 0;
    }
    for (size_t i = 3; i < level_bytes(out->width, out->height) && !out->has_alpha; i += 4) out->has_alpha = out->bgra[i] != 255;
    return 1;
}

int jpt_picture_load(const char* host_path, jpt_picture_t* out) {
    uint64_t size = 0;
    jpt_fs_file_t* f = jpt_fs_open_read(host_path, &size);
    uint8_t* bytes = f && size && size <= FILE_MAX ? (uint8_t*)malloc((size_t)size) : NULL;
    int ok = bytes && jpt_fs_read_at(f, 0, bytes, (size_t)size);
    if (f) jpt_fs_close(f);
    memset(out, 0, sizeof *out);
    if (ok) ok = jpt_picture_decode(bytes, (size_t)size, out);
    free(bytes);
    return ok;
}

void jpt_picture_free(jpt_picture_t* picture) {
    free(picture->bgra);
    memset(picture, 0, sizeof *picture);
}

/* Colours weigh by their alpha, so that what shows through does not bleed into a level. */
static void shrink(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    const uint32_t dw = half(w), dh = half(h);
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++, d += 4) {
            uint32_t sum[4] = { 0, 0, 0, 0 }, plain[3] = { 0, 0, 0 };
            for (uint32_t i = 0; i < 4; i++) {
                const uint32_t sx = 2 * x + (i & 1) < w ? 2 * x + (i & 1) : w - 1, sy = 2 * y + i / 2 < h ? 2 * y + i / 2 : h - 1;
                const uint8_t* q = s + level_bytes(w, sy) + (size_t)sx * 4u;
                for (int k = 0; k < 3; k++) sum[k] += (uint32_t)q[k] * q[3], plain[k] += q[k];
                sum[3] += q[3];
            }
            for (int k = 0; k < 3; k++) d[k] = (uint8_t)(sum[3] ? (sum[k] + sum[3] / 2) / sum[3] : (plain[k] + 2) / 4);
            d[3] = (uint8_t)((sum[3] + 2) / 4);
        }
}

static void cut(uint8_t* p, size_t size) {
    for (size_t i = 3; i < size; i += 4) p[i] = p[i] >= CUTOUT_ALPHA ? 255 : 0;
}

int jpt_picture_complete(jpt_picture_t* picture, int cutout) {
    const int whole = levels_of(picture->width, picture->height), had = picture->levels;
    uint32_t w = picture->width, h = picture->height;
    size_t at = 0;
    if (had < whole) {
        const size_t size = chain_bytes(w, h, whole);
        uint8_t* grown = (uint8_t*)realloc(picture->bgra, size);
        if (!grown) return 0;
        picture->bgra = grown;
        picture->size = size;
        picture->levels = whole;
    }
    for (int i = 1; i < whole; i++, w = half(w), h = half(h)) {
        const size_t next = at + level_bytes(w, h);
        if (i >= had) shrink(picture->bgra + at, w, h, picture->bgra + next);
        at = next;
    }
    if (cutout) cut(picture->bgra, picture->size);
    return 1;
}

const uint8_t* jpt_picture_level(const jpt_picture_t* picture, int level, uint32_t* width, uint32_t* height) {
    uint32_t w = picture->width, h = picture->height;
    size_t at = 0;
    if (level < 0 || level >= picture->levels) return NULL;
    for (int i = 0; i < level; i++, w = half(w), h = half(h)) at += level_bytes(w, h);
    *width = w;
    *height = h;
    return picture->bgra + at;
}

void jpt_picture_limit(jpt_picture_t* picture, uint32_t side) {
    size_t skip = 0;
    while (picture->levels > 1 && (picture->width > side || picture->height > side)) {
        skip += level_bytes(picture->width, picture->height);
        picture->width = half(picture->width);
        picture->height = half(picture->height);
        picture->levels--;
    }
    if (!skip) return;
    picture->size -= skip;
    memmove(picture->bgra, picture->bgra + skip, picture->size);
}
