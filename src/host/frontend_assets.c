#include "frontend_assets.h"

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
#define MAX_THREADS 8
#define BACKDROP_W 640
#define BACKDROP_H 480
#define UP 2
#define BLUR_W 80
#define BLUR_H 60
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
    OP_ARROW
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
    op_t ops[MAX_OPS];
    int op_count;
    fe_view_t view;
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

static void run_dim(const band_t* b, const op_t* o) {
    float k = 1.0f - o->alpha;
    uint32_t a = (uint32_t)(o->alpha * 255.0f + 0.5f);
    FOR_PIXELS(b, o, r) {
        uint32_t* d = PIXEL(b, x, y), v = *d;
        uint32_t al = (uint32_t)((float)(v >> 24) * k + 0.5f) + a;
        *d = ((al > 255 ? 255 : al) << 24) | ((uint32_t)((float)((v >> 16) & 255) * k + 0.5f) << 16) |
             ((uint32_t)((float)((v >> 8) & 255) * k + 0.5f) << 8) | (uint32_t)((float)(v & 255) * k + 0.5f);
    }
}

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

static void run_glow(const band_t* b, const op_t* o) {
    double nx, ny;
    FOR_PIXELS(b, o, r) {
        double d = rrect(o, x + 0.5, y + 0.5, &nx, &ny);
        float t = d <= 0 ? 1.0f : 1.0f - smooth(0.0f, (float)o->width, (float)d);
        put(PIXEL(b, x, y), o->color, o->alpha * t * t);
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

static void run_socket(const band_t* b, const op_t* o) {
    double nx, ny, rim = o->width;
    FOR_PIXELS(b, o, r) {
        double d = rrect(o, x + 0.5, y + 0.5, &nx, &ny), e = -d;
        float cov = coverage(d), lit = facing(nx, ny);
        rgb_t c;
        if (cov <= 0.0f) continue;
        if (e < rim) {
            c = scale(rgb(0.50f, 0.42f, 0.30f), 1.0f + 0.5f * lit);
            if (o->flags & F_HOVER) c = mix(c, o->color2, 0.75f);
        } else {
            double ie = e - rim;
            c = rgb(0.035f, 0.032f, 0.03f);
            if (ie < rim * 1.5 && lit > 0) c = scale(c, 1.0f - 0.6f * lit);
        }
        put(PIXEL(b, x, y), c, cov * o->alpha);
    }
}

static double segment_distance(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay, t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = clampd(t, 0, 1);
    wx -= vx * t;
    wy -= vy * t;
    return sqrt(wx * wx + wy * wy);
}

static void run_stroke(const band_t* b, const op_t* o) {
    FOR_PIXELS(b, o, r) {
        double px = x + 0.5, py = y + 0.5;
        double d1 = segment_distance(px, py, o->pts[0], o->pts[1], o->pts[2], o->pts[3]);
        double d2 = segment_distance(px, py, o->pts[2], o->pts[3], o->pts[4], o->pts[5]), d = d1 < d2 ? d1 : d2;
        float cov = clamp01((float)(o->width * 0.5 + 0.5 - d)), glow = 1.0f - smooth(0.0f, (float)(o->width * 1.6), (float)d);
        put(PIXEL(b, x, y), o->color2, glow * glow * 0.35f * o->alpha);
        put(PIXEL(b, x, y), mix(o->color, rgb(1, 0.95f, 0.8f), 0.3f * clamp01((float)(1.0 - d / (o->width * 0.5)))),
            cov * o->alpha);
    }
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

static float tri_edge(double px, double py, double ax, double ay, double bx, double by) {
    double ex = bx - ax, ey = by - ay, len = sqrt(ex * ex + ey * ey);
    return (float)(((px - ax) * ey - (py - ay) * ex) / len);
}

static void run_arrow(const band_t* b, const op_t* o) {
    double cx = (o->x0 + o->x1) * 0.5, cy = (o->y0 + o->y1) * 0.5, s = (o->x1 - o->x0) * 0.24;
    double ax = cx, ay = cy - s, bx = cx + s * 1.1, by = cy + s * 0.75, qx = cx - s * 1.1, qy = by;
    if (o->flags & F_DOWN) {
        ay = cy + s;
        by = qy = cy - s * 0.75;
        bx = cx - s * 1.1;
        qx = cx + s * 1.1;
    }
    FOR_PIXELS(b, o, r) {
        double px = x + 0.5, py = y + 0.5;
        float e1 = tri_edge(px, py, ax, ay, bx, by), e2 = tri_edge(px, py, bx, by, qx, qy), e3 = tri_edge(px, py, qx, qy, ax, ay);
        float m = e1 > e2 ? e1 : e2;
        if (e3 > m) m = e3;
        put(PIXEL(b, x, y), o->color, clamp01(0.5f - m) * o->alpha);
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

/* Catmull-Rom sample of an RGB565 image, its key colour transparent (premultiplied while filtering). */
static float sample_image(const op_t* o, double sx, double sy, rgb_t* c) {
    int ix = (int)floor(sx - 0.5), iy = (int)floor(sy - 0.5);
    float fx = (float)(sx - 0.5 - ix), fy = (float)(sy - 0.5 - iy), acc[4] = { 0, 0, 0, 0 };
    for (int j = -1; j <= 2; j++) {
        float wy = cubic_w((float)j - fy);
        int yy = iy + j < 0 ? 0 : iy + j >= o->image_h ? o->image_h - 1 : iy + j;
        for (int i = -1; i <= 2; i++) {
            float w = wy * cubic_w((float)i - fx), t[4];
            int xx = ix + i < 0 ? 0 : ix + i >= o->image_w ? o->image_w - 1 : ix + i;
            uint16_t v = o->image[(long)yy * o->image_pitch + xx];
            texel565(v, t);
            if (o->keyed && v == o->key) t[3] = 0.0f;
            acc[0] += t[0] * t[3] * w;
            acc[1] += t[1] * t[3] * w;
            acc[2] += t[2] * t[3] * w;
            acc[3] += t[3] * w;
        }
    }
    acc[3] = clamp01(acc[3]);
    if (acc[3] <= 0.002f) return 0.0f;
    *c = rgb(acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]);
    return acc[3];
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

/* Four bilinear taps over the pixel's footprint: pictures are drawn smaller than they are stored. */
static void run_image32(const band_t* b, const op_t* o) {
    double kx = (o->src[2] - o->src[0]) / (o->x1 - o->x0), ky = (o->src[3] - o->src[1]) / (o->y1 - o->y0);
    FOR_PIXELS(b, o, r) {
        float acc[3] = { 0, 0, 0 };
        double sx = o->src[0] + (x + 0.5 - o->x0) * kx, sy = o->src[1] + (y + 0.5 - o->y0) * ky;
        bilinear32(o, sx - kx * 0.25, sy - ky * 0.25, acc);
        bilinear32(o, sx + kx * 0.25, sy - ky * 0.25, acc);
        bilinear32(o, sx - kx * 0.25, sy + ky * 0.25, acc);
        bilinear32(o, sx + kx * 0.25, sy + ky * 0.25, acc);
        put(PIXEL(b, x, y), scale(rgb(acc[0] * 0.25f, acc[1] * 0.25f, acc[2] * 0.25f), o->light), o->alpha);
    }
}

static void run_image(const band_t* b, const op_t* o) {
    double kx = (o->src[2] - o->src[0]) / (o->x1 - o->x0), ky = (o->src[3] - o->src[1]) / (o->y1 - o->y0);
    if (o->image32) {
        run_image32(b, o);
        return;
    }
    FOR_PIXELS(b, o, r) {
        rgb_t c;
        float a = sample_image(o, o->src[0] + (x + 0.5 - o->x0) * kx, o->src[1] + (y + 0.5 - o->y0) * ky, &c);
        if (a > 0.0f) put(PIXEL(b, x, y), scale(c, o->light), a * o->alpha);
    }
}

static void up_sample(const fe_painter_t* p, double sx, double sy, float* c) {
    const int w = BACKDROP_W * UP, h = BACKDROP_H * UP;
    double fx = clampd(sx - 0.5, 0, w - 1.001), fy = clampd(sy - 0.5, 0, h - 1.001);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(fx - ix), ty = (float)(fy - iy);
    const uint32_t* r0 = p->up + (long)iy * w + ix;
    const uint32_t* r1 = r0 + w;
    for (int k = 0; k < 3; k++) {
        int sh = 16 - 8 * k;
        float a = (float)((r0[0] >> sh) & 255) + ((float)((r0[1] >> sh) & 255) - (float)((r0[0] >> sh) & 255)) * tx;
        float bb = (float)((r1[0] >> sh) & 255) + ((float)((r1[1] >> sh) & 255) - (float)((r1[0] >> sh) & 255)) * tx;
        c[k] = (a + (bb - a) * ty) / 255.0f;
    }
}

static void blur_sample(const fe_painter_t* p, double u, double v, float* c) {
    double fx = clampd(u / BACKDROP_W * BLUR_W - 0.5, 0, BLUR_W - 1.001), fy = clampd(v / BACKDROP_H * BLUR_H - 0.5, 0, BLUR_H - 1.001);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(fx - ix), ty = (float)(fy - iy);
    for (int k = 0; k < 3; k++) {
        const float* b = p->blur + k;
        float a = b[(iy * BLUR_W + ix) * 3] + (b[(iy * BLUR_W + ix + 1) * 3] - b[(iy * BLUR_W + ix) * 3]) * tx;
        float d = b[((iy + 1) * BLUR_W + ix) * 3] + (b[((iy + 1) * BLUR_W + ix + 1) * 3] - b[((iy + 1) * BLUR_W + ix) * 3]) * tx;
        c[k] = a + (d - a) * ty;
    }
}

/* The art left of BACKDROP_ART_LEFT is the original title plank, which the frontend paints again: the jungle is
 * mirrored under the new plank and past the 4:3 frame, blurred and darker the further it goes. The mirrored copies
 * stop above the original copyright lines; the original film logo in the lower left corner is kept as it is. */
#define BACKDROP_ART_LEFT 178.0
#define BACKDROP_MIRROR_BOTTOM 438.0

#define LOGO_X 40.0
#define LOGO_Y 457.0

static float logo_weight(double u, double v) {
    double dx = u - LOGO_X, dy = (v - LOGO_Y) * 1.15;
    return 1.0f - smooth(26.0f, 36.0f, (float)sqrt(dx * dx + dy * dy));
}

static float grade(float c) { return clamp01(c) * (0.93f + 0.07f * clamp01(c)); }

static void run_backdrop(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    float cxs = p->view.screen_w * 0.5f, cys = p->view.screen_h * 0.5f, rmax = sqrtf(cxs * cxs + cys * cys);
    fe_rect_t r = fe_rect_intersect(b->clip, o->bounds);
    for (int y = r.top; y < r.bottom; y++) {
        double v = (y + 0.5 - p->view.y0) / p->view.sy, cv = clampd(v, 0, BACKDROP_H);
        float dy = y + 0.5f - cys;
        for (int x = r.left; x < r.right; x++) {
            double u = (x + 0.5 - p->view.x0) / p->view.sx, mu = u, mv = cv, outside = 0;
            float sharp[3], soft[3], k, vig, grain, logo, dx = x + 0.5f - cxs;
            rgb_t c;
            if (u < BACKDROP_ART_LEFT) {
                outside = BACKDROP_ART_LEFT - u;
                mu = BACKDROP_ART_LEFT + fmod(outside, BACKDROP_W - BACKDROP_ART_LEFT);
            } else if (u > BACKDROP_W) {
                outside = u - BACKDROP_W;
                mu = BACKDROP_W - fmod(outside, BACKDROP_W - BACKDROP_ART_LEFT);
            }
            if (outside > 0 && mv > BACKDROP_MIRROR_BOTTOM) mv = BACKDROP_MIRROR_BOTTOM;
            logo = u < 80 && u > 0 && v > 416 ? logo_weight(u, v) : 0.0f;
            if (logo > 0.0f) {
                mu = mu + (u - mu) * logo;
                mv = cv;
            }
            up_sample(p, mu * UP, mv * UP, sharp);
            k = smooth(0.0f, 45.0f, (float)outside) * (1.0f - logo);
            if (k > 0.0f) {
                blur_sample(p, mu, mv, soft);
                for (int i = 0; i < 3; i++) sharp[i] += (soft[i] - sharp[i]) * k;
            }
            c = scale(rgb(grade(sharp[0]), grade(sharp[1]), grade(sharp[2])),
                      1.0f - 0.6f * smooth(0.0f, 320.0f, (float)outside) * (1.0f - logo));
            vig = 1.0f - 0.42f * smooth(0.45f, 1.05f, sqrtf(dx * dx + dy * dy) / rmax);
            grain = (noise_at(p, x * 1.9f + 33, y * 1.9f + 71) - 0.5f) * 0.045f;
            put(PIXEL(b, x, y), add(scale(c, vig), grain), 1.0f);
        }
    }
}

static void run_plank(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    double unit = o->unit;
    fe_rect_t r = fe_rect_intersect(b->clip, o->bounds);
    for (int y = r.top; y < r.bottom; y++) {
        double v = (y + 0.5 - p->view.y0) / p->view.sy;
        double left = 7.0 + 5.0 * (nz(p, 0, v, 90, 3) - 0.5);
        double right = 194.0 + 20.0 * (nz(p, 7, v, 160, 5) - 0.5) + 7.0 * (nz(p, 3, v, 22, 9) - 0.5);
        double seam = 141.0 + 3.0 * (nz(p, 1, v, 200, 47) - 0.5);
        for (int x = r.left; x < r.right; x++) {
            double u = (x + 0.5 - p->view.x0) / p->view.sx, e, warp, ring;
            double bottom = 421.0 + 14.0 * (nz(p, u, 0, 120, 11) - 0.5) + 5.0 * (nz(p, u, 5, 18, 13) - 0.5);
            int board = u >= seam;
            float cov, grain, crack, lichen, bleach, shade, across;
            rgb_t c;
            e = u - left;
            if (right - u < e) e = right - u;
            if (bottom - v < e) e = bottom - v;
            if (e <= 0) {
                double out = u - right > v - bottom ? u - right : v - bottom;
                if (u > right - 1 || v > bottom - 1) put(PIXEL(b, x, y), k_black, 0.6f * (1.0f - smooth(-2.0f, 10.0f, (float)out)));
                continue;
            }
            cov = clamp01((float)(e * unit + 0.5));
            warp = 3.0 * (nz(p, u, v * 0.5, 150, 61 + board) - 0.5);
            ring = 0.5 + 0.5 * sin((u + warp) * 1.15 + 4.0 * nz(p, u, v, 380, 71 + board));
            grain = nz(p, (u + warp) * 5.0, v * 0.08, 64, 21 + board);
            c = board ? rgb(0.34f, 0.31f, 0.26f) : rgb(0.31f, 0.28f, 0.24f);
            c = scale(c, 0.52f + 0.40f * grain + 0.10f * (float)ring);
            bleach = smooth(0.55f, 0.85f, nz(p, u, v, 340, 33 + board));
            c = mix(c, rgb(0.52f, 0.50f, 0.46f), bleach * 0.35f);
            crack = 1.0f - smooth(0.006f, 0.022f, fabsf(nz(p, (u + warp) * 2.0, v * 0.07, 64, 81 + board) - 0.5f));
            c = scale(c, 1.0f - crack * 0.6f * smooth(0.45f, 0.7f, nz(p, u, v, 120, 85)));
            lichen = smooth(0.62f, 0.80f, nz(p, u, v, 260, 41)) * (0.35f + 0.65f * smooth(250.0f, 420.0f, (float)v));
            c = mix(c, scale(rgb(0.30f, 0.34f, 0.17f), 0.8f + 0.4f * nz(p, u, v, 30, 43)), lichen * 0.6f);
            across = (float)(board ? (u - seam) / (right - seam) : (u - left) / (seam - left));
            shade = 0.80f + 0.30f * sinf(clamp01(across) * 3.14159f);
            if (fabs(u - seam) < 1.1) shade *= 0.25f;
            else if (u > seam && u - seam < 2.6) shade *= 1.12f;
            c = scale(c, shade * (float)(1.0 - 0.5 * (1.0 - smooth(0.0f, 8.0f, (float)e))));
            c = add(c, (noise_at(p, x * 2.2f, y * 2.2f) - 0.5f) * 0.045f);
            put(PIXEL(b, x, y), c, cov);
        }
    }
}

static void run_footer(const band_t* b, const op_t* o) {
    const fe_painter_t* p = b->p;
    FOR_PIXELS(b, o, r) {
        double u, v;
        float a;
        canvas_uv(p, x, y, &u, &v);
        a = 0.97f * smooth((float)o->y0, (float)o->y1, (float)v);
        put(PIXEL(b, x, y), k_black, a * (1.0f - logo_weight(u, v)));
    }
}

static void run_op(const band_t* b, const op_t* o) {
    switch (o->kind) {
    case OP_DIM: run_dim(b, o); break;
    case OP_BACKDROP: run_backdrop(b, o); break;
    case OP_PLANK: run_plank(b, o); break;
    case OP_FOOTER: run_footer(b, o); break;
    case OP_SHADOW: run_shadow(b, o); break;
    case OP_PLATE: run_plate(b, o); break;
    case OP_WELL: run_well(b, o); break;
    case OP_GLOW: run_glow(b, o); break;
    case OP_RIVET: run_rivet(b, o); break;
    case OP_TEXT: run_text(b, o); break;
    case OP_SOCKET: run_socket(b, o); break;
    case OP_STROKE: run_stroke(b, o); break;
    case OP_GROOVE:
    case OP_BAR: run_bar(b, o); break;
    case OP_IMAGE: run_image(b, o); break;
    case OP_ARROW: run_arrow(b, o); break;
    }
}

static const char* const k_faces[FE_FONT_COUNT][4] = {
    { "Stencil", "Rockwell Extra Bold", "Georgia", "Times New Roman" },
    { "Bahnschrift SemiBold", "Bahnschrift", "Franklin Gothic Medium", "Arial" },
    { "Bahnschrift", "Segoe UI", "Arial", "Tahoma" },
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
    if (!ensure_dib(p, 512, 128)) {
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
    free(p);
}

static int prepare_backdrop(fe_painter_t* p, const fe_scene_t* s) {
    const int w = BACKDROP_W * UP, h = BACKDROP_H * UP;
    if (!s->backdrop) return 0;
    if (p->up && p->backdrop_src == s->backdrop && p->backdrop_hash == s->backdrop_hash) return 1;
    if (!p->up) p->up = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
    if (!p->blur) p->blur = (float*)malloc(sizeof(float) * BLUR_W * BLUR_H * 3);
    if (!p->up || !p->blur) return 0;
    for (int y = 0; y < h; y++) {
        double sy = (y + 0.5) / UP;
        int iy = (int)floor(sy - 0.5);
        float fy = (float)(sy - 0.5 - iy);
        for (int x = 0; x < w; x++) {
            double sx = (x + 0.5) / UP;
            int ix = (int)floor(sx - 0.5);
            float fx = (float)(sx - 0.5 - ix), acc[3] = { 0, 0, 0 };
            for (int j = -1; j <= 2; j++) {
                int yy = iy + j < 0 ? 0 : iy + j >= BACKDROP_H ? BACKDROP_H - 1 : iy + j;
                float wy = cubic_w((float)j - fy);
                for (int i = -1; i <= 2; i++) {
                    int xx = ix + i < 0 ? 0 : ix + i >= BACKDROP_W ? BACKDROP_W - 1 : ix + i;
                    float wgt = wy * cubic_w((float)i - fx), t[4];
                    texel565(s->backdrop[(long)yy * s->backdrop_pitch + xx], t);
                    acc[0] += t[0] * wgt;
                    acc[1] += t[1] * wgt;
                    acc[2] += t[2] * wgt;
                }
            }
            p->up[(long)y * w + x] = ((uint32_t)(clamp01(acc[0]) * 255.0f + 0.5f) << 16) |
                                     ((uint32_t)(clamp01(acc[1]) * 255.0f + 0.5f) << 8) |
                                     (uint32_t)(clamp01(acc[2]) * 255.0f + 0.5f);
        }
    }
    for (int by = 0; by < BLUR_H; by++)
        for (int bx = 0; bx < BLUR_W; bx++) {
            float acc[3] = { 0, 0, 0 };
            int n = 0;
            for (int y = by * 8 - 12; y < by * 8 + 20; y += 2)
                for (int x = bx * 8 - 12; x < bx * 8 + 20; x += 2) {
                    float t[4];
                    int xx = x < 0 ? 0 : x >= BACKDROP_W ? BACKDROP_W - 1 : x, yy = y < 0 ? 0 : y >= BACKDROP_H ? BACKDROP_H - 1 : y;
                    texel565(s->backdrop[(long)yy * s->backdrop_pitch + xx], t);
                    acc[0] += t[0];
                    acc[1] += t[1];
                    acc[2] += t[2];
                    n++;
                }
            for (int k = 0; k < 3; k++) p->blur[(by * BLUR_W + bx) * 3 + k] = acc[k] / n;
        }
    p->backdrop_src = s->backdrop;
    p->backdrop_hash = s->backdrop_hash;
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

static void add_glow(fe_painter_t* p, box_t b, double radius, double reach, rgb_t color, float alpha) {
    op_t* o = op(p, OP_GLOW, b.x0, b.y0, b.x1, b.y1, reach + 1);
    if (!o) return;
    o->radius = radius;
    o->width = reach;
    o->color = color;
    o->alpha = alpha;
}

static uint32_t seed_of(fe_rect_t r, int salt) {
    return hash32((uint32_t)(r.left * 7 + r.top * 131 + r.right * 17 + r.bottom * 61 + salt * 977));
}

static void make_torn(op_t* o, double bite, double leg, uint32_t seed) {
    if (!o) return;
    o->pts[0] = bite;
    o->pts[1] = leg;
    o->pts[2] = (double)((seed >> 9) & 3);
}

/* The name plate is hung off-centre, each screen on its own side, as the original plates were. */
static void add_tab(fe_painter_t* p, fe_rect_t panel, const char* title, uint32_t seed) {
    static const double offsets[] = { -0.22, 0.17, -0.12, 0.25 };
    text_style_t st;
    double s = p->view.sy, w, h = 21.0 + (double)(seed % 3), pw = panel.right - panel.left;
    double cx = (panel.left + panel.right) * 0.5 + pw * offsets[(seed >> 3) & 3], top = panel.top - 14.0 - (double)((seed >> 5) % 3);
    box_t b;
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_PLAQUE;
    st.weight = FW_NORMAL;
    st.px = 12.5 * s;
    st.spacing = 0.16;
    st.color = rgb(0.16f, 0.10f, 0.06f);
    st.flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    st.wear = 0.25;
    st.wear_size = 40.0;
    st.emboss = s * 0.45;
    st.seed = 0x7AB;
    w = measure(p, &st, title, (int)strlen(title)) / s + 40.0 + (double)(seed % 9);
    if (cx - w * 0.5 < panel.left + 10) cx = panel.left + 10 + w * 0.5;
    if (cx + w * 0.5 > panel.right - 10) cx = panel.right - 10 - w * 0.5;
    b = box_of(p, cx - w * 0.5, top, cx + w * 0.5, top + h);
    add_shadow(p, b, s * 1.5, s * 2.5, s * 4.0, s * 3.0, 0.6f);
    make_torn(add_plate(p, b, s * 2.0, s * 2.2, MAT_COPPER, 0, 0x5EED + seed), s * 0.9, s * 5.0, seed);
    add_rivet(p, b.x0 + s * 7.0, (b.y0 + b.y1) * 0.5 + s * 0.6, s * 2.3, MAT_STEEL, 9, 0.95f);
    if (seed & 1) add_rivet(p, b.x1 - s * 7.0, (b.y0 + b.y1) * 0.5 - s * 0.8, s * 2.1, MAT_STEEL, 10, 0.9f);
    add_text(p, &st, title, box_of(p, cx - w * 0.5 + 12, top, cx + w * 0.5 - 12, top + h));
}

static int clear_of_widgets(const fe_scene_t* s, const fe_window_t* w, fe_rect_t r) {
    for (int i = w->first; i < w->first + w->count; i++)
        if (!fe_rect_empty(fe_rect_intersect(fe_rect_inflate(s->widgets[i].rect, 2, 2), r))) return 0;
    return 1;
}

/* Bolted-on repairs: a corner patch and a strap, only where they hide no control. */
static void add_patches(fe_painter_t* p, const fe_scene_t* s, const fe_window_t* w, uint32_t seed) {
    double u = p->view.sy;
    fe_rect_t pr = w->panel;
    for (int k = 0; k < 4; k++) {
        int corner = (int)((seed >> 2) + (uint32_t)k) & 3, pw = 30 + (int)(seed % 12), ph = 17 + (int)((seed >> 4) % 6);
        int left = corner & 1 ? pr.right - pw + 3 : pr.left - 3, top = corner & 2 ? pr.bottom - ph + 3 : pr.top - 3;
        fe_rect_t r = fe_rect(left, top, left + pw, top + ph);
        box_t b;
        if (!clear_of_widgets(s, w, r)) continue;
        b = box_rect(p, r);
        add_shadow(p, b, u * 1.0, u * 2.0, u * 4.0, u * 2.0, 0.55f);
        make_torn(add_plate(p, b, u * 1.5, u * 1.8, MAT_STEEL, 0, seed + 0x91), u * 1.1, u * 6.0, seed + 0x91);
        add_rivet(p, b.x0 + u * (corner & 1 ? 5.0 : pw - 6.0), b.y0 + u * 4.5, u * 1.8, MAT_STEEL, seed + 3, 1.0f);
        add_rivet(p, b.x0 + u * (corner & 1 ? pw - 6.0 : 5.0), b.y1 - u * 4.5, u * 1.8, MAT_STEEL, seed + 4, 1.0f);
        break;
    }
    for (int k = 0; k < 3; k++) {
        int right = (int)((seed >> 6) + (uint32_t)k) & 1, h = (pr.bottom - pr.top) * (30 + (int)(seed % 20)) / 100;
        int top = pr.top + (pr.bottom - pr.top) * (20 + 15 * k + (int)((seed >> 3) % 10)) / 100;
        int left = right ? pr.right - 11 : pr.left - 2;
        fe_rect_t r = fe_rect(left, top, left + 13, top + h);
        box_t b;
        if (r.bottom > pr.bottom - 12 || !clear_of_widgets(s, w, r)) continue;
        b = box_rect(p, r);
        add_shadow(p, b, u * 1.0, u * 1.5, u * 3.0, u * 1.5, 0.5f);
        make_torn(add_plate(p, b, u * 1.2, u * 1.6, MAT_IRON, 0, seed + 0x33), u * 0.8, 0, 0);
        for (int i = 0; i < 3; i++)
            add_rivet(p, (b.x0 + b.x1) * 0.5, b.y0 + (b.y1 - b.y0) * (0.18 + 0.32 * i), u * 1.7, MAT_IRON, seed + 40 + (uint32_t)i,
                      1.05f);
        break;
    }
}

static void add_panel(fe_painter_t* p, const fe_scene_t* s, const fe_window_t* w) {
    double u = p->view.sy, frame = 8.0;
    fe_rect_t pr = w->panel;
    box_t outer = box_rect(p, pr), inner = box_of(p, pr.left + frame, pr.top + frame, pr.right - frame, pr.bottom - frame);
    uint32_t seed = seed_of(pr, w->kind);
    int columns = 9 + (int)(seed % 5), rows = 5 + (int)((seed >> 4) % 3);
    add_shadow(p, outer, u * 3.0, u * 7.0, u * 16.0, u * 6.0, 0.72f);
    make_torn(add_plate(p, outer, u * 3.0, u * 3.2, MAT_IRON, 0, seed), u * 1.4, u * 12.0, seed);
    add_plate(p, inner, u * 2.0, u * 2.4, MAT_BRASS, F_RECESSED | F_AO, seed + 1);
    for (int i = 0; i <= columns; i++) {
        uint32_t h = hash32(seed + (uint32_t)i * 977);
        double x = pr.left + 5.0 + (pr.right - pr.left - 10.0) * (i + ((h & 255) / 255.0 - 0.5) * 0.35) / columns;
        if ((h >> 8) % 7 != 0) add_rivet(p, fe_view_x(&p->view, x), fe_view_y(&p->view, pr.top + 4.0 + ((h >> 12) & 1) * 0.8),
                                         u * (1.5 + ((h >> 13) & 3) * 0.15), MAT_IRON, h, 1.1f);
        if ((h >> 16) % 6 != 0) add_rivet(p, fe_view_x(&p->view, x + ((h >> 20) & 3) - 1.5), fe_view_y(&p->view, pr.bottom - 4.0),
                                          u * 1.7, MAT_IRON, h + 7, 1.1f);
    }
    for (int i = 1; i < rows; i++) {
        uint32_t h = hash32(seed + (uint32_t)i * 7919);
        double y = pr.top + 5.0 + (pr.bottom - pr.top - 10.0) * (i + ((h & 255) / 255.0 - 0.5) * 0.4) / rows;
        if ((h >> 8) % 5 != 0) add_rivet(p, fe_view_x(&p->view, pr.left + 4.0), fe_view_y(&p->view, y), u * 1.7, MAT_IRON, h, 1.1f);
        if ((h >> 12) % 5 != 0) add_rivet(p, fe_view_x(&p->view, pr.right - 4.0), fe_view_y(&p->view, y + ((h >> 16) & 7) - 3.5),
                                          u * 1.7, MAT_IRON, h + 3, 1.1f);
    }
    add_patches(p, s, w, seed);
    if (w->title > 0) add_tab(p, pr, fe_scene_str(s, w->title), hash32(seed + w->kind));
}

static int groups_values(int kind) { return kind == FE_W_CHECK || kind == FE_W_SLIDER || kind == FE_W_FIELD; }

/* One well behind a window's settings (check boxes, sliders, bound keys and their labels), one per list. */
static void add_wells(fe_painter_t* p, const fe_scene_t* s, const fe_window_t* w) {
    fe_rect_t group = fe_rect(0, 0, 0, 0), inner = fe_rect_inflate(w->panel, -11, -11);
    int any = 0;
    for (int i = w->first; i < w->first + w->count; i++)
        if (groups_values(s->widgets[i].kind)) any = 1;
    if (any) {
        for (int i = w->first; i < w->first + w->count; i++) {
            const fe_widget_t* x = &s->widgets[i];
            if (groups_values(x->kind) || x->kind == FE_W_LABEL) group = fe_rect_union(group, x->rect);
        }
        group = fe_rect_intersect(fe_rect_inflate(group, 6, 5), inner);
        if (!fe_rect_empty(group)) add_well(p, box_rect(p, group), p->view.sy * 3.0, F_GRILLE, seed_of(group, 3));
    }
}

static rgb_t plaque_label_color(int state) {
    if (state & FE_ST_DISABLED) return rgb(0.20f, 0.19f, 0.18f);
    if (state & FE_ST_PRESSED) return rgb(0.62f, 0.12f, 0.05f);
    if (state & (FE_ST_HOVER | FE_ST_FOCUS)) return k_red;
    return k_ink;
}

/* Each plaque is its own piece of scrap: torn edges, one chipped corner, its own rivets and a label set by hand. */
static void add_button(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    double u = p->view.sy, wu = w->rect.right - w->rect.left, hu = w->rect.bottom - w->rect.top, ix, iy, push;
    int large = w->style == FE_BUTTON_LARGE, hot = (w->state & (FE_ST_HOVER | FE_ST_FOCUS)) && !(w->state & FE_ST_DISABLED);
    uint32_t seed = seed_of(w->rect, w->id), h = hash32(seed);
    int skipped = (int)(h >> 9) & 3;
    text_style_t st;
    box_t b, text_box;
    op_t* plate;
    const char* label = fe_scene_str(s, w->text);
    ix = large ? 2.0 : clampd(wu * 0.02, 1.0, 2.5);
    iy = large ? 2.0 : clampd(hu * 0.06, 1.0, 3.0);
    push = (w->state & FE_ST_PRESSED) ? 1.2 : 0.0;
    b = box_of(p, w->rect.left + ix + push, w->rect.top + iy + push, w->rect.right - ix + push, w->rect.bottom - iy + push);
    if (hot) add_glow(p, b, u * 3.0, u * (large ? 12.0 : 8.0), k_amber, (w->state & FE_ST_PRESSED) ? 0.25f : 0.42f);
    add_shadow(p, b, u * 1.5, u * 3.0, u * (large ? 7.0 : 4.5), u * 3.0, 0.65f);
    plate = add_plate(p, b, u * (large ? 2.5 : 1.8), u * (large ? 3.6 : 2.6), MAT_STEEL,
                      (w->state & FE_ST_DISABLED) ? F_DISABLED : 0, seed);
    make_torn(plate, u * (large ? 2.4 : 1.5), u * (large ? 6.0 + (h % 7) : 2.5 + (h % 4)), h);
    if (plate) plate->light = (w->state & FE_ST_PRESSED) ? 0.82f : hot ? 1.1f : 1.0f;
    if (b.y1 - b.y0 >= u * 22.0) {
        double r = u * (large ? 2.6 : 1.8), in = u * (large ? 7.5 : 4.8);
        float light = (w->state & FE_ST_DISABLED) ? 0.55f : 1.0f;
        for (int corner = 0; corner < 4; corner++) {
            double jx = u * ((double)((h >> (corner * 4)) & 3) - 1.5) * 0.6, jy = u * ((double)((h >> (corner * 4 + 2)) & 3) - 1.5) * 0.5;
            if (corner == skipped) continue;
            add_rivet(p, (corner & 1 ? b.x1 - in : b.x0 + in) + jx, (corner & 2 ? b.y1 - in : b.y0 + in) + jy, r, MAT_STEEL,
                      seed + 1 + (uint32_t)corner, light);
        }
        if (large && (h & 0x10000)) add_rivet(p, (b.x0 + b.x1) * 0.5 + u * 9.0, b.y0 + u * 4.5, r * 0.85, MAT_STEEL, seed + 9, light);
    }
    if (!*label) return;
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_PLAQUE;
    st.weight = FW_NORMAL;
    st.px = (b.y1 - b.y0) * (large ? 0.47 + (h % 5) * 0.012 : 0.52 + (h % 4) * 0.012);
    st.spacing = 0.05 + (h % 3) * 0.02;
    st.color = plaque_label_color(w->state);
    st.flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    st.wear = 0.24 + (h % 5) * 0.03;
    st.wear_size = 40.0;
    st.emboss = u * 0.5;
    st.seed = seed;
    {
        double room = (b.x1 - b.x0) - u * (large ? 26.0 : 12.0), width = measure(p, &st, label, (int)strlen(label));
        if (width > room && width > 0) st.px *= room / width;
    }
    text_box = b;
    text_box.x0 += u * ((double)((h >> 20) % 5) - 2.0) * 0.6;
    text_box.x1 += u * ((double)((h >> 20) % 5) - 2.0) * 0.6;
    text_box.y0 += u * ((double)((h >> 24) % 3) - 1.0) * 0.6;
    text_box.y1 += u * ((double)((h >> 24) % 3) - 1.0) * 0.6;
    add_text(p, &st, label, text_box);
}

static void label_style(const fe_painter_t* p, const fe_widget_t* w, text_style_t* st) {
    double u = p->view.sy;
    memset(st, 0, sizeof *st);
    st->role = FE_FONT_LABEL;
    st->weight = FW_NORMAL;
    st->px = w->text_h * u;
    st->color = rgb_hex(w->color);
    st->flags = w->text_flags;
    st->shadow = (w->text_flags & FE_TEXT_SHADOW) ? (u * 0.7 > 1.0 ? u * 0.7 : 1.0) : 0.0;
    st->fit = !(w->text_flags & FE_TEXT_WRAP);
    if (w->state & FE_ST_ROW_HOVER) st->color = mix(st->color, rgb(1.0f, 0.82f, 0.48f), 0.75f);
    if (w->state & FE_ST_DISABLED) st->color = scale(st->color, 0.5f);
}

static void add_label(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    text_style_t st;
    label_style(p, w, &st);
    add_text(p, &st, fe_scene_str(s, w->text), box_rect(p, w->rect));
}

static void add_field(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    double u = p->view.sy;
    box_t b = box_of(p, w->rect.left + 0.5, w->rect.top + 0.5, w->rect.right - 0.5, w->rect.bottom - 0.5);
    text_style_t st;
    op_t* o = add_well(p, b, u * 1.5, (w->state & (FE_ST_ROW_HOVER | FE_ST_FOCUS | FE_ST_HOVER)) ? F_HOVER : 0,
                       seed_of(w->rect, 5));
    if (o) o->color = rgb(0.03f, 0.03f, 0.028f);
    label_style(p, w, &st);
    if (w->kind == FE_W_EDIT) {
        const char* text = fe_scene_str(s, w->text);
        box_t tb = b;
        tb.x0 += u * 4.0;
        st.flags = (uint16_t)((st.flags & ~(FE_TEXT_CENTER | FE_TEXT_RIGHT)) | FE_TEXT_VCENTER);
        add_text(p, &st, text, tb);
        if (w->state & FE_ST_CARET) {
            int len = (int)strlen(text), at = w->caret < 0 ? 0 : w->caret > len ? len : w->caret;
            double cx = floor(tb.x0 + measure(p, &st, text, at) + 0.5) + u * 0.5, lh = line_height(p, &st);
            op_t* c = op(p, OP_BAR, cx, (b.y0 + b.y1 - lh) * 0.5, cx + (u * 0.7 > 2 ? u * 0.7 : 2), (b.y0 + b.y1 + lh) * 0.5, 1);
            if (c) c->color = k_amber;
        }
        return;
    }
    add_text(p, &st, fe_scene_str(s, w->text), b);
}

static void add_check(fe_painter_t* p, const fe_widget_t* w) {
    double u = p->view.sy, side = (w->rect.bottom - w->rect.top) < (w->rect.right - w->rect.left)
                                       ? w->rect.bottom - w->rect.top : w->rect.right - w->rect.left;
    box_t b = box_of(p, w->rect.left - 1.0, w->rect.top - 1.0, w->rect.left + side + 1.0, w->rect.top + side + 1.0);
    op_t* o = op(p, OP_SOCKET, b.x0, b.y0, b.x1, b.y1, 1);
    if (o) {
        o->radius = u * 2.0;
        o->width = u * 1.6;
        o->color2 = k_amber;
        if (w->state & (FE_ST_HOVER | FE_ST_ROW_HOVER | FE_ST_FOCUS)) o->flags = F_HOVER;
        if (w->state & FE_ST_DISABLED) o->alpha = 0.5f;
    }
    if (w->state & FE_ST_CHECKED) {
        double sz = b.x1 - b.x0;
        o = op(p, OP_STROKE, b.x0 - sz * 0.3, b.y0 - sz * 0.3, b.x1 + sz * 0.3, b.y1 + sz * 0.3, 1);
        if (!o) return;
        o->pts[0] = b.x0 + sz * 0.24;
        o->pts[1] = b.y0 + sz * 0.52;
        o->pts[2] = b.x0 + sz * 0.43;
        o->pts[3] = b.y0 + sz * 0.73;
        o->pts[4] = b.x0 + sz * 0.80;
        o->pts[5] = b.y0 + sz * 0.26;
        o->width = sz * 0.17;
        o->color = k_amber;
        o->color2 = k_amber;
        if (w->state & FE_ST_DISABLED) o->alpha = 0.5f;
    }
}

static void add_slider(fe_painter_t* p, const fe_widget_t* w) {
    double u = p->view.sy, cy = (w->rect.top + w->rect.bottom) * 0.5;
    fe_rect_t track = w->rect;
    int thumb_w = 10, tx = w->value;
    box_t groove = box_of(p, track.left + 2.0, cy - 3.0, track.right - 2.0, cy + 3.0), knob;
    op_t* o = op(p, OP_GROOVE, groove.x0, groove.y0, groove.x1, groove.y1, 1);
    int hot = (w->state & (FE_ST_HOVER | FE_ST_PRESSED | FE_ST_FOCUS)) != 0;
    if (o) {
        o->radius = u * 3.0;
        o->width = u * 2.5;
        o->color = rgb(0.03f, 0.03f, 0.028f);
        o->flags = F_RECESSED;
    }
    o = op(p, OP_BAR, groove.x0 + u, groove.y0 + u, fe_view_x(&p->view, tx), groove.y1 - u, 1);
    if (o && o->x1 > o->x0 + u) {
        o->radius = u * 2.0;
        o->color = k_amber;
        o->flags = F_GRADIENT;
        o->alpha = 0.9f;
    }
    for (int i = 0; i <= 10; i++) {
        double x = groove.x0 + (groove.x1 - groove.x0) * i / 10.0;
        op_t* t = op(p, OP_BAR, x - u * 0.35, fe_view_y(&p->view, cy + 5.5), x + u * 0.35,
                     fe_view_y(&p->view, cy + (i % 5 ? 7.5 : 9.0)), 1);
        if (t) {
            t->color = rgb(0.85f, 0.78f, 0.62f);
            t->alpha = 0.55f;
        }
    }
    knob = box_of(p, tx - thumb_w * 0.5, w->rect.top + 1.0, tx + thumb_w * 0.5, w->rect.bottom - 1.0);
    if (hot) add_glow(p, knob, u * 2.0, u * 6.0, k_amber, 0.45f);
    add_shadow(p, knob, u * 1.0, u * 2.0, u * 3.0, u * 2.0, 0.6f);
    add_plate(p, knob, u * 2.0, u * 2.0, MAT_STEEL, 0, seed_of(w->rect, 9));
    for (int i = -1; i <= 1; i++) {
        double x = (knob.x0 + knob.x1) * 0.5 + i * u * 2.2;
        op_t* g = op(p, OP_BAR, x - u * 0.45, knob.y0 + u * 5.0, x + u * 0.45, knob.y1 - u * 5.0, 1);
        if (g) {
            g->color = rgb(0.10f, 0.09f, 0.08f);
            g->alpha = 0.8f;
        }
    }
}

static void add_arrow_button(fe_painter_t* p, const fe_widget_t* w) {
    double u = p->view.sy, push = (w->state & FE_ST_PRESSED) ? 0.8 : 0.0;
    box_t b = box_of(p, w->rect.left + 1.0 + push, w->rect.top + 1.0 + push, w->rect.right - 1.0 + push,
                     w->rect.bottom - 1.0 + push);
    int hot = (w->state & (FE_ST_HOVER | FE_ST_FOCUS)) && !(w->state & FE_ST_DISABLED);
    op_t *plate, *o;
    if (hot) add_glow(p, b, u * 1.5, u * 4.0, k_amber, 0.4f);
    plate = add_plate(p, b, u * 1.5, u * 1.6, MAT_STEEL, (w->state & FE_ST_DISABLED) ? F_DISABLED : 0, seed_of(w->rect, 8));
    if (plate) plate->light = (w->state & FE_ST_PRESSED) ? 0.8f : hot ? 1.12f : 1.0f;
    o = op(p, OP_ARROW, b.x0, b.y0, b.x1, b.y1, 1);
    if (!o) return;
    o->color = hot ? k_red : k_ink;
    o->alpha = (w->state & FE_ST_DISABLED) ? 0.4f : 1.0f;
    o->flags = w->style == FE_BUTTON_ARROW_DOWN ? F_DOWN : 0;
}

static void add_list(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    double u = p->view.sy;
    fe_rect_t text_area = w->rect;
    text_style_t st;
    op_t* o = add_well(p, box_of(p, w->rect.left - 3.0, w->rect.top - 3.0, w->rect.right + 3.0, w->rect.bottom + 3.0),
                       u * 2.5, 0, seed_of(w->rect, 4));
    if (o) o->color = rgb(0.035f, 0.033f, 0.03f);
    if (!fe_rect_empty(w->track)) {
        fe_rect_t t = w->track, thumb;
        int side = t.right - t.left;
        box_t groove = box_of(p, t.left + 3.0, t.top + side, t.right - 3.0, t.bottom - side);
        op_t* g = op(p, OP_GROOVE, groove.x0, groove.y0, groove.x1, groove.y1, 1);
        text_area.right = t.left - 1;
        if (g) {
            g->radius = u * 2.0;
            g->width = u * 2.0;
            g->color = rgb(0.02f, 0.02f, 0.02f);
            g->flags = F_RECESSED;
        }
        thumb = fe_list_thumb(t, side, w->top, w->rows, w->item_count);
        if (!fe_rect_empty(thumb))
            add_plate(p, box_of(p, thumb.left + 1.5, thumb.top + 0.5, thumb.right - 1.5, thumb.bottom - 0.5), u * 2.0, u * 1.8,
                      MAT_STEEL, 0, 79);
    }
    label_style(p, w, &st);
    st.flags = FE_TEXT_VCENTER;
    for (int r = 0; r < w->rows; r++) {
        int index = w->top + r;
        fe_rect_t row = fe_rect(text_area.left + 1, w->rect.top + r * w->row_h, text_area.right - 1,
                                w->rect.top + (r + 1) * w->row_h);
        box_t rb = box_rect(p, row);
        if (r >= fe_list_shown(w) || row.bottom > w->rect.bottom + 1) break;
        if (index == w->selected) {
            o = op(p, OP_BAR, rb.x0, rb.y0, rb.x1, rb.y1, 1);
            if (o) {
                o->radius = u * 1.5;
                o->color = scale(k_amber, 0.8f);
                o->flags = F_GRADIENT;
                o->alpha = 0.5f;
            }
        }
        rb.x0 += u * 4.0;
        st.color = index == w->selected ? rgb(1.0f, 0.95f, 0.85f) : rgb_hex(w->color);
        if (w->first_item + r < s->item_count) add_text(p, &st, fe_scene_str(s, s->items[w->first_item + r]), rb);
    }
}

/* The game draws a static image 1:1 at its rectangle's corner, cropped to the rectangle: here it is scaled with the
 * canvas and cropped the same way. */
static void set_image(op_t* o, const fe_widget_t* w, const double* src) {
    o->image = w->image;
    o->image32 = w->image32;
    o->image_w = w->image_w;
    o->image_h = w->image_h;
    o->image_pitch = w->image_pitch;
    o->key = w->image_key;
    o->keyed = w->image_keyed;
    for (int i = 0; i < 4; i++) o->src[i] = src[i];
}

/* A save's picture: an iron frame bolted over the plate's edge, the picture fitted (and lightly cropped) inside. */
static void add_photo(fe_painter_t* p, const fe_widget_t* w) {
    double u = p->view.sy, src[4], dst[4], border = 7.0;
    fe_rect_t r = w->rect;
    box_t frame = box_rect(p, r), inner = box_of(p, r.left + border, r.top + border, r.right - border, r.bottom - border);
    uint32_t seed = seed_of(r, 17);
    op_t* o;
    add_shadow(p, frame, u * 3.0, u * 5.0, u * 9.0, u * 3.0, 0.7f);
    add_plate(p, frame, u * 3.0, u * 3.0, MAT_IRON, 0, seed);
    o = add_well(p, box_of(p, r.left + border - 1.5, r.top + border - 1.5, r.right - border + 1.5, r.bottom - border + 1.5),
                 u * 1.5, 0, seed + 1);
    if (o) o->color = rgb(0.015f, 0.015f, 0.015f);
    add_rivet(p, frame.x0 + u * 3.6, frame.y0 + u * 3.6, u * 2.0, MAT_STEEL, seed + 2, 1.0f);
    add_rivet(p, frame.x1 - u * 3.6, frame.y0 + u * 3.6, u * 2.0, MAT_STEEL, seed + 3, 1.0f);
    add_rivet(p, frame.x0 + u * 3.6, frame.y1 - u * 3.6, u * 2.0, MAT_STEEL, seed + 4, 1.0f);
    add_rivet(p, frame.x1 - u * 3.6, frame.y1 - u * 3.6, u * 2.0, MAT_STEEL, seed + 5, 1.0f);
    if ((!w->image && !w->image32) || w->image_w <= 0 || w->image_h <= 0) return;
    fe_photo_fit(w->image_w, w->image_h, inner.x1 - inner.x0, inner.y1 - inner.y0, src, dst);
    o = op(p, OP_IMAGE, inner.x0 + dst[0], inner.y0 + dst[1], inner.x0 + dst[2], inner.y0 + dst[3], 0);
    if (o) set_image(o, w, src);
}

static void add_image(fe_painter_t* p, const fe_widget_t* w) {
    box_t b = box_of(p, w->rect.left, w->rect.top, w->rect.left + w->image_w, w->rect.top + w->image_h);
    fe_rect_t crop = fe_view_rect(&p->view, w->rect);
    double whole[4];
    op_t* o;
    if (w->style == FE_IMAGE_PHOTO) {
        add_photo(p, w);
        return;
    }
    if (!w->image || w->image_w <= 0 || w->image_h <= 0) return;
    if (w->style) {
        double u = p->view.sy;
        op_t* f = add_well(p, box_of(p, w->rect.left - 3.0, w->rect.top - 3.0, w->rect.right + 3.0, w->rect.bottom + 3.0),
                           u * 2.0, 0, 6);
        if (f) f->color = rgb(0.02f, 0.02f, 0.02f);
    }
    o = op(p, OP_IMAGE, b.x0, b.y0, b.x1, b.y1, 0);
    if (!o) return;
    o->bounds = fe_rect_intersect(o->bounds, crop);
    whole[0] = whole[1] = 0;
    whole[2] = w->image_w;
    whole[3] = w->image_h;
    set_image(o, w, whole);
}

static const char k_title[] = "TRESPASSER";
static const char k_footer1[] = "\xA9 1998 DreamWorks Interactive L.L.C. Trespasser is a trademark of UCS & Amblin.";
static const char k_footer2[] = "The Lost World: Jurassic Park\x99 & \xA9 1998 UCS & Amblin. All Rights Reserved.";

static void add_main_screen(fe_painter_t* p) {
    double u = p->view.sy, cell = 39.0, top = 10.0;
    text_style_t st;
    op_t* o = op(p, OP_BACKDROP, 0, 0, p->view.screen_w, p->view.screen_h, 0);
    (void)o;
    o = op(p, OP_PLANK, fe_view_x(&p->view, 0), 0, fe_view_x(&p->view, 200), fe_view_y(&p->view, 440), 2);
    if (o) o->bounds = fe_rect_intersect(fe_rect(0, 0, (int)ceil(fe_view_x(&p->view, 205)), (int)ceil(fe_view_y(&p->view, 445))),
                                         fe_rect(0, 0, p->view.screen_w, p->view.screen_h));
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_PLAQUE;
    st.weight = FW_NORMAL;
    st.px = 47.0 * u;
    st.color = k_bone;
    st.wear = 0.16;
    st.wear_size = 14.0;
    st.shadow = u * 2.2;
    st.seed = 0x1998;
    for (int i = 0; k_title[i]; i++) {
        double w = measure(p, &st, k_title + i, 1) / u, cx = 86.0 + 3.0 * (nz(p, i * 13.0, 3, 50, 9) - 0.5);
        box_t b = box_of(p, cx - w * 0.5, top + cell * i, cx + w * 0.5, top + cell * (i + 1));
        emit_line(p, &st, k_title + i, 1, floor(b.x0 + 0.5), floor(b.y0 + 0.5));
    }
    st.px = 9.0 * u;
    st.wear = 0.0;
    st.shadow = u;
    emit_line(p, &st, "TM", 2, floor(fe_view_x(&p->view, 112) + 0.5), floor(fe_view_y(&p->view, top + cell * 9.6) + 0.5));
    o = op(p, OP_FOOTER, 0, fe_view_y(&p->view, 440), p->view.screen_w, p->view.screen_h, 0);
    if (o) {
        o->y0 = 443.0;
        o->y1 = 456.0;
    }
    memset(&st, 0, sizeof st);
    st.role = FE_FONT_BODY;
    st.weight = FW_NORMAL;
    st.px = 9.0 * u;
    st.color = rgb(0.80f, 0.78f, 0.72f);
    st.flags = FE_TEXT_CENTER;
    add_text(p, &st, k_footer1, box_of(p, 40, 455, 600, 466));
    add_text(p, &st, k_footer2, box_of(p, 40, 466, 600, 477));
}

static void add_widget(fe_painter_t* p, const fe_scene_t* s, const fe_widget_t* w) {
    switch (w->kind) {
    case FE_W_BUTTON:
        if (w->style == FE_BUTTON_ARROW_UP || w->style == FE_BUTTON_ARROW_DOWN) add_arrow_button(p, w);
        else add_button(p, s, w);
        break;
    case FE_W_CHECK: add_check(p, w); break;
    case FE_W_SLIDER: add_slider(p, w); break;
    case FE_W_LABEL: add_label(p, s, w); break;
    case FE_W_FIELD:
    case FE_W_EDIT: add_field(p, s, w); break;
    case FE_W_LIST: add_list(p, s, w); break;
    case FE_W_IMAGE: add_image(p, w); break;
    }
}

/* As the game: windows under the top one show only their own background (the main screen's art), the top one is
 * drawn whole over a dimmed screen. */
static void build(fe_painter_t* p, const fe_scene_t* s) {
    int top = s->window_count - 1;
    p->op_count = 0;
    for (int i = 0; i <= top; i++) {
        const fe_window_t* w = &s->windows[i];
        if (w->kind == FE_WND_MAIN && s->backdrop) {
            p->marking_base = 1;
            add_main_screen(p);
            p->marking_base = 0;
        }
        if (i != top || w->kind == FE_WND_GAME) continue;
        if (i > 0) {
            op_t* d = op(p, OP_DIM, 0, 0, p->view.screen_w, p->view.screen_h, 0);
            if (d) d->alpha = s->windows[0].kind == FE_WND_GAME ? 0.30f : 0.50f;
        }
        if (w->kind != FE_WND_MAIN) {
            add_panel(p, s, w);
            add_wells(p, s, w);
        }
        for (int k = w->first; k < w->first + w->count; k++) add_widget(p, s, &s->widgets[k]);
    }
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
    free(p->base);
    free(p->up);
    free(p->blur);
    p->base = NULL;
    p->up = NULL;
    p->blur = NULL;
    p->base_valid = 0;
    p->backdrop_src = NULL;
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
    p->view = fe_view_make(s->screen_w, s->screen_h);
    for (int i = 0; i < s->window_count; i++)
        if (s->windows[i].kind == FE_WND_MAIN && s->backdrop && !prepare_backdrop(p, s)) return 0;
    build(p, s);
    if (has_base_ops(p) && ensure_base(p, s, threads)) mode = RUN_OVER_BASE;
    execute(p, pixels, pitch, clip, mode, threads);
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
