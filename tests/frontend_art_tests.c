/* The HD frontend's picture pipeline (frontend_art.c): scaling, colour keys, de-dithering, the cache and its
 * invalidation, TGA decoding, blits, and the view of a layer larger than the game's screen. */
#include "frontend_art.h"
#include "frontend_layout.h"
#include "jpt_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int* g_checks;
static int* g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        (*g_checks)++;                                                                                                  \
        if (!(cond)) {                                                                                                  \
            (*g_failures)++;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

#define KEY 0x07E0u

static fa_source_t source(const uint16_t* bits, int w, int h, int keyed, uint32_t hash) {
    fa_source_t s;
    memset(&s, 0, sizeof s);
    s.bits = bits;
    s.w = w;
    s.h = h;
    s.pitch = w;
    s.keyed = keyed;
    s.key = KEY;
    s.hash = hash;
    return s;
}

static int channel(uint32_t px, int shift) { return (int)((px >> shift) & 255); }

static void test_flat_and_size(void) {
    uint16_t bits[8 * 6];
    fa_bitmap_t out;
    fa_source_t s;
    for (int i = 0; i < 48; i++) bits[i] = 0xF800;
    s = source(bits, 8, 6, 0, 1);
    CHECK(fa_resample16(&s, NULL, 36, 27, 0, &out), "resample");
    CHECK(out.w == 36 && out.h == 27, "size %dx%d", out.w, out.h);
    for (int i = 0; i < 36 * 27; i++)
        if (out.px[i] != 0xFFFF0000u) {
            CHECK(0, "a flat red picture stays flat red (pixel %d is %08X)", i, out.px[i]);
            break;
        }
    fa_bitmap_free(&out);
    CHECK(!fa_resample16(&s, NULL, 0, 10, 0, &out), "an empty target is refused");
    CHECK(!fa_resample16(NULL, NULL, 10, 10, 0, &out), "no source is refused");
}

static void test_key_and_edges(void) {
    uint16_t bits[16 * 16];
    fa_bitmap_t soft, crisp;
    fa_source_t s;
    int soft_mid = 0, crisp_mid = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) bits[y * 16 + x] = x < 8 ? KEY : 0x001F;
    s = source(bits, 16, 16, 1, 2);
    CHECK(fa_resample16(&s, NULL, 72, 72, 0, &soft), "soft");
    CHECK(fa_resample16(&s, NULL, 72, 72, FA_CRISP_EDGE, &crisp), "crisp");
    CHECK(soft.px[36 * 72 + 2] >> 24 == 0, "the keyed side is transparent");
    CHECK(soft.px[36 * 72 + 70] >> 24 == 255, "the solid side is opaque");
    CHECK(channel(soft.px[36 * 72 + 70], 0) == 255 && channel(soft.px[36 * 72 + 70], 16) == 0, "solid colour kept: %08X",
          soft.px[36 * 72 + 70]);
    for (int x = 0; x < 72; x++) {
        int a = channel(soft.px[36 * 72 + x], 24), c = channel(crisp.px[36 * 72 + x], 24);
        uint32_t p = crisp.px[36 * 72 + x];
        if (a > 10 && a < 245) soft_mid++;
        if (c > 10 && c < 245) crisp_mid++;
        CHECK(channel(p, 0) <= c && channel(p, 8) <= c && channel(p, 16) <= c, "premultiplied: %08X", p);
        CHECK(channel(p, 0) >= c - 1 || c == 0, "the blue stays full strength under alpha: %08X", p);
    }
    CHECK(crisp_mid <= soft_mid && crisp_mid > 0, "crisp edge %d px vs soft %d px", crisp_mid, soft_mid);
    fa_bitmap_free(&soft);
    fa_bitmap_free(&crisp);
}

static void test_dither(void) {
    uint16_t lattice[16 * 16], edge[16 * 16];
    fa_bitmap_t plain, smooth, hard;
    fa_source_t s;
    int plain_var = 0, smooth_var = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            lattice[y * 16 + x] = (uint16_t)(((x + y) & 1) ? 0x6B4D : 0x6B6D);
            edge[y * 16 + x] = x < 8 ? 0x1082 : 0xE71C;
        }
    s = source(lattice, 16, 16, 0, 3);
    CHECK(fa_resample16(&s, NULL, 16, 16, 0, &plain) && fa_resample16(&s, NULL, 16, 16, FA_DEDITHER, &smooth), "resample");
    for (int x = 2; x < 14; x++) {
        int a = channel(plain.px[8 * 16 + x], 8), b = channel(plain.px[8 * 16 + x + 1], 8);
        int c = channel(smooth.px[8 * 16 + x], 8), d = channel(smooth.px[8 * 16 + x + 1], 8);
        plain_var += abs(a - b);
        smooth_var += abs(c - d);
    }
    CHECK(smooth_var < plain_var, "de-dither flattens the lattice (%d -> %d)", plain_var, smooth_var);
    fa_bitmap_free(&plain);
    fa_bitmap_free(&smooth);
    s = source(edge, 16, 16, 0, 4);
    CHECK(fa_resample16(&s, NULL, 16, 16, FA_DEDITHER, &hard), "edge");
    CHECK(channel(hard.px[8 * 16 + 7], 16) < 40 && channel(hard.px[8 * 16 + 8], 16) > 215, "a real edge is kept: %d %d",
          channel(hard.px[8 * 16 + 7], 16), channel(hard.px[8 * 16 + 8], 16));
    fa_bitmap_free(&hard);
}

static void test_shrink_and_part(void) {
    uint16_t bits[16 * 16];
    uint32_t bgra[16 * 16];
    fa_bitmap_t out;
    double right[4] = { 8, 0, 16, 16 };
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            bits[y * 16 + x] = (x + y) & 1 ? 0xFFFF : 0x0000;
            bgra[y * 16 + x] = x < 8 ? 0xFF0000FFu : 0xFFFF0000u;
        }
    {
        fa_source_t s = source(bits, 16, 16, 0, 5);
        CHECK(fa_resample16(&s, NULL, 4, 4, 0, &out), "shrink");
        for (int i = 0; i < 16; i++) CHECK(abs(channel(out.px[i], 8) - 128) <= 24, "a checkerboard shrinks to grey: %d", channel(out.px[i], 8));
        fa_bitmap_free(&out);
    }
    CHECK(fa_resample32(bgra, 16, 16, 16, right, 8, 8, &out), "part");
    CHECK(channel(out.px[3 * 8 + 1], 16) > 240 && channel(out.px[3 * 8 + 1], 0) < 15, "only the right half shows: %08X", out.px[3 * 8 + 1]);
    fa_bitmap_free(&out);
}

static void test_blit(void) {
    uint32_t dst[8 * 8];
    uint32_t solid[4 * 4], half[4 * 4];
    fa_bitmap_t a, b;
    for (int i = 0; i < 64; i++) dst[i] = 0xFF000000u | 0x00FF0000u;
    for (int i = 0; i < 16; i++) {
        solid[i] = 0xFF0000FFu;
        half[i] = 0x80000080u;
    }
    a.w = a.h = 4;
    a.px = solid;
    b.w = b.h = 4;
    b.px = half;
    fa_blit_over(dst, 8, fe_rect(0, 0, 8, 8), &a, 2, 2);
    CHECK(dst[2 * 8 + 2] == 0xFF0000FFu && dst[5 * 8 + 5] == 0xFF0000FFu && dst[1 * 8 + 1] == 0xFFFF0000u, "opaque blit");
    fa_blit_over(dst, 8, fe_rect(0, 0, 8, 8), &b, 4, 4);
    CHECK(abs(channel(dst[6 * 8 + 6], 0) - 128) <= 1 && abs(channel(dst[6 * 8 + 6], 16) - 127) <= 1 && channel(dst[6 * 8 + 6], 24) == 255,
          "half-covered pixel blends over red: %08X", dst[6 * 8 + 6]);
    for (int i = 0; i < 64; i++) dst[i] = 0xFF123456u;
    fa_blit_over(dst, 8, fe_rect(3, 3, 5, 5), &a, 2, 2);
    CHECK(dst[2 * 8 + 2] == 0xFF123456u && dst[3 * 8 + 3] == 0xFF0000FFu && dst[5 * 8 + 5] == 0xFF123456u, "blit stays in its clip");
    fa_blit_over(dst, 8, fe_rect(0, 0, 8, 8), &a, -2, -2);
    CHECK(dst[0] == 0xFF0000FFu && dst[1 * 8 + 1] == 0xFF0000FFu && dst[2 * 8 + 2] == 0xFF123456u, "blit clips at the target's corner");
}

static void test_cache(void) {
    uint16_t bits[8 * 8], other[8 * 8];
    fa_cache_t* c = fa_cache_create(2 * 40 * 40 * 4 + 100);
    fa_source_t s, t;
    const fa_bitmap_t *x, *y, *z;
    for (int i = 0; i < 64; i++) {
        bits[i] = 0x07FF;
        other[i] = 0xF81F;
    }
    s = source(bits, 8, 8, 0, 10);
    t = source(other, 8, 8, 0, 11);
    fa_cache_begin(c);
    x = fa_cache_get16(c, &s, NULL, 40, 40, 0);
    CHECK(x && fa_cache_misses(c) == 1 && fa_cache_hits(c) == 0, "first get is a miss");
    CHECK(fa_cache_get16(c, &s, NULL, 40, 40, 0) == x && fa_cache_hits(c) == 1, "the same picture at the same size is reused");
    y = fa_cache_get16(c, &s, NULL, 44, 40, 0);
    CHECK(y && y != x && fa_cache_misses(c) == 2, "another size is another picture");
    CHECK(fa_cache_entries(c) == 2 && fa_cache_bytes(c) == (size_t)(40 * 40 + 44 * 40) * 4, "accounting");
    z = fa_cache_get16(c, &t, NULL, 40, 40, 0);
    CHECK(z && fa_cache_entries(c) == 3, "pictures used this frame are never evicted (soft budget)");
    fa_cache_begin(c);
    z = fa_cache_get16(c, &t, NULL, 36, 36, 0);
    CHECK(z && fa_cache_bytes(c) <= 2 * 40 * 40 * 4 + 100 + 36 * 36 * 4, "older pictures are evicted once a frame is over");
    CHECK(fa_cache_entries(c) < 4, "entries after eviction: %d", fa_cache_entries(c));
    fa_cache_get16(c, &s, NULL, 40, 40, 0);
    CHECK(fa_cache_misses(c) >= 4, "an evicted picture is made again");
    fa_cache_clear(c);
    CHECK(fa_cache_entries(c) == 0 && fa_cache_bytes(c) == 0, "clear drops everything");
    fa_cache_begin(c);
    x = fa_cache_get16(c, &s, NULL, 40, 40, 0);
    s.hash = 99;
    y = fa_cache_get16(c, &s, NULL, 40, 40, 0);
    CHECK(x && y && x != y, "a changed picture (hash) is not served from the old entry");
    {
        const fa_bitmap_t* d = fa_cache_get16(c, &s, NULL, 40, 40, FA_DEDITHER);
        CHECK(d && d != y, "flags are part of the identity");
    }
    fa_cache_destroy(c);
}

static void put16(uint8_t* p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void test_tga(void) {
    uint8_t raw[18 + 2 * 3 * 2], rle[18 + 64];
    uint32_t* px = NULL;
    int w = 0, h = 0, n = 18;
    memset(raw, 0, sizeof raw);
    raw[2] = 2;
    raw[12] = 2;
    raw[14] = 2;
    raw[16] = 24;
    raw[18] = 255;
    raw[21] = 255;
    raw[26] = 255;
    raw[29] = 255;
    CHECK(fa_tga_decode(raw, 18 + 12, &px, &w, &h) && w == 2 && h == 2, "24-bit uncompressed");
    if (px) {
        CHECK(px[0] == 0xFFFF0000u && px[1] == 0xFFFF0000u, "bottom-up file: the last stored row is the top row (%08X)", px[0]);
        CHECK(px[2] == 0xFF0000FFu && px[3] == 0xFF0000FFu, "the first stored row is the bottom row (%08X)", px[2]);
        free(px);
    }
    px = NULL;
    memset(rle, 0, sizeof rle);
    rle[2] = 10;
    rle[12] = 4;
    rle[14] = 1;
    rle[16] = 16;
    rle[17] = 0x20;
    rle[n++] = 0x80 | 2;
    put16(rle + n, 0x7C00);
    n += 2;
    rle[n++] = 0;
    put16(rle + n, 0x001F);
    n += 2;
    CHECK(fa_tga_decode(rle, (size_t)n, &px, &w, &h) && w == 4 && h == 1, "16-bit run-length");
    if (px) {
        CHECK(px[0] == 0xFFFF0000u && px[2] == 0xFFFF0000u && px[3] == 0xFF0000FFu, "5-5-5 colours: %08X %08X", px[0], px[3]);
        free(px);
    }
    px = NULL;
    CHECK(!fa_tga_decode(rle, 20, &px, &w, &h), "a truncated file is refused");
    rle[2] = 1;
    CHECK(!fa_tga_decode(rle, (size_t)n, &px, &w, &h), "a palette file is refused");
}

static void test_layer_view(void) {
    fe_view_t v = fe_view_make_for(640, 480, 2880, 2160);
    fe_rect_t r;
    int u, w;
    CHECK(fabs(v.sx - 4.5) < 1e-9 && fabs(v.sy - 4.5) < 1e-9 && v.x0 == 0 && v.y0 == 0, "640x480 screen, 2880x2160 layer: %.3f", v.sx);
    r = fe_view_rect(&v, fe_rect(205, 70, 413, 151));
    CHECK(r.left == 922 && r.top == 315 && r.right == 1859 && r.bottom == 680, "main plaque at %d,%d-%d,%d", r.left, r.top, r.right, r.bottom);
    fe_view_to_canvas(&v, r.left + 1, r.top + 1, &u, &w);
    CHECK(u == 205 && w == 70, "layer pixel to canvas: %d,%d", u, w);
    fe_view_to_canvas(&v, r.left - 3, r.top + 20, &u, &w);
    CHECK(u == 204, "outside the plaque is outside the hit box: %d", u);
    v = fe_view_make_for(5424, 1520, 7680, 2160);
    CHECK(v.sy > 4.4 && v.sy < 4.6 && v.x0 > 2300 && v.x0 < 2500, "wide layer: scale %.3f origin %.1f", v.sy, v.x0);
    {
        jpt_ui_canvas_t c = jpt_ui_canvas(5424, 1520);
        fe_view_to_canvas(&v, (int)(v.x0 + 100 * v.sx), (int)(v.y0 + 50 * v.sy), &u, &w);
        CHECK(abs(u - 100) <= 1 && abs(w - 50) <= 1, "pointer mapping agrees with the game's canvas: %d,%d (canvas %d..%d)", u, w,
              c.rect.left, c.rect.right);
    }
}

static void test_level_pictures(void) {
    CHECK(!strcmp(fe_level_picture("as.scn"), "li_a1_0.tga"), "as");
    CHECK(!strcmp(fe_level_picture("AS2.SCN"), "li_a2_0.tga"), "as2, any case");
    CHECK(!strcmp(fe_level_picture("lab.scn"), "li_lab_0.tga"), "lab");
    CHECK(!strcmp(fe_level_picture("TestScene.scn"), "li_other_0.tga"), "unknown levels share the other picture");
    CHECK(!strcmp(fe_level_picture(""), "li_other_0.tga") && !strcmp(fe_level_picture(NULL), "li_other_0.tga"), "empty");
}

void frontend_art_tests(int* checks, int* failures) {
    g_checks = checks;
    g_failures = failures;
    test_flat_and_size();
    test_key_and_edges();
    test_dither();
    test_shrink_and_part();
    test_blit();
    test_cache();
    test_tga();
    test_layer_view();
    test_level_pictures();
}
