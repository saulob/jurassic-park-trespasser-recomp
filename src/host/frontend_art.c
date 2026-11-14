#include "frontend_art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_SLOTS 256
#define DEDITHER_STEP 0.045f
#define EDGE_LOW 0.25f
#define EDGE_HIGH 0.75f

typedef struct {
    int* first;
    int* count;
    float* weights;
    int stride;
    int lo, hi; /* source rows or columns any output touches */
} axis_t;

static float catmull_rom(float x) {
    x = fabsf(x);
    if (x < 1.0f) return (1.5f * x - 2.5f) * x * x + 1.0f;
    if (x < 2.0f) return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    return 0.0f;
}

static void axis_free(axis_t* a) {
    free(a->first);
    free(a->count);
    free(a->weights);
    memset(a, 0, sizeof *a);
}

/* Output pixel i takes the source around s0 + (i + 0.5) * scale; a shrink widens the filter so no texel is skipped. */
static int axis_make(axis_t* a, int dst, int src, double s0, double s1) {
    double scale = (s1 - s0) / dst, fs = scale > 1.0 ? scale : 1.0;
    memset(a, 0, sizeof *a);
    a->stride = (int)ceil(4.0 * fs) + 4;
    a->first = (int*)malloc(sizeof(int) * (size_t)dst);
    a->count = (int*)malloc(sizeof(int) * (size_t)dst);
    a->weights = (float*)malloc(sizeof(float) * (size_t)dst * (size_t)a->stride);
    if (!a->first || !a->count || !a->weights) {
        axis_free(a);
        return 0;
    }
    a->lo = src - 1;
    a->hi = 0;
    for (int i = 0; i < dst; i++) {
        double c = s0 + (i + 0.5) * scale, sum = 0.0;
        int lo = (int)floor(c - 2.0 * fs), hi = (int)ceil(c + 2.0 * fs), n = hi - lo + 1;
        float* w = a->weights + (size_t)i * (size_t)a->stride;
        if (n > a->stride) n = a->stride;
        for (int t = 0; t < n; t++) {
            float k = catmull_rom((float)((lo + t + 0.5 - c) / fs));
            w[t] = k;
            sum += k;
        }
        if (sum <= 1e-9) {
            w[0] = 1.0f;
            n = 1;
            sum = 1.0;
        }
        for (int t = 0; t < n; t++) w[t] = (float)(w[t] / sum);
        a->first[i] = lo;
        a->count[i] = n;
        {
            int a0 = lo < 0 ? 0 : lo > src - 1 ? src - 1 : lo, a1 = lo + n - 1;
            a1 = a1 < 0 ? 0 : a1 > src - 1 ? src - 1 : a1;
            if (a0 < a->lo) a->lo = a0;
            if (a1 > a->hi) a->hi = a1;
        }
    }
    return 1;
}

static int clamp_index(int i, int n) { return i < 0 ? 0 : i > n - 1 ? n - 1 : i; }

static float smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

static uint32_t pack(float b, float g, float r, float a) {
    int ib = (int)(b * 255.0f + 0.5f), ig = (int)(g * 255.0f + 0.5f), ir = (int)(r * 255.0f + 0.5f),
        ia = (int)(a * 255.0f + 0.5f);
    if (ia > 255) ia = 255;
    if (ia < 0) ia = 0;
    ib = ib < 0 ? 0 : ib > ia ? ia : ib;
    ig = ig < 0 ? 0 : ig > ia ? ia : ig;
    ir = ir < 0 ? 0 : ir > ia ? ia : ir;
    return (uint32_t)ib | ((uint32_t)ig << 8) | ((uint32_t)ir << 16) | ((uint32_t)ia << 24);
}

/* src: w x h premultiplied RGBA floats. */
static int resample_float(const float* src, int w, int h, const double* part, int dst_w, int dst_h, unsigned flags,
                          fa_bitmap_t* out) {
    double p[4] = { 0, 0, (double)w, (double)h };
    axis_t ax, ay;
    float* tmp = NULL;
    float* row = NULL;
    uint32_t* px = NULL;
    int rows, ok = 0;
    if (part) memcpy(p, part, sizeof p);
    if (dst_w <= 0 || dst_h <= 0 || p[2] - p[0] <= 0.0 || p[3] - p[1] <= 0.0 || dst_w > 32768 || dst_h > 32768) return 0;
    memset(&ax, 0, sizeof ax);
    memset(&ay, 0, sizeof ay);
    if (!axis_make(&ax, dst_w, w, p[0], p[2]) || !axis_make(&ay, dst_h, h, p[1], p[3])) goto done;
    rows = ay.hi - ay.lo + 1;
    tmp = (float*)malloc(sizeof(float) * 4 * (size_t)dst_w * (size_t)rows);
    row = (float*)malloc(sizeof(float) * 4 * (size_t)dst_w);
    px = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)dst_w * (size_t)dst_h);
    if (!tmp || !row || !px) goto done;
    for (int y = 0; y < rows; y++) {
        const float* in = src + (size_t)(ay.lo + y) * (size_t)w * 4;
        float* o = tmp + (size_t)y * (size_t)dst_w * 4;
        for (int x = 0; x < dst_w; x++) {
            const float* k = ax.weights + (size_t)x * (size_t)ax.stride;
            float acc[4] = { 0, 0, 0, 0 };
            for (int t = 0; t < ax.count[x]; t++) {
                const float* s = in + (size_t)clamp_index(ax.first[x] + t, w) * 4;
                acc[0] += k[t] * s[0];
                acc[1] += k[t] * s[1];
                acc[2] += k[t] * s[2];
                acc[3] += k[t] * s[3];
            }
            memcpy(o + (size_t)x * 4, acc, sizeof acc);
        }
    }
    for (int y = 0; y < dst_h; y++) {
        const float* k = ay.weights + (size_t)y * (size_t)ay.stride;
        uint32_t* o = px + (size_t)y * (size_t)dst_w;
        memset(row, 0, sizeof(float) * 4 * (size_t)dst_w);
        for (int t = 0; t < ay.count[y]; t++) {
            const float* s = tmp + (size_t)(clamp_index(ay.first[y] + t, h) - ay.lo) * (size_t)dst_w * 4;
            float kt = k[t];
            for (int i = 0; i < dst_w * 4; i++) row[i] += kt * s[i];
        }
        for (int x = 0; x < dst_w; x++) {
            float r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2], a = row[x * 4 + 3];
            if (a > 1.0f) a = 1.0f;
            if ((flags & FA_CRISP_EDGE) && a > 0.0f && a < 1.0f) {
                float inv = 1.0f / a, crisp = smoothstep(EDGE_LOW, EDGE_HIGH, a);
                r = r * inv * crisp;
                g = g * inv * crisp;
                b = b * inv * crisp;
                a = crisp;
            }
            o[x] = pack(b, g, r, a);
        }
    }
    out->w = dst_w;
    out->h = dst_h;
    out->px = px;
    px = NULL;
    ok = 1;
done:
    free(px);
    free(tmp);
    free(row);
    axis_free(&ax);
    axis_free(&ay);
    return ok;
}

static void dedither(float* rgba, const uint8_t* solid, int w, int h) {
    float* out = (float*)malloc(sizeof(float) * 4 * (size_t)w * (size_t)h);
    if (!out) return;
    memcpy(out, rgba, sizeof(float) * 4 * (size_t)w * (size_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float* c = rgba + ((size_t)y * w + x) * 4;
            float sum[3] = { c[0] * 2, c[1] * 2, c[2] * 2 }, weight = 2.0f, *o;
            if (!solid[(size_t)y * w + x]) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    const float* n;
                    float d;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h || !solid[(size_t)ny * w + nx]) continue;
                    n = rgba + ((size_t)ny * w + nx) * 4;
                    d = fabsf(n[0] - c[0]);
                    if (fabsf(n[1] - c[1]) > d) d = fabsf(n[1] - c[1]);
                    if (fabsf(n[2] - c[2]) > d) d = fabsf(n[2] - c[2]);
                    if (d > DEDITHER_STEP) continue;
                    sum[0] += n[0];
                    sum[1] += n[1];
                    sum[2] += n[2];
                    weight += 1.0f;
                }
            o = out + ((size_t)y * w + x) * 4;
            o[0] = sum[0] / weight;
            o[1] = sum[1] / weight;
            o[2] = sum[2] / weight;
        }
    memcpy(rgba, out, sizeof(float) * 4 * (size_t)w * (size_t)h);
    free(out);
}

#define LIFT_GROW 1.6f
#define LIFT_GLOW 0.35f

static float lift_ramp(float lo, float hi, float x) { return smoothstep(lo, hi, x); }

/* Hover letters can sit a few pixels off the normal ones (yes_1), so the normal picture only tells where red is new;
 * the hue keeps the orange rust out. */
static float lift_gain(const float* c, uint16_t normal, int normal_key, int x, int y, int w, int h) {
    float r0 = normal_key ? 0.0f : (float)((normal >> 11) & 31) / 31.0f;
    float g0 = normal_key ? 0.0f : (float)((normal >> 5) & 63) / 63.0f;
    float b0 = normal_key ? 0.0f : (float)(normal & 31) / 31.0f;
    float red1 = c[0] - (c[1] > c[2] ? c[1] : c[2]), red0 = r0 - (g0 > b0 ? g0 : b0);
    float hue = c[0] > 0.0f ? c[1] / c[0] : 1.0f;
    int edge = x < y ? x : y;
    if (w - 1 - x < edge) edge = w - 1 - x;
    if (h - 1 - y < edge) edge = h - 1 - y;
    return lift_ramp(0.045f, 0.14f, red1 - red0) * lift_ramp(0.07f, 0.16f, red1) * (1.0f - lift_ramp(0.30f, 0.46f, hue)) *
           lift_ramp(2.0f, 6.0f, (float)edge);
}

void fa_hover_lift(float* rgba, const uint8_t* solid, int w, int h, const uint16_t* normal, int normal_pitch,
                   uint16_t key, int keyed) {
    float* gain;
    if (!rgba || !solid || !normal || w <= 0 || h <= 0 || normal_pitch < w) return;
    gain = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!gain) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t v = normal[(size_t)y * (size_t)normal_pitch + (size_t)x];
            if (solid[(size_t)y * w + x])
                gain[(size_t)y * w + x] = lift_gain(rgba + ((size_t)y * w + x) * 4, v, keyed && v == key, x, y, w, h);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float* c = rgba + ((size_t)y * w + x) * 4;
            float near_sum = 0.0f, wide_sum = 0.0f, glow = 0.0f, m, density, t;
            int near_n = 0, wide_n = 0;
            if (!solid[(size_t)y * w + x]) continue;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int nx = x + dx, ny = y + dy, ring = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                    float g;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    g = gain[(size_t)ny * w + nx];
                    wide_sum += g;
                    wide_n++;
                    if (ring <= 1) {
                        near_sum += g;
                        near_n++;
                    }
                    if (g * (1.0f - 0.25f * (float)ring) > glow) glow = g * (1.0f - 0.25f * (float)ring);
                }
            density = lift_ramp(0.07f, 0.22f, wide_sum / (float)wide_n);
            m = near_sum / (float)near_n * LIFT_GROW;
            m = (m > 1.0f ? 1.0f : m) * density;
            glow *= density;
            t = (c[0] - 0.10f) / 0.45f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            c[0] += (0.80f + 0.20f * t - c[0]) * m;
            c[1] += (0.10f + 0.16f * t - c[1]) * m;
            c[2] += (0.06f + 0.08f * t - c[2]) * m;
            if (glow > m) {
                float k = LIFT_GLOW * (glow - m);
                c[0] += (0.85f - c[0]) * k;
                c[1] += (0.20f - c[1]) * k;
                c[2] += (0.08f - c[2]) * k;
            }
        }
    free(gain);
}

int fa_resample16(const fa_source_t* src, const double* part, int dst_w, int dst_h, unsigned flags, fa_bitmap_t* out) {
    float* rgba;
    uint8_t* solid;
    int ok;
    if (!src || !src->bits || !out || src->w <= 0 || src->h <= 0 || src->pitch < src->w || src->w > 8192 || src->h > 8192)
        return 0;
    rgba = (float*)malloc(sizeof(float) * 4 * (size_t)src->w * (size_t)src->h);
    solid = (uint8_t*)malloc((size_t)src->w * (size_t)src->h);
    if (!rgba || !solid) {
        free(rgba);
        free(solid);
        return 0;
    }
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) {
            uint16_t v = src->bits[(size_t)y * (size_t)src->pitch + (size_t)x];
            float* o = rgba + ((size_t)y * src->w + x) * 4;
            int key = src->keyed && v == src->key;
            solid[(size_t)y * src->w + x] = !key;
            o[0] = key ? 0.0f : (float)((v >> 11) & 31) / 31.0f;
            o[1] = key ? 0.0f : (float)((v >> 5) & 63) / 63.0f;
            o[2] = key ? 0.0f : (float)(v & 31) / 31.0f;
            o[3] = key ? 0.0f : 1.0f;
        }
    if ((flags & FA_HOVER_LIFT) && src->normal)
        fa_hover_lift(rgba, solid, src->w, src->h, src->normal, src->normal_pitch, src->key, src->keyed);
    if (flags & FA_DEDITHER) dedither(rgba, solid, src->w, src->h);
    ok = resample_float(rgba, src->w, src->h, part, dst_w, dst_h, src->keyed ? flags : (flags & ~(unsigned)FA_CRISP_EDGE), out);
    free(rgba);
    free(solid);
    return ok;
}

int fa_resample32(const uint32_t* bgra, int w, int h, int pitch, const double* part, int dst_w, int dst_h, fa_bitmap_t* out) {
    float* rgba;
    int ok;
    if (!bgra || !out || w <= 0 || h <= 0 || pitch < w || w > 8192 || h > 8192) return 0;
    rgba = (float*)malloc(sizeof(float) * 4 * (size_t)w * (size_t)h);
    if (!rgba) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = bgra[(size_t)y * (size_t)pitch + (size_t)x];
            float* o = rgba + ((size_t)y * w + x) * 4;
            o[0] = (float)((v >> 16) & 255) / 255.0f;
            o[1] = (float)((v >> 8) & 255) / 255.0f;
            o[2] = (float)(v & 255) / 255.0f;
            o[3] = 1.0f;
        }
    ok = resample_float(rgba, w, h, part, dst_w, dst_h, 0, out);
    free(rgba);
    return ok;
}

static uint32_t tga_pixel(const uint8_t* p, int bytes) {
    if (bytes == 2) {
        unsigned v = p[0] | ((unsigned)p[1] << 8), r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return 0xFF000000u | (((b << 3) | (b >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((r << 3) | (r >> 2)) << 16));
    }
    return 0xFF000000u | p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int fa_tga_decode(const uint8_t* data, size_t size, uint32_t** bgra, int* w, int* h) {
    int id_len, map_type, type, width, height, bits, bytes, top_first;
    size_t at, total, filled = 0;
    uint32_t* px;
    if (!data || size < 18 || !bgra || !w || !h) return 0;
    id_len = data[0];
    map_type = data[1];
    type = data[2];
    width = data[12] | (data[13] << 8);
    height = data[14] | (data[15] << 8);
    bits = data[16];
    top_first = (data[17] & 0x20) != 0;
    bytes = bits / 8;
    if (map_type != 0 || (type != 2 && type != 10) || (bits != 16 && bits != 24 && bits != 32) || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192)
        return 0;
    at = 18u + (size_t)id_len;
    total = (size_t)width * (size_t)height;
    px = (uint32_t*)malloc(total * sizeof *px);
    if (!px) return 0;
    while (filled < total) {
        if (type == 2) {
            if (at + (size_t)bytes > size) goto bad;
            px[filled++] = tga_pixel(data + at, bytes);
            at += (size_t)bytes;
        } else {
            unsigned head, count;
            if (at >= size) goto bad;
            head = data[at++];
            count = (head & 127) + 1;
            if (filled + count > total) goto bad;
            if (head & 128) {
                uint32_t v;
                if (at + (size_t)bytes > size) goto bad;
                v = tga_pixel(data + at, bytes);
                at += (size_t)bytes;
                while (count--) px[filled++] = v;
            } else {
                if (at + (size_t)bytes * count > size) goto bad;
                while (count--) {
                    px[filled++] = tga_pixel(data + at, bytes);
                    at += (size_t)bytes;
                }
            }
        }
    }
    if (!top_first)
        for (int y = 0; y < height / 2; y++) {
            uint32_t* a = px + (size_t)y * (size_t)width;
            uint32_t* b = px + (size_t)(height - 1 - y) * (size_t)width;
            for (int x = 0; x < width; x++) {
                uint32_t t = a[x];
                a[x] = b[x];
                b[x] = t;
            }
        }
    *bgra = px;
    *w = width;
    *h = height;
    return 1;
bad:
    free(px);
    return 0;
}

void fa_bitmap_free(fa_bitmap_t* b) {
    if (!b) return;
    free(b->px);
    b->px = NULL;
    b->w = b->h = 0;
}

void fa_blit_over(uint32_t* dst, long pitch, fe_rect_t clip, const fa_bitmap_t* b, int x, int y) {
    fe_rect_t r;
    if (!dst || !b || !b->px) return;
    r = fe_rect_intersect(clip, fe_rect(x, y, x + b->w, y + b->h));
    for (int yy = r.top; yy < r.bottom; yy++) {
        const uint32_t* s = b->px + (size_t)(yy - y) * (size_t)b->w + (size_t)(r.left - x);
        uint32_t* d = dst + (long)yy * pitch + r.left;
        for (int xx = r.left; xx < r.right; xx++, s++, d++) {
            uint32_t sp = *s, sa = sp >> 24, inv, dp, out;
            if (sa == 255) {
                *d = sp;
                continue;
            }
            if (!sa) continue;
            inv = 255 - sa;
            dp = *d;
            out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                uint32_t c = ((sp >> shift) & 255) + (((dp >> shift) & 255) * inv + 127) / 255;
                out |= (c > 255 ? 255 : c) << shift;
            }
            *d = out;
        }
    }
}

typedef struct {
    uint32_t hash;
    int sw, sh, pitch, dw, dh;
    unsigned flags;
    uint16_t key;
    uint8_t keyed, kind;
    double part[4];
} art_id_t;

typedef struct {
    art_id_t id;
    fa_bitmap_t bitmap;
    unsigned long stamp, epoch;
    int used;
} art_slot_t;

struct fa_cache {
    art_slot_t slots[CACHE_SLOTS];
    size_t budget, bytes;
    unsigned long stamp, epoch, hits, misses;
    int entries;
};

fa_cache_t* fa_cache_create(size_t budget_bytes) {
    fa_cache_t* c = (fa_cache_t*)calloc(1, sizeof *c);
    if (c) c->budget = budget_bytes;
    return c;
}

void fa_cache_clear(fa_cache_t* c) {
    if (!c) return;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        fa_bitmap_free(&c->slots[i].bitmap);
        c->slots[i].used = 0;
    }
    c->bytes = 0;
    c->entries = 0;
}

void fa_cache_destroy(fa_cache_t* c) {
    fa_cache_clear(c);
    free(c);
}

void fa_cache_begin(fa_cache_t* c) {
    if (c) c->epoch++;
}

static size_t bitmap_bytes(const fa_bitmap_t* b) { return (size_t)b->w * (size_t)b->h * 4; }

static void evict(fa_cache_t* c, art_slot_t* s) {
    c->bytes -= bitmap_bytes(&s->bitmap);
    fa_bitmap_free(&s->bitmap);
    s->used = 0;
    c->entries--;
}

/* A free slot for a picture of `need` bytes: the oldest pictures not used this frame go while the budget is exceeded. */
static art_slot_t* take_slot(fa_cache_t* c, size_t need) {
    for (;;) {
        art_slot_t *free_slot = NULL, *oldest = NULL;
        for (int i = 0; i < CACHE_SLOTS; i++) {
            art_slot_t* s = &c->slots[i];
            if (!s->used) {
                if (!free_slot) free_slot = s;
            } else if (s->epoch != c->epoch && (!oldest || s->stamp < oldest->stamp)) {
                oldest = s;
            }
        }
        if (free_slot && c->bytes + need <= c->budget) return free_slot;
        if (!oldest) return free_slot;
        evict(c, oldest);
    }
}

static art_slot_t* find_slot(fa_cache_t* c, const art_id_t* id) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (c->slots[i].used && !memcmp(&c->slots[i].id, id, sizeof *id)) return &c->slots[i];
    return NULL;
}

static const fa_bitmap_t* insert(fa_cache_t* c, const art_id_t* id, fa_bitmap_t* made) {
    art_slot_t* s = take_slot(c, bitmap_bytes(made));
    if (!s) {
        fa_bitmap_free(made);
        return NULL;
    }
    if (s->used) evict(c, s);
    s->id = *id;
    s->bitmap = *made;
    s->used = 1;
    s->stamp = ++c->stamp;
    s->epoch = c->epoch;
    c->bytes += bitmap_bytes(made);
    c->entries++;
    return &s->bitmap;
}

const fa_bitmap_t* fa_cache_get16(fa_cache_t* c, const fa_source_t* src, const double* part, int dst_w, int dst_h,
                                  unsigned flags) {
    art_id_t id;
    art_slot_t* s;
    fa_bitmap_t made;
    if (!c || !src) return NULL;
    memset(&id, 0, sizeof id);
    id.hash = src->hash;
    id.sw = src->w;
    id.sh = src->h;
    id.dw = dst_w;
    id.dh = dst_h;
    id.flags = flags;
    id.key = src->key;
    id.keyed = (uint8_t)(src->keyed != 0);
    id.kind = 16;
    if (part) memcpy(id.part, part, sizeof id.part);
    s = find_slot(c, &id);
    if (s) {
        s->stamp = ++c->stamp;
        s->epoch = c->epoch;
        c->hits++;
        return &s->bitmap;
    }
    c->misses++;
    if (!fa_resample16(src, part, dst_w, dst_h, flags, &made)) return NULL;
    return insert(c, &id, &made);
}

const fa_bitmap_t* fa_cache_get32(fa_cache_t* c, uint32_t hash, const uint32_t* bgra, int w, int h, int pitch,
                                  const double* part, int dst_w, int dst_h) {
    art_id_t id;
    art_slot_t* s;
    fa_bitmap_t made;
    if (!c || !bgra) return NULL;
    memset(&id, 0, sizeof id);
    id.hash = hash;
    id.sw = w;
    id.sh = h;
    id.pitch = pitch;
    id.dw = dst_w;
    id.dh = dst_h;
    id.kind = 32;
    if (part) memcpy(id.part, part, sizeof id.part);
    s = find_slot(c, &id);
    if (s) {
        s->stamp = ++c->stamp;
        s->epoch = c->epoch;
        c->hits++;
        return &s->bitmap;
    }
    c->misses++;
    if (!fa_resample32(bgra, w, h, pitch, part, dst_w, dst_h, &made)) return NULL;
    return insert(c, &id, &made);
}

size_t fa_cache_bytes(const fa_cache_t* c) { return c ? c->bytes : 0; }
int fa_cache_entries(const fa_cache_t* c) { return c ? c->entries : 0; }
unsigned long fa_cache_hits(const fa_cache_t* c) { return c ? c->hits : 0; }
unsigned long fa_cache_misses(const fa_cache_t* c) { return c ? c->misses : 0; }
