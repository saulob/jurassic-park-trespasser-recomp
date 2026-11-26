/* Custom skies: the file and its defaults, Trespasser CE's shapes, the projection on the engine's view, and which level of which
 * session has one. Headless; with file names as arguments, reports what each sky file holds. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_sky.h"
#include "jpt_user_data.h"

/* The guest the layers read: a few fields at made-up addresses. */
#define JPT_GUEST_ABI_H
static uint8_t g_guest[4096];
#define GVA(address) ((uint32_t)(address) & 0xFFFu)
#define ADDR(va) ((uintptr_t)(g_guest + (va)))
static unsigned g_logs;
static void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
    g_logs++;
}
#include "../src/host/sky_layers.c"

static unsigned g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static int near_to(float a, float b) { return fabsf(a - b) <= 0.0005f * (1.0f + fabsf(b)); }

static int parse(const char* text, jpt_sky_t* sky) { return jpt_sky_parse(text, strlen(text), sky); }

static void test_parser(void) {
    jpt_sky_t s;
    CHECK(parse("; a sky\r\n[SKY]\r\nHeight=10000.0\r\nFogNear = 2 ; clamped\r\n\r\n[LAYER1]\r\nType=DOME\r\nColorTopR=124\r\nColorHorizonB=239\r\n"
                "[LAYER3]\r\nType=BODY\r\nBlendMode=burn\r\nTexture=\"data\\texhw\\a;b\\sun\" ; the sun\r\nWidthScale=5.0\r\nElevation=140\r\n[LAYER13]\nType=DOME\n[LAYER2]\nBlendMode=ALPHA\n",
                &s) && s.count == 2);
    CHECK(s.has_height && near_to(s.height, 10000.0f) && s.has_fog_near && near_to(s.fog_near, 1.0f) && !s.has_fog_far);
    CHECK(s.layers[0].number == 1 && s.layers[0].type == JPT_SKY_DOME && near_to(s.layers[0].top[0], 124.0f / 255.0f) && near_to(s.layers[0].horizon[2], 239.0f / 255.0f) && near_to(s.layers[0].top[3], 1.0f));
    CHECK(s.layers[1].number == 3 && s.layers[1].type == JPT_SKY_BODY && s.layers[1].blend == JPT_SKY_BURN && !strcmp(s.layers[1].texture, "data\\texhw\\a;b\\sun") && near_to(s.layers[1].elevation, 140.0f));
    CHECK(near_to(s.layers[0].curvature, 0.02f) && near_to(s.layers[0].gradient, 1.0f) && near_to(s.layers[0].scroll_speed, 1.0f) && s.layers[0].fogged && near_to(s.layers[1].height_scale, 1.0f));

    CHECK(parse("[layer12]\ntype=\"strip\"\ntopfog=3\nBottomFog=-1\n[LAYER4]\nType=DOME\nCurvature=9\nGradientCurve=0\nHorizonHeightBias=7\nColorTopA=900\nAbsoluteHeightStrength=-2\nFogged=0\nFogAsAlpha=1\nColorHorizonR=51\nColorHorizon2G=0\n", &s) && s.count == 2);
    CHECK(s.layers[0].number == 4 && near_to(s.layers[0].curvature, 0.8f) && near_to(s.layers[0].gradient, 0.01f) && near_to(s.layers[0].horizon_bias, 1.0f) && near_to(s.layers[0].top[3], 1.0f) && s.layers[0].absolute_height == 0.0f);
    CHECK(!s.layers[0].fogged && !s.layers[0].fog_as_alpha && s.layers[0].second_horizon && near_to(s.layers[0].horizon2[0], 0.2f) && s.layers[0].horizon2[1] == 0.0f);
    CHECK(s.layers[1].type == JPT_SKY_STRIP && near_to(s.layers[1].top_z, 10.0f) && near_to(s.layers[1].top_fog, 1.0f) && s.layers[1].bottom_fog == 0.0f && s.layers[1].scroll_speed == 0.0f);

    CHECK(!parse("", &s) && !parse("[LAYER1]\nTexture=x\n", &s) && !parse("[LAYER1]\nType=PLANET\n", &s) && !parse("Type=DOME\n", &s));
    CHECK(parse("[LAYER1]\nType=STARS\n[LAYER2]\nType=METEORS\n[LAYER3]\nType=DOME\nTexture=$CSky\n[LAYER4]\nType=BODY\n[LAYER5]\nType=DOME\n", &s) && s.count == 5);
    for (int i = 0; i < 5; i++) CHECK(jpt_sky_drawn(&s.layers[i]) == (i == 4));
    CHECK(parse("[LAYER1]\nType=DOME\nColorTopR=abc\nCurvature=1e99\nUVScale=nan\nHeight", &s) && near_to(s.layers[0].top[0], 1.0f) && near_to(s.layers[0].uv_scale, 1.0f));
    {
        char junk[4096];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (char)(i * 37 + i / 11);
        jpt_sky_parse(junk, sizeof junk, &s);
        memset(junk, '[', sizeof junk);
        CHECK(!jpt_sky_parse(junk, sizeof junk, &s));
    }
}

static jpt_sky_moment_t moment(void) {
    const jpt_sky_moment_t m = { { 0.0f, 0.0f, 0.0f }, 1000.0f, 0.5f, 1.0f, 0.055f, { 3.0f, 1.8f }, 0.0f };
    return m;
}

static const jpt_sky_layer_t* one(const char* text, jpt_sky_t* sky) {
    CHECK(parse(text, sky) && sky->count == 1);
    return &sky->layers[0];
}

static void test_dome(void) {
    static jpt_sky_shape_t shape;
    jpt_sky_t s;
    jpt_sky_moment_t m = moment();
    const jpt_sky_layer_t* l = one("[LAYER1]\nType=DOME\nColorTopR=0\n", &s);
    const float scale = 0.055f / 256.0f;
    float base = 0.02f * 1400.0f;
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && shape.vertex_count == 144 && shape.index_count == 786);
    for (int i = 0; i < shape.index_count; i++) CHECK(shape.indices[i] < 144);
    CHECK(near_to(shape.vertices[0].position[0], 3500.0f) && near_to(shape.vertices[0].position[2], 0.0f) && near_to(shape.vertices[1].position[1], 3500.0f * sinf(-15.0f * 3.14159265f / 180.0f)));
    CHECK(near_to(shape.vertices[0].u, 3500.0f * (1400.0f / (400.0f + base)) * scale) && near_to(shape.vertices[0].v, 0.0f));
    CHECK(near_to(shape.vertices[120].position[2], 1400.0f * 0.968472f) && near_to(shape.vertices[120].u, 0.249121f * 3500.0f * (1400.0f / (400.0f + base + (1400.0f - base) * 0.968472f)) * scale));
    CHECK(near_to(shape.vertices[0].color[0], 1.0f) && near_to(shape.vertices[120].color[0], 0.0f) && near_to(shape.vertices[24].color[0], 1.0f - (1.0f - 0.963185f) / 0.750879f));
    CHECK(near_to(shape.vertices[0].clear, 0.0f) && near_to(shape.vertices[120].clear, 1.0f) && near_to(shape.vertices[72].clear, 1.0f - (0.692532f - 0.5f) / 0.5f));

    m.seconds = 10.0f;
    m.camera[0] = 100.0f, m.camera[2] = 400.0f;
    l = one("[SKY]\nHeight=2000\nFogNear=0\nFogFar=2\n[LAYER1]\nType=DOME\nUVScale=0.1\nUVScrollSpeed=2\nOffsetV=0.25\nHeightOffset=500\nAbsoluteHeightStrength=0.5\nHorizonHeightBias=0.5\nCurvature=0.4\nFogAsAlpha=1\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape));
    base = 700.0f + 0.4f * 700.0f;
    CHECK(near_to(shape.vertices[0].position[2], 700.0f));
    CHECK(near_to(shape.vertices[0].u, 3500.0f * ((1800.0f + 500.0f + 400.0f - 700.0f) / ((400.0f - 700.0f) + base)) * scale * 0.1f + 100.0f * scale * 0.1f + fmodf(10.0f * 2.0f * 3.0f / 256.0f, 1.0f)));
    CHECK(near_to(shape.vertices[0].v, fmodf(10.0f * 2.0f * 1.8f / 256.0f, 1.0f) + 0.25f));
    CHECK(near_to(shape.vertices[0].clear, 0.5f) && near_to(shape.vertices[0].color[3], 0.5f));
    m.camera[2] = 10000.0f;
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && near_to(shape.vertices[0].u - shape.vertices[12].u, 2.0f * 3500.0f * ((100.0f + 500.0f + 400.0f - 700.0f) / ((400.0f - 700.0f) + base)) * scale * 0.1f));

    l = one("[LAYER1]\nType=DOME\nColorHorizonR=255\nColorHorizon2R=0\nHorizonGradientCurve=1\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && near_to(shape.vertices[0].color[0], 1.0f) && near_to(shape.vertices[12].color[0], 0.0f) && near_to(shape.vertices[6].color[0], 0.5f));
}

static void test_body(void) {
    static jpt_sky_shape_t shape;
    jpt_sky_t s;
    const jpt_sky_moment_t m = moment();
    const jpt_sky_layer_t* l = one("[LAYER1]\nType=BODY\nTexture=sun\nWidthScale=2\nHeightScale=4\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 0.5f, &shape) && shape.vertex_count == 4 && shape.index_count == 6);
    CHECK(near_to(shape.vertices[0].position[0], -500.0f) && near_to(shape.vertices[0].position[1], 3500.0f) && near_to(shape.vertices[0].position[2], 500.0f) && shape.vertices[0].v == 0.0f && shape.vertices[1].u == 1.0f);
    CHECK(near_to(shape.vertices[2].position[2], 0.0f) && near_to(shape.vertices[2].v, 0.5f) && near_to(shape.vertices[2].position[0], 500.0f));
    l = one("[LAYER1]\nType=BODY\nTexture=sun\nElevation=-30\n", &s);
    CHECK(!jpt_sky_shape(&s, l, &m, 1.0f, &shape));
    l = one("[LAYER1]\nType=BODY\nTexture=sun\nElevation=45\nAzimuth=90\nColorR=128\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape));
    {
        float centre[3] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 3; k++) centre[k] += shape.vertices[i].position[k] / 4.0f;
        CHECK(near_to(centre[0], -3500.0f * 0.70710678f) && fabsf(centre[1]) < 0.01f && near_to(centre[2], 3500.0f * 0.70710678f) && near_to(shape.vertices[0].color[0], 128.0f / 255.0f));
        CHECK(shape.vertices[0].position[2] > shape.vertices[3].position[2] && shape.vertices[1].position[1] > shape.vertices[0].position[1]);
    }
    l = one("[LAYER1]\nType=BODY\nTexture=sun\nElevation=140\nBlendMode=BURN\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && shape.vertices[0].position[1] < 0.0f && shape.vertices[0].position[2] < shape.vertices[3].position[2] && shape.vertices[0].color[3] <= 1.0f);
    l = one("[LAYER1]\nType=BODY\nTexture=sun\nBlendMode=BURN\nFogged=0\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && near_to(shape.vertices[2].color[3], 1.0f) && near_to(shape.vertices[2].clear, 1.0f));
}

static void test_strip(void) {
    static jpt_sky_shape_t shape;
    jpt_sky_t s;
    jpt_sky_moment_t m = moment();
    const jpt_sky_layer_t* l = one("[LAYER6]\nType=STRIP\nTexture=horizon\nTopZ=15\nBottomZ=-3\nUVScale=3\nTopFog=0.2\nBottomFog=1\nUVScrollSpeed=0.5\nOffsetU=0.25\nColorTopG=51\nColorBottomA=102\n", &s);
    m.seconds = 3.0f;
    CHECK(jpt_sky_shape(&s, l, &m, 0.25f, &shape) && shape.vertex_count == 50 && shape.index_count == 24 * 6);
    CHECK(near_to(shape.vertices[0].position[0], 3500.0f) && near_to(shape.vertices[0].position[2], 1500.0f) && near_to(shape.vertices[1].position[2], -300.0f));
    CHECK(near_to(shape.vertices[0].u, 0.75f) && near_to(shape.vertices[48].u, 3.75f) && shape.vertices[0].v == 0.0f && shape.vertices[1].v == 1.0f);
    CHECK(near_to(shape.vertices[0].clear, 0.8f) && near_to(shape.vertices[1].clear, 0.0f) && near_to(shape.vertices[0].color[1], 0.2f) && near_to(shape.vertices[1].color[3], 0.4f));
    l = one("[LAYER1]\nType=STRIP\nTexture=h\nVerticalParallaxRangeZ=100\nVerticalParallaxAmplitude=2\n", &s);
    m.camera[2] = 50.0f;
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && near_to(shape.vertices[0].position[2], 900.0f));
}

static void test_passes(void) {
    static const char* const k_blends[] = { "OPAQUE", "ALPHA", "BURN", "2X_ALPHA", "2X_BURN" };
    for (int i = 0; i < 5; i++) {
        char text[96];
        jpt_sky_t s;
        jpt_sky_pass_t p;
        snprintf(text, sizeof text, "[LAYER1]\nType=DOME\nBlendMode=%s\n", k_blends[i]);
        jpt_sky_pass(one(text, &s), &p);
        CHECK(p.blend == (i == 1 || i == 3 ? JPT_SKY_ALPHA : i == 0 ? JPT_SKY_OPAQUE : JPT_SKY_BURN) && p.fog == (p.blend != JPT_SKY_BURN) && p.wrap_u && p.wrap_v && p.twice == (i >= 3));
    }
    {
        jpt_sky_t s;
        jpt_sky_pass_t p;
        jpt_sky_pass(one("[LAYER1]\nType=STRIP\nTexture=x\nBlendMode=ALPHA\nFogged=0\n", &s), &p);
        CHECK(p.fog && p.wrap_u && !p.wrap_v);
        jpt_sky_pass(one("[LAYER1]\nType=BODY\nTexture=x\nFogged=0\n", &s), &p);
        CHECK(!p.fog && !p.wrap_u && !p.wrap_v);
    }
}

/* Looking north, level, 90 degrees wide and 73.74 high. */
static jpt_sky_view_t north(void) {
    const float n = 1.0f / sqrtf(1.0f + 1.0f + 0.5625f);
    const jpt_sky_view_t v = { { -n, n, 0.75f * n }, { n, n, 0.75f * n }, { n, n, -0.75f * n }, { -n, n, -0.75f * n }, 10.0f, 20.0f, 650.0f, 500.0f };
    return v;
}

static void test_projection(void) {
    static jpt_sky_shape_t shape;
    static jpt_sky_screen_vertex_t out[8192];
    const jpt_sky_view_t view = north();
    const jpt_sky_moment_t m = moment();
    jpt_sky_t s;
    int n = 0;
    const jpt_sky_layer_t* l = one("[LAYER1]\nType=BODY\nTexture=sun\nFogged=0\nColorR=255\nColorG=128\nColorB=0\nColorA=64\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && (n = jpt_sky_project(&shape, &view, out, 8192)) == 6);
    CHECK(near_to(out[0].x, 10.0f + 320.0f * (1.0f - 250.0f / 3500.0f)) && near_to(out[0].y, 20.0f + 240.0f * (1.0f - 250.0f / 3500.0f / 0.75f)) && near_to(out[2].y, 260.0f) && out[0].rhw > 0.0f && out[0].rhw < 1.0f);
    CHECK(out[0].diffuse == 0x40FF8000u && out[0].specular == 0xFF000000u);
    l = one("[LAYER1]\nType=BODY\nTexture=sun\nAzimuth=180\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && jpt_sky_project(&shape, &view, out, 8192) == 0);

    l = one("[LAYER1]\nType=DOME\n", &s);
    CHECK(jpt_sky_shape(&s, l, &m, 1.0f, &shape) && (n = jpt_sky_project(&shape, &view, out, 8192)) > 0 && n % 3 == 0);
    {
        float area = 0.0f;
        for (int i = 0; i < n; i++) CHECK(out[i].x >= 10.0f - 7.0f && out[i].x <= 650.0f + 7.0f && out[i].y >= 20.0f - 6.0f && out[i].y <= 260.5f && out[i].rhw > 0.0f);
        for (int i = 0; i < n; i += 3) area += fabsf((out[i + 1].x - out[i].x) * (out[i + 2].y - out[i].y) - (out[i + 2].x - out[i].x) * (out[i + 1].y - out[i].y)) * 0.5f;
        CHECK(area > 640.0f * 240.0f * 0.97f && area < 660.0f * 250.0f);
    }
    CHECK(jpt_sky_project(&shape, &view, out, 7) == 6);
    {
        /* The engine's plane at the dome's own directions: where its corners are, so are ours. */
        jpt_sky_shape_t corners;
        const float* const at[4] = { view.top_left, view.top_right, view.bottom_right, view.bottom_left };
        const float x[4] = { 10.0f, 650.0f, 650.0f, 10.0f }, y[4] = { 20.0f, 20.0f, 500.0f, 500.0f };
        memset(&corners, 0, sizeof corners);
        corners.vertex_count = 4;
        corners.index_count = 6;
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 3; k++) corners.vertices[i].position[k] = at[i][k] * 50000.0f;
        corners.indices[1] = 1, corners.indices[2] = 2, corners.indices[4] = 2, corners.indices[5] = 3;
        CHECK(jpt_sky_project(&corners, &view, out, 8192) == 6);
        for (int i = 0; i < 3; i++) CHECK(near_to(out[i].x, x[i]) && near_to(out[i].y, y[i]));
        CHECK(near_to(out[5].x, x[3]) && near_to(out[5].y, y[3]));
    }
    {
        jpt_sky_view_t none;
        memset(&none, 0, sizeof none);
        CHECK(jpt_sky_project(&shape, &none, out, 8192) == 0);
    }
}

/* ---- the session ---- */

static char g_root[512];

static void put(const char* relative, const void* bytes, size_t size) {
    char path[700], folder[700];
    char* slash;
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", g_root, relative);
    snprintf(folder, sizeof folder, "%s", path);
    slash = strrchr(folder, '/');
    *slash = 0;
#ifdef _WIN32
    for (char* c = folder; *c; c++)
        if (*c == '/') *c = '\\';
#endif
    jpt_make_dirs(folder);
    f = fopen(path, "wb");
    if (f) {
        fwrite(bytes, 1, size, f);
        fclose(f);
    }
}

static void text(const char* relative, const char* content) { put(relative, content, strlen(content)); }

static void tga(const char* relative) {
    uint8_t bytes[18 + 2 * 1 * 4];
    memset(bytes, 0, sizeof bytes);
    bytes[2] = 2, bytes[12] = 2, bytes[14] = 1, bytes[16] = 32, bytes[17] = 0x28;
    for (size_t i = 18; i < sizeof bytes; i++) bytes[i] = 200;
    put(relative, bytes, sizeof bytes);
}

static int activate(const char* package, const char* scene) {
    jpt_custom_level_request_t r;
    char error[256];
    memset(&r, 0, sizeof r);
    snprintf(r.package, sizeof r.package, "%s", package);
    snprintf(r.scene, sizeof r.scene, "%s", scene);
    return jpt_custom_level_activate(g_root, &r, error, sizeof error);
}

static int ends_with(const char* s, const char* tail) {
    const size_t n = strlen(s), t = strlen(tail);
    if (n < t) return 0;
    for (size_t i = 0; i < t; i++)
        if (s[n - t + i] != tail[i] && !(tail[i] == '/' && s[n - t + i] == '\\')) return 0;
    return 1;
}

static int game_texture(const char* relative, char* out, size_t cap) {
    if (strcmp(relative, "texhw/backdrop/shared.bmp")) return 0;
    return snprintf(out, cap, "%s/game.bmp", g_root) > 0;
}

static unsigned g_emitted, g_freed, g_textured;
static int g_handle;
static void count_emit(void* user, const sky_draw_t* draw) {
    (void)user;
    g_emitted++;
    g_textured += draw->picture != NULL;
    if (draw->picture) *draw->gpu = &g_handle;
    CHECK(draw->count > 0 && draw->vertices && draw->pass);
}
static void count_free(void* gpu) {
    CHECK(gpu == &g_handle);
    g_freed++;
}

static void guest_float(uint32_t va, float v) { memcpy(g_guest + va, &v, 4); }

static void test_session(void) {
    char path[JPT_PATH_MAX];
    const uint32_t sky = 0x10;
    const float rect[4] = { 0.0f, 0.0f, 640.0f, 480.0f };
    text("custom_levels/Mod/M/data/one.scn", "scene");
    text("custom_levels/Mod/M/data/two.scn", "scene");
    text("custom_levels/Mod/M/data/one.sky", "[SKY]\nHeight=5000\n[LAYER1]\nType=DOME\n[LAYER2]\nType=DOME\nTexture=\"data\\texhw\\backdrop\\clouds\"\n[LAYER3]\nType=DOME\nTexture=texhw\\gone\n[LAYER4]\nType=STARS\n");
    text("custom_levels/Mod/M/retail.sky", "[LAYER1]\nType=DOME\n");
    tga("custom_levels/Mod/M/data/texhw/backdrop/clouds.tga");
    text("custom_levels/Mod/M/data/texhw/backdrop/clouds.bmp", "BM");
    text("custom_levels/Mod/M/data/texhw/backdrop/both.bmp", "BM");
    tga("custom_levels/Mod/M/data/texhw/backdrop/both.tga");
    text("custom_levels/Mod/M/texhw/backdrop/root.dds", "DDS ");
    text("custom_levels/Mod/runtime.map", "trespasser-runtime 1\nscene M/data/one.scn\nscene M/data/two.scn\nneed sky\n");
    text("custom_levels/None/one.scn", "scene");
    text("custom_levels/None/one.sky", "[LAYER1]\nType=DOME\n");
    text("custom_levels/None/runtime.map", "trespasser-runtime 1\nscene one.scn\n");
    sky_layers_install(count_free);

    CHECK(!sky_layers_ready() && !jpt_custom_level_sky(path, sizeof path) && !jpt_custom_level_sky_texture("texhw\\backdrop\\clouds", path, sizeof path));
    CHECK(activate("None", "one.scn") && !sky_layers_ready() && !jpt_custom_level_sky(path, sizeof path));
    jpt_custom_level_deactivate();
    CHECK(activate("Mod", "M/data/one.scn"));
    jpt_custom_level_set_game(game_texture);
    sky_layers_install(count_free);
    CHECK(jpt_custom_level_sky(path, sizeof path) && ends_with(path, "M/data/one.sky"));
    CHECK(jpt_custom_level_sky_texture("data\\texhw\\backdrop\\clouds", path, sizeof path) && ends_with(path, "clouds.tga"));
    CHECK(jpt_custom_level_sky_texture("TEXHW/backdrop/Both.dds", path, sizeof path) && ends_with(path, "both.tga"));
    CHECK(jpt_custom_level_sky_texture("texhw\\backdrop\\root", path, sizeof path) && ends_with(path, "M/texhw/backdrop/root.dds"));
    CHECK(jpt_custom_level_sky_texture("data\\texhw\\backdrop\\shared", path, sizeof path) && ends_with(path, "game.bmp"));
    CHECK(!jpt_custom_level_sky_texture("..\\..\\secret", path, sizeof path) && !jpt_custom_level_sky_texture("C:\\x", path, sizeof path) && !jpt_custom_level_sky_texture("", path, sizeof path));

    CHECK(sky_layers_ready() && S.count == 2 && S.sky.count == 4 && S.drawn[0] && S.drawn[1] && !S.drawn[2] && !S.drawn[3]);
    guest_float(sky + SKY_HEIGHT, 1000.0f);
    guest_float(sky + SKY_FOG_FAR, 1.0f);
    guest_float(sky + SKY_FOG_NEAR, 0.5f);
    guest_float(sky + SKY_TEXTURE_SCALE, 0.055f);
    guest_float(sky + SKY_WIND_U, 196608.0f);
    guest_float(sky + SKY_WIND_V, 117964.8f);
    {
        const jpt_sky_view_t v = north();
        const float* const corners[4] = { v.top_left, v.top_right, v.bottom_left, v.bottom_right };
        for (uint32_t c = 0; c < 4; c++)
            for (uint32_t i = 0; i < 3; i++) guest_float(VIEW_CORNERS + 12u * c + 4u * i, corners[c][i]);
    }
    sky_layers_begin(sky);
    {
        float height;
        memcpy(&height, g_guest + sky + SKY_HEIGHT, 4);
        CHECK(height < -1.0e8f && near_to(S.moment.wind[0], 3.0f) && near_to(S.moment.wind[1], 1.8f) && near_to(S.moment.height, 1000.0f) && near_to(S.view.bottom_right[0], -S.view.bottom_left[0]));
    }
    sky_layers_emit(rect, count_emit, NULL);
    sky_layers_end(sky);
    {
        float height;
        memcpy(&height, g_guest + sky + SKY_HEIGHT, 4);
        CHECK(height == 1000.0f);
    }
    CHECK(g_emitted == 2 && g_textured == 1 && !g_freed);

    jpt_custom_level_scene_loading("C:\\data\\two.scn");
    CHECK(!sky_layers_ready() && g_freed == 1);
    jpt_custom_level_scene_loading("other.scn");
    CHECK(!jpt_custom_level_sky(path, sizeof path) && !sky_layers_ready());
    jpt_custom_level_scene_loading("retail.scn");
    CHECK(jpt_custom_level_sky(path, sizeof path) && ends_with(path, "M/retail.sky") && sky_layers_ready() && S.count == 1);
    jpt_custom_level_scene_loading("one.scn");
    CHECK(sky_layers_ready() && S.count == 2);
    jpt_custom_level_deactivate();
    CHECK(!S.count && !sky_layers_ready() && !jpt_custom_level_sky(path, sizeof path));
}

static void test_rect(void) {
    uint8_t vertices[5 * 32];
    float rect[4];
    const float x[4] = { 5.0f, 645.0f, 645.0f, 5.0f }, y[4] = { 7.0f, 7.0f, 487.0f, 487.0f };
    memset(vertices, 0, sizeof vertices);
    for (int i = 0; i < 4; i++) {
        memcpy(vertices + i * 32, &x[i], 4);
        memcpy(vertices + i * 32 + 4, &y[i], 4);
    }
    CHECK(sky_layers_rect(vertices, 4, rect) && rect[0] == 5.0f && rect[1] == 7.0f && rect[2] == 645.0f && rect[3] == 487.0f);
    CHECK(!sky_layers_rect(vertices, 5, rect) && !sky_layers_rect(vertices, 3, rect));
    memset(vertices, 0, sizeof vertices);
    CHECK(!sky_layers_rect(vertices, 4, rect));
}

static int report(const char* file) {
    static const char* const k_blends[] = { "OPAQUE", "ALPHA", "BURN", "2X_ALPHA", "2X_BURN" };
    static char bytes[256 * 1024];
    static jpt_sky_shape_t shape;
    const jpt_sky_moment_t m = moment();
    FILE* f = fopen(file, "rb");
    size_t n = f ? fread(bytes, 1, sizeof bytes, f) : 0;
    jpt_sky_t s;
    if (f) fclose(f);
    if (!jpt_sky_parse(bytes, n, &s)) return printf("%s: no layers\n", file), 1;
    printf("%s: height %g%s\n", file, (double)s.height, s.has_height ? "" : " (the level's)");
    for (int i = 0; i < s.count; i++) {
        const jpt_sky_layer_t* l = &s.layers[i];
        const int made = jpt_sky_shape(&s, l, &m, 1.0f, &shape);
        printf("  layer %d %s %s %s: %s, %d vertices\n", l->number, jpt_sky_type_name(l->type), k_blends[l->blend], l->texture[0] ? l->texture : "-", jpt_sky_drawn(l) ? "drawn" : "NOT DRAWN", made ? shape.vertex_count : 0);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* temp = getenv("TEMP") ? getenv("TEMP") : "/tmp";
    if (argc > 1) {
        int bad = 0;
        for (int i = 1; i < argc; i++) bad |= report(argv[i]);
        return bad;
    }
    snprintf(g_root, sizeof g_root, "%s/jpt_sky_tests_%u", temp, (unsigned)((uintptr_t)&temp >> 4));
    test_parser();
    test_dome();
    test_body();
    test_strip();
    test_passes();
    test_projection();
    test_rect();
    test_session();
    printf("sky_tests: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
