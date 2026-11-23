/*
 * Runtime fixtures for the Windows Vulkan backend: the same Direct3D 6 sequence the game issues (device, render states,
 * surfaces used as textures, pre-transformed vertices, Flip) goes through jpt_vulkan_backend_ops into an offscreen
 * Vulkan target, and the picture and the counters are checked. No window is created.
 */
#include <stdio.h>
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/d3d_translate.h"
#include "ddraw/renderer_backend.h"
#include "ddraw/vk_win_policy.h"
#include "ddraw/vk_win_test.h"
#include "host.h"
#include "jpt_display.h"
#include "text_layer.h"
#include "texture_gpu_policy.h"
#include "vk_context.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        g_checks++;                                                                          \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond);                 \
            fprintf(stderr, __VA_ARGS__);                                                    \
            fputc('\n', stderr);                                                             \
            g_failures++;                                                                    \
        }                                                                                    \
    } while (0)

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}
HWND window_host_surface(HWND window) { return window; }
int window_host_frames_allowed(void) { return 0; }
void window_host_frame_commit(unsigned generation) { (void)generation; }
void window_host_frame_presented(void) {}
void video_hd_main_presented(void) {}
int video_hd_present(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma) {
    (void)window; (void)bits; (void)pitch; (void)source; (void)rgb565; (void)gamma;
    return 0;
}
const video_subtitle_image_t* video_subtitles_for_screen(int screen_w, int screen_h, int target_w, int target_h) {
    (void)screen_w; (void)screen_h; (void)target_w; (void)target_h;
    return NULL;
}
static int g_test_text_boxes_known;
static int g_test_ui_held;
static int g_test_loading_held;
static int g_test_dm_wanted;
static uint32_t g_test_dm_rgba_color;

int debug_menu_overlay_is_wanted(void) { return g_test_dm_wanted; }
int debug_menu_overlay_gpu(struct dmp_gpu_draw_s* out, int width, int height) {
    (void)out;
    (void)width;
    (void)height;
    return -1;
}

int debug_menu_overlay_rgba(uint8_t* rgba, int width, int height, overlay_tiles_t* tiles) {
    if (tiles && tiles->mask) memset(tiles->mask, 1, (size_t)tiles->tiles_w * (size_t)tiles->tiles_h);
    if (!g_test_dm_wanted || !rgba || width <= 0 || height <= 0) return 0;
    for (int i = 0; i < width * height; i++) {
        rgba[i * 4 + 0] = (uint8_t)(g_test_dm_rgba_color & 0xFFu);
        rgba[i * 4 + 1] = (uint8_t)((g_test_dm_rgba_color >> 8) & 0xFFu);
        rgba[i * 4 + 2] = (uint8_t)((g_test_dm_rgba_color >> 16) & 0xFFu);
        rgba[i * 4 + 3] = (uint8_t)((g_test_dm_rgba_color >> 24) & 0xFFu);
    }
    return 1;
}
void debug_menu_release(void) {}
void engine_limits_summary(unsigned long long hardware_draw_calls) { (void)hardware_draw_calls; }
void d3d_objects_counts(unsigned long* render_states, unsigned long* stage_states, unsigned long* failed_calls) {
    if (render_states) *render_states = 0;
    if (stage_states) *stage_states = 0;
    if (failed_calls) *failed_calls = 0;
}
float widescreen_tan_half_height(void) { return 0.57735f; }

void vkp_classify_access(cpu_access_t access, uint32_t lock_flags, vkp_access_t* out) {
    (void)lock_flags;
    out->caller = 0;
    out->reads = (access == CPU_ACCESS_GETDC && g_test_text_boxes_known) ? 0 : 1;
    out->inert = 0;
    out->text_overlay = (access == CPU_ACCESS_GETDC && g_test_text_boxes_known) ? 1 : 0;
}
uint32_t vkp_lock_caller(void) { return 0; }
int vkp_polygon_caller_ok(uint32_t caller) { (void)caller; return 1; }
int vkp_polygon_store_only(uint32_t caller) { (void)caller; return 0; }
void vkp_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch) { (void)box; (void)origin_bytes; (void)pitch; }
int vkp_text_boxes_known(void) { return g_test_text_boxes_known; }
int vkp_gameplay_now(void) { return 1; }
int vkp_ui_canvas_present_held(void) { return g_test_ui_held; }
int vkp_loading_hd_present_held(void) { return g_test_loading_held; }
int vkp_window_host_view_size(unsigned* w, unsigned* h) { (void)w; (void)h; return 0; }
float vkp_view_tan_half_height(void) { return 0.57735f; }
int vkp_guest_section_count(void) { return 0; }
const char* vkp_guest_section_name(int index) { (void)index; return ""; }
double vkp_guest_section_ms(int index) { (void)index; return 0.0; }
static dd_surface_t* g_polygon_surface;
dd_surface_t* dd_surface_from(void* iface) { return iface ? g_polygon_surface : NULL; }
int dd_format_bytes(const DDPIXELFORMAT* pf) { return pf ? (int)(pf->dwRGBBitCount / 8) : 2; }

#define W 64
#define H 64
#define FVF_TL (D3DT_FVF_XYZRHW | D3DT_FVF_DIFFUSE | D3DT_FVF_SPECULAR | (1u << D3DT_FVF_TEXCOUNT_SHIFT))

typedef struct {
    float sx, sy, sz, rhw;
    DWORD color, specular;
    float tu, tv;
} tl_vertex_t;

typedef struct {
    dd_device_t dd;
    dd_surface_t target;
    d3d_device_t dev;
    uint8_t rgba[W * H * 4];
} rig_t;

static const jpt_win_backend_ops_t* ops(void) { return &jpt_vulkan_backend_ops; }

static void set_defaults(d3d_device_t* dev) {
    memset(dev->render, 0, sizeof dev->render);
    memset(dev->stage, 0, sizeof dev->stage);
    dev->render[D3DRENDERSTATE_ZENABLE] = 1;
    dev->render[D3DRENDERSTATE_ZWRITEENABLE] = 1;
    dev->render[D3DRENDERSTATE_ZFUNC] = 7;
    dev->render[D3DRENDERSTATE_CULLMODE] = 1;
    dev->render[D3DRENDERSTATE_FILLMODE] = 3;
    dev->render[D3DRENDERSTATE_SHADEMODE] = 2;
    dev->render[D3DRENDERSTATE_TEXTUREMAPBLEND] = 2;
    dev->render[D3DRENDERSTATE_SRCBLEND] = 2;
    dev->render[D3DRENDERSTATE_DESTBLEND] = 1;
    dev->render[D3DRENDERSTATE_ALPHAFUNC] = 8;
    dev->stage[0][D3DTSS_MAGFILTER] = 1;
    dev->stage[0][D3DTSS_MINFILTER] = 1;
}

static int rig_open(rig_t* g) {
    HRESULT hr;
    memset(g, 0, sizeof *g);
    for (int i = 0; i < 256; i++) g->dd.gamma.red[i] = g->dd.gamma.green[i] = g->dd.gamma.blue[i] = (WORD)(i * 257);
    g->target.device = &g->dd;
    g->target.width = W;
    g->target.height = H;
    g->target.pitch = W * 2;
    g->target.bits = (BYTE*)calloc(1, (size_t)W * H * 2u);
    g->target.lock_rect.right = W;
    g->target.lock_rect.bottom = H;
    g_polygon_surface = &g->target;
    g->target.format.dwFlags = DDPF_RGB;
    g->target.format.dwRGBBitCount = 16;
    g->target.format.dwRBitMask = 0xF800;
    g->target.format.dwGBitMask = 0x07E0;
    g->target.format.dwBBitMask = 0x001F;
    g->dev.dd = &g->dd;
    g->dev.target = &g->target;
    set_defaults(&g->dev);
    hr = ops()->attach(&g->dev);
    return SUCCEEDED(hr);
}

static void rig_close(rig_t* g) {
    ops()->detach(&g->dev);
    free(g->target.bits);
}

static void begin_frame(rig_t* g, DWORD argb) {
    D3DRECT full = { 0, 0, W, H };
    g->dev.in_scene = 1;
    ops()->begin_scene(&g->dev);
    ops()->clear(&g->dev, 1, &full, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, argb, 0.0f);
}

static int end_frame(rig_t* g) {
    uint32_t rw = 0, rh = 0;
    ops()->end_scene(&g->dev);
    g->dev.in_scene = 0;
    if (!ops()->flip(&g->target, 0)) return 0;
    return vk_win_test_readback(g->rgba, sizeof g->rgba, &rw, &rh) && rw == W && rh == H;
}

static const uint8_t* px(const rig_t* g, int x, int y) { return g->rgba + ((size_t)y * W + (size_t)x) * 4u; }

static int near_rgb(const uint8_t* p, int r, int gr, int b, int tol) {
    return abs((int)p[0] - r) <= tol && abs((int)p[1] - gr) <= tol && abs((int)p[2] - b) <= tol;
}

static HRESULT draw_quad(rig_t* g, float x0, float y0, float x1, float y1, float z, DWORD diffuse, DWORD specular) {
    tl_vertex_t v[4] = {
        { x0, y0, z, 1.0f, diffuse, specular, 0.0f, 0.0f },
        { x1, y0, z, 1.0f, diffuse, specular, 1.0f, 0.0f },
        { x1, y1, z, 1.0f, diffuse, specular, 1.0f, 1.0f },
        { x0, y1, z, 1.0f, diffuse, specular, 0.0f, 1.0f },
    };
    return ops()->draw(&g->dev, D3DT_PT_TRIANGLEFAN, FVF_TL, v, 4, NULL, 0);
}

typedef struct {
    dd_surface_t s;
    BYTE* bits;
} tex_t;

static void tex_init(tex_t* t, DWORD w, DWORD h, DWORD flags, DWORD bits, DWORD rm, DWORD gm, DWORD bm, DWORD am) {
    memset(t, 0, sizeof *t);
    t->s.width = w;
    t->s.height = h;
    t->s.caps = 0x1800;
    t->s.format.dwFlags = flags;
    t->s.format.dwRGBBitCount = bits;
    t->s.format.dwRBitMask = rm;
    t->s.format.dwGBitMask = gm;
    t->s.format.dwBBitMask = bm;
    t->s.format.dwRGBAlphaBitMask = am;
    t->s.pitch = (LONG)(w * (bits / 8));
    t->bits = (BYTE*)calloc(1, (size_t)t->s.pitch * h);
    t->s.bits = t->bits;
}

static void tex_free(tex_t* t) {
    ops()->surface_freed(&t->s);
    free(t->bits);
}

static void tex_fill16(tex_t* t, const uint16_t* texels) {
    memcpy(t->bits, texels, (size_t)t->s.pitch * t->s.height);
}

static void stats(jpt_vk_renderer_stats_t* st) { vk_win_test_stats(st); }

static void test_basic_textures(rig_t* g) {
    tex_t green, foliage, glass, opaque4444;
    uint16_t green_px[4] = { 0x07E0, 0x07E0, 0x07E0, 0x07E0 };
    uint16_t leaf[16], glass_px[4], x4444[4] = { 0x00F0, 0x00F0, 0x00F0, 0x00F0 };
    jpt_vk_renderer_stats_t before, after;

    tex_init(&green, 2, 2, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
    tex_fill16(&green, green_px);
    g->dev.texture = &green.s;
    stats(&before);
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u)), "RGB565 draw accepted");
    CHECK(end_frame(g), "RGB565 frame presented and read back");
    CHECK(near_rgb(px(g, 32, 32), 0, 255, 0, 4), "RGB565 texture reaches the picture as green, not white (got %u,%u,%u)",
          px(g, 32, 32)[0], px(g, 32, 32)[1], px(g, 32, 32)[2]);
    stats(&after);
    CHECK(after.textures_created > before.textures_created, "RGB565 texture created on the GPU (%llu)",
          (unsigned long long)after.textures_created);
    CHECK(after.pipelines_created > before.pipelines_created, "a pipeline was created for the first draw");
    CHECK(after.draws > before.draws && after.draw_calls_recorded > before.draw_calls_recorded, "the draw reached a Vulkan draw call");
    CHECK(after.textured_draws > before.textured_draws, "the draw counted as textured");

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) leaf[y * 4 + x] = x < 2 ? 0x0000 : 0xFC00;
    tex_init(&foliage, 4, 4, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000);
    tex_fill16(&foliage, leaf);
    g->dev.texture = &foliage.s;
    g->dev.render[D3DRENDERSTATE_ALPHATESTENABLE] = 1;
    g->dev.render[D3DRENDERSTATE_ALPHAFUNC] = 5;
    g->dev.render[D3DRENDERSTATE_ALPHAREF] = 0x80;
    begin_frame(g, 0xFF0000FFu);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "ARGB1555 frame");
    CHECK(near_rgb(px(g, 8, 32), 0, 0, 255, 4), "ARGB1555 alpha 0 texels are discarded by the alpha test (got %u,%u,%u)",
          px(g, 8, 32)[0], px(g, 8, 32)[1], px(g, 8, 32)[2]);
    CHECK(near_rgb(px(g, 56, 32), 255, 0, 0, 4), "ARGB1555 alpha 1 texels draw red (got %u,%u,%u)", px(g, 56, 32)[0],
          px(g, 56, 32)[1], px(g, 56, 32)[2]);
    g->dev.render[D3DRENDERSTATE_ALPHATESTENABLE] = 0;

    for (int i = 0; i < 4; i++) glass_px[i] = 0x8F00;
    tex_init(&glass, 2, 2, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    tex_fill16(&glass, glass_px);
    g->dev.texture = &glass.s;
    g->dev.render[D3DRENDERSTATE_ALPHABLENDENABLE] = 1;
    g->dev.render[D3DRENDERSTATE_SRCBLEND] = 5;
    g->dev.render[D3DRENDERSTATE_DESTBLEND] = 6;
    begin_frame(g, 0xFF0000FFu);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "ARGB4444 frame");
    CHECK(near_rgb(px(g, 32, 32), 136, 0, 119, 8), "ARGB4444 translucent texture blends over blue (got %u,%u,%u)",
          px(g, 32, 32)[0], px(g, 32, 32)[1], px(g, 32, 32)[2]);
    g->dev.render[D3DRENDERSTATE_ALPHABLENDENABLE] = 0;

    tex_init(&opaque4444, 2, 2, DDPF_RGB, 16, 0x0F00, 0x00F0, 0x000F, 0);
    tex_fill16(&opaque4444, x4444);
    g->dev.texture = &opaque4444.s;
    begin_frame(g, 0xFF0000FFu);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "XRGB4444 frame");
    CHECK(near_rgb(px(g, 32, 32), 0, 255, 0, 4), "XRGB4444 has no alpha: opaque green (got %u,%u,%u)", px(g, 32, 32)[0],
          px(g, 32, 32)[1], px(g, 32, 32)[2]);

    g->dev.texture = NULL;
    tex_free(&green);
    tex_free(&foliage);
    tex_free(&glass);
    tex_free(&opaque4444);
}

static void test_blend_modes_match_reference(rig_t* g) {
    tex_t t;
    uint16_t texel = 0x8F80;
    uint16_t texels[4];
    const uint32_t diffuse = 0xC0A0E0C0u;
    for (int i = 0; i < 4; i++) texels[i] = texel;
    tex_init(&t, 2, 2, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    tex_fill16(&t, texels);
    g->dev.texture = &t.s;
    for (DWORD mode = 1; mode <= 8; mode++) {
        uint32_t want, got;
        g->dev.render[D3DRENDERSTATE_TEXTUREMAPBLEND] = mode;
        begin_frame(g, 0xFF000000u);
        draw_quad(g, 0, 0, W, H, 0.5f, diffuse, 0xFF000000u);
        CHECK(end_frame(g), "blend mode %lu frame", (unsigned long)mode);
        {
            uint32_t a4 = (texel >> 12) & 15u, r4 = (texel >> 8) & 15u, g4 = (texel >> 4) & 15u, b4 = texel & 15u;
            uint32_t texel_bgra = ((a4 * 17u) << 24) | ((r4 * 17u) << 16) | ((g4 * 17u) << 8) | (b4 * 17u);
            want = d3dt_texture_blend(mode, texel_bgra, diffuse, 1);
        }
        got = ((uint32_t)px(g, 32, 32)[0] << 16) | ((uint32_t)px(g, 32, 32)[1] << 8) | px(g, 32, 32)[2];
        CHECK(abs((int)((got >> 16) & 255) - (int)((want >> 16) & 255)) <= 6 &&
                  abs((int)((got >> 8) & 255) - (int)((want >> 8) & 255)) <= 6 &&
                  abs((int)(got & 255) - (int)(want & 255)) <= 6,
              "texture blend %lu matches the Direct3D reference: want %06X got %06X", (unsigned long)mode, want & 0xFFFFFFu,
              got);
    }
    g->dev.render[D3DRENDERSTATE_TEXTUREMAPBLEND] = 2;
    g->dev.texture = NULL;
    tex_free(&t);
}

static void test_geometry(rig_t* g) {
    tl_vertex_t verts[4] = {
        { 8, 8, 0.5f, 1.0f, 0xFF00FF00u, 0xFF000000u, 0, 0 },
        { 56, 8, 0.5f, 1.0f, 0xFF00FF00u, 0xFF000000u, 1, 0 },
        { 56, 56, 0.5f, 1.0f, 0xFF00FF00u, 0xFF000000u, 1, 1 },
        { 8, 56, 0.5f, 1.0f, 0xFF00FF00u, 0xFF000000u, 0, 1 },
    };
    WORD idx[6] = { 0, 1, 2, 0, 2, 3 };
    WORD bad[3] = { 0, 1, 9 };
    jpt_vk_renderer_stats_t before, after;
    vk_win_test_bridge_stats_t b0, b1;
    tl_vertex_t tri[3] = {
        { 10, 10, 0.5f, 1.0f, 0xFFFF0000u, 0xFF000000u, 0, 0 },
        { 54, 10, 0.5f, 1.0f, 0xFFFF0000u, 0xFF000000u, 1, 0 },
        { 32, 54, 0.5f, 1.0f, 0xFFFF0000u, 0xFF000000u, 0, 1 },
    };

    begin_frame(g, 0xFF000000u);
    stats(&before);
    CHECK(SUCCEEDED(ops()->draw(&g->dev, D3DT_PT_TRIANGLELIST, FVF_TL, verts, 4, idx, 6)), "indexed list accepted");
    vk_win_test_bridge_stats(&b0);
    CHECK(ops()->draw(&g->dev, D3DT_PT_TRIANGLELIST, FVF_TL, verts, 4, bad, 3) == DDERR_INVALIDPARAMS,
          "an index past the vertices is refused like the Direct3D 11 backend");
    vk_win_test_bridge_stats(&b1);
    CHECK(b1.bridge_skipped == b0.bridge_skipped + 1, "the refused draw is counted as skipped");
    CHECK(end_frame(g), "indexed frame");
    CHECK(near_rgb(px(g, 32, 32), 0, 255, 0, 4) && near_rgb(px(g, 4, 4), 0, 0, 0, 2), "indexed quad covers its square only");
    stats(&after);
    CHECK(after.indexed_draws == before.indexed_draws + 1, "indexed draw counted");

    g->dev.render[D3DRENDERSTATE_CULLMODE] = 2;
    begin_frame(g, 0xFF000000u);
    ops()->draw(&g->dev, D3DT_PT_TRIANGLELIST, FVF_TL, tri, 3, NULL, 0);
    CHECK(end_frame(g), "cull CW frame");
    CHECK(near_rgb(px(g, 32, 24), 0, 0, 0, 2), "D3DCULL_CW removes a clockwise triangle (got %u,%u,%u)", px(g, 32, 24)[0],
          px(g, 32, 24)[1], px(g, 32, 24)[2]);
    g->dev.render[D3DRENDERSTATE_CULLMODE] = 3;
    begin_frame(g, 0xFF000000u);
    ops()->draw(&g->dev, D3DT_PT_TRIANGLELIST, FVF_TL, tri, 3, NULL, 0);
    CHECK(end_frame(g), "cull CCW frame");
    CHECK(near_rgb(px(g, 32, 24), 255, 0, 0, 4), "D3DCULL_CCW keeps a clockwise triangle");
    g->dev.render[D3DRENDERSTATE_CULLMODE] = 1;
    begin_frame(g, 0xFF000000u);
    ops()->draw(&g->dev, D3DT_PT_TRIANGLELIST, FVF_TL, tri, 3, NULL, 0);
    CHECK(end_frame(g), "cull none frame");
    CHECK(near_rgb(px(g, 32, 24), 255, 0, 0, 4), "D3DCULL_NONE keeps it");
    stats(&after);
    CHECK(after.cull_draws[1] > 0 && after.cull_draws[2] > 0 && after.cull_draws[3] > 0, "every cull state is counted");

    for (int order = 0; order < 2; order++) {
        begin_frame(g, 0xFF000000u);
        if (order == 0) {
            draw_quad(g, 8, 8, 56, 56, 0.3f, 0xFF00FF00u, 0xFF000000u);
            draw_quad(g, 8, 8, 56, 56, 0.8f, 0xFFFF0000u, 0xFF000000u);
        } else {
            draw_quad(g, 8, 8, 56, 56, 0.8f, 0xFFFF0000u, 0xFF000000u);
            draw_quad(g, 8, 8, 56, 56, 0.3f, 0xFF00FF00u, 0xFF000000u);
        }
        CHECK(end_frame(g), "depth overlap frame %d", order);
        CHECK(near_rgb(px(g, 32, 32), 255, 0, 0, 4), "the nearer quad wins whichever is drawn first (order %d)", order);
    }
}

static void test_viewport_clips_without_scaling(rig_t* g) {
    d3d_viewport_t vp;
    memset(&vp, 0, sizeof vp);
    vp.data_set = 1;
    vp.data.dwSize = sizeof vp.data;
    vp.data.dwX = 16;
    vp.data.dwY = 16;
    vp.data.dwWidth = 32;
    vp.data.dwHeight = 32;
    g->dev.current = &vp;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.4f, 0xFFFF0000u, 0xFF000000u);
    draw_quad(g, 20, 20, 28, 28, 0.6f, 0xFF00FF00u, 0xFF000000u);
    CHECK(end_frame(g), "viewport frame");
    CHECK(near_rgb(px(g, 40, 40), 255, 0, 0, 4), "inside the game's viewport the quad is drawn");
    CHECK(near_rgb(px(g, 8, 8), 0, 0, 0, 2) && near_rgb(px(g, 56, 56), 0, 0, 0, 2), "outside the viewport nothing is drawn");
    CHECK(near_rgb(px(g, 24, 24), 0, 255, 0, 4),
          "vertices keep their surface coordinates inside a smaller viewport (got %u,%u,%u)", px(g, 24, 24)[0],
          px(g, 24, 24)[1], px(g, 24, 24)[2]);
    g->dev.current = NULL;
}

static void test_color_key(rig_t* g) {
    tex_t t;
    uint16_t texels[16];
    for (int i = 0; i < 16; i++) texels[i] = (i & 3) < 2 ? 0x0000 : 0xF800;
    tex_init(&t, 4, 4, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
    tex_fill16(&t, texels);
    t.s.has_src_key = 1;
    t.s.src_key.dwColorSpaceLowValue = 0;
    t.s.src_key.dwColorSpaceHighValue = 0;
    g->dev.texture = &t.s;
    g->dev.render[D3DRENDERSTATE_COLORKEYENABLE] = 1;
    begin_frame(g, 0xFF0000FFu);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "colour-key frame");
    CHECK(near_rgb(px(g, 8, 32), 0, 0, 255, 4), "keyed texels are not drawn (got %u,%u,%u)", px(g, 8, 32)[0], px(g, 8, 32)[1],
          px(g, 8, 32)[2]);
    CHECK(near_rgb(px(g, 56, 32), 255, 0, 0, 4), "unkeyed texels are drawn");
    g->dev.render[D3DRENDERSTATE_COLORKEYENABLE] = 0;
    begin_frame(g, 0xFF0000FFu);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "colour-key off frame");
    CHECK(near_rgb(px(g, 8, 32), 0, 0, 0, 4), "with the key disabled the keyed texel keeps its colour");
    g->dev.texture = NULL;
    tex_free(&t);
}

static void test_dynamic_vertices(rig_t* g) {
    for (int step = 0; step < 3; step++) {
        float left = 4.0f + (float)step * 16.0f;
        begin_frame(g, 0xFF000000u);
        draw_quad(g, left, 8, left + 12.0f, 56, 0.5f, 0xFFFFFF00u, 0xFF000000u);
        CHECK(end_frame(g), "dynamic mesh frame %d", step);
        CHECK(near_rgb(px(g, (int)left + 6, 32), 255, 255, 0, 4), "the mesh moved to x=%d", (int)left + 6);
        CHECK(near_rgb(px(g, step == 0 ? 40 : 6, 32), 0, 0, 0, 2), "the previous position is clear of the mesh");
    }
}

static void test_fog_and_specular(rig_t* g) {
    g->dev.render[D3DRENDERSTATE_FOGENABLE] = 1;
    g->dev.render[D3DRENDERSTATE_FOGCOLOR] = 0x00808080u;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFF0000u, 0x00000000u);
    CHECK(end_frame(g), "fog frame");
    CHECK(near_rgb(px(g, 32, 32), 128, 128, 128, 6), "vertex fog alpha 0 gives the fog colour");
    g->dev.render[D3DRENDERSTATE_FOGENABLE] = 0;
    g->dev.render[D3DRENDERSTATE_SPECULARENABLE] = 1;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFF400000u, 0xFF004000u);
    CHECK(end_frame(g), "specular frame");
    CHECK(near_rgb(px(g, 32, 32), 64, 64, 0, 6), "specular is added to the diffuse colour (got %u,%u,%u)", px(g, 32, 32)[0],
          px(g, 32, 32)[1], px(g, 32, 32)[2]);
    g->dev.render[D3DRENDERSTATE_SPECULARENABLE] = 0;
}

static void test_dirty_tracking(rig_t* g) {
    tex_t t, src;
    uint16_t green[16], red[16];
    jpt_vk_renderer_stats_t a, b;
    RECT corner = { 2, 2, 4, 4 };

    for (int i = 0; i < 16; i++) {
        green[i] = 0x07E0;
        red[i] = 0xF800;
    }
    tex_init(&t, 4, 4, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
    tex_init(&src, 4, 4, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
    tex_fill16(&t, green);
    g->dev.texture = &t.s;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "dirty-tracking frame 1");
    CHECK(near_rgb(px(g, 32, 32), 0, 255, 0, 4), "first upload is green");

    stats(&a);
    memcpy(t.bits + 2 * t.s.pitch + 4, red, 4);
    memcpy(t.bits + 3 * t.s.pitch + 4, red, 4);
    ops()->surface_cpu_written(&t.s, &corner);
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "dirty-tracking frame 2");
    stats(&b);
    CHECK(b.texture_updates == a.texture_updates + 1, "a CPU write is one texture update (%llu)",
          (unsigned long long)(b.texture_updates - a.texture_updates));
    CHECK(b.texture_upload_bytes - a.texture_upload_bytes == (16u + 4u + 1u) * 4u, "a texture with mipmaps is uploaded whole with its chain (%llu bytes)",
          (unsigned long long)(b.texture_upload_bytes - a.texture_upload_bytes));
    CHECK(near_rgb(px(g, 56, 56), 255, 0, 0, 4) && near_rgb(px(g, 8, 8), 0, 255, 0, 4),
          "the written corner is red, the rest stays green");

    tex_fill16(&src, red);
    ops()->texture_loaded(&t.s, &src.s);
    tex_fill16(&t, red);
    ops()->surface_cpu_written(&t.s, NULL);
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "dirty-tracking frame 3");
    CHECK(near_rgb(px(g, 8, 8), 255, 0, 0, 4), "IDirect3DTexture2::Load's copy reaches the GPU texture");

    g->dev.texture = NULL;
    tex_free(&t);
    tex_free(&src);
}

static void test_palette_and_unavailable(rig_t* g) {
    tex_t t, none;
    dd_palette_t pal;
    BYTE idx[4] = { 1, 1, 1, 1 };
    vk_win_test_bridge_stats_t before, after;
    jpt_vk_renderer_stats_t sb, sa;

    memset(&pal, 0, sizeof pal);
    pal.entries[1].peRed = 255;
    tex_init(&t, 2, 2, DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0);
    memcpy(t.bits, idx, sizeof idx);
    t.s.palette = &pal;
    g->dev.texture = &t.s;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "palette frame 1");
    CHECK(near_rgb(px(g, 32, 32), 255, 0, 0, 4), "palette entry 1 is red");
    pal.entries[1].peRed = 0;
    pal.entries[1].peGreen = 255;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
    CHECK(end_frame(g), "palette frame 2");
    CHECK(near_rgb(px(g, 32, 32), 0, 255, 0, 4), "a palette change refreshes the texture without a CPU write");
    tex_free(&t);

    tex_init(&none, 2, 2, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
    none.s.bits = NULL;
    g->dev.texture = &none.s;
    vk_win_test_bridge_stats(&before);
    stats(&sb);
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFFFFFFFFu, 0xFF000000u)), "a draw with an unusable texture returns");
    CHECK(end_frame(g), "unavailable-texture frame");
    vk_win_test_bridge_stats(&after);
    stats(&sa);
    CHECK(after.texture_skips == before.texture_skips + 1, "the draw is counted as skipped for its texture");
    CHECK(sa.draws == sb.draws, "nothing was drawn");
    CHECK(near_rgb(px(g, 32, 32), 0, 0, 0, 2), "no silent white placeholder (got %u,%u,%u)", px(g, 32, 32)[0],
          px(g, 32, 32)[1], px(g, 32, 32)[2]);
    g->dev.texture = NULL;
    free(none.bits);
}

static void test_batching(rig_t* g) {
    jpt_vk_renderer_stats_t a, b;
    stats(&a);
    begin_frame(g, 0xFF000000u);
    for (int i = 0; i < 100; i++) {
        float x = (float)(i % 10) * 6.0f + 2.0f, y = (float)(i / 10) * 6.0f + 2.0f;
        draw_quad(g, x, y, x + 4.0f, y + 4.0f, 0.5f, (i & 1) ? 0xFFFF0000u : 0xFF00FF00u, 0xFF000000u);
    }
    CHECK(end_frame(g), "batched frame");
    stats(&b);
    CHECK(b.draws - a.draws == 100, "100 draws submitted (%llu)", (unsigned long long)(b.draws - a.draws));
    CHECK(b.draw_calls_recorded - a.draw_calls_recorded <= 3, "100 same-state draws record %llu Vulkan draw calls",
          (unsigned long long)(b.draw_calls_recorded - a.draw_calls_recorded));
    CHECK(near_rgb(px(g, 4, 4), 0, 255, 0, 3) && near_rgb(px(g, 10, 4), 255, 0, 0, 3), "merged draws keep their own colours");
}


static void write_box(rig_t* g, int x0, int y0, int x1, int y1, uint16_t value) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) ((uint16_t*)(g->target.bits + (size_t)y * g->target.pitch))[x] = value;
}

static void software_polygon(rig_t* g, int x0, int y0, int x1, int y1, int reads, void (*paint)(rig_t*)) {
    long box[4] = { x0, y0, x1, y1 };
    RECT rect = { x0, y0, x1, y1 };
    ops()->cpu_polygon(&g->target, box, 0, reads);
    ops()->surface_cpu_access(&g->target, 1, CPU_ACCESS_LOCK);
    g->target.locked = 1;
    g->target.lock_rect.left = 0;
    g->target.lock_rect.top = 0;
    g->target.lock_rect.right = W;
    g->target.lock_rect.bottom = H;
    paint(g);
    g->target.locked = 0;
    ops()->surface_cpu_written(&g->target, &rect);
    ops()->cpu_polygon_done();
}

static void paint_partial_red(rig_t* g) { write_box(g, 24, 24, 32, 32, 0xF800); }

static void paint_blended(rig_t* g) {
    uint16_t seen = ((const uint16_t*)(g->target.bits + 22 * (size_t)g->target.pitch))[22];
    CHECK(seen == 0x07E0, "a polygon that reads the target sees the GPU picture (0x%04X)", seen);
    write_box(g, 22, 22, 26, 26, 0x001F);
}

static void test_software_polygons(rig_t* g) {
    vk_win_test_bridge_stats_t b0, b1;
    uint16_t sentinel_ok;

    vk_win_test_bridge_stats(&b0);
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, 32, H, 0.5f, 0xFF00FF00u, 0xFF000000u);
    software_polygon(g, 20, 20, 36, 36, 0, paint_partial_red);
    draw_quad(g, 60, 60, 62, 62, 0.5f, 0xFF0000FFu, 0xFF000000u);
    CHECK(end_frame(g), "software polygon frame");
    vk_win_test_bridge_stats(&b1);
    CHECK(near_rgb(px(g, 28, 28), 255, 0, 0, 8), "pixels the game's rasteriser wrote reach the Vulkan picture (got %u,%u,%u)",
          px(g, 28, 28)[0], px(g, 28, 28)[1], px(g, 28, 28)[2]);
    CHECK(near_rgb(px(g, 22, 22), 0, 255, 0, 4), "pixels it did not write keep the GPU picture (got %u,%u,%u)",
          px(g, 22, 22)[0], px(g, 22, 22)[1], px(g, 22, 22)[2]);
    CHECK(near_rgb(px(g, 34, 34), 0, 0, 0, 4), "outside the GPU geometry and the written pixels the target stays clear");
    CHECK(b1.composites > b0.composites && b1.cpu_polygons > b0.cpu_polygons, "the polygon was composited (%llu composites)",
          b1.composites - b0.composites);
    (void)sentinel_ok;

    vk_win_test_bridge_stats(&b0);
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, 32, H, 0.5f, 0xFF00FF00u, 0xFF000000u);
    software_polygon(g, 20, 20, 36, 36, 1, paint_blended);
    draw_quad(g, 40, 8, 56, 24, 0.5f, 0xFFFFFF00u, 0xFF000000u);
    CHECK(end_frame(g), "reading polygon frame");
    vk_win_test_bridge_stats(&b1);
    CHECK(b1.readbacks > b0.readbacks && b1.readback_failures == b0.readback_failures,
          "the polygon's box was read back from the GPU (%llu)", b1.readbacks - b0.readbacks);
    CHECK(near_rgb(px(g, 24, 24), 0, 0, 255, 8), "the CPU-blended pixels are drawn (got %u,%u,%u)", px(g, 24, 24)[0],
          px(g, 24, 24)[1], px(g, 24, 24)[2]);
    CHECK(near_rgb(px(g, 10, 40), 0, 255, 0, 4), "GPU geometry drawn before the readback survived it");
    CHECK(near_rgb(px(g, 48, 16), 255, 255, 0, 4), "GPU geometry drawn after the readback is there");
}

static void lock_read(rig_t* g, int x0, int y0, int x1, int y1) {
    g->target.lock_rect.left = x0;
    g->target.lock_rect.top = y0;
    g->target.lock_rect.right = x1;
    g->target.lock_rect.bottom = y1;
    ops()->surface_cpu_access(&g->target, 0, CPU_ACCESS_LOCK);
}

static void test_regional_readbacks(rig_t* g) {
    vk_win_test_bridge_stats_t a, b;
    jpt_vk_renderer_stats_t sa, sb;
    set_defaults(&g->dev);
    g->dev.texture = NULL;

    begin_frame(g, 0xFF000000u);
    draw_quad(g, 4, 4, 28, 28, 0.5f, 0xFF00FF00u, 0xFF000000u);
    vk_win_test_bridge_stats(&a);
    vk_win_test_stats(&sa);
    lock_read(g, 0, 0, 16, 16);
    vk_win_test_stats(&sb);
    vk_win_test_bridge_stats(&b);
    CHECK(sb.one_shot_by_reason[JPT_VK_OS_READBACK] == sa.one_shot_by_reason[JPT_VK_OS_READBACK] &&
              sb.readbacks_in_frame > sa.readbacks_in_frame,
          "a readback inside the frame is recorded in its command buffer and waited for once, not submitted alone");
    CHECK(((const uint16_t*)g->target.bits)[10 + 10 * (g->target.pitch / 2)] == 0x07E0,
          "the tile read back holds the GPU's RGB565 pixels");
    CHECK(b.readbacks - a.readbacks == 1 && b.tiles_read - a.tiles_read == 1,
          "a small Lock reads only the tile the GPU drew in (%llu readbacks, %llu tiles)",
          b.readbacks - a.readbacks, b.tiles_read - a.tiles_read);

    a = b;
    lock_read(g, 0, 0, 16, 16);
    vk_win_test_bridge_stats(&b);
    CHECK(b.readbacks == a.readbacks && b.tiles_read == a.tiles_read, "a second Lock of the same region reads nothing");

    a = b;
    lock_read(g, 40, 40, 60, 60);
    vk_win_test_bridge_stats(&b);
    CHECK(b.readbacks == a.readbacks && b.tiles_read == a.tiles_read,
          "a Lock of a region the GPU never drew in reads nothing from the GPU");

    a = b;
    lock_read(g, 0, 0, W, H);
    vk_win_test_bridge_stats(&b);
    CHECK(b.readbacks == a.readbacks && b.tiles_read == a.tiles_read, "the whole target needs no tile that is already valid");
    CHECK(end_frame(g), "regional readback frame");

    begin_frame(g, 0xFF000000u);
    draw_quad(g, 4, 4, 28, 28, 0.5f, 0xFF00FF00u, 0xFF000000u);
    draw_quad(g, 36, 36, 60, 60, 0.5f, 0xFF0000FFu, 0xFF000000u);
    vk_win_test_bridge_stats(&a);
    lock_read(g, 0, 0, W, H);
    vk_win_test_bridge_stats(&b);
    CHECK(b.readbacks - a.readbacks == 1 && b.tiles_read - a.tiles_read == 2 && b.readback_rects - a.readback_rects == 2,
          "two distant GPU tiles are read in one submit with two copy regions (%llu readbacks, %llu tiles, %llu regions)",
          b.readbacks - a.readbacks, b.tiles_read - a.tiles_read, b.readback_rects - a.readback_rects);
    CHECK(end_frame(g), "multi-region readback frame");

    begin_frame(g, 0xFF000000u);
    draw_quad(g, 2, 2, 30, 30, 0.5f, 0xFF00FF00u, 0xFF000000u);
    g_test_text_boxes_known = 1;
    g->target.dc_out = 1;
    g->target.dc = (HDC)(uintptr_t)0x1234u;
    vk_win_test_bridge_stats(&a);
    ops()->surface_cpu_access(&g->target, 1, CPU_ACCESS_GETDC);
    vk_win_test_bridge_stats(&b);
    g->target.dc_out = 0;
    g->target.dc = NULL;
    g_test_text_boxes_known = 0;
    CHECK(b.readbacks == a.readbacks, "an access that only writes reads nothing back");
    CHECK(end_frame(g), "write-only frame");

    begin_frame(g, 0xFF000000u);
    draw_quad(g, 2, 2, 30, 30, 0.5f, 0xFF00FF00u, 0xFF000000u);
    lock_read(g, 0, 0, W, H);
    CHECK(end_frame(g), "flip after a read frame");
    begin_frame(g, 0xFF000000u);
    vk_win_test_bridge_stats(&a);
    lock_read(g, 0, 0, W, H);
    vk_win_test_bridge_stats(&b);
    CHECK(b.readbacks == a.readbacks && b.tiles_read == a.tiles_read,
          "a frame that only clears, after a Flip and with the same colour, reads nothing back (%llu)", b.readbacks - a.readbacks);
    CHECK(end_frame(g), "clear-only frame");
}

static void test_f2_text_layer_zero_stale_pixels(rig_t* g) {
    uint8_t base_rgba[W * H * 4];
    tl_view_t tv;
    RECT text_rc = { 16, 16, 48, 48 };
    HDC fake_dc = (HDC)(uintptr_t)0x1234u;

    set_defaults(&g->dev);
    g->dev.texture = NULL;
    g_test_text_boxes_known = 1;

    /* Frame 0: clean green 3D scene baseline */
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF008020u, 0)), "f2 baseline draw");
    CHECK(end_frame(g), "f2 baseline flip");
    memcpy(base_rgba, g->rgba, sizeof base_rgba);
    CHECK(!tl_current(&tv), "text_layer empty after clean frame");

    /* Frame 1: draw 3D scene + CTextOverlay F2 controls card into GetDC surface */
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF008020u, 0)), "f2 show draw");
    g->target.dc_out = 1;
    g->target.dc = fake_dc;
    ops()->surface_cpu_access(&g->target, 1, CPU_ACCESS_GETDC);
    ops()->text_box(fake_dc, &text_rc);
    for (int y = 16; y < 48; y++) {
        uint16_t* row = (uint16_t*)(g->target.bits + (size_t)y * (size_t)g->target.pitch);
        for (int x = 16; x < 48; x++) row[x] = 0xFFFFu; /* bright white F2 card pixels */
    }
    ops()->surface_cpu_written(&g->target, &text_rc);
    g->target.dc_out = 0;
    g->target.dc = NULL;
    CHECK(tl_current(&tv), "text_layer captured F2 pixels before flip");
    CHECK(end_frame(g), "f2 show flip");
    CHECK(near_rgb(px(g, 32, 32), 255, 255, 255, 4), "F2 text pixels visible on show frame (got %u,%u,%u)",
          px(g, 32, 32)[0], px(g, 32, 32)[1], px(g, 32, 32)[2]);
    CHECK(!tl_current(&tv), "vk_win_flip ended text_layer frame (0 stale state after flip)");

    /* Frame 2: F2 closed (state controls -> none), draw clean green scene without F2 */
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF008020u, 0)), "f2 hide draw");
    CHECK(end_frame(g), "f2 hide flip");
    CHECK(!tl_current(&tv), "text_layer stays empty after hide");
    CHECK(memcmp(g->rgba, base_rgba, sizeof base_rgba) == 0,
          "0 stale F2 pixels remain after closing F2 (center got %u,%u,%u)", px(g, 32, 32)[0], px(g, 32, 32)[1],
          px(g, 32, 32)[2]);

    g_test_text_boxes_known = 0;
}

static void test_frontend_hd_vs_ui_layer_mutual_exclusion(rig_t* g) {
    uint32_t fe_bgra[W * H];
    uint16_t* ui_bits = (uint16_t*)calloc(JPT_UI_CANVAS_W * JPT_UI_CANVAS_H, sizeof(uint16_t));
    RECT full_rc = { 0, 0, W, H };
    RECT dirty_rc = { 0, 0, 16, 16 };
    CHECK(ui_bits != NULL, "alloc ui canvas");
    if (!ui_bits) return;

    /* Frontend HD = crimson red (BGRA 0xFFD02020); 640x480 legacy UI canvas = bright blue (RGB565 0x001F) */
    for (int i = 0; i < W * H; i++) fe_bgra[i] = 0xFFD02020u;
    for (int i = 0; i < JPT_UI_CANVAS_W * JPT_UI_CANVAS_H; i++) ui_bits[i] = 0x001Fu;

    set_defaults(&g->dev);
    g->dev.texture = NULL;
    ops()->set_ui_layer(ui_bits, JPT_UI_CANVAS_W, W, H);
    ops()->set_frontend(fe_bgra, W, H, &full_rc);

    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF00FF00u, 0)), "fe mutual excl draw");
    CHECK(end_frame(g), "fe mutual excl flip");
    CHECK(near_rgb(px(g, 32, 32), 0xD0, 0x20, 0x20, 3),
          "Frontend HD crimson red shown, NOT overwritten by 640x480 UI blue (got %u,%u,%u)", px(g, 32, 32)[0],
          px(g, 32, 32)[1], px(g, 32, 32)[2]);

    /* Partial dirty-rect update on Frontend HD */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) fe_bgra[y * W + x] = 0xFF20E040u;
    ops()->set_frontend(fe_bgra, W, H, &dirty_rc);
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF00FF00u, 0)), "fe dirty draw");
    CHECK(end_frame(g), "fe dirty flip");
    CHECK(near_rgb(px(g, 8, 8), 0x20, 0xE0, 0x40, 3), "Frontend HD dirty rect updated");
    CHECK(near_rgb(px(g, 48, 48), 0xD0, 0x20, 0x20, 3), "Frontend HD outside dirty rect preserved");

    /* Clear Frontend HD -> 640x480 UI canvas fallback MUST appear */
    ops()->set_frontend(NULL, 0, 0, NULL);
    begin_frame(g, 0xFF000000u);
    CHECK(SUCCEEDED(draw_quad(g, 0, 0, W, H, 0.5f, 0xFF00FF00u, 0)), "ui fallback draw");
    CHECK(end_frame(g), "ui fallback flip");
    CHECK(near_rgb(px(g, 32, 32), 0, 0, 255, 4), "640x480 UI blue fallback shown when Frontend HD absent (got %u,%u,%u)",
          px(g, 32, 32)[0], px(g, 32, 32)[1], px(g, 32, 32)[2]);

    ops()->set_ui_layer(NULL, 0, 0, 0);
    ops()->frontend_release();
    free(ui_bits);
}

static void test_bridge_texture_coverage_24_sizes(rig_t* g) {
    tex_t tex[24];
    uint8_t want[24][3];
    vk_win_test_bridge_stats_t b0, b1;
    jpt_vk_renderer_stats_t s0, s1;

    vk_win_test_bridge_stats(&b0);
    stats(&s0);
    set_defaults(&g->dev);
    begin_frame(g, 0xFF000000u);

    for (int i = 0; i < 24; i++) {
        DWORD tw = (DWORD)(2 + i * 2);
        DWORD th = (DWORD)(2 + ((i * 5) % 23) * 2);
        uint8_t r5 = (uint8_t)(4 + (i % 27));
        uint8_t g6 = (uint8_t)(8 + ((i * 3) % 55));
        uint8_t b5 = (uint8_t)(4 + ((i * 7) % 27));
        uint16_t c565 = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        want[i][0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        want[i][1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        want[i][2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        tex_init(&tex[i], tw, th, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
        for (DWORD y = 0; y < th; y++) {
            uint16_t* row = (uint16_t*)(tex[i].bits + (size_t)y * (size_t)tex[i].s.pitch);
            for (DWORD x = 0; x < tw; x++) row[x] = c565;
        }
        g->dev.texture = &tex[i].s;
        float x0 = (float)((i % 6) * 10);
        float y0 = (float)((i / 6) * 16);
        CHECK(SUCCEEDED(draw_quad(g, x0, y0, x0 + 10.0f, y0 + 16.0f, 0.5f, 0xFFFFFFFFu, 0)), "24-size draw %d", i);
    }
    g->dev.texture = NULL;
    CHECK(end_frame(g), "24-size flip");

    for (int i = 0; i < 24; i++) {
        int cx = (i % 6) * 10 + 5;
        int cy = (i / 6) * 16 + 8;
        const uint8_t* p = px(g, cx, cy);
        CHECK(near_rgb(p, want[i][0], want[i][1], want[i][2], 4),
              "bridge 24-size tile %d mismatch: want (%u,%u,%u) got (%u,%u,%u)", i, want[i][0], want[i][1], want[i][2],
              p[0], p[1], p[2]);
        tex_free(&tex[i]);
    }

    vk_win_test_bridge_stats(&b1);
    stats(&s1);
    CHECK(b1.texture_skips == b0.texture_skips, "0 texture skips across 24 distinct sizes (%llu)",
          b1.texture_skips - b0.texture_skips);
    CHECK(s1.texture_failures[JPT_VK_TEX_NO_POOL] == s0.texture_failures[JPT_VK_TEX_NO_POOL],
          "0 NO_POOL failures across 24 distinct sizes");
}


static void exchange_bits(dd_surface_t* a, dd_surface_t* b) {
    BYTE* t = a->bits;
    a->bits = b->bits;
    b->bits = t;
}

static size_t count_value(const uint16_t* p, size_t n, uint16_t v) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += p[i] == v;
    return c;
}

static size_t count_rgba(const uint8_t* rgba, int r, int gr, int b) {
    size_t c = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) c += near_rgb(rgba + i * 4u, r, gr, b, 0);
    return c;
}

static void rgb565_of(const uint16_t v, uint8_t out[3]) {
    out[0] = (uint8_t)(((v >> 11) & 31u) * 255u / 31u);
    out[1] = (uint8_t)(((v >> 5) & 63u) * 255u / 63u);
    out[2] = (uint8_t)((v & 31u) * 255u / 31u);
}

static void gameplay_frame(rig_t* g, dd_surface_t* front, DWORD color, int with_text) {
    RECT text_rc = { 8, 8, 24, 24 };
    HDC fake_dc = (HDC)(uintptr_t)0x4321u;
    begin_frame(g, 0xFF000000u);
    draw_quad(g, 0, 0, W, H, 0.5f, color, 0);
    if (with_text) {
        g->target.dc_out = 1;
        g->target.dc = fake_dc;
        ops()->surface_cpu_access(&g->target, 1, CPU_ACCESS_GETDC);
        ops()->text_box(fake_dc, &text_rc);
        for (int y = 8; y < 24; y++) {
            uint16_t* row = (uint16_t*)(g->target.bits + (size_t)y * (size_t)g->target.pitch);
            for (int x = 8; x < 24; x++) row[x] = 0xFFFFu;
        }
        ops()->surface_cpu_written(&g->target, &text_rc);
        g->target.dc_out = 0;
        g->target.dc = NULL;
    }
    ops()->end_scene(&g->dev);
    g->dev.in_scene = 0;
    CHECK(ops()->flip(front, 0), "gameplay frame flips");
    exchange_bits(front, &g->target);
}

/* The game freezes its pause background through GetDC of the front buffer: the picture must be the last frame whatever
 * the CPU copy of that buffer holds and whether the next frame has begun. */
static void capture_front(dd_surface_t* front, uint16_t* dib, int next_frame_open, rig_t* g) {
    if (next_frame_open) {
        begin_frame(g, 0xFF000000u);
        draw_quad(g, 0, 0, 2, 2, 0.5f, 0xFFFF00FFu, 0);
    }
    ops()->surface_cpu_access(front, 1, CPU_ACCESS_GETDC);
    memcpy(dib, front->bits, (size_t)W * H * 2u);
    if (next_frame_open) {
        ops()->end_scene(&g->dev);
        g->dev.in_scene = 0;
    }
}

static void pause_frame(rig_t* g, dd_surface_t* front, const uint16_t* dib, uint16_t* ui_bits) {
    CHECK(ops()->draw_still_picture(&g->target, dib, W, W, H), "the frozen picture is drawn on the GPU target");
    ops()->set_ui_layer(ui_bits, JPT_UI_CANVAS_W, W, H);
    CHECK(ops()->flip(front, 0), "pause frame flips");
    exchange_bits(front, &g->target);
    ops()->set_ui_layer(NULL, 0, 0, 0);
}

static void test_pause_snapshot(rig_t* g) {
    dd_surface_t front;
    uint16_t dib[W * H];
    uint16_t* ui_bits = (uint16_t*)malloc((size_t)JPT_UI_CANVAS_W * JPT_UI_CANVAS_H * sizeof(uint16_t));
    vk_win_test_bridge_stats_t b0, b1;
    uint32_t rw = 0, rh = 0;
    CHECK(ui_bits != NULL, "alloc ui canvas");
    if (!ui_bits) return;

    memset(&front, 0, sizeof front);
    front.device = &g->dd;
    front.width = W;
    front.height = H;
    front.pitch = W * 2;
    front.format = g->target.format;
    front.bits = (BYTE*)calloc(1, (size_t)W * H * 2u);
    front.caps = g->target.caps;
    front.next = &g->target;
    g->target.next = &front;
    g->target.root = &front;
    set_defaults(&g->dev);
    g->dev.texture = NULL;
    g_test_text_boxes_known = 1;

    for (int phase = 0; phase < 2; phase++) {
        int next_open = phase == 1;
        size_t unwritten, magenta;
        uint8_t want[3];

        gameplay_frame(g, &front, 0xFF008020u, 1);
        unwritten = count_value((const uint16_t*)front.bits, (size_t)W * H, 0xF81Fu);
        CHECK(unwritten > 0, "the buffer the flip brought to the front still holds the text capture fill (%zu texels)",
              unwritten);

        capture_front(&front, dib, next_open, g);
        magenta = count_value(dib, (size_t)W * H, 0xF81Fu);
        CHECK(magenta == 0, "pause capture %d: the frozen picture holds no capture colour (%zu texels)", phase, magenta);
        rgb565_of(dib[40 * W + 40], want);
        CHECK(near_rgb(want, 0, 128, 33, 12), "pause capture %d: the picture is the last frame (got %u,%u,%u)", phase,
              want[0], want[1], want[2]);
        CHECK(dib[12 * W + 12] == 0xFFFFu, "pause capture %d: the HUD text of that frame is in the picture (0x%04X)", phase,
              dib[12 * W + 12]);

        for (int i = 0; i < JPT_UI_CANVAS_W * JPT_UI_CANVAS_H; i++) ui_bits[i] = 0xF81Fu;
        for (int y = 200; y < 280; y++)
            for (int x = 280; x < 360; x++) ui_bits[y * JPT_UI_CANVAS_W + x] = 0x001Fu;
        pause_frame(g, &front, dib, ui_bits);
        CHECK(vk_win_test_readback(g->rgba, sizeof g->rgba, &rw, &rh) && rw == W && rh == H, "pause frame read back");
        CHECK(count_rgba(g->rgba, 255, 0, 255) == 0, "pause %d: no pixel of the final picture is the chroma key", phase);
        CHECK(near_rgb(px(g, 2, 2), 0, 130, 32, 10), "pause %d: outside the menu the frozen gameplay shows (got %u,%u,%u)",
              phase, px(g, 2, 2)[0], px(g, 2, 2)[1], px(g, 2, 2)[2]);
        CHECK(near_rgb(px(g, 12, 12), 255, 255, 255, 4), "pause %d: the frozen HUD text shows", phase);
        CHECK(near_rgb(px(g, 32, 32), 0, 0, 255, 4), "pause %d: the menu is drawn over it (got %u,%u,%u)", phase,
              px(g, 32, 32)[0], px(g, 32, 32)[1], px(g, 32, 32)[2]);
        CHECK(near_rgb(px(g, 50, 40), 0, 130, 32, 10), "pause %d: a transparent menu texel shows the picture, not the key",
              phase);

        gameplay_frame(g, &front, 0xFFFF0000u, 0);
        CHECK(vk_win_test_readback(g->rgba, sizeof g->rgba, &rw, &rh), "resume frame read back");
        CHECK(near_rgb(px(g, 32, 32), 255, 0, 0, 4) && near_rgb(px(g, 2, 2), 255, 0, 0, 4),
              "resume %d: gameplay is back without the old picture (got %u,%u,%u)", phase, px(g, 32, 32)[0],
              px(g, 32, 32)[1], px(g, 32, 32)[2]);
    }

    gameplay_frame(g, &front, 0xFFFF0000u, 0);
    capture_front(&front, dib, 0, g);
    CHECK(count_value(dib, (size_t)W * H, 0xF81Fu) == 0, "reopened pause: no capture colour in the new picture");
    for (int i = 0; i < JPT_UI_CANVAS_W * JPT_UI_CANVAS_H; i++) ui_bits[i] = 0xF81Fu;
    pause_frame(g, &front, dib, ui_bits);
    CHECK(vk_win_test_readback(g->rgba, sizeof g->rgba, &rw, &rh), "reopened pause read back");
    CHECK(near_rgb(px(g, 32, 32), 255, 0, 0, 6) && count_rgba(g->rgba, 255, 0, 255) == 0,
          "reopened pause shows the new frame (got %u,%u,%u), never the first one or the key", px(g, 32, 32)[0],
          px(g, 32, 32)[1], px(g, 32, 32)[2]);

    vk_win_test_bridge_stats(&b0);
    for (int i = 0; i < 4; i++) pause_frame(g, &front, dib, ui_bits);
    vk_win_test_bridge_stats(&b1);
    CHECK(b1.still_draws - b0.still_draws == 4 && b1.still_uploads == b0.still_uploads,
          "four pause frames over one picture draw it four times and convert it never again (%llu uploads)",
          b1.still_uploads - b0.still_uploads);
    CHECK(vk_win_test_readback(g->rgba, sizeof g->rgba, &rw, &rh) && near_rgb(px(g, 32, 32), 255, 0, 0, 6),
          "a reused frozen picture still shows");

    g->target.next = NULL;
    g->target.root = NULL;
    free(front.bits);
    free(ui_bits);
    g_test_text_boxes_known = 0;
}


typedef struct {
    double frame_ms[64];
    int frames;
    double bridge_ms, draw_ms, upload_ms, present_ms;
} bench_result_t;

static double qpc_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int compare_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* A heavy gameplay frame as the guest issues it: thousands of small textured fans over many textures, alpha tested
 * and blended, with a share of the textures rewritten every frame (terrain and animated surfaces). */
static void bench_workload(rig_t* g, int frames, int draws_per_frame, bench_result_t* out) {
    enum { TEXTURES = 96, TEX_SIZE = 64 };
    static tex_t tex[TEXTURES];
    jpt_vk_renderer_stats_t s0, s1;
    uint32_t seed = 12345;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < TEXTURES; i++) {
        int kind = i % 3;
        if (kind == 0) tex_init(&tex[i], TEX_SIZE, TEX_SIZE, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
        else if (kind == 1) tex_init(&tex[i], TEX_SIZE, TEX_SIZE, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000);
        else tex_init(&tex[i], TEX_SIZE, TEX_SIZE, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
        for (int p = 0; p < TEX_SIZE * TEX_SIZE; p++) ((uint16_t*)tex[i].bits)[p] = (uint16_t)(seed = seed * 1664525u + 1013904223u) | 0x8000u;
    }
    set_defaults(&g->dev);
    stats(&s0);
    for (int f = 0; f < frames && f < 64; f++) {
        double t0 = qpc_ms(), t1;
        begin_frame(g, 0xFF000000u);
        for (int d = 0; d < draws_per_frame; d++) {
            int ti = (d * 7 + f) % TEXTURES;
            float x = (float)(d % 56), y = (float)((d / 56) % 56);
            g->dev.texture = &tex[ti].s;
            g->dev.render[D3DRENDERSTATE_ALPHATESTENABLE] = ti % 3 == 1;
            g->dev.render[D3DRENDERSTATE_ALPHABLENDENABLE] = ti % 3 == 2;
            g->dev.render[D3DRENDERSTATE_SRCBLEND] = 5;
            g->dev.render[D3DRENDERSTATE_DESTBLEND] = 6;
            draw_quad(g, x, y, x + 6.0f, y + 6.0f, 0.2f + (float)(d % 100) * 0.005f, 0xFFFFFFFFu, 0xFF000000u);
            if (d % 500 == 0) {
                RECT dirty = { 0, 0, TEX_SIZE, 8 };
                int w = (d / 500 + f) % TEXTURES;
                ops()->surface_cpu_access(&tex[w].s, 1, CPU_ACCESS_LOCK);
                for (int p = 0; p < TEX_SIZE * 8; p++) ((uint16_t*)tex[w].bits)[p] ^= 0x0421u;
                ops()->surface_cpu_written(&tex[w].s, &dirty);
            }
        }
        g->dev.texture = NULL;
        t1 = qpc_ms();
        ops()->end_scene(&g->dev);
        g->dev.in_scene = 0;
        CHECK(ops()->flip(&g->target, 0), "benchmark frame presented");
        out->frame_ms[out->frames++] = qpc_ms() - t0;
        out->bridge_ms += t1 - t0;
    }
    stats(&s1);
    out->draw_ms = (s1.cpu_draw_ms - s0.cpu_draw_ms) / (double)frames;
    out->upload_ms = (s1.cpu_upload_ms - s0.cpu_upload_ms) / (double)frames;
    out->present_ms = (s1.cpu_present_ms - s0.cpu_present_ms) / (double)frames;
    out->bridge_ms /= (double)frames;
    for (int i = 0; i < TEXTURES; i++) tex_free(&tex[i]);
}

/* Host overhead of the backend on a synthetic heavy frame: per draw, stable from frame to frame, and the cost of the
 * same frame with the texture working set already resident must not exceed the first frames' by a factor. */
static void test_backend_overhead(rig_t* g, int verbose) {
    bench_result_t r;
    double sorted[64], median, p95, per_draw_us;
    const int draws = 4000;
    bench_workload(g, 40, draws, &r);
    memcpy(sorted, r.frame_ms, sizeof(double) * (size_t)r.frames);
    qsort(sorted, (size_t)r.frames, sizeof sorted[0], compare_double);
    median = sorted[r.frames / 2];
    p95 = sorted[(r.frames * 95) / 100];
    per_draw_us = r.bridge_ms * 1000.0 / draws;
    if (verbose)
        printf("vulkan backend overhead: %d draws per frame, bridge %.2f ms per frame (%.2f us per draw; renderer draw "
               "recording %.2f ms, texture upload %.2f ms, present/submit %.2f ms), frame median %.2f ms, p95 %.2f ms\n", draws,
               r.bridge_ms, per_draw_us, r.draw_ms, r.upload_ms, r.present_ms, median, p95);
    CHECK(per_draw_us < 40.0, "backend host cost per draw stays bounded (%.2f us per draw)", per_draw_us);
    CHECK(p95 < median * 4.0 + 5.0, "frames are stable (median %.2f ms, p95 %.2f ms)", median, p95);
}


static int nearly(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_cpu_accounting_model(void) {
    vka_t a, base, d;
    vka_split_t sp;
    vka_reset(&a);
    vka_enter(&a, VKA_DRAW, 0.0);
    vka_enter(&a, VKA_TEXTURE, 2.0);
    vka_enter(&a, VKA_SYNC, 3.0);
    vka_leave(&a, 4.0);
    vka_leave(&a, 5.0);
    vka_leave(&a, 10.0);
    CHECK(a.depth == 0 && !a.unbalanced && !a.overflow, "scopes balance");
    CHECK(nearly(a.ms[VKA_DRAW], 7.0) && nearly(a.ms[VKA_TEXTURE], 2.0) && nearly(a.ms[VKA_SYNC], 1.0),
          "a scope's own time excludes the scopes opened inside it (%.1f, %.1f, %.1f)", a.ms[VKA_DRAW], a.ms[VKA_TEXTURE],
          a.ms[VKA_SYNC]);
    CHECK(nearly(vka_total_ms(&a), 10.0), "the categories add up to the outer span (%.1f)", vka_total_ms(&a));
    CHECK(a.calls[VKA_DRAW] == 1 && a.calls[VKA_SYNC] == 1 && nearly(a.max_ms[VKA_DRAW], 7.0), "calls and maxima");
    vka_leave(&a, 11.0);
    CHECK(a.unbalanced == 1, "a leave without an enter is counted, not subtracted");
    vka_reset(&a);
    for (int i = 0; i < VKA_MAX_DEPTH + 3; i++) vka_enter(&a, VKA_OTHER, (double)i);
    for (int i = 0; i < VKA_MAX_DEPTH + 3; i++) vka_leave(&a, 100.0);
    CHECK(a.depth == 0 && a.overflow == 3, "scopes nested past the limit are counted and unwind");
    vka_reset(&base);
    vka_reset(&a);
    vka_enter(&a, VKA_FLIP, 0.0);
    vka_leave(&a, 4.0);
    vka_delta(&a, &base, &d);
    CHECK(nearly(d.ms[VKA_FLIP], 4.0) && d.calls[VKA_FLIP] == 1, "a window is the difference of two snapshots");

    vka_split(1000.0, 600.0, 300.0, 250.0, 10, &sp);
    CHECK(nearly(sp.frame_ms, 100.0) && nearly(sp.backend_ms, 30.0) && nearly(sp.guest_ms, 70.0) && nearly(sp.cpu_ms, 60.0),
          "the split of a window per frame (%.1f, %.1f, %.1f, %.1f)", sp.frame_ms, sp.backend_ms, sp.guest_ms, sp.cpu_ms);
    CHECK(nearly(sp.blocked_ms, 40.0) && nearly(sp.waits_ms, 25.0) && nearly(sp.unaccounted_ms, 15.0) &&
              nearly(sp.unaccounted_pct, 15.0),
          "blocked time minus the timed waits is what no wait explains (%.1f ms, %.1f%%)", sp.unaccounted_ms,
          sp.unaccounted_pct);
    vka_split(1000.0, 600.0, 300.0, 900.0, 10, &sp);
    CHECK(nearly(sp.unaccounted_ms, 0.0) && sp.waits_ms <= sp.blocked_ms, "waits never exceed the blocked time");
    vka_split(100.0, 150.0, 400.0, 0.0, 0, &sp);
    CHECK(sp.backend_ms <= sp.frame_ms && sp.cpu_ms <= sp.frame_ms, "measured values never exceed the wall time");
}

/* The bridge's own scopes on a real frame stream: balanced, every category the frame used is non-zero, and together they
 * stay inside the wall time of the frames that ran them. */
static void test_cpu_accounting_bridge(rig_t* g) {
    vka_t before, after, d;
    double t0, wall;
    vk_win_test_account(&before);
    t0 = qpc_ms();
    {
        bench_result_t r;
        bench_workload(g, 12, 1500, &r);
    }
    wall = qpc_ms() - t0;
    vk_win_test_account(&after);
    vka_delta(&after, &before, &d);
    CHECK(after.depth == 0 && !after.unbalanced && !after.overflow, "bridge scopes balance after real frames");
    CHECK(d.ms[VKA_DRAW] > 0.0 && d.calls[VKA_DRAW] >= 12u * 1500u, "draw translation is timed (%.2f ms, %llu calls)",
          d.ms[VKA_DRAW], d.calls[VKA_DRAW]);
    CHECK(d.ms[VKA_FLIP] > 0.0 && d.calls[VKA_FLIP] >= 12u, "flip and present is timed (%.2f ms)", d.ms[VKA_FLIP]);
    CHECK(d.calls[VKA_TEXTURE] > 0 && d.calls[VKA_CPU_ACCESS] > 0, "texture preparation and CPU access are timed");
    CHECK(vka_total_ms(&d) <= wall * 1.02, "the categories (%.1f ms) stay inside the wall time (%.1f ms)", vka_total_ms(&d),
          wall);
}


static uint16_t poly_rgb565(unsigned r, unsigned gr, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((gr >> 2) << 5) | (b >> 3));
}

static void poly_expected(uint16_t px, uint8_t out[3]) {
    unsigned r5 = (px >> 11) & 31u, g6 = (px >> 5) & 63u, b5 = px & 31u;
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Perspective-textured polygons the guest would rasterise itself, drawn by the GPU: every pixel is what the span walk
 * computes (texel index, CLUT, expansion), the rest of the target is untouched, and a polygon the GPU cannot take is
 * refused. */
static void test_gpu_texture_polygons(rig_t* g) {
    static uint8_t texels[64 * 64];
    static uint16_t clut[256];
    static uint8_t table[TGPU_BUMP_TABLE_BYTES];
    static uint16_t bump_texels[64 * 64];
    static uint16_t bump_clut[64 * 8];
    gpu_texture_polygon_t p;
    vk_win_test_bridge_stats_t b0, b1;
    jpt_vk_renderer_stats_t s0, s1;
    int exact = 1, outside = 1, bump_exact = 1, refused;

    for (int i = 0; i < 64 * 64; i++) texels[i] = (uint8_t)((i % 64) * 3 + (i / 64));
    for (int i = 0; i < 256; i++) clut[i] = poly_rgb565((unsigned)i, (unsigned)(255 - i), (unsigned)((i * 7) & 255));
    memset(&p, 0, sizeof p);
    p.count = 4;
    p.x[0] = 8; p.y[0] = 8;
    p.x[1] = 56; p.y[1] = 8;
    p.x[2] = 56; p.y[2] = 56;
    p.x[3] = 8; p.y[3] = 56;
    p.plane[0][0] = 0.0f; p.plane[0][1] = 1.0f; p.plane[0][2] = 0.0f;
    p.plane[1][0] = 0.0f; p.plane[1][1] = 0.0f; p.plane[1][2] = 1.0f;
    p.plane[2][0] = 1.0f; p.plane[2][1] = 0.0f; p.plane[2][2] = 0.0f;
    p.plane[3][0] = 8.0f; p.plane[3][1] = 8.0f;
    p.clamped = 1;
    p.umin = 0.0f;
    p.umax = 63.0f;
    p.vmax = 63.0f;
    p.line = 64;
    p.umask = 63;
    p.tile_mask = 0xFFF;
    p.texels = texels;
    p.texel_bytes = sizeof texels;
    p.clut = clut;
    p.clut_entries = 256;
    p.kind = GPU_POLYGON_TEXTURE;

    set_defaults(&g->dev);
    ops()->texture_polygon_reset();
    vk_win_test_bridge_stats(&b0);
    stats(&s0);
    begin_frame(g, 0xFF000000u);
    {
        gpu_polygon_status_t st_ = ops()->texture_polygon_prepare(&g->target, &p);
        CHECK(st_ == GPU_POLYGON_READY, "a perspective polygon is accepted (status %d, bits %p pitch %ld target %p)", (int)st_,
              (void*)g->target.bits, (long)g->target.pitch, (void*)g->target.target);
    }
    CHECK(ops()->texture_polygon(&g->target, &p) == 1, "it is drawn");
    CHECK(end_frame(g), "polygon frame presented");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* got = px(g, x, y);
            if (x >= 8 && x < 56 && y >= 8 && y < 56) {
                uint8_t want[3];
                unsigned qx = (unsigned)(x - 8), qy = (unsigned)(y - 8);
                poly_expected(clut[texels[((qy * 64u) + (qx & 63u)) & 0xFFFu]], want);
                if (!near_rgb(got, want[0], want[1], want[2], 0)) exact = 0;
            } else if ((x < 7 || x > 56 || y < 7 || y > 56) && !near_rgb(got, 0, 0, 0, 0)) {
                outside = 0;
            }
        }
    CHECK(exact, "every pixel of the polygon is the CLUT colour of its texel");
    CHECK(outside, "pixels outside the polygon stay as the GPU had them");
    vk_win_test_bridge_stats(&b1);
    stats(&s1);
    CHECK(s1.poly_draws - s0.poly_draws == 1 && s1.poly_uploads - s0.poly_uploads >= 2,
          "one polygon drawn, its texels and CLUT uploaded (%llu draws, %llu uploads)",
          (unsigned long long)(s1.poly_draws - s0.poly_draws), (unsigned long long)(s1.poly_uploads - s0.poly_uploads));

    begin_frame(g, 0xFF000000u);
    CHECK(ops()->texture_polygon_prepare(&g->target, &p) == GPU_POLYGON_READY && ops()->texture_polygon(&g->target, &p),
          "the same polygon again");
    stats(&s1);
    CHECK(s1.poly_uploads - s0.poly_uploads <= 3, "the texture and CLUT are cached, not uploaded again (%llu uploads)",
          (unsigned long long)(s1.poly_uploads - s0.poly_uploads));
    CHECK(end_frame(g), "second polygon frame");

    p.count = 2;
    refused = ops()->texture_polygon_prepare(&g->target, &p) != GPU_POLYGON_READY;
    CHECK(refused, "fewer than three vertices are refused so the CPU draws them");
    p.count = 4;
    CHECK(ops()->texture_polygon_prepare(NULL, &p) == GPU_POLYGON_NO_TARGET, "no surface, no target");

    /* bump: 16-bit texels, an intensity table and CLUT rows */
    for (int i = 0; i < 64 * 64; i++) bump_texels[i] = (uint16_t)(((i % 64) & 0x3F) | (((i / 64) & 7) << 10));
    for (int i = 0; i < (int)TGPU_BUMP_TABLE_BYTES; i++) table[i] = (uint8_t)((i >> 2) & 7);
    for (int i = 0; i < 64 * 8; i++) bump_clut[i] = poly_rgb565((unsigned)((i * 5) & 255), (unsigned)(i & 255), (unsigned)(255 - (i & 255)));
    p.kind = GPU_POLYGON_BUMP;
    p.texels = (const uint8_t*)bump_texels;
    p.texel_bytes = sizeof bump_texels;
    p.clut = bump_clut;
    p.clut_entries = tgpu_bump_clut_entries(table);
    p.bump_table = table;
    p.light_theta = 5;
    p.tile_mask = 0xFFF;
    begin_frame(g, 0xFF000000u);
    CHECK(ops()->texture_polygon_prepare(&g->target, &p) == GPU_POLYGON_READY && ops()->texture_polygon(&g->target, &p),
          "a bump-mapped polygon is accepted and drawn");
    CHECK(end_frame(g), "bump frame presented");
    for (int y = 8; y < 56; y++)
        for (int x = 8; x < 56; x++) {
            unsigned qx = (unsigned)(x - 8), qy = (unsigned)(y - 8);
            unsigned texel = bump_texels[((qy * 64u) + (qx & 63u)) & 0xFFFu];
            uint8_t want[3];
            poly_expected(bump_clut[tgpu_bump_clut_index(texel, p.light_theta, table)], want);
            if (!near_rgb(px(g, x, y), want[0], want[1], want[2], 0)) bump_exact = 0;
        }
    CHECK(bump_exact, "every bump pixel is the CLUT row the table picks for its angle");
}


/* A 1-texel checker of black and white drawn 8 times smaller than itself: with the mipmap chain the Direct3D 11 backend
 * builds (d3dt_build_mips) it averages to grey; the chain is uploaded whole and reused until the texture changes. */
static void test_mipmaps(rig_t* g) {
    struct {
        const char* name;
        DWORD flags, bits, rm, gm, bm, am;
        uint16_t light, dark;
    } fmt[3] = {
        { "RGB565", DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0, 0xFFFF, 0x0000 },
        { "ARGB1555", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000, 0xFFFF, 0x8000 },
        { "ARGB4444", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000, 0xFFFF, 0xF000 },
    };
    set_defaults(&g->dev);
    for (int k = 0; k < 3; k++) {
        tex_t t;
        vk_win_test_bridge_stats_t b0, b1;
        jpt_vk_renderer_stats_t s0, s1;
        int grey = 1;
        tex_init(&t, 64, 64, fmt[k].flags, fmt[k].bits, fmt[k].rm, fmt[k].gm, fmt[k].bm, fmt[k].am);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) ((uint16_t*)(t.bits + (size_t)y * t.s.pitch))[x] = ((x + y) & 1) ? fmt[k].light : fmt[k].dark;
        g->dev.texture = &t.s;
        vk_win_test_bridge_stats(&b0);
        stats(&s0);
        begin_frame(g, 0xFF000000u);
        draw_quad(g, 8, 8, 16, 16, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
        CHECK(end_frame(g), "%s mipmap frame", fmt[k].name);
        stats(&s1);
        vk_win_test_bridge_stats(&b1);
        for (int y = 10; y < 14; y++)
            for (int x = 10; x < 14; x++) {
                const uint8_t* p = px(g, x, y);
                if (abs((int)p[0] - 127) > 40 || abs((int)p[1] - 127) > 40) grey = 0;
            }
        CHECK(grey, "%s: the distant checker averages to grey through its mip levels (got %u,%u,%u)", fmt[k].name,
              px(g, 11, 11)[0], px(g, 11, 11)[1], px(g, 11, 11)[2]);
        CHECK(s1.texture_upload_bytes - s0.texture_upload_bytes == (64u * 64u + 32u * 32u + 16u * 16u + 8u * 8u + 4u * 4u +
                                                                    2u * 2u + 1u) * 4u,
              "%s: the whole chain of seven levels is uploaded (%llu bytes)", fmt[k].name,
              (unsigned long long)(s1.texture_upload_bytes - s0.texture_upload_bytes));
        CHECK(b1.mipped_textures - b0.mipped_textures == 1, "%s: counted as a texture with generated mipmaps", fmt[k].name);

        stats(&s0);
        begin_frame(g, 0xFF000000u);
        draw_quad(g, 8, 8, 16, 16, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
        CHECK(end_frame(g), "%s second frame", fmt[k].name);
        stats(&s1);
        CHECK(s1.texture_upload_bytes == s0.texture_upload_bytes, "%s: an unchanged texture is not uploaded or rebuilt per draw", fmt[k].name);
        g->dev.texture = NULL;
        tex_free(&t);
    }
}


/* The sampler the game's own filters and the chosen filter give (d3dt_choose_sampler): point stays point, linear without a
 * chain is linear level 0 only. */
static void test_sampler_policy(rig_t* g) {
    const int filters[2] = { JPT_FILTER_ORIGINAL, JPT_FILTER_ANISO4 };
    for (int f = 0; f < 2; f++) {
        for (int linear = 0; linear < 2; linear++) {
            tex_t t;
            uint16_t texels[4] = { 0x0000, 0xFFFF, 0xFFFF, 0x0000 };
            int soft;
            vk_win_test_set_filter(filters[f]);
            set_defaults(&g->dev);
            g->dev.stage[0][D3DTSS_MAGFILTER] = linear ? 2 : 1;
            g->dev.stage[0][D3DTSS_MINFILTER] = linear ? 2 : 1;
            tex_init(&t, 2, 2, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
            tex_fill16(&t, texels);
            g->dev.texture = &t.s;
            begin_frame(g, 0xFF000000u);
            draw_quad(g, 0, 0, 32, 32, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
            CHECK(end_frame(g), "sampler frame");
            soft = px(g, 15, 8)[0] > 60 && px(g, 15, 8)[0] < 200;
            CHECK(soft == linear, "%s with the game's %s filters: %s (got %u)", filters[f] == JPT_FILTER_ORIGINAL ? "Original" : "Anisotropic",
                  linear ? "linear" : "point", linear ? "interpolated" : "hard edged", px(g, 15, 8)[0]);
            g->dev.texture = NULL;
            tex_free(&t);
        }
    }
    {
        /* mixed: a 2x2 checker magnified 16x (mag decides) */
        const int cases[2][2] = { { 1, 2 }, { 2, 1 } }; /* { mag, min } game filter values */
        for (int k = 0; k < 2; k++) {
            tex_t t;
            uint16_t texels[4] = { 0x0000, 0xFFFF, 0xFFFF, 0x0000 };
            int soft;
            vk_win_test_set_filter(JPT_FILTER_ORIGINAL);
            set_defaults(&g->dev);
            g->dev.stage[0][D3DTSS_MAGFILTER] = (DWORD)cases[k][0];
            g->dev.stage[0][D3DTSS_MINFILTER] = (DWORD)cases[k][1];
            tex_init(&t, 2, 2, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
            tex_fill16(&t, texels);
            g->dev.texture = &t.s;
            begin_frame(g, 0xFF000000u);
            draw_quad(g, 0, 0, 32, 32, 0.5f, 0xFFFFFFFFu, 0xFF000000u);
            CHECK(end_frame(g), "mixed sampler frame");
            soft = px(g, 15, 8)[0] > 60 && px(g, 15, 8)[0] < 200;
            CHECK(soft == (k == 1), "min %s / mag %s: magnification is %s (got %u)", k ? "point" : "linear",
                  k ? "linear" : "point", k ? "linear (interpolated)" : "point (hard edge)", px(g, 15, 8)[0]);
            g->dev.texture = NULL;
            tex_free(&t);
        }
    }
    vk_win_test_set_filter(JPT_FILTER_BILINEAR);
}

static void test_counter_gate(void) {
    jpt_vk_renderer_stats_t st;
    vk_win_test_bridge_stats_t br;
    stats(&st);
    vk_win_test_bridge_stats(&br);
    CHECK(st.draws > 0 && st.pipelines_created > 0, "draws without pipelines never pass: %llu draws, %llu pipelines",
          (unsigned long long)st.draws, (unsigned long long)st.pipelines_created);
    CHECK(st.textured_draws == 0 || st.textures_uploaded > 0, "textured draws without texture uploads never pass");
    CHECK(st.draw_calls_recorded > 0 && st.draw_calls_recorded <= st.draws, "recorded draw calls never exceed draws");
    CHECK(st.pipeline_cache_hits > 0, "pipelines are reused between draws (%llu hits)", (unsigned long long)st.pipeline_cache_hits);
    CHECK(br.guest_draws >= st.draws, "guest draws (%llu) cover Vulkan draws (%llu)", br.guest_draws, (unsigned long long)st.draws);
    CHECK(br.validation_enabled == 0 || br.validation_errors == 0, "no Vulkan validation errors (%ld)", br.validation_errors);
    for (int i = 1; i < JPT_VK_SKIP_REASONS; i++)
        CHECK(st.draws_skipped[i] == 0, "renderer skipped no draw for '%s' (%llu)", jpt_vk_skip_reason_name((jpt_vk_draw_result_t)i),
              (unsigned long long)st.draws_skipped[i]);
}

int main(int argc, char** argv) {
    int verbose = argc > 1 && !strcmp(argv[1], "--bench");
    jpt_vk_probe_result_t probe;
    rig_t* g;
    if (!jpt_vk_probe_headless("", &probe) || !probe.available) {
        printf("vulkan_runtime_tests: 0 checks, 0 failures (GPU fixtures skipped: %s)\n", probe.summary);
        return 0;
    }
    printf("vulkan_runtime_tests: running on '%s'\n", probe.gpus[probe.selected_index].label);
    g = (rig_t*)malloc(sizeof *g);
    if (!g) return 1;
    vk_win_test_enable_headless(W, H);
    CHECK(rig_open(g), "device attaches to the Vulkan backend");
    if (g_failures == 0) {
        test_basic_textures(g);
        test_blend_modes_match_reference(g);
        test_geometry(g);
        test_viewport_clips_without_scaling(g);
        test_color_key(g);
        test_dynamic_vertices(g);
        test_fog_and_specular(g);
        test_dirty_tracking(g);
        test_palette_and_unavailable(g);
        test_batching(g);
        test_software_polygons(g);
        test_regional_readbacks(g);
        test_f2_text_layer_zero_stale_pixels(g);
        test_frontend_hd_vs_ui_layer_mutual_exclusion(g);
        test_bridge_texture_coverage_24_sizes(g);
        test_pause_snapshot(g);
        test_gpu_texture_polygons(g);
        test_mipmaps(g);
        test_sampler_policy(g);
        test_backend_overhead(g, verbose);
        test_cpu_accounting_model();
        test_cpu_accounting_bridge(g);
        test_counter_gate();
    }
    rig_close(g);
    vk_win_test_shutdown();
    free(g);
    if (g_failures) {
        fprintf(stderr, "vulkan_runtime_tests: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("vulkan_runtime_tests: %d checks, 0 failures\n", g_checks);
    return 0;
}
