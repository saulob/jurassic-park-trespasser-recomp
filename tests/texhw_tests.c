/* The pictures custom levels bring (BMP, TGA, DDS) and their hardware texture overrides: decoding, hostile files, which file stands
 * for a texture, what the texture's own format keeps of it, and which textures of the game are told. Headless. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"
#include "jpt_lit.h"
#include "jpt_picture.h"
#include "jpt_texhw.h"
#include "texhw_fixture.h"
#ifdef _WIN32
#include "guest_abi.h"
#include "guest_va.h"
#include "test_profile.h"
#include "texhw_guest.h"
#endif

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static uint8_t g_file[1 << 20];

static uint32_t pixel(const jpt_picture_t* p, int level, uint32_t x, uint32_t y) {
    uint32_t w = 0, h = 0, v = 0;
    const uint8_t* at = jpt_picture_level(p, level, &w, &h);
    if (at) memcpy(&v, at + 4 * ((size_t)y * w + x), 4);
    return v;
}

static void test_bmp(void) {
    jpt_picture_t p;
    size_t size = fx_bmp(g_file, 3, 2, 24, 0x00112233u);
    CHECK(jpt_picture_decode(g_file, size, &p) && p.width == 3 && p.height == 2 && p.levels == 1 && !p.has_alpha && p.size == 24);
    CHECK(pixel(&p, 0, 2, 1) == 0xFF112233u);
    jpt_picture_free(&p);
    CHECK(!p.bgra && !p.size);
    size = fx_bmp(g_file, 4, 4, 32, 0x80112233u);
    CHECK(jpt_picture_decode(g_file, size, &p) && p.has_alpha && pixel(&p, 0, 0, 0) == 0x80112233u);
    jpt_picture_free(&p);
    size = fx_bmp(g_file, 4, 4, 32, 0x00112233u);
    CHECK(jpt_picture_decode(g_file, size, &p) && !p.has_alpha && pixel(&p, 0, 3, 3) == 0xFF112233u);
    jpt_picture_free(&p);

    /* 8 bits, rows from the bottom and from the top. */
    for (int down = 0; down < 2; down++) {
        memset(g_file, 0, 4096);
        g_file[0] = 'B', g_file[1] = 'M';
        fx_put(g_file + 10, 54 + 1024, 4), fx_put(g_file + 14, 40, 4), fx_put(g_file + 18, 2, 4), fx_put(g_file + 22, down ? (uint32_t)-2 : 2, 4), fx_put(g_file + 28, 8, 2);
        fx_put(g_file + 54 + 4 * 7, 0x00010203u, 4), fx_put(g_file + 54 + 4 * 9, 0x00040506u, 4);
        g_file[54 + 1024] = g_file[54 + 1025] = 7;
        g_file[54 + 1028] = g_file[54 + 1029] = 9;
        CHECK(jpt_picture_decode(g_file, 54 + 1024 + 8, &p) && pixel(&p, 0, 0, down ? 0 : 1) == 0xFF010203u && pixel(&p, 0, 1, down ? 1 : 0) == 0xFF040506u);
        jpt_picture_free(&p);
        CHECK(!jpt_picture_decode(g_file, 54 + 1024 + 7, &p) && !p.bgra);
    }
    fx_bmp(g_file, 4, 4, 24, 0);
    fx_put(g_file + 18, 5000, 4);
    CHECK(!jpt_picture_decode(g_file, sizeof g_file, &p));
    fx_bmp(g_file, 4, 4, 24, 0);
    fx_put(g_file + 30, 1, 4);
    CHECK(!jpt_picture_decode(g_file, sizeof g_file, &p));
}

static void test_tga(void) {
    jpt_picture_t p;
    size_t size = fx_tga(g_file, 5, 3, 0x40302010u);
    CHECK(jpt_picture_decode(g_file, size, &p) && p.width == 5 && p.height == 3 && p.has_alpha && pixel(&p, 0, 4, 2) == 0x40302010u);
    jpt_picture_free(&p);
    CHECK(!jpt_picture_decode(g_file, size - 1, &p));

    /* Packed: a run of 5, then 3 of their own, over a row's end. */
    memset(g_file, 0, 64);
    g_file[2] = 10, g_file[16] = 24, g_file[17] = 0;
    fx_put(g_file + 12, 4, 2), fx_put(g_file + 14, 2, 2);
    g_file[18] = 0x84, g_file[19] = 1, g_file[20] = 2, g_file[21] = 3;
    g_file[22] = 2;
    for (int i = 0; i < 9; i++) g_file[23 + i] = (uint8_t)(10 + i);
    CHECK(jpt_picture_decode(g_file, 32, &p) && pixel(&p, 0, 0, 1) == 0xFF030201u && pixel(&p, 0, 0, 0) == 0xFF030201u && pixel(&p, 0, 1, 0) == 0xFF0C0B0Au && pixel(&p, 0, 3, 0) == 0xFF121110u);
    jpt_picture_free(&p);
    CHECK(!jpt_picture_decode(g_file, 31, &p) && !jpt_picture_decode(g_file, 22, &p));

    /* A palette of 2 colours from index 4, and grey. */
    memset(g_file, 0, 64);
    g_file[1] = 1, g_file[2] = 1, g_file[7] = 24, g_file[16] = 8, g_file[17] = 0x20;
    fx_put(g_file + 3, 4, 2), fx_put(g_file + 5, 2, 2), fx_put(g_file + 12, 2, 2), fx_put(g_file + 14, 1, 2);
    memcpy(g_file + 18, "\1\2\3\4\5\6\5\xFF", 8);
    CHECK(jpt_picture_decode(g_file, 26, &p) && pixel(&p, 0, 0, 0) == 0xFF060504u && pixel(&p, 0, 1, 0) == 0xFF030201u);
    jpt_picture_free(&p);
    CHECK(!jpt_picture_decode(g_file, 25, &p));
    memset(g_file, 0, 64);
    g_file[2] = 3, g_file[16] = 8;
    fx_put(g_file + 12, 1, 2), fx_put(g_file + 14, 1, 2);
    g_file[18] = 77;
    CHECK(jpt_picture_decode(g_file, 19, &p) && pixel(&p, 0, 0, 0) == 0xFF4D4D4Du);
    jpt_picture_free(&p);
}

static void test_dds(void) {
    jpt_picture_t p;
    size_t at = fx_dds_head(g_file, 8, 8, 0, "DXT1");
    for (int i = 0; i < 4; i++, at += 8) fx_put(g_file + at, 0xF800, 2), fx_put(g_file + at + 2, 0x001F, 2), fx_put(g_file + at + 4, 0xE4, 4);
    CHECK(jpt_picture_decode(g_file, at, &p) && p.levels == 1 && !p.has_alpha);
    CHECK(pixel(&p, 0, 0, 0) == 0xFFFF0000u && pixel(&p, 0, 1, 0) == 0xFF0000FFu && pixel(&p, 0, 2, 0) == 0xFFAA0055u && pixel(&p, 0, 3, 0) == 0xFF5500AAu && pixel(&p, 0, 4, 4) == 0xFFFF0000u);
    jpt_picture_free(&p);
    CHECK(!jpt_picture_decode(g_file, at - 1, &p));
    /* The other order: half way, and nothing. */
    fx_put(g_file + 128, 0x001F, 2), fx_put(g_file + 130, 0xF800, 2);
    CHECK(jpt_picture_decode(g_file, at, &p) && p.has_alpha && pixel(&p, 0, 2, 0) == 0xFF7F007Fu && pixel(&p, 0, 3, 0) == 0);
    jpt_picture_free(&p);

    at = fx_dds_head(g_file, 4, 4, 3, "DXT3");
    for (int level = 0; level < 3; level++, at += 16) {
        memset(g_file + at, 0, 16);
        fx_put(g_file + at, 0x000000F8u, 4);
        fx_put(g_file + at + 8, 0x07E0, 2);
    }
    CHECK(jpt_picture_decode(g_file, at, &p) && p.levels == 3 && pixel(&p, 0, 0, 0) == 0x8800FF00u && pixel(&p, 0, 1, 0) == 0xFF00FF00u && pixel(&p, 0, 2, 0) == 0x0000FF00u && pixel(&p, 2, 0, 0) == 0x8800FF00u);
    jpt_picture_free(&p);
    /* Levels that are not there are not read. */
    CHECK(jpt_picture_decode(g_file, at - 16, &p) && p.levels == 2);
    jpt_picture_free(&p);
    fx_put(g_file + 28, 0xFFFFFFFFu, 4);
    CHECK(jpt_picture_decode(g_file, at, &p) && p.levels == 3);
    jpt_picture_free(&p);

    at = fx_dds_head(g_file, 4, 4, 0, "DXT5");
    memset(g_file + at, 0, 16);
    g_file[at] = 255, g_file[at + 1] = 0, g_file[at + 2] = 0x88; /* 0, 1, 2 */
    fx_put(g_file + at + 8, 0xFFFF, 2);
    CHECK(jpt_picture_decode(g_file, at + 16, &p) && pixel(&p, 0, 0, 0) == 0xFFFFFFFFu && pixel(&p, 0, 1, 0) == 0x00FFFFFFu && pixel(&p, 0, 2, 0) == 0xDAFFFFFFu);
    jpt_picture_free(&p);
    memcpy(g_file + 84, "DXT4", 4);
    CHECK(jpt_picture_decode(g_file, at + 16, &p) && pixel(&p, 0, 2, 0) == 0xDAFFFFFFu);
    jpt_picture_free(&p);
    memcpy(g_file + 84, "DX10", 4);
    CHECK(!jpt_picture_decode(g_file, at + 16, &p));

    at = fx_dds_head(g_file, 2, 2, 0, NULL);
    for (int i = 0; i < 4; i++, at += 4) fx_put(g_file + at, 0x80112233u, 4);
    CHECK(jpt_picture_decode(g_file, at, &p) && pixel(&p, 0, 1, 1) == 0x80112233u);
    jpt_picture_free(&p);
    fx_put(g_file + 92, 0xFF00FF00u, 4);
    CHECK(!jpt_picture_decode(g_file, at, &p));
}

static void test_levels(void) {
    jpt_picture_t p;
    uint32_t w = 0, h = 0;
    size_t size = fx_tga(g_file, 8, 2, 0xFF808080u);
    fx_put(g_file + 18, 0x00FFFFFFu, 4);
    CHECK(jpt_picture_decode(g_file, size, &p) && jpt_picture_complete(&p, 0) && p.levels == 4 && p.size == 4u * (16 + 4 + 2 + 1));
    CHECK(pixel(&p, 1, 0, 0) == 0xBF808080u && pixel(&p, 1, 1, 0) == 0xFF808080u && jpt_picture_level(&p, 3, &w, &h) && w == 1 && h == 1 && !jpt_picture_level(&p, 4, &w, &h));
    jpt_picture_limit(&p, 4);
    CHECK(p.width == 4 && p.height == 1 && p.levels == 3 && p.size == 4u * 7 && pixel(&p, 0, 0, 0) == 0xBF808080u);
    jpt_picture_limit(&p, 0);
    CHECK(p.levels == 1 && p.width == 1);
    jpt_picture_free(&p);
    CHECK(jpt_picture_decode(g_file, size, &p) && jpt_picture_complete(&p, 1) && pixel(&p, 0, 0, 0) == 0x00FFFFFFu && pixel(&p, 1, 0, 0) == 0xFF808080u);
    jpt_picture_free(&p);
}

static void test_hostile(void) {
    uint32_t seed = 12345;
    static uint8_t bytes[4096];
    unsigned long decoded = 0;
    for (int round = 0; round < 60000; round++) {
        jpt_picture_t p;
        size_t size;
        switch (round % 5) {
        case 0: size = fx_bmp(bytes, 8, 8, 24, round); break;
        case 1: size = fx_tga(bytes, 8, 8, round), bytes[2] = (uint8_t)(round % 12); break;
        case 2: size = fx_dds_head(bytes, 16, 16, 5, "DXT1") + 700; break;
        case 3: size = fx_dds_head(bytes, 16, 16, 5, "DXT5") + 700; break;
        default: size = fx_dds_head(bytes, 8, 8, 4, NULL) + 400; break;
        }
        for (int i = 0; i < 1 + round % 6; i++) {
            seed = seed * 1664525u + 1013904223u;
            bytes[(seed >> 8) % (round % 2 ? 128 : size)] = (uint8_t)(seed >> 24);
        }
        seed = seed * 1664525u + 1013904223u;
        if (round % 3 == 0) size = (seed >> 8) % (size + 1);
        if (!jpt_picture_decode(bytes, size, &p)) {
            g_failures += p.bgra != NULL;
            continue;
        }
        decoded++;
        g_failures += !jpt_picture_complete(&p, round & 1) || p.width > JPT_PICTURE_SIDE_MAX || p.levels > JPT_PICTURE_LEVELS_MAX;
        jpt_picture_free(&p);
    }
    CHECK(decoded > 5000);
}

/* ---- overrides ---- */

static char g_user[512];

static int in_game(const char* relative, char* out, size_t cap) {
    snprintf(out, cap, "%s/game/%s", g_user, relative);
    return jpt_fs_exists(out);
}

static int stands(const char* texture, const char* end) {
    char path[1024];
    size_t n, m = strlen(end);
    if (!jpt_custom_level_texhw(texture, "", path, sizeof path)) return !m;
    for (char* c = path; *c; c++)
        if (*c == '\\') *c = '/';
    n = strlen(path);
    return m && n >= m && !_stricmp(path + n - m, end);
}


/* ---- normal-mapped polygons ---- */

static int near(float a, float b, float within) { return fabsf(a - b) <= within; }

static void test_lit_light(void) {
    jpt_lit_lights_t l;
    jpt_lit_bump_t b;
    memset(&l, 0, sizeof l);
    CHECK(!jpt_lit_combine(&l, &b));
    l.ambient = 0.25f, l.count = 1, l.light[0].towards[2] = 0.999f, l.light[0].strength = 0.8f;
    CHECK(jpt_lit_combine(&l, &b) && near(b.strength, 0.6f, 1e-6f) && near(b.ambient, 0.25f, 1e-6f) && near(b.towards[2], 1.0f, 1e-6f));
    l.count = 2, l.light[1].towards[0] = 0.999f, l.light[1].strength = 0.4f;
    {
        const float total = 0.8f + 0.4f - 0.32f, length = 0.999f * sqrtf(0.8f * 0.8f + 0.4f * 0.4f), strength = length * total / 1.2f;
        CHECK(jpt_lit_combine(&l, &b) && near(b.strength, strength * 0.75f, 1e-5f) && near(b.ambient, 0.25f + (total - strength) * 0.75f, 1e-5f));
        CHECK(near(b.towards[0] / b.towards[2], 0.5f, 1e-5f));
    }
    l.light[1].towards[0] = 0, l.light[1].towards[2] = -0.999f, l.light[1].strength = 0.8f;
    CHECK(!jpt_lit_combine(&l, &b));
    {
        const jpt_lit_material_t m = { 1.0f, 1.0f, 0.9f, 0.0f };
        const float eye[3] = { 1, 0, 0 };
        l.count = 1;
        CHECK(jpt_lit_combine(&l, &b));
        jpt_lit_specular(&b, &m, eye);
        CHECK(b.towards[0] > 0.3f && b.towards[2] > b.towards[0] && near(b.towards[0] * b.towards[0] + b.towards[2] * b.towards[2], 1.0f, 1e-5f));
    }
}

/* The engine's mx3ObjToTexture, of a polygon's first three points. */
static void engine_towards(const float at[3][3], const float uv[3][2], const float n[3], const float light[3], float out[3]) {
    float x[3], z[3], d[2], length = 0.0f, area, lx, lz;
    for (int k = 0; k < 3; k++) x[k] = (at[1][k] + at[2][k]) * 0.5f - at[0][k], length += x[k] * x[k];
    for (int k = 0; k < 3; k++) x[k] /= sqrtf(length);
    z[0] = x[1] * n[2] - x[2] * n[1], z[1] = x[2] * n[0] - x[0] * n[2], z[2] = x[0] * n[1] - x[1] * n[0];
    area = (uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[1][1] - uv[0][1]) * (uv[2][0] - uv[0][0]);
    for (int k = 0; k < 2; k++) d[k] = (uv[1][k] + uv[2][k]) * 0.5f - uv[0][k];
    length = sqrtf(d[0] * d[0] + d[1] * d[1]);
    d[0] /= length, d[1] /= length;
    lx = light[0] * x[0] + light[1] * x[1] + light[2] * x[2];
    lz = light[0] * z[0] + light[1] * z[1] + light[2] * z[2];
    if (area < 0) lz = -lz;
    out[0] = lx * d[0] + lz * d[1];
    out[1] = lx * d[1] - lz * d[0];
    out[2] = light[0] * n[0] + light[1] * n[1] + light[2] * n[2];
}

static void test_lit_frame(void) {
    static const float k_at[3][3] = { { 2, 10, 1 }, { 5, 10, 1 }, { 5, 10, 3 } }; /* facing the eye: out is -y */
    static const float k_uv[2][3][2] = { { { 0, 0 }, { 3, 0 }, { 3, -2 } }, { { 0, 0 }, { 3, 0 }, { 3, 2 } } };
    const float n[3] = { 0, -1, 0 }, light[3] = { 0.48f, -0.6f, 0.64f };
    jpt_lit_frame_t f;
    jpt_lit_bump_t b = { 0.5f, 0.1f, { 0.48f, -0.6f, 0.64f } };
    float towards[3], engine[3];
    for (int mirrored = 0; mirrored < 2; mirrored++) {
        CHECK(jpt_lit_face(k_at, k_uv[mirrored], 3, &f) && near(f.out[1], -1.0f, 1e-6f) && near(f.u[0], 1.0f, 1e-6f) && near(f.v[2], mirrored ? 1.0f : -1.0f, 1e-6f));
        jpt_lit_towards(&f, f.out, &b, -1.0f, towards);
        engine_towards(k_at, k_uv[mirrored], n, light, engine);
        for (int k = 0; k < 3; k++) CHECK(near(towards[k], engine[k], 1e-5f));
        jpt_lit_towards(&f, f.out, &b, 0.28f, towards);
        CHECK(near(towards[2], 0.28f, 1e-6f) && near(towards[0] * towards[0] + towards[1] * towards[1], 1.0f - 0.28f * 0.28f, 1e-5f) && near(towards[0] / towards[1], engine[0] / engine[1], 1e-4f));
    }
    {
        const float flat[3][2] = { { 0, 0 }, { 1, 1 }, { 2, 2 } }, line[3][3] = { { 0, 1, 0 }, { 1, 1, 0 }, { 2, 1, 0 } }, smooth[3] = { 0.6f, -0.8f, 0 };
        CHECK(!jpt_lit_face(k_at, flat, 3, &f) && !jpt_lit_face(line, k_uv[0], 3, &f) && !jpt_lit_face(k_at, k_uv[0], 2, &f));
        CHECK(jpt_lit_face(k_at, k_uv[0], 3, &f));
        jpt_lit_towards(&f, smooth, &b, -1.0f, towards);
        CHECK(near(towards[2], 0.48f * 0.6f + 0.6f * 0.8f, 1e-5f) && near(towards[0] * towards[0] + towards[1] * towards[1] + towards[2] * towards[2], 1.0f, 1e-5f));
    }
}

static void test_lit_cosine(void) {
    const jpt_lit_shade_t s = { 0.7f, 0.999f, 0.2f, 24.0f };
    const jpt_lit_material_t m = { 0.9f, 0, 0, 0 }, shiny = { 0.9f, 0.5f, 0.9f, 0 };
    const jpt_lit_bump_t b = { 0.6f, 0.3f, { 0, 0, 1 } };
    for (float c = 0.1f; c < 1.0f; c += 0.1f) {
        const float shown = s.scale * powf((b.ambient + c * b.strength) * m.diffuse, s.exponent), value = (shown - s.least) / (1.0f - s.least) * s.shaded;
        CHECK(near(jpt_lit_cosine(&s, &m, &b, value), c, 2e-3f) && jpt_lit_cosine(&s, &shiny, &b, value) < 0);
    }
    CHECK(jpt_lit_cosine(&s, &m, &b, 0) < 0 && near(jpt_lit_cosine(&s, &m, &b, 30.0f), 1.0f, 1e-6f));
}

/* Trespasser CE's pixel shaders, register by register. */
static void ce_pixel(const jpt_lit_vertex_t* v, const float v0[3], const float v1[3], const float r1[4], const float norm[4], float out[3]) {
    const float length = sqrtf(v->light[0] * v->light[0] + v->light[1] * v->light[1] + v->light[2] * v->light[2]);
    float x = 0, y, z, r2;
    for (int k = 0; k < 3; k++) x += (norm[k] * 2 - 1) * v->light[k] / length;
    x = x >= 0 ? x * v->light[3] : 0;
    x += v->material[3];
    if (v->material[1] > 0) {
        y = v->material[2] * -0.998749197f + x;
        x *= v->material[0];
        z = 1 / (v->material[2] * -0.998749197f + 0.998749197f);
        y = z * y < 0 ? 0 : z * y > 1 ? 1 : z * y;
        y *= norm[3];
        r2 = y * (v->material[1] - x) + x;
        x = powf(r2, v->shade[0]);
        y = r2 - 1;
        x = y >= 0 ? 1 : x;
        r2 = x > v->shade[1] ? x : v->shade[1];
        for (int k = 0; k < 3; k++) {
            const float r4 = r2 * (v1[k] - v0[k]) + v0[k], r0 = y >= 0 ? y * v1[k] : 0;
            out[k] = r1[k] * (r4 - r0) + r0;
        }
        return;
    }
    y = x * v->material[0];
    x = x * v->material[0] - 1 >= 0 ? 1 : powf(y, v->shade[0]);
    r2 = x > v->shade[1] ? x : v->shade[1];
    for (int k = 0; k < 3; k++) out[k] = (r2 * (v1[k] - v0[k]) + v0[k]) * r1[k];
}

static void test_lit_pixel(void) {
    static const float k_lights[][3] = { { 0, 0, 1 }, { 0.6f, 0, 0.8f }, { -0.5f, 0.7f, 0.2f }, { 0, -2, 0.1f }, { 0.1f, 0.1f, -1 } };
    static const float k_norm[][4] = { { 0.5f, 0.5f, 1, 1 }, { 0.8f, 0.5f, 0.9f, 0.5f }, { 0.2f, 0.9f, 0.7f, 0 } };
    const float dark[3] = { 0.1f, 0.05f, 0.2f }, full[3] = { 1, 0.9f, 0.8f }, texture[4] = { 0.7f, 0.4f, 0.9f, 1 };
    unsigned checked = 0;
    for (size_t l = 0; l < sizeof k_lights / sizeof k_lights[0]; l++)
        for (size_t n = 0; n < sizeof k_norm / sizeof k_norm[0]; n++)
            for (int specular = 0; specular < 3; specular++)
                for (float strength = 0.2f; strength < 1.5f; strength += 0.6f) {
                    const jpt_lit_vertex_t v = { { k_lights[l][0], k_lights[l][1], k_lights[l][2], strength }, { 1.1f, specular * 0.8f, 0.7f, 0.15f }, { 0.75f, 0.2f } };
                    float got[3], want[3];
                    jpt_lit_pixel(&v, dark, full, texture, k_norm[n], got);
                    ce_pixel(&v, dark, full, texture, k_norm[n], want);
                    for (int k = 0; k < 3; k++) CHECK(near(got[k], want[k], 1e-5f));
                    checked++;
                }
    CHECK(checked == 135);
    {
        const jpt_lit_vertex_t v[3] = { { { 0, 0, 1, 1 }, { 1, 0, 0, 0 }, { 1, 0 } } };
        const jpt_lit_draw_t d = { 1, 3, 0, 0xFFFFFF, v };
        CHECK(!jpt_lit_draw(3));
        jpt_lit_set_draw(&d);
        CHECK(jpt_lit_draw(3) == &d && !jpt_lit_draw(4));
        jpt_lit_set_draw(NULL);
        CHECK(!jpt_lit_draw(3));
    }
}

static int g_logged;
static void count_line(const char* line) { g_logged += strstr(line, "normal maps") != NULL; }

static void test_overrides(void) {
    static const char* const k_files[] = { "data/texhw/Map/w/crate.dds", "data/texhw/Map/w/crate.bmp", "data/texhw/map/crate.tga", "data/texhw/map/Leaf.TGA", "data/texhw/map/pane.tga",
                                           "data/texhw/Map/pane.norm.dds", "data/texhw/Map/other/crate.dds", "data/texhw/Map/w/broken.dds", "data/texhw/Map/broken.bmp", "data/texhw/Map/w/wall.bmp" };
    char dir[600];
    jpt_picture_t p;
    size_t size;
    int crate, leaf, pane;
    unsigned generation;
    snprintf(dir, sizeof dir, "%s/custom_levels/Tex", g_user);
    size = fx_dds_head(g_file, 2, 2, 0, NULL);
    for (int i = 0; i < 4; i++, size += 4) fx_put(g_file + size, 0x40102030u, 4);
    CHECK(fx_file(dir, "texhw/Map/w/crate.dds", g_file, size) && fx_file(dir, "texhw/Map/other/crate.dds", g_file, size) && fx_file(dir, "texhw/Map/pane.norm.dds", g_file, size));
    CHECK(fx_file(dir, "texhw/Map/w/broken.dds", g_file, 130) && fx_file(dir, "texhw/Map/broken.bmp", "BM", 2));
    size = fx_bmp(g_file, 4, 4, 24, 0x00AABBCCu);
    CHECK(fx_file(dir, "texhw/Map/w/crate.bmp", g_file, size) && fx_file(dir, "texhw/Map/w/wall.bmp", g_file, size));
    snprintf(dir, sizeof dir, "%s/game", g_user);
    CHECK(fx_file(dir, "texhw/Map/w/wall.dds", g_file, size) && fx_file(dir, "texhw/Map/sand.bmp", g_file, size) && fx_file(dir, "texhw/Map/crate.dds", g_file, size));
    snprintf(dir, sizeof dir, "%s/custom_levels/Tex", g_user);
    size = fx_tga(g_file, 4, 4, 0x60AABBCCu);
    CHECK(fx_file(dir, "texhw/map/crate.tga", g_file, size) && fx_file(dir, "texhw/map/pane.tga", g_file, size));
    fx_put(g_file + 18, 0xFF010203u, 4);
    CHECK(fx_file(dir, "texhw/map/Leaf.TGA", g_file, size));

    jpt_texhw_name(1, "Map\\w\\crate.bmp");
    CHECK(!jpt_texhw_active() && !jpt_texhw_find(1) && stands("Map\\w\\crate.bmp", ""));
    CHECK(fx_activate(g_user, k_files, (int)(sizeof k_files / sizeof k_files[0])) && jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW));
    jpt_custom_level_set_game(in_game);
    CHECK(stands("Map\\w\\crate.bmp", "texhw/Map/w/crate.dds") && stands("MAP/W/CRATE.BMP", "texhw/Map/w/crate.dds"));
    CHECK(stands("Map\\x\\crate.bmp", "Tex/texhw/map/crate.tga") || stands("Map\\x\\crate.bmp", "game/texhw/Map/crate.dds"));
    CHECK(stands("Map\\w\\leaf.bmp", "texhw/map/Leaf.TGA") && stands("Map\\w\\sand.bmp", "game/texhw/Map/sand.bmp") && stands("Map\\w\\wall.bmp", "game/texhw/Map/w/wall.dds"));
    CHECK(stands("Map\\w\\gone.bmp", "") && stands("..\\Map\\w\\crate.bmp", "") && stands("", "") && stands("C:\\crate.bmp", ""));

    jpt_texhw_set_log(count_line);
    jpt_texhw_name(0x30, "Map\\w\\pane.bmp");
    jpt_texhw_name(0x10, "Map\\w\\crate.bmp");
    jpt_texhw_name(0x11, "Map\\w\\crate.bmp");
    jpt_texhw_name(0x20, "Map\\w\\leaf.bmp");
    jpt_texhw_name(0x40, "Map\\w\\gone.bmp");
    jpt_texhw_name(0x50, "Map\\w\\broken.bmp");
    crate = jpt_texhw_find(0x10), leaf = jpt_texhw_find(0x20), pane = jpt_texhw_find(0x30);
    CHECK(jpt_texhw_active() && crate && crate == jpt_texhw_find(0x11) && leaf && leaf != crate && pane && !jpt_texhw_find(0x40) && !jpt_texhw_find(0x12) && !g_logged);
    CHECK(jpt_texhw_normals() && jpt_texhw_normal(0x30) && jpt_texhw_normal(0x30) != pane && !jpt_texhw_normal(0x10) && !jpt_texhw_normal(0x20) && !jpt_texhw_normal(0x99));

    jpt_texhw_frame();
    CHECK(jpt_texhw_picture(crate, JPT_TEXHW_OPAQUE, &p) == 1 && p.levels == 2 && pixel(&p, 0, 0, 0) == 0xFF102030u && pixel(&p, 1, 0, 0) == 0xFF102030u);
    jpt_picture_free(&p);
    CHECK(jpt_texhw_picture(crate, JPT_TEXHW_BLENDED, &p) == 1 && pixel(&p, 0, 0, 0) == 0x40102030u);
    jpt_picture_free(&p);
    CHECK(jpt_texhw_picture(jpt_texhw_normal(0x30), JPT_TEXHW_NORMAL, &p) == 1 && pixel(&p, 0, 0, 0) == 0x40102030u && p.levels == 2);
    jpt_picture_free(&p);
    CHECK(jpt_texhw_picture(leaf, JPT_TEXHW_CUTOUT, &p) == 1 && pixel(&p, 0, 0, 0) == 0xFF010203u && pixel(&p, 0, 1, 0) == 0x00AABBCCu && p.levels == 3);
    jpt_picture_free(&p);
    CHECK(jpt_texhw_picture(jpt_texhw_find(0x50), JPT_TEXHW_OPAQUE, &p) == 0 && !p.bgra && jpt_texhw_picture(0, JPT_TEXHW_OPAQUE, &p) == 0 && jpt_texhw_picture(9999, JPT_TEXHW_OPAQUE, &p) == 0);

    /* Only so much in a frame. */
    {
        static uint8_t big[4 * 2048 * 2048 + 64];
        snprintf(dir, sizeof dir, "%s/custom_levels/Tex", g_user);
        size = fx_tga(big, 2048, 2048, 0xFF00FF00u);
        CHECK(fx_file(dir, "texhw/map/pane.tga", big, size));
        jpt_texhw_frame();
        CHECK(jpt_texhw_picture(pane, JPT_TEXHW_OPAQUE, &p) == 1 && p.width == 2048 && p.levels == 12);
        jpt_picture_free(&p);
        CHECK(jpt_texhw_picture(leaf, JPT_TEXHW_OPAQUE, &p) == -1 && !p.bgra);
        jpt_texhw_frame();
        CHECK(jpt_texhw_picture(leaf, JPT_TEXHW_OPAQUE, &p) == 1);
        jpt_picture_free(&p);
    }
    generation = jpt_texhw_generation();
    jpt_custom_level_deactivate();
    CHECK(!jpt_texhw_active() && !jpt_texhw_normals() && !jpt_texhw_normal(0x30) && !jpt_texhw_find(0x10) && jpt_texhw_generation() != generation && jpt_texhw_picture(crate, JPT_TEXHW_OPAQUE, &p) == 0 && stands("Map\\w\\crate.bmp", ""));
}

static void test_budget(void) {
    static const char* const k_files[] = { "data/texhw/Map/a.tga", "data/texhw/Map/b.tga", "data/texhw/Map/c.tga", "data/texhw/Map/d.tga", "data/texhw/Map/e.tga", "data/texhw/Map/f.tga",
                                           "data/texhw/Map/g.tga", "data/texhw/Map/h.tga", "data/texhw/Map/i.tga", "data/texhw/Map/j.tga", "data/texhw/Map/k.tga", "data/texhw/Map/l.tga" };
    const int count = (int)(sizeof k_files / sizeof k_files[0]);
    static uint8_t big[4 * 2048 * 2048 + 64];
    const size_t size = fx_tga(big, 2048, 2048, 0xFF00FF00u);
    char dir[600], name[64];
    jpt_picture_t p;
    size_t total = 0;
    unsigned generation;
    snprintf(dir, sizeof dir, "%s/custom_levels/Tex", g_user);
    for (int i = 0; i < count; i++) CHECK(fx_file(dir, k_files[i] + 5, big, size));
    CHECK(fx_activate(g_user, k_files, count));
    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof name, "Map\\w\\%c.bmp", 'a' + i);
        jpt_texhw_name(100u + (uint32_t)i, name);
    }
    generation = jpt_texhw_generation();
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < count; i++)
            for (int k = 0; k < JPT_TEXHW_CLASSES; k++) {
                jpt_texhw_frame();
                if (jpt_texhw_picture(jpt_texhw_find(100u + (uint32_t)i), (jpt_texhw_class_t)k, &p) != 1) continue;
                if (generation != jpt_texhw_generation()) generation = jpt_texhw_generation(), total = 0;
                if (!round) total += p.size;
                jpt_picture_free(&p);
            }
    CHECK(total > (200u << 20) && total <= (256u << 20));
    jpt_custom_level_deactivate();
}

/* ---- which textures of the game are told ---- */

#ifdef _WIN32
void host_log(const char* tag, const char* fmt, ...) { (void)tag, (void)fmt; }
int guest_read32(uint32_t va, uint32_t* out) {
    if (va < 0x10000u) return 0;
    memcpy(out, (const void*)(uintptr_t)va, 4);
    return 1;
}
int guest_readable(uint32_t va, uint32_t bytes) { return va >= 0x10000u && bytes; }
uint32_t g_eax, g_ecx, g_edx, g_esp;

typedef struct {
    uint32_t words[0x80 / 4];
} block_t;
static block_t g_polygon[4], g_texture[4], g_raster[4], g_linked[4];
static uint32_t g_list[4], g_told[8];
static int g_ids[8], g_count;

static uint32_t at(const void* p) { return (uint32_t)(uintptr_t)p; }

static void tell(uint32_t iface, int id) {
    if (g_count < 8) g_told[g_count] = iface, g_ids[g_count] = id;
    g_count++;
}

static void scene(void) {
    memset(g_polygon, 0, sizeof g_polygon), memset(g_texture, 0, sizeof g_texture), memset(g_raster, 0, sizeof g_raster), memset(g_linked, 0, sizeof g_linked);
    for (uint32_t i = 0; i < 4; i++) {
        g_list[i] = at(&g_polygon[i]);
        g_polygon[i].words[0x0C / 4] = at(&g_texture[i]);
        g_polygon[i].words[0x74 / 4] = 6;
        g_texture[i].words[0x08 / 4] = at(&g_raster[i]);
        g_texture[i].words[0x20 / 4] = 1;
        g_texture[i].words[0x38 / 4] = 0x1000 + i;
        g_raster[i].words[0x08 / 4] = g_linked[i].words[0x08 / 4] = 64;
        g_raster[i].words[0x0C / 4] = g_linked[i].words[0x0C / 4] = 32;
        g_raster[i].words[0x48 / 4] = at(&g_linked[i]);
        g_linked[i].words[0x5C / 4] = 0x00900000u + i;
    }
    g_count = 0;
}

static void test_tags(void) {
    static const char* const k_files[] = { "data/texhw/Map/w/crate.dds", "data/texhw/map/Leaf.TGA" };
    scene();
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(!g_count);
    CHECK(fx_activate(g_user, k_files, 2));
    jpt_texhw_name(0x1000, "Map\\w\\crate.bmp");
    jpt_texhw_name(0x1002, "Map\\w\\leaf.bmp");
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(g_count == 2 && g_told[0] == 0x00900000u && g_ids[0] == jpt_texhw_find(0x1000) && g_told[1] == 0x00900002u && g_ids[1] == jpt_texhw_find(0x1002));
    /* A second mip, a mip that is not there, the flat fill, an unlinked one, a padded one and a texture that only looks like it. */
    scene();
    g_texture[0].words[0x20 / 4] = 2;
    g_texture[0].words[0x0C / 4] = at(&g_raster[3]);
    g_polygon[0].words[0x30 / 4] = 1;
    texhw_tag_polygons(1, at(g_list), tell);
    CHECK(g_count == 1 && g_told[0] == 0x00900003u);
    scene();
    g_texture[0].words[0x20 / 4] = 2;
    g_polygon[0].words[0x30 / 4] = 5;
    g_polygon[2].words[0x74 / 4] = 5;
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(!g_count);
    scene();
    g_raster[0].words[0x48 / 4] = 0;
    g_linked[2].words[0x08 / 4] = 128;
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(!g_count);
    scene();
    g_texture[1].words[0x38 / 4] = 0x1000;
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(g_count == 2 && g_told[0] == 0x00900000u && g_told[1] == 0x00900002u);
    /* The key's texture was made again elsewhere. */
    scene();
    g_texture[0].words[0x38 / 4] = 7;
    g_texture[3].words[0x38 / 4] = 0x1000;
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(g_count == 2 && g_told[1] == 0x00900003u);
    jpt_custom_level_deactivate();
    scene();
    texhw_tag_polygons(4, at(g_list), tell);
    CHECK(!g_count);
}

/* ---- what of the guest's scene lights a normal-mapped polygon ---- */

static struct {
    float corners[12], material[6], presence[2][8], vertices[8][9], gamma[0x210];
    uint32_t table[16], colors[4], list[2], node[3][3], instance[2][16], render[2][4], light[2][8], vertex_lists[2][4];
} W;

static void putf(uint32_t* at, float v) { memcpy(at, &v, 4); }

/* Two polygons facing a camera that looks north, 90 degrees wide and high; the sun is straight up, with a light of the sky. */
static void lit_scene(void) {
    static const float k_corners[12] = { -1, 1, 1, 1, 1, 1, -1, 1, -1, 1, 1, -1 };
    scene();
    memset(&W, 0, sizeof W);
    memcpy(W.corners, k_corners, sizeof k_corners);
    W.material[0] = 0.9f;
    W.table[1] = 0xFF101010u, W.table[2] = 0xFFFFEEDDu, W.table[0x2C / 4] = 24;
    putf(&W.table[0x30 / 4], 0.2f);
    W.table[0] = at(W.material), W.colors[2] = at(W.table);
    W.gamma[0x808 / 4] = 0.7f, W.gamma[0x80C / 4] = 2.0f;
    W.list[0] = at(W.node[0]);
    for (int i = 0; i < 3; i++) W.node[i][0] = at(W.node[(i + 1) % 3]);
    for (int i = 0; i < 2; i++) {
        W.node[i + 1][2] = at(W.instance[i]);
        W.instance[i][0x28 / 4] = at(W.render[i]);
        W.render[i][2] = at(W.light[i]);
        W.light[i][2] = at(W.presence[i]);
        W.presence[i][3] = 1.0f;
    }
    W.light[0][0] = 0x1111, W.light[1][0] = 0x2222; /* a directional one, turned half round x, and an ambient one */
    W.presence[0][3] = 0.0f, W.presence[0][4] = 1.0f;
    putf(&W.light[0][4], 0.8f), putf(&W.light[1][4], 0.25f);
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 4; i++) {
            float* v = W.vertices[4 * p + i];
            v[0] = (float)p + (i == 1 || i == 2), v[1] = 4.0f, v[2] = i < 2 ? 1.0f : 0.0f;
            v[7] = v[0], v[8] = 1.0f - v[2];
            W.vertex_lists[p][i] = at(v);
        }
    for (int p = 0; p < 2; p++) {
        g_polygon[p].words[0] = 4;
        g_polygon[p].words[1] = at(W.vertex_lists[p]);
        g_polygon[p].words[2] = 0x20104;
        g_texture[p].words[0x2C / 4] = at(W.colors);
        g_texture[p].words[0x38 / 4] = 0x1000 + (uint32_t)p;
    }
    jpt_gva[GVAI_0x007328E0] = at(W.corners), jpt_gva[GVAI_0x006D1680] = at(W.gamma), jpt_gva[GVAI_0x00740718] = at(W.list);
    jpt_gva[GVAI_0x0064EBA8] = 0x1111, jpt_gva[GVAI_0x0064EB60] = 0x2222;
}

static const jpt_lit_draw_t* lit_of(int polygon) {
    const jpt_lit_draw_t* d;
    texhw_lit_begin(at(&g_polygon[polygon]));
    d = jpt_lit_draw(4);
    texhw_lit_end();
    return d;
}

static void test_lit_scene(void) {
    static const char* const k_files[] = { "data/texhw/Map/w/crate.dds", "data/texhw/map/pane.tga", "data/texhw/Map/pane.norm.dds" };
    uint32_t kept[JPT_GVA_COUNT];
    const jpt_lit_draw_t* d;
    memcpy(kept, jpt_gva, sizeof kept);
    CHECK(fx_activate(g_user, k_files, 3));
    jpt_texhw_name(0x1000, "Map\\w\\pane.bmp");
    jpt_texhw_name(0x1001, "Map\\w\\crate.bmp");
    lit_scene();
    texhw_tag_polygons(2, at(g_list), tell);
    d = lit_of(0);
    CHECK(g_count == 2 && d && d->normal == jpt_texhw_normal(0x1000) && d->dark == 0x101010 && d->full == 0xFFEEDD && !lit_of(1) && !jpt_lit_draw(4));
    /* Straight up is towards a smaller v. */
    for (int i = 0; d && i < 4; i++) {
        const jpt_lit_vertex_t* v = &d->vertices[i];
        CHECK(near(v->light[0], 0, 1e-5f) && near(v->light[1], -1, 1e-5f) && near(v->light[2], 0, 1e-5f) && near(v->light[3], 0.6f, 1e-5f));
        CHECK(near(v->material[0], 0.9f, 1e-6f) && !v->material[1] && near(v->material[3], 0.25f, 1e-6f) && near(v->shade[0], 0.7f, 1e-6f) && near(v->shade[1], 0.2f, 1e-6f));
    }
    /* A shaded polygon's light rises as its own vertices tell. */
    lit_scene();
    g_polygon[0].words[2] = 0x20106;
    for (int i = 0; i < 4; i++) W.vertices[i][6] = (0.999f * powf((0.25f + 0.5f * 0.6f) * 0.9f, 0.7f) - 0.2f) / 0.8f * 24.0f;
    texhw_tag_polygons(1, at(g_list), tell);
    d = lit_of(0);
    CHECK(d && near(d->vertices[0].light[2], 0.5f, 2e-3f) && near(d->vertices[0].light[1], -sqrtf(0.75f), 2e-3f));
    /* Faces that share a corner and its light share its normal; those that do not, do not. */
    for (int same = 0; same < 2; same++) {
        lit_scene();
        g_texture[1].words[0x38 / 4] = 0x1000;
        g_polygon[1].words[0x0C / 4] = at(&g_texture[0]);
        W.material[1] = 0.5f;
        for (int p = 0; p < 2; p++) g_polygon[p].words[2] = 0x20106;
        W.vertices[5][1] = W.vertices[6][1] = 5.0f; /* the second polygon turns away */
        W.vertices[4][6] = W.vertices[7][6] = same ? 0.0f : 3.0f;
        texhw_tag_polygons(2, at(g_list), tell);
        d = lit_of(0);
        CHECK(d && lit_of(1) && near(d->vertices[1].light[0], d->vertices[0].light[0], 1e-3f) == !same);
    }
    /* Not lit: a copy, an unlit shape, a refractive material, a list that does not end, no view, no texture axes, terrain. */
    for (int refused = 0; refused < 7; refused++) {
        lit_scene();
        if (refused == 0) g_polygon[0].words[2] = 0x105;
        if (refused == 1) putf(&g_polygon[0].words[4], 24.0f);
        if (refused == 2) ((uint32_t*)W.material)[3] = 0x10000;
        if (refused == 3) W.node[2][0] = at(W.node[1]);
        if (refused == 4) memset(W.corners, 0, sizeof W.corners);
        if (refused == 6) g_polygon[0].words[2] = 0x820104;
        if (refused == 5) W.vertices[1][7] = W.vertices[2][7] = W.vertices[0][7];
        texhw_tag_polygons(1, at(g_list), tell);
        CHECK(g_count == 1 && !lit_of(0));
    }
    jpt_custom_level_deactivate();
    lit_scene();
    texhw_tag_polygons(2, at(g_list), tell);
    CHECK(!g_count && !lit_of(0));
    memcpy(jpt_gva, kept, sizeof kept);
}
#endif

int main(void) {
    const char* temp = getenv("TEMP");
    snprintf(g_user, sizeof g_user, "%s/jpt_texhw_tests", temp ? temp : "/tmp");
    test_bmp();
    test_tga();
    test_dds();
    test_levels();
    test_hostile();
    test_overrides();
    test_budget();
    test_lit_light();
    test_lit_frame();
    test_lit_cosine();
    test_lit_pixel();
#ifdef _WIN32
    test_profile_activate();
    test_tags();
    test_lit_scene();
#endif
    printf("texhw_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
