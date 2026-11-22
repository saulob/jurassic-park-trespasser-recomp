#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_present_policy.h"
#include "vk_context.h"
#include "vk_renderer.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);                     \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fputc('\n', stderr);                                                                 \
            g_failures++;                                                                        \
        }                                                                                        \
    } while (0)

static jpt_render_vertex_t make_v(float x, float y, float z, uint32_t diffuse, uint32_t specular, float u, float v) {
    jpt_render_vertex_t out;
    out.sx = x;
    out.sy = y;
    out.sz = z;
    out.rhw = 1.0f;
    out.diffuse = diffuse;
    out.specular = specular;
    out.tu = u;
    out.tv = v;
    return out;
}

static jpt_render_state_t default_state(void) {
    jpt_render_state_t s;
    memset(&s, 0, sizeof s);
    s.z_enable = 1;
    s.z_write = 1;
    s.z_func = 7; /* D3DCMP_GREATEREQUAL */
    s.cull_mode = 1; /* D3DCULL_NONE */
    s.fill_mode = 3; /* D3DFILL_SOLID */
    s.shade_mode = 2; /* D3DSHADE_GOURAUD */
    s.tex_map_blend = 2; /* D3DTBLEND_MODULATE */
    return s;
}

static const uint8_t* px_at(const uint8_t* rgba, uint32_t w, uint32_t x, uint32_t y) {
    return rgba + ((size_t)y * w + x) * 4u;
}

static int near_rgb(const uint8_t* p, int r, int g, int b, int tol) {
    return abs((int)p[0] - r) <= tol && abs((int)p[1] - g) <= tol && abs((int)p[2] - b) <= tol;
}

static void test_vk_log(const char* msg, void* user) {
    (void)user;
    fprintf(stderr, "[vk] %s\n", msg ? msg : "");
}

static void run_offscreen_parity_fixtures(void) {
    const uint32_t W = 64, H = 64;
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    uint8_t rgba[64 * 64 * 4];
    uint16_t rgb565[64 * 64];
    uint32_t rw = 0, rh = 0;

    memset(&desc, 0, sizeof desc);
    desc.window = NULL; /* headless offscreen */
    desc.log_fn = test_vk_log;
    desc.cfg.guest_w = W;
    desc.cfg.guest_h = H;
    desc.cfg.internal_w = W;
    desc.cfg.internal_h = H;
    desc.cfg.window_w = W;
    desc.cfg.window_h = H;
    desc.cfg.msaa = 1;
    desc.cfg.texture_filter = JPT_FILTER_BILINEAR;
    desc.cfg.vsync = 1;
    desc.cfg.water_refraction = 1;
    desc.cfg.enhanced_sky = 1;
    desc.cfg.ao_level = JPT_AO_OFF;

    jpt_vk_renderer_t* r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "create headless Vulkan renderer failed: %s", err);
    if (!r) return;

    /* 1. Opaque textured quad (RGB565 green texture) */
    uint16_t green_565[4] = { 0x07E0u, 0x07E0u, 0x07E0u, 0x07E0u };
    jpt_render_tex_desc_t td;
    memset(&td, 0, sizeof td);
    td.width = 2;
    td.height = 2;
    td.format = JPT_TEXFMT_RGB565;
    uint64_t tex_green = jpt_vk_renderer_create_texture(r, &td, green_565);
    CHECK(tex_green != 0, "create RGB565 texture");

    jpt_render_vertex_t full_quad[4] = {
        make_v(0, 0, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 0, 0),
        make_v(64, 0, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 1, 1),
        make_v(0, 64, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 0, 1),
    };

    jpt_render_state_t st = default_state();
    st.has_texture = 1;
    st.texture_id = tex_green;

    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 1");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 1");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba) && rw == W && rh == H, "readback 1");
    CHECK(near_rgb(px_at(rgba, W, 32, 32), 0, 255, 0, 4), "opaque textured quad green (got %u,%u,%u)",
          px_at(rgba, W, 32, 32)[0], px_at(rgba, W, 32, 32)[1], px_at(rgba, W, 32, 32)[2]);

    /* 2. Alpha-test foliage (ARGB1555 half transparent / half opaque red) */
    uint16_t foliage[4] = { 0x0000u, 0xFC00u, 0x0000u, 0xFC00u }; /* left transparent, right opaque red */
    td.format = JPT_TEXFMT_ARGB1555;
    uint64_t tex_foliage = jpt_vk_renderer_create_texture(r, &td, foliage);
    CHECK(tex_foliage != 0, "create ARGB1555 foliage texture");
    st = default_state();
    st.has_texture = 1;
    st.texture_id = tex_foliage;
    st.tex_min_filter = 1;
    st.tex_mag_filter = 1;
    st.alpha_test = 1;
    st.alpha_func = 5; /* GREATER */
    st.alpha_ref = 128;

    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 2");
    jpt_vk_renderer_clear(r, 1, 0xFF0000FFu, 1, 0.0f, 1, 0); /* blue background */
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 2");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback 2");
    CHECK(near_rgb(px_at(rgba, W, 16, 32), 0, 0, 255, 4), "alpha-test left discarded -> blue");
    CHECK(near_rgb(px_at(rgba, W, 48, 32), 255, 0, 0, 4), "alpha-test right kept -> red");

    /* 3. Alpha blend (50% red over blue -> ~128, 0, 128) + Z-overlap (0.8 occludes 0.3) */
    st = default_state();
    st.alpha_blend = 1;
    st.src_blend = 5; /* SRCALPHA */
    st.dst_blend = 6; /* INVSRCALPHA */
    jpt_render_vertex_t blend_quad[4] = {
        make_v(0, 0, 0.8f, 0x80FF0000u, 0xFF000000u, 0, 0),
        make_v(64, 0, 0.8f, 0x80FF0000u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.8f, 0x80FF0000u, 0xFF000000u, 1, 1),
        make_v(0, 64, 0.8f, 0x80FF0000u, 0xFF000000u, 0, 1),
    };
    jpt_render_vertex_t far_quad[4] = {
        make_v(0, 0, 0.3f, 0xFF00FF00u, 0xFF000000u, 0, 0),
        make_v(64, 0, 0.3f, 0xFF00FF00u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.3f, 0xFF00FF00u, 0xFF000000u, 1, 1),
        make_v(0, 64, 0.3f, 0xFF00FF00u, 0xFF000000u, 0, 1),
    };
    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 3");
    jpt_vk_renderer_clear(r, 1, 0xFF0000FFu, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, blend_quad, 4, NULL, NULL, 0);
    st.alpha_blend = 0;
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, far_quad, 4, NULL, NULL, 0); /* behind z=0.8: must fail Z */
    CHECK(jpt_vk_renderer_end_and_present(r), "present 3");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback 3");
    CHECK(near_rgb(px_at(rgba, W, 32, 32), 128, 0, 127, 8), "alpha blend + Z overlap (got %u,%u,%u)",
          px_at(rgba, W, 32, 32)[0], px_at(rgba, W, 32, 32)[1], px_at(rgba, W, 32, 32)[2]);

    /* 4. Fog + Specular + Vertex Color */
    st = default_state();
    st.fog_enable = 1;
    st.fog_color = 0x00808080u;
    st.specular_enable = 1;
    jpt_render_vertex_t fog_quad[4] = {
        make_v(0, 0, 0.5f, 0xFFFF0000u, 0x00004000u, 0, 0), /* fog factor = 0 -> full fog gray */
        make_v(64, 0, 0.5f, 0xFFFF0000u, 0x00004000u, 1, 0),
        make_v(64, 64, 0.5f, 0xFFFF0000u, 0x00004000u, 1, 1),
        make_v(0, 64, 0.5f, 0xFFFF0000u, 0x00004000u, 0, 1),
    };
    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 4");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, fog_quad, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 4");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback 4");
    CHECK(near_rgb(px_at(rgba, W, 32, 32), 128, 128, 128, 6), "vertex fog blends to fog color");

    /* 5. Water refraction displaces scene_behind */
    float waves[4 * 2] = { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };
    jpt_vk_renderer_set_water_params(r, 0.35f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, waves, 2, 2);
    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 5");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    /* Left half red, right half blue */
    jpt_render_vertex_t left_q[4] = {
        make_v(0, 0, 0.4f, 0xFFFF0000u, 0xFF000000u, 0, 0), make_v(32, 0, 0.4f, 0xFFFF0000u, 0xFF000000u, 1, 0),
        make_v(32, 64, 0.4f, 0xFFFF0000u, 0xFF000000u, 1, 1), make_v(0, 64, 0.4f, 0xFFFF0000u, 0xFF000000u, 0, 1),
    };
    jpt_render_vertex_t right_q[4] = {
        make_v(32, 0, 0.4f, 0xFF0000FFu, 0xFF000000u, 0, 0), make_v(64, 0, 0.4f, 0xFF0000FFu, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.4f, 0xFF0000FFu, 0xFF000000u, 1, 1), make_v(32, 64, 0.4f, 0xFF0000FFu, 0xFF000000u, 0, 1),
    };
    st = default_state();
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, left_q, 4, NULL, NULL, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, right_q, 4, NULL, NULL, 0);
    /* Transparent water shifts left side sample to the right (sampling blue instead of red) */
    st.water_mode = 1;
    jpt_render_vertex_t water_q[4] = {
        make_v(0, 0, 0.6f, 0x00000000u, 0xFF000000u, 0, 0), make_v(64, 0, 0.6f, 0x00000000u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.6f, 0x00000000u, 0xFF000000u, 1, 1), make_v(0, 64, 0.6f, 0x00000000u, 0xFF000000u, 0, 1),
    };
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, water_q, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 5");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback 5");
    CHECK(px_at(rgba, W, 24, 32)[2] > 180, "water refraction shifted blue from right onto x=24 (got %u,%u,%u)",
          px_at(rgba, W, 24, 32)[0], px_at(rgba, W, 24, 32)[1], px_at(rgba, W, 24, 32)[2]);

    /* 6. Enhanced Sky + Host Overlay order + RGB565 readback */
    jpt_vk_renderer_set_sky_params(r, 0.5f, 0.5f, 0.25f, 0.2f, 0.8f);
    st = default_state();
    st.sky_mode = 1;
    st.fog_enable = 1;
    st.fog_color = 0x00FFFFFFu; /* white horizon fog */
    jpt_render_vertex_t sky_q[4] = {
        make_v(0, 0, 0.1f, 0xFF0040C0u, 0xFF000000u, 0, 0), make_v(64, 0, 0.1f, 0xFF0040C0u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.1f, 0xFF0040C0u, 0xFF000000u, 1, 1), make_v(0, 64, 0.1f, 0xFF0040C0u, 0xFF000000u, 0, 1),
    };
    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 6");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, sky_q, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 6");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback 6");
    /* Center (zenith uv=0.5,0.5) stays blue; corner (horizon uv=0,0) gets whiter from sky fog */
    CHECK(px_at(rgba, W, 2, 2)[0] > px_at(rgba, W, 32, 32)[0] + 40,
          "Enhanced Sky fogs horizon (%u) more than zenith (%u)", px_at(rgba, W, 2, 2)[0], px_at(rgba, W, 32, 32)[0]);
    CHECK(jpt_vk_renderer_readback_rgb565(r, rgb565, W, H), "RGB565 readback succeeds");

    /* 7. AO pass on depth step + overlay compositing */
    jpt_vk_renderer_set_ao_params(r, JPT_AO_MEDIUM, 0.577f, 1);
    st = default_state();
    CHECK(jpt_vk_renderer_begin_frame(r), "begin_frame 7");
    jpt_vk_renderer_clear(r, 1, 0xFFFFFFFFu, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "present 7");
    jpt_vk_renderer_stats_t stats;
    jpt_vk_renderer_get_stats(r, &stats);
    CHECK(stats.ao_passes >= 1 && stats.water_batches >= 1 && stats.sky_batches >= 1,
          "stats tracked AO/water/sky (%llu/%llu/%llu)", (unsigned long long)stats.ao_passes,
          (unsigned long long)stats.water_batches, (unsigned long long)stats.sky_batches);
    CHECK(stats.water_draws >= 1 && stats.sky_draws >= 1 && stats.pipelines_created >= 3,
          "water and sky ran their own pipelines (%llu water draws, %llu sky draws, %llu pipelines)",
          (unsigned long long)stats.water_draws, (unsigned long long)stats.sky_draws,
          (unsigned long long)stats.pipelines_created);

    jpt_vk_renderer_destroy_texture(r, tex_green);
    jpt_vk_renderer_destroy_texture(r, tex_foliage);

    /* Snapshot lifetime right before final teardown is not 0; verify full destroy succeeds cleanly */
    jpt_vk_renderer_destroy(r);

    /* Verify standalone context + swapchain create/destroy leaves 0 logical leaks */
    jpt_vk_context_t ctx;
    jpt_vk_context_desc_t cdesc;
    jpt_vk_swapchain_t sc;
    memset(&cdesc, 0, sizeof cdesc);
    if (jpt_vk_context_init(&ctx, &cdesc)) {
        CHECK(jpt_vk_swapchain_init(&sc, &ctx, 64, 64, 1, 0), "swapchain init for leak check");
        CHECK(jpt_vk_swapchain_recreate(&sc, 128, 96, 0, 0), "swapchain resize for leak check");
        jpt_vk_swapchain_shutdown(&sc);
        CHECK(jpt_vk_lifetime_is_clean(&ctx.lifetime), "0 leaked Vulkan objects after swapchain shutdown (total=%d)",
              jpt_vk_lifetime_total_objects(&ctx.lifetime));
        jpt_vk_context_shutdown(&ctx);
    }
}

static jpt_vk_renderer_t* open_renderer(uint32_t w, uint32_t h, uint32_t msaa) {
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    jpt_vk_renderer_t* r;
    memset(&desc, 0, sizeof desc);
    desc.log_fn = test_vk_log;
    desc.enable_validation = 1;
    desc.collect_timings = 1;
    desc.cfg.guest_w = w;
    desc.cfg.guest_h = h;
    desc.cfg.internal_w = w;
    desc.cfg.internal_h = h;
    desc.cfg.window_w = w;
    desc.cfg.window_h = h;
    desc.cfg.msaa = msaa;
    desc.cfg.texture_filter = JPT_FILTER_ORIGINAL;
    desc.cfg.vsync = 1;
    desc.cfg.ao_level = JPT_AO_OFF;
    desc.cfg.water_refraction = 1;
    desc.cfg.enhanced_sky = 1;
    r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "create renderer %ux%u msaa %u: %s", w, h, msaa, err);
    return r;
}

static void fill_square(uint16_t* texels, uint32_t n, uint16_t value) {
    for (uint32_t i = 0; i < n * n; i++) texels[i] = value;
}

static void set_quad(jpt_render_vertex_t* q, float x0, float y0, float x1, float y1, float z, uint32_t c) {
    q[0] = make_v(x0, y0, z, c, 0xFF000000u, 0, 0);
    q[1] = make_v(x1, y0, z, c, 0xFF000000u, 1, 0);
    q[2] = make_v(x1, y1, z, c, 0xFF000000u, 1, 1);
    q[3] = make_v(x0, y1, z, c, 0xFF000000u, 0, 1);
}


static void draw_fill(jpt_vk_renderer_t* r, uint32_t color) {
    jpt_render_vertex_t quad[4];
    jpt_render_state_t st = default_state();
    set_quad(quad, 0, 0, 64, 64, 0.5f, color);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
}

static uint16_t rgb565_px(const uint16_t* p, uint32_t x, uint32_t y) { return p[y * 64u + x]; }

/* The last presented frame stays readable while the next frame is open, after a mid-frame submit, and after a resize
 * of the window; frames that submitted early still report GPU time. */
static void run_last_frame_and_timing_fixtures(void) {
    uint16_t pix[64 * 64];
    uint8_t rgba[64 * 64 * 4];
    uint32_t rw, rh;
    jpt_vk_renderer_t* r = open_renderer(64, 64, 1);
    jpt_vk_renderer_stats_t st0, st1;
    jpt_render_rect_t rect = { 0, 0, 16, 16 };
    uint16_t reads[16 * 16];

    if (!r) return;
    CHECK(!jpt_vk_renderer_last_frame_valid(r), "no last frame before the first present");
    CHECK(!jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64), "nothing to read back before the first present");

    CHECK(jpt_vk_renderer_begin_frame(r), "first frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    draw_fill(r, 0xFFFF0000u);
    CHECK(jpt_vk_renderer_end_and_present(r), "first frame presented");
    CHECK(jpt_vk_renderer_last_frame_valid(r), "the presented frame is kept");

    CHECK(jpt_vk_renderer_begin_frame(r), "second frame opens");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    draw_fill(r, 0xFF0000FFu);
    CHECK(jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64),
          "the last frame reads back while the next one is being drawn");
    CHECK(rgb565_px(pix, 32, 32) == 0xF800, "it is the presented frame, not the one being drawn (0x%04X)", rgb565_px(pix, 32, 32));
    CHECK(jpt_vk_renderer_sync_frame(r), "mid-frame submit");
    CHECK(jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64) && rgb565_px(pix, 8, 8) == 0xF800,
          "still the presented frame after a mid-frame submit");
    CHECK(jpt_vk_renderer_end_and_present(r), "second frame presented");
    CHECK(jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64) && rgb565_px(pix, 32, 32) == 0x001F,
          "after the second present the last frame is the second (0x%04X)", rgb565_px(pix, 32, 32));
    CHECK(jpt_vk_renderer_read_scene_rgb565 != NULL, "mid-frame readback entry point exists");

    jpt_vk_renderer_on_resize(r, 96, 72);
    CHECK(jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64) && rgb565_px(pix, 32, 32) == 0x001F,
          "a window resize does not touch the kept frame");
    CHECK(jpt_vk_renderer_begin_frame(r), "frame after the resize");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    draw_fill(r, 0xFF00FF00u);
    CHECK(jpt_vk_renderer_end_and_present(r), "frame after the resize presented");
    CHECK(jpt_vk_renderer_read_last_frame_rgb565(r, pix, 64, 64, 64) && rgb565_px(pix, 32, 32) == 0x07E0,
          "the frame after the resize is kept");
    (void)rect;
    (void)reads;
    (void)rgba;
    (void)rw;
    (void)rh;

    jpt_vk_renderer_get_stats(r, &st0);
    for (int f = 0; f < 10; f++) {
        CHECK(jpt_vk_renderer_begin_frame(r), "timing frame %d", f);
        jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
        draw_fill(r, 0xFF336699u);
        jpt_vk_renderer_sync_frame(r);
        draw_fill(r, 0xFF996633u);
        jpt_vk_renderer_sync_frame(r);
        draw_fill(r, 0xFF339966u);
        CHECK(jpt_vk_renderer_end_and_present(r), "timing frame %d presented", f);
    }
    for (int f = 0; f < 3; f++) {
        jpt_vk_renderer_begin_frame(r);
        jpt_vk_renderer_end_and_present(r);
    }
    jpt_vk_renderer_get_stats(r, &st1);
    CHECK(st1.frame_syncs - st0.frame_syncs == 20, "twenty mid-frame submits (%llu)", (unsigned long long)(st1.frame_syncs - st0.frame_syncs));
    CHECK(st1.gpu_samples - st0.gpu_samples >= 10 && st1.gpu_frames_unsampled == st0.gpu_frames_unsampled,
          "frames that submitted early still report GPU time (%llu samples, %llu unsampled)",
          (unsigned long long)(st1.gpu_samples - st0.gpu_samples),
          (unsigned long long)(st1.gpu_frames_unsampled - st0.gpu_frames_unsampled));
    CHECK(st1.gpu_total_ms > st0.gpu_total_ms && st1.gpu_scene_ms > st0.gpu_scene_ms, "their GPU time is not zero (%.3f ms)",
          st1.gpu_total_ms - st0.gpu_total_ms);
    jpt_vk_renderer_destroy(r);
}


static void fill_box_quad(jpt_render_vertex_t* q, float x0, float y0, float x1, float y1, float z) {
    set_quad(q, x0, y0, x1, y1, z, 0xFFFFFFFFu);
}

/* Ambient occlusion darkens the depth-written 3D pixels beside a near box and leaves the pixels the CPU wrote untouched,
 * as the Direct3D 11 stencil mask does. */
static void run_ao_cpu_mask_fixture(void) {
    uint8_t rgba[64 * 64 * 4];
    uint32_t rw, rh, cpu[16 * 24];
    jpt_vk_renderer_t* r = open_renderer(64, 64, 1);
    jpt_render_state_t st;
    jpt_render_vertex_t far_q[4], near_q[4];
    jpt_render_rect_t box = { 24, 8, 16, 24 };
    jpt_vk_renderer_stats_t s;
    int gpu_dark, cpu_clean;
    if (!r) return;
    for (int i = 0; i < 16 * 24; i++) cpu[i] = 0xFFFFFFFFu;
    st = default_state();
    fill_box_quad(far_q, 0, 0, 64, 64, 0.2f);
    fill_box_quad(near_q, 8, 8, 24, 56, 0.8f);
    jpt_vk_renderer_set_ao_params(r, JPT_AO_MEDIUM, 0.577f, 1);
    CHECK(jpt_vk_renderer_begin_frame(r), "ao mask frame");
    jpt_vk_renderer_clear(r, 1, 0xFFFFFFFFu, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, far_q, 4, NULL, NULL, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, near_q, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_draw_overlay(r, cpu, 16, &box, 64, 64, 0), "CPU pixels composited over the scene");
    CHECK(jpt_vk_renderer_end_and_present(r), "ao mask frame presented");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "ao mask readback");
    jpt_vk_renderer_get_stats(r, &s);
    CHECK(s.ao_passes >= 1, "the ambient occlusion pass ran (%llu)", (unsigned long long)s.ao_passes);
    gpu_dark = px_at(rgba, 64, 26, 48)[0] < 245;
    cpu_clean = px_at(rgba, 64, 26, 20)[0] >= 253 && px_at(rgba, 64, 30, 20)[0] >= 253;
    CHECK(gpu_dark, "the depth-written pixel beside the box is darkened (%u)", px_at(rgba, 64, 26, 48)[0]);
    CHECK(cpu_clean, "the CPU pixel at the same distance is not (%u, %u)", px_at(rgba, 64, 26, 20)[0],
          px_at(rgba, 64, 30, 20)[0]);
    jpt_vk_renderer_destroy(r);
}


static void ao_poly_constants(jpt_vk_poly_constants_t* c, float x0, float y0, float x1, float y1) {
    memset(c, 0, sizeof *c);
    c->xy[0][0] = x0; c->xy[0][1] = y0; c->xy[0][2] = x1; c->xy[0][3] = y0;
    c->xy[1][0] = x1; c->xy[1][1] = y1; c->xy[1][2] = x0; c->xy[1][3] = y1;
    c->u[1] = 1.0f;
    c->v[2] = 1.0f;
    c->w[0] = 1.0f;
    c->origin[0] = x0;
    c->origin[1] = y0;
    c->size[0] = c->size[1] = 64.0f;
    c->size[2] = c->size[3] = 1.0f;
    c->tex[2] = 63;
    c->tex[3] = 0xFFF;
}

/* The same mask for a colour fill and for a polygon the GPU draws in place of the CPU's. */
static void run_ao_fill_and_polygon_fixture(void) {
    uint8_t rgba[64 * 64 * 4], texels[4096];
    uint16_t clut[256];
    uint32_t rw, rh;
    jpt_vk_renderer_t* r = open_renderer(64, 64, 1);
    jpt_render_state_t st = default_state();
    jpt_render_vertex_t far_q[4], near_q[4];
    jpt_vk_poly_constants_t pc;
    if (!r) return;
    memset(texels, 0, sizeof texels);
    for (int i = 0; i < 256; i++) clut[i] = 0xFFFF;
    fill_box_quad(far_q, 0, 0, 64, 64, 0.2f);
    fill_box_quad(near_q, 8, 8, 24, 56, 0.8f);
    jpt_vk_renderer_set_ao_params(r, JPT_AO_MEDIUM, 0.577f, 1);
    CHECK(jpt_vk_renderer_begin_frame(r), "ao fill frame");
    jpt_vk_renderer_clear(r, 1, 0xFFFFFFFFu, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, far_q, 4, NULL, NULL, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, near_q, 4, NULL, NULL, 0);
    jpt_vk_renderer_set_scissor(r, 1, 0, 8, 8, 24);
    jpt_vk_renderer_clear(r, 1, 0xFFFFFFFFu, 0, 0.0f, 0, 0);
    jpt_vk_renderer_set_scissor(r, 0, 0, 0, 0, 0);
    ao_poly_constants(&pc, 24.0f, 36.0f, 40.0f, 56.0f);
    CHECK(jpt_vk_renderer_poly_ready(r) && jpt_vk_renderer_poly_upload(r, JPT_VK_POLY_TEXELS, 0, texels, sizeof texels) &&
              jpt_vk_renderer_poly_upload(r, JPT_VK_POLY_CLUT, 0, clut, sizeof clut),
          "GPU polygon resources");
    CHECK(jpt_vk_renderer_poly_draw(r, 0, &pc, 4), "a GPU polygon beside the box");
    CHECK(jpt_vk_renderer_end_and_present(r), "ao fill frame presented");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "ao fill readback");
    CHECK(px_at(rgba, 64, 6, 50)[0] < 245, "the depth-written reference pixel is darkened (%u)", px_at(rgba, 64, 6, 50)[0]);
    CHECK(px_at(rgba, 64, 6, 20)[0] >= 253, "a colour fill is not darkened (%u)", px_at(rgba, 64, 6, 20)[0]);
    CHECK(px_at(rgba, 64, 26, 48)[0] >= 253, "a GPU polygon is not darkened (%u)", px_at(rgba, 64, 26, 48)[0]);
    jpt_vk_renderer_destroy(r);
}

/* The paths the game takes at run time and the offscreen fixtures above skip: textures created while a frame is
 * open, new texture pools mid-frame, batching, a mid-frame readback and an overlay, video layers that change. */
static void run_runtime_path_fixtures(uint32_t msaa) {
    const uint32_t W = 64, H = 64;
    uint8_t rgba[64 * 64 * 4];
    uint32_t rw, rh;
    jpt_vk_renderer_t* r = open_renderer(W, H, msaa);
    jpt_vk_renderer_stats_t st0, st1;
    jpt_render_state_t st;
    jpt_render_tex_desc_t td;
    static uint16_t texels[64 * 64];
    jpt_render_vertex_t quad[4];
    uint64_t ids[6];
    int pass_validation;
    static const uint16_t colours[6] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF };

    if (!r) return;
    memset(&td, 0, sizeof td);
    td.format = JPT_TEXFMT_RGB565;
    st = default_state();
    st.has_texture = 1;
    st.tex_min_filter = 1;
    st.tex_mag_filter = 1;

    jpt_vk_renderer_get_stats(r, &st0);
    CHECK(jpt_vk_renderer_begin_frame(r), "frame open for in-frame uploads");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    for (int i = 0; i < 6; i++) {
        uint32_t size = 2u << i;
        float x0 = (float)(i % 3) * 20.0f + 2.0f, y0 = (float)(i / 3) * 30.0f + 2.0f;
        td.width = td.height = size;
        fill_square(texels, size, colours[i]);
        ids[i] = jpt_vk_renderer_create_texture(r, &td, texels);
        CHECK(ids[i] != 0, "texture %d created while the frame is open (%s)", i,
              jpt_vk_texture_error_name(jpt_vk_renderer_texture_error(r)));
        set_quad(quad, x0, y0, x0 + 16, y0 + 16, 0.5f, 0xFFFFFFFFu);
        st.texture_id = ids[i];
        CHECK(jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0) == JPT_VK_DRAW_SUBMITTED,
              "draw with texture %d submitted", i);
    }
    CHECK(jpt_vk_renderer_end_and_present(r), "present in-frame upload frame");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "readback in-frame upload frame");
    CHECK(near_rgb(px_at(rgba, W, 10, 10), 255, 0, 0, 4), "texture 0 (red) at its square (got %u,%u,%u)",
          px_at(rgba, W, 10, 10)[0], px_at(rgba, W, 10, 10)[1], px_at(rgba, W, 10, 10)[2]);
    CHECK(near_rgb(px_at(rgba, W, 30, 10), 0, 255, 0, 4), "texture 1 (green), a new pool created mid-frame");
    CHECK(near_rgb(px_at(rgba, W, 50, 10), 0, 0, 255, 4), "texture 2 (blue)");
    CHECK(near_rgb(px_at(rgba, W, 10, 40), 255, 255, 0, 4), "texture 3 (yellow)");
    CHECK(near_rgb(px_at(rgba, W, 30, 40), 255, 0, 255, 4), "texture 4 (magenta)");
    CHECK(near_rgb(px_at(rgba, W, 50, 40), 0, 255, 255, 4), "texture 5 (cyan)");
    jpt_vk_renderer_get_stats(r, &st1);
    CHECK(st1.textures_created - st0.textures_created == 6 && st1.draws - st0.draws == 6, "six textures, six draws");
    CHECK(st1.descriptor_updates > st0.descriptor_updates, "new pools re-describe the world set (%llu updates)",
          (unsigned long long)(st1.descriptor_updates - st0.descriptor_updates));

    /* a texture slot freed and reused inside one frame: the draw before the reuse keeps the old texture */
    td.width = td.height = 8;
    fill_square(texels, 8, 0xF800);
    CHECK(jpt_vk_renderer_begin_frame(r), "reuse frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    {
        uint64_t a = jpt_vk_renderer_create_texture(r, &td, texels), b;
        st.texture_id = a;
        set_quad(quad, 0, 0, 32, 32, 0.5f, 0xFFFFFFFFu);
        jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
        jpt_vk_renderer_destroy_texture(r, a);
        fill_square(texels, 8, 0x07E0);
        b = jpt_vk_renderer_create_texture(r, &td, texels);
        st.texture_id = b;
        set_quad(quad, 32, 0, 64, 32, 0.5f, 0xFFFFFFFFu);
        jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
    }
    CHECK(jpt_vk_renderer_end_and_present(r), "present reuse frame");
    jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
    CHECK(near_rgb(px_at(rgba, W, 16, 16), 255, 0, 0, 4), "the draw before the slot was reused still shows the old texture");
    CHECK(near_rgb(px_at(rgba, W, 48, 16), 0, 255, 0, 4), "the draw after it shows the new one");

    /* batching */
    jpt_vk_renderer_get_stats(r, &st0);
    CHECK(jpt_vk_renderer_begin_frame(r), "batch frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    st = default_state();
    for (int i = 0; i < 64; i++) {
        float x = (float)(i % 8) * 8.0f, y = (float)(i / 8) * 8.0f;
        set_quad(quad, x, y, x + 7, y + 7, 0.5f, (i & 1) ? 0xFFFF0000u : 0xFF0000FFu);
        jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
    }
    CHECK(jpt_vk_renderer_end_and_present(r), "present batch frame");
    jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
    jpt_vk_renderer_get_stats(r, &st1);
    CHECK(st1.draws - st0.draws == 64 && st1.draw_calls_recorded - st0.draw_calls_recorded == 1,
          "64 draws of one state are one draw call (%llu)",
          (unsigned long long)(st1.draw_calls_recorded - st0.draw_calls_recorded));
    CHECK(near_rgb(px_at(rgba, W, 3, 3), 0, 0, 255, 3) && near_rgb(px_at(rgba, W, 11, 3), 255, 0, 0, 3) &&
              near_rgb(px_at(rgba, W, 59, 59), 255, 0, 0, 3),
          "draws merged into one call keep their own colours and positions");

    /* water and sky textures from any texture pool: the effect shaders once knew only the first seven */
    {
        static const uint32_t dims[11][2] = { { 4, 4 },  { 4, 8 },  { 8, 4 },  { 8, 8 },  { 4, 16 }, { 16, 4 },
                                              { 8, 16 }, { 16, 8 }, { 16, 16 }, { 4, 32 }, { 32, 4 } };
        uint64_t last = 0;
        float waves[2 * 2 * 2] = { 0 };
        jpt_vk_renderer_stats_t w0, w1;
        CHECK(jpt_vk_renderer_begin_frame(r), "effect-texture frame");
        jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
        for (int i = 0; i < 11; i++) {
            td.width = dims[i][0];
            td.height = dims[i][1];
            for (uint32_t k = 0; k < td.width * td.height; k++) texels[k] = 0x07E0;
            last = jpt_vk_renderer_create_texture(r, &td, texels);
            CHECK(last != 0, "effect texture %d created", i);
        }
        jpt_vk_renderer_set_water_params(r, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, waves, 2, 2);
        jpt_vk_renderer_set_sky_params(r, 0.5f, 0.5f, 0.25f, 0.2f, 0.8f);
        jpt_vk_renderer_get_stats(r, &w0);
        st = default_state();
        st.has_texture = 1;
        st.texture_id = last;
        st.water_mode = 1;
        set_quad(quad, 0, 0, 32, 64, 0.5f, 0xFFFFFFFFu);
        CHECK(jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0) == JPT_VK_DRAW_SUBMITTED,
              "water draw with a late pool's texture");
        st.water_mode = 0;
        st.sky_mode = 1;
        set_quad(quad, 32, 0, 64, 64, 0.5f, 0xFFFFFFFFu);
        CHECK(jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0) == JPT_VK_DRAW_SUBMITTED,
              "sky draw with a late pool's texture");
        CHECK(jpt_vk_renderer_end_and_present(r), "present effect-texture frame");
        jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
        jpt_vk_renderer_get_stats(r, &w1);
        CHECK(near_rgb(px_at(rgba, W, 16, 32), 0, 255, 0, 6),
              "a water draw shows its texture from the eleventh pool, not the white fallback (got %u,%u,%u)",
              px_at(rgba, W, 16, 32)[0], px_at(rgba, W, 16, 32)[1], px_at(rgba, W, 16, 32)[2]);
        CHECK(near_rgb(px_at(rgba, W, 48, 32), 0, 255, 0, 40),
              "a sky draw shows its texture from the eleventh pool, not the white fallback (got %u,%u,%u)",
              px_at(rgba, W, 48, 32)[0], px_at(rgba, W, 48, 32)[1], px_at(rgba, W, 48, 32)[2]);
        CHECK(w1.water_draws > w0.water_draws && w1.sky_draws > w0.sky_draws, "water and sky ran their pipelines");
    }

    /* the viewport clips and never rescales pre-transformed vertices */
    CHECK(jpt_vk_renderer_begin_frame(r), "viewport frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_set_viewport(r, 16, 16, 32, 32);
    st = default_state();
    set_quad(quad, 0, 0, 64, 64, 0.4f, 0xFFFF0000u);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
    set_quad(quad, 20, 20, 28, 28, 0.6f, 0xFF00FF00u);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
    jpt_vk_renderer_set_viewport(r, 0, 0, W, H);
    CHECK(jpt_vk_renderer_end_and_present(r), "present viewport frame");
    jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
    CHECK(near_rgb(px_at(rgba, W, 40, 40), 255, 0, 0, 4) && near_rgb(px_at(rgba, W, 8, 8), 0, 0, 0, 2),
          "a smaller viewport clips the draw");
    CHECK(near_rgb(px_at(rgba, W, 24, 24), 0, 255, 0, 4), "vertices keep their render-target pixels inside the viewport");

    /* mid-frame readback and an overlay */
    CHECK(jpt_vk_renderer_begin_frame(r), "readback frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    st = default_state();
    set_quad(quad, 0, 0, 32, 64, 0.5f, 0xFF00FF00u);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0);
    {
        uint16_t pixels[16 * 16];
        uint32_t overlay[8 * 8];
        jpt_render_rect_t box = { 16, 16, 16, 16 }, obox = { 40, 40, 8, 8 };
        CHECK(jpt_vk_renderer_read_scene_rgb565(r, &box, W, H, pixels, 16), "scene box read back mid-frame");
        CHECK(pixels[0] == 0x07E0 && pixels[15] == 0x07E0 && pixels[16 * 15] == 0x07E0,
              "the box holds the green drawn before it (0x%04X)", pixels[0]);
        for (int i = 0; i < 64; i++) overlay[i] = 0xFFFF0000u;
        CHECK(jpt_vk_renderer_draw_overlay(r, overlay, 8, &obox, W, H, 1), "overlay drawn into the open frame");
        set_quad(quad, 32, 0, 40, 8, 0.5f, 0xFF0000FFu);
        CHECK(jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, quad, 4, NULL, NULL, 0) == JPT_VK_DRAW_SUBMITTED,
              "a draw after the readback records");
    }
    CHECK(jpt_vk_renderer_end_and_present(r), "present readback frame");
    jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
    CHECK(near_rgb(px_at(rgba, W, 8, 8), 0, 255, 0, 4), "geometry before the readback survives it");
    CHECK(near_rgb(px_at(rgba, W, 44, 44), 255, 0, 0, 4), "the overlay replaced its box");
    CHECK(near_rgb(px_at(rgba, W, 36, 4), 0, 0, 255, 4), "geometry after the readback is drawn");
    jpt_vk_renderer_get_stats(r, &st1);
    CHECK(st1.scene_readbacks >= 1 && st1.overlays_drawn >= 1 && st1.frame_syncs >= 1,
          "readback, overlay and sync are counted");

    /* video layers: the same layer re-uploaded with a block that moves, at three picture heights */
    {
        static const uint32_t heights[3] = { 35, 34, 48 };
        for (int g = 0; g < 3; g++) {
            uint32_t lh = heights[g];
            uint8_t layer[64 * 48 * 4];
            for (int step = 0; step < 4; step++) {
                float rect[4] = { 0.0f, 0.0f, 1.0f, (float)lh / (float)H };
                int bx = step * 12 + 2, by = (int)(lh / 2) - 4, bad = 0;
                for (uint32_t y = 0; y < lh; y++)
                    for (uint32_t x = 0; x < 64; x++) {
                        uint8_t* p = layer + ((size_t)y * 64 + x) * 4;
                        int inside = (int)x >= bx && (int)x < bx + 8 && (int)y >= by && (int)y < by + 8;
                        p[0] = inside ? 255 : (uint8_t)(x * 3);
                        p[1] = inside ? 255 : (uint8_t)(y * 5);
                        p[2] = inside ? 0 : (uint8_t)((x + y) * 2);
                        p[3] = 255;
                    }
                CHECK(jpt_vk_renderer_begin_frame(r), "video frame");
                jpt_vk_renderer_clear(r, 1, 0xFF000000u, 0, 0.0f, 0, 0);
                jpt_vk_renderer_submit_layer_rect_rgba(r, JPT_LAYER_VIDEO, layer, 64, lh, 1, 0, rect);
                CHECK(jpt_vk_renderer_end_and_present(r), "present video frame");
                jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba);
                for (uint32_t y = 0; y < lh && !bad; y++)
                    for (uint32_t x = 0; x < 64; x++) {
                        const uint8_t* want = layer + ((size_t)y * 64 + x) * 4;
                        const uint8_t* got = px_at(rgba, W, x, y);
                        if (abs((int)want[0] - got[0]) > 2 || abs((int)want[1] - got[1]) > 2 ||
                            abs((int)want[2] - got[2]) > 2) {
                            bad = 1;
                            CHECK(0, "video 64x%u step %d differs at %u,%u: want %u,%u,%u got %u,%u,%u", lh, step, x, y,
                                  want[0], want[1], want[2], got[0], got[1], got[2]);
                            break;
                        }
                    }
                CHECK(!bad, "video layer 64x%u frame %d is exact", lh, step);
            }
            jpt_vk_renderer_clear_layer(r, JPT_LAYER_VIDEO);
        }
    }

    jpt_vk_renderer_get_stats(r, &st1);
    CHECK(st1.draw_calls_recorded > 0 && st1.pipelines_created > 0 && st1.textures_uploaded > 0,
          "the run executed GPU work: %llu draw calls, %llu pipelines, %llu uploads",
          (unsigned long long)st1.draw_calls_recorded, (unsigned long long)st1.pipelines_created,
          (unsigned long long)st1.textures_uploaded);
    pass_validation = !jpt_vk_renderer_validation_enabled(r) || jpt_vk_renderer_validation_errors(r) == 0;
    CHECK(pass_validation, "no Vulkan validation errors (%ld)", jpt_vk_renderer_validation_errors(r));
    printf("vulkan_render_tests: runtime path (msaa %u): validation %s, %ld errors, %ld warnings, GPU %.3f ms over %llu "
           "samples\n",
           msaa, jpt_vk_renderer_validation_enabled(r) ? "on" : "unavailable", jpt_vk_renderer_validation_errors(r),
           jpt_vk_renderer_validation_warnings(r),
           st1.gpu_samples ? st1.gpu_total_ms / (double)st1.gpu_samples : 0.0, (unsigned long long)st1.gpu_samples);
    jpt_vk_renderer_destroy(r);
}

static void test_texture_coverage_unlimited(void) {
    const uint32_t W = 128, H = 64;
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    uint8_t rgba[128 * 64 * 4];
    uint32_t rw = 0, rh = 0;
    uint64_t tex_ids[32];
    uint8_t want_rgb[32][3];

    memset(&desc, 0, sizeof desc);
    desc.enable_validation = 1;
    desc.log_fn = test_vk_log;
    desc.cfg.guest_w = W;
    desc.cfg.guest_h = H;
    desc.cfg.internal_w = W;
    desc.cfg.internal_h = H;
    desc.cfg.window_w = W;
    desc.cfg.window_h = H;
    desc.cfg.msaa = 1;
    desc.cfg.texture_filter = JPT_FILTER_ORIGINAL;
    desc.cfg.vsync = 1;

    jpt_vk_renderer_t* r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "texture coverage: create renderer failed: %s", err);
    if (!r) return;

    /* 32 distinct dimensions (exceeds old 15-pool cap AND 16-view per-batch cap) */
    for (int i = 0; i < 32; i++) {
        uint32_t tw = (uint32_t)(2 + i * 2);
        uint32_t th = (uint32_t)(2 + ((i * 3) % 29) * 2);
        size_t px = (size_t)tw * (size_t)th;
        uint32_t* bgra = (uint32_t*)malloc(px * 4u);
        CHECK(bgra != NULL, "alloc tex %d", i);
        uint8_t cr = (uint8_t)(40 + ((i * 37) % 200));
        uint8_t cg = (uint8_t)(40 + ((i * 73) % 200));
        uint8_t cb = (uint8_t)(40 + ((i * 113) % 200));
        want_rgb[i][0] = cr;
        want_rgb[i][1] = cg;
        want_rgb[i][2] = cb;
        uint32_t pix = 0xFF000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
        for (size_t k = 0; k < px; k++) bgra[k] = pix;

        jpt_render_tex_desc_t td;
        memset(&td, 0, sizeof td);
        td.width = tw;
        td.height = th;
        td.pitch = tw * 4u;
        td.format = JPT_TEXFMT_BGRA8888_PREPARED;
        tex_ids[i] = jpt_vk_renderer_create_texture(r, &td, bgra);
        free(bgra);
        CHECK(tex_ids[i] != 0, "create texture %d (%ux%u) failed: %s", i, tw, th,
              jpt_vk_texture_error_name(jpt_vk_renderer_texture_error(r)));
    }

    CHECK(jpt_vk_renderer_begin_frame(r), "texture coverage: begin_frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    for (int i = 0; i < 32; i++) {
        float x0 = (float)((i % 8) * 16);
        float y0 = (float)((i / 8) * 16);
        jpt_render_vertex_t q[4] = {
            make_v(x0, y0, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 0, 0),
            make_v(x0 + 16, y0, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 1, 0),
            make_v(x0 + 16, y0 + 16, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 1, 1),
            make_v(x0, y0 + 16, 0.5f, 0xFFFFFFFFu, 0xFF000000u, 0, 1),
        };
        jpt_render_state_t st = default_state();
        st.has_texture = 1;
        st.texture_id = tex_ids[i];
        jpt_vk_draw_result_t dr = jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, q, 4, NULL, NULL, 0);
        CHECK(dr == JPT_VK_DRAW_SUBMITTED, "draw 32-size tile %d submitted (got %d)", i, (int)dr);
    }
    CHECK(jpt_vk_renderer_end_and_present(r), "texture coverage: present");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba) && rw == W && rh == H,
          "texture coverage: readback");
    for (int i = 0; i < 32; i++) {
        uint32_t cx = (uint32_t)((i % 8) * 16 + 8);
        uint32_t cy = (uint32_t)((i / 8) * 16 + 8);
        const uint8_t* p = px_at(rgba, W, cx, cy);
        CHECK(near_rgb(p, want_rgb[i][0], want_rgb[i][1], want_rgb[i][2], 3),
              "tile %d color mismatch: want (%u,%u,%u) got (%u,%u,%u)", i, want_rgb[i][0], want_rgb[i][1],
              want_rgb[i][2], p[0], p[1], p[2]);
    }

    jpt_vk_renderer_stats_t st;
    jpt_vk_renderer_get_stats(r, &st);
    for (int e = 1; e < JPT_VK_TEX_ERRORS; e++)
        CHECK(st.texture_failures[e] == 0, "0 texture failures for '%s' (got %llu)",
              jpt_vk_texture_error_name((jpt_vk_texture_error_t)e), (unsigned long long)st.texture_failures[e]);
    for (int s = 1; s < JPT_VK_SKIP_REASONS; s++)
        CHECK(st.draws_skipped[s] == 0, "0 draws skipped for '%s' (got %llu)",
              jpt_vk_skip_reason_name((jpt_vk_draw_result_t)s), (unsigned long long)st.draws_skipped[s]);
    CHECK(st.batch_flushes_texture_full >= 1, "per-batch 16-view flush triggered (%llu)",
          (unsigned long long)st.batch_flushes_texture_full);
    CHECK(!jpt_vk_renderer_validation_enabled(r) || jpt_vk_renderer_validation_errors(r) == 0,
          "texture coverage: 0 validation errors (%ld)", jpt_vk_renderer_validation_errors(r));

    for (int i = 0; i < 32; i++) jpt_vk_renderer_destroy_texture(r, tex_ids[i]);
    jpt_vk_renderer_destroy(r);
}

static void test_overlay_lifetime_zero_stale_pixels(void) {
    const uint32_t W = 64, H = 64;
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    uint8_t base_rgba[64 * 64 * 4];
    uint8_t cur_rgba[64 * 64 * 4];
    uint8_t ov_rgba[64 * 64 * 4];
    uint16_t mid_rb[64 * 64];
    uint32_t rw = 0, rh = 0;

    memset(&desc, 0, sizeof desc);
    desc.enable_validation = 1;
    desc.log_fn = test_vk_log;
    desc.cfg.guest_w = W;
    desc.cfg.guest_h = H;
    desc.cfg.internal_w = W;
    desc.cfg.internal_h = H;
    desc.cfg.window_w = W;
    desc.cfg.window_h = H;
    desc.cfg.msaa = 1;
    desc.cfg.texture_filter = JPT_FILTER_BILINEAR;
    desc.cfg.vsync = 1;

    jpt_vk_renderer_t* r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "overlay lifetime: create renderer failed: %s", err);
    if (!r) return;

    jpt_render_vertex_t full_quad[4] = {
        make_v(0, 0, 0.5f, 0xFF208040u, 0xFF000000u, 0, 0),
        make_v(64, 0, 0.5f, 0xFF208040u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.5f, 0xFF208040u, 0xFF000000u, 1, 1),
        make_v(0, 64, 0.5f, 0xFF208040u, 0xFF000000u, 0, 1),
    };
    jpt_render_state_t st = default_state();

    /* Frame 1: clean base scene */
    CHECK(jpt_vk_renderer_begin_frame(r), "overlay lifetime: begin base");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    CHECK(jpt_vk_renderer_end_and_present(r), "overlay lifetime: present base");
    CHECK(jpt_vk_renderer_readback_rgba(r, base_rgba, &rw, &rh, sizeof base_rgba), "overlay lifetime: readback base");

    /* Sequence: F2 -> none -> F3 -> none -> F5 -> none -> F10 -> none */
    static const uint8_t k_ov_cols[4][3] = { { 240, 30, 30 }, { 30, 30, 240 }, { 240, 220, 20 }, { 220, 40, 220 } };
    for (int step = 0; step < 4; step++) {
        memset(ov_rgba, 0, sizeof ov_rgba);
        for (uint32_t y = 16; y < 48; y++)
            for (uint32_t x = 16; x < 48; x++) {
                uint8_t* p = ov_rgba + ((size_t)y * W + x) * 4u;
                p[0] = k_ov_cols[step][0];
                p[1] = k_ov_cols[step][1];
                p[2] = k_ov_cols[step][2];
                p[3] = 255;
            }
        jpt_vk_layer_opts_t opts = { 1, 1, 0, 1, NULL };

        /* Show overlay */
        CHECK(jpt_vk_renderer_begin_frame(r), "overlay step %d show: begin", step);
        jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
        jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
        jpt_vk_renderer_submit_layer_ex(r, JPT_LAYER_HOST_OVERLAYS, ov_rgba, W, H, &opts);

        /* Mid-frame readback MUST contain only clean scene, NEVER host overlays */
        jpt_render_rect_t box = { 0, 0, W, H };
        CHECK(jpt_vk_renderer_read_scene_rgb565(r, &box, W, H, mid_rb, W), "overlay step %d: mid readback", step);
        uint16_t mid_c = mid_rb[32 * W + 32];
        uint8_t mr = (uint8_t)(((mid_c >> 11) & 31u) * 255u / 31u);
        uint8_t mg = (uint8_t)(((mid_c >> 5) & 63u) * 255u / 63u);
        uint8_t mb = (uint8_t)((mid_c & 31u) * 255u / 31u);
        CHECK(abs((int)mr - 0x20) <= 10 && abs((int)mg - 0x80) <= 10 && abs((int)mb - 0x40) <= 10,
              "mid-frame readback uncontaminated by overlay step %d (got %u,%u,%u)", step, mr, mg, mb);

        CHECK(jpt_vk_renderer_end_and_present(r), "overlay step %d show: present", step);
        CHECK(jpt_vk_renderer_readback_rgba(r, cur_rgba, &rw, &rh, sizeof cur_rgba), "overlay step %d show: rb", step);
        CHECK(near_rgb(px_at(cur_rgba, W, 32, 32), k_ov_cols[step][0], k_ov_cols[step][1], k_ov_cols[step][2], 3),
              "overlay step %d visible", step);

        /* Hide overlay */
        CHECK(jpt_vk_renderer_begin_frame(r), "overlay step %d hide: begin", step);
        jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
        jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
        jpt_vk_renderer_clear_layer(r, JPT_LAYER_HOST_OVERLAYS);
        CHECK(jpt_vk_renderer_end_and_present(r), "overlay step %d hide: present", step);
        CHECK(jpt_vk_renderer_readback_rgba(r, cur_rgba, &rw, &rh, sizeof cur_rgba), "overlay step %d hide: rb", step);
        CHECK(memcmp(cur_rgba, base_rgba, sizeof base_rgba) == 0,
              "overlay step %d hide: 0 stale pixels (byte-identical to base)", step);
    }

    CHECK(!jpt_vk_renderer_validation_enabled(r) || jpt_vk_renderer_validation_errors(r) == 0,
          "overlay lifetime: 0 validation errors (%ld)", jpt_vk_renderer_validation_errors(r));
    jpt_vk_renderer_destroy(r);
}

static void test_frontend_colour_gamma_premul_parity(void) {
    const uint32_t W = 64, H = 64;
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    uint8_t out_rgba[64 * 64 * 4];
    uint32_t fe_bgra[64 * 64];
    uint8_t fe_rgba[64 * 64 * 4];
    uint32_t rw = 0, rh = 0;

    memset(&desc, 0, sizeof desc);
    desc.enable_validation = 1;
    desc.log_fn = test_vk_log;
    desc.cfg.guest_w = W;
    desc.cfg.guest_h = H;
    desc.cfg.internal_w = W;
    desc.cfg.internal_h = H;
    desc.cfg.window_w = W;
    desc.cfg.window_h = H;
    desc.cfg.msaa = 1;
    desc.cfg.texture_filter = JPT_FILTER_BILINEAR;
    desc.cfg.vsync = 1;

    jpt_vk_renderer_t* r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "frontend colour parity: create renderer failed: %s", err);
    if (!r) return;

    /* Non-identity gamma ramp (brightens midtones) */
    uint16_t red[256], green[256], blue[256];
    for (int i = 0; i < 256; i++) {
        double x = (double)i / 255.0;
        uint16_t v = (uint16_t)(pow(x, 0.72) * 65535.0 + 0.5);
        red[i] = green[i] = blue[i] = v;
    }
    jpt_vk_renderer_set_gamma_ramp(r, red, green, blue);

    /* Left half: OP_DIM dark scrim (A=179, premul RGB=0); right half: menu red (R=200, G=32, B=24, A=255) */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            fe_bgra[y * W + x] = (x < 32) ? 0xB3000000u : 0xFFC82018u;
        }
    }
    jpt_bgra_to_rgba_premul(fe_bgra, fe_rgba, W * H);
    jpt_vk_layer_opts_t fe_opts = { 1, 2, 1, 1, NULL }; /* native, premul, gamma=1, point=1 */

    jpt_render_vertex_t full_quad[4] = {
        make_v(0, 0, 0.5f, 0xFF808080u, 0xFF000000u, 0, 0),
        make_v(64, 0, 0.5f, 0xFF808080u, 0xFF000000u, 1, 0),
        make_v(64, 64, 0.5f, 0xFF808080u, 0xFF000000u, 1, 1),
        make_v(0, 64, 0.5f, 0xFF808080u, 0xFF000000u, 0, 1),
    };
    jpt_render_state_t st = default_state();

    CHECK(jpt_vk_renderer_begin_frame(r), "frontend colour: begin");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    jpt_vk_renderer_submit_layer_ex(r, JPT_LAYER_FRONTEND_HD, fe_rgba, W, H, &fe_opts);
    CHECK(jpt_vk_renderer_end_and_present(r), "frontend colour: present");
    CHECK(jpt_vk_renderer_readback_rgba(r, out_rgba, &rw, &rh, sizeof out_rgba), "frontend colour: readback");

    /* Reference math matching D3D11 PS_PRESENT + R.blend_premultiplied */
    uint32_t base_lit = jpt_apply_gamma_rgba(0xFF808080u, red, green, blue);
    uint32_t scrim_lit = jpt_apply_gamma_rgba(0xB3000000u, red, green, blue);
    uint32_t want_scrim = jpt_blend_premul_over_rgba(base_lit, scrim_lit);
    uint32_t want_red = jpt_apply_gamma_rgba(0xFF1820C8u, red, green, blue);

    const uint8_t* p_scrim = px_at(out_rgba, W, 16, 32);
    const uint8_t* p_red = px_at(out_rgba, W, 48, 32);
    CHECK(near_rgb(p_scrim, (int)(want_scrim & 0xFFu), (int)((want_scrim >> 8) & 0xFFu),
                   (int)((want_scrim >> 16) & 0xFFu), 2),
          "OP_DIM scrim matches D3D11 gamma+premul oracle: want (%u,%u,%u) got (%u,%u,%u)",
          (unsigned)(want_scrim & 0xFFu), (unsigned)((want_scrim >> 8) & 0xFFu),
          (unsigned)((want_scrim >> 16) & 0xFFu), p_scrim[0], p_scrim[1], p_scrim[2]);
    CHECK(near_rgb(p_red, (int)(want_red & 0xFFu), (int)((want_red >> 8) & 0xFFu), (int)((want_red >> 16) & 0xFFu), 2),
          "menu red matches D3D11 gamma+premul oracle: want (%u,%u,%u) got (%u,%u,%u)",
          (unsigned)(want_red & 0xFFu), (unsigned)((want_red >> 8) & 0xFFu), (unsigned)((want_red >> 16) & 0xFFu),
          p_red[0], p_red[1], p_red[2]);

    /* Partial dirty-rect update on Frontend HD updates only dirty sub-rect */
    for (uint32_t y = 8; y < 24; y++)
        for (uint32_t x = 40; x < 56; x++) fe_bgra[y * W + x] = 0xFF20E040u;
    jpt_bgra_to_rgba_premul(fe_bgra, fe_rgba, W * H);
    jpt_render_rect_t dirty = { 40, 8, 16, 16 };
    CHECK(jpt_vk_renderer_begin_frame(r), "frontend dirty: begin");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 1, 0.0f, 1, 0);
    jpt_vk_renderer_draw(r, &st, JPT_PRIM_TRIANGLEFAN, full_quad, 4, NULL, NULL, 0);
    jpt_vk_renderer_update_layer_rect_ex(r, JPT_LAYER_FRONTEND_HD, fe_rgba, W, H, &dirty, &fe_opts);
    CHECK(jpt_vk_renderer_end_and_present(r), "frontend dirty: present");
    CHECK(jpt_vk_renderer_readback_rgba(r, out_rgba, &rw, &rh, sizeof out_rgba), "frontend dirty: readback");
    uint32_t want_dirty = jpt_apply_gamma_rgba(0xFF40E020u, red, green, blue);
    CHECK(near_rgb(px_at(out_rgba, W, 48, 16), (int)(want_dirty & 0xFFu), (int)((want_dirty >> 8) & 0xFFu),
                   (int)((want_dirty >> 16) & 0xFFu), 2),
          "dirty rect pixel updated");
    CHECK(near_rgb(px_at(out_rgba, W, 48, 48), (int)(want_red & 0xFFu), (int)((want_red >> 8) & 0xFFu),
                   (int)((want_red >> 16) & 0xFFu), 2),
          "outside dirty rect preserved");

    CHECK(!jpt_vk_renderer_validation_enabled(r) || jpt_vk_renderer_validation_errors(r) == 0,
          "frontend colour parity: 0 validation errors (%ld)", jpt_vk_renderer_validation_errors(r));
    jpt_vk_renderer_destroy(r);
}

typedef struct {
    float x, y, u, v;
    uint32_t col;
} ui_vertex_t;

static void ui_quad(ui_vertex_t* v, uint16_t* idx, unsigned at, float x0, float y0, float x1, float y1, uint32_t col) {
    v[0] = (ui_vertex_t){ x0, y0, 0.5f, 0.5f, col };
    v[1] = (ui_vertex_t){ x1, y0, 0.5f, 0.5f, col };
    v[2] = (ui_vertex_t){ x1, y1, 0.5f, 0.5f, col };
    v[3] = (ui_vertex_t){ x0, y1, 0.5f, 0.5f, col };
    idx[0] = (uint16_t)(at + 0);
    idx[1] = (uint16_t)(at + 1);
    idx[2] = (uint16_t)(at + 2);
    idx[3] = (uint16_t)(at + 0);
    idx[4] = (uint16_t)(at + 2);
    idx[5] = (uint16_t)(at + 3);
}

static void run_ui_overlay_fixture(void) {
    uint8_t rgba[64 * 64 * 4];
    const uint8_t white[4] = { 255, 255, 255, 255 };
    ui_vertex_t verts[8];
    uint16_t idx[12];
    jpt_vk_ui_cmd_t cmds[2];
    jpt_vk_ui_texture_t tex;
    jpt_vk_ui_draw_t d;
    jpt_vk_renderer_stats_t before, after;
    uint32_t rw, rh;
    jpt_vk_renderer_t* r = open_renderer(64, 64, 1);
    if (!r) return;
    ui_quad(verts, idx, 0, 8.0f, 8.0f, 40.0f, 40.0f, 0x800000FFu);
    ui_quad(verts + 4, idx + 6, 4, 48.0f, 8.0f, 60.0f, 20.0f, 0xFFFF0000u);
    memset(cmds, 0, sizeof cmds);
    cmds[0] = (jpt_vk_ui_cmd_t){ { 0, 0, 64, 64 }, 0, 6, 0, 0 };
    cmds[1] = (jpt_vk_ui_cmd_t){ { 0, 0, 54, 64 }, 6, 6, 0, 0 };
    tex = (jpt_vk_ui_texture_t){ 1, 1, 1, 4, white, 1 };
    memset(&d, 0, sizeof d);
    d.display_size[0] = d.display_size[1] = 64.0f;
    d.target_w = d.target_h = 64;
    d.vertices = verts;
    d.vertex_count = 8;
    d.indices = idx;
    d.index_count = 12;
    d.cmds = cmds;
    d.cmd_count = 2;
    d.textures = &tex;
    d.texture_count = 1;
    CHECK(jpt_vk_renderer_begin_frame(r), "ui overlay frame");
    jpt_vk_renderer_clear(r, 1, 0xFFFFFFFFu, 0, 0.0f, 0, 0);
    jpt_vk_renderer_get_stats(r, &before);
    CHECK(jpt_vk_renderer_submit_ui(r, &d), "the ImGui draw data is accepted");
    jpt_vk_renderer_get_stats(r, &after);
    CHECK(after.one_shot_submits == before.one_shot_submits, "the overlay texture upload is recorded in the frame, not submitted alone");
    CHECK(jpt_vk_renderer_end_and_present(r), "ui overlay frame presented");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "ui overlay readback");
    CHECK(px_at(rgba, 64, 20, 20)[0] >= 253 && abs((int)px_at(rgba, 64, 20, 20)[1] - 127) <= 2 &&
              abs((int)px_at(rgba, 64, 20, 20)[2] - 127) <= 2,
          "half-transparent red over white is pink (%u %u %u)", px_at(rgba, 64, 20, 20)[0], px_at(rgba, 64, 20, 20)[1],
          px_at(rgba, 64, 20, 20)[2]);
    CHECK(px_at(rgba, 64, 4, 4)[0] == 255 && px_at(rgba, 64, 4, 4)[1] == 255, "pixels no rectangle covers keep the scene");
    CHECK(px_at(rgba, 64, 50, 12)[0] == 0 && px_at(rgba, 64, 50, 12)[2] == 255, "the second rectangle is opaque blue inside its clip");
    CHECK(px_at(rgba, 64, 57, 12)[0] == 255 && px_at(rgba, 64, 57, 12)[1] == 255, "its clip rectangle cuts it at x = 54 (%u)",
          px_at(rgba, 64, 57, 12)[0]);
    jpt_vk_renderer_get_stats(r, &after);
    CHECK(after.ui_draws >= 2 && after.ui_vertices >= 8 && after.ui_texture_binds >= 1, "the overlay counters report the draw");
    jpt_vk_renderer_destroy(r);
}

static void test_ui_math(void) {
    float scale[2], translate[2];
    const float pos[2] = { 0, 0 }, size[2] = { 200, 100 };
    const float clip[4] = { 20, 10, 120, 60 };
    int32_t sc[4];
    jpt_vk_ui_projection(pos, size, scale, translate);
    CHECK(scale[0] == 0.01f && scale[1] == 0.02f && translate[0] == -1.0f && translate[1] == -1.0f,
          "orthographic projection over the display (%f %f)", scale[0], scale[1]);
    CHECK(jpt_vk_ui_scissor(clip, 200, 100, 0, 0, 200, 100, sc) && sc[0] == 20 && sc[1] == 10 && sc[2] == 100 && sc[3] == 50,
          "a clip rectangle in the target's pixels is the scissor");
    CHECK(jpt_vk_ui_scissor(clip, 200, 100, 100, 50, 400, 200, sc) && sc[0] == 140 && sc[1] == 70 && sc[2] == 200 && sc[3] == 100,
          "a clip rectangle scales and moves with the presented rectangle");
    {
        const float outside[4] = { -50, -50, -10, -10 };
        CHECK(!jpt_vk_ui_scissor(outside, 200, 100, 0, 0, 200, 100, sc), "a clip rectangle outside the target draws nothing");
    }
}

static void run_overlay_batch_fixture(void) {
    uint8_t rgba[64 * 64 * 4];
    uint32_t red[8 * 8], blue[8 * 8], rw, rh;
    jpt_render_rect_t a = { 4, 4, 8, 8 }, b = { 40, 40, 8, 8 }, c = { 6, 6, 8, 8 };
    jpt_vk_renderer_stats_t before, after;
    jpt_vk_renderer_t* r = open_renderer(64, 64, 1);
    if (!r) return;
    for (int i = 0; i < 64; i++) {
        red[i] = 0xFFFF0000u;
        blue[i] = 0xFF0000FFu;
    }
    CHECK(jpt_vk_renderer_begin_frame(r), "overlay batch frame");
    jpt_vk_renderer_clear(r, 1, 0xFF000000u, 0, 0.0f, 0, 0);
    jpt_vk_renderer_get_stats(r, &before);
    CHECK(jpt_vk_renderer_queue_overlay(r, red, 8, &a, 64, 64, 1) && jpt_vk_renderer_queue_overlay(r, blue, 8, &b, 64, 64, 1) &&
              jpt_vk_renderer_flush_overlays(r, 64, 64),
          "two disjoint overlay boxes are queued and flushed");
    jpt_vk_renderer_get_stats(r, &after);
    CHECK(after.overlay_batches - before.overlay_batches == 1 && after.overlays_drawn - before.overlays_drawn == 2,
          "two disjoint boxes share one scene pass break (%llu batches, %llu drawn)",
          (unsigned long long)(after.overlay_batches - before.overlay_batches),
          (unsigned long long)(after.overlays_drawn - before.overlays_drawn));
    before = after;
    CHECK(jpt_vk_renderer_queue_overlay(r, red, 8, &a, 64, 64, 1) && jpt_vk_renderer_queue_overlay(r, blue, 8, &c, 64, 64, 1) &&
              jpt_vk_renderer_flush_overlays(r, 64, 64),
          "two overlapping boxes");
    jpt_vk_renderer_get_stats(r, &after);
    CHECK(after.overlay_batches - before.overlay_batches == 2, "overlapping boxes are flushed in order, not merged");
    CHECK(jpt_vk_renderer_end_and_present(r), "overlay batch frame presented");
    CHECK(jpt_vk_renderer_readback_rgba(r, rgba, &rw, &rh, sizeof rgba), "overlay batch readback");
    CHECK(px_at(rgba, 64, 5, 5)[0] == 255 && px_at(rgba, 64, 5, 5)[2] == 0, "the first box is red");
    CHECK(px_at(rgba, 64, 44, 44)[2] == 255 && px_at(rgba, 64, 44, 44)[0] == 0, "the second box is blue");
    CHECK(px_at(rgba, 64, 10, 10)[2] == 255, "the later overlapping box covers the earlier one");
    jpt_vk_renderer_destroy(r);
}

int main(void) {
    jpt_vk_probe_result_t probe;
    int avail = jpt_vk_probe_headless("", &probe);
    test_ui_math();
    if (!avail) {
        CHECK(probe.summary[0] != 0, "unavailable probe reports readable reason");
        printf("vulkan_render_tests: %d checks, 0 failures (GPU fixtures skipped: %s)\n", g_checks, probe.summary);
        return g_failures ? 1 : 0;
    }
    printf("vulkan_render_tests: running offscreen fixtures on '%s' (%s)\n", probe.gpus[probe.selected_index].label,
           probe.summary);
    run_offscreen_parity_fixtures();
    run_runtime_path_fixtures(1);
    run_runtime_path_fixtures(4);
    run_last_frame_and_timing_fixtures();
    run_ao_cpu_mask_fixture();
    run_ao_fill_and_polygon_fixture();
    test_texture_coverage_unlimited();
    test_overlay_lifetime_zero_stale_pixels();
    test_frontend_colour_gamma_premul_parity();
    run_ui_overlay_fixture();
    run_overlay_batch_fixture();
    if (g_failures) {
        fprintf(stderr, "vulkan_render_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("vulkan_render_tests: %d checks, 0 failures\n", g_checks);
    return 0;
}
