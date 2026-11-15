#include "frontend_plaque.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_RANGE 5
#define FILL_SWEEPS 400
#define FILL_SOR 1.85f
#define NEAR_GAP 3
#define LETTER_GAP 8
#define WIDE_PLATE 150 /* the main screen's torn plaques: their edges and rivets reach further in */

typedef struct {
    int w, h;
    float* rgb;     /* 0..255, 3 per pixel */
    uint8_t* solid; /* not the transparent key */
} image_t;

static void image_free(image_t* im) {
    free(im->rgb);
    free(im->solid);
    memset(im, 0, sizeof *im);
}

static int image_load(const fa_source_t* s, image_t* im) {
    size_t n;
    memset(im, 0, sizeof *im);
    if (!s || !s->bits || s->w <= 0 || s->h <= 0 || s->w > 2048 || s->h > 2048 || s->pitch < s->w) return 0;
    n = (size_t)s->w * (size_t)s->h;
    im->rgb = (float*)malloc(sizeof(float) * 3 * n);
    im->solid = (uint8_t*)malloc(n);
    if (!im->rgb || !im->solid) {
        image_free(im);
        return 0;
    }
    im->w = s->w;
    im->h = s->h;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint16_t v = s->bits[(size_t)y * (size_t)s->pitch + (size_t)x];
            float* c = im->rgb + ((size_t)y * s->w + x) * 3;
            im->solid[(size_t)y * s->w + x] = !(s->keyed && v == s->key);
            c[0] = (float)((v >> 11) & 31) * (255.0f / 31.0f);
            c[1] = (float)((v >> 5) & 63) * (255.0f / 63.0f);
            c[2] = (float)(v & 31) * (255.0f / 31.0f);
        }
    return 1;
}

static uint16_t pack565(const float* c) {
    int r = (int)floorf(c[0] * 31.0f / 255.0f + 0.5f), g = (int)floorf(c[1] * 63.0f / 255.0f + 0.5f),
        b = (int)floorf(c[2] * 31.0f / 255.0f + 0.5f);
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 63 ? 63 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static float mean3(const float* c) { return (c[0] + c[1] + c[2]) / 3.0f; }

static float spread3(const float* c) {
    float hi = c[0] > c[1] ? c[0] : c[1], lo = c[0] < c[1] ? c[0] : c[1];
    hi = c[2] > hi ? c[2] : hi;
    lo = c[2] < lo ? c[2] : lo;
    return hi - lo;
}

static uint8_t* mask_new(int w, int h) { return (uint8_t*)calloc((size_t)w * (size_t)h, 1); }

static uint8_t* dilated(const uint8_t* m, int w, int h, int r) {
    uint8_t* out = mask_new(w, h);
    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!m[(size_t)y * w + x]) continue;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int ny = y + dy, nx = x + dx;
                    if (ny >= 0 && nx >= 0 && ny < h && nx < w) out[(size_t)ny * w + nx] = 1;
                }
        }
    return out;
}

static int inside(fe_rect_t r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

typedef struct {
    int size, in_band;
    fe_rect_t box;
} component_t;

/* 8-connected components of m; calls keep for each (with its pixels' indices) and clears the ones it refuses. */
typedef int (*keep_fn)(void* ctx, const component_t* c, const int* pixels);

static int components(uint8_t* m, int w, int h, int band0, int band1, keep_fn keep, void* ctx) {
    int* stack = (int*)malloc(sizeof(int) * (size_t)w * (size_t)h);
    int* members = (int*)malloc(sizeof(int) * (size_t)w * (size_t)h);
    uint8_t* seen = mask_new(w, h);
    int kept = 0;
    if (!stack || !members || !seen) {
        free(stack), free(members), free(seen);
        return -1;
    }
    for (int start = 0; start < w * h; start++) {
        int top = 0, count = 0;
        component_t c;
        if (!m[start] || seen[start]) continue;
        memset(&c, 0, sizeof c);
        c.box = fe_rect(w, h, -1, -1);
        stack[top++] = start;
        seen[start] = 1;
        while (top) {
            int p = stack[--top], px = p % w, py = p / w;
            members[count++] = p;
            if (px < c.box.left) c.box.left = px;
            if (py < c.box.top) c.box.top = py;
            if (px + 1 > c.box.right) c.box.right = px + 1;
            if (py + 1 > c.box.bottom) c.box.bottom = py + 1;
            c.in_band += py >= band0 && py <= band1;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = px + dx, ny = py + dy, q = ny * w + nx;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || !m[q] || seen[q]) continue;
                    seen[q] = 1;
                    stack[top++] = q;
                }
        }
        c.size = count;
        if (keep(ctx, &c, members)) kept++;
        else
            for (int i = 0; i < count; i++) m[members[i]] = 0;
    }
    free(stack), free(members), free(seen);
    return kept;
}

typedef struct {
    int band_h;
} keep_ctx_t;

static int keep_letter(void* ctx, const component_t* c, const int* pixels) {
    const keep_ctx_t* k = (const keep_ctx_t*)ctx;
    (void)pixels;
    return c->in_band * 10 >= c->size * 6 && (c->box.bottom - c->box.top) * 100 >= k->band_h * 45;
}

typedef struct {
    const uint8_t* black;
    uint8_t* footprint; /* the kept letters, each moved onto its black letter */
    int w, h, dx, dy, band;
    fe_rect_t strong[64]; /* the letters that match well */
    int strong_count;
} place_ctx_t;

/* Where the words are: the widest run of well-matched letters with gaps narrower than a letter is tall. */
static fe_rect_t word_box(place_ctx_t* k) {
    fe_rect_t best = fe_rect(0, 0, 0, 0), run;
    for (int i = 1; i < k->strong_count; i++)
        for (int j = i; j > 0 && k->strong[j].left < k->strong[j - 1].left; j--) {
            fe_rect_t t = k->strong[j];
            k->strong[j] = k->strong[j - 1];
            k->strong[j - 1] = t;
        }
    for (int i = 0; i < k->strong_count; i++) {
        run = k->strong[i];
        for (int j = i + 1; j < k->strong_count && (k->strong[j].left - run.right) * 5 <= k->band * 4; j++)
            run = fe_rect_union(run, k->strong[j]);
        if (run.right - run.left > best.right - best.left) best = run;
    }
    return best;
}

/* A red letter of the hover picture lies on a black letter of the normal one (red rust does not). The hover picture's
 * spacing differs a little, so each letter is moved by its own amount around the picture's. */
static int keep_on_black(void* ctx, const component_t* c, const int* pixels) {
    place_ctx_t* k = (place_ctx_t*)ctx;
    int best = -1, bx = 0, by = 0;
    for (int dy = k->dy - 3; dy <= k->dy + 3; dy++)
        for (int dx = k->dx - 4; dx <= k->dx + 4; dx++) {
            int n = 0;
            for (int i = 0; i < c->size; i++) {
                int x = pixels[i] % k->w + dx, y = pixels[i] / k->w + dy;
                n += x >= 0 && y >= 0 && x < k->w && y < k->h && k->black[y * k->w + x];
            }
            if (n > best) best = n, bx = dx, by = dy;
        }
    if (best * 100 >= c->size * 60 && (c->box.bottom - c->box.top) * 10 >= k->band * 6 && k->strong_count < 64)
        k->strong[k->strong_count++] = fe_rect_offset(c->box, bx, by);
    for (int i = 0; i < c->size; i++) {
        int x = pixels[i] % k->w + bx, y = pixels[i] / k->w + by;
        if (x >= 0 && y >= 0 && x < k->w && y < k->h) k->footprint[y * k->w + x] = 1;
    }
    return 1;
}

#define BLOCK_R 3
#define BLOCK_STEP 3
#define DETAIL_FINE 1
#define DETAIL_COARSE 4
#define CALM_PERCENT 50
#define WINDOW_MARGIN 40

static uint32_t next_random(uint32_t* state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static int compare_floats(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

/* The harmonic fill of the hole from the pixels around it: smooth, and free of anything the hole's neighbours lack. */
static void smooth_fill(float* rgb, const uint8_t* valid, const int* holes, int count, int w, int h) {
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int sweep = 0; sweep < FILL_SWEEPS; sweep++)
        for (int i = 0; i < count; i++) {
            int p = holes[i], x = p % w, y = p / w, n = 0;
            float sum[3] = { 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int nx = x + d[k][0], ny = y + d[k][1], q = ny * w + nx;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || !valid[q]) continue;
                for (int c = 0; c < 3; c++) sum[c] += rgb[(size_t)q * 3 + c];
                n++;
            }
            if (!n) continue;
            for (int c = 0; c < 3; c++) {
                float* v = rgb + (size_t)p * 3 + c;
                *v += FILL_SOR * (sum[c] / (float)n - *v);
            }
        }
}

/* The brightness spread of the block around centre; detail is the picture without its blur. */
static float block_contrast(const float* detail, int w, int centre) {
    float sq = 0.0f;
    for (int dy = -BLOCK_R; dy <= BLOCK_R; dy++)
        for (int dx = -BLOCK_R; dx <= BLOCK_R; dx++) {
            float v = mean3(detail + ((size_t)centre + (size_t)(dy * w + dx)) * 3);
            sq += v * v;
        }
    return sq;
}

static float hann(int k) { return 0.5f + 0.5f * cosf(3.14159265f * (float)k / (float)(BLOCK_R + 1)); }

/* The grain the smooth fill lacks: blocks of the picture's own fine detail (its pixels less their blur), taken from
 * the calmer parts of the picture near the hole (no rivets, edges or glints), set down in random places overlapping and
 * faded into each other, so that no stripe or edge of the plate is repeated. */
static void grain_fill(float* rgb, const uint8_t* hole, const uint8_t* good, const float* original, int w, int h) {
    fe_rect_t box = fe_rect(w, h, 0, 0), win;
    float *detail, *sum, *weight, *spread;
    int *pool, pool_count = 0;
    uint32_t seed = 987654321u;
    float level = 0.0f, result = 0.0f;
    size_t n = (size_t)w * (size_t)h;
    int hole_count = 0;
    for (int i = 0; i < w * h; i++)
        if (hole[i]) {
            if (i % w < box.left) box.left = i % w;
            if (i / w < box.top) box.top = i / w;
            if (i % w + 1 > box.right) box.right = i % w + 1;
            if (i / w + 1 > box.bottom) box.bottom = i / w + 1;
        }
    win = fe_rect_intersect(fe_rect_inflate(box, WINDOW_MARGIN, WINDOW_MARGIN), fe_rect(0, 0, w, h));
    detail = (float*)calloc(n * 3, sizeof(float));
    sum = (float*)calloc(n * 3, sizeof(float));
    weight = (float*)calloc(n, sizeof(float));
    spread = (float*)malloc(sizeof(float) * n);
    pool = (int*)malloc(sizeof(int) * n);
    if (!detail || !sum || !weight || !spread || !pool) goto done;
    for (int y = win.top; y < win.bottom; y++)
        for (int x = win.left; x < win.right; x++) {
            float mean[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
            int m[2] = { 0, 0 };
            if (!good[y * w + x]) continue;
            for (int dy = -DETAIL_COARSE; dy <= DETAIL_COARSE; dy++)
                for (int dx = -DETAIL_COARSE; dx <= DETAIL_COARSE; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || !good[ny * w + nx]) continue;
                    for (int k = 0; k < 2; k++) {
                        int reach = k ? DETAIL_COARSE : DETAIL_FINE;
                        if (abs(dx) > reach || abs(dy) > reach) continue;
                        for (int c = 0; c < 3; c++) mean[k][c] += original[((size_t)ny * w + nx) * 3 + c];
                        m[k]++;
                    }
                }
            for (int c = 0; c < 3; c++) detail[((size_t)y * w + x) * 3 + c] = mean[0][c] / (float)m[0] - mean[1][c] / (float)m[1];
        }
    for (int y = win.top + BLOCK_R; y < win.bottom - BLOCK_R; y++)
        for (int x = win.left + BLOCK_R; x < win.right - BLOCK_R; x++) {
            int ok = 1;
            for (int dy = -BLOCK_R; ok && dy <= BLOCK_R; dy++)
                for (int dx = -BLOCK_R; dx <= BLOCK_R; dx++)
                    if (!good[(y + dy) * w + x + dx]) {
                        ok = 0;
                        break;
                    }
            if (ok) pool[pool_count++] = y * w + x;
        }
    if (pool_count < 8) goto done;
    for (int k = 0; k < pool_count; k++) spread[k] = block_contrast(detail, w, pool[k]);
    {
        float* sorted = (float*)malloc(sizeof(float) * (size_t)pool_count);
        float limit;
        int kept = 0;
        if (!sorted) goto done;
        memcpy(sorted, spread, sizeof(float) * (size_t)pool_count);
        qsort(sorted, (size_t)pool_count, sizeof(float), compare_floats);
        limit = sorted[(pool_count - 1) * CALM_PERCENT / 100];
        free(sorted);
        for (int k = 0; k < pool_count; k++) {
            if (spread[k] > limit) continue;
            pool[kept++] = pool[k];
            level += spread[k];
        }
        pool_count = kept;
        level = sqrtf(level / (float)(pool_count * (2 * BLOCK_R + 1) * (2 * BLOCK_R + 1)));
    }
    for (int by = box.top - BLOCK_R; by < box.bottom + BLOCK_R; by += BLOCK_STEP)
        for (int bx = box.left - BLOCK_R; bx < box.right + BLOCK_R; bx += BLOCK_STEP) {
            int s = pool[next_random(&seed) % (unsigned)pool_count];
            for (int dy = -BLOCK_R; dy <= BLOCK_R; dy++)
                for (int dx = -BLOCK_R; dx <= BLOCK_R; dx++) {
                    int x = bx + dx, y = by + dy;
                    float k;
                    if (x < 0 || y < 0 || x >= w || y >= h || !hole[y * w + x]) continue;
                    k = hann(dx) * hann(dy);
                    for (int c = 0; c < 3; c++) sum[((size_t)y * w + x) * 3 + c] += k * detail[((size_t)s + (size_t)(dy * w + dx)) * 3 + c];
                    weight[y * w + x] += k;
                }
        }
    for (int i = 0; i < w * h; i++)
        if (hole[i] && weight[i] > 0.0f) {
            float v;
            for (int c = 0; c < 3; c++) sum[(size_t)i * 3 + c] /= weight[i];
            v = mean3(sum + (size_t)i * 3);
            result += v * v;
            hole_count++;
        }
    if (!hole_count) goto done;
    result = sqrtf(result / (float)hole_count);
    {
        /* Averaged blocks are calmer than the blocks themselves. */
        float gain = result > 0.01f ? level / result : 1.0f;
        gain = gain < 0.6f ? 0.6f : gain > 1.2f ? 1.2f : gain;
        for (int i = 0; i < w * h; i++)
            if (hole[i] && weight[i] > 0.0f)
                for (int c = 0; c < 3; c++) rgb[(size_t)i * 3 + c] += gain * sum[(size_t)i * 3 + c];
    }
done:
    free(detail), free(sum), free(weight), free(spread), free(pool);
}

void fp_fill(float* rgb, const uint8_t* valid, const uint8_t* hole, int w, int h) {
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int count = 0;
    int* holes;
    uint8_t *good, *around;
    float *original, *texture;
    for (int i = 0; i < w * h; i++) count += hole[i] != 0;
    if (!count) return;
    holes = (int*)malloc(sizeof(int) * (size_t)count);
    good = mask_new(w, h);
    around = dilated(hole, w, h, 2);
    original = (float*)malloc(sizeof(float) * 3 * (size_t)w * (size_t)h);
    texture = (float*)malloc(sizeof(float) * 3 * (size_t)w * (size_t)h);
    if (!holes || !good || !around || !original || !texture) goto done;
    memcpy(original, rgb, sizeof(float) * 3 * (size_t)w * (size_t)h);
    count = 0;
    for (int i = 0; i < w * h; i++) {
        if (hole[i]) holes[count++] = i;
        good[i] = valid[i] && !around[i];
    }
    smooth_fill(rgb, valid, holes, count, w, h);
    grain_fill(rgb, hole, good, original, w, h);
    memcpy(texture, rgb, sizeof(float) * 3 * (size_t)w * (size_t)h);
    /* The grain's gradients are kept, its level is set by the hole's edge. */
    for (int sweep = 0; sweep < FILL_SWEEPS; sweep++)
        for (int i = 0; i < count; i++) {
            int p = holes[i], x = p % w, y = p / w, n = 0;
            float sum[3] = { 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int nx = x + d[k][0], ny = y + d[k][1], q = ny * w + nx;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || !valid[q]) continue;
                for (int c = 0; c < 3; c++)
                    sum[c] += rgb[(size_t)q * 3 + c] + (hole[q] ? texture[(size_t)p * 3 + c] - texture[(size_t)q * 3 + c] : 0.0f);
                n++;
            }
            if (!n) continue;
            for (int c = 0; c < 3; c++) {
                float* v = rgb + (size_t)p * 3 + c;
                *v += FILL_SOR * (sum[c] / (float)n - *v);
            }
        }
    for (int i = 0; i < count; i++)
        for (int c = 0; c < 3; c++) {
            float* v = rgb + (size_t)holes[i] * 3 + c;
            *v = *v < 0.0f ? 0.0f : *v > 255.0f ? 255.0f : *v;
        }
done:
    free(holes), free(good), free(around), free(original), free(texture);
}

/* The letters grown by grow, and footprint when given, are filled from the picture around them. */
static int finish(const fa_source_t* s, image_t* im, uint8_t* letters, const uint8_t* footprint, int grow, fp_plaque_t* out) {
    uint8_t* hole = dilated(letters, im->w, im->h, grow);
    float ink[3] = { 0, 0, 0 };
    int n = 0;
    if (!hole) return 0;
    out->letters = fe_rect(im->w, im->h, 0, 0);
    for (int i = 0; i < im->w * im->h; i++) {
        hole[i] = (hole[i] || (footprint && footprint[i])) && im->solid[i];
        if (!letters[i]) continue;
        for (int c = 0; c < 3; c++) ink[c] += im->rgb[(size_t)i * 3 + c];
        n++;
        if (i % im->w < out->letters.left) out->letters.left = i % im->w;
        if (i / im->w < out->letters.top) out->letters.top = i / im->w;
        if (i % im->w + 1 > out->letters.right) out->letters.right = i % im->w + 1;
        if (i / im->w + 1 > out->letters.bottom) out->letters.bottom = i / im->w + 1;
    }
    if (!n) {
        free(hole);
        return 0;
    }
    out->ink = ((uint32_t)(ink[0] / n) << 16) | ((uint32_t)(ink[1] / n) << 8) | (uint32_t)(ink[2] / n);
    fp_fill(im->rgb, im->solid, hole, im->w, im->h);
    free(hole);
    out->w = im->w;
    out->h = im->h;
    out->clean = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)im->w * (size_t)im->h);
    if (!out->clean) return 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            size_t i = (size_t)y * im->w + x;
            out->clean[i] = im->solid[i] ? pack565(im->rgb + i * 3) : s->bits[(size_t)y * s->pitch + x];
        }
    return 1;
}

/* The plate around the letters along their middle row and column, inset from its edges; not far above or below them. */
static fe_rect_t plate_room(const image_t* im, fe_rect_t l) {
    int cx = (l.left + l.right) / 2, cy = (l.top + l.bottom) / 2, x0 = cx, x1 = cx, y0 = cy, y1 = cy;
    int lh = l.bottom - l.top, inset_x, inset_y;
    fe_rect_t r;
    while (x0 > 0 && im->solid[(size_t)cy * im->w + x0 - 1]) x0--;
    while (x1 < im->w - 1 && im->solid[(size_t)cy * im->w + x1 + 1]) x1++;
    while (y0 > 0 && im->solid[(size_t)(y0 - 1) * im->w + cx]) y0--;
    while (y1 < im->h - 1 && im->solid[(size_t)(y1 + 1) * im->w + cx]) y1++;
    inset_x = (x1 - x0) * (x1 - x0 >= WIDE_PLATE ? 12 : 8) / 100;
    if (inset_x < 3) inset_x = 3;
    inset_y = (y1 - y0) * 12 / 100 > 2 ? (y1 - y0) * 12 / 100 : 2;
    r = fe_rect(x0 + inset_x, y0 + inset_y, x1 + 1 - inset_x, y1 + 1 - inset_y);
    r = fe_rect_intersect(r, fe_rect(0, l.top - lh * 6 / 10, im->w, l.bottom + lh * 6 / 10));
    return fe_rect_union(r, l);
}

static int is_black(const float* c) { return mean3(c) < 40.0f && spread3(c) < 20.0f; }
static int is_red(const float* c) { return c[0] > 2.0f * c[1] + 8.0f && c[1] < 60.0f; }
/* The letters' thin strokes and edges are less red in 16-bit colour: taken only next to clearly red pixels. */
static int is_reddish(const float* c) { return c[0] > 1.6f * c[1] + 6.0f && c[0] > 1.3f * c[2] + 4.0f && c[1] < 72.0f && c[0] > 36.0f; }

/* The box of a mask's pixels. */
static fe_rect_t letters_box(const uint8_t* m, int w, int h) {
    fe_rect_t r = fe_rect(w, h, 0, 0);
    for (int i = 0; i < w * h; i++)
        if (m[i]) {
            if (i % w < r.left) r.left = i % w;
            if (i / w < r.top) r.top = i / w;
            if (i % w + 1 > r.right) r.right = i % w + 1;
            if (i / w + 1 > r.bottom) r.bottom = i / w + 1;
        }
    return r;
}

static int dark_in_column(const image_t* im, int x, int top, int bottom) {
    int n = 0;
    for (int y = top; y < bottom; y++) {
        size_t i = (size_t)y * im->w + x;
        n += im->solid[i] && mean3(im->rgb + i * 3) < 40.0f;
    }
    return n;
}

/* The box grown sideways over letters next to it (a first or last letter the hover picture lacks): through columns
 * with dark pixels, a gap of a few columns allowed, by a letter's width at most. The hover picture's spacing is wider
 * than the normal one's, so its last letters are the ones displaced out of reach: at the right end a wider gap is
 * crossed to a separate run of columns that holds as much dark as a letter does and ends within a letter's width. */
static fe_rect_t widen_to_letters(const image_t* im, fe_rect_t box) {
    int band = box.bottom - box.top;
    for (int side = -1; side <= 1; side += 2) {
        int start = side < 0 ? box.left - 1 : box.right, last = start - side, gap = 0, mass = 0, run = -1, run_last = 0;
        for (int x = start; x >= 0 && x < im->w && abs(x - start) < band * 9 / 10 && gap < (side < 0 ? NEAR_GAP : LETTER_GAP); x += side) {
            int dark = dark_in_column(im, x, box.top, box.bottom), strong = dark * 3 >= band && dark * 10 < band * 8;
            if (run >= 0) {
                if (strong) mass += dark, run_last = x, gap = 0;
                else gap++;
                if (gap >= 2 && mass >= band * 2) {
                    last = run_last;
                    break;
                }
                if (abs(x - run) >= band * 3 / 4) break;
            } else if (dark * 4 >= band && gap < NEAR_GAP) {
                gap = 0;
                last = x;
            } else if (side > 0 && strong && gap >= NEAR_GAP) {
                run = x;
                run_last = x;
                mass = dark;
                gap = 0;
            } else
                gap++;
        }
        if (side < 0) box.left = last;
        else box.right = last + 1;
    }
    return box;
}

/* The rows of the hover picture's red letters: of the bands of rows rich in red, the one with the most red. */
static int letter_band(const uint8_t* red, int w, int h, int* top, int* bottom) {
    int* rows = (int*)calloc((size_t)h, sizeof(int));
    int best = 0;
    if (!rows) return 0;
    for (int i = 0; i < w * h; i++) rows[i / w] += red[i];
    for (int seed = 0; seed < h; seed++) {
        int y0 = seed, y1 = seed, mass = 0;
        if (rows[seed] < 3) continue;
        while (y0 > 0 && rows[y0 - 1] * 10 > rows[seed] * 3) y0--;
        while (y1 < h - 1 && rows[y1 + 1] * 10 > rows[seed] * 3) y1++;
        for (int y = y0; y <= y1; y++) mass += rows[y];
        if (y1 - y0 + 1 >= 6 && mass > best) best = mass, *top = y0, *bottom = y1;
    }
    free(rows);
    return best > 0;
}

int fp_button(const fa_source_t* normal, const fa_source_t* hover, fp_plaque_t* out) {
    image_t a, b;
    uint8_t *black = NULL, *red = NULL, *letters = NULL;
    int ok = 0, w, h, best = -1, best_dx = 0, best_dy = 0, y0 = 0, y1 = 0, x0, x1, bh;
    keep_ctx_t k;
    memset(out, 0, sizeof *out);
    if (!image_load(normal, &a)) return 0;
    if (!image_load(hover, &b) || b.w != a.w || b.h != a.h) goto done;
    w = a.w;
    h = a.h;
    black = mask_new(w, h);
    red = mask_new(w, h);
    letters = mask_new(w, h);
    if (!black || !red || !letters) goto done;
    for (int i = 0; i < w * h; i++) {
        black[i] = a.solid[i] && is_black(a.rgb + (size_t)i * 3);
        red[i] = b.solid[i] && is_red(b.rgb + (size_t)i * 3);
    }
    if (!letter_band(red, w, h, &y0, &y1)) goto done;
    bh = y1 - y0 + 1;
    k.band_h = bh;
    for (int i = 0; i < w * h; i++)
        if (i / w < y0 - 2 || i / w > y1 + 2) red[i] = 0;
    if (bh < 5 || components(red, w, h, y0, y1, keep_letter, &k) <= 0) goto done;
    {
        uint8_t* near_red = dilated(red, w, h, 2);
        if (!near_red) goto done;
        for (int i = 0; i < w * h; i++)
            if (near_red[i] && b.solid[i] && is_reddish(b.rgb + (size_t)i * 3)) red[i] = 1;
        free(near_red);
    }
    x0 = w;
    x1 = -1;
    for (int i = 0; i < w * h; i++)
        if (red[i]) {
            if (i % w < x0) x0 = i % w;
            if (i % w > x1) x1 = i % w;
            if (i / w < y0) y0 = i / w;
            if (i / w > y1) y1 = i / w;
        }
    for (int dy = -ALIGN_RANGE; dy <= ALIGN_RANGE; dy++)
        for (int dx = -ALIGN_RANGE; dx <= ALIGN_RANGE; dx++) {
            int n = 0;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    if (red[y * w + x] && y + dy >= 0 && y + dy < h && x + dx >= 0 && x + dx < w)
                        n += black[(y + dy) * w + x + dx];
            if (n > best) best = n, best_dx = dx, best_dy = dy;
        }
    if (best < 20) goto done;
    {
        place_ctx_t pc;
        uint8_t *shifted = mask_new(w, h), *near = NULL;
        pc.black = black;
        pc.footprint = shifted;
        pc.w = w;
        pc.h = h;
        pc.dx = best_dx;
        pc.dy = best_dy;
        pc.band = bh;
        pc.strong_count = 0;
        if (!shifted || components(red, w, h, y0, y1, keep_on_black, &pc) <= 0) {
            free(shifted);
            goto done;
        }
        near = dilated(shifted, w, h, 2);
        free(shifted);
        if (!near) goto done;
        {
            /* Every dark pixel among the letters goes: stencil letters come in pieces the hover picture does not
             * always show in red. */
            fe_rect_t box = widen_to_letters(&a, letters_box(near, w, h));
            for (int y = box.top; y < box.bottom; y++)
                for (int x = box.left; x < box.right; x++) {
                    int i = y * w + x;
                    letters[i] = a.solid[i] && mean3(a.rgb + (size_t)i * 3) < 64.0f;
                }
            /* The whole band of the words is filled, not just the letters' shapes: left alone, the plate between the
             * letters would outline them. */
            box = fe_rect_intersect(fe_rect_inflate(box, 1, 2), fe_rect(0, 0, w, h));
            for (int y = box.top; y < box.bottom; y++)
                for (int x = box.left; x < box.right; x++) near[y * w + x] = 1;
        }
        out->lines = 1;
        ok = finish(normal, &a, letters, near, 2, out);
        if (ok && pc.strong_count) out->letters = word_box(&pc);
        free(near);
    }
    if (ok) out->room = plate_room(&a, out->letters);
done:
    free(black), free(red), free(letters);
    image_free(&a);
    image_free(&b);
    if (!ok) fp_free(out);
    return ok;
}

static int keep_mark(void* ctx, const component_t* c, const int* pixels) {
    (void)ctx;
    (void)pixels;
    return c->size >= 4;
}

static int median_of(const image_t* im, fe_rect_t area) {
    int hist[256], total = 0, at = 0;
    memset(hist, 0, sizeof hist);
    for (int y = area.top; y < area.bottom; y++)
        for (int x = area.left; x < area.right; x++) {
            int v = (int)mean3(im->rgb + ((size_t)y * im->w + x) * 3);
            hist[v < 0 ? 0 : v > 255 ? 255 : v]++;
            total++;
        }
    for (int v = 0; v < 256; v++)
        if ((at += hist[v]) * 2 >= total) return v;
    return 128;
}

int fp_engraved(const fa_source_t* picture, fe_rect_t zone, fe_rect_t room, fp_plaque_t* out) {
    image_t a;
    uint8_t *letters, *band = NULL;
    int ok = 0, median, n = 0;
    memset(out, 0, sizeof *out);
    if (!image_load(picture, &a)) return 0;
    zone = fe_rect_intersect(zone, fe_rect(0, 0, a.w, a.h));
    letters = mask_new(a.w, a.h);
    if (fe_rect_empty(zone) || !letters) goto done;
    median = median_of(&a, zone);
    for (int y = zone.top; y < zone.bottom; y++)
        for (int x = zone.left; x < zone.right; x++) {
            size_t i = (size_t)y * a.w + x;
            letters[i] = a.solid[i] && mean3(a.rgb + i * 3) < (float)median * 0.8f;
        }
    if (components(letters, a.w, a.h, 0, a.h, keep_mark, NULL) < 0) goto done;
    for (int i = 0; i < a.w * a.h; i++) n += letters[i];
    if (n < 40) goto done;
    out->lines = 1;
    band = mask_new(a.w, a.h);
    if (!band) goto done;
    /* The engraving's lit and shaded edges reach past the dark of the letters: the whole zone is filled. */
    for (int y = zone.top; y < zone.bottom; y++)
        for (int x = zone.left; x < zone.right; x++) band[(size_t)y * a.w + x] = 1;
    ok = finish(picture, &a, letters, band, 2, out);
    if (ok) out->room = room;
done:
    free(letters);
    free(band);
    image_free(&a);
    if (!ok) fp_free(out);
    return ok;
}

int fp_light(const fa_source_t* picture, fe_rect_t area, fp_plaque_t* out) {
    image_t a;
    uint8_t* letters;
    int ok = 0, n = 0, in_line = 0;
    memset(out, 0, sizeof *out);
    if (!image_load(picture, &a)) return 0;
    area = fe_rect_intersect(area, fe_rect(0, 0, a.w, a.h));
    letters = mask_new(a.w, a.h);
    if (fe_rect_empty(area) || !letters) goto done;
    for (int y = area.top; y < area.bottom; y++) {
        int row = 0;
        for (int x = area.left; x < area.right; x++) {
            size_t i = (size_t)y * a.w + x;
            const float* c = a.rgb + i * 3;
            letters[i] = a.solid[i] && mean3(c) > 120.0f && spread3(c) < 70.0f;
            row += letters[i];
            n += letters[i];
        }
        if (row >= 8 && !in_line) out->lines++;
        in_line = row >= 8;
    }
    if (n < 20) goto done;
    {
        int lines = out->lines;
        ok = finish(picture, &a, letters, NULL, 1, out);
        out->lines = lines;
    }
    if (ok) out->room = area;
done:
    free(letters);
    image_free(&a);
    if (!ok) fp_free(out);
    return ok;
}

int fp_tone(const fp_plaque_t* p, const fa_source_t* state, uint16_t* out) {
    image_t t, c;
    fa_source_t clean;
    double sum_s[3] = { 0, 0, 0 }, sq_s[3] = { 0, 0, 0 }, sum_t[3] = { 0, 0, 0 }, sq_t[3] = { 0, 0, 0 }, spread = 0;
    float mean_s[3], sd_s[3], mean_t[3], sd_t[3];
    fe_rect_t skip;
    int n = 0, grey;
    if (!p || !p->clean || !image_load(state, &t)) return 0;
    memset(&clean, 0, sizeof clean);
    clean.bits = p->clean;
    clean.w = p->w;
    clean.h = p->h;
    clean.pitch = p->w;
    clean.keyed = state->keyed;
    clean.key = p->clean[0];
    if (t.w != p->w || t.h != p->h || !image_load(&clean, &c)) {
        image_free(&t);
        return 0;
    }
    skip = fe_rect_inflate(p->letters, 3, 3);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            size_t i = (size_t)y * t.w + x;
            if (!t.solid[i] || inside(skip, x, y)) continue;
            for (int k = 0; k < 3; k++) {
                sum_t[k] += t.rgb[i * 3 + k];
                sq_t[k] += t.rgb[i * 3 + k] * t.rgb[i * 3 + k];
            }
            spread += spread3(t.rgb + i * 3);
            n++;
        }
    grey = n && spread / n < 12.0;
    n = 0;
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            size_t i = (size_t)y * c.w + x;
            float* v = c.rgb + i * 3;
            if (grey) v[0] = v[1] = v[2] = mean3(v);
            if (!c.solid[i] || inside(skip, x, y)) continue;
            for (int k = 0; k < 3; k++) {
                sum_s[k] += v[k];
                sq_s[k] += v[k] * v[k];
            }
            n++;
        }
    if (!n) n = 1;
    for (int k = 0; k < 3; k++) {
        int m = 0;
        for (int i = 0; i < t.w * t.h; i++) m += t.solid[i] && !inside(skip, i % t.w, i / t.w);
        if (!m) m = 1;
        mean_s[k] = (float)(sum_s[k] / n);
        sd_s[k] = (float)sqrt(fmax(sq_s[k] / n - mean_s[k] * mean_s[k], 1.0));
        mean_t[k] = (float)(sum_t[k] / m);
        sd_t[k] = (float)sqrt(fmax(sq_t[k] / m - mean_t[k] * mean_t[k], 1.0));
    }
    for (int i = 0; i < p->w * p->h; i++) {
        float v[3];
        if (!c.solid[i]) {
            out[i] = p->clean[i];
            continue;
        }
        for (int k = 0; k < 3; k++) v[k] = (c.rgb[(size_t)i * 3 + k] - mean_s[k]) / sd_s[k] * sd_t[k] + mean_t[k];
        out[i] = pack565(v);
        if (state->keyed && out[i] == p->clean[0]) out[i] ^= 1;
    }
    image_free(&t);
    image_free(&c);
    return 1;
}

void fp_free(fp_plaque_t* p) {
    if (!p) return;
    free(p->clean);
    memset(p, 0, sizeof *p);
}
