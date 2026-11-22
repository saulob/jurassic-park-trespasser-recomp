#ifndef JPT_RENDERER_WORKLOAD_H
#define JPT_RENDERER_WORKLOAD_H

/*
 * One deterministic Direct3D 6 command stream for every backend: the same textures, render states, vertices, indices,
 * polygons and texture updates, issued through the backend's own table (jpt_win_backend_ops_t) the way the game's
 * device object does. Only the backend behind the table differs between renderer_benchmark_d3d11 and _vulkan.
 */
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/d3d_translate.h"
#include "ddraw/renderer_backend.h"

#define WL_W 640
#define WL_H 480
#define WL_TEXTURES 64
#define WL_TEX_SIZE 64
#define WL_DRAWS_PER_FRAME 4000
#define WL_POLYGONS_PER_FRAME 24
#define WL_FVF (D3DT_FVF_XYZRHW | D3DT_FVF_DIFFUSE | D3DT_FVF_SPECULAR | (1u << D3DT_FVF_TEXCOUNT_SHIFT))

typedef struct {
    float sx, sy, sz, rhw;
    DWORD color, specular;
    float tu, tv;
} wl_vertex_t;

static uint32_t g_wl_mutation; /* tests: changes the stream so a comparison of two backends has to notice */

typedef enum { WL_CAT_STATE, WL_CAT_TEXTURE_BIND, WL_CAT_DRAW, WL_CAT_TEXTURE_UPDATE, WL_CAT_POLYGON, WL_CAT_SCENE, WL_CAT_COUNT } wl_category_t;

static const char* const k_wl_category_names[WL_CAT_COUNT] = { "state processing", "texture binding", "draw translation",
                                                               "texture updates", "polygon preparation and draw",
                                                               "scene begin and end" };

typedef struct {
    const jpt_win_backend_ops_t* ops;
    dd_device_t dd;
    dd_surface_t target;
    d3d_device_t dev;
    dd_surface_t texture[WL_TEXTURES];
    BYTE* texture_bits[WL_TEXTURES];
    uint8_t poly_texels[64 * 64];
    uint16_t poly_clut[256];
    uint8_t bump_table[1024];
    uint16_t bump_texels[64 * 64];
    uint16_t bump_clut[512];
    double ms[WL_CAT_COUNT];
    unsigned long long stream_hash;
    unsigned long long draws, state_changes, texture_updates, polygons;
} wl_rig_t;

static double wl_now_ms(void) {
    static double scale;
    LARGE_INTEGER c;
    if (scale == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * scale;
}

static uint32_t wl_next(uint32_t* s) { return *s = *s * 1664525u + 1013904223u; }

static void wl_hash(wl_rig_t* g, unsigned long long v) { g->stream_hash = (g->stream_hash ^ v) * 1099511628211ull; }

static void wl_set_render(wl_rig_t* g, DWORD state, DWORD value) {
    double t0 = wl_now_ms();
    if (g->dev.render[state] != value) {
        if (d3dt_render_change_flushes((uint32_t)state, g->dev.render[state], value))
            g->ops->state_will_change(&g->dev, D3DR_CHANGE_RENDER, state, value);
        else
            g->ops->state_kept_batch(&g->dev);
        g->dev.render[state] = value;
        g->state_changes++;
    }
    g->ms[WL_CAT_STATE] += wl_now_ms() - t0;
    wl_hash(g, ((unsigned long long)state << 32) | value);
}

static void wl_set_stage(wl_rig_t* g, DWORD type, DWORD value) {
    double t0 = wl_now_ms();
    if (g->dev.stage[0][type] != value) {
        g->ops->state_will_change(&g->dev, D3DR_CHANGE_STAGE, type, 0);
        g->dev.stage[0][type] = value;
        g->state_changes++;
    }
    g->ms[WL_CAT_STATE] += wl_now_ms() - t0;
    wl_hash(g, ((unsigned long long)type << 40) | value);
}

static void wl_set_texture(wl_rig_t* g, int index) {
    double t0 = wl_now_ms();
    g->dev.texture = index < 0 ? NULL : &g->texture[index];
    g->ms[WL_CAT_TEXTURE_BIND] += wl_now_ms() - t0;
    wl_hash(g, 0x7e57000000000000ull | (unsigned long long)(index + 1));
}

static void wl_defaults(wl_rig_t* g) {
    memset(g->dev.render, 0, sizeof g->dev.render);
    memset(g->dev.stage, 0, sizeof g->dev.stage);
    g->dev.render[D3DRENDERSTATE_ZENABLE] = 1;
    g->dev.render[D3DRENDERSTATE_ZWRITEENABLE] = 1;
    g->dev.render[D3DRENDERSTATE_ZFUNC] = 7;
    g->dev.render[D3DRENDERSTATE_CULLMODE] = 1;
    g->dev.render[D3DRENDERSTATE_FILLMODE] = 3;
    g->dev.render[D3DRENDERSTATE_SHADEMODE] = 2;
    g->dev.render[D3DRENDERSTATE_TEXTUREMAPBLEND] = 2;
    g->dev.render[D3DRENDERSTATE_SRCBLEND] = 2;
    g->dev.render[D3DRENDERSTATE_DESTBLEND] = 1;
    g->dev.render[D3DRENDERSTATE_ALPHAFUNC] = 8;
    g->dev.stage[0][D3DTSS_MAGFILTER] = 2;
    g->dev.stage[0][D3DTSS_MINFILTER] = 2;
}

static void wl_textures(wl_rig_t* g) {
    uint32_t seed = 777u;
    for (int i = 0; i < WL_TEXTURES; i++) {
        dd_surface_t* s = &g->texture[i];
        int kind = i % 3;
        memset(s, 0, sizeof *s);
        s->width = s->height = WL_TEX_SIZE;
        s->caps = 0x1800;
        s->format.dwFlags = kind == 0 ? DDPF_RGB : (DDPF_RGB | DDPF_ALPHAPIXELS);
        s->format.dwRGBBitCount = 16;
        s->format.dwRBitMask = kind == 0 ? 0xF800 : kind == 1 ? 0x7C00 : 0x0F00;
        s->format.dwGBitMask = kind == 0 ? 0x07E0 : kind == 1 ? 0x03E0 : 0x00F0;
        s->format.dwBBitMask = kind == 0 ? 0x001F : kind == 1 ? 0x001F : 0x000F;
        s->format.dwRGBAlphaBitMask = kind == 0 ? 0 : kind == 1 ? 0x8000 : 0xF000;
        s->pitch = WL_TEX_SIZE * 2;
        g->texture_bits[i] = (BYTE*)calloc(1, (size_t)s->pitch * WL_TEX_SIZE);
        s->bits = g->texture_bits[i];
        s->device = &g->dd;
        for (int p = 0; p < WL_TEX_SIZE * WL_TEX_SIZE; p++) ((uint16_t*)s->bits)[p] = (uint16_t)(wl_next(&seed) >> 8) | 0x8000u;
    }
    for (int i = 0; i < 64 * 64; i++) {
        g->poly_texels[i] = (uint8_t)((i % 64) * 3 + i / 64);
        g->bump_texels[i] = (uint16_t)((i % 64) | (((i / 64) & 7) << 10));
    }
    for (int i = 0; i < 256; i++) g->poly_clut[i] = (uint16_t)(i * 257);
    for (int i = 0; i < 1024; i++) g->bump_table[i] = (uint8_t)((i >> 2) & 7);
    for (int i = 0; i < 512; i++) g->bump_clut[i] = (uint16_t)(i * 91);
}

static int wl_open(wl_rig_t* g, const jpt_win_backend_ops_t* ops) {
    memset(g, 0, sizeof *g);
    g->ops = ops;
    g->stream_hash = 14695981039346656037ull;
    for (int i = 0; i < 256; i++) g->dd.gamma.red[i] = g->dd.gamma.green[i] = g->dd.gamma.blue[i] = (WORD)(i * 257);
    g->target.device = &g->dd;
    g->target.width = WL_W;
    g->target.height = WL_H;
    g->target.pitch = WL_W * 2;
    g->target.bits = (BYTE*)calloc(1, (size_t)WL_W * WL_H * 2u);
    g->target.lock_rect.right = WL_W;
    g->target.lock_rect.bottom = WL_H;
    g->target.format.dwFlags = DDPF_RGB;
    g->target.format.dwRGBBitCount = 16;
    g->target.format.dwRBitMask = 0xF800;
    g->target.format.dwGBitMask = 0x07E0;
    g->target.format.dwBBitMask = 0x001F;
    g->dev.dd = &g->dd;
    g->dev.target = &g->target;
    wl_defaults(g);
    wl_textures(g);
    return SUCCEEDED(ops->attach(&g->dev));
}

static void wl_close(wl_rig_t* g) {
    g->ops->detach(&g->dev);
    for (int i = 0; i < WL_TEXTURES; i++) {
        g->ops->surface_freed(&g->texture[i]);
        free(g->texture_bits[i]);
    }
    free(g->target.bits);
}

static void wl_quad(wl_vertex_t* v, float x, float y, float w, float h, float z, DWORD color, DWORD specular) {
    v[0] = (wl_vertex_t){ x, y, z, 1.0f, color, specular, 0.0f, 0.0f };
    v[1] = (wl_vertex_t){ x + w, y, z, 1.0f, color, specular, 1.0f, 0.0f };
    v[2] = (wl_vertex_t){ x + w, y + h, z, 1.0f, color, specular, 1.0f, 1.0f };
    v[3] = (wl_vertex_t){ x, y + h, z, 1.0f, color, specular, 0.0f, 1.0f };
}

static void wl_draw(wl_rig_t* g, const wl_vertex_t* v, const WORD* indices, DWORD index_count) {
    double t0 = wl_now_ms();
    g->ops->draw(&g->dev, indices ? D3DT_PT_TRIANGLELIST : D3DT_PT_TRIANGLEFAN, WL_FVF, v, 4, indices, index_count);
    g->ms[WL_CAT_DRAW] += wl_now_ms() - t0;
    g->draws++;
    wl_hash(g, ((unsigned long long)(unsigned)v[0].sx << 20) ^ ((unsigned long long)(unsigned)v[0].sy << 8) ^ index_count);
}

static void wl_polygon(wl_rig_t* g, int bump, float x, float y) {
    gpu_texture_polygon_t p;
    double t0 = wl_now_ms();
    memset(&p, 0, sizeof p);
    p.count = 4;
    p.x[0] = x; p.y[0] = y;
    p.x[1] = x + 40; p.y[1] = y;
    p.x[2] = x + 40; p.y[2] = y + 30;
    p.x[3] = x; p.y[3] = y + 30;
    p.plane[0][1] = 1.0f;
    p.plane[1][2] = 1.0f;
    p.plane[2][0] = 1.0f;
    p.plane[3][0] = x;
    p.plane[3][1] = y;
    p.clamped = 1;
    p.umax = 63.0f;
    p.vmax = 63.0f;
    p.line = 64;
    p.umask = 63;
    p.tile_mask = 0xFFF;
    if (bump) {
        p.kind = GPU_POLYGON_BUMP;
        p.texels = (const uint8_t*)g->bump_texels;
        p.texel_bytes = sizeof g->bump_texels;
        p.clut = g->bump_clut;
        p.clut_entries = 512;
        p.bump_table = g->bump_table;
        p.light_theta = 3;
    } else {
        p.kind = GPU_POLYGON_TEXTURE;
        p.texels = g->poly_texels;
        p.texel_bytes = sizeof g->poly_texels;
        p.clut = g->poly_clut;
        p.clut_entries = 256;
    }
    if (g->ops->texture_polygon_prepare(&g->target, &p) == GPU_POLYGON_READY) g->ops->texture_polygon(&g->target, &p);
    g->ms[WL_CAT_POLYGON] += wl_now_ms() - t0;
    g->polygons++;
    wl_hash(g, 0x9017000000000000ull | (unsigned long long)(unsigned)(x * 4.0f) | ((unsigned long long)bump << 40));
}

/* One frame of the stream; seed makes frame n the same everywhere. */
static void wl_frame(wl_rig_t* g, unsigned frame) {
    static const WORD k_indices[6] = { 0, 1, 2, 0, 2, 3 };
    uint32_t seed = 0x5eed0000u + frame * 7919u + g_wl_mutation;
    D3DRECT full = { 0, 0, WL_W, WL_H };
    double t0 = wl_now_ms();
    wl_vertex_t v[4];
    g->dev.in_scene = 1;
    g->ops->begin_scene(&g->dev);
    g->ops->clear(&g->dev, 1, &full, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF102030u, 0.0f);
    g->ms[WL_CAT_SCENE] += wl_now_ms() - t0;
    for (int d = 0; d < WL_DRAWS_PER_FRAME; d++) {
        uint32_t r = wl_next(&seed);
        int texture = (int)((r >> 4) % WL_TEXTURES);
        int family = (int)((r >> 12) % 8);
        float x = (float)((r >> 16) % 600), y = (float)((r >> 8) % 440);
        wl_set_texture(g, family == 7 ? -1 : texture);
        wl_set_render(g, D3DRENDERSTATE_ALPHATESTENABLE, family == 1 || family == 5);
        wl_set_render(g, D3DRENDERSTATE_ALPHABLENDENABLE, family == 2);
        wl_set_render(g, D3DRENDERSTATE_SRCBLEND, 5);
        wl_set_render(g, D3DRENDERSTATE_DESTBLEND, 6);
        wl_set_render(g, D3DRENDERSTATE_FOGENABLE, family == 3);
        wl_set_render(g, D3DRENDERSTATE_ZWRITEENABLE, family != 2);
        wl_set_stage(g, D3DTSS_MAGFILTER, family == 4 ? 1 : 2);
        wl_set_stage(g, D3DTSS_MINFILTER, family == 6 ? 1 : 2);
        wl_quad(v, x, y, 6.0f + (float)(r % 24), 6.0f + (float)((r >> 5) % 24), 0.2f + (float)(d % 100) * 0.007f,
                family == 3 ? 0xFF808080u : 0xFFFFFFFFu, family == 3 ? 0xFF404040u : 0xFF000000u);
        wl_draw(g, v, family == 5 ? k_indices : NULL, family == 5 ? 6u : 0u);
        if (d % 400 == 0) {
            double t1 = wl_now_ms();
            int i = (int)((r >> 3) % WL_TEXTURES);
            RECT dirty = { 0, 0, WL_TEX_SIZE, 8 };
            g->ops->surface_cpu_access(&g->texture[i], 1, CPU_ACCESS_LOCK);
            for (int p = 0; p < WL_TEX_SIZE * 8; p++) ((uint16_t*)g->texture[i].bits)[p] ^= 0x0421u;
            g->ops->surface_cpu_written(&g->texture[i], &dirty);
            g->ms[WL_CAT_TEXTURE_UPDATE] += wl_now_ms() - t1;
            g->texture_updates++;
            wl_hash(g, 0x4000000000000000ull | (unsigned long long)i);
        }
        if (d % (WL_DRAWS_PER_FRAME / WL_POLYGONS_PER_FRAME) == 0)
            wl_polygon(g, (d / (WL_DRAWS_PER_FRAME / WL_POLYGONS_PER_FRAME)) & 1, (float)((r >> 9) % 560), (float)((r >> 2) % 420));
    }
    t0 = wl_now_ms();
    g->ops->end_scene(&g->dev);
    g->dev.in_scene = 0;
    g->ms[WL_CAT_SCENE] += wl_now_ms() - t0;
}

#endif
