#include "frontend_assets.h"

#include "frontend_art.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE 256
#define MAX_OPS 1536
#define GLYPH_CACHE 192
#define FONT_CACHE 48
#define ART_BUDGET ((size_t)96 << 20)
#define MAX_THREADS 8
#define BACKDROP_W 640
#define BACKDROP_H 480
#define UP 2
#define BLUR_W 80
#define BLUR_H 60
#define FIELD_W 16
#define FIELD_H 12
#define PI 3.14159265358979

typedef struct {
    float r, g, b;
} rgb_t;

typedef struct {
    uint32_t key;
    unsigned stamp;
    int w, h, pad, advance, line_h;
    uint8_t* a;
} glyphs_t;

typedef enum {
    OP_DIM,
    OP_BACKDROP,
    OP_PLANK,
    OP_FOOTER,
    OP_SHADOW,
    OP_PLATE,
    OP_WELL,
    OP_GLOW,
    OP_RIVET,
    OP_TEXT,
    OP_SOCKET,
    OP_STROKE,
    OP_GROOVE,
    OP_BAR,
    OP_IMAGE,
    OP_ARROW,
    OP_AMBIENT,
    OP_PHOTO,
    OP_ART,
    OP_WIDE,
    OP_FILL
} op_kind_t;

enum { MAT_IRON, MAT_BRASS, MAT_STEEL, MAT_COPPER, MAT_PLAIN };

enum {
    F_RECESSED = 1,
    F_DISABLED = 2,
    F_WEAR = 4,
    F_BLUR = 8,
    F_HOVER = 16,
    F_GRILLE = 32,
    F_DOWN = 64,
    F_GRADIENT = 128,
    F_GRIP = 256,
    F_AO = 512
};

typedef struct {
    uint8_t kind, material;
    uint16_t flags;
    fe_rect_t bounds;
    double x0, y0, x1, y1;
    double radius, width, unit;
    double pts[6];
    rgb_t color, color2;
    float alpha, light;
    uint32_t seed;
    const glyphs_t* glyphs;
    const uint16_t* image;
    const uint32_t* image32;
    int image_w, image_h, image_pitch;
    double src[4]; /* the part of the image drawn into x0..y1 */
    uint16_t key;
    uint8_t keyed, base;
    const fa_bitmap_t* art;  /* OP_ART, OP_WIDE: a scaled picture with its corner at art_x, art_y */
    const fa_bitmap_t* art2; /* OP_WIDE: the same picture very small, for the blurred sides */
    int art_x, art_y;
} op_t;

typedef struct {
    int role, px, weight;
    HFONT font;
    unsigned stamp;
} font_entry_t;

struct fe_painter {
    float noise[TILE * TILE];
    HDC dc;
    char faces[FE_FONT_COUNT][LF_FACESIZE];
    int boost[FE_FONT_COUNT];
    font_entry_t fonts[FONT_CACHE];
    glyphs_t glyphs[GLYPH_CACHE];
    unsigned stamp;
    HBITMAP dib;
    uint32_t* dib_bits;
    int dib_w, dib_h;
    const uint16_t* backdrop_src;
    uint32_t backdrop_hash;
    uint32_t* up;
    float* blur;
    uint32_t* base; /* the main screen's static layer at the screen size */
    int base_w, base_h, base_valid, marking_base;
    fe_rect_t clip; /* applied to new ops while clipping */
    int clipping;
    uint32_t base_key;
    uint32_t* photo; /* a loading screen's picture, de-dithered and up-sampled from the game's */
    const uint32_t* photo_src;
    int photo_w, photo_h, photo_ready;
    uint32_t photo_key;
    float* photo_blur;  /* BLUR_W x BLUR_H */
    float* photo_field; /* FIELD_W x FIELD_H: the picture's colours with no shape left */
    op_t ops[MAX_OPS];
    int op_count;
    fe_view_t view;
    fa_cache_t* art;
};

typedef struct {
    const fe_painter_t* p;
    uint32_t* px;
    long pitch;
    fe_rect_t clip;
} band_t;

static const rgb_t k_black = { 0, 0, 0 };
static const rgb_t k_bone = { 0.93f, 0.90f, 0.83f };
static const rgb_t k_amber = { 1.0f, 0.64f, 0.20f };
static const rgb_t k_ink = { 0.12f, 0.10f, 0.08f };
static const rgb_t k_red = { 0.84f, 0.17f, 0.07f };

static rgb_t rgb(float r, float g, float b) {
    rgb_t c;
    c.r = r;
    c.g = g;
    c.b = b;
    return c;
}

static rgb_t rgb_hex(uint32_t v) {
    return rgb((float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f, (float)(v & 255) / 255.0f);
}

static rgb_t mix(rgb_t a, rgb_t b, float t) { return rgb(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t); }

static rgb_t scale(rgb_t a, float k) { return rgb(a.r * k, a.g * k, a.b * k); }

static rgb_t add(rgb_t a, float k) { return rgb(a.r + k, a.g + k, a.b + k); }

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static float smooth(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

static rgb_t desaturate(rgb_t c, float k) {
    float l = c.r * 0.3f + c.g * 0.59f + c.b * 0.11f;
    return mix(c, rgb(l, l, l), k);
}

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static void make_noise(float* out) {
    static const int periods[] = { 4, 8, 16, 32, 64 };
    float amp = 1.0f, lo = 1e9f, hi = -1e9f;
    memset(out, 0, sizeof(float) * TILE * TILE);
    for (int o = 0; o < 5; o++, amp *= 0.5f) {
        int n = periods[o], cell = TILE / n;
        for (int y = 0; y < TILE; y++) {
            int cy = y / cell;
            float fy = (float)(y % cell) / cell;
            fy = fy * fy * (3 - 2 * fy);
            for (int x = 0; x < TILE; x++) {
                int cx = x / cell;
                float fx = (float)(x % cell) / cell, v[4];
                fx = fx * fx * (3 - 2 * fx);
                for (int k = 0; k < 4; k++) {
                    int lx = (cx + (k & 1)) % n, ly = (cy + (k >> 1)) % n;
                    v[k] = (float)(hash32((uint32_t)(lx * 73856093) ^ (uint32_t)(ly * 19349663) ^ (uint32_t)(o * 83492791)) &
                                   0xFFFF) / 65535.0f;
                }
                out[y * TILE + x] += amp * ((v[0] + (v[1] - v[0]) * fx) * (1 - fy) + (v[2] + (v[3] - v[2]) * fx) * fy);
            }
        }
    }
    for (int i = 0; i < TILE * TILE; i++) {
        if (out[i] < lo) lo = out[i];
        if (out[i] > hi) hi = out[i];
    }
    for (int i = 0; i < TILE * TILE; i++) out[i] = (out[i] - lo) / (hi - lo);
}

static float noise_at(const fe_painter_t* p, float x, float y) {
    int fx = (int)x, fy = (int)y;
    int ix, iy, jx, jy;
    float tx, ty, a, b;
    const float* n = p->noise;
    if (x < (float)fx) fx--;
    if (y < (float)fy) fy--;
    tx = x - (float)fx;
    ty = y - (float)fy;
    ix = fx & (TILE - 1);
    iy = fy & (TILE - 1);
    jx = (ix + 1) & (TILE - 1);
    jy = (iy + 1) & (TILE - 1);
    a = n[iy * TILE + ix] + (n[iy * TILE + jx] - n[iy * TILE + ix]) * tx;
    b = n[jy * TILE + ix] + (n[jy * TILE + jx] - n[jy * TILE + ix]) * tx;
    return a + (b - a) * ty;
}

static float nz(const fe_painter_t* p, double u, double v, double size, uint32_t seed) {
    float k = (float)(TILE / size);
    return noise_at(p, (float)u * k + (float)(seed & 0xFF) * 13.7f, (float)v * k + (float)((seed >> 8) & 0xFF) * 7.3f);
}

static void put(uint32_t* d, rgb_t c, float a) {
    uint32_t v;
    float ia, r, g, b, al;
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    v = *d;
    ia = 1.0f - a;
    r = clamp01(c.r) * 255.0f * a + (float)((v >> 16) & 255) * ia;
    g = clamp01(c.g) * 255.0f * a + (float)((v >> 8) & 255) * ia;
    b = clamp01(c.b) * 255.0f * a + (float)(v & 255) * ia;
    al = 255.0f * a + (float)(v >> 24) * ia;
    *d = ((uint32_t)(al + 0.5f) << 24) | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}

/* Rounded rectangle: signed distance (negative inside) and the outward normal. */
static double rrect(const op_t* o, double px, double py, double* nx, double* ny) {
    double r = o->radius, cx = (o->x0 + o->x1) * 0.5, cy = (o->y0 + o->y1) * 0.5;
    double hx = (o->x1 - o->x0) * 0.5 - r, hy = (o->y1 - o->y0) * 0.5 - r;
    double dx = px - cx, dy = py - cy, qx = fabs(dx) - hx, qy = fabs(dy) - hy;
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, out = sqrt(ox * ox + oy * oy);
    double in = qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0);
    if (out > 0) {
        *nx = ox / out * (dx < 0 ? -1 : 1);
        *ny = oy / out * (dy < 0 ? -1 : 1);
    } else if (qx > qy) {
        *nx = dx < 0 ? -1 : 1;
        *ny = 0;
    } else {
        *nx = 0;
        *ny = dy < 0 ? -1 : 1;
    }
    return out + in - r;
}

static float coverage(double d) { return clamp01((float)(0.5 - d)); }

static float facing(double nx, double ny) { return (float)(-0.55 * nx - 0.83 * ny); }

/* edge: 0 at a plate's border, 1 well inside it (more rust and grime towards the edges). */
static rgb_t material(const fe_painter_t* p, int m, double u, double v, double x, double y, uint32_t seed, float edge) {
    float large = nz(p, u, v, 420, seed), mid = nz(p, u, v, 64, seed + 0x51);
    float fine = noise_at(p, (float)x * 2.1f, (float)y * 2.1f) - 0.5f;
    switch (m) {
    case MAT_IRON: {
        rgb_t c = scale(rgb(0.19f, 0.17f, 0.15f), 0.78f + 0.42f * large + 0.18f * mid);
        float rust = smooth(0.58f, 0.78f, nz(p, u, v, 150, seed + 0x77));
        c = mix(c, scale(rgb(0.36f, 0.17f, 0.07f), 0.7f + 0.6f * mid), rust * 0.65f);
        return add(c, fine * 0.035f);
    }
    case MAT_BRASS: {
        float brush = noise_at(p, (float)u * 0.3f + (float)(seed & 31), (float)v * 21.0f), tarnish, pit;
        rgb_t c = scale(rgb(0.40f, 0.31f, 0.19f), 0.74f + 0.40f * large + 0.10f * mid);
        c = scale(c, 0.95f + 0.09f * brush);
        tarnish = smooth(0.42f, 0.95f, nz(p, u, v, 260, seed + 0x33)) + (1.0f - edge) * 0.35f;
        c = mix(c, scale(rgb(0.19f, 0.18f, 0.13f), 0.8f + 0.4f * mid), clamp01(tarnish) * 0.5f);
        pit = smooth(0.74f, 0.88f, noise_at(p, (float)x * 1.1f + 41, (float)y * 1.1f + 9));
        c = scale(c, 1.0f - pit * 0.18f);
        return add(c, fine * 0.035f);
    }
    case MAT_STEEL: {
        rgb_t c = scale(rgb(0.36f, 0.355f, 0.34f), 0.70f + 0.38f * large + 0.14f * mid);
        float drip = nz(p, u * 3.0, v * 0.25, 64, seed + 0x2F);
        float rust = smooth(0.46f, 0.82f, nz(p, u, v, 90, seed + 0x91) + (1.0f - edge) * 0.30f);
        float speck = smooth(0.64f, 0.78f, noise_at(p, (float)x * 0.8f + 13, (float)y * 0.8f + 57));
        float pit = smooth(0.70f, 0.86f, noise_at(p, (float)x * 1.3f + 91, (float)y * 1.3f + 17));
        c = scale(c, 0.92f + 0.16f * drip);
        c = mix(c, scale(rgb(0.37f, 0.17f, 0.07f), 0.55f + 0.8f * mid), clamp01(rust * 0.75f + speck * 0.30f));
        c = mix(c, rgb(0.10f, 0.09f, 0.08f), (1.0f - edge) * 0.30f);
        c = scale(c, 1.0f - pit * 0.25f);
        return add(c, fine * 0.05f);
    }
    case MAT_COPPER: {
        rgb_t c = scale(rgb(0.66f, 0.45f, 0.27f), 0.72f + 0.38f * large + 0.12f * mid);
        float patina = smooth(0.60f, 0.85f, nz(p, u, v, 200, seed + 0x15));
        c = mix(c, rgb(0.30f, 0.38f, 0.30f), patina * 0.35f);
        return add(c, fine * 0.04f);
    }
    default:
        return rgb(0.5f, 0.5f, 0.5f);
    }
}

static void canvas_uv(const fe_painter_t* p, int x, int y, double* u, double* v) {
    *u = (x + 0.5 - p->view.x0) / p->view.sx;
    *v = (y + 0.5 - p->view.y0) / p->view.sy;
}

#define FOR_PIXELS(b, o, r)                                                                                             \
    fe_rect_t r = fe_rect_intersect((b)->clip, (o)->bounds);                                                            \
    for (int y = r.top; y < r.bottom; y++)                                                                              \
        for (int x = r.left; x < r.right; x++)

#define PIXEL(b, x, y) ((b)->px + (long)(y) * (b)->pitch + (x))

static void run_shadow(const band_t* b, const op_t* o) {
    double nx, ny;
    FOR_PIXELS(b, o, r) {
        double d = rrect(o, x + 0.5, y + 0.5, &nx, &ny);
        float a = o->alpha * (1.0f - smooth((float)(-o->width * 0.35), (float)o->width, (float)d));
        put(PIXEL(b, x, y), k_black, a);
    }
}

/* Torn plates (pts[0] bite depth, pts[1] chipped corner leg, pts[2] which corner): the edge only ever moves in, so a
 * plate never covers more than its rectangle. */
static double torn(const fe_painter_t* p, const op_t* o, double px, double py, double u, double v, double d) {
    double bite = o->pts[0], leg = o->pts[1];
    if (bite > 0 && d > -bite * 2.5) d += bite * smooth(0.42f, 0.86f, nz(p, u, v, 26, o->seed + 0x4D));
    if (leg > 0) {
        int corner = (int)o->pts[2] & 3;
        double cx = corner & 1 ? o->x1 : o->x0, cy = corner & 2 ? o->y1 : o->y0;
        double cut = (leg - (fabs(px - cx) + fabs(py - cy))) * 0.7071;
        if (cut > d) d = cut;
    }
    return d;
}

static void run_plate(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    double nx, ny, u, v, bevel = o->width;
    FOR_PIXELS(b, o, r) {
        double d, e;
        float cov, shade;
        rgb_t c;
        canvas_uv(p, x, y, &u, &v);
        d = torn(p, o, x + 0.5, y + 0.5, u, v, rrect(o, x + 0.5, y + 0.5, &nx, &ny));
        e = -d;
        cov = coverage(d);
        if (cov <= 0.0f) continue;
        c = material(p, o->material, u, v, x, y, o->seed, smooth(0.0f, (float)(o->unit * 7.0), (float)e));
        shade = e < bevel ? facing(nx, ny) * (float)(1.0 - e / bevel) : 0.0f;
        if (o->flags & F_RECESSED) shade = -shade;
        c = scale(c, (1.0f + 0.55f * shade) * (1.06f - 0.14f * (float)((y + 0.5 - o->y0) / (o->y1 - o->y0))));
        if (o->flags & F_AO) c = scale(c, 0.62f + 0.38f * smooth(0.0f, (float)(o->unit * 14.0), (float)e));
        if (e < 1.2) c = scale(c, 0.55f + 0.45f * (float)(e / 1.2));
        c = scale(c, o->light);
        if (o->flags & F_DISABLED) c = desaturate(scale(c, 0.6f), 0.6f);
        put(PIXEL(b, x, y), c, cov * o->alpha);
    }
}

static void run_well(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    double nx, ny, u, v, unit = o->unit, depth = unit * 6.0;
    FOR_PIXELS(b, o, r) {
        double d = rrect(o, x + 0.5, y + 0.5, &nx, &ny), e = -d;
        float cov = coverage(d), lit;
        rgb_t c;
        if (cov <= 0.0f) continue;
        canvas_uv(p, x, y, &u, &v);
        c = scale(o->color, 0.85f + 0.3f * nz(p, u, v, 300, o->seed));
        if (o->flags & F_GRILLE) {
            double gx = u / 3.4, gy = v / 2.95, row = floor(gy), cx = gx - floor(gx + (fmod(row, 2.0) ? 0.5 : 0.0)) - 0.5;
            double cy = gy - row - 0.5, hole = sqrt(cx * cx * 11.56 + cy * cy * 8.7) - 0.95;
            float h = clamp01((float)(0.5 - hole * unit));
            c = mix(c, scale(c, 0.35f), h);
            c = mix(c, add(c, 0.05f), clamp01((float)(0.5 - fabs(hole - 0.25) * unit)) * 0.5f);
        }
        lit = facing(nx, ny);
        if (e < depth) c = scale(c, 1.0f - (lit > 0 ? lit : 0) * 0.7f * (float)(1.0 - e / depth));
        if (e < unit * 1.2 && lit < 0) c = mix(c, rgb(0.55f, 0.45f, 0.30f), -lit * 0.55f * (float)(1.0 - e / (unit * 1.2)));
        if ((o->flags & F_HOVER) && e < unit * 1.1) c = mix(c, o->color2, 0.85f * (float)(1.0 - e / (unit * 1.1)));
        put(PIXEL(b, x, y), c, cov * o->alpha);
    }
}

static void run_rivet(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    double cx = o->x0, cy = o->y0, rad = o->radius;
    FOR_PIXELS(b, o, r) {
        double dx = x + 0.5 - cx, dy = y + 0.5 - cy, dist = sqrt(dx * dx + dy * dy);
        double sx = dx - rad * 0.35, sy = dy - rad * 0.45, sd = sqrt(sx * sx + sy * sy);
        float shadow = 1.0f - smooth((float)(rad * 0.7), (float)(rad * 1.35), (float)sd), cov, diffuse, spec, nzc;
        double u, v, n[3], h[3] = { -0.30, -0.40, 0.87 };
        rgb_t c;
        put(PIXEL(b, x, y), k_black, shadow * 0.55f * o->alpha);
        cov = clamp01((float)(rad + 0.5 - dist));
        if (cov <= 0.0f) continue;
        n[0] = dx / rad;
        n[1] = dy / rad;
        nzc = (float)(1.0 - n[0] * n[0] - n[1] * n[1]);
        n[2] = nzc > 0 ? sqrt(nzc) : 0;
        diffuse = (float)clampd(-0.45 * n[0] - 0.55 * n[1] + 0.70 * n[2], 0, 1);
        spec = (float)pow(clampd(n[0] * h[0] + n[1] * h[1] + n[2] * h[2], 0, 1), 24);
        canvas_uv(p, x, y, &u, &v);
        c = material(p, o->material, u * 3.0, v * 3.0, x, y, o->seed, 1.0f);
        c = add(scale(c, 0.35f + 0.95f * diffuse), spec * 0.45f);
        if (dist > rad - 1.2) c = scale(c, 0.6f);
        put(PIXEL(b, x, y), scale(c, o->light), cov * o->alpha);
    }
}

static float glyph_at(const glyphs_t* g, int gx, int gy) {
    if (gx < 0 || gy < 0 || gx >= g->w || gy >= g->h) return 0.0f;
    return g->a[gy * g->w + gx] / 255.0f;
}

static void run_text(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    const glyphs_t* g = o->glyphs;
    int ox = (int)floor(o->x0 + 0.5), oy = (int)floor(o->y0 + 0.5);
    FOR_PIXELS(b, o, r) {
        int gx = x - ox, gy = y - oy;
        float a = glyph_at(g, gx, gy);
        if (o->flags & F_BLUR)
            a = (a * 4.0f + glyph_at(g, gx - 1, gy) + glyph_at(g, gx + 1, gy) + glyph_at(g, gx, gy - 1) +
                 glyph_at(g, gx, gy + 1)) / 8.0f;
        if (a <= 0.0f) continue;
        if (o->flags & F_WEAR) {
            double u, v;
            canvas_uv(p, x, y, &u, &v);
            a *= 1.0f - (float)o->width + (float)o->width * smooth(0.18f, 0.55f, nz(p, u, v, o->radius, o->seed));
        }
        put(PIXEL(b, x, y), o->color, a * o->alpha);
    }
}

static double segment_distance(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay, t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = clampd(t, 0, 1);
    wx -= vx * t;
    wy -= vy * t;
    return sqrt(wx * wx + wy * wy);
}

static void run_bar(const band_t* b, const op_t* o) {
    double nx, ny, h = o->y1 - o->y0;
    FOR_PIXELS(b, o, r) {
        double d = rrect(o, x + 0.5, y + 0.5, &nx, &ny), e = -d;
        float cov = coverage(d), t = (float)((y + 0.5 - o->y0) / (h > 1 ? h : 1));
        rgb_t c = o->color;
        if (cov <= 0.0f) continue;
        if (o->flags & F_GRADIENT) c = mix(add(o->color, 0.12f), scale(o->color, 0.62f), t);
        if (o->flags & F_RECESSED) {
            float lit = facing(nx, ny);
            if (e < o->width && lit > 0) c = scale(c, 1.0f - 0.65f * lit * (float)(1.0 - e / o->width));
            if (e < 1.2 && lit < 0) c = mix(c, rgb(0.6f, 0.5f, 0.35f), -lit * 0.6f);
        }
        put(PIXEL(b, x, y), c, cov * o->alpha);
    }
}

static float cubic_w(float t) {
    float a = fabsf(t);
    if (a < 1.0f) return 1.5f * a * a * a - 2.5f * a * a + 1.0f;
    if (a < 2.0f) return -0.5f * a * a * a + 2.5f * a * a - 4.0f * a + 2.0f;
    return 0.0f;
}

static void texel565(uint16_t v, float* out) {
    out[0] = (float)((v >> 11) & 31) / 31.0f;
    out[1] = (float)((v >> 5) & 63) / 63.0f;
    out[2] = (float)(v & 31) / 31.0f;
    out[3] = 1.0f;
}

static void bilinear32(const op_t* o, double sx, double sy, float* acc) {
    double fx = clampd(sx - 0.5, 0, o->image_w - 1.001), fy = clampd(sy - 0.5, 0, o->image_h - 1.001);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(fx - ix), ty = (float)(fy - iy);
    const uint32_t* r0 = o->image32 + (long)iy * o->image_pitch + ix;
    const uint32_t* r1 = r0 + o->image_pitch;
    for (int k = 0; k < 3; k++) {
        int sh = 16 - 8 * k;
        float a = (float)((r0[0] >> sh) & 255) + ((float)((r0[1] >> sh) & 255) - (float)((r0[0] >> sh) & 255)) * tx;
        float c = (float)((r1[0] >> sh) & 255) + ((float)((r1[1] >> sh) & 255) - (float)((r1[0] >> sh) & 255)) * tx;
        acc[k] += (a + (c - a) * ty) / 255.0f;
    }
}

static float grade(float c) { return clamp01(c) * (0.93f + 0.07f * clamp01(c)); }

/* A grid of RGB cells at normalised coordinates, clamped, eased between cells. */
static void grid_sample(const float* grid, int gw, int gh, double nu, double nv, float* c) {
    double fx = clampd(nu * gw - 0.5, 0, gw - 1.001), fy = clampd(nv * gh - 0.5, 0, gh - 1.001);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(fx - ix), ty = (float)(fy - iy);
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    for (int k = 0; k < 3; k++) {
        const float* g = grid + k;
        float a = g[(iy * gw + ix) * 3] + (g[(iy * gw + ix + 1) * 3] - g[(iy * gw + ix) * 3]) * tx;
        float d = g[((iy + 1) * gw + ix) * 3] + (g[((iy + 1) * gw + ix + 1) * 3] - g[((iy + 1) * gw + ix) * 3]) * tx;
        c[k] = a + (d - a) * ty;
    }
}

static double mirror01(double t) {
    t = fmod(fabs(t), 2.0);
    return t > 1.0 ? 2.0 - t : t;
}

/* The surroundings of the framed picture (pts: its screen box): the picture's own edges carried outwards, blurred and
 * mirrored, turning within a fifth of its height into a field of its colours with no shape left, darker further out. */
static void run_ambient(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    float cxs = p->view.screen_w * 0.5f, cys = p->view.screen_h * 0.5f, rmax = sqrtf(cxs * cxs + cys * cys);
    double pw = o->pts[2] - o->pts[0], ph = o->pts[3] - o->pts[1];
    FOR_PIXELS(b, o, r) {
        float near_[3], far_[3], c[3], lum, t, vig, grain, dx = x + 0.5f - cxs, dy = y + 0.5f - cys;
        double u = (x + 0.5 - o->pts[0]) / pw, v = (y + 0.5 - o->pts[1]) / ph;
        double ox = u < 0 ? -u * pw / ph : u > 1 ? (u - 1) * pw / ph : 0, oy = v < 0 ? -v : v > 1 ? v - 1 : 0;
        float out = (float)sqrt(ox * ox + oy * oy);
        rgb_t col;
        grid_sample(p->photo_blur, BLUR_W, BLUR_H, mirror01(u), mirror01(v), near_);
        grid_sample(p->photo_field, FIELD_W, FIELD_H, mirror01(u), mirror01(v), far_);
        t = smooth(0.0f, 0.2f, out);
        for (int k = 0; k < 3; k++) c[k] = near_[k] + (far_[k] - near_[k]) * t;
        lum = 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];
        col = scale(mix(rgb(c[0], c[1], c[2]), rgb(lum, lum * 0.97f, lum * 0.9f), 0.25f), 0.56f - 0.24f * smooth(0.0f, 0.9f, out));
        vig = 1.0f - 0.40f * smooth(0.45f, 1.1f, sqrtf(dx * dx + dy * dy) / rmax);
        grain = (noise_at(p, x * 1.9f + 33, y * 1.9f + 71) - 0.5f) * 0.03f;
        put(PIXEL(b, x, y), add(scale(col, vig), grain), 1.0f);
    }
}

/* The framed picture: filtered from the prepared picture, with grain at the screen's resolution and a little shading
 * towards the frame. */
static void run_photo(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    double kx = (o->src[2] - o->src[0]) / (o->x1 - o->x0), ky = (o->src[3] - o->src[1]) / (o->y1 - o->y0);
    double edge = o->unit * 26.0;
    int footprint = kx > 1.25 || ky > 1.25;
    FOR_PIXELS(b, o, r) {
        float acc[3] = { 0, 0, 0 }, shade, grain;
        double sx = o->src[0] + (x + 0.5 - o->x0) * kx, sy = o->src[1] + (y + 0.5 - o->y0) * ky;
        double e = x + 0.5 - o->x0;
        if (o->x1 - (x + 0.5) < e) e = o->x1 - (x + 0.5);
        if (y + 0.5 - o->y0 < e) e = y + 0.5 - o->y0;
        if (o->y1 - (y + 0.5) < e) e = o->y1 - (y + 0.5);
        if (footprint) {
            bilinear32(o, sx - kx * 0.25, sy - ky * 0.25, acc);
            bilinear32(o, sx + kx * 0.25, sy - ky * 0.25, acc);
            bilinear32(o, sx - kx * 0.25, sy + ky * 0.25, acc);
            bilinear32(o, sx + kx * 0.25, sy + ky * 0.25, acc);
            for (int i = 0; i < 3; i++) acc[i] *= 0.25f;
        } else {
            bilinear32(o, sx, sy, acc);
        }
        shade = 0.80f + 0.20f * smooth(0.0f, (float)edge, (float)e);
        grain = (noise_at(p, x * 2.3f + 17, y * 2.3f + 5) - 0.5f) * 0.035f;
        put(PIXEL(b, x, y), add(scale(rgb(grade(acc[0]), grade(acc[1]), grade(acc[2])), shade), grain), 1.0f);
    }
}

static void run_fill(const band_t* b, const op_t* o);
static void run_wide(const band_t* b, const op_t* o);

static void run_op(const band_t* b, const op_t* o) {
    switch (o->kind) {
    case OP_SHADOW: run_shadow(b, o); break;
    case OP_PLATE: run_plate(b, o); break;
    case OP_WELL: run_well(b, o); break;
    case OP_RIVET: run_rivet(b, o); break;
    case OP_TEXT: run_text(b, o); break;
    case OP_GROOVE:
    case OP_BAR: run_bar(b, o); break;
    case OP_AMBIENT: run_ambient(b, o); break;
    case OP_PHOTO: run_photo(b, o); break;
    case OP_ART: fa_blit_over(b->px, b->pitch, b->clip, o->art, o->art_x, o->art_y); break;
    case OP_WIDE: run_wide(b, o); break;
    case OP_FILL: run_fill(b, o); break;
    default: break;
    }
}

static const char* const k_faces[FE_FONT_COUNT][4] = {
    { "Stencil", "Rockwell Extra Bold", "Georgia", "Times New Roman" },
    { "Bahnschrift SemiBold", "Bahnschrift", "Franklin Gothic Medium", "Arial" },
    { "Bahnschrift", "Segoe UI", "Arial", "Tahoma" },
    { "Arial", "Segoe UI", "Tahoma", "Microsoft Sans Serif" },
};

static int face_exists(HDC dc, const char* face) {
    char got[LF_FACESIZE];
    HFONT f = CreateFontA(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ old;
    int same;
    if (!f) return 0;
    old = SelectObject(dc, f);
    got[0] = 0;
    GetTextFaceA(dc, sizeof got, got);
    SelectObject(dc, old);
    DeleteObject(f);
    same = !_stricmp(got, face);
    return same;
}

static HFONT font_for(fe_painter_t* p, int role, int px, int weight) {
    font_entry_t* slot = NULL;
    for (int i = 0; i < FONT_CACHE; i++) {
        font_entry_t* f = &p->fonts[i];
        if (f->font && f->role == role && f->px == px && f->weight == weight) {
            f->stamp = ++p->stamp;
            return f->font;
        }
        if (!slot || !f->font || (slot->font && f->stamp < slot->stamp)) slot = f;
    }
    if (slot->font) DeleteObject(slot->font);
    slot->font = CreateFontA(-px, 0, 0, 0, weight + p->boost[role] > 900 ? 900 : weight + p->boost[role], 0, 0, 0,
                             ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, p->faces[role]);
    slot->role = role;
    slot->px = px;
    slot->weight = weight;
    slot->stamp = ++p->stamp;
    return slot->font;
}

static int ensure_dib(fe_painter_t* p, int w, int h) {
    BITMAPINFO bi;
    void* bits = NULL;
    if (p->dib && w <= p->dib_w && h <= p->dib_h) return 1;
    if (w < p->dib_w) w = p->dib_w;
    if (h < p->dib_h) h = p->dib_h;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (p->dib) DeleteObject(p->dib);
    p->dib = CreateDIBSection(p->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!p->dib) {
        p->dib_w = p->dib_h = 0;
        return 0;
    }
    SelectObject(p->dc, p->dib);
    p->dib_bits = (uint32_t*)bits;
    p->dib_w = w;
    p->dib_h = h;
    return 1;
}

static int text_extent(fe_painter_t* p, int role, int px, int weight, int spacing, const char* text, int len, SIZE* size) {
    HGDIOBJ old = SelectObject(p->dc, font_for(p, role, px, weight));
    int ok;
    SetTextCharacterExtra(p->dc, spacing);
    ok = GetTextExtentPoint32A(p->dc, text, len, size);
    SelectObject(p->dc, old);
    return ok;
}

static const glyphs_t* glyphs(fe_painter_t* p, int role, int px, int weight, int spacing, const char* text, int len) {
    uint32_t key = fe_hash(0, text, (size_t)len);
    int params[4];
    glyphs_t* slot = NULL;
    TEXTMETRICA tm;
    SIZE size;
    HGDIOBJ old;
    int w, h, pad;
    params[0] = role;
    params[1] = px;
    params[2] = weight;
    params[3] = spacing;
    key = fe_hash(key, params, sizeof params);
    for (int i = 0; i < GLYPH_CACHE; i++) {
        glyphs_t* g = &p->glyphs[i];
        if (g->a && g->key == key) {
            g->stamp = ++p->stamp;
            return g;
        }
        if (!slot || !g->a || (slot->a && g->stamp < slot->stamp)) slot = g;
    }
    if (px < 4 || len <= 0) return NULL;
    old = SelectObject(p->dc, font_for(p, role, px, weight));
    SetTextCharacterExtra(p->dc, spacing);
    if (!GetTextExtentPoint32A(p->dc, text, len, &size) || !GetTextMetricsA(p->dc, &tm)) {
        SelectObject(p->dc, old);
        return NULL;
    }
    pad = px / 4 + 2;
    w = size.cx + 2 * pad;
    h = tm.tmHeight + 2 * pad;
    if (w > 8192 || h > 2048 || !ensure_dib(p, w, h)) {
        SelectObject(p->dc, old);
        return NULL;
    }
    SelectObject(p->dc, font_for(p, role, px, weight));
    for (int y = 0; y < h; y++) memset(p->dib_bits + (long)y * p->dib_w, 0, (size_t)w * 4);
    SetBkMode(p->dc, TRANSPARENT);
    SetTextColor(p->dc, RGB(255, 255, 255));
    SetTextAlign(p->dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    TextOutA(p->dc, pad, pad, text, len);
    GdiFlush();
    SelectObject(p->dc, old);
    free(slot->a);
    slot->a = (uint8_t*)malloc((size_t)w * h);
    if (!slot->a) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) slot->a[y * w + x] = (uint8_t)((p->dib_bits[(long)y * p->dib_w + x] >> 8) & 255);
    slot->key = key;
    slot->stamp = ++p->stamp;
    slot->w = w;
    slot->h = h;
    slot->pad = pad;
    slot->advance = size.cx;
    slot->line_h = tm.tmHeight;
    return slot;
}

const char* fe_painter_face(const fe_painter_t* p, int role) { return role >= 0 && role < FE_FONT_COUNT ? p->faces[role] : ""; }

fe_painter_t* fe_painter_create(void) {
    fe_painter_t* p = (fe_painter_t*)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->dc = CreateCompatibleDC(NULL);
    if (!p->dc) {
        free(p);
        return NULL;
    }
    for (int role = 0; role < FE_FONT_COUNT; role++) {
        int found = 0;
        for (int i = 0; i < 4 && !found; i++)
            if (face_exists(p->dc, k_faces[role][i])) {
                strncpy(p->faces[role], k_faces[role][i], LF_FACESIZE - 1);
                p->boost[role] = role == FE_FONT_PLAQUE && i >= 2 ? 300 : 0;
                found = 1;
            }
        if (!found) {
            strncpy(p->faces[role], "Arial", LF_FACESIZE - 1);
            p->boost[role] = role == FE_FONT_PLAQUE ? 300 : 0;
        }
    }
    make_noise(p->noise);
    p->art = fa_cache_create(ART_BUDGET);
    if (!p->art || !ensure_dib(p, 512, 128)) {
        fe_painter_destroy(p);
        return NULL;
    }
    return p;
}

void fe_painter_destroy(fe_painter_t* p) {
    if (!p) return;
    for (int i = 0; i < FONT_CACHE; i++)
        if (p->fonts[i].font) DeleteObject(p->fonts[i].font);
    for (int i = 0; i < GLYPH_CACHE; i++) free(p->glyphs[i].a);
    if (p->dc) DeleteDC(p->dc);
    if (p->dib) DeleteObject(p->dib);
    free(p->up);
    free(p->blur);
    free(p->base);
    free(p->photo);
    free(p->photo_blur);
    free(p->photo_field);
    fa_cache_destroy(p->art);
    free(p);
}

/* Catmull-Rom up-sampling by UP of w x h RGB texels (0..1) into BGRA rows of w * UP pixels. */
static void up_rgb(const float* rgb, int w, int h, uint32_t* out) {
    const int ow = w * UP, oh = h * UP;
    for (int y = 0; y < oh; y++) {
        double sy = (y + 0.5) / UP;
        int iy = (int)floor(sy - 0.5);
        float fy = (float)(sy - 0.5 - iy);
        for (int x = 0; x < ow; x++) {
            double sx = (x + 0.5) / UP;
            int ix = (int)floor(sx - 0.5);
            float fx = (float)(sx - 0.5 - ix), acc[3] = { 0, 0, 0 };
            for (int j = -1; j <= 2; j++) {
                int yy = iy + j < 0 ? 0 : iy + j >= h ? h - 1 : iy + j;
                float wy = cubic_w((float)j - fy);
                for (int i = -1; i <= 2; i++) {
                    int xx = ix + i < 0 ? 0 : ix + i >= w ? w - 1 : ix + i;
                    float wgt = wy * cubic_w((float)i - fx);
                    const float* t = rgb + ((long)yy * w + xx) * 3;
                    acc[0] += t[0] * wgt;
                    acc[1] += t[1] * wgt;
                    acc[2] += t[2] * wgt;
                }
            }
            out[(long)y * ow + x] = ((uint32_t)(clamp01(acc[0]) * 255.0f + 0.5f) << 16) |
                                    ((uint32_t)(clamp01(acc[1]) * 255.0f + 0.5f) << 8) | (uint32_t)(clamp01(acc[2]) * 255.0f + 0.5f);
        }
    }
}

/* The 1998 pictures are dithered 15-bit renders: each texel is averaged with the neighbours within a couple of 5-bit
 * steps of it, so the dither goes and edges stay. */
#define DITHER_STEP 0.07f

static void dedither(const uint16_t* src, int pitch, int w, int h, float* rgb) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float c[4], sum[3], n = 2.0f;
            texel565(src[(long)y * pitch + x], c);
            for (int k = 0; k < 3; k++) sum[k] = c[k] * 2.0f;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    int xx = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i, yy = y + j < 0 ? 0 : y + j >= h ? h - 1 : y + j;
                    float t[4];
                    if (!i && !j) continue;
                    texel565(src[(long)yy * pitch + xx], t);
                    if (fabsf(t[0] - c[0]) > DITHER_STEP || fabsf(t[1] - c[1]) > DITHER_STEP || fabsf(t[2] - c[2]) > DITHER_STEP)
                        continue;
                    for (int k = 0; k < 3; k++) sum[k] += t[k];
                    n += 1.0f;
                }
            for (int k = 0; k < 3; k++) rgb[((long)y * w + x) * 3 + k] = sum[k] / n;
        }
}

/* BLUR_W x BLUR_H cell averages of a BGRA picture, each over twice its cell, then smoothed twice more. */
static int blur_grid(const uint32_t* px, int w, int h, float* grid) {
    float* tmp = (float*)malloc(sizeof(float) * BLUR_W * BLUR_H * 3);
    if (!tmp) return 0;
    for (int by = 0; by < BLUR_H; by++)
        for (int bx = 0; bx < BLUR_W; bx++) {
            int x0 = (int)((bx - 0.5) * w / BLUR_W), x1 = (int)((bx + 1.5) * w / BLUR_W);
            int y0 = (int)((by - 0.5) * h / BLUR_H), y1 = (int)((by + 1.5) * h / BLUR_H);
            int sx = (x1 - x0) / 12 > 1 ? (x1 - x0) / 12 : 1, sy = (y1 - y0) / 12 > 1 ? (y1 - y0) / 12 : 1;
            float acc[3] = { 0, 0, 0 }, n = 0;
            for (int y = y0; y < y1; y += sy)
                for (int x = x0; x < x1; x += sx) {
                    uint32_t v = px[(long)(y < 0 ? 0 : y >= h ? h - 1 : y) * w + (x < 0 ? 0 : x >= w ? w - 1 : x)];
                    acc[0] += (float)((v >> 16) & 255);
                    acc[1] += (float)((v >> 8) & 255);
                    acc[2] += (float)(v & 255);
                    n += 1.0f;
                }
            for (int k = 0; k < 3; k++) grid[(by * BLUR_W + bx) * 3 + k] = acc[k] / (n * 255.0f);
        }
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < BLUR_H; y++)
            for (int x = 0; x < BLUR_W; x++)
                for (int k = 0; k < 3; k++) {
                    int l = x > 0 ? x - 1 : 0, r = x + 1 < BLUR_W ? x + 1 : BLUR_W - 1;
                    tmp[(y * BLUR_W + x) * 3 + k] = (grid[(y * BLUR_W + l) * 3 + k] + 2.0f * grid[(y * BLUR_W + x) * 3 + k] +
                                                     grid[(y * BLUR_W + r) * 3 + k]) * 0.25f;
                }
        for (int y = 0; y < BLUR_H; y++)
            for (int x = 0; x < BLUR_W; x++)
                for (int k = 0; k < 3; k++) {
                    int t = y > 0 ? y - 1 : 0, b = y + 1 < BLUR_H ? y + 1 : BLUR_H - 1;
                    grid[(y * BLUR_W + x) * 3 + k] = (tmp[(t * BLUR_W + x) * 3 + k] + 2.0f * tmp[(y * BLUR_W + x) * 3 + k] +
                                                      tmp[(b * BLUR_W + x) * 3 + k]) * 0.25f;
                }
    }
    free(tmp);
    return 1;
}

/* FIELD_W x FIELD_H averages of the blur grid, smoothed across cells. */
static void colour_field(const float* blur, float* field) {
    const int bx = BLUR_W / FIELD_W, by = BLUR_H / FIELD_H;
    float tmp[FIELD_W * FIELD_H * 3];
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++)
            for (int k = 0; k < 3; k++) {
                float sum = 0;
                for (int j = 0; j < by; j++)
                    for (int i = 0; i < bx; i++) sum += blur[((y * by + j) * BLUR_W + x * bx + i) * 3 + k];
                field[(y * FIELD_W + x) * 3 + k] = sum / (float)(bx * by);
            }
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < FIELD_H; y++)
            for (int x = 0; x < FIELD_W; x++)
                for (int k = 0; k < 3; k++) {
                    int l = x > 0 ? x - 1 : 0, r = x + 1 < FIELD_W ? x + 1 : FIELD_W - 1;
                    tmp[(y * FIELD_W + x) * 3 + k] = (field[(y * FIELD_W + l) * 3 + k] + 2.0f * field[(y * FIELD_W + x) * 3 + k] +
                                                      field[(y * FIELD_W + r) * 3 + k]) * 0.25f;
                }
        for (int y = 0; y < FIELD_H; y++)
            for (int x = 0; x < FIELD_W; x++)
                for (int k = 0; k < 3; k++) {
                    int t = y > 0 ? y - 1 : 0, b = y + 1 < FIELD_H ? y + 1 : FIELD_H - 1;
                    field[(y * FIELD_W + x) * 3 + k] = (tmp[(t * FIELD_W + x) * 3 + k] + 2.0f * tmp[(y * FIELD_W + x) * 3 + k] +
                                                        tmp[(b * FIELD_W + x) * 3 + k]) * 0.25f;
                }
    }
}

static int prepare_photo(fe_painter_t* p, const ld_scene_t* s) {
    if (p->photo_ready && p->photo_key == s->art_key) return 1;
    p->photo_ready = 0;
    p->photo_src = NULL;
    if (!p->photo_blur) p->photo_blur = (float*)malloc(sizeof(float) * BLUR_W * BLUR_H * 3);
    if (!p->photo_field) p->photo_field = (float*)malloc(sizeof(float) * FIELD_W * FIELD_H * 3);
    if (!p->photo_blur || !p->photo_field || s->art_w <= 0 || s->art_h <= 0) return 0;
    if (s->art32) {
        free(p->photo);
        p->photo = NULL;
        p->photo_src = s->art32;
        p->photo_w = s->art_w;
        p->photo_h = s->art_h;
    } else if (s->art565 && s->art_pitch >= s->art_w && s->art_w <= 2048 && s->art_h <= 2048) {
        float* rgb = (float*)malloc(sizeof(float) * (size_t)s->art_w * s->art_h * 3);
        uint32_t* up = (uint32_t*)realloc(p->photo, sizeof(uint32_t) * (size_t)s->art_w * UP * s->art_h * UP);
        if (up) p->photo = up;
        if (!rgb || !up) {
            free(rgb);
            return 0;
        }
        dedither(s->art565, s->art_pitch, s->art_w, s->art_h, rgb);
        up_rgb(rgb, s->art_w, s->art_h, p->photo);
        free(rgb);
        p->photo_src = p->photo;
        p->photo_w = s->art_w * UP;
        p->photo_h = s->art_h * UP;
    } else {
        return 0;
    }
    if (!blur_grid(p->photo_src, p->photo_w, p->photo_h, p->photo_blur)) return 0;
    colour_field(p->photo_blur, p->photo_field);
    p->photo_key = s->art_key;
    p->photo_ready = 1;
    return 1;
}

static fe_rect_t bounds_of(double x0, double y0, double x1, double y1, double margin) {
    return fe_rect((int)floor(x0 - margin), (int)floor(y0 - margin), (int)ceil(x1 + margin), (int)ceil(y1 + margin));
}

static op_t* op(fe_painter_t* p, int kind, double x0, double y0, double x1, double y1, double margin) {
    op_t* o;
    if (p->op_count >= MAX_OPS) return NULL;
    o = &p->ops[p->op_count++];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->x0 = x0;
    o->y0 = y0;
    o->x1 = x1;
    o->y1 = y1;
    o->alpha = 1.0f;
    o->light = 1.0f;
    o->unit = p->view.sy;
    o->base = (uint8_t)p->marking_base;
    o->bounds = fe_rect_intersect(bounds_of(x0, y0, x1, y1, margin), fe_rect(0, 0, p->view.screen_w, p->view.screen_h));
    if (p->clipping) o->bounds = fe_rect_intersect(o->bounds, p->clip);
    return o;
}

typedef struct {
    double x0, y0, x1, y1;
} box_t;

static box_t box_of(const fe_painter_t* p, double l, double t, double r, double b) {
    box_t x;
    x.x0 = fe_view_x(&p->view, l);
    x.y0 = fe_view_y(&p->view, t);
    x.x1 = fe_view_x(&p->view, r);
    x.y1 = fe_view_y(&p->view, b);
    return x;
}

static box_t box_rect(const fe_painter_t* p, fe_rect_t r) { return box_of(p, r.left, r.top, r.right, r.bottom); }

static void add_text_line(fe_painter_t* p, int role, double px_h, int weight, double spacing_em, const char* text, int len,
                          double x, double y, rgb_t color, float alpha, int flags, uint32_t seed, double wear,
                          double wear_size) {
    int px = (int)floor(px_h + 0.5), spacing = (int)floor(spacing_em * px + 0.5);
    const glyphs_t* g = glyphs(p, role, px, weight, spacing, text, len);
    op_t* o;
    if (!g) return;
    o = op(p, OP_TEXT, x - g->pad, y - g->pad, x - g->pad + g->w, y - g->pad + g->h, 2);
    if (!o) return;
    o->glyphs = g;
    o->color = color;
    o->alpha = alpha;
    o->flags = (uint16_t)flags;
    o->seed = seed;
    o->width = wear;
    o->radius = wear_size;
}

typedef struct {
    int role, weight;
    double px, spacing;
    rgb_t color;
    int flags;     /* FE_TEXT_* */
    double wear, wear_size;
    double shadow; /* pixels; 0 none */
    double emboss; /* pixels of the light lower lip of stamped letters; 0 none */
    int fit;       /* one line made smaller to fit its box, and clipped to it */
    uint32_t seed;
    int exact;             /* the game's own text: clipped to its box, its shadow an offset copy */
    double shadow_dx;      /* pixels */
    rgb_t shadow_color;
} text_style_t;

static int measure(fe_painter_t* p, const text_style_t* st, const char* text, int len) {
    int px = (int)floor(st->px + 0.5);
    SIZE size;
    if (len <= 0) return 0;
    if (!text_extent(p, st->role, px, st->weight, (int)floor(st->spacing * px + 0.5), text, len, &size)) return 0;
    return size.cx;
}

static int line_height(fe_painter_t* p, const text_style_t* st) {
    int px = (int)floor(st->px + 0.5);
    TEXTMETRICA tm;
    HGDIOBJ old = SelectObject(p->dc, font_for(p, st->role, px, st->weight));
    int h = GetTextMetricsA(p->dc, &tm) ? tm.tmHeight : px;
    SelectObject(p->dc, old);
    return h;
}

static void emit_line(fe_painter_t* p, const text_style_t* st, const char* text, int len, double x, double y) {
    if (st->exact) {
        if (st->shadow_dx > 0)
            add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x + st->shadow_dx, y + st->shadow_dx,
                          st->shadow_color, 1.0f, 0, st->seed, 0, 0);
        add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x, y, st->color, 1.0f, 0, st->seed, 0, 0);
        return;
    }
    if (st->shadow > 0) {
        add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x + st->shadow, y + st->shadow, k_black,
                      0.85f, F_BLUR, st->seed, 0, 0);
        add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x + st->shadow * 0.5,
                      y + st->shadow * 0.5, k_black, 0.6f, 0, st->seed, 0, 0);
    }
    if (st->emboss > 0)
        add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x + st->emboss * 0.6, y + st->emboss,
                      rgb(1, 0.96f, 0.88f), 0.30f, F_BLUR, st->seed, 0, 0);
    add_text_line(p, st->role, st->px, st->weight, st->spacing, text, len, x, y, st->color, 1.0f,
                  st->wear > 0 ? F_WEAR : 0, st->seed, st->wear, st->wear_size);
}

static int split_lines(fe_painter_t* p, const text_style_t* st, const char* text, double width, int* starts, int* lens,
                       int max_lines) {
    int n = 0, at = 0, total = (int)strlen(text);
    while (at <= total && n < max_lines) {
        int end = at, best = -1;
        while (end < total && text[end] != '\n' && text[end] != '\r') end++;
        if ((st->flags & FE_TEXT_WRAP) && measure(p, st, text + at, end - at) > width) {
            for (int k = at; k < end; k++)
                if (text[k] == ' ' && measure(p, st, text + at, k - at) <= width) best = k;
            if (best > at) end = best;
        }
        starts[n] = at;
        lens[n] = end - at;
        n++;
        at = end;
        if (at < total && text[at] == '\r') at++;
        if (at < total && (text[at] == '\n' || text[at] == ' ')) at++;
        if (at >= total) break;
    }
    return n;
}

#define FIT_MIN 0.7

static void add_text(fe_painter_t* p, const text_style_t* style, const char* text, box_t b) {
    int starts[32], lens[32], n, lh;
    double y;
    text_style_t fitted = *style;
    const text_style_t* st = &fitted;
    if (!text || !*text) return;
    n = split_lines(p, st, text, b.x1 - b.x0, starts, lens, 32);
    if (st->fit && n == 1) {
        double width = measure(p, st, text, lens[0]), room = b.x1 - b.x0;
        if (width > room && width > 0) fitted.px *= room / width > FIT_MIN ? room / width : FIT_MIN;
        p->clip = fe_rect((int)floor(b.x0), (int)floor(b.y0 - st->shadow), (int)ceil(b.x1 + st->shadow + 1),
                          (int)ceil(b.y1 + st->shadow + 1));
        p->clipping = 1;
    }
    if (st->exact) {
        p->clip = fe_rect((int)floor(b.x0), (int)floor(b.y0), (int)ceil(b.x1 + st->shadow_dx), (int)ceil(b.y1 + st->shadow_dx));
        p->clipping = 1;
    }
    lh = line_height(p, st);
    if (st->flags & FE_TEXT_VCENTER) y = (b.y0 + b.y1 - (double)lh * n) * 0.5;
    else if (st->flags & FE_TEXT_BOTTOM) y = b.y1 - (double)lh * n;
    else y = b.y0;
    for (int i = 0; i < n; i++, y += lh) {
        double w = measure(p, st, text + starts[i], lens[i]), x = b.x0;
        if (st->flags & FE_TEXT_CENTER) x = (b.x0 + b.x1 - w) * 0.5;
        else if (st->flags & FE_TEXT_RIGHT) x = b.x1 - w;
        emit_line(p, st, text + starts[i], lens[i], floor(x + 0.5), floor(y + 0.5));
    }
    p->clipping = 0;
}

static void add_rivet(fe_painter_t* p, double cx, double cy, double radius, int mat, uint32_t seed, float light) {
    op_t* o = op(p, OP_RIVET, cx - radius, cy - radius, cx + radius, cy + radius, radius * 0.9 + 2);
    if (!o) return;
    o->radius = radius;
    o->material = (uint8_t)mat;
    o->seed = seed;
    o->light = light;
}

static op_t* add_plate(fe_painter_t* p, box_t b, double radius, double bevel, int mat, int flags, uint32_t seed) {
    op_t* o = op(p, OP_PLATE, b.x0, b.y0, b.x1, b.y1, 1);
    if (!o) return NULL;
    o->radius = radius;
    o->width = bevel;
    o->material = (uint8_t)mat;
    o->flags = (uint16_t)flags;
    o->seed = seed;
    return o;
}

static void add_shadow(fe_painter_t* p, box_t b, double dx, double dy, double blur, double radius, float alpha) {
    op_t* o = op(p, OP_SHADOW, b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy, blur + 1);
    if (!o) return;
    o->width = blur;
    o->radius = radius;
    o->alpha = alpha;
}

static op_t* add_well(fe_painter_t* p, box_t b, double radius, int flags, uint32_t seed) {
    op_t* o = op(p, OP_WELL, b.x0, b.y0, b.x1, b.y1, 1);
    if (!o) return NULL;
    o->radius = radius;
    o->color = rgb(0.062f, 0.056f, 0.05f);
    o->color2 = k_amber;
    o->flags = (uint16_t)flags;
    o->seed = seed;
    return o;
}

static void make_torn(op_t* o, double bite, double leg, uint32_t seed) {
    if (!o) return;
    o->pts[0] = bite;
    o->pts[1] = leg;
    o->pts[2] = (double)((seed >> 9) & 3);
}

static const char k_title[] = "TRESPASSER";
static const char k_footer1[] = "\xA9 1998 DreamWorks Interactive L.L.C. Trespasser is a trademark of UCS & Amblin.";
static const char k_footer2[] = "The Lost World: Jurassic Park\x99 & \xA9 1998 UCS & Amblin. All Rights Reserved.";

/* ---- The menus drawn from the game's own pictures ---------------------------------------------------------------
 * Every picture is the game's bitmap for that control and state, scaled with the canvas by the art cache and placed
 * where the game draws it; the text is real text at the screen's resolution, in the face the game uses. */

/* A bilinear sample (0..1 per channel) of an opaque picture at picture pixel (x, y), edges clamped. */
static void sample_bitmap(const fa_bitmap_t* bm, double x, double y, float* c) {
    double fx = clampd(x - 0.5, 0, bm->w - 1.001), fy = clampd(y - 0.5, 0, bm->h - 1.001);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(fx - ix), ty = (float)(fy - iy);
    const uint32_t* r0 = bm->px + (size_t)iy * bm->w + ix;
    const uint32_t* r1 = r0 + bm->w;
    for (int k = 0; k < 3; k++) {
        int sh = 16 - 8 * k;
        float a = (float)((r0[0] >> sh) & 255) + ((float)((r0[1] >> sh) & 255) - (float)((r0[0] >> sh) & 255)) * tx;
        float d = (float)((r1[0] >> sh) & 255) + ((float)((r1[1] >> sh) & 255) - (float)((r1[0] >> sh) & 255)) * tx;
        c[k] = (a + (d - a) * ty) / 255.0f;
    }
}

/* The main screen: the original art at its own 4:3 place, untouched; on a wider screen the jungle of the art carried
 * past both edges, mirrored, blurred and darker the further it goes. The title plank is never repeated. */
#define WIDE_JUNGLE_LEFT 178.0
#define WIDE_KEEP_BOTTOM 438.0

static void run_wide(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    const fa_bitmap_t* art = o->art;
    fe_rect_t r = fe_rect_intersect(b->clip, o->bounds);
    for (int y = r.top; y < r.bottom; y++) {
        double v = clampd((y + 0.5 - p->view.y0) / p->view.sy, 0, BACKDROP_H);
        for (int x = r.left; x < r.right; x++) {
            uint32_t* d = PIXEL(b, x, y);
            double u = (x + 0.5 - p->view.x0) / p->view.sx, mu = u, mv = v, outside = 0;
            float sharp[3], soft[3], k, dark;
            if (x >= o->art_x && x < o->art_x + art->w && y >= o->art_y && y < o->art_y + art->h) {
                *d = art->px[(size_t)(y - o->art_y) * art->w + (size_t)(x - o->art_x)];
                continue;
            }
            if (u < 0) {
                outside = -u;
                mu = WIDE_JUNGLE_LEFT + fmod(outside, BACKDROP_W - WIDE_JUNGLE_LEFT);
            } else if (u > BACKDROP_W) {
                outside = u - BACKDROP_W;
                mu = BACKDROP_W - fmod(outside, BACKDROP_W - WIDE_JUNGLE_LEFT);
            }
            if (outside > 0 && mv > WIDE_KEEP_BOTTOM) mv = WIDE_KEEP_BOTTOM;
            sample_bitmap(art, mu * p->view.sx, mv * p->view.sy, sharp);
            sample_bitmap(o->art2, mu / BACKDROP_W * o->art2->w, mv / BACKDROP_H * o->art2->h, soft);
            k = smooth(0.0f, 45.0f, (float)outside);
            dark = 1.0f - 0.6f * smooth(0.0f, 320.0f, (float)outside);
            *d = 0xFF000000u | ((uint32_t)(clamp01((sharp[0] + (soft[0] - sharp[0]) * k) * dark) * 255.0f + 0.5f) << 16) |
                 ((uint32_t)(clamp01((sharp[1] + (soft[1] - sharp[1]) * k) * dark) * 255.0f + 0.5f) << 8) |
                 (uint32_t)(clamp01((sharp[2] + (soft[2] - sharp[2]) * k) * dark) * 255.0f + 0.5f);
        }
    }
}

#define CAPTION_POINTS 12.0
#define TRACK_GREY 107
#define THUMB_GREY 148

static rgb_t grey(int v) { return rgb(v / 255.0f, v / 255.0f, v / 255.0f); }

static void run_fill(const band_t* b, const op_t* o) {
    FOR_PIXELS(b, o, r) put(PIXEL(b, x, y), o->color, o->alpha);
}

/* A solid rectangle at the pixels a canvas rectangle covers. */
static void add_fill(fe_painter_t* p, fe_rect_t canvas, rgb_t color, float alpha) {
    fe_rect_t d = fe_view_rect(&p->view, canvas);
    op_t* o;
    if (fe_rect_empty(d)) return;
    o = op(p, OP_FILL, d.left, d.top, d.right, d.bottom, 0);
    if (!o) return;
    o->color = color;
    o->alpha = alpha;
}

static void picture_source(const fe_widget_t* w, fa_source_t* s) {
    memset(s, 0, sizeof *s);
    s->bits = w->image;
    s->w = w->image_w;
    s->h = w->image_h;
    s->pitch = w->image_pitch;
    s->keyed = w->image_keyed;
    s->key = w->image_key;
    s->hash = w->image_hash;
}

static op_t* add_bitmap(fe_painter_t* p, const fa_bitmap_t* bm, fe_rect_t dest) {
    op_t* o;
    if (!bm) return NULL;
    o = op(p, OP_ART, dest.left, dest.top, dest.right, dest.bottom, 0);
    if (!o) return NULL;
    o->art = bm;
    o->art_x = dest.left;
    o->art_y = dest.top;
    return o;
}

/* The widget's picture with its corner at (x, y) of the canvas, at the game's size. */
static void add_picture(fe_painter_t* p, const fe_widget_t* w, int x, int y) {
    fa_source_t src;
    fe_rect_t d;
    if (!w->image || w->image_w <= 0 || w->image_h <= 0) return;
    picture_source(w, &src);
    d = fe_view_rect(&p->view, fe_rect(x, y, x + w->image_w, y + w->image_h));
    add_bitmap(p, fa_cache_get16(p->art, &src, NULL, d.right - d.left, d.bottom - d.top,
                                 FA_DEDITHER | (w->image_keyed ? FA_CRISP_EDGE : 0)),
               d);
}

static void game_text_style(const fe_painter_t* p, const fe_widget_t* w, text_style_t* st) {
    double u = p->view.sy;
    memset(st, 0, sizeof *st);
    st->role = FE_FONT_UI;
    st->weight = w->weight >= 100 && w->weight <= 900 ? w->weight : FW_MEDIUM;
    st->px = w->text_h * u;
    st->color = rgb_hex(w->color);
    st->flags = w->text_flags;
    st->exact = 1;
    if (w->shadow_offset) {
        st->shadow_dx = w->shadow_offset * u;
        st->shadow_color = rgb_hex(w->shadow_color);
    }
}

static void add_label(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    text_style_t st;
    if (w->filled) add_fill(p, w->rect, rgb_hex(w->fill_color), 1.0f);
    game_text_style(p, w, &st);
    add_text(p, &st, fe_scene_str(s, w->text), box_rect(p, w->rect));
}

/* DrawEdge(EDGE_SUNKEN) around a field: dark outside top and left, light bottom and right, a second softer ring inside. */
static void add_sunken_edge(fe_painter_t* p, fe_rect_t r) {
    for (int ring = 0; ring < 2; ring++) {
        rgb_t dark = ring ? grey(64) : grey(128), light = ring ? grey(212) : grey(255);
        fe_rect_t q = fe_rect_inflate(r, -ring, -ring);
        add_fill(p, fe_rect(q.left, q.top, q.right, q.top + 1), dark, 1.0f);
        add_fill(p, fe_rect(q.left, q.top + 1, q.left + 1, q.bottom), dark, 1.0f);
        add_fill(p, fe_rect(q.right - 1, q.top + 1, q.right, q.bottom), light, 1.0f);
        add_fill(p, fe_rect(q.left + 1, q.bottom - 1, q.right - 1, q.bottom), light, 1.0f);
    }
}

static void add_field(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    add_label(p, s, w);
    if (w->border) add_sunken_edge(p, w->rect);
}

/* CUIEditBox: the text, then a caret one canvas unit wide at its end. */
static void add_edit(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    text_style_t st;
    const char* text = fe_scene_str(s, w->text);
    int len = (int)strlen(text);
    box_t b = box_rect(p, w->rect);
    game_text_style(p, w, &st);
    st.flags |= FE_TEXT_VCENTER;
    add_text(p, &st, text, b);
    if (w->state & FE_ST_CARET) {
        double lh = line_height(p, &st), cx = floor(b.x0 + measure(p, &st, text, len) + 0.5), cw = p->view.sy < 1.0 ? 1.0 : p->view.sy;
        op_t* c = op(p, OP_FILL, cx, (b.y0 + b.y1 - lh) * 0.5, cx + cw, (b.y0 + b.y1 + lh) * 0.5, 0);
        if (c) c->color = rgb_hex(w->color);
    }
}

/* CUIListbox (0x00404470): rows of left-aligned text from the top of the control, the selected row a band of the text
 * colour with the background colour for its text, a scroll column of two greys between the arrow buttons. */
static void add_list(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    int arrow = w->arrow_w > 0 ? w->arrow_w : 16, width = (w->rect.right - w->rect.left) - arrow;
    text_style_t st;
    game_text_style(p, w, &st);
    st.flags = 0;
    for (int r = 0; r < w->rows && r < fe_list_shown(w); r++) {
        int index = w->top + r, top = w->rect.top + r * w->row_h;
        fe_rect_t row = fe_rect(w->rect.left, top, w->rect.left + width, top + w->row_h);
        box_t tb = box_of(p, row.left + 5.0, row.top, row.right - 5.0, row.bottom);
        const char* text = w->first_item + r < s->item_count ? fe_scene_str(s, s->items[w->first_item + r]) : "";
        text_style_t line = st;
        if (index == w->selected) {
            add_fill(p, row, rgb_hex(w->color), 1.0f);
            line.color = rgb_hex(w->selected_color);
            line.shadow_dx = 0;
        }
        add_text(p, &line, text, tb);
    }
    if (!fe_rect_empty(w->track) && w->item_count > w->rows) {
        fe_rect_t column = w->track, groove = fe_rect(column.left, column.top + arrow, column.right, column.bottom - arrow);
        fe_rect_t thumb = fe_list_thumb(column, arrow, w->top, w->rows, w->item_count);
        add_fill(p, groove, grey(TRACK_GREY), 1.0f);
        if (!fe_rect_empty(thumb)) {
            add_fill(p, thumb, grey(THUMB_GREY), 1.0f);
            add_fill(p, fe_rect(thumb.left, thumb.top, thumb.right, thumb.top + 1), grey(255), 1.0f);
            add_fill(p, fe_rect(thumb.left, thumb.top, thumb.left + 1, thumb.bottom), grey(255), 1.0f);
            add_fill(p, fe_rect(thumb.right - 1, thumb.top, thumb.right, thumb.bottom), grey(0), 1.0f);
            add_fill(p, fe_rect(thumb.left, thumb.bottom - 1, thumb.right, thumb.bottom), grey(0), 1.0f);
        }
    }
}

/* A picture of its own (a save's, a level's) fitted into its frame, only its edges cropped. */
static void add_photo(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w, int boxed) {
    double src[4], dst[4], fw = w->rect.right - w->rect.left, fh = w->rect.bottom - w->rect.top;
    fe_rect_t d, frame = fe_view_rect(&p->view, w->rect);
    int dw, dh;
    if (boxed) {
        add_fill(p, fe_rect_inflate(w->rect, 4, 4), rgb(0, 0, 0), 0.62f);
        add_fill(p, fe_rect_inflate(w->rect, 1, 1), rgb(0.78f, 0.72f, 0.60f), 0.85f);
    }
    if (!w->image && !w->image32) return;
    fe_photo_fit(w->image_w, w->image_h, fw, fh, src, dst);
    d = fe_view_rect(&p->view, fe_rect((int)floor(w->rect.left + dst[0]), (int)floor(w->rect.top + dst[1]),
                                       (int)ceil(w->rect.left + dst[2]), (int)ceil(w->rect.top + dst[3])));
    dw = d.right - d.left;
    dh = d.bottom - d.top;
    if (dw <= 0 || dh <= 0) return;
    (void)frame;
    if (w->image32) {
        add_bitmap(p, fa_cache_get32(p->art, w->image_hash, w->image32, w->image_w, w->image_h, w->image_pitch, src, dw, dh), d);
    } else {
        fa_source_t source;
        picture_source(w, &source);
        add_bitmap(p, fa_cache_get16(p->art, &source, src, dw, dh, FA_DEDITHER), d);
    }
    if (boxed && w->text > 0) {
        text_style_t st;
        fe_widget_t caption;
        memset(&caption, 0, sizeof caption);
        caption.text_h = (float)CAPTION_POINTS;
        caption.color = 0xFFFFFF;
        caption.text_flags = FE_TEXT_CENTER | FE_TEXT_EXACT;
        caption.shadow_offset = 1;
        caption.shadow_color = 0;
        caption.weight = 500;
        game_text_style(p, &caption, &st);
        add_text(p, &st, fe_scene_str(s, w->text), box_of(p, w->rect.left, w->rect.bottom + 5.0, w->rect.right, w->rect.bottom + 22.0));
    }
}

static void add_widget(fe_painter_t* p, const fe_scene_t* s, const fe_window_t* win, const fe_widget_t* w) {
    switch (w->kind) {
    case FE_W_BUTTON:
    case FE_W_CHECK:
    case FE_W_IMAGE:
        if (w->kind == FE_W_IMAGE && w->style == FE_IMAGE_PHOTO) add_photo(p, s, w, win->kind == FE_WND_DIRECT_LOAD);
        else add_picture(p, w, w->rect.left, w->rect.top);
        break;
    case FE_W_SLIDER: add_picture(p, w, w->value - w->image_w / 2, w->rect.top); break;
    case FE_W_LABEL: add_label(p, s, w); break;
    case FE_W_FIELD: add_field(p, s, w); break;
    case FE_W_EDIT: add_edit(p, s, w); break;
    case FE_W_LIST: add_list(p, s, w); break;
    default: break;
    }
}

static int add_main_backdrop(fe_painter_t* p, const fe_scene_t* s) {
    fa_source_t src;
    fe_rect_t d = fe_view_rect(&p->view, fe_rect(0, 0, BACKDROP_W, BACKDROP_H));
    const fa_bitmap_t *art, *soft;
    op_t* o;
    memset(&src, 0, sizeof src);
    src.bits = s->backdrop;
    src.w = BACKDROP_W;
    src.h = BACKDROP_H;
    src.pitch = s->backdrop_pitch;
    src.hash = s->backdrop_hash;
    art = fa_cache_get16(p->art, &src, NULL, d.right - d.left, d.bottom - d.top, FA_DEDITHER);
    soft = fa_cache_get16(p->art, &src, NULL, BLUR_W, BLUR_H, 0);
    if (!art || !soft) return 0;
    o = op(p, OP_WIDE, 0, 0, p->view.screen_w, p->view.screen_h, 0);
    if (!o) return 0;
    o->art = art;
    o->art2 = soft;
    o->art_x = d.left;
    o->art_y = d.top;
    return 1;
}

static int build(fe_painter_t* p, const fe_scene_t* s) {
    int top = s->window_count - 1;
    p->op_count = 0;
    for (int i = 0; i <= top; i++) {
        const fe_window_t* w = &s->windows[i];
        if (w->kind == FE_WND_MAIN && s->backdrop) {
            p->marking_base = 1;
            if (!add_main_backdrop(p, s)) return 0;
            p->marking_base = 0;
        }
        if (i != top || w->kind == FE_WND_GAME) continue;
        if (w->kind == FE_WND_DIRECT_LOAD) add_fill(p, fe_rect_inflate(w->rect, 4, 4), rgb(0, 0, 0), 0.62f);
        for (int k = w->first; k < w->first + w->count; k++) add_widget(p, s, w, &s->widgets[k]);
    }
    return 1;
}

typedef struct {
    const fe_painter_t* p;
    uint32_t* px;
    long pitch;
    fe_rect_t clip;
    int mode;
    volatile LONG* next_row;
} job_t;

enum { RUN_ALL, RUN_BASE, RUN_OVER_BASE };

#define BAND 32

static void run_band(const fe_painter_t* p, uint32_t* px, long pitch, fe_rect_t clip, int mode) {
    band_t b;
    size_t row_bytes = (size_t)(clip.right - clip.left) * 4;
    b.p = p;
    b.px = px;
    b.pitch = pitch;
    b.clip = clip;
    for (int y = clip.top; y < clip.bottom; y++) {
        if (mode == RUN_OVER_BASE) memcpy(px + (long)y * pitch + clip.left, p->base + (long)y * p->base_w + clip.left, row_bytes);
        else memset(px + (long)y * pitch + clip.left, 0, row_bytes);
    }
    for (int i = 0; i < p->op_count; i++) {
        const op_t* o = &p->ops[i];
        if ((mode == RUN_BASE && !o->base) || (mode == RUN_OVER_BASE && o->base)) continue;
        if (!fe_rect_empty(fe_rect_intersect(clip, o->bounds))) run_op(&b, o);
    }
}

static DWORD WINAPI worker(LPVOID arg) {
    job_t* j = (job_t*)arg;
    for (;;) {
        LONG top = InterlockedExchangeAdd(j->next_row, BAND);
        fe_rect_t band;
        if (top >= j->clip.bottom) break;
        band = fe_rect(j->clip.left, top, j->clip.right, top + BAND < j->clip.bottom ? top + BAND : j->clip.bottom);
        run_band(j->p, j->px, j->pitch, band, j->mode);
    }
    return 0;
}

static void execute(const fe_painter_t* p, uint32_t* px, long pitch, fe_rect_t clip, int mode, int threads) {
    HANDLE handles[MAX_THREADS];
    volatile LONG next = clip.top;
    job_t job;
    int started = 0;
    job.p = p;
    job.px = px;
    job.pitch = pitch;
    job.clip = clip;
    job.mode = mode;
    job.next_row = &next;
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    for (int i = 1; i < threads; i++) {
        handles[started] = CreateThread(NULL, 64 * 1024, worker, &job, 0, NULL);
        if (handles[started]) started++;
    }
    worker(&job);
    if (started) WaitForMultipleObjects((DWORD)started, handles, TRUE, INFINITE);
    for (int i = 0; i < started; i++) CloseHandle(handles[i]);
}

static int has_base_ops(const fe_painter_t* p) {
    for (int i = 0; i < p->op_count; i++)
        if (p->ops[i].base) return 1;
    return 0;
}

/* The main screen's art, plank and lettering never change for a screen size: painted once, then copied. */
static int ensure_base(fe_painter_t* p, const fe_scene_t* s, int threads) {
    uint32_t key = fe_hash(0, &s->backdrop_hash, sizeof s->backdrop_hash);
    key = fe_hash(key, &s->backdrop, sizeof s->backdrop);
    key = fe_hash(key, &s->guest_w, 2 * sizeof s->guest_w);
    if (p->base && p->base_valid && p->base_w == s->screen_w && p->base_h == s->screen_h && p->base_key == key) return 1;
    if (!p->base || p->base_w != s->screen_w || p->base_h != s->screen_h) {
        free(p->base);
        p->base = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)s->screen_w * s->screen_h);
        p->base_w = s->screen_w;
        p->base_h = s->screen_h;
    }
    p->base_valid = 0;
    if (!p->base) return 0;
    execute(p, p->base, s->screen_w, fe_rect(0, 0, s->screen_w, s->screen_h), RUN_BASE, threads);
    p->base_key = key;
    p->base_valid = 1;
    return 1;
}

void fe_painter_trim(fe_painter_t* p) {
    if (!p) return;
    fa_cache_clear(p->art);
    free(p->base);
    free(p->up);
    free(p->blur);
    p->base = NULL;
    p->up = NULL;
    p->blur = NULL;
    p->base_valid = 0;
    p->backdrop_src = NULL;
    free(p->photo);
    free(p->photo_blur);
    free(p->photo_field);
    p->photo = NULL;
    p->photo_blur = NULL;
    p->photo_field = NULL;
    p->photo_src = NULL;
    p->photo_ready = 0;
    for (int i = 0; i < GLYPH_CACHE; i++) {
        free(p->glyphs[i].a);
        p->glyphs[i].a = NULL;
    }
}

int fe_paint(fe_painter_t* p, const fe_scene_t* s, uint32_t* pixels, long pitch, fe_rect_t clip, int threads) {
    int mode = RUN_ALL;
    clip = fe_rect_intersect(clip, fe_rect(0, 0, s->screen_w, s->screen_h));
    if (!p || !pixels || s->screen_w <= 0 || s->screen_h <= 0) return 0;
    if (fe_rect_empty(clip)) return 1;
    p->view = fe_view_make_for(s->guest_w, s->guest_h, s->screen_w, s->screen_h);
    fa_cache_begin(p->art);
    if (!build(p, s)) return 0;
    if (has_base_ops(p) && ensure_base(p, s, threads)) mode = RUN_OVER_BASE;
    execute(p, pixels, pitch, clip, mode, threads);
    return 1;
}

/* The level's picture in a bolted iron frame over its own blurred surroundings. */
static void add_loading_picture(fe_painter_t* p, const ld_layout_t* l) {
    enum { TOP, BOTTOM, LEFT, RIGHT };
    static const struct {
        int edge;
        double at;
    } k_bolts[] = { { TOP, 0.25 },    { TOP, 0.5 },    { TOP, 0.75 },  { BOTTOM, 0.25 }, { BOTTOM, 0.5 },
                    { BOTTOM, 0.75 }, { LEFT, 0.33 },  { LEFT, 0.67 }, { RIGHT, 0.33 },  { RIGHT, 0.67 } };
    double u = p->view.sy, src[4], dst[4];
    box_t frame = box_rect(p, l->frame), inner = box_rect(p, l->photo);
    double in = u * FE_PANEL_MARGIN * 0.6;
    uint32_t seed = 0x10AD;
    op_t* o;
    fe_photo_fit(p->photo_w, p->photo_h, inner.x1 - inner.x0, inner.y1 - inner.y0, src, dst);
    o = op(p, OP_AMBIENT, 0, 0, p->view.screen_w, p->view.screen_h, 0);
    if (o) {
        o->pts[0] = inner.x0 + dst[0];
        o->pts[1] = inner.y0 + dst[1];
        o->pts[2] = inner.x0 + dst[2];
        o->pts[3] = inner.y0 + dst[3];
    }
    add_shadow(p, frame, u * 3.0, u * 6.0, u * 16.0, u * 4.0, 0.8f);
    add_plate(p, frame, u * 2.5, u * 3.0, MAT_IRON, 0, seed);
    o = add_well(p, box_of(p, l->photo.left - 1.5, l->photo.top - 1.5, l->photo.right + 1.5, l->photo.bottom + 1.5), u * 1.5, 0,
                 seed + 1);
    if (o) o->color = rgb(0.015f, 0.015f, 0.015f);
    o = op(p, OP_PHOTO, inner.x0 + dst[0], inner.y0 + dst[1], inner.x0 + dst[2], inner.y0 + dst[3], 0);
    if (o) {
        o->image32 = p->photo_src;
        o->image_w = p->photo_w;
        o->image_h = p->photo_h;
        o->image_pitch = p->photo_w;
        for (int i = 0; i < 4; i++) o->src[i] = src[i];
    }
    for (int corner = 0; corner < 4; corner++)
        add_rivet(p, corner & 1 ? frame.x1 - in : frame.x0 + in, corner & 2 ? frame.y1 - in : frame.y0 + in, u * 2.2, MAT_IRON,
                  seed + 2 + (uint32_t)corner, 1.05f);
    for (int i = 0; i < (int)(sizeof k_bolts / sizeof k_bolts[0]); i++) {
        uint32_t h = hash32(seed + (uint32_t)i * 977);
        double jitter = u * (((h & 255) / 255.0) - 0.5) * 6.0, x, y;
        int edge = k_bolts[i].edge;
        if (edge == TOP || edge == BOTTOM) {
            x = frame.x0 + (frame.x1 - frame.x0) * k_bolts[i].at + jitter;
            y = edge == TOP ? frame.y0 + in : frame.y1 - in;
        } else {
            x = edge == LEFT ? frame.x0 + in : frame.x1 - in;
            y = frame.y0 + (frame.y1 - frame.y0) * k_bolts[i].at + jitter;
        }
        if ((h >> 16) % 5) add_rivet(p, x, y, u * 1.8, MAT_IRON, h, 1.05f);
    }
}

/* The window's free lines and progress bars on a riveted plate: each caption over its groove, filled as far as the game
 * fills it. */
static void add_gauges(fe_painter_t* p, const ld_scene_t* s, const ld_layout_t* l) {
    double u = p->view.sy, in = u * 4.0;
    box_t pb = box_rect(p, l->plate);
    uint32_t seed = 0x6A06;
    text_style_t st;
    if (!l->gauge_count && !l->note_count) return;
    add_shadow(p, pb, u * 2.0, u * 4.5, u * 12.0, u * 3.0, 0.75f);
    make_torn(add_plate(p, pb, u * 2.5, u * 3.2, MAT_STEEL, 0, seed), u * 1.2, u * 7.0, seed);
    add_plate(p, box_of(p, l->plate.left + 7, l->plate.top + 6, l->plate.right - 7, l->plate.bottom - 6), u * 2.0, u * 2.2,
              MAT_BRASS, F_RECESSED | F_AO, seed + 1);
    for (int corner = 0; corner < 4; corner++)
        if (corner != 1)
            add_rivet(p, corner & 1 ? pb.x1 - in : pb.x0 + in, corner & 2 ? pb.y1 - in : pb.y0 + in, u * 1.8, MAT_STEEL,
                      seed + 2 + (uint32_t)corner, 1.0f);
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_LABEL;
    st.weight = FW_NORMAL;
    st.px = 11.5 * u;
    st.flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    st.shadow = u * 0.7 > 1.0 ? u * 0.7 : 1.0;
    st.fit = 1;
    for (int i = 0; i < l->note_count; i++) {
        st.color = rgb_hex(s->note_colors[i]);
        add_text(p, &st, s->notes[i], box_rect(p, l->notes[i]));
    }
    for (int i = 0; i < l->gauge_count; i++) {
        const ld_gauge_t* g = &s->gauges[i];
        box_t groove = box_rect(p, l->grooves[i]);
        double right = ld_fill_right(l, i, g->fraction);
        op_t* o;
        st.color = rgb_hex(g->text_color);
        add_text(p, &st, g->caption, box_rect(p, l->captions[i]));
        o = op(p, OP_GROOVE, groove.x0, groove.y0, groove.x1, groove.y1, 1);
        if (o) {
            o->radius = u * 3.0;
            o->width = u * 2.5;
            o->color = rgb(0.03f, 0.03f, 0.028f);
            o->flags = F_RECESSED;
        }
        o = right > groove.x0 + u * 1.5 ? op(p, OP_BAR, groove.x0 + u, groove.y0 + u, right, groove.y1 - u, 1) : NULL;
        if (o) {
            o->radius = u * 2.0;
            o->color = rgb_hex(g->color);
            o->flags = F_GRADIENT;
            o->alpha = 0.95f;
        }
        for (int t = 0; t <= 10; t++) {
            double x = groove.x0 + (groove.x1 - groove.x0) * t / 10.0;
            o = op(p, OP_BAR, x - u * 0.35, groove.y1 + u * 1.5, x + u * 0.35, groove.y1 + u * (t % 5 ? 3.5 : 5.0), 1);
            if (o) {
                o->color = rgb(0.85f, 0.78f, 0.62f);
                o->alpha = 0.5f;
            }
        }
    }
}

/* A message the game writes over the screen, on a plaque as the menus' own. */
static void add_message(fe_painter_t* p, const ld_scene_t* s, const ld_layout_t* l) {
    double u = p->view.sy, cx = (l->message.left + l->message.right) * 0.5, w, h = l->message.bottom - l->message.top;
    uint32_t seed = 0x3E55;
    text_style_t st;
    op_t* plate;
    box_t b;
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_PLAQUE;
    st.weight = FW_NORMAL;
    st.px = h * 0.40 * u;
    st.spacing = 0.04;
    st.color = k_ink;
    st.flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    st.wear = 0.22;
    st.wear_size = 40.0;
    st.emboss = u * 0.5;
    st.seed = seed;
    st.fit = 1;
    w = measure(p, &st, s->message, (int)strlen(s->message)) / u + 64.0;
    w = clampd(w, 200.0, FE_CANVAS_W - 40.0);
    b = box_of(p, cx - w * 0.5, l->message.top, cx + w * 0.5, l->message.bottom);
    add_shadow(p, b, u * 1.5, u * 3.0, u * 8.0, u * 3.0, 0.7f);
    plate = add_plate(p, b, u * 2.5, u * 3.6, MAT_STEEL, 0, seed);
    make_torn(plate, u * 2.0, u * 8.0, seed);
    for (int corner = 0; corner < 4; corner++)
        if (corner != 2)
            add_rivet(p, corner & 1 ? b.x1 - u * 7.5 : b.x0 + u * 7.5, corner & 2 ? b.y1 - u * 7.5 : b.y0 + u * 7.5, u * 2.4,
                      MAT_STEEL, seed + 1 + (uint32_t)corner, 1.0f);
    add_text(p, &st, s->message, box_of(p, cx - w * 0.5 + 18.0, l->message.top, cx + w * 0.5 - 18.0, l->message.bottom));
}

int fe_paint_loading(fe_painter_t* p, const ld_scene_t* s, uint32_t* pixels, long pitch, fe_rect_t clip, int threads) {
    ld_layout_t l;
    if (!p || !s || !pixels || s->screen_w <= 0 || s->screen_h <= 0 || s->kind == LD_SCENE_NONE) return 0;
    clip = fe_rect_intersect(clip, fe_rect(0, 0, s->screen_w, s->screen_h));
    if (fe_rect_empty(clip)) return 1;
    ld_layout_scene(&l, s);
    p->view = l.view;
    if (s->kind == LD_SCENE_LOADER && !prepare_photo(p, s)) return 0;
    p->op_count = 0;
    p->marking_base = 0;
    p->clipping = 0;
    if (s->kind == LD_SCENE_LOADER) {
        add_loading_picture(p, &l);
        add_gauges(p, s, &l);
    } else if (s->message[0]) {
        add_message(p, s, &l);
    }
    execute(p, pixels, pitch, clip, RUN_ALL, threads);
    return 1;
}

static const double k_arrow[][2] = { { 0, 0 }, { 0, 15.5 }, { 3.7, 12.1 }, { 6.3, 18.0 }, { 8.7, 16.9 }, { 6.1, 11.3 }, { 10.9, 11.3 } };
#define ARROW_POINTS (int)(sizeof k_arrow / sizeof k_arrow[0])
#define CURSOR_SHARE 0.62

int fe_cursor_size(double scale) { return (int)ceil(21.0 * scale * CURSOR_SHARE) + 6; }

static double arrow_distance(double x, double y, double k, int* inside) {
    double best = 1e9;
    int crossings = 0;
    for (int i = 0, j = ARROW_POINTS - 1; i < ARROW_POINTS; j = i++) {
        double ax = k_arrow[j][0] * k, ay = k_arrow[j][1] * k, bx = k_arrow[i][0] * k, by = k_arrow[i][1] * k;
        double d = segment_distance(x, y, ax, ay, bx, by);
        if (d < best) best = d;
        if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax) crossings++;
    }
    *inside = crossings & 1;
    return best;
}

void fe_paint_cursor(double scale_, uint32_t* pixels, int size, int* hot_x, int* hot_y) {
    double k = scale_ * CURSOR_SHARE, outline = k * 0.16 > 1.2 ? k * 0.16 : 1.2, ox = 2.0 + outline, oy = 2.0 + outline;
    memset(pixels, 0, sizeof(uint32_t) * (size_t)size * size);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int inside, sin_;
            double px = x + 0.5 - ox, py = y + 0.5 - oy;
            double d = arrow_distance(px, py, k, &inside), sd = arrow_distance(px - k * 0.9, py - k * 1.2, k, &sin_);
            float shadow = sin_ ? 1.0f : 1.0f - smooth(0.0f, (float)(k * 1.2), (float)sd);
            float fill = inside ? clamp01((float)(d - outline + 0.5)) : 0.0f;
            float body = inside ? 1.0f : clamp01((float)(outline + 0.5 - d));
            rgb_t c = mix(k_bone, rgb(0.72f, 0.66f, 0.56f), clamp01((float)(py / (18.0 * k))));
            uint32_t* d32 = pixels + (long)y * size + x;
            put(d32, k_black, shadow * 0.4f);
            put(d32, rgb(0.07f, 0.05f, 0.03f), body);
            put(d32, c, fill);
        }
    *hot_x = (int)floor(ox);
    *hot_y = (int)floor(oy);
}
