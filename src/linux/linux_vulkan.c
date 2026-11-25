#include "linux_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_display.h"
#include "jpt_present_policy.h"
#include "linux_d3d.h"
#include "linux_log.h"
#include "vk_context.h"
#include "vk_renderer.h"

typedef struct {
    int configured;
    jpt_config_t cfg;
    char env_renderer[32];
    char cli_renderer[32];
    char cli_gpu[JPT_GPU_LABEL_MAX];
    char cache_path[512];
    jpt_renderer_decision_t decision;
    char notice[512];
    int active;

    jpt_vk_renderer_t* renderer;
    int scene_begun;
    unsigned generation;
    uint32_t win_w, win_h;
    uint32_t rt_w, rt_h;
    int fullscreen;
    uint8_t* frame_rgba;
    size_t frame_rgba_cap;
} lx_vk_state_t;

static lx_vk_state_t g_vk;

static const lx_gpu_ops_t k_gpu_ops;

static void vk_log_bridge(const char* msg, void* user) {
    (void)user;
    if (msg && msg[0]) lx_log("vulkan", "%s", msg);
}

void lx_vk_set_cli_overrides(const char* renderer_opt, const char* gpu_opt) {
    if (renderer_opt && renderer_opt[0]) snprintf(g_vk.cli_renderer, sizeof g_vk.cli_renderer, "%s", renderer_opt);
    else g_vk.cli_renderer[0] = 0;
    if (gpu_opt && gpu_opt[0]) snprintf(g_vk.cli_gpu, sizeof g_vk.cli_gpu, "%s", gpu_opt);
    else g_vk.cli_gpu[0] = 0;
}

void lx_vk_set_env_renderer(const char* renderer_env) {
    if (renderer_env && renderer_env[0]) snprintf(g_vk.env_renderer, sizeof g_vk.env_renderer, "%s", renderer_env);
    else g_vk.env_renderer[0] = 0;
}

void lx_vk_configure(const jpt_config_t* cfg, const char* cache_dir) {
    jpt_renderer_selection_t rsel;
    jpt_renderer_t req = JPT_RENDERER_AUTO;
    jpt_vk_probe_result_t probe;
    const char* gpu_req = "";

    if (cfg) g_vk.cfg = *cfg;
    else jpt_config_defaults(&g_vk.cfg);

    jpt_renderer_select_request(g_vk.cfg.renderer_from_ini, (jpt_renderer_t)g_vk.cfg.renderer, g_vk.env_renderer,
                                g_vk.cli_renderer, &rsel);
    jpt_config_apply(&g_vk.cfg, JPT_KEY_RENDERER, jpt_renderer_key(rsel.requested));
    if (g_vk.cli_gpu[0]) jpt_config_apply(&g_vk.cfg, JPT_KEY_GRAPHICS_CARD, g_vk.cli_gpu);

    lx_log("Linux settings",
           "renderer selection: ini=%s, env(TRESPASSER_RENDERER)=%s%s, cli=%s -> requested=%s (source=%s)",
           rsel.ini_present ? jpt_renderer_key(rsel.ini_value) : "unset",
           rsel.env_present ? g_vk.env_renderer : "unset",
           rsel.env_present ? (!rsel.env_valid ? " [invalid, ignored]"
                                               : (rsel.env_ignored ? " [ignored]" : ""))
                            : "",
           rsel.cli_present ? g_vk.cli_renderer : "unset", jpt_renderer_key(rsel.requested),
           jpt_renderer_source_name(rsel.source));

    req = rsel.requested;
    gpu_req = g_vk.cfg.graphics_card;

    if (cache_dir && cache_dir[0])
        snprintf(g_vk.cache_path, sizeof g_vk.cache_path, "%s/vulkan_pipeline.cache", cache_dir);
    else
        g_vk.cache_path[0] = 0;

    memset(&probe, 0, sizeof probe);
    int vk_ok = (req == JPT_RENDERER_SOFTWARE) ? 0 : jpt_vk_probe_headless(gpu_req, &probe);
    jpt_renderer_resolve_policy(req, 0 /* Linux */, 0 /* D3D11 */, vk_ok, probe.summary, &g_vk.decision);
    g_vk.active = (g_vk.decision.chosen == JPT_RENDERER_VULKAN) ? 1 : 0;
    g_vk.configured = 1;
    lx_d3d_set_gpu(g_vk.active ? &k_gpu_ops : NULL);

    lx_log("renderer", "requested=%s chosen=%s%s (%s)", jpt_renderer_key(g_vk.decision.requested),
           jpt_renderer_key(g_vk.decision.chosen), g_vk.decision.fallback ? " [fallback]" : "", g_vk.decision.reason);
    g_vk.notice[0] = 0;
    if (req != JPT_RENDERER_SOFTWARE && g_vk.decision.chosen == JPT_RENDERER_SOFTWARE)
        snprintf(g_vk.notice, sizeof g_vk.notice, "Vulkan could not start: %s. The game runs with the Software renderer.",
                 probe.summary[0] ? probe.summary : g_vk.decision.reason);
}

const char* lx_vk_graphics_notice(void) { return g_vk.notice; }

int lx_vk_active(void) { return g_vk.active; }

const jpt_renderer_decision_t* lx_vk_decision(void) { return &g_vk.decision; }

static jpt_size_t render_box(uint32_t width, uint32_t height) {
    jpt_size_t box = jpt_render_box(g_vk.cfg.render_preset, g_vk.cfg.aspect, (int)width, (int)height);
    if (g_vk.cfg.render_preset < 0 && g_vk.cfg.render_w > 0 && g_vk.cfg.render_h > 0) {
        box.width = g_vk.cfg.render_w;
        box.height = g_vk.cfg.render_h;
    }
    return box;
}

void lx_vk_window_failed(const char* why) {
    g_vk.active = 0;
    lx_d3d_set_gpu(NULL);
    g_vk.decision.chosen = JPT_RENDERER_SOFTWARE;
    g_vk.decision.fallback = 1;
    snprintf(g_vk.decision.reason, sizeof g_vk.decision.reason, "Vulkan window init failed: %s", why);
    snprintf(g_vk.notice, sizeof g_vk.notice, "Vulkan could not start: %s. The game runs with the Software renderer.", why);
}

int lx_vk_attach_window(struct SDL_Window* window, uint32_t width, uint32_t height, int fullscreen) {
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };

    if (!g_vk.active || !window) return 0;
    if (g_vk.renderer) jpt_vk_renderer_destroy(g_vk.renderer);
    g_vk.generation++;
    g_vk.scene_begun = 0;

    memset(&desc, 0, sizeof desc);
    desc.window = window;
    desc.gpu_request = g_vk.cfg.graphics_card;
    desc.cache_path = g_vk.cache_path[0] ? g_vk.cache_path : NULL;
    desc.log_fn = vk_log_bridge;

    jpt_size_t box = render_box(width, height);
    desc.cfg.guest_w = 640;
    desc.cfg.guest_h = 480;
    desc.cfg.internal_w = (uint32_t)(box.width > 0 ? box.width : (int)width);
    desc.cfg.internal_h = (uint32_t)(box.height > 0 ? box.height : (int)height);
    desc.cfg.window_w = width;
    desc.cfg.window_h = height;
    desc.cfg.aspect_idx = g_vk.cfg.aspect;
    desc.cfg.stretch = g_vk.cfg.stretch;
    desc.cfg.vsync = g_vk.cfg.vsync;
    desc.cfg.msaa = g_vk.cfg.msaa ? g_vk.cfg.msaa : 1u;
    desc.cfg.texture_filter = g_vk.cfg.texture_filter;
    desc.cfg.ao_level = g_vk.cfg.ambient_occlusion;
    desc.cfg.water_refraction = g_vk.cfg.water_refraction;
    desc.cfg.enhanced_sky = g_vk.cfg.enhanced_sky;
    desc.cfg.max_object_detail = g_vk.cfg.max_object_detail;
    desc.cfg.fov_degrees = g_vk.cfg.fov;

    g_vk.renderer = jpt_vk_renderer_create(&desc, err, sizeof err);
    if (!g_vk.renderer) {
        lx_log("vulkan", "Vulkan window initialization failed (%s); falling back to Software", err);
        lx_vk_window_failed(err);
        return 0;
    }

    g_vk.rt_w = desc.cfg.internal_w;
    g_vk.rt_h = desc.cfg.internal_h;
    g_vk.win_w = width;
    g_vk.win_h = height;
    g_vk.fullscreen = fullscreen ? 1 : 0;
    jpt_vk_renderer_set_present_options(g_vk.renderer, g_vk.cfg.vsync, g_vk.fullscreen, g_vk.cfg.stretch);
    return 1;
}

void lx_vk_on_resize(uint32_t width, uint32_t height) {
    if (width && height) {
        g_vk.win_w = width;
        g_vk.win_h = height;
    }
    if (g_vk.renderer) jpt_vk_renderer_on_resize(g_vk.renderer, width, height);
}

void lx_vk_set_fullscreen(int fullscreen) {
    g_vk.fullscreen = fullscreen ? 1 : 0;
    if (g_vk.renderer) jpt_vk_renderer_set_present_options(g_vk.renderer, g_vk.cfg.vsync, g_vk.fullscreen, g_vk.cfg.stretch);
}

static void convert_lx_frame_rgba(const lx_frame_t* f) {
    size_t need = (size_t)f->width * (size_t)f->height * 4u;
    if (need > g_vk.frame_rgba_cap) {
        uint8_t* grown = (uint8_t*)realloc(g_vk.frame_rgba, need);
        if (!grown) return;
        g_vk.frame_rgba = grown;
        g_vk.frame_rgba_cap = need;
    }
    const uint8_t* src = (const uint8_t*)f->pixels;
    for (int y = 0; y < f->height; y++) {
        const uint8_t* row = src + (size_t)y * (size_t)f->pitch;
        uint8_t* dst = g_vk.frame_rgba + (size_t)y * (size_t)f->width * 4u;
        if (f->format == LX_PF_RGB565) {
            for (int x = 0; x < f->width; x++) {
                uint16_t p = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
                uint32_t r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
                dst[x * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
                dst[x * 4 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
                dst[x * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
                dst[x * 4 + 3] = 255;
            }
        } else {
            for (int x = 0; x < f->width; x++) {
                dst[x * 4 + 0] = row[x * 4 + 2];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
    }
}

static int open_frame(void) {
    if (!g_vk.renderer || jpt_vk_renderer_is_device_lost(g_vk.renderer)) return 0;
    if (!g_vk.scene_begun) g_vk.scene_begun = jpt_vk_renderer_begin_frame(g_vk.renderer);
    return g_vk.scene_begun;
}

static void close_frame(void) {
    jpt_vk_renderer_end_and_present(g_vk.renderer);
    g_vk.scene_begun = 0;
}

/* The picture in its own shape inside the whole window (as the SDL renderer shows it), whatever shape the 3D target has. */
void lx_vk_present_frame(const lx_frame_t* frame) {
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    jpt_vk_layer_opts_t opts = { 1, 0, 0, 0, NULL };
    lx_view_t view;
    float rect[4];
    if (!frame || !frame->pixels || frame->width <= 0 || frame->height <= 0 || !open_frame()) return;
    jpt_vk_renderer_set_present_options(g_vk.renderer, g_vk.cfg.vsync, g_vk.fullscreen, 1);
    jpt_vk_renderer_set_ao_params(g_vk.renderer, JPT_AO_OFF, 0.0f, 0);
    jpt_vk_renderer_clear_layer(g_vk.renderer, JPT_LAYER_HOST_OVERLAYS);
    convert_lx_frame_rgba(frame);
    if (g_vk.frame_rgba) {
        view = lx_present_view((int)g_vk.win_w, (int)g_vk.win_h, frame->width, frame->height, g_vk.cfg.stretch);
        rect[0] = (float)view.x / (float)g_vk.win_w;
        rect[1] = (float)view.y / (float)g_vk.win_h;
        rect[2] = (float)(view.x + view.w) / (float)g_vk.win_w;
        rect[3] = (float)(view.y + view.h) / (float)g_vk.win_h;
        jpt_vk_renderer_submit_layer_ex(g_vk.renderer, JPT_LAYER_VIDEO, black, 1, 1, &opts);
        opts.norm_rect = rect;
        jpt_vk_renderer_submit_layer_ex(g_vk.renderer, JPT_LAYER_FRONTEND_HD, g_vk.frame_rgba, (uint32_t)frame->width,
                                        (uint32_t)frame->height, &opts);
    }
    close_frame();
}

void lx_vk_present_scene(const lx_scene_frame_t* frame) {
    const jpt_vk_layer_opts_t opts = { 1, 2, 0, 0, NULL };
    if (!frame || !open_frame()) return;
    jpt_vk_renderer_set_present_options(g_vk.renderer, g_vk.cfg.vsync, g_vk.fullscreen, g_vk.cfg.stretch);
    jpt_vk_renderer_clear_layer(g_vk.renderer, JPT_LAYER_VIDEO);
    jpt_vk_renderer_clear_layer(g_vk.renderer, JPT_LAYER_FRONTEND_HD);
    if (frame->overlay)
        jpt_vk_renderer_submit_layer_ex(g_vk.renderer, JPT_LAYER_HOST_OVERLAYS, frame->overlay, (uint32_t)frame->overlay_w,
                                        (uint32_t)frame->overlay_h, &opts);
    else
        jpt_vk_renderer_clear_layer(g_vk.renderer, JPT_LAYER_HOST_OVERLAYS);
    close_frame();
}

void lx_vk_shutdown(void) {
    if (g_vk.renderer) {
        jpt_vk_renderer_destroy(g_vk.renderer);
        g_vk.renderer = NULL;
    }
    free(g_vk.frame_rgba);
    g_vk.frame_rgba = NULL;
    g_vk.frame_rgba_cap = 0;
    g_vk.scene_begun = 0;
    g_vk.generation++;
}

/* ---- the operations linux_d3d_target.c draws with ---- */

static int gpu_available(void) { return g_vk.active && lx_window_backend() != lx_virtual_backend(); }

/* The target keeps the guest's shape inside the launcher's render resolution. */
static void size_target(uint32_t legacy_w, uint32_t legacy_h) {
    const jpt_size_t box = render_box(g_vk.win_w, g_vk.win_h);
    unsigned w = legacy_w, h = legacy_h;
    jpt_choose_render_size((unsigned)box.width, (unsigned)box.height, (int)g_vk.win_w, (int)g_vk.win_h, legacy_w, legacy_h, &w, &h);
    jpt_vk_renderer_set_guest_size(g_vk.renderer, legacy_w, legacy_h);
    if (!w || !h || (w == g_vk.rt_w && h == g_vk.rt_h)) return;
    if (!jpt_vk_renderer_set_internal_size(g_vk.renderer, w, h)) return;
    g_vk.rt_w = w;
    g_vk.rt_h = h;
    lx_log("vulkan", "3D render target %ux%u for a %ux%u screen", w, h, legacy_w, legacy_h);
}

static int gpu_begin(uint32_t legacy_w, uint32_t legacy_h) {
    if (!g_vk.renderer) return 0;
    if (!g_vk.scene_begun) size_target(legacy_w, legacy_h);
    return open_frame();
}

static void gpu_target_size(uint32_t* w, uint32_t* h) {
    *w = g_vk.rt_w;
    *h = g_vk.rt_h;
}

static int gpu_uses_mips(void) { return g_vk.cfg.texture_filter != JPT_FILTER_ORIGINAL; }

static uint64_t gpu_texture_create(const jpt_render_tex_desc_t* desc, const void* pixels) {
    return g_vk.renderer ? jpt_vk_renderer_create_texture(g_vk.renderer, desc, pixels) : 0;
}

static int gpu_texture_update(uint64_t id, const jpt_render_tex_desc_t* desc, const void* pixels, const jpt_render_rect_t* dirty) {
    return g_vk.renderer && jpt_vk_renderer_update_texture(g_vk.renderer, id, desc, pixels, dirty);
}

static int gpu_texture_alive(uint64_t id) { return g_vk.renderer && jpt_vk_renderer_texture_alive(g_vk.renderer, id); }

static void gpu_texture_destroy(uint64_t id) {
    if (g_vk.renderer && id) jpt_vk_renderer_destroy_texture(g_vk.renderer, id);
}

static void gpu_scissor(int enabled, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    jpt_vk_renderer_set_viewport(g_vk.renderer, 0, 0, g_vk.rt_w, g_vk.rt_h);
    jpt_vk_renderer_set_scissor(g_vk.renderer, enabled, x, y, w, h);
}

static int gpu_draw(const jpt_render_state_t* state, int prim, const jpt_render_vertex_t* vertices, uint32_t count,
                    const uint16_t* indices, uint32_t index_count) {
    return jpt_vk_renderer_draw(g_vk.renderer, state, (jpt_render_prim_t)prim, vertices, count, indices, NULL, index_count) ==
           JPT_VK_DRAW_SUBMITTED;
}

static void gpu_clear(int color, uint32_t argb, int depth, float z) {
    jpt_vk_renderer_clear(g_vk.renderer, color, argb, depth, z, depth, 0);
}

static void gpu_select_waves(uint32_t slot) { jpt_vk_renderer_select_waves(g_vk.renderer, slot); }

static void gpu_water_params(float strength, float rhw_metre, const float b[4], const float* waves, uint32_t w, uint32_t h) {
    jpt_vk_renderer_set_water_params(g_vk.renderer, strength, rhw_metre, b[0], b[1], b[2], b[3], waves, w, h);
}

static void gpu_sky_params(const float s[5]) { jpt_vk_renderer_set_sky_params(g_vk.renderer, s[0], s[1], s[2], s[3], s[4]); }

static void gpu_ao_params(int level, float tan_half_height, int gameplay) {
    jpt_vk_renderer_set_ao_params(g_vk.renderer, level, tan_half_height, gameplay);
}

static int gpu_read_scene(const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h, uint16_t* out, size_t pitch) {
    return jpt_vk_renderer_read_scene_rgb565(g_vk.renderer, box, legacy_w, legacy_h, out, pitch);
}

static int gpu_read_last(uint16_t* out, uint32_t w, uint32_t h, size_t pitch) {
    return g_vk.renderer && jpt_vk_renderer_read_last_frame_rgb565(g_vk.renderer, out, w, h, pitch);
}

static int gpu_overlay(const uint32_t* bgra, size_t pitch, const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h, int opaque) {
    return jpt_vk_renderer_draw_overlay(g_vk.renderer, bgra, pitch, box, legacy_w, legacy_h, opaque);
}

static unsigned gpu_generation(void) { return g_vk.generation; }

_Static_assert(LX_GPU_WAVE_SLOTS == JPT_VK_WAVE_SLOTS, "one wave field per renderer slot");

static const lx_gpu_ops_t k_gpu_ops = {
    "Vulkan",          gpu_available,     gpu_begin,         gpu_target_size,     gpu_uses_mips, gpu_texture_create,
    gpu_texture_update, gpu_texture_alive, gpu_texture_destroy, gpu_scissor,       gpu_draw,      gpu_clear,
    gpu_select_waves,  gpu_water_params,  gpu_sky_params,    gpu_ao_params,       gpu_read_scene, gpu_read_last,
    gpu_overlay,       gpu_generation,
};
