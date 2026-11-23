#include "linux_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_display.h"
#include "linux_log.h"
#include "vk_context.h"
#include "vk_renderer.h"

#define DD_OK 0u
#define D3D_OK 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_INVALIDARG_ 0x80070057u
#define E_OUTOFMEMORY_ 0x8007000Eu

#define MAX_D3D_TEX 512

typedef struct {
    int used;
    uint32_t surface_slot;
    uint32_t guest_obj;
    uint32_t width, height, pitch, guest_bits;
    uint32_t pf[8];
    int has_colorkey;
    uint32_t ck_lo, ck_hi;
    uint64_t vk_tex_id;
    int dirty;
} lx_d3d_tex_t;

typedef struct {
    int configured;
    jpt_config_t cfg;
    char cli_renderer[32];
    char cli_gpu[JPT_GPU_LABEL_MAX];
    char cache_path[512];
    jpt_renderer_decision_t decision;
    char notice[512];
    int active;

    jpt_vk_renderer_t* renderer;
    int scene_begun;
    int scene_drew_3d;
    uint32_t win_w, win_h;
    int fullscreen;
    uint8_t* frame_rgba;
    size_t frame_rgba_cap;

    /* Guest COM singletons / tables */
    uint32_t d3d3_obj;
    uint32_t dev3_obj;
    uint32_t vp3_obj;
    int com_ready;
    uint32_t d3d3_vtable;
    uint32_t dev3_vtable;
    uint32_t vp3_vtable;
    uint32_t tex2_vtable;

    jpt_render_state_t state;
    uint32_t bound_tex_guest;
    lx_d3d_tex_t textures[MAX_D3D_TEX];
} lx_vk_state_t;

static lx_vk_state_t g_vk;

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

void lx_vk_configure(const jpt_config_t* cfg, const char* cache_dir) {
    jpt_renderer_t req = JPT_RENDERER_AUTO;
    jpt_vk_probe_result_t probe;
    const char* gpu_req = "";

    if (cfg) g_vk.cfg = *cfg;
    else jpt_config_defaults(&g_vk.cfg);

    if (g_vk.cli_renderer[0]) jpt_config_apply(&g_vk.cfg, JPT_KEY_RENDERER, g_vk.cli_renderer);
    if (g_vk.cli_gpu[0]) jpt_config_apply(&g_vk.cfg, JPT_KEY_GRAPHICS_CARD, g_vk.cli_gpu);

    req = (jpt_renderer_t)g_vk.cfg.renderer;
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

int lx_vk_attach_window(struct SDL_Window* window, uint32_t width, uint32_t height, int fullscreen) {
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };

    if (!g_vk.active || !window) return 0;
    if (g_vk.renderer) jpt_vk_renderer_destroy(g_vk.renderer);

    memset(&desc, 0, sizeof desc);
    desc.window = window;
    desc.gpu_request = g_vk.cfg.graphics_card;
    desc.cache_path = g_vk.cache_path[0] ? g_vk.cache_path : NULL;
    desc.log_fn = vk_log_bridge;

    jpt_size_t box = jpt_render_box(g_vk.cfg.render_preset, g_vk.cfg.aspect, (int)width, (int)height);
    if (g_vk.cfg.render_preset < 0 && g_vk.cfg.render_w > 0 && g_vk.cfg.render_h > 0) {
        box.width = g_vk.cfg.render_w;
        box.height = g_vk.cfg.render_h;
    }
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
        g_vk.active = 0;
        g_vk.decision.chosen = JPT_RENDERER_SOFTWARE;
        g_vk.decision.fallback = 1;
        snprintf(g_vk.decision.reason, sizeof g_vk.decision.reason, "Vulkan window init failed: %s", err);
        snprintf(g_vk.notice, sizeof g_vk.notice, "Vulkan could not start: %s. The game runs with the Software renderer.", err);
        return 0;
    }

    g_vk.win_w = width;
    g_vk.win_h = height;
    g_vk.fullscreen = fullscreen ? 1 : 0;
    jpt_vk_renderer_set_present_options(g_vk.renderer, g_vk.cfg.vsync, g_vk.fullscreen, g_vk.cfg.stretch);
    return 1;
}

void lx_vk_on_resize(uint32_t width, uint32_t height) {
    g_vk.win_w = width;
    g_vk.win_h = height;
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

void lx_vk_present_frame(const lx_frame_t* frame) {
    if (!g_vk.renderer || !frame || !frame->pixels || frame->width <= 0 || frame->height <= 0) return;
    if (jpt_vk_renderer_is_device_lost(g_vk.renderer)) return;

    if (!g_vk.scene_begun) {
        if (!jpt_vk_renderer_begin_frame(g_vk.renderer)) return;
        g_vk.scene_begun = 1;
    }

    if (!g_vk.scene_drew_3d) {
        convert_lx_frame_rgba(frame);
        if (g_vk.frame_rgba) {
            jpt_vk_renderer_submit_layer_rgba(g_vk.renderer, JPT_LAYER_FRONTEND_HD, g_vk.frame_rgba,
                                              (uint32_t)frame->width, (uint32_t)frame->height, 1, 0);
        }
    } else {
        jpt_vk_renderer_clear_layer(g_vk.renderer, JPT_LAYER_FRONTEND_HD);
    }

    jpt_vk_renderer_end_and_present(g_vk.renderer);
    g_vk.scene_begun = 0;
    g_vk.scene_drew_3d = 0;
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
    g_vk.scene_drew_3d = 0;
}

/* ---- Direct3D 6 Guest COM HLE (IDirect3D3, IDirect3DDevice3, IDirect3DViewport3, IDirect3DTexture2) ---- */

static jpt_render_tex_format_t decode_ddpf(const uint32_t pf[8]) {
    uint32_t flags = pf[1], bpp = pf[3], rmask = pf[4], gmask = pf[5], amask = pf[7];
    if (flags & 0x20u) return JPT_TEXFMT_PAL8;
    if (bpp == 32u) return amask ? JPT_TEXFMT_ARGB8888 : JPT_TEXFMT_XRGB8888;
    if (bpp == 24u) return JPT_TEXFMT_RGB888;
    if (bpp == 8u) return JPT_TEXFMT_L8;
    if (rmask == 0xF800u && gmask == 0x07E0u) return JPT_TEXFMT_RGB565;
    if (rmask == 0x0F00u) return amask == 0xF000u ? JPT_TEXFMT_ARGB4444 : JPT_TEXFMT_XRGB4444;
    if (amask == 0x8000u) return JPT_TEXFMT_ARGB1555;
    return JPT_TEXFMT_XRGB1555;
}

static lx_d3d_tex_t* find_tex_by_obj(uint32_t guest_obj) {
    for (int i = 0; i < MAX_D3D_TEX; i++)
        if (g_vk.textures[i].used && g_vk.textures[i].guest_obj == guest_obj) return &g_vk.textures[i];
    return NULL;
}

static void sync_tex_to_gpu(lx_d3d_tex_t* t) {
    jpt_render_tex_desc_t desc;
    const void* bits;
    if (!g_vk.renderer || !t || !t->dirty || !t->guest_bits || !t->width || !t->height) return;
    bits = gp_to_host_checked(t->guest_bits, t->pitch * t->height, 0);
    if (!bits) return;
    memset(&desc, 0, sizeof desc);
    desc.width = t->width;
    desc.height = t->height;
    desc.pitch = t->pitch;
    desc.format = decode_ddpf(t->pf);
    desc.has_colorkey = t->has_colorkey;
    desc.colorkey_lo = t->ck_lo;
    desc.colorkey_hi = t->ck_hi;

    if (!t->vk_tex_id) t->vk_tex_id = jpt_vk_renderer_create_texture(g_vk.renderer, &desc, bits);
    else jpt_vk_renderer_update_texture(g_vk.renderer, t->vk_tex_id, &desc, bits, NULL);
    t->dirty = 0;
}

static uint64_t com_ok(const uint32_t* a, void* u) { (void)a; (void)u; return D3D_OK; }
static uint64_t com_ref(const uint32_t* a, void* u) { (void)a; (void)u; return 1u; }
static uint64_t com_qi_self(const uint32_t* a, void* u) {
    (void)u;
    if (a[2]) gp_write_u32(a[2], a[0]);
    return D3D_OK;
}

static uint64_t d3d3_create_viewport(const uint32_t* a, void* u) {
    (void)u;
    if (a[1]) gp_write_u32(a[1], g_vk.vp3_obj);
    return D3D_OK;
}

static uint64_t d3d3_create_device(const uint32_t* a, void* u) {
    (void)u;
    if (a[3]) gp_write_u32(a[3], g_vk.dev3_obj);
    memset(&g_vk.state, 0, sizeof g_vk.state);
    g_vk.state.z_enable = 1;
    g_vk.state.z_write = 1;
    g_vk.state.z_func = 7; /* GREATEREQUAL */
    g_vk.state.cull_mode = 3;
    g_vk.state.fill_mode = 3;
    g_vk.state.shade_mode = 2;
    g_vk.state.tex_map_blend = 2;
    return D3D_OK;
}

static uint64_t dev3_begin_scene(const uint32_t* a, void* u) {
    (void)a; (void)u;
    if (g_vk.renderer && !g_vk.scene_begun) g_vk.scene_begun = jpt_vk_renderer_begin_frame(g_vk.renderer);
    return D3D_OK;
}

static uint64_t dev3_end_scene(const uint32_t* a, void* u) {
    (void)a; (void)u;
    return D3D_OK;
}

static uint64_t dev3_set_render_state(const uint32_t* a, void* u) {
    uint32_t rs = a[1], val = a[2];
    (void)u;
    switch (rs) {
    case 7: g_vk.state.z_enable = val; break;
    case 8: g_vk.state.fill_mode = val; break;
    case 9: g_vk.state.shade_mode = val; break;
    case 14: g_vk.state.z_write = val; break;
    case 15: g_vk.state.alpha_test = val; break;
    case 19: g_vk.state.src_blend = val; break;
    case 20: g_vk.state.dst_blend = val; break;
    case 21: g_vk.state.tex_map_blend = val; break;
    case 22: g_vk.state.cull_mode = val; break;
    case 23: g_vk.state.z_func = val; break;
    case 24: g_vk.state.alpha_ref = val; break;
    case 25: g_vk.state.alpha_func = val; break;
    case 27: g_vk.state.alpha_blend = val; break;
    case 28: g_vk.state.fog_enable = val; break;
    case 29: g_vk.state.specular_enable = val; break;
    case 34: g_vk.state.fog_color = val; break;
    case 41: g_vk.state.colorkey_enable = val; break;
    case 47: g_vk.state.z_bias = val; break;
    default: break;
    }
    return D3D_OK;
}

static uint64_t dev3_set_texture(const uint32_t* a, void* u) {
    (void)u;
    if (a[1] == 0u) {
        g_vk.bound_tex_guest = a[2];
        lx_d3d_tex_t* t = a[2] ? find_tex_by_obj(a[2]) : NULL;
        if (t) {
            sync_tex_to_gpu(t);
            g_vk.state.has_texture = 1;
            g_vk.state.texture_id = t->vk_tex_id;
        } else {
            g_vk.state.has_texture = 0;
            g_vk.state.texture_id = 0;
        }
    }
    return D3D_OK;
}

static uint64_t dev3_set_tss(const uint32_t* a, void* u) {
    (void)u;
    if (a[1] == 0u) {
        if (a[2] == 13u) g_vk.state.tex_addr_u = a[3];
        else if (a[2] == 14u) g_vk.state.tex_addr_v = a[3];
        else if (a[2] == 16u) g_vk.state.tex_mag_filter = a[3];
        else if (a[2] == 17u) g_vk.state.tex_min_filter = a[3];
    }
    return D3D_OK;
}

static uint64_t dev3_draw_primitive(const uint32_t* a, void* u) {
    uint32_t prim = a[1], vptr = a[3], vcount = a[4];
    const jpt_render_vertex_t* verts;
    (void)u;
    if (!g_vk.renderer || !vptr || !vcount) return D3D_OK;
    if (!g_vk.scene_begun) g_vk.scene_begun = jpt_vk_renderer_begin_frame(g_vk.renderer);
    verts = (const jpt_render_vertex_t*)gp_to_host_checked(vptr, vcount * (uint32_t)sizeof(jpt_render_vertex_t), 0);
    if (verts) {
        jpt_vk_renderer_draw(g_vk.renderer, &g_vk.state, (jpt_render_prim_t)prim, verts, vcount, NULL, NULL, 0);
        g_vk.scene_drew_3d = 1;
    }
    return D3D_OK;
}

static uint64_t dev3_draw_indexed_primitive(const uint32_t* a, void* u) {
    uint32_t prim = a[1], vptr = a[3], vcount = a[4], iptr = a[5], icount = a[6];
    const jpt_render_vertex_t* verts;
    const uint16_t* idx;
    (void)u;
    if (!g_vk.renderer || !vptr || !vcount || !iptr || !icount) return D3D_OK;
    if (!g_vk.scene_begun) g_vk.scene_begun = jpt_vk_renderer_begin_frame(g_vk.renderer);
    verts = (const jpt_render_vertex_t*)gp_to_host_checked(vptr, vcount * (uint32_t)sizeof(jpt_render_vertex_t), 0);
    idx = (const uint16_t*)gp_to_host_checked(iptr, icount * (uint32_t)sizeof(uint16_t), 0);
    if (verts && idx) {
        jpt_vk_renderer_draw(g_vk.renderer, &g_vk.state, (jpt_render_prim_t)prim, verts, vcount, idx, NULL, icount);
        g_vk.scene_drew_3d = 1;
    }
    return D3D_OK;
}

static uint64_t vp3_set_viewport2(const uint32_t* a, void* u) {
    const uint32_t* vd;
    (void)u;
    if (!g_vk.renderer || !a[1]) return D3D_OK;
    vd = (const uint32_t*)gp_to_host_checked(a[1], 44u, 0);
    if (vd) jpt_vk_renderer_set_viewport(g_vk.renderer, (int32_t)vd[1], (int32_t)vd[2], vd[3], vd[4]);
    return D3D_OK;
}

static uint64_t vp3_clear2(const uint32_t* a, void* u) {
    uint32_t flags = a[3], color = a[4], zbits = a[5], stencil = a[6];
    float z = 0.0f;
    (void)u;
    memcpy(&z, &zbits, sizeof z);
    if (g_vk.renderer) {
        if (!g_vk.scene_begun) g_vk.scene_begun = jpt_vk_renderer_begin_frame(g_vk.renderer);
        jpt_vk_renderer_clear(g_vk.renderer, (flags & 1u) != 0, color, (flags & 2u) != 0, z, (flags & 4u) != 0, stencil);
    }
    return D3D_OK;
}

static uint64_t tex2_load(const uint32_t* a, void* u) {
    lx_d3d_tex_t* dst = find_tex_by_obj(a[0]);
    lx_d3d_tex_t* src = find_tex_by_obj(a[1]);
    (void)u;
    if (dst && src && src->guest_bits && dst->guest_bits && src->width == dst->width && src->height == dst->height) {
        uint32_t bytes = src->pitch * src->height;
        const void* s = gp_to_host_checked(src->guest_bits, bytes, 0);
        void* d = gp_to_host_checked(dst->guest_bits, bytes, 1);
        if (s && d) memcpy(d, s, bytes);
        dst->dirty = 1;
        sync_tex_to_gpu(dst);
    }
    return D3D_OK;
}

static uint32_t make_vtable(int slots, const uint32_t* thunks) {
    uint32_t vt = gm_heap_alloc((uint32_t)slots * 4u, 1);
    if (!vt) return 0;
    for (int i = 0; i < slots; i++) gp_write_u32(vt + (uint32_t)i * 4u, thunks[i]);
    return vt;
}

static void ensure_com_tables(void) {
    uint32_t t_ok, t_ref, t_qi;
    if (g_vk.com_ready) return;

    t_ok = ht_register("d3d.ok", com_ok, 1, HT_STDCALL, NULL);
    t_ref = ht_register("d3d.ref", com_ref, 1, HT_STDCALL, NULL);
    t_qi = ht_register("d3d.qi", com_qi_self, 3, HT_STDCALL, NULL);

    /* IDirect3D3 (11 methods) */
    {
        uint32_t vt[11];
        for (int i = 0; i < 11; i++) vt[i] = t_ok;
        vt[0] = t_qi;
        vt[1] = t_ref;
        vt[2] = t_ref;
        vt[6] = ht_register("d3d3.CreateViewport", d3d3_create_viewport, 3, HT_STDCALL, NULL);
        vt[8] = ht_register("d3d3.CreateDevice", d3d3_create_device, 5, HT_STDCALL, NULL);
        g_vk.d3d3_vtable = make_vtable(11, vt);
        g_vk.d3d3_obj = gm_heap_alloc(16u, 1);
        if (g_vk.d3d3_obj) gp_write_u32(g_vk.d3d3_obj, g_vk.d3d3_vtable);
    }
    /* IDirect3DDevice3 (44 methods) */
    {
        uint32_t vt[44];
        for (int i = 0; i < 44; i++) vt[i] = t_ok;
        vt[0] = t_qi;
        vt[1] = t_ref;
        vt[2] = t_ref;
        vt[5] = ht_register("dev3.BeginScene", dev3_begin_scene, 1, HT_STDCALL, NULL);
        vt[6] = ht_register("dev3.EndScene", dev3_end_scene, 1, HT_STDCALL, NULL);
        vt[22] = ht_register("dev3.SetRenderState", dev3_set_render_state, 3, HT_STDCALL, NULL);
        vt[25] = ht_register("dev3.DrawPrimitive", dev3_draw_primitive, 6, HT_STDCALL, NULL);
        vt[26] = ht_register("dev3.DrawIndexedPrimitive", dev3_draw_indexed_primitive, 8, HT_STDCALL, NULL);
        vt[35] = ht_register("dev3.SetTexture", dev3_set_texture, 3, HT_STDCALL, NULL);
        vt[37] = ht_register("dev3.SetTextureStageState", dev3_set_tss, 4, HT_STDCALL, NULL);
        g_vk.dev3_vtable = make_vtable(44, vt);
        g_vk.dev3_obj = gm_heap_alloc(16u, 1);
        if (g_vk.dev3_obj) gp_write_u32(g_vk.dev3_obj, g_vk.dev3_vtable);
    }
    /* IDirect3DViewport3 (23 methods) */
    {
        uint32_t vt[23];
        for (int i = 0; i < 23; i++) vt[i] = t_ok;
        vt[0] = t_qi;
        vt[1] = t_ref;
        vt[2] = t_ref;
        vt[19] = ht_register("vp3.SetViewport2", vp3_set_viewport2, 2, HT_STDCALL, NULL);
        vt[22] = ht_register("vp3.Clear2", vp3_clear2, 7, HT_STDCALL, NULL);
        g_vk.vp3_vtable = make_vtable(23, vt);
        g_vk.vp3_obj = gm_heap_alloc(16u, 1);
        if (g_vk.vp3_obj) gp_write_u32(g_vk.vp3_obj, g_vk.vp3_vtable);
    }
    /* IDirect3DTexture2 (7 methods) */
    {
        uint32_t vt[7];
        for (int i = 0; i < 7; i++) vt[i] = t_ok;
        vt[0] = t_qi;
        vt[1] = t_ref;
        vt[2] = t_ref;
        vt[5] = ht_register("tex2.Load", tex2_load, 2, HT_STDCALL, NULL);
        g_vk.tex2_vtable = make_vtable(7, vt);
    }
    g_vk.com_ready = 1;
}

uint32_t lx_vk_query_direct3d3(void) {
    if (!g_vk.active) return 0;
    ensure_com_tables();
    return g_vk.d3d3_obj;
}

uint32_t lx_vk_surface_query_texture2(uint32_t surface_slot, uint32_t width, uint32_t height, uint32_t pitch,
                                      uint32_t guest_bits, const uint32_t pf[8], int has_colorkey, uint32_t ck_lo,
                                      uint32_t ck_hi) {
    int free_i = -1;
    if (!g_vk.active) return 0;
    ensure_com_tables();
    for (int i = 0; i < MAX_D3D_TEX; i++) {
        if (g_vk.textures[i].used && g_vk.textures[i].surface_slot == surface_slot) {
            return g_vk.textures[i].guest_obj;
        }
        if (!g_vk.textures[i].used && free_i < 0) free_i = i;
    }
    if (free_i < 0) return 0;
    lx_d3d_tex_t* t = &g_vk.textures[free_i];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->surface_slot = surface_slot;
    t->guest_obj = gm_heap_alloc(16u, 1);
    if (!t->guest_obj) {
        t->used = 0;
        return 0;
    }
    gp_write_u32(t->guest_obj, g_vk.tex2_vtable);
    t->width = width;
    t->height = height;
    t->pitch = pitch;
    t->guest_bits = guest_bits;
    if (pf) memcpy(t->pf, pf, sizeof t->pf);
    t->has_colorkey = has_colorkey;
    t->ck_lo = ck_lo;
    t->ck_hi = ck_hi;
    t->dirty = 1;
    return t->guest_obj;
}

void lx_vk_surface_notify_unlock(uint32_t surface_slot, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t guest_bits, const uint32_t pf[8], int has_colorkey, uint32_t ck_lo,
                                 uint32_t ck_hi) {
    for (int i = 0; i < MAX_D3D_TEX; i++) {
        if (g_vk.textures[i].used && g_vk.textures[i].surface_slot == surface_slot) {
            g_vk.textures[i].width = width;
            g_vk.textures[i].height = height;
            g_vk.textures[i].pitch = pitch;
            g_vk.textures[i].guest_bits = guest_bits;
            if (pf) memcpy(g_vk.textures[i].pf, pf, sizeof g_vk.textures[i].pf);
            g_vk.textures[i].has_colorkey = has_colorkey;
            g_vk.textures[i].ck_lo = ck_lo;
            g_vk.textures[i].ck_hi = ck_hi;
            g_vk.textures[i].dirty = 1;
            return;
        }
    }
}

void lx_vk_surface_notify_destroy(uint32_t surface_slot) {
    for (int i = 0; i < MAX_D3D_TEX; i++) {
        if (g_vk.textures[i].used && g_vk.textures[i].surface_slot == surface_slot) {
            if (g_vk.renderer && g_vk.textures[i].vk_tex_id)
                jpt_vk_renderer_destroy_texture(g_vk.renderer, g_vk.textures[i].vk_tex_id);
            g_vk.textures[i].used = 0;
            return;
        }
    }
}

void lx_vk_reset_com(void) {
    for (int i = 0; i < MAX_D3D_TEX; i++) {
        if (g_vk.textures[i].used && g_vk.renderer && g_vk.textures[i].vk_tex_id)
            jpt_vk_renderer_destroy_texture(g_vk.renderer, g_vk.textures[i].vk_tex_id);
        g_vk.textures[i].used = 0;
    }
    g_vk.com_ready = 0;
    g_vk.d3d3_obj = g_vk.dev3_obj = g_vk.vp3_obj = 0;
    g_vk.bound_tex_guest = 0;
}
