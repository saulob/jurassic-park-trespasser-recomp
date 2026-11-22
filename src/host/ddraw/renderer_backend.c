#include "renderer_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_context.h"

static jpt_win_renderer_state_t g_be;
static jpt_win_renderer_cfg_t g_cfg;
static int g_suppress_assert;
static char g_cached_gpu[128];
static int g_cached_vsync = 1;
static int g_cached_fullscreen = 0;

const jpt_win_backend_ops_t* renderer_backend_resolve_ops(jpt_renderer_t backend) {
    switch (backend) {
    case JPT_RENDERER_D3D11: return &jpt_d3d11_backend_ops;
    case JPT_RENDERER_VULKAN: return &jpt_vulkan_backend_ops;
    case JPT_RENDERER_SOFTWARE: return &jpt_software_backend_ops;
    default: return NULL;
    }
}

void renderer_backend_reset_for_test(int suppress_assert) {
    memset(&g_be, 0, sizeof g_be);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_suppress_assert = suppress_assert ? 1 : 0;
}

static const jpt_win_backend_ops_t* current_ops(void) {
    return g_be.ops ? g_be.ops : &jpt_d3d11_backend_ops;
}

void renderer_backend_configure_with_probe(const jpt_win_renderer_cfg_t* cfg, int d3d11_available, int vulkan_available,
                                           const char* vulkan_probe_summary) {
    jpt_renderer_decision_t dec;
    jpt_renderer_t req = cfg ? cfg->requested : JPT_RENDERER_AUTO;

    memset(&g_be, 0, sizeof g_be);
    if (cfg) g_cfg = *cfg;
    else memset(&g_cfg, 0, sizeof g_cfg);

    if (cfg && cfg->gpu_request && cfg->gpu_request[0])
        snprintf(g_cached_gpu, sizeof g_cached_gpu, "%s", cfg->gpu_request);
    if (cfg) g_cached_vsync = cfg->vsync ? 1 : 0;

    if (g_cfg.system_ddraw) {
        g_be.requested = req;
        g_be.chosen = JPT_RENDERER_SOFTWARE;
        g_be.active = JPT_RENDERER_AUTO;
        g_be.fallback = (req != JPT_RENDERER_SOFTWARE);
        snprintf(g_be.reason, sizeof g_be.reason, "system DirectDraw requested; hardware renderer disabled");
        g_be.ops = &jpt_software_backend_ops;
    } else if (req == JPT_RENDERER_VULKAN && !vulkan_available) {
        /* Explicit Vulkan refuses silent fallback to D3D11 or Software. */
        g_be.requested = JPT_RENDERER_VULKAN;
        g_be.chosen = JPT_RENDERER_VULKAN;
        g_be.active = JPT_RENDERER_AUTO;
        g_be.fallback = 0;
        g_be.probe_failed = 1;
        snprintf(g_be.reason, sizeof g_be.reason, "explicit Vulkan unavailable (%s); refusing silent fallback",
                 (vulkan_probe_summary && vulkan_probe_summary[0]) ? vulkan_probe_summary : "no Vulkan GPU");
        g_be.ops = &jpt_vulkan_backend_ops;
    } else {
        jpt_renderer_resolve_policy(req, 1, d3d11_available, vulkan_available, vulkan_probe_summary, &dec);
        g_be.requested = dec.requested;
        g_be.chosen = dec.chosen;
        g_be.active = JPT_RENDERER_AUTO;
        g_be.fallback = dec.fallback;
        snprintf(g_be.reason, sizeof g_be.reason, "%s", dec.reason);
        g_be.ops = renderer_backend_resolve_ops(dec.chosen);
        if (!g_be.ops) g_be.ops = &jpt_software_backend_ops;
    }

    host_log("renderer", "requested=%s chosen=%s%s (%s)", jpt_renderer_key(g_be.requested),
             jpt_renderer_key(g_be.chosen), g_be.fallback ? " [fallback]" : "", g_be.reason);

    if (g_be.ops->present_set_graphics_card && g_cached_gpu[0]) g_be.ops->present_set_graphics_card(g_cached_gpu);
    if (g_be.ops->present_set_vsync) g_be.ops->present_set_vsync(g_cached_vsync);
    if (g_be.ops->configure) g_be.ops->configure(&g_cfg);
    if (g_be.ops->configure_ao) g_be.ops->configure_ao(g_cfg.ao_level);
    if (g_be.ops->configure_effects) g_be.ops->configure_effects(g_cfg.water_refraction, g_cfg.enhanced_sky);
}

void renderer_backend_configure(const jpt_win_renderer_cfg_t* cfg) {
    jpt_vk_probe_result_t probe;
    int vk_avail = 0;
    jpt_renderer_t req = cfg ? cfg->requested : JPT_RENDERER_AUTO;
    const char* gpu_req = (cfg && cfg->gpu_request) ? cfg->gpu_request : g_cached_gpu;

    memset(&probe, 0, sizeof probe);
    if (req == JPT_RENDERER_VULKAN) {
        vk_avail = jpt_vk_probe_headless(gpu_req, &probe);
        if (vk_avail)
            host_log("vulkan", "probed GPU '%s' (%s)", probe.gpus[probe.selected_index].label, probe.summary);
        else
            host_log("vulkan", "Vulkan probe failed: %s", probe.summary);
    }
    renderer_backend_configure_with_probe(cfg, 1, vk_avail, probe.summary);
}

int renderer_backend_startup(void) {
    if (!g_be.ops) g_be.ops = &jpt_d3d11_backend_ops;

    if (g_be.probe_failed) {
        host_log("renderer", "ERROR: %s", g_be.reason);
        g_be.active = JPT_RENDERER_AUTO;
        g_be.initialized = 0;
        return 0;
    }

    if (g_be.ops->startup && g_be.ops->startup()) {
        g_be.active = g_be.ops->id;
        g_be.initialized = 1;
        host_log("renderer", "requested=%s chosen=%s active=%s", jpt_renderer_key(g_be.requested),
                 jpt_renderer_key(g_be.chosen), jpt_renderer_key(g_be.active));
        return 1;
    }

    if (g_be.requested == JPT_RENDERER_VULKAN) {
        host_log("renderer", "ERROR: Vulkan initialization failed; refusing silent fallback to D3D11");
        g_be.active = JPT_RENDERER_AUTO;
        g_be.initialized = 0;
        return 0;
    }

    /* Auto mode (or explicit D3D11 hardware failure) may fall back explicitly with clear logging. */
    if (g_be.requested == JPT_RENDERER_AUTO && g_be.chosen == JPT_RENDERER_D3D11) {
        jpt_vk_probe_result_t probe;
        memset(&probe, 0, sizeof probe);
        if (jpt_vk_probe_headless(g_cached_gpu, &probe)) {
            host_log("renderer", "auto fallback: D3D11 startup failed; trying Vulkan (%s)", probe.summary);
            g_be.chosen = JPT_RENDERER_VULKAN;
            g_be.fallback = 1;
            g_be.ops = &jpt_vulkan_backend_ops;
            if (g_be.ops->present_set_graphics_card && g_cached_gpu[0]) g_be.ops->present_set_graphics_card(g_cached_gpu);
            if (g_be.ops->present_set_vsync) g_be.ops->present_set_vsync(g_cached_vsync);
            if (g_be.ops->configure) g_be.ops->configure(&g_cfg);
            if (g_be.ops->configure_ao) g_be.ops->configure_ao(g_cfg.ao_level);
            if (g_be.ops->configure_effects) g_be.ops->configure_effects(g_cfg.water_refraction, g_cfg.enhanced_sky);
            if (g_be.ops->startup && g_be.ops->startup()) {
                g_be.active = JPT_RENDERER_VULKAN;
                g_be.initialized = 1;
                host_log("renderer", "requested=%s chosen=%s active=%s [fallback]", jpt_renderer_key(g_be.requested),
                         jpt_renderer_key(g_be.chosen), jpt_renderer_key(g_be.active));
                return 1;
            }
        }
    }

    if (g_be.ops != &jpt_software_backend_ops) {
        host_log("renderer", "fallback: %s startup failed; falling back to Software", jpt_renderer_key(g_be.chosen));
        g_be.chosen = JPT_RENDERER_SOFTWARE;
        g_be.fallback = 1;
        g_be.ops = &jpt_software_backend_ops;
        if (g_be.ops->present_set_graphics_card && g_cached_gpu[0]) g_be.ops->present_set_graphics_card(g_cached_gpu);
        if (g_be.ops->present_set_vsync) g_be.ops->present_set_vsync(g_cached_vsync);
        if (g_be.ops->configure) g_be.ops->configure(&g_cfg);
        if (g_be.ops->startup && g_be.ops->startup()) {
            g_be.active = JPT_RENDERER_SOFTWARE;
            g_be.initialized = 1;
            host_log("renderer", "requested=%s chosen=%s active=%s [fallback]", jpt_renderer_key(g_be.requested),
                     jpt_renderer_key(g_be.chosen), jpt_renderer_key(g_be.active));
            return 1;
        }
    }

    g_be.active = JPT_RENDERER_AUTO;
    g_be.initialized = 0;
    return 0;
}

const jpt_win_renderer_state_t* renderer_backend_state(void) { return &g_be; }
jpt_renderer_t renderer_backend_requested(void) { return g_be.requested; }
jpt_renderer_t renderer_backend_chosen(void) { return g_be.chosen; }
jpt_renderer_t renderer_backend_active(void) { return g_be.active; }

const char* renderer_backend_key(void) {
    return current_ops()->key;
}

const char* renderer_backend_short_name(void) {
    return current_ops()->short_name;
}

const char* renderer_backend_hal_description(void) {
    return current_ops()->hal_description;
}

int renderer_backend_guard_d3d11(const char* caller) {
    if (g_be.chosen == JPT_RENDERER_VULKAN || g_be.active == JPT_RENDERER_VULKAN) {
        host_log("renderer", "FATAL: %s attempted D3D11 initialization while Vulkan is selected (requested=%s chosen=%s active=%s)",
                 caller ? caller : "unknown", jpt_renderer_key(g_be.requested), jpt_renderer_key(g_be.chosen),
                 jpt_renderer_key(g_be.active));
        if (!g_suppress_assert) assert(!"D3D11 initialized while Vulkan is selected");
        return 0;
    }
    return 1;
}

/* ---- Unified public dispatch (d3dr_* and present_*) ---- */

void d3dr_configure(int hardware, unsigned box_w, unsigned box_h, int filter, unsigned anisotropy, unsigned msaa) {
    jpt_win_renderer_cfg_t cfg = g_cfg;
    cfg.requested = hardware ? JPT_RENDERER_D3D11 : JPT_RENDERER_SOFTWARE;
    cfg.system_ddraw = 0;
    cfg.box_w = box_w;
    cfg.box_h = box_h;
    cfg.filter = filter;
    cfg.anisotropy = anisotropy;
    cfg.msaa = msaa;
    cfg.vsync = g_cached_vsync;
    cfg.gpu_request = g_cached_gpu;
    renderer_backend_configure(&cfg);
}

int d3dr_startup(void) { return renderer_backend_startup(); }
int d3dr_hardware(void) { return current_ops()->hardware ? current_ops()->hardware() : 0; }
unsigned long long renderer_hardware_frames(void) {
    return current_ops()->hardware_frames ? current_ops()->hardware_frames() : 0;
}
const char* d3dr_name(void) { return current_ops()->name ? current_ops()->name() : "Software"; }
int d3dr_scene_view(float out[4]) { return current_ops()->scene_view ? current_ops()->scene_view(out) : 0; }
void d3dr_render_box(unsigned* w, unsigned* h) {
    if (current_ops()->render_box) current_ops()->render_box(w, h);
    else {
        if (w) *w = 0;
        if (h) *h = 0;
    }
}
int d3dr_frame_size(int guest_w, int guest_h, int* w, int* h) {
    return current_ops()->frame_size ? current_ops()->frame_size(guest_w, guest_h, w, h) : 0;
}
int d3dr_set_debug_output(const char* mode) {
    return current_ops()->set_debug_output ? current_ops()->set_debug_output(mode) : 0;
}
void d3dr_configure_ambient_occlusion(int level) {
    g_cfg.ao_level = level;
    if (current_ops()->configure_ao) current_ops()->configure_ao(level);
}
void d3dr_configure_effects(int water_refraction, int enhanced_sky) {
    g_cfg.water_refraction = water_refraction;
    g_cfg.enhanced_sky = enhanced_sky;
    if (current_ops()->configure_effects) current_ops()->configure_effects(water_refraction, enhanced_sky);
}
void d3dr_sky_begin(const float sky[5]) {
    if (current_ops()->sky_begin) current_ops()->sky_begin(sky);
}
void d3dr_sky_end(void) {
    if (current_ops()->sky_end) current_ops()->sky_end();
}
void d3dr_summary(void) {
    if (current_ops()->summary) current_ops()->summary();
}
void d3dr_device_lost(void) {
    if (current_ops()->device_lost) current_ops()->device_lost();
}

HRESULT d3dr_attach(d3d_device_t* dev) {
    return current_ops()->attach ? current_ops()->attach(dev) : DDERR_UNSUPPORTED;
}
void d3dr_detach(d3d_device_t* dev) {
    if (current_ops()->detach) current_ops()->detach(dev);
}
void d3dr_begin_scene(d3d_device_t* dev) {
    if (current_ops()->begin_scene) current_ops()->begin_scene(dev);
}
void d3dr_end_scene(d3d_device_t* dev) {
    if (current_ops()->end_scene) current_ops()->end_scene(dev);
}
void d3dr_state_will_change(d3d_device_t* dev, int change, DWORD state, DWORD value) {
    if (current_ops()->state_will_change) current_ops()->state_will_change(dev, change, state, value);
}
void d3dr_state_kept_batch(d3d_device_t* dev) {
    if (current_ops()->state_kept_batch) current_ops()->state_kept_batch(dev);
}
HRESULT d3dr_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                  DWORD index_count) {
    return current_ops()->draw ? current_ops()->draw(dev, type, fvf, vertices, count, indices, index_count)
                               : DDERR_UNSUPPORTED;
}
HRESULT d3dr_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    return current_ops()->clear ? current_ops()->clear(dev, count, rects, flags, color, z) : DDERR_UNSUPPORTED;
}
int d3dr_texture_format_known(const DDPIXELFORMAT* pf) {
    return current_ops()->texture_format_known ? current_ops()->texture_format_known(pf) : 0;
}

void d3dr_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access) {
    if (current_ops()->surface_cpu_access) current_ops()->surface_cpu_access(s, will_write, access);
}
void d3dr_surface_cpu_written(dd_surface_t* s, const RECT* r) {
    if (current_ops()->surface_cpu_written) current_ops()->surface_cpu_written(s, r);
}
int d3dr_surface_unlocked(dd_surface_t* s) {
    return current_ops()->surface_unlocked ? current_ops()->surface_unlocked(s) : 0;
}
int d3dr_surface_fill(dd_surface_t* s, const RECT* r, DWORD color) {
    return current_ops()->surface_fill ? current_ops()->surface_fill(s, r, color) : 0;
}
int d3dr_flip(dd_surface_t* front, int vsync) {
    return current_ops()->flip ? current_ops()->flip(front, vsync) : 0;
}
void d3dr_frame_presented(double present_ms) {
    if (current_ops()->frame_presented) current_ops()->frame_presented(present_ms);
}
void d3dr_surface_freed(dd_surface_t* s) {
    if (current_ops()->surface_freed) current_ops()->surface_freed(s);
}
void d3dr_texture_loaded(dd_surface_t* dst, const dd_surface_t* src) {
    if (current_ops()->texture_loaded) current_ops()->texture_loaded(dst, src);
}

void d3dr_set_ui_layer(const uint16_t* bits, long pitch, int guest_w, int guest_h) {
    if (current_ops()->set_ui_layer) current_ops()->set_ui_layer(bits, pitch, guest_w, guest_h);
}
void d3dr_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty) {
    if (current_ops()->set_frontend) current_ops()->set_frontend(pixels, w, h, dirty);
}
void d3dr_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    if (current_ops()->set_frontend_pointer) current_ops()->set_frontend_pointer(sprite, size, generation, x, y);
}
int d3dr_frontend_failed(void) {
    return current_ops()->frontend_failed ? current_ops()->frontend_failed() : 0;
}
void d3dr_frontend_release(void) {
    if (current_ops()->frontend_release) current_ops()->frontend_release();
}
int d3dr_draw_still_picture(void* surface, const uint16_t* bits, long pitch, int w, int h) {
    return current_ops()->draw_still_picture ? current_ops()->draw_still_picture(surface, bits, pitch, w, h) : 0;
}
void d3dr_text_box(HDC dc, const RECT* box) {
    if (current_ops()->text_box) current_ops()->text_box(dc, box);
}

void d3dr_cpu_polygon(void* surface_iface, const long box[4], uint32_t origin_bytes, int reads) {
    if (current_ops()->cpu_polygon) current_ops()->cpu_polygon(surface_iface, box, origin_bytes, reads);
}
void d3dr_cpu_polygon_done(void) {
    if (current_ops()->cpu_polygon_done) current_ops()->cpu_polygon_done();
}
gpu_polygon_status_t d3dr_texture_polygon_prepare(void* surface_iface, const gpu_texture_polygon_t* p) {
    return current_ops()->texture_polygon_prepare ? current_ops()->texture_polygon_prepare(surface_iface, p)
                                                  : GPU_POLYGON_NO_TARGET;
}
int d3dr_texture_polygon(void* surface_iface, const gpu_texture_polygon_t* p) {
    return current_ops()->texture_polygon ? current_ops()->texture_polygon(surface_iface, p) : 0;
}
void d3dr_texture_polygon_reset(void) {
    if (current_ops()->texture_polygon_reset) current_ops()->texture_polygon_reset();
}
void d3dr_texture_polygon_warm(void) {
    if (current_ops()->texture_polygon_warm) current_ops()->texture_polygon_warm();
}
uint32_t d3dr_lock_caller(void) {
    return current_ops()->lock_caller ? current_ops()->lock_caller() : 0;
}

int present_adapter(dd_adapter_t* out) {
    return current_ops()->present_adapter ? current_ops()->present_adapter(out) : 0;
}
void present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                   const DDGAMMARAMP* gamma, int gamma_changed, int vsync, unsigned generation) {
    if (current_ops()->present_frame)
        current_ops()->present_frame(window, bits, pitch, source, format, gamma, gamma_changed, vsync, generation);
}
void present_release(void) {
    if (current_ops()->present_release) current_ops()->present_release();
}
void present_set_vsync(int on) {
    g_cached_vsync = on ? 1 : 0;
    g_cfg.vsync = g_cached_vsync;
    if (g_be.ops && g_be.ops->present_set_vsync) g_be.ops->present_set_vsync(g_cached_vsync);
}
void present_set_graphics_card(const char* name) {
    snprintf(g_cached_gpu, sizeof g_cached_gpu, "%s", name ? name : "");
    g_cfg.gpu_request = g_cached_gpu;
    if (g_be.ops && g_be.ops->present_set_graphics_card) g_be.ops->present_set_graphics_card(g_cached_gpu);
}
void present_on_resize(uint32_t w, uint32_t h) {
    if (g_be.ops && g_be.ops->present_on_resize) g_be.ops->present_on_resize(w, h);
}
void present_set_fullscreen(int on) {
    g_cached_fullscreen = on ? 1 : 0;
    if (g_be.ops && g_be.ops->present_set_fullscreen) g_be.ops->present_set_fullscreen(g_cached_fullscreen);
}
int present_video_frame_gpu(HWND window, const jpt_vk_video_frame_t* frame, const video_subtitle_image_t* subtitle) {
    return (g_be.ops && g_be.ops->present_video_frame) ? g_be.ops->present_video_frame(window, frame, subtitle) : 0;
}
int present_video_hd_rgba(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                          const video_subtitle_image_t* subtitle) {
    return (g_be.ops && g_be.ops->present_video_hd)
               ? g_be.ops->present_video_hd(window, rgba, w, h, norm_rect, subtitle)
               : 0;
}
