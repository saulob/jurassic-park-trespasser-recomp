#define COBJMACROS
#include "ao_policy.h"
#include "ce_effects.h"
#include "d3d_legacy.h"
#include "d3d_translate.h"
#include "page_guard.h"
#include "debug_menu_panel.h"
#include "host.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_present_policy.h"
#include "jpt_renderer.h"
#include "renderer_backend.h"
#include "target_sync.h"
#include "texture_gpu_policy.h"
#include "text_layer.h"
#include "vk_context.h"
#include "vk_policy.h"
#include "vk_renderer.h"
#include "vk_shaders.h"
#include "vk_win_account.h"
#include "vk_win_policy.h"
#include "vk_win_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_WIN_INIT_TEX_ENTRIES 4096
#define VK_WIN_WATCH_PAGES 1024
#define VK_WIN_FVF_SLOTS 8
#define VK_WIN_FAIL_LOG_LIMIT 12

typedef struct {
    dd_surface_t* surface;
    uint64_t vk_tex_id;
    uint32_t width, height;
    int format;
    int has_colorkey;
    unsigned levels;
    int dynamic;
    d3dt_upload_history_t history;
    uint32_t ck_lo, ck_hi;
    uint32_t palette_sum;
    int dirty;
} vk_win_tex_t;

typedef enum {
    BR_SKIP_NO_TARGET,
    BR_SKIP_VERTEX_FORMAT,
    BR_SKIP_OUTSIDE_SCENE,
    BR_SKIP_EMPTY,
    BR_SKIP_INDEX_RANGE,
    BR_SKIP_NO_RENDERER,
    BR_SKIP_NO_FRAME,
    BR_SKIP_TEXTURE,
    BR_SKIP_OUT_OF_MEMORY,
    BR_SKIP_COUNT
} vk_win_skip_t;

static const char* const k_skip_names[BR_SKIP_COUNT] = {
    "no target", "vertex format", "outside BeginScene", "no primitives", "index out of range", "renderer unavailable",
    "no frame", "texture unavailable", "out of memory"
};

typedef struct vk_win_target_s {
    d3d_device_t* device;
    dd_surface_t* surface;
    uint32_t legacy_w, legacy_h;
    uint32_t internal_w, internal_h;
    tsync_t sync;
    int ao_scene;
    int frame_started;
    uint16_t* reference;
    uint32_t* scratch;
    d3dt_dirty_t dirty;
    int inert_lock;
    int gpu_box_known;
    d3dt_rect_t gpu_box;
    uint16_t gpu_clear;
    uint8_t *tile_stale, *tile_drawn, *tile_work;
    tsync_deferred_t deferred;
    d3dt_rect_t deferred_region;
    int deferred_reason;
    uint32_t deferred_caller;
    uint8_t* tile_set[2];
    const uint8_t* tile_set_bits[2];
    int tile_clear_known;
    uint16_t tile_clear_color;
    unsigned tiles_x, tiles_y;
} vk_win_target_t;

typedef struct {
    int pending, pending_reads;
    d3dt_rect_t pending_box;
    vk_win_target_t* target;
    int captured;
    d3dt_dirty_t dirty;
    tsync_tiles_t tiles;
    vk_win_target_t* text_target;
} vk_polys_t;

static void end_polygon_run(const vk_win_target_t* t);

typedef enum { VSR_GPU_DRAW, VSR_POLYGON_RUN, VSR_LOCK, VSR_OTHER_ACCESS, VSR_PRESENT, VSR_COUNT } vk_sync_reason_t;

typedef struct {
    uint32_t caller;
    unsigned long long accesses, readbacks, read_px;
    double ms;
} vk_caller_stat_t;

typedef struct {
    unsigned long long frames, locks, readbacks;
    double sync_ms;
} vk_water_stat_t;

typedef struct {
    double raster_ms, clear_ms, detect_ms, upload_ms, build_ms;
    unsigned long long frames, pixels, rects, tiles, gpu_frames;
} vk_overlay_stat_t;

typedef struct {
    unsigned long long calls, readbacks, read_px, uploads, upload_px, requested_px;
    double ms;
} vk_sync_stat_t;

static const char* const k_sync_reason_names[VSR_COUNT] = { "GPU draw after a CPU write", "CPU polygon run", "Lock",
                                                            "GetDC and other access", "present" };

static struct {
    int configured;
    int active;
    char gpu_request[128];
    char cache_path[512];
    unsigned box_w, box_h;
    int filter_d3dr;
    int filter_cfg;
    unsigned anisotropy;
    unsigned msaa_request;
    int vsync;
    int fullscreen;
    int stretch;
    int aspect;
    int ao_level;
    fx_switch_t fx_water;
    fx_switch_t fx_sky;
    float sky_constants[5];
    int sky_usable;
    int sky_drawing;
    uint32_t debug_flags;

    jpt_vk_probe_result_t probe;
    dd_adapter_t adapter;
    int adapter_valid;

    jpt_vk_renderer_t* vk;
    HWND game_hwnd;
    HWND surface_hwnd;
    uint32_t win_w, win_h;
    uint32_t rt_w, rt_h;
    int frame_open;
    int drew_3d;
    int first_frame_logged;
    int swapchain_logged;
    int ao_logged;
    int water_logged;
    int sky_logged;

    vk_win_target_t* active_target;
    vk_win_tex_t** tex_entries;
    int tex_count;
    int tex_cap;

    /* Scratch buffers */
    jpt_render_vertex_t* vtx_buf;
    size_t vtx_cap;
    uint8_t* rgba_scratch;
    size_t rgba_cap;
    uint8_t* fe_rgba;
    size_t fe_rgba_cap;
    uint8_t* ptr_rgba;
    size_t ptr_rgba_cap;
    uint8_t* ui_rgba;
    size_t ui_rgba_cap;
    uint8_t* overlay_rgba;
    size_t overlay_rgba_cap;
    uint32_t overlay_w, overlay_h;
    int overlay_valid, overlay_force_full;
    int overlay_tiles_w, overlay_tiles_h;
    uint8_t *overlay_cpu_mask, *overlay_new_mask, *overlay_gpu_mask, *overlay_upload_mask;
    uint64_t* overlay_hash;
    vk_overlay_stat_t overlay_win, overlay_all;
    int cpu_overlay;
    uint64_t overlay_pixels;
    uint8_t* sub_rgba;
    size_t sub_rgba_cap;
    float* wave_buf;
    size_t wave_cap;
    unsigned wave_serial;

    /* Frontend HD / UI Canvas / Still picture state */
    const uint32_t* fe_pixels;
    int fe_w, fe_h;
    int fe_uploaded_w, fe_uploaded_h;
    int fe_dirty;
    int fe_dirty_full;
    jpt_render_rect_t fe_dirty_rect;
    int fe_layer_active;
    const uint32_t* ptr_sprite;
    int ptr_size;
    unsigned ptr_gen, ptr_uploaded;
    int ptr_x, ptr_y;

    const uint16_t* ui_bits;
    long ui_pitch;
    int ui_guest_w, ui_guest_h;

    uint8_t* still_rgba;
    int last_flip_valid;
    uint32_t last_flip_w, last_flip_h;
    unsigned long long last_frame_reads;
    int still_w, still_h;
    int still_active;
    int still_reuse;
    int still_resident;
    unsigned long long mipped_textures, dynamic_textures;
    const uint16_t* still_bits;
    uint32_t still_key;
    unsigned long long still_draws, still_uploads;

    /* Gamma & scene view */
    int gamma_set;
    DDGAMMARAMP last_gamma;
    float scene_view[4];
    float scene_frame[4];
    uint64_t scene_frame_area;

    int test_headless;
    int stats_on;
    vka_t acc, acc_base;
    struct {
        tgpu_texture_cache_t textures, bump_table_cache, bump_clut_cache;
        tgpu_clut_cache_t cluts;
        const jpt_vk_renderer_t* renderer;
        vk_win_target_t* prepared;
        uint32_t texel_offset, clut_slot, bump_table_offset, bump_clut_offset;
        unsigned long long drawn, failed_uploads, failed_draws;
    } poly;
    double win_cpu_base_ms, win_wall_base_ms, win_sections_base[16];
    double total_cpu_start_ms, total_wall_start_ms;
    unsigned long long stats_window_frames;
    jpt_vk_renderer_stats_t stats_window_base;
    jpt_vk_renderer_stats_t totals;
    unsigned long long guest_draws;
    unsigned long long bridge_skips[BR_SKIP_COUNT];
    unsigned long long fmt_uploads[D3DT_FMT_COUNT];
    unsigned long long tex_untracked_writes;
    unsigned long long tex_palette_refresh;
    unsigned long long tex_unavailable_logged;
    unsigned long long water_requested;
    unsigned long long water_no_link;
    unsigned long long water_wave_uploads;
    unsigned long long sky_requested;
    struct {
        DWORD fvf;
        unsigned long long draws;
    } fvf_seen[VK_WIN_FVF_SLOTS];
    unsigned fvf_count;
    vk_polys_t polys;
    unsigned long long sync_composites, sync_uploads, sync_readbacks, sync_readback_pixels, sync_synthesized,
        sync_front_copies, sync_captures, sync_overlay_failures, sync_readback_failures, cpu_polygons,
        cpu_polygon_locks, text_boxes, inert_locks;
    uint16_t* readback_scratch;
    size_t readback_scratch_cap;
    vk_sync_stat_t sync_win[VSR_COUNT], sync_all[VSR_COUNT];
    struct vk_win_target_s* deferred;
    unsigned long long deferred_locks, deferred_untouched, deferred_touched;
    unsigned long long tile_flips, tile_reads_avoided, tiles_kept, tiles_read, tiles_filled, tiles_stale_total, tile_rects, tile_read_pixels;
    unsigned long long tile_ctx_reads[3];
    unsigned long long tile_full_invalidations[4], cpu_epochs, epoch_locks, epoch_locks_gpu;
    int epoch_open;
    int tiles_logged;
    vk_caller_stat_t callers[48];
    unsigned callers_n;
    uint32_t tile_caller;
    vk_water_stat_t water_stat[2];
    int frame_water;
    unsigned long long flip_locks_mark, flip_reads_mark;
    double flip_sync_mark;
    int tile_ctx;
    int ao_executed_logged;
    int water_executed_logged;
    int water_w, water_h;
    int sky_executed_logged;

    /* Stats */
    unsigned long long frames_hw;
    unsigned long long frames_dib;
    unsigned long long draw_calls;
    unsigned long long triangles;
    unsigned long long clears;
    unsigned long long water_batches;
    unsigned long long sky_batches;
    unsigned long long readbacks;
    uint32_t rgb565_rgba[65536];
    int rgb565_ready;
} V;

static void settle_deferred(void);
static void drop_deferred(const struct vk_win_target_s* t);

#define VK_TILE_SHIFT 5
#define VK_READ_RECTS 128u

static size_t tile_count(const vk_win_target_t* t) { return (size_t)t->tiles_x * (size_t)t->tiles_y; }

static void tiles_set_all(vk_win_target_t* t, uint8_t* mask, uint8_t value) {
    if (mask) memset(mask, value, tile_count(t));
}

static void tiles_select(vk_win_target_t* t) {
    const uint8_t* bits = t->surface ? t->surface->bits : NULL;
    int pick = -1;
    if (t->tile_set_bits[0] == bits) pick = 0;
    else if (t->tile_set_bits[1] == bits) pick = 1;
    if (pick < 0) {
        pick = !t->tile_set_bits[0] ? 0 : !t->tile_set_bits[1] ? 1 : (t->tile_stale == t->tile_set[0] ? 1 : 0);
        t->tile_set_bits[pick] = bits;
        memset(t->tile_set[pick], 1, tile_count(t));
    }
    t->tile_stale = t->tile_set[pick];
}

enum { TILE_INVAL_FLIP, TILE_INVAL_CLEAR, TILE_INVAL_FAILURE, TILE_INVAL_ATTACH };

static void tiles_invalidate_all(vk_win_target_t* t, int reason) {
    tiles_select(t);
    tiles_set_all(t, t->tile_stale, 1);
    V.tile_full_invalidations[reason]++;
}

static void tiles_mark_rect(const vk_win_target_t* t, uint8_t* mask, const d3dt_rect_t* r, uint8_t value) {
    long x0, y0, x1, y1;
    if (!mask || d3dt_rect_empty(r)) return;
    x0 = r->left < 0 ? 0 : r->left;
    y0 = r->top < 0 ? 0 : r->top;
    x1 = r->right > (long)t->legacy_w ? (long)t->legacy_w : r->right;
    y1 = r->bottom > (long)t->legacy_h ? (long)t->legacy_h : r->bottom;
    if (x1 <= x0 || y1 <= y0) return;
    for (long ty = y0 >> VK_TILE_SHIFT; ty <= (y1 - 1) >> VK_TILE_SHIFT; ty++)
        for (long tx = x0 >> VK_TILE_SHIFT; tx <= (x1 - 1) >> VK_TILE_SHIFT; tx++) mask[ty * (long)t->tiles_x + tx] = value;
}


static void vk_win_log_cb(const char* msg, void* user) {
    (void)user;
    if (msg && msg[0]) host_log("vulkan", "%s", msg);
}

static void init_rgb565_table(void) {
    if (V.rgb565_ready) return;
    for (uint32_t p = 0; p < 65536u; p++) {
        uint32_t r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
        uint32_t r = (r5 << 3) | (r5 >> 2);
        uint32_t g = (g6 << 2) | (g6 >> 4);
        uint32_t b = (b5 << 3) | (b5 >> 2);
        V.rgb565_rgba[p] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    V.rgb565_ready = 1;
}

static uint8_t* ensure_buf(uint8_t** ptr, size_t* cap, size_t need) {
    if (need > *cap) {
        uint8_t* grown = (uint8_t*)realloc(*ptr, need);
        if (!grown) return NULL;
        *ptr = grown;
        *cap = need;
    }
    return *ptr;
}

static int map_d3dr_filter_to_cfg(int filter, unsigned aniso) {
    if (filter == D3DR_FILTER_ORIGINAL || filter == D3DR_FILTER_POINT) return JPT_FILTER_ORIGINAL;
    if (filter == D3DR_FILTER_BILINEAR || filter == D3DR_FILTER_TRILINEAR) return JPT_FILTER_BILINEAR;
    if (filter == D3DR_FILTER_ANISOTROPIC) {
        if (aniso >= 16) return JPT_FILTER_ANISO16;
        if (aniso >= 8) return JPT_FILTER_ANISO8;
        return JPT_FILTER_ANISO4;
    }
    return JPT_FILTER_ANISO16;
}

static const char* filter_name(int filter) {
    switch (filter) {
    case D3DR_FILTER_ORIGINAL: return "original";
    case D3DR_FILTER_POINT: return "point";
    case D3DR_FILTER_BILINEAR: return "bilinear";
    case D3DR_FILTER_TRILINEAR: return "trilinear";
    case D3DR_FILTER_ANISOTROPIC: return "anisotropic";
    default: return "bilinear";
    }
}

static const char* vk_format_label(VkFormat fmt) {
    switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    case VK_FORMAT_D16_UNORM: return "D16_UNORM";
    default: return "VK_FORMAT";
    }
}

static const char* vk_present_mode_label(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "FIFO";
    }
}

static int effective_stretch(HWND game_window, HWND surf) {
    if (!V.test_headless && surf && surf != game_window) return 1;
    return V.stretch;
}

static void choose_internal_size(uint32_t legacy_w, uint32_t legacy_h, uint32_t* out_w, uint32_t* out_h) {
    unsigned w = legacy_w, h = legacy_h;
    unsigned view_w = V.win_w, view_h = V.win_h;
    if (V.test_headless) {
        *out_w = legacy_w ? legacy_w : 640u;
        *out_h = legacy_h ? legacy_h : 480u;
        return;
    }
    if (!V.box_w || !V.box_h) vkp_window_host_view_size(&view_w, &view_h);
    jpt_choose_render_size(V.box_w, V.box_h, (int)view_w, (int)view_h, legacy_w, legacy_h, &w, &h);
    *out_w = w ? w : (legacy_w ? legacy_w : 640u);
    *out_h = h ? h : (legacy_h ? legacy_h : 480u);
}

static void drop_all_textures(void) {
    for (int i = 0; i < V.tex_count; i++) {
        vk_win_tex_t* e = V.tex_entries[i];
        if (!e) continue;
        if (V.vk && e->vk_tex_id) jpt_vk_renderer_destroy_texture(V.vk, e->vk_tex_id);
        e->vk_tex_id = 0;
        e->dirty = 1;
    }
}

static void bank_vk_stats(void) {
    jpt_vk_renderer_stats_t live;
    if (!V.vk) return;
    jpt_vk_renderer_get_stats(V.vk, &live);
    jpt_vk_renderer_stats_add(&V.totals, &live);
    memset(&V.stats_window_base, 0, sizeof V.stats_window_base);
}

static void release_vk_instance(void) {
    if (V.vk) {
        if (V.active_target) tsync_gpu_gone(&V.active_target->sync);
        bank_vk_stats();
        drop_all_textures();
        jpt_vk_renderer_destroy(V.vk);
        V.vk = NULL;
    }
    V.frame_open = 0;
    V.drew_3d = 0;
    V.surface_hwnd = NULL;
    V.game_hwnd = NULL;
    V.win_w = V.win_h = 0;
    V.rt_w = V.rt_h = 0;
    V.swapchain_logged = 0;
    V.fe_layer_active = 0;
    V.fe_uploaded_w = V.fe_uploaded_h = 0;
    V.fe_dirty = 1;
    V.fe_dirty_full = 1;
    V.ptr_uploaded = 0;
    V.gamma_set = 0;
    V.overlay_valid = 0;
    V.overlay_force_full = 1;
}

static int ensure_vk_renderer(HWND game_window, uint32_t legacy_w, uint32_t legacy_h) {
    HWND surf = (game_window && !V.test_headless) ? window_host_surface(game_window) : NULL;
    RECT rc;
    uint32_t cw = 0, ch = 0, iw = 0, ih = 0;

    if (V.test_headless) {
        cw = V.win_w ? V.win_w : legacy_w;
        ch = V.win_h ? V.win_h : legacy_h;
    } else {
        if (!surf || !IsWindow(surf) || IsIconic(game_window)) return 0;
        if (!GetClientRect(surf, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
        cw = (uint32_t)rc.right;
        ch = (uint32_t)rc.bottom;
    }

    if (V.vk && !V.test_headless && V.surface_hwnd != surf) release_vk_instance();

    V.win_w = cw;
    V.win_h = ch;
    choose_internal_size(legacy_w ? legacy_w : 640u, legacy_h ? legacy_h : 480u, &iw, &ih);

    if (!V.vk) {
        jpt_vk_renderer_desc_t desc;
        char err[256] = { 0 };
        const jpt_vk_context_t* ctx;

        memset(&desc, 0, sizeof desc);
        desc.window = NULL;
        desc.native_hwnd = (void*)surf;
        desc.gpu_request = V.gpu_request;
        desc.cache_path = V.cache_path[0] ? V.cache_path : NULL;
        desc.enable_validation = 0;
        desc.collect_timings = V.stats_on;
        desc.log_fn = vk_win_log_cb;
        desc.cfg.guest_w = legacy_w ? legacy_w : 640u;
        desc.cfg.guest_h = legacy_h ? legacy_h : 480u;
        desc.cfg.internal_w = iw;
        desc.cfg.internal_h = ih;
        desc.cfg.window_w = cw;
        desc.cfg.window_h = ch;
        desc.cfg.aspect_idx = V.aspect;
        desc.cfg.stretch = effective_stretch(game_window, surf);
        desc.cfg.vsync = V.vsync;
        desc.cfg.msaa = V.msaa_request ? V.msaa_request : 1u;
        desc.cfg.texture_filter = V.filter_cfg;
        desc.cfg.max_anisotropy = V.anisotropy ? V.anisotropy : 1u;
        desc.cfg.ao_level = V.ao_level;
        desc.cfg.water_refraction = V.fx_water.setting;
        desc.cfg.enhanced_sky = V.fx_sky.setting;

        V.vk = jpt_vk_renderer_create(&desc, err, sizeof err);
        if (!V.vk) {
            host_log("vulkan", "ERROR: Vulkan window renderer creation failed on HWND 0x%08X: %s",
                     (uint32_t)(uintptr_t)surf, err[0] ? err : "unknown error");
            return 0;
        }
        V.game_hwnd = game_window;
        V.surface_hwnd = surf;
        V.rt_w = iw;
        V.rt_h = ih;
        if (V.debug_flags) jpt_vk_renderer_set_debug_output(V.vk, V.debug_flags);
        jpt_vk_renderer_set_present_options(V.vk, V.vsync, V.fullscreen, effective_stretch(game_window, surf));

        ctx = jpt_vk_renderer_context(V.vk);
        if (ctx && !V.swapchain_logged) {
            V.swapchain_logged = 1;
            host_log("vulkan", "API %u.%u.%u | GPU: %s (%s, id=%s) | driver: %s %s | VRAM: %u MiB",
                     VK_API_VERSION_MAJOR(ctx->gpu.api_version), VK_API_VERSION_MINOR(ctx->gpu.api_version),
                     VK_API_VERSION_PATCH(ctx->gpu.api_version), ctx->gpu.label,
                     jpt_vk_device_type_str(ctx->gpu.device_type), ctx->gpu.stable_id,
                     ctx->gpu.driver_name[0] ? ctx->gpu.driver_name : "driver", ctx->gpu.driver_version_text,
                     (unsigned)(ctx->gpu.vram_bytes / (1024ULL * 1024ULL)));
            host_log("vulkan", "swapchain on window 0x%08X: %ux%u %s (%s)", (uint32_t)(uintptr_t)surf, cw, ch,
                     vk_format_label(jpt_vk_renderer_swapchain_format(V.vk)),
                     vk_present_mode_label(jpt_vk_renderer_present_mode(V.vk)));
            host_log("vulkan", "3D render target %ux%u (MSAA %ux, filter %s, aniso %.0fx)", iw, ih,
                     jpt_vk_renderer_active_msaa(V.vk), filter_name(V.filter_d3dr),
                     jpt_vk_renderer_active_anisotropy(V.vk));
            host_log("vulkan", "depth buffer %ux%u (%s)", iw, ih, vk_format_label(ctx->gpu.preferred_depth_format));
        }
        return 1;
    }

    if (V.vk && !V.frame_open) {
        jpt_vk_frame_geometry_t geom;
        jpt_vk_renderer_get_geometry(V.vk, &geom);
        if (geom.window_w != cw || geom.window_h != ch) {
            jpt_vk_renderer_on_resize(V.vk, cw, ch);
            host_log("vulkan", "swapchain resized to %ux%u", cw, ch);
        }
        if (legacy_w && legacy_h) jpt_vk_renderer_set_guest_size(V.vk, legacy_w, legacy_h);
        if ((iw != V.rt_w || ih != V.rt_h) && jpt_vk_renderer_set_internal_size(V.vk, iw, ih)) {
            V.rt_w = iw;
            V.rt_h = ih;
            host_log("vulkan", "3D render target resized to %ux%u", iw, ih);
        }
    }
    return 1;
}

static double acc_now(void) {
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

static void acc_enter(vka_cat_t cat) {
    if (V.stats_on) vka_enter(&V.acc, cat, acc_now());
}

static void acc_leave(void) {
    if (V.stats_on) vka_leave(&V.acc, acc_now());
}

static int renderer_ready(HWND window, uint32_t legacy_w, uint32_t legacy_h) {
    if (V.vk && V.frame_open) return 1;
    return ensure_vk_renderer(window, legacy_w, legacy_h);
}

static int ensure_frame_begun_untimed(void);
static int ensure_frame_begun(void) {
    int result_;
    acc_enter(VKA_FRAME_BEGIN);
    result_ = ensure_frame_begun_untimed();
    acc_leave();
    return result_;
}

static int ensure_frame_begun_untimed(void) {
    if (!V.vk) return 0;
    if (V.frame_open) return 1;
    if (!jpt_vk_renderer_begin_frame(V.vk)) return 0;
    V.frame_open = 1;
    V.drew_3d = 0;
    return 1;
}

/* ---- Texture translation ---- */

static int pixelformat_to_d3dt(const DDPIXELFORMAT* pf) {
    if (!pf) return D3DT_FMT_UNKNOWN;
    return d3dt_format_from_pixelformat(pf->dwFlags, pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask,
                                        pf->dwRGBAlphaBitMask);
}

static uint32_t palette_bgra_sum(const dd_surface_t* s, uint32_t* out) {
    uint32_t sum = 2166136261u;
    for (int i = 0; i < 256; i++) {
        const PALETTEENTRY* e = s->palette ? &s->palette->entries[i] : NULL;
        out[i] = e ? 0xFF000000u | ((uint32_t)e->peRed << 16) | ((uint32_t)e->peGreen << 8) | e->peBlue : 0xFF000000u;
        sum = (sum ^ out[i]) * 16777619u;
    }
    return sum;
}

static void note_untracked_writes(dd_surface_t* s) {
    static PVOID pages[VK_WIN_WATCH_PAGES];
    ULONG_PTR count = VK_WIN_WATCH_PAGES;
    ULONG granularity = 0;
    size_t bytes = (size_t)s->pitch * s->height;
    d3dt_rect_t rows = { 0, 0, (long)s->width, 0 };
    if (!s->watched || !s->exposed || !s->bits || !s->pitch) return;
    if (s->watch_frame == V.frames_hw + 1 && d3dt_rect_empty(&s->texture_dirty)) return;
    s->watch_frame = V.frames_hw + 1;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, s->bits, bytes, pages, &count, &granularity) || !count) return;
    rows.top = (long)s->height;
    if (count == VK_WIN_WATCH_PAGES) {
        rows.top = 0;
        rows.bottom = (long)s->height;
    }
    for (ULONG_PTR i = 0; i < count && count < VK_WIN_WATCH_PAGES; i++) {
        size_t off = (size_t)((BYTE*)pages[i] - s->bits);
        long first = (long)(off / (size_t)s->pitch);
        long end = (long)((off + granularity + (size_t)s->pitch - 1) / (size_t)s->pitch);
        if (first < rows.top) rows.top = first;
        if (end > rows.bottom) rows.bottom = end;
    }
    if (rows.bottom > (long)s->height) rows.bottom = (long)s->height;
    if (rows.bottom <= rows.top) return;
    d3dt_rect_union(&s->texture_dirty, &rows);
    V.tex_untracked_writes++;
}

static void log_texture_unavailable(const dd_surface_t* s, int fmt, const char* reason) {
    if (V.tex_unavailable_logged >= VK_WIN_FAIL_LOG_LIMIT) return;
    V.tex_unavailable_logged++;
    host_log("vulkan", "ERROR: texture %lux%lu format %s (R %lX G %lX B %lX A %lX, %lu bpp, caps 0x%08lX, source %s) is "
             "unavailable: %s; draws that use it are skipped and counted", (unsigned long)s->width,
             (unsigned long)s->height, d3dt_format_name(fmt), (unsigned long)s->format.dwRBitMask,
             (unsigned long)s->format.dwGBitMask, (unsigned long)s->format.dwBBitMask,
             (unsigned long)s->format.dwRGBAlphaBitMask, (unsigned long)s->format.dwRGBBitCount,
             (unsigned long)s->caps, s->bits ? "system memory" : "no pixel memory", reason);
}

/* One texture, converted exactly as the Direct3D 11 backend does (the same d3dt row converters), then uploaded. */
static int upload_surface_pixels(dd_surface_t* s, int fmt, const jpt_render_rect_t* rect, uint32_t* palette,
                                 vk_win_tex_t* e) {
    static uint32_t* rows;
    static size_t rows_cap;
    jpt_render_tex_desc_t desc;
    size_t texels = (size_t)s->width * (size_t)s->height;
    int bytes = d3dt_format_bytes(fmt);
    uint32_t x0 = rect ? (uint32_t)rect->x : 0, y0 = rect ? (uint32_t)rect->y : 0;
    uint32_t w = rect ? rect->w : s->width, h = rect ? rect->h : s->height;

    size_t chain_cap = e->levels > 1 ? d3dt_mip_chain_texels(s->width, s->height, e->levels) : texels;
    if (e->levels > 1) {
        x0 = y0 = 0;
        w = s->width;
        h = s->height;
        rect = NULL;
    }
    if (chain_cap > rows_cap) {
        uint32_t* grown = (uint32_t*)realloc(rows, chain_cap * 4u);
        if (!grown) return 0;
        rows = grown;
        rows_cap = chain_cap;
    }
    for (uint32_t y = y0; y < y0 + h; y++) {
        const BYTE* in = s->bits + (size_t)y * (size_t)s->pitch + (size_t)x0 * (size_t)bytes;
        uint32_t* out = rows + (size_t)y * s->width + x0;
        if (fmt == D3DT_FMT_PAL8)
            d3dt_convert_row_pal8(in, out, w, palette, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                                  s->src_key.dwColorSpaceHighValue);
        else
            d3dt_convert_row(fmt, in, out, w, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                             s->src_key.dwColorSpaceHighValue);
    }

    memset(&desc, 0, sizeof desc);
    desc.width = s->width;
    desc.height = s->height;
    desc.pitch = s->width * 4u;
    desc.format = JPT_TEXFMT_BGRA8888_PREPARED;
    desc.has_alpha = d3dt_format_has_alpha(fmt);
    desc.mip_levels = e->levels > 1 ? e->levels : 0;
    if (e->levels > 1) d3dt_build_mips(rows, s->width, s->height, e->levels, d3dt_alpha_kind(rows, texels));
    desc.has_colorkey = s->has_src_key ? 1 : 0;
    desc.colorkey_lo = s->src_key.dwColorSpaceLowValue;
    desc.colorkey_hi = s->src_key.dwColorSpaceHighValue;

    if (!e->vk_tex_id || !jpt_vk_renderer_texture_alive(V.vk, e->vk_tex_id)) {
        e->vk_tex_id = jpt_vk_renderer_create_texture(V.vk, &desc, rows);
        return e->vk_tex_id != 0;
    }
    return jpt_vk_renderer_update_texture(V.vk, e->vk_tex_id, &desc, rows, rect);
}

static uint64_t ensure_surface_vk_texture_untimed(dd_surface_t* s, const char** why);
static uint64_t ensure_surface_vk_texture(dd_surface_t* s, const char** why) {
    uint64_t result_;
    acc_enter(VKA_TEXTURE);
    result_ = ensure_surface_vk_texture_untimed(s, why);
    acc_leave();
    return result_;
}

static uint64_t ensure_surface_vk_texture_untimed(dd_surface_t* s, const char** why) {
    vk_win_tex_t* e;
    uint32_t palette[256];
    uint32_t palette_sum = 0;
    int fmt, full;
    jpt_render_rect_t dr;
    const jpt_render_rect_t* pdr = NULL;

    *why = NULL;
    if (!V.vk || !s) {
        *why = "no Vulkan renderer";
        return 0;
    }
    fmt = pixelformat_to_d3dt(&s->format);
    if (fmt == D3DT_FMT_UNKNOWN) {
        *why = "pixel format has no Vulkan mapping";
        log_texture_unavailable(s, fmt, *why);
        return 0;
    }
    if (!s->bits || !s->width || !s->height) {
        *why = !s->bits ? "surface has no pixel memory" : "surface has no size";
        log_texture_unavailable(s, fmt, *why);
        return 0;
    }
    if (fmt == D3DT_FMT_PAL8) palette_sum = palette_bgra_sum(s, palette);

    e = (vk_win_tex_t*)s->gpu_texture;
    if (!e) {
        if (V.tex_count >= V.tex_cap) {
            int new_cap = V.tex_cap ? V.tex_cap * 2 : VK_WIN_INIT_TEX_ENTRIES;
            vk_win_tex_t** grown = (vk_win_tex_t**)realloc(V.tex_entries, (size_t)new_cap * sizeof *grown);
            if (!grown) {
                *why = "out of host memory";
                log_texture_unavailable(s, fmt, *why);
                return 0;
            }
            V.tex_entries = grown;
            V.tex_cap = new_cap;
        }
        e = (vk_win_tex_t*)calloc(1, sizeof *e);
        if (!e) {
            *why = "out of host memory";
            return 0;
        }
        e->surface = s;
        e->dirty = 1;
        s->gpu_texture = e;
        V.tex_entries[V.tex_count++] = e;
    }

    note_untracked_writes(s);
    if (fmt == D3DT_FMT_PAL8 && e->palette_sum != palette_sum && e->vk_tex_id) {
        e->dirty = 1;
        V.tex_palette_refresh++;
    }
    if (!e->vk_tex_id) {
        e->levels = !e->dynamic && d3dt_filter_uses_mips(V.filter_d3dr) ? d3dt_mip_count(s->width, s->height) : 1;
        if (e->levels > 1) V.mipped_textures++;
    }
    full = e->dirty || !e->vk_tex_id || e->width != s->width || e->height != s->height || e->format != fmt ||
           e->has_colorkey != (s->has_src_key ? 1 : 0) ||
           (s->has_src_key && (e->ck_lo != s->src_key.dwColorSpaceLowValue || e->ck_hi != s->src_key.dwColorSpaceHighValue));
    if (!full && d3dt_rect_empty(&s->texture_dirty)) return e->vk_tex_id;

    if (e->levels > 1 && !full && d3dt_note_upload(&e->history, V.frames_hw)) {
        jpt_vk_renderer_destroy_texture(V.vk, e->vk_tex_id);
        e->vk_tex_id = 0;
        e->dynamic = 1;
        e->levels = 1;
        full = 1;
        V.dynamic_textures++;
    }
    if (e->vk_tex_id && (e->width != s->width || e->height != s->height)) {
        jpt_vk_renderer_destroy_texture(V.vk, e->vk_tex_id);
        e->vk_tex_id = 0;
        e->levels = !e->dynamic && d3dt_filter_uses_mips(V.filter_d3dr) ? d3dt_mip_count(s->width, s->height) : 1;
    }
    if (!full && e->levels <= 1) {
        d3dt_rect_t* d = &s->texture_dirty;
        if (d->left < 0) d->left = 0;
        if (d->top < 0) d->top = 0;
        if (d->right > (long)s->width) d->right = (long)s->width;
        if (d->bottom > (long)s->height) d->bottom = (long)s->height;
        if (!d3dt_rect_empty(d)) {
            dr.x = (int32_t)d->left;
            dr.y = (int32_t)d->top;
            dr.w = (uint32_t)(d->right - d->left);
            dr.h = (uint32_t)(d->bottom - d->top);
            pdr = &dr;
        }
    }

    if (!upload_surface_pixels(s, fmt, pdr, palette, e)) {
        *why = jpt_vk_texture_error_name(jpt_vk_renderer_texture_error(V.vk));
        log_texture_unavailable(s, fmt, *why);
        e->dirty = 1;
        return 0;
    }
    if (!e->width || full) V.fmt_uploads[fmt]++;
    e->width = s->width;
    e->height = s->height;
    e->format = fmt;
    e->has_colorkey = s->has_src_key ? 1 : 0;
    e->ck_lo = s->src_key.dwColorSpaceLowValue;
    e->ck_hi = s->src_key.dwColorSpaceHighValue;
    e->palette_sum = palette_sum;
    e->dirty = 0;
    memset(&s->texture_dirty, 0, sizeof s->texture_dirty);
    return e->vk_tex_id;
}

/* ---- Overlay & layer helpers ---- */

static const jpt_present_stage_cmd_t* find_plan_stage(const jpt_present_plan_t* plan, jpt_present_stage_t stage) {
    if (!plan) return NULL;
    for (int i = 0; i < plan->stage_count; i++) {
        if (plan->stages[i].stage == stage) return &plan->stages[i];
    }
    return NULL;
}

static void layer_opts_from_stage(const jpt_present_stage_cmd_t* st, const float* norm_rect, jpt_vk_layer_opts_t* out) {
    out->native_res = 1;
    out->blend_mode = st ? (int)st->blend : 0;
    out->apply_gamma = st ? st->apply_gamma : 0;
    out->point_filter = st ? (st->filter == JPT_PRESENT_FILTER_POINT) : 0;
    out->norm_rect = norm_rect;
}

static void submit_frontend_and_ui_layers_untimed(const jpt_present_plan_t* plan);
static void submit_frontend_and_ui_layers(const jpt_present_plan_t* plan) {
    acc_enter(VKA_LAYERS);
    submit_frontend_and_ui_layers_untimed(plan);
    acc_leave();
}

static void submit_frontend_and_ui_layers_untimed(const jpt_present_plan_t* plan) {
    const jpt_present_stage_cmd_t* st;
    if (!V.vk || !plan) return;

    /* Frontend HD / Loading HD layer (mutually exclusive with 640x480 UI canvas, matching D3D11) */
    if (plan->draw_frontend_hd && V.fe_pixels && V.fe_w > 0 && V.fe_h > 0) {
        size_t px = (size_t)V.fe_w * (size_t)V.fe_h;
        uint8_t* rgba = ensure_buf(&V.fe_rgba, &V.fe_rgba_cap, px * 4u);
        if (rgba) {
            int need_full = !V.fe_layer_active || V.fe_uploaded_w != V.fe_w || V.fe_uploaded_h != V.fe_h || V.fe_dirty_full;
            jpt_vk_layer_opts_t opts;
            st = find_plan_stage(plan, JPT_STAGE_FRONTEND_HD);
            layer_opts_from_stage(st, NULL, &opts);

            if (need_full) {
                jpt_bgra_to_rgba_premul(V.fe_pixels, rgba, px);
                jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_FRONTEND_HD, rgba, (uint32_t)V.fe_w, (uint32_t)V.fe_h,
                                                &opts);
                V.fe_uploaded_w = V.fe_w;
                V.fe_uploaded_h = V.fe_h;
                V.fe_layer_active = 1;
                V.fe_dirty = 0;
                V.fe_dirty_full = 0;
            } else if (V.fe_dirty && V.fe_dirty_rect.w > 0 && V.fe_dirty_rect.h > 0) {
                uint32_t x0 = (uint32_t)V.fe_dirty_rect.x;
                uint32_t y0 = (uint32_t)V.fe_dirty_rect.y;
                uint32_t rw = V.fe_dirty_rect.w;
                uint32_t rh = V.fe_dirty_rect.h;
                for (uint32_t y = y0; y < y0 + rh; y++) {
                    jpt_bgra_to_rgba_premul(V.fe_pixels + (size_t)y * (size_t)V.fe_w + x0,
                                            rgba + ((size_t)y * (size_t)V.fe_w + x0) * 4u, rw);
                }
                jpt_vk_renderer_update_layer_rect_ex(V.vk, JPT_LAYER_FRONTEND_HD, rgba, (uint32_t)V.fe_w,
                                                     (uint32_t)V.fe_h, &V.fe_dirty_rect, &opts);
                V.fe_dirty = 0;
            }
            memset(&V.fe_dirty_rect, 0, sizeof V.fe_dirty_rect);
        }
    } else {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_FRONTEND_HD);
        V.fe_layer_active = 0;
    }

    /* Frontend HD pointer sprite (drawn only with Frontend HD, premultiplied + gamma + point) */
    if (plan->draw_pointer && V.ptr_sprite && V.ptr_size > 0 && V.fe_w > 0 && V.fe_h > 0) {
        size_t px = (size_t)V.ptr_size * (size_t)V.ptr_size;
        uint8_t* rgba = ensure_buf(&V.ptr_rgba, &V.ptr_rgba_cap, px * 4u);
        if (rgba) {
            float rect[4] = {
                (float)V.ptr_x / (float)V.fe_w,
                (float)V.ptr_y / (float)V.fe_h,
                (float)(V.ptr_x + V.ptr_size) / (float)V.fe_w,
                (float)(V.ptr_y + V.ptr_size) / (float)V.fe_h,
            };
            jpt_vk_layer_opts_t opts;
            st = find_plan_stage(plan, JPT_STAGE_CURSOR);
            layer_opts_from_stage(st, rect, &opts);
            if (V.ptr_uploaded != V.ptr_gen) {
                jpt_bgra_to_rgba_premul(V.ptr_sprite, rgba, px);
                V.ptr_uploaded = V.ptr_gen;
            }
            jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_CURSOR, rgba, (uint32_t)V.ptr_size, (uint32_t)V.ptr_size,
                                            &opts);
        }
    } else {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_CURSOR);
    }

    /* 640x480 game UI canvas: fallback ONLY when Frontend HD is not drawn */
    if (plan->draw_ui_layer && V.ui_bits && V.ui_guest_w > 0 && V.ui_guest_h > 0) {
        size_t px = (size_t)JPT_UI_CANVAS_W * (size_t)JPT_UI_CANVAS_H;
        uint8_t* rgba = ensure_buf(&V.ui_rgba, &V.ui_rgba_cap, px * 4u);
        if (rgba) {
            jpt_ui_canvas_t c = jpt_ui_canvas(V.ui_guest_w, V.ui_guest_h);
            float rect[4] = {
                (float)c.rect.left / (float)V.ui_guest_w,
                (float)c.rect.top / (float)V.ui_guest_h,
                (float)c.rect.right / (float)V.ui_guest_w,
                (float)c.rect.bottom / (float)V.ui_guest_h,
            };
            jpt_vk_layer_opts_t opts;
            st = find_plan_stage(plan, JPT_STAGE_UI_LAYER);
            layer_opts_from_stage(st, rect, &opts);
            uint32_t* dst = (uint32_t*)rgba;
            init_rgb565_table();
            for (int y = 0; y < JPT_UI_CANVAS_H; y++) {
                const uint16_t* in = V.ui_bits + (size_t)y * (size_t)V.ui_pitch;
                uint32_t* out = dst + (size_t)y * JPT_UI_CANVAS_W;
                for (int x = 0; x < JPT_UI_CANVAS_W; x++)
                    out[x] = (in[x] == TSYNC_UNWRITTEN) ? 0u : V.rgb565_rgba[in[x]];
            }
            jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_LOADING_HD, rgba, JPT_UI_CANVAS_W, JPT_UI_CANVAS_H, &opts);
        }
    } else {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_LOADING_HD);
    }
}

static uint64_t overlay_tile_hash(const uint8_t* rgba, uint32_t width, uint32_t height, int tx, int ty) {
    const uint32_t size = 1u << OVERLAY_TILE_SHIFT;
    const uint32_t x0 = (uint32_t)tx << OVERLAY_TILE_SHIFT, y0 = (uint32_t)ty << OVERLAY_TILE_SHIFT;
    const uint32_t x1 = x0 + size < width ? x0 + size : width, y1 = y0 + size < height ? y0 + size : height;
    uint64_t h = 0;
    for (uint32_t y = y0; y < y1; y++) {
        const uint32_t* px = (const uint32_t*)(rgba + ((size_t)y * width + x0) * 4u);
        for (uint32_t x = 0; x < x1 - x0; x++) h = (h ^ px[x]) * 1099511628211ull;
    }
    return h;
}

static void overlay_clear_tiles(uint8_t* rgba, uint32_t width, uint32_t height, const uint8_t* mask, int tiles_w, int tiles_h) {
    const uint32_t size = 1u << OVERLAY_TILE_SHIFT;
    for (int ty = 0; ty < tiles_h; ty++)
        for (int tx = 0; tx < tiles_w; tx++) {
            uint32_t x0, x1, y0, y1;
            if (!mask[ty * tiles_w + tx]) continue;
            x0 = (uint32_t)tx << OVERLAY_TILE_SHIFT;
            y0 = (uint32_t)ty << OVERLAY_TILE_SHIFT;
            x1 = x0 + size < width ? x0 + size : width;
            y1 = y0 + size < height ? y0 + size : height;
            for (uint32_t y = y0; y < y1; y++) memset(rgba + ((size_t)y * width + x0) * 4u, 0, (size_t)(x1 - x0) * 4u);
        }
}

static unsigned overlay_tile_rects(const uint8_t* mask, int tiles_w, int tiles_h, uint32_t width, uint32_t height,
                                   jpt_render_rect_t* out, unsigned cap) {
    const int size = 1 << OVERLAY_TILE_SHIFT;
    unsigned n = 0;
    for (int ty = 0; ty < tiles_h; ty++) {
        int tx = 0;
        while (tx < tiles_w) {
            int run_end, merged = 0;
            if (!mask[ty * tiles_w + tx]) {
                tx++;
                continue;
            }
            run_end = tx;
            while (run_end < tiles_w && mask[ty * tiles_w + run_end]) run_end++;
            for (unsigned i = 0; i < n && !merged; i++) {
                const jpt_render_rect_t* r = &out[i];
                if (r->x == (int32_t)(tx * size) && r->w == (uint32_t)((run_end - tx) * size) &&
                    r->y + (int32_t)r->h == ty * size) {
                    out[i].h += (uint32_t)size;
                    merged = 1;
                }
            }
            if (!merged) {
                if (n >= cap) return cap + 1u;
                out[n].x = tx * size;
                out[n].y = ty * size;
                out[n].w = (uint32_t)((run_end - tx) * size);
                out[n].h = (uint32_t)size;
                n++;
            }
            tx = run_end;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        if ((uint32_t)out[i].x + out[i].w > width) out[i].w = width - (uint32_t)out[i].x;
        if ((uint32_t)out[i].y + out[i].h > height) out[i].h = height - (uint32_t)out[i].y;
    }
    return n;
}

static void overlay_stat_add(const vk_overlay_stat_t* d) {
    for (int k = 0; k < 2; k++) {
        vk_overlay_stat_t* t = k ? &V.overlay_all : &V.overlay_win;
        t->raster_ms += d->raster_ms;
        t->clear_ms += d->clear_ms;
        t->detect_ms += d->detect_ms;
        t->upload_ms += d->upload_ms;
        t->frames++;
        t->pixels += d->pixels;
        t->rects += d->rects;
        t->tiles += d->tiles;
        t->build_ms += d->build_ms;
        t->gpu_frames += d->gpu_frames;
    }
}

static int overlay_resize(uint32_t ow, uint32_t oh) {
    const int tw = (int)((ow + (1u << OVERLAY_TILE_SHIFT) - 1u) >> OVERLAY_TILE_SHIFT);
    const int th = (int)((oh + (1u << OVERLAY_TILE_SHIFT) - 1u) >> OVERLAY_TILE_SHIFT);
    const size_t tiles = (size_t)tw * (size_t)th;
    free(V.overlay_cpu_mask);
    free(V.overlay_new_mask);
    free(V.overlay_gpu_mask);
    free(V.overlay_upload_mask);
    free(V.overlay_hash);
    V.overlay_cpu_mask = (uint8_t*)calloc(tiles, 1);
    V.overlay_new_mask = (uint8_t*)calloc(tiles, 1);
    V.overlay_gpu_mask = (uint8_t*)calloc(tiles, 1);
    V.overlay_upload_mask = (uint8_t*)calloc(tiles, 1);
    V.overlay_hash = (uint64_t*)calloc(tiles, sizeof(uint64_t));
    V.overlay_tiles_w = tw;
    V.overlay_tiles_h = th;
    V.overlay_w = ow;
    V.overlay_h = oh;
    V.overlay_force_full = 1;
    return V.overlay_cpu_mask && V.overlay_new_mask && V.overlay_gpu_mask && V.overlay_upload_mask && V.overlay_hash;
}

static void submit_host_overlays_untimed(const jpt_present_plan_t* plan);
static void submit_host_overlays(const jpt_present_plan_t* plan) {
    acc_enter(VKA_HOST_OVERLAYS);
    submit_host_overlays_untimed(plan);
    acc_leave();
}

static void submit_host_overlays_untimed(const jpt_present_plan_t* plan) {
    const jpt_present_stage_cmd_t* st;
    uint32_t ow, oh;
    uint8_t* rgba;

    if (!V.vk) return;
    if (!plan || !plan->draw_host_overlay) {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_HOST_OVERLAYS);
        jpt_vk_renderer_clear_ui(V.vk);
        return;
    }

    ow = V.win_w ? V.win_w : (V.rt_w ? V.rt_w : 640u);
    oh = V.win_h ? V.win_h : (V.rt_h ? V.rt_h : 480u);
    if (!V.cpu_overlay) {
        dmp_gpu_draw_t gd;
        jpt_vk_ui_draw_t ud;
        vk_overlay_stat_t stat;
        double t0 = V.stats_on ? acc_now() : 0.0, t1;
        int built;
        memset(&gd, 0, sizeof gd);
        memset(&stat, 0, sizeof stat);
        built = debug_menu_overlay_gpu(&gd, (int)ow, (int)oh);
        t1 = V.stats_on ? acc_now() : 0.0;
        if (built >= 0) {
            int drawn = 0;
            if (built > 0) {
                memcpy(ud.display_pos, gd.display_pos, sizeof ud.display_pos);
                memcpy(ud.display_size, gd.display_size, sizeof ud.display_size);
                ud.target_w = gd.target_w;
                ud.target_h = gd.target_h;
                ud.vertices = gd.vertices;
                ud.vertex_count = gd.vertex_count;
                ud.indices = gd.indices;
                ud.index_count = gd.index_count;
                ud.cmds = (const jpt_vk_ui_cmd_t*)gd.cmds;
                ud.cmd_count = gd.cmd_count;
                ud.textures = (const jpt_vk_ui_texture_t*)gd.textures;
                ud.texture_count = gd.texture_count;
                drawn = jpt_vk_renderer_submit_ui(V.vk, &ud);
            }
            if (!built || drawn) {
                jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_HOST_OVERLAYS);
                if (!built) jpt_vk_renderer_clear_ui(V.vk);
                if (V.stats_on) {
                    stat.build_ms = t1 - t0;
                    stat.upload_ms = acc_now() - t1;
                    stat.gpu_frames = 1;
                    overlay_stat_add(&stat);
                }
                return;
            }
        }
        jpt_vk_renderer_clear_ui(V.vk);
    }

    rgba = ensure_buf(&V.overlay_rgba, &V.overlay_rgba_cap, (size_t)ow * (size_t)oh * 4u);
    if (!rgba) return;
    if (V.overlay_w != ow || V.overlay_h != oh || !V.overlay_valid) {
        memset(rgba, 0, (size_t)ow * (size_t)oh * 4u);
        if (!overlay_resize(ow, oh)) return;
        V.overlay_valid = 1;
    }
    {
        const size_t tiles = (size_t)V.overlay_tiles_w * (size_t)V.overlay_tiles_h;
        vk_overlay_stat_t stat;
        overlay_tiles_t tile_out;
        double t0, t1, t2, t3, t4;
        int got;
        memset(&stat, 0, sizeof stat);
        t0 = V.stats_on ? acc_now() : 0.0;
        overlay_clear_tiles(rgba, ow, oh, V.overlay_cpu_mask, V.overlay_tiles_w, V.overlay_tiles_h);
        memset(V.overlay_new_mask, 0, tiles);
        t1 = V.stats_on ? acc_now() : 0.0;
        tile_out.mask = V.overlay_new_mask;
        tile_out.tiles_w = V.overlay_tiles_w;
        tile_out.tiles_h = V.overlay_tiles_h;
        got = debug_menu_overlay_rgba(rgba, (int)ow, (int)oh, &tile_out);
        t2 = V.stats_on ? acc_now() : 0.0;
        memcpy(V.overlay_cpu_mask, V.overlay_new_mask, tiles);
        if (got) {
            jpt_vk_layer_opts_t opts;
            jpt_render_rect_t rects[64];
            unsigned count = 0;
            st = find_plan_stage(plan, JPT_STAGE_HOST_OVERLAYS);
            layer_opts_from_stage(st, NULL, &opts);
            if (V.overlay_force_full) {
                jpt_render_rect_t all = { 0, 0, (int32_t)ow, (int32_t)oh };
                t3 = V.stats_on ? acc_now() : 0.0;
                all.w = ow;
                all.h = oh;
                jpt_vk_renderer_update_layer_rect_ex(V.vk, JPT_LAYER_HOST_OVERLAYS, rgba, ow, oh, &all, &opts);
                for (size_t i = 0; i < tiles; i++)
                    V.overlay_hash[i] = overlay_tile_hash(rgba, ow, oh, (int)(i % (size_t)V.overlay_tiles_w),
                                                          (int)(i / (size_t)V.overlay_tiles_w));
                memcpy(V.overlay_gpu_mask, V.overlay_new_mask, tiles);
                V.overlay_force_full = 0;
                stat.pixels = (uint64_t)ow * oh;
                stat.rects = 1;
            } else {
                size_t changed = 0;
                for (size_t i = 0; i < tiles; i++) {
                    uint64_t h;
                    V.overlay_upload_mask[i] = 0;
                    if (!V.overlay_new_mask[i] && !V.overlay_gpu_mask[i]) continue;
                    h = overlay_tile_hash(rgba, ow, oh, (int)(i % (size_t)V.overlay_tiles_w),
                                          (int)(i / (size_t)V.overlay_tiles_w));
                    if (h != V.overlay_hash[i]) {
                        V.overlay_hash[i] = h;
                        V.overlay_upload_mask[i] = 1;
                        changed++;
                    }
                }
                memcpy(V.overlay_gpu_mask, V.overlay_new_mask, tiles);
                if (changed) count = overlay_tile_rects(V.overlay_upload_mask, V.overlay_tiles_w, V.overlay_tiles_h, ow, oh, rects, 64u);
                if (count > 64u) {
                    rects[0].x = 0;
                    rects[0].y = 0;
                    rects[0].w = ow;
                    rects[0].h = oh;
                    count = 1;
                }
                t3 = V.stats_on ? acc_now() : 0.0;
                if (count) {
                    jpt_vk_renderer_update_layer_rects(V.vk, JPT_LAYER_HOST_OVERLAYS, rgba, ow, oh, rects, count, &opts);
                    for (unsigned i = 0; i < count; i++) stat.pixels += (uint64_t)rects[i].w * rects[i].h;
                } else {
                    if (!jpt_vk_renderer_reactivate_layer(V.vk, JPT_LAYER_HOST_OVERLAYS, ow, oh, &opts)) {
                        jpt_render_rect_t all = { 0, 0, (int32_t)ow, (int32_t)oh };
                        jpt_vk_renderer_update_layer_rect_ex(V.vk, JPT_LAYER_HOST_OVERLAYS, rgba, ow, oh, &all, &opts);
                        stat.pixels = (uint64_t)ow * oh;
                    }
                }
                stat.rects = count;
                stat.tiles = changed;
            }
            t4 = V.stats_on ? acc_now() : 0.0;
            V.overlay_pixels += stat.pixels;
        } else {
            jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_HOST_OVERLAYS);
            t3 = t4 = t2;
        }
        if (V.stats_on) {
            stat.clear_ms = t1 - t0;
            stat.raster_ms = t2 - t1;
            stat.detect_ms = t3 - t2;
            stat.upload_ms = t4 - t3;
            overlay_stat_add(&stat);
        }
    }
}

static void submit_subtitle_layer_untimed(const video_subtitle_image_t* sub, int target_w, int target_h,
                                          const jpt_present_plan_t* plan) {
    const jpt_present_stage_cmd_t* st;
    if (!V.vk) return;
    if (!plan || !plan->draw_subtitle || !sub || !sub->pixels || sub->w <= 0 || sub->h <= 0 || target_w <= 0 ||
        target_h <= 0) {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_VIDEO_SUBTITLES);
        return;
    }
    size_t px = (size_t)sub->w * (size_t)sub->h;
    uint8_t* rgba = ensure_buf(&V.sub_rgba, &V.sub_rgba_cap, px * 4u);
    if (!rgba) return;
    jpt_bgra_to_rgba_premul(sub->pixels, rgba, px);
    float rect[4] = {
        (float)sub->x / (float)target_w,
        (float)sub->y / (float)target_h,
        (float)(sub->x + sub->w) / (float)target_w,
        (float)(sub->y + sub->h) / (float)target_h,
    };
    jpt_vk_layer_opts_t opts;
    st = find_plan_stage(plan, JPT_STAGE_VIDEO_SUBTITLES);
    layer_opts_from_stage(st, rect, &opts);
    jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_VIDEO_SUBTITLES, rgba, (uint32_t)sub->w, (uint32_t)sub->h, &opts);
}

static void submit_subtitle_layer(const video_subtitle_image_t* sub, int target_w, int target_h,
                                  const jpt_present_plan_t* plan) {
    acc_enter(VKA_SUBTITLES);
    submit_subtitle_layer_untimed(sub, target_w, target_h, plan);
    acc_leave();
}

static void sync_gamma(const DDGAMMARAMP* gamma) {
    if (!V.vk || !gamma) return;
    if (!V.gamma_set || memcmp(&V.last_gamma, gamma, sizeof *gamma) != 0) {
        jpt_vk_renderer_set_gamma_ramp(V.vk, gamma->red, gamma->green, gamma->blue);
        V.last_gamma = *gamma;
        V.gamma_set = 1;
    }
}

/* ---- Backend ops implementation ---- */

static void vk_win_configure(const jpt_win_renderer_cfg_t* cfg) {
    if (!cfg) return;
    V.box_w = cfg->box_w;
    V.box_h = cfg->box_h;
    V.filter_d3dr = cfg->filter;
    V.anisotropy = cfg->anisotropy;
    V.filter_cfg = map_d3dr_filter_to_cfg(cfg->filter, cfg->anisotropy);
    V.msaa_request = cfg->msaa ? cfg->msaa : 1u;
    V.vsync = cfg->vsync ? 1 : 0;
    V.stretch = cfg->stretch ? 1 : 0;
    V.aspect = cfg->aspect;
    V.ao_level = cfg->ao_level;
    if (cfg->gpu_request && cfg->gpu_request[0]) snprintf(V.gpu_request, sizeof V.gpu_request, "%s", cfg->gpu_request);
    if (cfg->cache_path && cfg->cache_path[0]) snprintf(V.cache_path, sizeof V.cache_path, "%s", cfg->cache_path);
    {
        const char* stats = getenv("JPT_VULKAN_STATS");
        const char* cpu_overlay = getenv("JPT_VULKAN_CPU_OVERLAY");
        V.stats_on = stats && stats[0] && stats[0] != '0';
        V.cpu_overlay = cpu_overlay && cpu_overlay[0] && cpu_overlay[0] != '0';
    }
    V.configured = 1;
}

static void vk_win_configure_ao(int level) {
    V.ao_level = (level > JPT_AO_OFF && level < JPT_AO_COUNT) ? level : JPT_AO_OFF;
}

static void vk_win_configure_effects(int water_refraction, int enhanced_sky) {
    fx_switch_start(&V.fx_water, water_refraction);
    fx_switch_start(&V.fx_sky, enhanced_sky);
    host_log("video", "water refraction: %s (Vulkan)", V.fx_water.setting ? "on" : "off");
    host_log("video", "enhanced sky: %s (Vulkan)", V.fx_sky.setting ? "on" : "off");
}

static int vk_win_startup(void) {
    init_rgb565_table();
    memset(&V.probe, 0, sizeof V.probe);
    if (!jpt_vk_probe_headless(V.gpu_request, &V.probe) || !V.probe.available || V.probe.selected_index < 0) {
        host_log("vulkan", "ERROR: Vulkan startup failed (%s)",
                 V.probe.summary[0] ? V.probe.summary : "no usable Vulkan GPU");
        V.active = 0;
        return 0;
    }

    const jpt_vk_gpu_info_t* g = &V.probe.gpus[V.probe.selected_index];
    memset(&V.adapter, 0, sizeof V.adapter);
    snprintf(V.adapter.description, sizeof V.adapter.description, "%s", g->name);
    V.adapter.vendor = g->vendor_id;
    V.adapter.device = g->device_id;
    V.adapter.dedicated_video_memory = (SIZE_T)g->vram_bytes;
    V.adapter.driver_version.QuadPart = (LONGLONG)g->driver_version;
    V.adapter_valid = 1;
    V.active = 1;

    host_log("renderer", "Renderer: Vulkan hardware (Direct3D 6 translated: IDirect3D3/Device3/Viewport3/Texture2)");
    host_log("vulkan", "Vulkan build provenance: bridge compiled %s %s; regional target sync v2 (32 px tiles, in-frame readback), GPU host overlay v1, video pipeline v1; shader bundle %.16s", __DATE__, __TIME__,
             jpt_vk_shader_bundle_sha256());
    if (V.box_w)
        host_log("renderer", "render box %ux%u (each target keeps its surface's shape inside it), texture filter %s%s, "
                 "MSAA requested %ux",
                 V.box_w, V.box_h, filter_name(V.filter_d3dr),
                 V.filter_d3dr == D3DR_FILTER_ANISOTROPIC ? " (anisotropic)" : "", V.msaa_request);
    else
        host_log("renderer", "render resolution original, texture filter %s, MSAA requested %ux",
                 filter_name(V.filter_d3dr), V.msaa_request);
    host_log("vulkan", "selected GPU: %s (%s, id=%s, Vulkan %u.%u.%u, VRAM %u MiB)", g->label,
             jpt_vk_device_type_str(g->device_type), g->stable_id, VK_API_VERSION_MAJOR(g->api_version),
             VK_API_VERSION_MINOR(g->api_version), VK_API_VERSION_PATCH(g->api_version),
             (unsigned)(g->vram_bytes / (1024ULL * 1024ULL)));
    host_log("graphics", "Ambient Occlusion %s%s", jpt_ao_names[V.ao_level],
             V.ao_level != JPT_AO_OFF ? ": Vulkan AO over the 3D scene, before the game's UI and the host's overlays"
                                      : ": off");
    return 1;
}

static int vk_win_hardware(void) { return V.active ? 1 : 0; }
static unsigned long long vk_win_hardware_frames(void) { return V.frames_hw; }
static const char* vk_win_name(void) { return "Vulkan"; }

static int vk_win_scene_view(float out[4]) {
    if (V.scene_frame[2] > 0.0f) {
        memcpy(V.scene_view, V.scene_frame, sizeof V.scene_view);
        memset(V.scene_frame, 0, sizeof V.scene_frame);
        V.scene_frame_area = 0;
    }
    memcpy(out, V.scene_view, sizeof V.scene_view);
    return V.scene_view[2] > 0.0f && V.scene_view[3] > 0.0f;
}

static void vk_win_render_box(unsigned* w, unsigned* h) {
    if (w) *w = V.box_w;
    if (h) *h = V.box_h;
}

static int vk_win_frame_size(int guest_w, int guest_h, int* w, int* h) {
    uint32_t iw, ih;
    if (guest_w <= 0 || guest_h <= 0) return 0;
    choose_internal_size((uint32_t)guest_w, (uint32_t)guest_h, &iw, &ih);
    if (w) *w = (int)iw;
    if (h) *h = (int)ih;
    return 1;
}

static int vk_win_set_debug_output(const char* mode) {
    if (!mode) return 0;
    if (!strcmp(mode, "texture")) V.debug_flags = 128u;
    else if (!strcmp(mode, "vertex-color")) V.debug_flags = 256u;
    else if (!strcmp(mode, "depth")) V.debug_flags = 512u;
    else return 0;
    if (V.vk) jpt_vk_renderer_set_debug_output(V.vk, V.debug_flags);
    return 1;
}

static void vk_win_sky_begin(const float sky[5]) {
    if (!sky) return;
    memcpy(V.sky_constants, sky, sizeof V.sky_constants);
    V.sky_usable = fx_sky_usable(sky[3], sky[4], sky[2]);
    V.sky_drawing = 1;
}

static void vk_win_sky_end(void) { V.sky_drawing = 0; }

static void stats_snapshot(jpt_vk_renderer_stats_t* out) {
    jpt_vk_renderer_stats_t live;
    *out = V.totals;
    if (V.vk) {
        memset(&live, 0, sizeof live);
        jpt_vk_renderer_get_stats(V.vk, &live);
        jpt_vk_renderer_stats_add(out, &live);
    }
}

static const char* const k_zfunc_names[9] = { "?", "never", "less", "equal", "lequal", "greater", "notequal", "gequal",
                                              "always" };
static const char* const k_blend_names[14] = { "?",         "zero",       "one",        "srccolor",   "invsrccolor",
                                               "srcalpha",  "invsrcalpha", "dstalpha",  "invdstalpha", "dstcolor",
                                               "invdstcolor", "srcalphasat", "bothsrcalpha", "bothinvsrcalpha" };
static const char* const k_prim_names[7] = { "?", "points", "lines", "linestrip", "trilist", "tristrip", "trifan" };
static const char* const k_cull_names[4] = { "?", "none", "cw", "ccw" };

static void log_histogram(const char* label, const unsigned long long* counts, const char* const* names, size_t n) {
    char line[384];
    size_t used = 0;
    line[0] = 0;
    for (size_t i = 0; i < n; i++) {
        int w;
        if (!counts[i]) continue;
        w = _snprintf(line + used, sizeof line - used - 1, "%s%s %llu", used ? ", " : "", names[i], counts[i]);
        if (w < 0) break;
        used += (size_t)w;
    }
    if (used) host_log("vulkan", "%s: %s", label, line);
}

static void log_u64_histogram(const char* label, const uint64_t* counts, const char* const* names, size_t n) {
    unsigned long long copy[16];
    if (n > 16) n = 16;
    for (size_t i = 0; i < n; i++) copy[i] = counts[i];
    log_histogram(label, copy, names, n);
}

static void log_evidence_summary(const jpt_vk_renderer_stats_t* st) {
    host_log("vulkan", "evidence: ambient occlusion %llu passes executed (requested level %s), water %llu requested / "
             "%llu Vulkan draws (%llu without a wave link, %llu wave uploads), enhanced sky %llu requested / %llu "
             "Vulkan draws", (unsigned long long)st->ao_passes, jpt_ao_names[V.ao_level], V.water_requested,
             (unsigned long long)st->water_draws, V.water_no_link, V.water_wave_uploads, V.sky_requested,
             (unsigned long long)st->sky_draws);
}

static double thread_cpu_ms(void);
static void log_sync_reasons(const char* label, vk_sync_stat_t* stats, unsigned long long frames, int reset);
static void log_account(const char* label, const vka_t* d, double wall_ms, double cpu_ms,
                        const jpt_vk_renderer_stats_t* now, const jpt_vk_renderer_stats_t* base, unsigned long long frames,
                        const double* sections_base);

static void vk_win_summary(void) {
    jpt_vk_renderer_stats_t st;
    unsigned long refused_render = 0, refused_stage = 0, failed = 0;
    unsigned long long skipped_total = 0;
    unsigned long long bridge_skipped = 0;
    stats_snapshot(&st);
    d3d_objects_counts(&refused_render, &refused_stage, &failed);
    for (int i = 0; i < BR_SKIP_COUNT; i++) bridge_skipped += V.bridge_skips[i];
    for (int i = 1; i < JPT_VK_SKIP_REASONS; i++) skipped_total += st.draws_skipped[i];
    skipped_total += bridge_skipped;

    host_log("vulkan", "summary: %llu hw frames, %llu 2D frames, %llu guest draws, %llu Vulkan draws (%llu draw calls "
             "after merging %llu), %llu skipped, %llu triangles, %llu clears", V.frames_hw, V.frames_dib, V.guest_draws,
             (unsigned long long)st.draws, (unsigned long long)st.draw_calls_recorded,
             (unsigned long long)st.draw_calls_merged, skipped_total, (unsigned long long)st.triangles,
             (unsigned long long)st.clears);
    host_log("vulkan", "resources: %llu textures created (%llu standalone), %llu updated, %.1f MiB uploaded, %llu "
             "pipelines created, %llu pipeline cache hits, %llu descriptor updates, %llu texture-full batch flushes, "
             "%llu scene passes, %llu one-shot submits, %llu readbacks",
             (unsigned long long)st.textures_created, (unsigned long long)st.standalone_textures_created,
             (unsigned long long)st.texture_updates, (double)st.texture_upload_bytes / (1024.0 * 1024.0),
             (unsigned long long)st.pipelines_created, (unsigned long long)st.pipeline_cache_hits,
             (unsigned long long)st.descriptor_updates, (unsigned long long)st.batch_flushes_texture_full,
             (unsigned long long)st.scene_passes, (unsigned long long)st.one_shot_submits, V.readbacks);
    log_evidence_summary(&st);
    host_log("vulkan", "mipmaps: %llu textures with generated chains, %llu of them became dynamic (level 0 only)",
             V.mipped_textures, V.dynamic_textures);
    host_log("vulkan", "GPU polygons (perspective-textured and bump-mapped software primitives): %llu drawn by the GPU, %llu "
             "uploads of %.1f MiB, %llu failed uploads, %llu failed draws, %llu without room in the constants ring",
             V.poly.drawn, (unsigned long long)st.poly_uploads, (double)st.poly_upload_bytes / (1024.0 * 1024.0),
             V.poly.failed_uploads, V.poly.failed_draws, (unsigned long long)st.poly_failures);
    {
        unsigned long long bridge[BR_SKIP_COUNT];
        for (int i = 0; i < BR_SKIP_COUNT; i++) bridge[i] = V.bridge_skips[i];
        log_histogram("skipped draws (bridge)", bridge, k_skip_names, BR_SKIP_COUNT);
        {
            const char* names[JPT_VK_SKIP_REASONS];
            unsigned long long rend[JPT_VK_SKIP_REASONS];
            for (int i = 0; i < JPT_VK_SKIP_REASONS; i++) {
                names[i] = jpt_vk_skip_reason_name((jpt_vk_draw_result_t)i);
                rend[i] = i ? st.draws_skipped[i] : 0;
            }
            log_histogram("skipped draws (renderer)", rend, names, JPT_VK_SKIP_REASONS);
        }
        {
            const char* names[JPT_VK_TEX_ERRORS];
            unsigned long long fail[JPT_VK_TEX_ERRORS];
            for (int i = 0; i < JPT_VK_TEX_ERRORS; i++) {
                names[i] = jpt_vk_texture_error_name((jpt_vk_texture_error_t)i);
                fail[i] = i ? st.texture_failures[i] : 0;
            }
            log_histogram("texture failures", fail, names, JPT_VK_TEX_ERRORS);
        }
        {
            const char* names[D3DT_FMT_COUNT];
            for (int i = 0; i < D3DT_FMT_COUNT; i++) names[i] = d3dt_format_name(i);
            log_histogram("texture formats uploaded", V.fmt_uploads, names, D3DT_FMT_COUNT);
        }
    }
    log_u64_histogram("cull modes", st.cull_draws, k_cull_names, 4);
    log_u64_histogram("depth functions", st.zfunc_draws, k_zfunc_names, 9);
    log_u64_histogram("alpha-test functions", st.alpha_func_draws, k_zfunc_names, 9);
    log_u64_histogram("source blend factors", st.src_blend_draws, k_blend_names, 14);
    log_u64_histogram("destination blend factors", st.dst_blend_draws, k_blend_names, 14);
    log_u64_histogram("primitive types", st.prim_draws, k_prim_names, 7);
    host_log("vulkan", "draw kinds: %llu textured, %llu indexed, %llu alpha-tested, %llu alpha-blended, %llu fogged, "
             "%llu colour-keyed; %llu texture rows written without Lock/Unlock, %llu palette refreshes",
             (unsigned long long)st.textured_draws, (unsigned long long)st.indexed_draws,
             (unsigned long long)st.alpha_test_draws, (unsigned long long)st.alpha_blend_draws,
             (unsigned long long)st.fog_draws, (unsigned long long)st.colorkey_draws, V.tex_untracked_writes,
             V.tex_palette_refresh);
    for (unsigned i = 0; i < V.fvf_count; i++)
        host_log("vulkan", "vertex format 0x%lX: %llu draws", (unsigned long)V.fvf_seen[i].fvf, V.fvf_seen[i].draws);
    if (V.stats_on && V.frames_hw + V.frames_dib) {
        double frames = (double)(V.frames_hw + V.frames_dib);
        double gs = st.gpu_samples ? (double)st.gpu_samples : 1.0;
        host_log("vulkan", "timings per frame over %.0f frames: CPU frame %.2f ms (draw recording %.2f, pipelines %.2f, "
                 "texture upload %.2f, descriptors %.2f, present/submit %.2f); GPU %.2f ms over %llu samples (scene %.2f, "
                 "AO %.2f, layers %.2f, present %.2f, post %.2f)", frames, st.cpu_frame_ms / frames,
                 st.cpu_draw_ms / frames, st.cpu_pipeline_ms / frames, st.cpu_upload_ms / frames,
                 st.cpu_descriptor_ms / frames, st.cpu_present_ms / frames,
                 st.gpu_samples ? st.gpu_total_ms / gs : 0.0, (unsigned long long)st.gpu_samples,
                 st.gpu_samples ? st.gpu_scene_ms / gs : 0.0, st.gpu_samples ? st.gpu_ao_ms / gs : 0.0,
                 st.gpu_samples ? st.gpu_layers_ms / gs : 0.0, st.gpu_samples ? st.gpu_present_ms / gs : 0.0,
                 st.gpu_samples ? st.gpu_post_ms / gs : 0.0);
    }
    if (V.stats_on && V.total_wall_start_ms > 0.0 && V.frames_hw + V.frames_dib) {
        jpt_vk_renderer_stats_t zero;
        double sections[16] = { 0 };
        memset(&zero, 0, sizeof zero);
        log_account("account over the whole run", &V.acc, acc_now() - V.total_wall_start_ms,
                    thread_cpu_ms() - V.total_cpu_start_ms, &st, &zero, V.frames_hw + V.frames_dib, sections);
    }
    if (V.stats_on) log_sync_reasons("account over the whole run", V.sync_all, V.frames_hw + V.frames_dib, 0);
    engine_limits_summary(V.draw_calls);
    host_log("d3d", "unsupported_render_states %lu", refused_render);
    host_log("d3d", "unsupported_texture_stage_states %lu", refused_stage);
    host_log("d3d", "failed_d3d_calls %lu", failed);
}

static void note_effect_evidence(void) {
    jpt_vk_renderer_stats_t live;
    if (!V.vk) return;
    jpt_vk_renderer_get_stats(V.vk, &live);
    if (!V.ao_executed_logged && live.ao_passes) {
        V.ao_executed_logged = 1;
        host_log("vulkan", "Ambient Occlusion %s executed on Vulkan (first pass over %ux%u)", jpt_ao_names[V.ao_level],
                 V.rt_w, V.rt_h);
    }
    if (!V.water_executed_logged && live.water_draws) {
        V.water_executed_logged = 1;
        host_log("vulkan", "Water Refraction executed on Vulkan (%llu draws, %dx%d wave field)",
                 (unsigned long long)live.water_draws, V.water_w, V.water_h);
    }
    if (!V.sky_executed_logged && live.sky_draws) {
        V.sky_executed_logged = 1;
        host_log("vulkan", "Enhanced Sky executed on Vulkan (%llu draws)", (unsigned long long)live.sky_draws);
    }
}

static double thread_cpu_ms(void) {
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) / 10000.0;
}

static void sections_now(double* out) {
    int n = vkp_guest_section_count();
    for (int i = 0; i < 16; i++) out[i] = i < n ? vkp_guest_section_ms(i) : 0.0;
}

static void log_guest_sections(const double* base, double frames) {
    char line[900];
    size_t used = 0;
    int n = vkp_guest_section_count();
    if (n > 16) n = 16;
    line[0] = 0;
    for (int i = 0; i < n; i++) {
        double ms = (vkp_guest_section_ms(i) - base[i]) / frames;
        int w;
        if (ms < 0.005) continue;
        w = _snprintf(line + used, sizeof line - used - 1, "%s%s %.2f", used ? "; " : "", vkp_guest_section_name(i), ms);
        if (w < 0) break;
        used += (size_t)w;
    }
    if (used) host_log("vulkan", "guest sections per frame (they include the backend calls made inside them): %s", line);
}

static void log_account(const char* label, const vka_t* d, double wall_ms, double cpu_ms,
                        const jpt_vk_renderer_stats_t* now, const jpt_vk_renderer_stats_t* base, unsigned long long frames,
                        const double* sections_base) {
    vka_split_t sp;
    char cats[512];
    size_t used = 0;
    double f = frames ? (double)frames : 1.0;
    double sync_ms = now->cpu_sync_ms - base->cpu_sync_ms, shot_ms = now->cpu_one_shot_ms - base->cpu_one_shot_ms;
    double acquire_ms = now->cpu_acquire_ms - base->cpu_acquire_ms, present_ms = now->cpu_present_ms - base->cpu_present_ms;
    cats[0] = 0;
    for (int i = 0; i < VKA_COUNT; i++) {
        int w;
        if (d->ms[i] / f < 0.005) continue;
        w = _snprintf(cats + used, sizeof cats - used - 1, "%s%s %.2f", used ? ", " : "", vka_name((vka_cat_t)i), d->ms[i] / f);
        if (w < 0) break;
        used += (size_t)w;
    }
    vka_split(wall_ms, cpu_ms, vka_total_ms(d), sync_ms + shot_ms + acquire_ms + present_ms, frames, &sp);
    host_log("vulkan", "%s: %.2f ms per frame over %llu frames = %.2f guest game code and host hooks + %.2f backend (%s); "
             "thread CPU %.2f ms, blocked %.2f ms of which timed renderer waits %.2f (frame syncs %.2f in %llu, one-shot "
             "submits %.2f in %llu, acquire %.2f, present %.2f) and unaccounted %.2f ms (%.1f%% of the frame)",
             label, sp.frame_ms, (unsigned long long)frames, sp.guest_ms, sp.backend_ms, cats, sp.cpu_ms, sp.blocked_ms,
             sp.waits_ms, sync_ms / f, (unsigned long long)(now->frame_syncs - base->frame_syncs), shot_ms / f,
             (unsigned long long)(now->one_shot_submits - base->one_shot_submits), acquire_ms / f, present_ms / f,
             sp.unaccounted_ms, sp.unaccounted_pct);
    host_log("vulkan", "%s: GPU polygons drawn %llu (%llu uploads); mid-frame readbacks %llu, front-buffer readbacks %llu (%.2f ms per frame), last-frame copies "
             "%llu; largest waits: frame sync %.2f ms, one-shot submit %.2f ms, readback %.2f ms; GPU frames without a "
             "timestamp sample %llu", label, (unsigned long long)(now->poly_draws - base->poly_draws),
             (unsigned long long)(now->poly_uploads - base->poly_uploads),
             (unsigned long long)(now->scene_readbacks - base->scene_readbacks),
             (unsigned long long)(now->last_frame_readbacks - base->last_frame_readbacks),
             (now->cpu_readback_ms - base->cpu_readback_ms) / f,
             (unsigned long long)(now->last_frame_copies - base->last_frame_copies), now->cpu_sync_max_ms,
             now->cpu_one_shot_max_ms, now->cpu_readback_max_ms,
             (unsigned long long)(now->gpu_frames_unsampled - base->gpu_frames_unsampled));
    {
        static const char* const names[JPT_VK_OS_COUNT] = { "texture upload", "polygon upload", "resource init", "readback" };
        char line[320];
        size_t used = 0;
        line[0] = 0;
        for (int i = 0; i < JPT_VK_OS_COUNT; i++) {
            unsigned long long n = now->one_shot_by_reason[i] - base->one_shot_by_reason[i];
            int w;
            if (!n) continue;
            w = _snprintf(line + used, sizeof line - used - 1, "%s%s %llu (%.2f ms)", used ? ", " : "", names[i], n,
                          now->one_shot_ms_by_reason[i] - base->one_shot_ms_by_reason[i]);
            if (w < 0) break;
            used += (size_t)w;
        }
        host_log("vulkan", "%s: one-shot submits by reason: %s; texture uploads recorded in the frame %llu; one-shot fallbacks: outside a frame %llu, staging full %llu; extra staging buffers %llu",
                 label, used ? line : "none", (unsigned long long)(now->uploads_in_frame - base->uploads_in_frame),
                 (unsigned long long)(now->upload_fallback_no_frame - base->upload_fallback_no_frame),
                 (unsigned long long)(now->upload_fallback_staging_full - base->upload_fallback_staging_full),
                 (unsigned long long)(now->staging_extra_buffers - base->staging_extra_buffers));
    }
    {
#define BRK(i) (unsigned long long)(now->pass_breaks[i] - base->pass_breaks[i])
        host_log("vulkan", "%s: scene pass breaks by reason: texture upload %llu, polygon upload %llu, layer upload %llu, frame sync %llu, readback %llu, water %llu, CPU overlay composite %llu (%llu batches), frame end %llu, other %llu (%.2f scene passes per frame)",
                 label, BRK(JPT_VK_BREAK_TEXTURE_UPLOAD), BRK(JPT_VK_BREAK_POLY_UPLOAD), BRK(JPT_VK_BREAK_LAYER_UPLOAD),
                 BRK(JPT_VK_BREAK_SYNC), BRK(JPT_VK_BREAK_READBACK), BRK(JPT_VK_BREAK_WATER), BRK(JPT_VK_BREAK_OVERLAY),
                 (unsigned long long)(now->overlay_batches - base->overlay_batches), BRK(JPT_VK_BREAK_FRAME_END),
                 BRK(JPT_VK_BREAK_OTHER), (double)(now->scene_passes - base->scene_passes) / f);
#undef BRK
    }
    log_guest_sections(sections_base, f);
}

static void log_sync_reasons(const char* label, vk_sync_stat_t* stats, unsigned long long frames, int reset) {
    char line[640];
    size_t used = 0;
    double f = frames ? (double)frames : 1.0;
    line[0] = 0;
    for (int i = 0; i < VSR_COUNT; i++) {
        const vk_sync_stat_t* st = &stats[i];
        int w;
        if (!st->calls) continue;
        w = _snprintf(line + used, sizeof line - used - 1, "%s%s %llu syncs %.2f ms/frame (%llu readbacks, %.1f MB read of %.1f MB requested; %llu uploads)",
                      used ? "; " : "", k_sync_reason_names[i], st->calls, st->ms / f, st->readbacks,
                      (double)st->read_px * 2.0 / 1048576.0, (double)st->requested_px * 2.0 / 1048576.0, st->uploads);
        if (w < 0) break;
        used += (size_t)w;
    }
    if (used) host_log("vulkan", "%s: target sync by reason: %s", label, line);
    host_log("vulkan", "%s: target tiles (32 px) since start: %llu stale in requested regions, %llu read from the GPU in %llu copy regions (%.1f MiB), %llu filled with the clear colour, %llu kept valid (%.1f MiB not read), %llu Lock reads needing nothing; readbacks by caller: polygon run %llu, text %llu, other %llu",
                 label, V.tiles_stale_total, V.tiles_read, V.tile_rects, (double)V.tile_read_pixels * 2.0 / 1048576.0,
                 V.tiles_filled, V.tiles_kept, (double)V.tiles_kept * 2048.0 / 1048576.0, V.tile_reads_avoided,
                 V.tile_ctx_reads[2], V.tile_ctx_reads[1], V.tile_ctx_reads[0]);
    {
        unsigned order[48], n = V.callers_n;
        char line[900];
        size_t used = 0;
        for (unsigned i = 0; i < n; i++) order[i] = i;
        for (unsigned i = 0; i < n; i++)
            for (unsigned j = i + 1; j < n; j++)
                if (V.callers[order[j]].ms > V.callers[order[i]].ms) {
                    const unsigned tmp = order[i];
                    order[i] = order[j];
                    order[j] = tmp;
                }
        line[0] = 0;
        for (unsigned i = 0; i < n && i < 10u; i++) {
            const vk_caller_stat_t* c = &V.callers[order[i]];
            const int w = _snprintf(line + used, sizeof line - used - 1, "%ssub_%08X %llu accesses, %llu readbacks, %.1f MiB, %.1f ms",
                                    used ? "; " : "", c->caller, c->accesses, c->readbacks, (double)c->read_px * 2.0 / 1048576.0, c->ms);
            if (w < 0) break;
            used += (size_t)w;
        }
        if (used) host_log("vulkan", "%s: top Lock callers by wait: %s", label, line);
        for (int k = 0; k < 2; k++) {
            const vk_water_stat_t* w = &V.water_stat[k];
            if (w->frames)
                host_log("vulkan", "%s: water %s over %llu frames: %.2f Locks, %.3f readbacks, %.2f ms target sync per frame", label,
                         k ? "visible" : "hidden", w->frames, (double)w->locks / (double)w->frames,
                         (double)w->readbacks / (double)w->frames, w->sync_ms / (double)w->frames);
        }
    }
    host_log("vulkan", "%s: deferred Locks (first-touch): %llu armed, %llu touched, %llu ended untouched", label, V.deferred_locks, V.deferred_touched, V.deferred_untouched);
    host_log("vulkan", "%s: regional sync: %llu CPU access epochs, %.2f Locks per epoch, %llu Locks needing GPU data, %.2f readbacks per epoch; Flips %llu (no invalidation); full tile invalidations: clear colour change %llu, readback failure %llu, attach %llu",
             label, V.cpu_epochs, V.cpu_epochs ? (double)V.epoch_locks / (double)V.cpu_epochs : 0.0, V.epoch_locks_gpu,
             V.cpu_epochs ? (double)V.sync_readbacks / (double)V.cpu_epochs : 0.0, V.tile_flips,
             V.tile_full_invalidations[1], V.tile_full_invalidations[2], V.tile_full_invalidations[3]);
    {
        const vk_overlay_stat_t* o = reset ? &V.overlay_win : &V.overlay_all;
        if (o->gpu_frames) {
            jpt_vk_renderer_stats_t ui;
            stats_snapshot(&ui);
            host_log("vulkan", "%s: host overlay mode gpu over %llu frames: ImGui build %.2f ms, command submit %.2f ms per frame; since start %.1f draw commands, %.0f vertices, %.0f indices, %.1f texture binds per frame and %llu texture uploads",
                     label, o->gpu_frames, o->build_ms / (double)o->gpu_frames, o->upload_ms / (double)o->gpu_frames,
                     ui.ui_frames ? (double)ui.ui_draws / (double)ui.ui_frames : 0.0,
                     ui.ui_frames ? (double)ui.ui_vertices / (double)ui.ui_frames : 0.0,
                     ui.ui_frames ? (double)ui.ui_indices / (double)ui.ui_frames : 0.0,
                     ui.ui_frames ? (double)ui.ui_texture_binds / (double)ui.ui_frames : 0.0,
                     (unsigned long long)ui.ui_texture_uploads);
        }
        if (o->frames)
            host_log("vulkan", "%s: host overlay per frame: raster %.2f ms, clear %.2f ms, change detection %.2f ms, upload %.2f ms; %.0f pixels (%.2f MiB), %.1f rects, %.1f tiles over %llu frames",
                     label, o->raster_ms / (double)o->frames, o->clear_ms / (double)o->frames, o->detect_ms / (double)o->frames,
                     o->upload_ms / (double)o->frames, (double)o->pixels / (double)o->frames,
                     (double)o->pixels * 4.0 / (double)o->frames / 1048576.0, (double)o->rects / (double)o->frames,
                     (double)o->tiles / (double)o->frames, o->frames);
        if (reset) memset(&V.overlay_win, 0, sizeof V.overlay_win);
    }
    if (reset) memset(stats, 0, VSR_COUNT * sizeof *stats);
}

static void log_stats_window(void) {
    jpt_vk_renderer_stats_t now;
    static ULONGLONG last_tick;
    ULONGLONG tick = GetTickCount64();
    double frames, draws;
    if (!V.stats_on) return;
    if (!last_tick) {
        last_tick = tick;
        V.win_wall_base_ms = acc_now();
        V.win_cpu_base_ms = thread_cpu_ms();
        sections_now(V.win_sections_base);
        V.total_wall_start_ms = V.win_wall_base_ms;
        V.total_cpu_start_ms = V.win_cpu_base_ms;
    }
    V.stats_window_frames++;
    if (tick - last_tick < 5000) return;
    stats_snapshot(&now);
    frames = (double)V.stats_window_frames;
    draws = (double)(now.draw_calls_requested - V.stats_window_base.draw_calls_requested);
    double gs = now.gpu_samples > V.stats_window_base.gpu_samples
                    ? (double)(now.gpu_samples - V.stats_window_base.gpu_samples)
                    : 0.0;
    host_log("vulkan", "stats %.0f frames in %.1f s: %.0f guest draws and %.0f draw calls per frame, CPU frame %.2f ms "
             "(draws %.2f, pipelines %.2f, uploads %.2f, descriptors %.2f, present %.2f), GPU %.2f ms (scene %.2f, "
             "AO %.2f, layers %.2f, present %.2f), %llu textures created, %llu updated", frames,
             (double)(tick - last_tick) / 1000.0, draws / frames,
             (double)(now.draw_calls_recorded - V.stats_window_base.draw_calls_recorded) / frames,
             (now.cpu_frame_ms - V.stats_window_base.cpu_frame_ms) / frames,
             (now.cpu_draw_ms - V.stats_window_base.cpu_draw_ms) / frames,
             (now.cpu_pipeline_ms - V.stats_window_base.cpu_pipeline_ms) / frames,
             (now.cpu_upload_ms - V.stats_window_base.cpu_upload_ms) / frames,
             (now.cpu_descriptor_ms - V.stats_window_base.cpu_descriptor_ms) / frames,
             (now.cpu_present_ms - V.stats_window_base.cpu_present_ms) / frames,
             gs > 0.0 ? (now.gpu_total_ms - V.stats_window_base.gpu_total_ms) / gs : 0.0,
             gs > 0.0 ? (now.gpu_scene_ms - V.stats_window_base.gpu_scene_ms) / gs : 0.0,
             gs > 0.0 ? (now.gpu_ao_ms - V.stats_window_base.gpu_ao_ms) / gs : 0.0,
             gs > 0.0 ? (now.gpu_layers_ms - V.stats_window_base.gpu_layers_ms) / gs : 0.0,
             gs > 0.0 ? (now.gpu_present_ms - V.stats_window_base.gpu_present_ms) / gs : 0.0,
             (unsigned long long)(now.textures_created - V.stats_window_base.textures_created),
             (unsigned long long)(now.texture_updates - V.stats_window_base.texture_updates));
    {
        vka_t window;
        double wall_now = acc_now(), cpu_now = thread_cpu_ms();
        vka_delta(&V.acc, &V.acc_base, &window);
        log_account("account", &window, wall_now - V.win_wall_base_ms, cpu_now - V.win_cpu_base_ms, &now,
                    &V.stats_window_base, V.stats_window_frames, V.win_sections_base);
        log_sync_reasons("account", V.sync_win, V.stats_window_frames, 1);
        V.acc_base = V.acc;
        V.win_wall_base_ms = wall_now;
        V.win_cpu_base_ms = cpu_now;
        sections_now(V.win_sections_base);
    }
    V.stats_window_base = now;
    V.stats_window_frames = 0;
    last_tick = tick;
}

static void vk_win_device_lost(void) {
    drop_deferred(NULL);
    release_vk_instance();
}

static HRESULT vk_win_attach(d3d_device_t* dev) {
    vk_win_target_t* t;
    if (!dev || !dev->target) return DDERR_INVALIDPARAMS;
    t = (vk_win_target_t*)calloc(1, sizeof *t);
    if (!t) return DDERR_OUTOFMEMORY;
    t->device = dev;
    t->surface = dev->target;
    t->legacy_w = dev->target->width;
    t->legacy_h = dev->target->height;
    t->reference = (uint16_t*)malloc((size_t)t->legacy_w * t->legacy_h * sizeof(uint16_t));
    t->scratch = (uint32_t*)malloc((size_t)t->legacy_w * t->legacy_h * sizeof(uint32_t));
    t->tiles_x = (t->legacy_w + (1u << VK_TILE_SHIFT) - 1u) >> VK_TILE_SHIFT;
    t->tiles_y = (t->legacy_h + (1u << VK_TILE_SHIFT) - 1u) >> VK_TILE_SHIFT;
    t->tile_set[0] = (uint8_t*)malloc(tile_count(t));
    t->tile_set[1] = (uint8_t*)malloc(tile_count(t));
    t->tile_drawn = (uint8_t*)malloc(tile_count(t));
    t->tile_work = (uint8_t*)calloc(tile_count(t), 1);
    if (!t->reference || !t->scratch || !t->tile_set[0] || !t->tile_set[1] || !t->tile_drawn || !t->tile_work) {
        free(t->reference);
        free(t->scratch);
        free(t->tile_set[0]);
        free(t->tile_set[1]);
        free(t->tile_drawn);
        free(t->tile_work);
        free(t);
        return DDERR_OUTOFMEMORY;
    }
    memset(t->tile_set[0], 1, tile_count(t));
    memset(t->tile_set[1], 1, tile_count(t));
    memset(t->tile_drawn, 1, tile_count(t));
    t->tile_stale = t->tile_set[0];
    if (!V.tiles_logged) {
        V.tiles_logged = 1;
        host_log("vulkan", "regional target sync enabled: %d px tiles, %ux%u tiles for the %ux%u target", 1 << VK_TILE_SHIFT,
                 t->tiles_x, t->tiles_y, t->legacy_w, t->legacy_h);
    }
    tsync_attach(&t->sync);
    choose_internal_size(t->legacy_w, t->legacy_h, &t->internal_w, &t->internal_h);
    dev->backend = (d3dr_target_t*)t;
    dev->target->target = (d3dr_target_t*)t;
    V.active_target = t;
    if (dev->dd && (dev->dd->window || V.test_headless))
        ensure_vk_renderer(dev->dd->window, t->legacy_w, t->legacy_h);
    return DD_OK;
}

static void vk_win_detach(d3d_device_t* dev) {
    vk_win_target_t* t;
    if (!dev || !dev->backend) return;
    t = (vk_win_target_t*)dev->backend;
    drop_deferred(t);
    end_polygon_run(t);
    if (t->surface && t->surface->target == dev->backend) t->surface->target = NULL;
    if (V.active_target == t) V.active_target = NULL;
    free(t->reference);
    free(t->scratch);
    free(t->tile_set[0]);
    free(t->tile_set[1]);
    free(t->tile_drawn);
    free(t->tile_work);
    free(t);
    dev->backend = NULL;
}

static void vk_win_begin_scene_untimed(d3d_device_t* dev);
static void vk_win_begin_scene(d3d_device_t* dev) {
    acc_enter(VKA_OTHER);
    vk_win_begin_scene_untimed(dev);
    acc_leave();
}

static void vk_win_begin_scene_untimed(d3d_device_t* dev) {
    vk_win_target_t* t = dev ? (vk_win_target_t*)dev->backend : NULL;
    settle_deferred();
    if (!t || !dev->dd || (!dev->dd->window && !V.test_headless)) return;
    if (!ensure_vk_renderer(dev->dd->window, t->legacy_w, t->legacy_h)) return;
    t->internal_w = V.rt_w;
    t->internal_h = V.rt_h;
    t->frame_started = 1;
    ensure_frame_begun();
}

static void vk_win_end_scene(d3d_device_t* dev) { (void)dev; }
static void vk_win_state_will_change(d3d_device_t* dev, int change, DWORD state, DWORD value) {
    (void)dev; (void)change; (void)state; (void)value;
}
static void vk_win_state_kept_batch(d3d_device_t* dev) { (void)dev; }

/* ---- the hybrid picture: the game rasterises some polygons and text into its 16-bit copy of the target ---- */

static d3dt_rect_t whole_target(const vk_win_target_t* t) {
    d3dt_rect_t r = { 0, 0, (long)t->legacy_w, (long)t->legacy_h };
    return r;
}

static int target_usable(const vk_win_target_t* t) {
    return t && t->surface && t->surface->bits && t->reference && t->surface->pitch > 0 && t->legacy_w && t->legacy_h;
}

static void fill_copy(vk_win_target_t* t, const d3dt_rect_t* r, uint16_t value) {
    if (!target_usable(t)) return;
    tiles_select(t);
    tiles_mark_rect(t, t->tile_stale, r, 1);
    tsync_fill(t->surface->bits, t->surface->pitch, t->reference, t->legacy_w, r, value);
}

static double sync_ms_total(void) {
    double ms = 0.0;
    for (int i = 0; i < VSR_COUNT; i++) ms += V.sync_all[i].ms;
    return ms;
}

static void water_frame_end(void) {
    vk_water_stat_t* w = &V.water_stat[V.frame_water ? 1 : 0];
    if (V.stats_on) {
        w->frames++;
        w->locks += V.epoch_locks - V.flip_locks_mark;
        w->readbacks += V.sync_readbacks - V.flip_reads_mark;
        w->sync_ms += sync_ms_total() - V.flip_sync_mark;
        V.flip_locks_mark = V.epoch_locks;
        V.flip_reads_mark = V.sync_readbacks;
        V.flip_sync_mark = sync_ms_total();
    }
    V.frame_water = 0;
}

static unsigned tiles_stale_in(vk_win_target_t* t, const d3dt_rect_t* region) {
    d3dt_rect_t want = *region;
    unsigned n = 0;
    if (!t->tile_set[0]) return 0;
    tiles_select(t);
    if (want.left < 0) want.left = 0;
    if (want.top < 0) want.top = 0;
    if (want.right > (long)t->legacy_w) want.right = (long)t->legacy_w;
    if (want.bottom > (long)t->legacy_h) want.bottom = (long)t->legacy_h;
    if (want.right <= want.left || want.bottom <= want.top) return 0;
    for (long ty = want.top >> VK_TILE_SHIFT; ty <= (want.bottom - 1) >> VK_TILE_SHIFT; ty++)
        for (long tx = want.left >> VK_TILE_SHIFT; tx <= (want.right - 1) >> VK_TILE_SHIFT; tx++)
            n += t->tile_stale[(size_t)ty * t->tiles_x + (size_t)tx];
    return n;
}

static void gpu_box_add(vk_win_target_t* t, const d3dt_rect_t* r) {
    d3dt_rect_t padded = *r;
    if (t->gpu_box_known) d3dt_rect_union(&t->gpu_box, r);
    padded.left -= 2;
    padded.top -= 2;
    padded.right += 2;
    padded.bottom += 2;
    tiles_mark_rect(t, t->tile_drawn, &padded, 1);
    tiles_mark_rect(t, t->tile_set[0], &padded, 1);
    tiles_mark_rect(t, t->tile_set[1], &padded, 1);
}

static void gpu_box_cleared(vk_win_target_t* t, uint16_t color, int pending_cpu_writes) {
    const int same = t->tile_clear_known && t->tile_clear_color == color && !pending_cpu_writes;
    const size_t tiles = tile_count(t);
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < tiles; i++)
            if (!same || t->tile_drawn[i]) t->tile_set[k][i] = 1;
    if (!same) V.tile_full_invalidations[TILE_INVAL_CLEAR]++;
    if (pending_cpu_writes) {
        for (unsigned i = 0; i < t->dirty.n; i++) {
            tiles_mark_rect(t, t->tile_set[0], &t->dirty.r[i], 1);
            tiles_mark_rect(t, t->tile_set[1], &t->dirty.r[i], 1);
        }
    }
    tiles_set_all(t, t->tile_drawn, 0);
    t->tile_clear_known = 1;
    t->tile_clear_color = color;
    t->gpu_box_known = 1;
    t->gpu_clear = color;
    memset(&t->gpu_box, 0, sizeof t->gpu_box);
}

static d3dt_rect_t dirty_region(const vk_win_target_t* t, const d3dt_rect_t* written) {
    d3dt_rect_t r = *written;
    if (d3dt_rect_empty(&r)) return whole_target(t);
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (long)t->legacy_w) r.right = (long)t->legacy_w;
    if (r.bottom > (long)t->legacy_h) r.bottom = (long)t->legacy_h;
    return r;
}

static uint16_t* readback_buffer(size_t pixels) {
    if (pixels > V.readback_scratch_cap) {
        uint16_t* grown = (uint16_t*)realloc(V.readback_scratch, pixels * sizeof(uint16_t));
        if (!grown) return NULL;
        V.readback_scratch = grown;
        V.readback_scratch_cap = pixels;
    }
    return V.readback_scratch;
}

/* The GPU picture into the CPU copy and its reference: only the tiles of the region the GPU drew in since they were last read. */
static void sync_readback(vk_win_target_t* t, const d3dt_rect_t* region) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t all = whole_target(t), want = all;
    jpt_render_rect_t rects[VK_READ_RECTS];
    uint16_t* pixels;
    unsigned stale = 0, kept = 0, work = 0, filled = 0, count;
    size_t total = 0;
    long tx0, ty0, tx1, ty1;
    if (!target_usable(t) || !t->tile_set[0]) return;
    tiles_select(t);
    if (region && !d3dt_rect_empty(region)) {
        want = *region;
        if (want.left < 0) want.left = 0;
        if (want.top < 0) want.top = 0;
        if (want.right > all.right) want.right = all.right;
        if (want.bottom > all.bottom) want.bottom = all.bottom;
        if (d3dt_rect_empty(&want)) want = all;
    }
    tx0 = want.left >> VK_TILE_SHIFT;
    ty0 = want.top >> VK_TILE_SHIFT;
    tx1 = (want.right - 1) >> VK_TILE_SHIFT;
    ty1 = (want.bottom - 1) >> VK_TILE_SHIFT;
    memset(t->tile_work, 0, tile_count(t));
    for (long ty = ty0; ty <= ty1; ty++)
        for (long tx = tx0; tx <= tx1; tx++) {
            const size_t i = (size_t)ty * t->tiles_x + (size_t)tx;
            if (!t->tile_stale[i]) {
                kept++;
                continue;
            }
            stale++;
            if (t->gpu_box_known && !t->tile_drawn[i]) {
                d3dt_rect_t cell = { tx << VK_TILE_SHIFT, ty << VK_TILE_SHIFT, (tx + 1) << VK_TILE_SHIFT, (ty + 1) << VK_TILE_SHIFT };
                if (cell.right > all.right) cell.right = all.right;
                if (cell.bottom > all.bottom) cell.bottom = all.bottom;
                tsync_fill(s->bits, s->pitch, t->reference, t->legacy_w, &cell, t->gpu_clear);
                t->tile_stale[i] = 0;
                filled++;
            } else {
                t->tile_work[i] = 1;
                work++;
            }
        }
    V.tiles_kept += kept;
    V.tiles_filled += filled;
    V.tiles_stale_total += stale;
    if (!work) {
        if (!stale) V.tile_reads_avoided++;
        return;
    }
    count = overlay_tile_rects(t->tile_work, (int)t->tiles_x, (int)t->tiles_y, t->legacy_w, t->legacy_h, rects, VK_READ_RECTS);
    if (count > 1 && count <= VK_READ_RECTS) {
        long long sum = 0;
        int32_t bx0 = rects[0].x, by0 = rects[0].y, bx1 = rects[0].x + (int32_t)rects[0].w, by1 = rects[0].y + (int32_t)rects[0].h;
        for (unsigned i = 0; i < count; i++) {
            sum += (long long)rects[i].w * rects[i].h;
            if (rects[i].x < bx0) bx0 = rects[i].x;
            if (rects[i].y < by0) by0 = rects[i].y;
            if (rects[i].x + (int32_t)rects[i].w > bx1) bx1 = rects[i].x + (int32_t)rects[i].w;
            if (rects[i].y + (int32_t)rects[i].h > by1) by1 = rects[i].y + (int32_t)rects[i].h;
        }
        if ((long long)(bx1 - bx0) * (by1 - by0) * 4 <= sum * 5) count = VK_READ_RECTS + 1u;
    }
    if (count > VK_READ_RECTS) {
        long x0 = (long)t->legacy_w, y0 = (long)t->legacy_h, x1 = 0, y1 = 0;
        for (size_t i = 0; i < tile_count(t); i++) {
            if (!t->tile_work[i]) continue;
            const long cx = (long)(i % t->tiles_x) << VK_TILE_SHIFT, cy = (long)(i / t->tiles_x) << VK_TILE_SHIFT;
            if (cx < x0) x0 = cx;
            if (cy < y0) y0 = cy;
            if (cx + (1 << VK_TILE_SHIFT) > x1) x1 = cx + (1 << VK_TILE_SHIFT);
            if (cy + (1 << VK_TILE_SHIFT) > y1) y1 = cy + (1 << VK_TILE_SHIFT);
        }
        if (x1 > (long)t->legacy_w) x1 = (long)t->legacy_w;
        if (y1 > (long)t->legacy_h) y1 = (long)t->legacy_h;
        rects[0].x = (int32_t)x0;
        rects[0].y = (int32_t)y0;
        rects[0].w = (uint32_t)(x1 - x0);
        rects[0].h = (uint32_t)(y1 - y0);
        for (size_t i = 0; i < tile_count(t); i++)
            if (t->tile_work[i]) t->tile_work[i] = 0;
        tiles_mark_rect(t, t->tile_work, &(d3dt_rect_t){ x0, y0, x1, y1 }, 1);
        count = 1;
    }
    for (unsigned i = 0; i < count; i++) total += (size_t)rects[i].w * rects[i].h;
    pixels = V.vk ? readback_buffer(total) : NULL;
    if (!pixels || !jpt_vk_renderer_read_scene_rects_rgb565(V.vk, rects, count, t->legacy_w, t->legacy_h, pixels)) {
        d3dt_rect_t whole = all;
        tsync_fill(s->bits, s->pitch, t->reference, t->legacy_w, &whole, t->gpu_clear);
        V.sync_readback_failures++;
        tiles_invalidate_all(t, TILE_INVAL_FAILURE);
        return;
    }
    {
        const uint16_t* in = pixels;
        for (unsigned i = 0; i < count; i++) {
            for (uint32_t y = 0; y < rects[i].h; y++) {
                const long row = (long)rects[i].y + (long)y;
                memcpy(s->bits + (size_t)row * (size_t)s->pitch + (size_t)rects[i].x * 2u, in, (size_t)rects[i].w * 2u);
                memcpy(t->reference + (size_t)row * t->legacy_w + (size_t)rects[i].x, in, (size_t)rects[i].w * 2u);
                in += rects[i].w;
            }
        }
    }
    for (size_t i = 0; i < tile_count(t); i++)
        if (t->tile_work[i]) t->tile_stale[i] = 0;
    V.tiles_read += work;
    V.tile_rects += count;
    V.tile_read_pixels += total;
    V.tile_ctx_reads[V.tile_ctx]++;
    V.sync_readbacks++;
    V.sync_readback_pixels += total;
    V.readbacks++;
}

/* CPU -> GPU: only pixels that differ from the reference, uploaded as the box that holds them. */
static void sync_composite_region(vk_win_target_t* t, const d3dt_rect_t* written) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t region = dirty_region(t, written), box;
    tsync_counts_t counts = { 0, 0 };
    if (!tsync_changed_box(s->bits, s->pitch, t->reference, t->legacy_w, &region, &box)) return;
    {
        d3dt_rect_t apron = d3dt_overlay_upload_rect(&box, t->legacy_w, t->legacy_h);
        jpt_render_rect_t rect;
        rect.x = (int32_t)apron.left;
        rect.y = (int32_t)apron.top;
        rect.w = (uint32_t)(apron.right - apron.left);
        rect.h = (uint32_t)(apron.bottom - apron.top);
        tsync_overlay_box(s->bits, s->pitch, t->reference, t->legacy_w, &apron, t->scratch, &counts);
        if (jpt_vk_renderer_queue_overlay(V.vk, t->scratch, rect.w, &rect, t->legacy_w, t->legacy_h, 0)) {
            gpu_box_add(t, &apron);
            V.sync_composites++;
        } else {
            V.sync_overlay_failures++;
        }
    }
}

static void sync_composite(vk_win_target_t* t) {
    const d3dt_dirty_t written = t->dirty;
    const d3dt_rect_t none = { 0, 0, 0, 0 };
    memset(&t->dirty, 0, sizeof t->dirty);
    if (!target_usable(t) || !ensure_frame_begun()) return;
    if (!written.n) sync_composite_region(t, &none);
    for (unsigned i = 0; i < written.n; i++) sync_composite_region(t, &written.r[i]);
    if (!jpt_vk_renderer_flush_overlays(V.vk, t->legacy_w, t->legacy_h)) V.sync_overlay_failures++;
}

/* The CPU copy is the whole picture: drawn opaque over the target. */
static void sync_upload_all(vk_win_target_t* t) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t all = whole_target(t);
    jpt_render_rect_t rect = { 0, 0, t->legacy_w, t->legacy_h };
    if (!target_usable(t) || !ensure_frame_begun()) return;
    for (unsigned y = 0; y < t->legacy_h; y++) {
        const uint16_t* row = (const uint16_t*)(s->bits + (size_t)y * (size_t)s->pitch);
        uint32_t* out = t->scratch + (size_t)y * t->legacy_w;
        for (unsigned x = 0; x < t->legacy_w; x++) out[x] = d3dt_rgb565_to_bgra(row[x]);
        memcpy(t->reference + (size_t)y * t->legacy_w, row, (size_t)t->legacy_w * 2u);
    }
    if (jpt_vk_renderer_draw_overlay(V.vk, t->scratch, t->legacy_w, &rect, t->legacy_w, t->legacy_h, 1)) {
        gpu_box_add(t, &all);
        tiles_select(t);
        tiles_set_all(t, t->tile_stale, 0);
        V.sync_uploads++;
    } else {
        V.sync_overlay_failures++;
    }
}

static void sync_copy_front(vk_win_target_t* t) {
    dd_surface_t* s = t->surface;
    const dd_surface_t* front = s->root;
    if (!front || !front->bits || front->width != s->width || front->height != s->height) return;
    for (unsigned y = 0; y < t->legacy_h; y++) {
        const BYTE* in = front->bits + (size_t)y * (size_t)front->pitch;
        memcpy(s->bits + (size_t)y * (size_t)s->pitch, in, (size_t)t->legacy_w * 2u);
        memcpy(t->reference + (size_t)y * t->legacy_w, in, (size_t)t->legacy_w * 2u);
    }
    tiles_select(t);
    tiles_set_all(t, t->tile_stale, 0);
    V.sync_front_copies++;
}

static void apply_sync_untimed(vk_win_target_t* t, unsigned acts, const d3dt_rect_t* region);
static void apply_sync(vk_win_target_t* t, unsigned acts, const d3dt_rect_t* region, vk_sync_reason_t reason) {
    const unsigned long long reads = V.sync_readbacks, read_px = V.sync_readback_pixels, uploads = V.sync_uploads + V.sync_composites;
    double start;
    if (!acts) return;
    if (!V.stats_on) {
        apply_sync_untimed(t, acts, region);
        return;
    }
    start = acc_now();
    acc_enter(VKA_SYNC);
    apply_sync_untimed(t, acts, region);
    acc_leave();
    for (int k = 0; k < 2; k++) {
        vk_sync_stat_t* st = k ? &V.sync_all[reason] : &V.sync_win[reason];
        st->calls++;
        if (region && region->right > region->left && region->bottom > region->top)
            st->requested_px += (unsigned long long)(region->right - region->left) * (unsigned long long)(region->bottom - region->top);
        st->readbacks += V.sync_readbacks - reads;
        st->read_px += V.sync_readback_pixels - read_px;
        st->uploads += V.sync_uploads + V.sync_composites - uploads;
        st->ms += acc_now() - start;
    }
    if (reason == VSR_LOCK || reason == VSR_OTHER_ACCESS) {
        vk_caller_stat_t* c = NULL;
        for (unsigned i = 0; i < V.callers_n && !c; i++)
            if (V.callers[i].caller == V.tile_caller) c = &V.callers[i];
        if (!c && V.callers_n < 48u) {
            c = &V.callers[V.callers_n++];
            c->caller = V.tile_caller;
        }
        if (c) {
            c->accesses++;
            c->readbacks += V.sync_readbacks - reads;
            c->read_px += V.sync_readback_pixels - read_px;
            c->ms += acc_now() - start;
        }
    }
}

static void apply_sync_untimed(vk_win_target_t* t, unsigned acts, const d3dt_rect_t* region) {
    d3dt_rect_t all = whole_target(t), r = all;
    if (!acts || !t->surface) return;
    if (acts & TSYNC_COMPOSITE) sync_composite(t);
    if (acts & TSYNC_UPLOAD) sync_upload_all(t);
    if (acts & TSYNC_READBACK) sync_readback(t, region);
    if (acts & TSYNC_COPY_FRONT) sync_copy_front(t);
    if (acts & TSYNC_SYNTHESIZE) {
        fill_copy(t, &all, t->sync.color);
        tiles_select(t);
        tiles_set_all(t, t->tile_stale, 0);
        V.sync_synthesized++;
    }
    if (acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL)) {
        if (!(acts & TSYNC_CAPTURE_ALL) && region) {
            r = *region;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > all.right) r.right = all.right;
            if (r.bottom > all.bottom) r.bottom = all.bottom;
        }
        fill_copy(t, &r, TSYNC_UNWRITTEN);
        V.sync_captures++;
    }
}

static int front_mirrors_target(const vk_win_target_t* t) {
    const dd_surface_t* s = t->surface;
    const dd_surface_t* front = s ? s->root : NULL;
    return front && front->bits && front->width == s->width && front->height == s->height &&
           front->format.dwRGBBitCount == 16;
}

static void check_front(vk_win_target_t* t) {
    if (t->sync.front_mirror && !front_mirrors_target(t)) tsync_front_changed(&t->sync);
}

/* Before the GPU target is drawn into (not a full clear). */
static void gpu_write(vk_win_target_t* t) {
    settle_deferred();
    V.epoch_open = 0;
    unsigned acts = tsync_gpu_draw(&t->sync);
    if (acts) apply_sync(t, acts, NULL, VSR_GPU_DRAW);
}

static void end_polygon_run(const vk_win_target_t* t) {
    if (!t || V.polys.target == t) V.polys.target = NULL;
    if (!t || V.polys.text_target == t) V.polys.text_target = NULL;
}

static d3dt_rect_t clip_to_lock(const dd_surface_t* s, d3dt_rect_t box) {
    if (box.left < s->lock_rect.left) box.left = s->lock_rect.left;
    if (box.top < s->lock_rect.top) box.top = s->lock_rect.top;
    if (box.right > s->lock_rect.right) box.right = s->lock_rect.right;
    if (box.bottom > s->lock_rect.bottom) box.bottom = s->lock_rect.bottom;
    if (box.right < box.left) box.right = box.left;
    if (box.bottom < box.top) box.bottom = box.top;
    return box;
}

static void capture_polygon_tiles(void* context, const d3dt_rect_t* r) { fill_copy((vk_win_target_t*)context, r, TSYNC_UNWRITTEN); }

/* A polygon that reads the target (blending) takes its box from the GPU first. */
static void sync_readback_region(vk_win_target_t* t, const d3dt_rect_t* r) {
    dd_surface_t* s = t->surface;
    jpt_render_rect_t rect;
    uint16_t* pixels;
    size_t bw;
    if (!target_usable(t) || d3dt_rect_empty(r)) return;
    bw = (size_t)(r->right - r->left);
    rect.x = (int32_t)r->left;
    rect.y = (int32_t)r->top;
    rect.w = (uint32_t)bw;
    rect.h = (uint32_t)(r->bottom - r->top);
    pixels = V.vk ? readback_buffer(bw * rect.h) : NULL;
    if (!pixels || !jpt_vk_renderer_read_scene_rgb565(V.vk, &rect, t->legacy_w, t->legacy_h, pixels, bw)) {
        V.sync_readback_failures++;
        return;
    }
    tsync_fill_unwritten(s->bits, s->pitch, t->reference, t->legacy_w, r, pixels, bw);
    V.sync_readbacks++;
    V.sync_readback_pixels += (unsigned long long)bw * rect.h;
    V.readbacks++;
}

static void add_polygon(vk_win_target_t* t, const d3dt_rect_t* box, int reads) {
    d3dt_dirty_add(&V.polys.dirty, box);
    V.cpu_polygons++;
    if (!V.polys.captured || d3dt_rect_empty(box)) return;
    tsync_tiles_claim(&V.polys.tiles, box, capture_polygon_tiles, t);
    if (reads) sync_readback_region(t, box);
}

static void begin_polygon_run(vk_win_target_t* t) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t all = { 0, 0, (long)s->width, (long)s->height };
    unsigned acts;
    check_front(t);
    acts = tsync_cpu_access(&t->sync, TSYNC_ACCESS_NO_READ);
    apply_sync(t, acts & ~(unsigned)(TSYNC_CAPTURE | TSYNC_CAPTURE_ALL), &all, VSR_POLYGON_RUN);
    V.polys.target = t;
    V.polys.captured = !(t->sync.valid & TSYNC_CPU);
    memset(&V.polys.dirty, 0, sizeof V.polys.dirty);
    if (V.polys.captured) {
        if (!tsync_tiles_reset(&V.polys.tiles, s->width, s->height)) {
            fill_copy(t, &all, TSYNC_UNWRITTEN);
            tsync_tiles_claim_all(&V.polys.tiles);
        }
    }
}

static void polygon_lock(vk_win_target_t* t) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t box = clip_to_lock(s, V.polys.pending_box);
    int reads = V.polys.pending_reads;
    V.polys.pending = 0;
    begin_polygon_run(t);
    add_polygon(t, &box, reads);
    V.cpu_polygon_locks++;
}

/* The run's polygons become pending CPU writes of the target; the number of boxes. */
static unsigned flush_polygon_run(vk_win_target_t* t) {
    const d3dt_dirty_t polygons = V.polys.dirty;
    end_polygon_run(t);
    if (!polygons.n) return 0;
    tsync_cpu_wrote(&t->sync);
    for (unsigned i = 0; i < polygons.n; i++) d3dt_dirty_add(&t->dirty, &polygons.r[i]);
    return polygons.n;
}

static void deferred_touched(void* context) {
    vk_win_target_t* t = (vk_win_target_t*)context;
    V.deferred = NULL;
    V.deferred_touched++;
    V.tile_caller = t->deferred_caller;
    apply_sync(t, tsync_lock_touched(&t->deferred), &t->deferred_region, (vk_sync_reason_t)t->deferred_reason);
}

static void settle_deferred(void) {
    if (V.deferred) page_guard_touch();
}

static page_guard_result_t arm_lock_guard(vk_win_target_t* t) {
    dd_surface_t* s = t->surface;
    size_t bytes = (size_t)s->pitch * s->height;
    if (!s->guard_alias && s->section) s->guard_alias = page_alias_create(s->section, bytes);
    if (s->guard_alias && page_guard_arm_alias(s->guard_alias, deferred_touched, t) == PAGE_GUARD_ARMED) {
        s->lock_alias = 1;
        return PAGE_GUARD_ARMED;
    }
    return page_guard_arm(s->bits, bytes, deferred_touched, t);
}

static int defer_lock(vk_win_target_t* t, tsync_access_t kind, const d3dt_rect_t* region, vk_sync_reason_t reason, uint32_t caller) {
    page_guard_result_t armed;
    apply_sync(t, tsync_lock_deferred(&t->sync, kind, &t->deferred), region, reason);
    if (!t->deferred.acts && kind == TSYNC_ACCESS_READ && (t->sync.valid & TSYNC_CPU) && tiles_stale_in(t, region))
        t->deferred.acts = TSYNC_READBACK;
    if (!t->deferred.acts) return 1;
    t->deferred_region = *region;
    t->deferred_reason = (int)reason;
    t->deferred_caller = caller;
    armed = arm_lock_guard(t);
    if (armed != PAGE_GUARD_ARMED) {
        apply_sync(t, tsync_lock_touched(&t->deferred), region, reason);
        return 1;
    }
    V.deferred = t;
    V.deferred_locks++;
    return 1;
}

static void drop_deferred(const struct vk_win_target_s* t) {
    if (!V.deferred || (t && V.deferred != t)) return;
    page_guard_disarm();
    tsync_lock_untouched(&V.deferred->sync, &V.deferred->deferred);
    V.deferred = NULL;
}

static void target_cpu_access(vk_win_target_t* t, int will_write, cpu_access_t access) {
    dd_surface_t* s = t->surface;
    vkp_access_t a;
    tsync_access_t kind;
    d3dt_rect_t region = { 0, 0, (long)s->width, (long)s->height };
    (void)will_write;
    vkp_classify_access(access, s->lock_flags, &a);
    kind = a.reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ;
    if (a.inert) {
        t->inert_lock = 1;
        V.inert_locks++;
        return;
    }
    if (!target_usable(t)) return;
    settle_deferred();
    if (access == CPU_ACCESS_LOCK && V.polys.pending) {
        polygon_lock(t);
        return;
    }
    if (access == CPU_ACCESS_LOCK) {
        region.left = s->lock_rect.left;
        region.top = s->lock_rect.top;
        region.right = s->lock_rect.right;
        region.bottom = s->lock_rect.bottom;
    }
    check_front(t);
    if (a.text_overlay && vkp_text_boxes_known()) V.polys.text_target = t;
    V.tile_ctx = V.polys.target == t || V.polys.pending ? 2 : a.text_overlay ? 1 : 0;
    V.tile_caller = vkp_lock_caller();
    if (access == CPU_ACCESS_LOCK && !V.test_headless &&
        defer_lock(t, kind, &region, VSR_LOCK, V.tile_caller))
        return;
    {
        const vk_sync_reason_t reason = access == CPU_ACCESS_LOCK ? VSR_LOCK : VSR_OTHER_ACCESS;
        const unsigned acts = tsync_cpu_access(&t->sync, kind);
        const unsigned long long reads_before = V.sync_readbacks;
        V.tile_caller = vkp_lock_caller();
        if (!V.epoch_open) {
            V.epoch_open = 1;
            V.cpu_epochs++;
        }
        V.epoch_locks++;
        apply_sync(t, acts, &region, reason);
        if (kind == TSYNC_ACCESS_READ && (t->sync.valid & TSYNC_CPU) && !(acts & TSYNC_READBACK))
            apply_sync(t, TSYNC_READBACK, &region, reason);
        if (V.sync_readbacks != reads_before) V.epoch_locks_gpu++;
    }
}

static void front_written(dd_surface_t* s) {
    if (s->root || !s->next) return;
    for (dd_surface_t* m = s->next; m && m != s; m = m->next)
        if (m->target) tsync_front_changed(&((vk_win_target_t*)m->target)->sync);
}

static HRESULT draw_skipped(vk_win_skip_t why, HRESULT hr) {
    V.bridge_skips[why]++;
    return hr;
}

static void note_fvf(DWORD fvf) {
    for (unsigned i = 0; i < V.fvf_count; i++) {
        if (V.fvf_seen[i].fvf == fvf) {
            V.fvf_seen[i].draws++;
            return;
        }
    }
    if (V.fvf_count < VK_WIN_FVF_SLOTS) {
        V.fvf_seen[V.fvf_count].fvf = fvf;
        V.fvf_seen[V.fvf_count].draws = 1;
        V.fvf_count++;
    }
}

static HRESULT vk_win_draw_untimed(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                           DWORD index_count);
static HRESULT vk_win_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                           DWORD index_count) {
    HRESULT result_;
    acc_enter(VKA_DRAW);
    result_ = vk_win_draw_untimed(dev, type, fvf, vertices, count, indices, index_count);
    acc_leave();
    return result_;
}

static HRESULT vk_win_draw_untimed(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                           DWORD index_count) {
    vk_win_target_t* t = dev ? (vk_win_target_t*)dev->backend : NULL;
    unsigned stride = 0, off_diff = 0, off_spec = 0, off_uv = 0;
    jpt_render_state_t st;
    float sx_scale, sy_scale;
    const DWORD* r = dev ? dev->render : NULL;
    const DWORD* s0 = dev ? dev->stage[0] : NULL;
    jpt_vk_draw_result_t result;
    const char* why = NULL;

    V.guest_draws++;
    if (!t || !dev->dd || (!dev->dd->window && !V.test_headless) || !vertices || !count)
        return draw_skipped(BR_SKIP_NO_TARGET, DDERR_INVALIDPARAMS);
    if (!d3dt_fvf_layout(fvf, &stride, &off_diff, &off_spec, &off_uv))
        return draw_skipped(BR_SKIP_VERTEX_FORMAT, DDERR_INVALIDPARAMS);
    note_fvf(fvf);
    if (!dev->in_scene) return draw_skipped(BR_SKIP_OUTSIDE_SCENE, D3DERR_SCENE_NOT_IN_SCENE);
    if (type < D3DT_PT_POINTLIST || type > D3DT_PT_TRIANGLEFAN) return draw_skipped(BR_SKIP_NO_TARGET, DDERR_INVALIDPARAMS);
    if (!d3dt_primitive_count(type, indices ? index_count : count)) return draw_skipped(BR_SKIP_EMPTY, DD_OK);
    if (indices)
        for (DWORD i = 0; i < index_count; i++)
            if (indices[i] >= count) return draw_skipped(BR_SKIP_INDEX_RANGE, DDERR_INVALIDPARAMS);
    if (!renderer_ready(dev->dd->window, t->legacy_w, t->legacy_h)) return draw_skipped(BR_SKIP_NO_RENDERER, DDERR_GENERIC);
    if (!ensure_frame_begun()) return draw_skipped(BR_SKIP_NO_FRAME, DDERR_GENERIC);

    t->internal_w = V.rt_w;
    t->internal_h = V.rt_h;
    sx_scale = (float)V.rt_w / (float)(t->legacy_w ? t->legacy_w : 1u);
    sy_scale = (float)V.rt_h / (float)(t->legacy_h ? t->legacy_h : 1u);

    int32_t vpx = 0, vpy = 0;
    uint32_t vpw = V.rt_w, vph = V.rt_h;
    if (dev->current && dev->current->data_set) {
        const D3DVIEWPORT2* vp = &dev->current->data;
        d3dt_rect_t scaled = d3dt_scale_rect((long)vp->dwX, (long)vp->dwY, (long)vp->dwWidth, (long)vp->dwHeight,
                                             t->legacy_w, t->legacy_h, V.rt_w, V.rt_h);
        if (!d3dt_rect_empty(&scaled)) {
            vpx = (int32_t)scaled.left;
            vpy = (int32_t)scaled.top;
            vpw = (uint32_t)(scaled.right - scaled.left);
            vph = (uint32_t)(scaled.bottom - scaled.top);
            if (r[D3DRENDERSTATE_ZENABLE]) {
                uint64_t area = (uint64_t)vpw * (uint64_t)vph;
                if (area >= V.scene_frame_area) {
                    V.scene_frame_area = area;
                    V.scene_frame[0] = (float)scaled.left / (float)V.rt_w;
                    V.scene_frame[1] = (float)scaled.top / (float)V.rt_h;
                    V.scene_frame[2] = (float)vpw / (float)V.rt_w;
                    V.scene_frame[3] = (float)vph / (float)V.rt_h;
                }
            }
        }
    }
    jpt_vk_renderer_set_viewport(V.vk, 0, 0, V.rt_w, V.rt_h);
    jpt_vk_renderer_set_scissor(V.vk, 1, vpx, vpy, vpw, vph);

    memset(&st, 0, sizeof st);
    st.z_enable = r[D3DRENDERSTATE_ZENABLE];
    st.z_write = r[D3DRENDERSTATE_ZWRITEENABLE];
    st.z_func = r[D3DRENDERSTATE_ZFUNC];
    st.z_bias = r[D3DRENDERSTATE_ZBIAS];
    st.alpha_blend = r[D3DRENDERSTATE_ALPHABLENDENABLE];
    st.src_blend = r[D3DRENDERSTATE_SRCBLEND];
    st.dst_blend = r[D3DRENDERSTATE_DESTBLEND];
    st.alpha_test = r[D3DRENDERSTATE_ALPHATESTENABLE];
    st.alpha_func = r[D3DRENDERSTATE_ALPHAFUNC];
    st.alpha_ref = d3dt_alpha_ref(r[D3DRENDERSTATE_ALPHAREF]);
    st.fog_enable = r[D3DRENDERSTATE_FOGENABLE];
    st.fog_color = r[D3DRENDERSTATE_FOGCOLOR];
    st.specular_enable = r[D3DRENDERSTATE_SPECULARENABLE];
    st.shade_mode = r[D3DRENDERSTATE_SHADEMODE];
    st.cull_mode = r[D3DRENDERSTATE_CULLMODE];
    st.fill_mode = r[D3DRENDERSTATE_FILLMODE];
    st.colorkey_enable = r[D3DRENDERSTATE_COLORKEYENABLE];
    st.tex_map_blend = r[D3DRENDERSTATE_TEXTUREMAPBLEND];
    st.tex_addr_u = s0[D3DTSS_ADDRESSU] ? s0[D3DTSS_ADDRESSU] : r[D3DRENDERSTATE_TEXTUREADDRESSU];
    st.tex_addr_v = s0[D3DTSS_ADDRESSV] ? s0[D3DTSS_ADDRESSV] : r[D3DRENDERSTATE_TEXTUREADDRESSV];
    st.tex_mag_filter = s0[D3DTSS_MAGFILTER];
    st.tex_min_filter = s0[D3DTSS_MINFILTER];
    st.tex_mip_filter = s0[D3DTSS_MIPFILTER];

    if (dev->texture) {
        uint64_t tid = ensure_surface_vk_texture(dev->texture, &why);
        if (!tid) return draw_skipped(BR_SKIP_TEXTURE, DD_OK);
        st.has_texture = 1;
        st.texture_id = tid;
    }

    if (V.sky_drawing && V.fx_sky.running && V.sky_usable) {
        jpt_vk_renderer_set_sky_params(V.vk, V.sky_constants[0], V.sky_constants[1], V.sky_constants[2],
                                       V.sky_constants[3], V.sky_constants[4]);
        st.sky_mode = 1;
        V.sky_requested++;
        V.sky_batches++;
    }

    if (!st.sky_mode && V.fx_water.running && dev->texture && st.alpha_blend &&
        st.src_blend == D3DBLEND_SRCALPHA && st.dst_blend == D3DBLEND_INVSRCALPHA) {
        const fx_water_link_t* link = fx_water_link(dev->texture);
        if (link && link->width > 0 && link->height > 0) {
            unsigned serial = fx_water_serial();
            size_t floats = (size_t)link->width * (size_t)link->height * 2u;
            if (floats > V.wave_cap) {
                float* grown = (float*)realloc(V.wave_buf, floats * sizeof(float));
                if (grown) {
                    V.wave_buf = grown;
                    V.wave_cap = floats;
                }
            }
            const float* upload_waves = NULL;
            if (V.wave_buf && V.wave_cap >= floats && V.wave_serial != serial) {
                fx_water_fill(link->bits, link->pitch, link->width, link->height, V.wave_buf, NULL);
                V.wave_serial = serial;
                upload_waves = V.wave_buf;
                V.water_wave_uploads++;
            }
            float bounds[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            fx_water_scene_bounds(vpx, vpy, vpx + (long)vpw, vpy + (long)vph, (int)V.rt_w, (int)V.rt_h, bounds);
            jpt_vk_renderer_set_water_params(V.vk, FX_WATER_STRENGTH, FX_WATER_RHW_METRE, bounds[0], bounds[1],
                                             bounds[2], bounds[3], upload_waves, (uint32_t)link->width,
                                             (uint32_t)link->height);
            st.water_mode = 1;
            V.frame_water = 1;
            V.water_w = link->width;
            V.water_h = link->height;
            V.water_requested++;
            V.water_batches++;
        } else {
            V.water_no_link++;
        }
    }

    if ((size_t)count > V.vtx_cap) {
        jpt_render_vertex_t* grown = (jpt_render_vertex_t*)realloc(V.vtx_buf, (size_t)count * sizeof *grown);
        if (!grown) return draw_skipped(BR_SKIP_OUT_OF_MEMORY, DDERR_OUTOFMEMORY);
        V.vtx_buf = grown;
        V.vtx_cap = count;
    }

    const uint8_t* src = (const uint8_t*)vertices;
    for (DWORD i = 0; i < count; i++) {
        const float* pos = (const float*)(src + (size_t)i * stride);
        V.vtx_buf[i].sx = pos[0] * sx_scale;
        V.vtx_buf[i].sy = pos[1] * sy_scale;
        V.vtx_buf[i].sz = pos[2];
        V.vtx_buf[i].rhw = pos[3];
        V.vtx_buf[i].diffuse = off_diff ? *(const uint32_t*)(src + (size_t)i * stride + off_diff) : 0xFFFFFFFFu;
        V.vtx_buf[i].specular = off_spec ? *(const uint32_t*)(src + (size_t)i * stride + off_spec) : 0xFF000000u;
        if (off_uv) {
            const float* uv = (const float*)(src + (size_t)i * stride + off_uv);
            V.vtx_buf[i].tu = uv[0];
            V.vtx_buf[i].tv = uv[1];
        } else {
            V.vtx_buf[i].tu = 0.0f;
            V.vtx_buf[i].tv = 0.0f;
        }
    }

    gpu_write(t);
    result = jpt_vk_renderer_draw(V.vk, &st, (jpt_render_prim_t)type, V.vtx_buf, (uint32_t)count, indices, NULL,
                                  (uint32_t)index_count);
    if (result != JPT_VK_DRAW_SUBMITTED) return DD_OK;

    if (t->gpu_box_known) {
        d3dt_rect_t drawn = d3dt_vertex_box((const float*)vertices, stride, count, t->legacy_w, t->legacy_h);
        gpu_box_add(t, &drawn);
    }
    V.drew_3d = 1;
    if (st.z_write) t->ao_scene = 1;
    V.draw_calls++;
    V.triangles += d3dt_primitive_count(type, index_count ? index_count : count);
    return DD_OK;
}

static HRESULT vk_win_clear_untimed(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z);
static HRESULT vk_win_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    HRESULT result_;
    acc_enter(VKA_OTHER);
    result_ = vk_win_clear_untimed(dev, count, rects, flags, color, z);
    acc_leave();
    return result_;
}

static HRESULT vk_win_clear_untimed(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    vk_win_target_t* t = dev ? (vk_win_target_t*)dev->backend : NULL;
    settle_deferred();
    if (!t || !dev->dd || (!dev->dd->window && !V.test_headless)) return DDERR_GENERIC;
    if (!renderer_ready(dev->dd->window, t->legacy_w, t->legacy_h)) return DDERR_GENERIC;
    if (!ensure_frame_begun()) return DDERR_GENERIC;

    int clr_c = (flags & D3DCLEAR_TARGET) != 0;
    int clr_z = (flags & D3DCLEAR_ZBUFFER) != 0;
    for (DWORD i = 0; i < count; i++) {
        long x = rects[i].x1, y = rects[i].y1, w = rects[i].x2 - rects[i].x1, h = rects[i].y2 - rects[i].y1;
        d3dt_rect_t r = d3dt_scale_rect(x, y, w, h, t->legacy_w, t->legacy_h, V.rt_w, V.rt_h);
        if (d3dt_rect_empty(&r)) continue;
        if (clr_c && x <= 0 && y <= 0 && x + w >= (long)t->legacy_w && y + h >= (long)t->legacy_h) {
            const int pending = t->sync.pending;
            tsync_gpu_clear(&t->sync, d3dt_bgra_to_rgb565(color));
            gpu_box_cleared(t, d3dt_bgra_to_rgb565(color), pending);
        } else if (clr_c) {
            d3dt_rect_t legacy = { x, y, x + w, y + h };
            gpu_write(t);
            gpu_box_add(t, &legacy);
        }
        jpt_vk_renderer_set_scissor(V.vk, 1, (int32_t)r.left, (int32_t)r.top, (uint32_t)(r.right - r.left),
                                    (uint32_t)(r.bottom - r.top));
        jpt_vk_renderer_clear(V.vk, clr_c, color, clr_z, z, clr_z, 0);
        V.clears++;
    }
    jpt_vk_renderer_set_scissor(V.vk, 0, 0, 0, V.rt_w, V.rt_h);
    return DD_OK;
}

static int vk_win_texture_format_known(const DDPIXELFORMAT* pf) { return pixelformat_to_d3dt(pf) != D3DT_FMT_UNKNOWN; }

/* The front buffer of a hardware frame is the last presented scene, read back only when the game asks for it. */
static void read_last_frame_untimed(dd_surface_t* s);
static void read_last_frame(dd_surface_t* s) {
    acc_enter(VKA_READBACK);
    read_last_frame_untimed(s);
    acc_leave();
}

static void read_last_frame_untimed(dd_surface_t* s) {
    uint16_t* pixels;
    size_t count = (size_t)s->width * (size_t)s->height;
    if (!V.vk || s->target || !V.last_flip_valid || V.last_flip_w != s->width || V.last_flip_h != s->height ||
        s->format.dwRGBBitCount != 16 || !s->bits)
        return;
    pixels = readback_buffer(count);
    if (!pixels || !jpt_vk_renderer_read_last_frame_rgb565(V.vk, pixels, s->width, s->height, s->width)) {
        V.sync_readback_failures++;
        return;
    }
    for (DWORD y = 0; y < s->height; y++)
        memcpy(s->bits + (size_t)y * (size_t)s->pitch, pixels + (size_t)y * s->width, (size_t)s->width * 2u);
    V.last_frame_reads++;
    V.readbacks++;
}

static void vk_win_surface_cpu_access_untimed(dd_surface_t* s, int will_write, cpu_access_t access);
static void vk_win_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access) {
    acc_enter(VKA_CPU_ACCESS);
    vk_win_surface_cpu_access_untimed(s, will_write, access);
    acc_leave();
}

static void vk_win_surface_cpu_access_untimed(dd_surface_t* s, int will_write, cpu_access_t access) {
    if (!s || !V.vk) return;
    if (s->target && ((vk_win_target_t*)s->target)->device) target_cpu_access((vk_win_target_t*)s->target, will_write, access);

    if (s->front_from_gpu) {
        s->front_from_gpu = 0;
        read_last_frame(s);
    }
}

static void vk_win_surface_cpu_written_untimed(dd_surface_t* s, const RECT* r);
static void vk_win_surface_cpu_written(dd_surface_t* s, const RECT* r) {
    acc_enter(VKA_CPU_ACCESS);
    vk_win_surface_cpu_written_untimed(s, r);
    acc_leave();
}

static void vk_win_surface_cpu_written_untimed(dd_surface_t* s, const RECT* r) {
    if (!s) return;
    s->cpu_writes++;
    if (V.polys.text_target && V.polys.text_target->surface == s) {
        tl_scan(s->bits, s->pitch, V.polys.text_target->legacy_w, V.polys.text_target->legacy_h);
        V.polys.text_target = NULL;
    }
    front_written(s);
    if (s->target) {
        vk_win_target_t* t = (vk_win_target_t*)s->target;
        d3dt_rect_t add = { 0, 0, (long)s->width, (long)s->height };
        if (r) {
            add.left = r->left;
            add.top = r->top;
            add.right = r->right;
            add.bottom = r->bottom;
        }
        if (V.polys.target == t) {
            if (!flush_polygon_run(t)) return;
        } else {
            tsync_cpu_wrote(&t->sync);
            d3dt_dirty_add(&t->dirty, &add);
        }
    }
    if (s->caps & DDSCAPS_TEXTURE) {
        d3dt_rect_t add = { 0, 0, (long)s->width, (long)s->height };
        if (r) {
            add.left = r->left;
            add.top = r->top;
            add.right = r->right;
            add.bottom = r->bottom;
        }
        d3dt_rect_union(&s->texture_dirty, &add);
    }
}

static int vk_win_surface_unlocked(dd_surface_t* s) {
    vk_win_target_t* t = s ? (vk_win_target_t*)s->target : NULL;
    if (t && t->inert_lock) {
        t->inert_lock = 0;
        return 1;
    }
    if (!t || V.deferred != t || !page_guard_disarm()) return 0;
    V.deferred = NULL;
    tsync_lock_untouched(&t->sync, &t->deferred);
    V.deferred_untouched++;
    return 1;
}

static int vk_win_surface_fill_untimed(dd_surface_t* s, const RECT* r, DWORD color);
static int vk_win_surface_fill(dd_surface_t* s, const RECT* r, DWORD color) {
    int result_;
    acc_enter(VKA_OTHER);
    result_ = vk_win_surface_fill_untimed(s, r, color);
    acc_leave();
    return result_;
}

static int vk_win_surface_fill_untimed(dd_surface_t* s, const RECT* r, DWORD color) {
    vk_win_target_t* t = s ? (vk_win_target_t*)s->target : NULL;
    D3DRECT rect;
    uint32_t bgra;
    if (!t || !t->device || !r) return 0;
    rect.x1 = r->left;
    rect.y1 = r->top;
    rect.x2 = r->right;
    rect.y2 = r->bottom;
    bgra = d3dt_rgb565_to_bgra((uint16_t)color);
    return SUCCEEDED(vk_win_clear(t->device, 1, &rect, D3DCLEAR_TARGET, bgra, 0.0f));
}

static void vk_win_surface_freed(dd_surface_t* s) {
    if (!s) return;
    if (s->target) drop_deferred((vk_win_target_t*)s->target);
    fx_water_surface_freed(s, (uintptr_t)s->bits, s->bits ? (uintptr_t)s->bits + (uintptr_t)s->pitch * s->height : 0);
    if (s->gpu_texture) {
        vk_win_tex_t* e = (vk_win_tex_t*)s->gpu_texture;
        if (V.vk && e->vk_tex_id) jpt_vk_renderer_destroy_texture(V.vk, e->vk_tex_id);
        for (int i = 0; i < V.tex_count; i++) {
            if (V.tex_entries[i] == e) {
                V.tex_entries[i] = V.tex_entries[--V.tex_count];
                break;
            }
        }
        free(e);
        s->gpu_texture = NULL;
    }
}

static void vk_win_texture_loaded(dd_surface_t* dst, const dd_surface_t* src) {
    if (!dst || !src) return;
    fx_water_loaded(dst, src, (uintptr_t)src->bits, src->pitch, (int)src->width, (int)src->height,
                    dd_format_bytes(&src->format));
    if (dst->gpu_texture) ((vk_win_tex_t*)dst->gpu_texture)->dirty = 1;
}

static void vk_win_set_ui_layer(const uint16_t* bits, long pitch, int guest_w, int guest_h) {
    V.ui_bits = bits;
    V.ui_pitch = pitch;
    V.ui_guest_w = guest_w;
    V.ui_guest_h = guest_h;
}

static void vk_win_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty) {
    if (!pixels || w <= 0 || h <= 0) {
        V.fe_pixels = NULL;
        V.fe_layer_active = 0;
        V.fe_dirty = 0;
        V.fe_dirty_full = 0;
        memset(&V.fe_dirty_rect, 0, sizeof V.fe_dirty_rect);
        return;
    }
    int size_changed = (V.fe_pixels == NULL || V.fe_w != w || V.fe_h != h || V.fe_uploaded_w != w || V.fe_uploaded_h != h);
    V.fe_pixels = pixels;
    V.fe_w = w;
    V.fe_h = h;
    if (size_changed) {
        V.fe_dirty = 1;
        V.fe_dirty_full = 1;
        memset(&V.fe_dirty_rect, 0, sizeof V.fe_dirty_rect);
        return;
    }
    if (!dirty) return;
    if (dirty->left <= 0 && dirty->top <= 0 && dirty->right >= w && dirty->bottom >= h) {
        V.fe_dirty = 1;
        V.fe_dirty_full = 1;
        return;
    }
    int32_t x0 = dirty->left < 0 ? 0 : (int32_t)dirty->left;
    int32_t y0 = dirty->top < 0 ? 0 : (int32_t)dirty->top;
    int32_t x1 = dirty->right > w ? (int32_t)w : (int32_t)dirty->right;
    int32_t y1 = dirty->bottom > h ? (int32_t)h : (int32_t)dirty->bottom;
    if (x1 <= x0 || y1 <= y0) return;
    if (!V.fe_dirty || V.fe_dirty_rect.w == 0 || V.fe_dirty_rect.h == 0) {
        V.fe_dirty_rect.x = x0;
        V.fe_dirty_rect.y = y0;
        V.fe_dirty_rect.w = (uint32_t)(x1 - x0);
        V.fe_dirty_rect.h = (uint32_t)(y1 - y0);
    } else {
        int32_t ux0 = V.fe_dirty_rect.x < x0 ? V.fe_dirty_rect.x : x0;
        int32_t uy0 = V.fe_dirty_rect.y < y0 ? V.fe_dirty_rect.y : y0;
        int32_t ux1 = (V.fe_dirty_rect.x + (int32_t)V.fe_dirty_rect.w) > x1
                          ? (V.fe_dirty_rect.x + (int32_t)V.fe_dirty_rect.w)
                          : x1;
        int32_t uy1 = (V.fe_dirty_rect.y + (int32_t)V.fe_dirty_rect.h) > y1
                          ? (V.fe_dirty_rect.y + (int32_t)V.fe_dirty_rect.h)
                          : y1;
        V.fe_dirty_rect.x = ux0;
        V.fe_dirty_rect.y = uy0;
        V.fe_dirty_rect.w = (uint32_t)(ux1 - ux0);
        V.fe_dirty_rect.h = (uint32_t)(uy1 - uy0);
    }
    V.fe_dirty = 1;
}

static void vk_win_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    V.ptr_sprite = sprite;
    V.ptr_size = size;
    V.ptr_gen = generation;
    V.ptr_x = x;
    V.ptr_y = y;
}

static int vk_win_frontend_failed(void) { return 0; }

static void vk_win_frontend_release(void) {
    V.fe_pixels = NULL;
    V.fe_layer_active = 0;
    V.fe_uploaded_w = V.fe_uploaded_h = 0;
    V.fe_dirty = 0;
    V.fe_dirty_full = 0;
    memset(&V.fe_dirty_rect, 0, sizeof V.fe_dirty_rect);
    V.ptr_sprite = NULL;
    if (V.vk) {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_FRONTEND_HD);
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_CURSOR);
    }
}

/* Every 8th row and 4th pixel: the frozen picture is replaced whole when the game captures a new one. */
static uint32_t still_key(const uint16_t* bits, long pitch, int w, int h) {
    uint32_t key = 2166136261u;
    for (int y = 0; y < h; y += 8)
        for (int x = (y / 8) & 3; x < w; x += 4) key = (key ^ bits[(size_t)y * (size_t)pitch + (size_t)x]) * 16777619u;
    return key;
}

static int vk_win_draw_still_picture(void* surface, const uint16_t* bits, long pitch, int w, int h) {
    dd_surface_t* s = dd_surface_from(surface);
    vk_win_target_t* t = s ? (vk_win_target_t*)s->target : NULL;
    uint32_t key;
    if (!t || !bits || w <= 0 || h <= 0 || t->legacy_w != (unsigned)w || t->legacy_h != (unsigned)h) return 0;
    key = still_key(bits, pitch, w, h);
    V.still_reuse = V.still_resident && V.still_rgba && V.still_bits == bits && V.still_w == w && V.still_h == h &&
                    V.still_key == key;
    if (!V.still_reuse) {
        size_t bytes = (size_t)w * (size_t)h * 4u;
        uint8_t* dst = (uint8_t*)realloc(V.still_rgba, bytes);
        uint32_t* out;
        if (!dst) return 0;
        V.still_rgba = dst;
        V.still_w = w;
        V.still_h = h;
        V.still_bits = bits;
        V.still_key = key;
        V.still_resident = 0;
        init_rgb565_table();
        out = (uint32_t*)dst;
        for (int y = 0; y < h; y++) {
            const uint16_t* in = bits + (size_t)y * (size_t)pitch;
            for (int x = 0; x < w; x++) out[(size_t)y * (size_t)w + (size_t)x] = V.rgb565_rgba[in[x]];
        }
        V.still_uploads++;
    }
    V.still_active = 1;
    V.still_draws++;
    gpu_write(t);
    t->ao_scene = 0;
    return 1;
}

static void vk_win_text_box(HDC dc, const RECT* box) {
    vk_win_target_t* t = V.polys.text_target;
    d3dt_rect_t r;
    if (!t || !t->surface || !t->surface->dc_out || t->surface->dc != dc) return;
    if (box) {
        r.left = box->left;
        r.top = box->top;
        r.right = box->right;
        r.bottom = box->bottom;
    } else {
        r = whole_target(t);
    }
    tl_note(t->legacy_w, t->legacy_h, &(tl_rect_t){ r.left, r.top, r.right, r.bottom });
    V.text_boxes++;
}

static void build_present_plan(int base_is_3d, int has_subtitles, int has_ao, jpt_present_plan_t* out) {
    jpt_present_input_t pin;
    int allowed = V.test_headless ? 1 : window_host_frames_allowed();
    int ui_held = allowed ? vkp_ui_canvas_present_held() : 0;
    memset(&pin, 0, sizeof pin);
    pin.frames_allowed = allowed;
    pin.ui_canvas_held = ui_held;
    pin.loading_hd_held = (allowed && !ui_held) ? vkp_loading_hd_present_held() : 0;
    pin.base_is_3d = base_is_3d ? 1 : 0;
    pin.ao_wanted = has_ao ? 1 : 0;
    pin.has_frontend_hd = (V.fe_pixels != NULL && V.fe_w > 0 && V.fe_h > 0);
    pin.fe_w = V.fe_w > 0 ? V.fe_w : 0;
    pin.fe_h = V.fe_h > 0 ? V.fe_h : 0;
    pin.has_pointer = (V.ptr_sprite != NULL && V.ptr_size > 0);
    pin.has_ui_layer = (V.ui_bits != NULL && V.ui_guest_w > 0 && V.ui_guest_h > 0);
    pin.has_subtitle = has_subtitles ? 1 : 0;
    pin.has_host_overlay = debug_menu_overlay_is_wanted();
    pin.out_w = (int)(V.win_w ? V.win_w : (V.rt_w ? V.rt_w : 640u));
    pin.out_h = (int)(V.win_h ? V.win_h : (V.rt_h ? V.rt_h : 480u));
    jpt_present_plan_build(&pin, out);
}

static int vk_win_flip_untimed(dd_surface_t* front, int vsync);
static int vk_win_flip(dd_surface_t* front, int vsync) {
    int result_;
    acc_enter(VKA_FLIP);
    result_ = vk_win_flip_untimed(front, vsync);
    acc_leave();
    return result_;
}

static int vk_win_flip_untimed(dd_surface_t* front, int vsync) {
    static int s_held_logged;
    vk_win_target_t* t = NULL;
    dd_device_t* d = front ? front->device : NULL;
    jpt_present_plan_t plan;
    int presented;
    unsigned present_acts;

    if (!V.active || !front || !d || (!d->window && !V.test_headless)) return 0;
    for (dd_surface_t* m = front->next; m && m != front; m = m->next)
        if (m->target) t = (vk_win_target_t*)m->target;
    if (!t) t = (vk_win_target_t*)front->target;
    if (!t) return 0;

    present_acts = tsync_present(&t->sync);
    if (!tsync_presents_gpu(&t->sync) && !V.still_active) {
        front->front_from_gpu = 0;
        t->ao_scene = 0;
        return 0;
    }

    if (!ensure_vk_renderer(d->window, t->legacy_w, t->legacy_h)) return 0;

    int ao_allowed = t->ao_scene && !V.fe_pixels && !V.ui_bits && (V.test_headless || vkp_gameplay_now());
    build_present_plan(1, 0, ao_allowed, &plan);
    if (plan.held && !V.frame_open) {
        if (!s_held_logged++)
            host_log("ui", "a blank present between two menu frames was held: the last menu frame stays on screen");
        if (plan.end_text_frame) tl_frame_end();
        t->ao_scene = 0;
        t->frame_started = 0;
        tsync_flipped(&t->sync);
        V.tile_flips++;
        water_frame_end();
        memset(&t->dirty, 0, sizeof t->dirty);
        return 1;
    }

    if (!ensure_frame_begun()) return 0;
    if (tsync_presents_gpu(&t->sync)) apply_sync(t, present_acts, NULL, VSR_PRESENT);

    sync_gamma(&d->gamma);
    d->gamma_changed = 0;

    if (V.still_active && V.still_rgba) {
        jpt_vk_layer_opts_t vopts = { 0, 0, 0, 0, NULL };
        if (!(V.still_reuse && V.still_resident &&
              jpt_vk_renderer_reactivate_layer(V.vk, JPT_LAYER_VIDEO, (uint32_t)V.still_w, (uint32_t)V.still_h, &vopts))) {
            jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_VIDEO, V.still_rgba, (uint32_t)V.still_w,
                                            (uint32_t)V.still_h, &vopts);
            V.still_resident = 1;
        }
        V.still_active = 0;
    } else {
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_VIDEO);
    }

    jpt_vk_renderer_set_ao_params(V.vk, plan.draw_ao ? V.ao_level : JPT_AO_OFF, vkp_view_tan_half_height(),
                                  plan.draw_ao);

    jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_VIDEO_SUBTITLES);
    submit_frontend_and_ui_layers(&plan);
    submit_host_overlays(&plan);

    (void)vsync;
    window_host_frame_commit(front->display_generation);
    presented = jpt_vk_renderer_end_and_present(V.vk);
    V.frame_open = 0;
    V.drew_3d = 0;
    t->ao_scene = 0;
    t->frame_started = 0;
    front->front_from_gpu = !(t->sync.valid & TSYNC_CPU);
    tsync_flipped(&t->sync);
    V.tile_flips++;
    water_frame_end();
    memset(&t->dirty, 0, sizeof t->dirty);
    if (plan.end_text_frame) tl_frame_end();

    if (!presented) return 0;
    V.last_flip_valid = 1;
    V.last_flip_w = t->legacy_w;
    V.last_flip_h = t->legacy_h;

    V.frames_hw++;
    window_host_frame_presented();
    video_hd_main_presented();
    note_effect_evidence();
    log_stats_window();
    if (!V.first_frame_logged) {
        V.first_frame_logged = 1;
        host_log("vulkan", "first frame presented by Vulkan hardware path: %ux%u (%llu draw calls, %llu triangles)",
                 V.rt_w, V.rt_h, V.draw_calls, V.triangles);
    }
    return 1;
}

static void vk_win_present_frame_untimed(HWND window, const BYTE* bits, LONG pitch, const RECT* source,
                                 const DDPIXELFORMAT* format, const DDGAMMARAMP* gamma, int gamma_changed, int vsync,
                                 unsigned generation);
static void vk_win_present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source,
                                 const DDPIXELFORMAT* format, const DDGAMMARAMP* gamma, int gamma_changed, int vsync,
                                 unsigned generation) {
    acc_enter(VKA_VIDEO);
    vk_win_present_frame_untimed(window, bits, pitch, source, format, gamma, gamma_changed, vsync, generation);
    acc_leave();
}

static void vk_win_present_frame_untimed(HWND window, const BYTE* bits, LONG pitch, const RECT* source,
                                 const DDPIXELFORMAT* format, const DDGAMMARAMP* gamma, int gamma_changed, int vsync,
                                 unsigned generation) {
    jpt_present_plan_t plan;
    uint32_t w, h;
    uint8_t* rgba;
    const video_subtitle_image_t* sub;

    (void)gamma_changed;
    (void)vsync;
    if (!window || !bits || !source || !format || format->dwRGBBitCount != 16 || !window_host_frames_allowed()) return;
    if (IsIconic(window)) {
        Sleep(10);
        return;
    }

    w = (uint32_t)(source->right - source->left);
    h = (uint32_t)(source->bottom - source->top);
    if (!w || !h) return;

    if (video_hd_present(window, bits, pitch, source, format->dwRBitMask == 0xF800, (const uint16_t*)gamma)) return;

    if (!ensure_vk_renderer(window, w, h)) return;

    sub = video_subtitles_for_screen((int)w, (int)h, (int)V.win_w, (int)V.win_h);
    build_present_plan(0, sub && sub->pixels && sub->w > 0 && sub->h > 0, 0, &plan);
    if (plan.held) return;

    if (!ensure_frame_begun()) return;

    sync_gamma(gamma);
    rgba = ensure_buf(&V.rgba_scratch, &V.rgba_cap, (size_t)w * (size_t)h * 4u);
    if (!rgba) return;

    init_rgb565_table();
    int is_565 = (format->dwRBitMask == 0xF800);
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t* in = (const uint16_t*)(bits + (size_t)(source->top + (LONG)y) * (size_t)pitch) + source->left;
        uint32_t* out = (uint32_t*)(rgba + (size_t)y * (size_t)w * 4u);
        if (is_565) {
            for (uint32_t x = 0; x < w; x++) out[x] = V.rgb565_rgba[in[x]];
        } else {
            for (uint32_t x = 0; x < w; x++) {
                uint16_t p = in[x];
                uint32_t r5 = (p >> 10) & 31u, g5 = (p >> 5) & 31u, b5 = p & 31u;
                out[x] = ((r5 << 3) | (r5 >> 2)) | (((g5 << 3) | (g5 >> 2)) << 8) | (((b5 << 3) | (b5 >> 2)) << 16) |
                         0xFF000000u;
            }
        }
    }

    jpt_vk_renderer_set_ao_params(V.vk, JPT_AO_OFF, 0.57735f, 0);
    V.last_flip_valid = 0;
    V.still_resident = 0;
    {
        jpt_vk_layer_opts_t vopts = { 0, 0, 0, 0, NULL };
        jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_VIDEO, rgba, w, h, &vopts);
    }
    submit_subtitle_layer(sub, (int)V.win_w, (int)V.win_h, &plan);
    submit_frontend_and_ui_layers(&plan);
    submit_host_overlays(&plan);

    window_host_frame_commit(generation);
    if (jpt_vk_renderer_end_and_present(V.vk)) {
        V.frames_dib++;
        window_host_frame_presented();
        video_hd_main_presented();
        if (!V.first_frame_logged) {
            V.first_frame_logged = 1;
            host_log("vulkan", "first frame presented by Vulkan: %ux%u", w, h);
        }
    }
    V.frame_open = 0;
    V.drew_3d = 0;
}

static int vk_win_present_video_frame(HWND window, const jpt_vk_video_frame_t* frame,
                                      const video_subtitle_image_t* subtitle) {
    jpt_present_plan_t plan;
    int ok;
    if (!window || !frame) return 0;
    acc_enter(VKA_VIDEO);
    ok = ensure_vk_renderer(window, 0, 0);
    if (ok) {
        build_present_plan(0, subtitle && subtitle->pixels && subtitle->w > 0 && subtitle->h > 0, 0, &plan);
        ok = ensure_frame_begun();
    }
    if (ok) {
        jpt_vk_renderer_set_guest_size(V.vk, V.win_w, V.win_h);
        jpt_vk_renderer_set_ao_params(V.vk, JPT_AO_OFF, 0.57735f, 0);
        V.last_flip_valid = 0;
        V.still_resident = 0;
        jpt_vk_renderer_clear(V.vk, 1, 0xFF000000u, 0, 0.0f, 0, 0);
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_VIDEO);
        ok = jpt_vk_renderer_video_draw(V.vk, frame);
    }
    if (ok) {
        submit_subtitle_layer(subtitle, (int)V.win_w, (int)V.win_h, &plan);
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_FRONTEND_HD);
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_LOADING_HD);
        jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_CURSOR);
        submit_host_overlays(&plan);
        ok = jpt_vk_renderer_end_and_present(V.vk);
        V.frame_open = 0;
        V.drew_3d = 0;
        if (plan.end_text_frame) tl_frame_end();
        if (ok) {
            V.frames_dib++;
            window_host_frame_presented();
        }
    }
    acc_leave();
    return ok;
}

static int vk_win_present_video_hd_untimed(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                                   const video_subtitle_image_t* subtitle);
static int vk_win_present_video_hd(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                                   const video_subtitle_image_t* subtitle) {
    int result_;
    acc_enter(VKA_VIDEO);
    result_ = vk_win_present_video_hd_untimed(window, rgba, w, h, norm_rect, subtitle);
    acc_leave();
    return result_;
}

static int vk_win_present_video_hd_untimed(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                                   const video_subtitle_image_t* subtitle) {
    jpt_present_plan_t plan;
    if (!window || !rgba || w <= 0 || h <= 0) return 0;
    if (!ensure_vk_renderer(window, 0, 0)) return 0;
    build_present_plan(0, subtitle && subtitle->pixels && subtitle->w > 0 && subtitle->h > 0, 0, &plan);
    if (!ensure_frame_begun()) return 0;
    jpt_vk_renderer_set_guest_size(V.vk, V.win_w, V.win_h);

    jpt_vk_renderer_set_ao_params(V.vk, JPT_AO_OFF, 0.57735f, 0);
    V.last_flip_valid = 0;
    V.still_resident = 0;
    jpt_vk_renderer_clear(V.vk, 1, 0xFF000000u, 0, 0.0f, 0, 0);
    {
        jpt_vk_layer_opts_t vopts = { 1, 0, 0, 0, norm_rect };
        jpt_vk_renderer_submit_layer_ex(V.vk, JPT_LAYER_VIDEO, rgba, (uint32_t)w, (uint32_t)h, &vopts);
    }
    submit_subtitle_layer(subtitle, (int)V.win_w, (int)V.win_h, &plan);
    jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_FRONTEND_HD);
    jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_LOADING_HD);
    jpt_vk_renderer_clear_layer(V.vk, JPT_LAYER_CURSOR);
    submit_host_overlays(&plan);

    int ok = jpt_vk_renderer_end_and_present(V.vk);
    V.frame_open = 0;
    V.drew_3d = 0;
    if (plan.end_text_frame) tl_frame_end();
    if (ok) {
        V.frames_dib++;
        window_host_frame_presented();
    }
    return ok;
}

static int vk_win_present_adapter(dd_adapter_t* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!V.adapter_valid) {
        jpt_vk_probe_result_t probe;
        memset(&probe, 0, sizeof probe);
        if (jpt_vk_probe_headless(V.gpu_request, &probe) && probe.available && probe.selected_index >= 0) {
            const jpt_vk_gpu_info_t* g = &probe.gpus[probe.selected_index];
            snprintf(V.adapter.description, sizeof V.adapter.description, "%s", g->name);
            V.adapter.vendor = g->vendor_id;
            V.adapter.device = g->device_id;
            V.adapter.dedicated_video_memory = (SIZE_T)g->vram_bytes;
            V.adapter.driver_version.QuadPart = (LONGLONG)g->driver_version;
            V.adapter_valid = 1;
        }
    }
    if (!V.adapter_valid) return 0;
    *out = V.adapter;
    return 1;
}

static void vk_win_present_release(void) {
    debug_menu_release();
    release_vk_instance();
}

static void vk_win_present_set_vsync(int on) {
    V.vsync = on ? 1 : 0;
    if (V.vk) jpt_vk_renderer_set_present_options(V.vk, V.vsync, V.fullscreen, effective_stretch(V.game_hwnd, V.surface_hwnd));
}

static void vk_win_present_set_graphics_card(const char* name) {
    snprintf(V.gpu_request, sizeof V.gpu_request, "%s", name ? name : "");
}

static void vk_win_present_on_resize(uint32_t w, uint32_t h) {
    if (!w || !h) return;
    V.win_w = w;
    V.win_h = h;
    if (V.vk) jpt_vk_renderer_on_resize(V.vk, w, h);
}

static void vk_win_present_set_fullscreen(int on) {
    V.fullscreen = on ? 1 : 0;
    if (V.vk) jpt_vk_renderer_set_present_options(V.vk, V.vsync, V.fullscreen, effective_stretch(V.game_hwnd, V.surface_hwnd));
}

static void vk_win_frame_presented(double present_ms) { (void)present_ms; }
static void vk_win_cpu_polygon(void* surface_iface, const long raster_box[4], uint32_t origin_bytes, int reads) {
    dd_surface_t* s = V.active && surface_iface ? dd_surface_from(surface_iface) : NULL;
    vk_win_target_t* t;
    long at[4] = { raster_box[0], raster_box[1], raster_box[2], raster_box[3] };
    d3dt_rect_t box;
    uint32_t caller;
    if (!s || !s->target) return;
    t = (vk_win_target_t*)s->target;
    if (!t->device) return;
    vkp_polygon_to_surface(at, origin_bytes, s->pitch);
    box.left = at[0];
    box.top = at[1];
    box.right = at[2];
    box.bottom = at[3];
    caller = vkp_lock_caller();
    if (!vkp_polygon_caller_ok(caller)) return;
    if (reads && vkp_polygon_store_only(caller)) reads = 0;
    if (!s->locked) {
        V.polys.pending = 1;
        V.polys.pending_reads = reads;
        V.polys.pending_box = box;
        return;
    }
    if (V.polys.target == t) {
        box = clip_to_lock(s, box);
        add_polygon(t, &box, reads);
    } else if (V.deferred == t && page_guard_disarm()) {
        V.deferred = NULL;
        tsync_lock_untouched(&t->sync, &t->deferred);
        V.polys.pending_reads = reads;
        V.polys.pending_box = box;
        polygon_lock(t);
    }
}
static void vk_win_cpu_polygon_done(void) { V.polys.pending = 0; }
/* Perspective-textured software polygons drawn by the GPU (cpu_polygons.c): the guest's 8-bit texels and CLUTs, or the
 * bump path's 16-bit texels and tables, live in four GPU buffers behind the caches of texture_gpu_policy.c. */
static void poly_caches_reset(void) {
    tgpu_texture_cache_reset(&V.poly.textures, JPT_VK_POLY_TEXEL_BYTES);
    tgpu_clut_cache_reset(&V.poly.cluts);
    tgpu_texture_cache_reset(&V.poly.bump_table_cache, JPT_VK_POLY_TABLE_BYTES);
    tgpu_texture_cache_reset(&V.poly.bump_clut_cache, JPT_VK_POLY_BUMP_CLUT_BYTES);
}

/* A new renderer starts with empty buffers: the caches must not point into the old ones. */
static int poly_resources(void) {
    if (!V.vk || !jpt_vk_renderer_poly_ready(V.vk)) return 0;
    if (V.poly.renderer != V.vk) {
        poly_caches_reset();
        V.poly.renderer = V.vk;
    }
    return 1;
}

static void vk_win_texture_polygon_reset(void) { poly_caches_reset(); }

static void vk_win_texture_polygon_warm(void) {
    if (V.active && V.vk) (void)poly_resources();
}

static vk_win_target_t* poly_target(void* surface_iface, dd_surface_t** surface) {
    dd_surface_t* s = V.active && V.vk && surface_iface ? dd_surface_from(surface_iface) : NULL;
    vk_win_target_t* t = s ? (vk_win_target_t*)s->target : NULL;
    *surface = s;
    return t && t->device && s->pitch && target_usable(t) ? t : NULL;
}

/* The polygon's pixels on the target, and where its vertices sit in the surface's pixels. */
static d3dt_rect_t poly_box(const gpu_texture_polygon_t* p, const dd_surface_t* s, float* ox, float* oy) {
    d3dt_rect_t box = { 0, 0, 0, 0 };
    *ox = (float)((p->origin_bytes % (uint32_t)s->pitch) / 2u);
    *oy = (float)(p->origin_bytes / (uint32_t)s->pitch);
    for (unsigned i = 0; i < p->count; i++) {
        float x = p->x[i] + *ox, y = p->y[i] + *oy;
        if (!i || x < box.left) box.left = (long)floorf(x);
        if (!i || y < box.top) box.top = (long)floorf(y);
        if (!i || x > box.right) box.right = (long)ceilf(x) + 1;
        if (!i || y > box.bottom) box.bottom = (long)ceilf(y) + 1;
    }
    return box;
}

static int dirty_overlaps(const d3dt_dirty_t* d, const d3dt_rect_t* box) {
    for (unsigned i = 0; i < d->n; i++)
        if (d->r[i].left < box->right && box->left < d->r[i].right && d->r[i].top < box->bottom &&
            box->top < d->r[i].bottom)
            return 1;
    return 0;
}

static int poly_upload_cached(jpt_vk_poly_pool_t pool, int64_t at, const void* data, uint32_t bytes, int upload) {
    if (!upload) return 1;
    if (jpt_vk_renderer_poly_upload(V.vk, pool, (uint32_t)at, data, bytes)) return 1;
    poly_caches_reset();
    V.poly.failed_uploads++;
    return 0;
}

/* Everything that can fail before the polygon's spans run: the target, the resources, the texture and CLUT uploads and
 * the order against the CPU polygons already written (a surface the game holds locked keeps its run, split where the
 * polygon overlaps it so the pixels written earlier reach the target first). */
static gpu_polygon_status_t vk_win_texture_polygon_prepare(void* surface_iface, const gpu_texture_polygon_t* p) {
    dd_surface_t* s;
    vk_win_target_t* t = poly_target(surface_iface, &s);
    d3dt_rect_t box;
    tgpu_run_t action;
    int64_t texels;
    float ox, oy;
    int upload;
    V.poly.prepared = NULL;
    if (!t) return GPU_POLYGON_NO_TARGET;
    if (p->count < 3 || p->count > JPT_VK_POLY_MAX_VERTICES || !ensure_frame_begun() || !poly_resources())
        return GPU_POLYGON_NO_RESOURCES;
    box = poly_box(p, s, &ox, &oy);
    action = tgpu_run_action(s->locked, V.polys.target == t, 0, V.polys.captured, dirty_overlaps(&V.polys.dirty, &box));
    if (action == TGPU_RUN_BUSY) return GPU_POLYGON_BUSY;
    if (action == TGPU_RUN_ADOPT) return GPU_POLYGON_BUSY;
    if ((texels = tgpu_texture_cache_find(&V.poly.textures, p->texels, p->texel_bytes, V.frames_hw, &upload)) < 0)
        return GPU_POLYGON_NO_RESOURCES;
    if (action != TGPU_RUN_KEEP) {
        if (action == TGPU_RUN_SPLIT) flush_polygon_run(t);
        gpu_write(t);
        if (action != TGPU_RUN_FRESH) begin_polygon_run(t);
    }
    if (!poly_upload_cached(JPT_VK_POLY_TEXELS, texels, p->texels, p->texel_bytes, upload)) return GPU_POLYGON_NO_RESOURCES;
    V.poly.texel_offset = (uint32_t)texels;
    if (p->kind == GPU_POLYGON_BUMP) {
        int64_t table, clut;
        table = tgpu_texture_cache_find_checked(&V.poly.bump_table_cache, p->bump_table, TGPU_BUMP_TABLE_BYTES, &upload);
        if (table < 0 || !poly_upload_cached(JPT_VK_POLY_BUMP_TABLES, table, p->bump_table, TGPU_BUMP_TABLE_BYTES, upload))
            return GPU_POLYGON_NO_RESOURCES;
        V.poly.bump_table_offset = (uint32_t)table;
        clut = tgpu_texture_cache_find_checked(&V.poly.bump_clut_cache, (const uint8_t*)p->clut, p->clut_entries * 2u,
                                               &upload);
        if (clut < 0 || !poly_upload_cached(JPT_VK_POLY_BUMP_CLUT, clut, p->clut, p->clut_entries * 2u, upload))
            return GPU_POLYGON_NO_RESOURCES;
        V.poly.bump_clut_offset = (uint32_t)clut / 2u;
    } else {
        V.poly.clut_slot = tgpu_clut_cache_find(&V.poly.cluts, p->clut, &upload);
        if (!poly_upload_cached(JPT_VK_POLY_CLUT, (int64_t)V.poly.clut_slot * TGPU_CLUT_ENTRIES * 2, p->clut,
                                TGPU_CLUT_ENTRIES * 2u, upload))
            return GPU_POLYGON_NO_RESOURCES;
    }
    V.poly.prepared = t;
    return GPU_POLYGON_READY;
}

/* Draws a polygon the preparation accepted. */
static int vk_win_texture_polygon(void* surface_iface, const gpu_texture_polygon_t* p) {
    dd_surface_t* s;
    vk_win_target_t* t = poly_target(surface_iface, &s);
    jpt_vk_poly_constants_t c;
    d3dt_rect_t drawn;
    float ox, oy;
    if (!t || t != V.poly.prepared || p->count < 3 || p->count > JPT_VK_POLY_MAX_VERTICES) return 0;
    V.poly.prepared = NULL;
    memset(&c, 0, sizeof c);
    drawn = poly_box(p, s, &ox, &oy);
    for (unsigned i = 0; i < p->count; i++) {
        c.xy[i >> 1][(i & 1u) * 2u] = p->x[i] + ox;
        c.xy[i >> 1][(i & 1u) * 2u + 1u] = p->y[i] + oy;
    }
    memcpy(c.u, p->plane[0], 3 * sizeof(float));
    memcpy(c.v, p->plane[1], 3 * sizeof(float));
    memcpy(c.w, p->plane[2], 3 * sizeof(float));
    c.origin[0] = p->plane[3][0] + ox;
    c.origin[1] = p->plane[3][1] + oy;
    t->internal_w = V.rt_w;
    t->internal_h = V.rt_h;
    c.size[0] = (float)t->legacy_w;
    c.size[1] = (float)t->legacy_h;
    c.size[2] = (float)t->legacy_w / (float)(V.rt_w ? V.rt_w : t->legacy_w);
    c.size[3] = (float)t->legacy_h / (float)(V.rt_h ? V.rt_h : t->legacy_h);
    c.clamp[0] = p->umin;
    c.clamp[1] = p->umax;
    c.clamp[2] = p->vmax;
    c.clamp[3] = p->clamped ? 1.0f : 0.0f;
    c.tex[0] = V.poly.texel_offset;
    c.tex[1] = p->line;
    c.tex[2] = p->umask;
    c.tex[3] = p->tile_mask;
    c.lut[0] = V.poly.clut_slot * TGPU_CLUT_ENTRIES;
    c.bump[0] = V.poly.bump_table_offset;
    c.bump[1] = p->light_theta;
    c.bump[2] = V.poly.bump_clut_offset;
    if (!jpt_vk_renderer_poly_draw(V.vk, p->kind == GPU_POLYGON_BUMP, &c, p->count)) {
        V.poly.failed_draws++;
        return 0;
    }
    gpu_box_add(t, &drawn);
    V.poly.drawn++;
    return 1;
}

static uint32_t vk_win_lock_caller(void) { return vkp_lock_caller(); }

int vk_win_test_enable_headless(uint32_t width, uint32_t height) {
    memset(&V, 0, sizeof V);
    V.test_headless = 1;
    V.win_w = width;
    V.win_h = height;
    V.active = 1;
    V.configured = 1;
    V.msaa_request = 1u;
    V.filter_cfg = JPT_FILTER_BILINEAR;
    V.filter_d3dr = D3DR_FILTER_BILINEAR;
    V.stats_on = 1;
    init_rgb565_table();
    return 1;
}

void vk_win_test_set_stats(int on) { V.stats_on = on ? 1 : 0; }
void vk_win_test_set_effects(int water_refraction, int enhanced_sky, int ao_level) {
    fx_switch_start(&V.fx_water, water_refraction);
    fx_switch_start(&V.fx_sky, enhanced_sky);
    V.ao_level = ao_level;
}

void vk_win_test_shutdown(void) {
    release_vk_instance();
    free(V.vtx_buf);
    free(V.rgba_scratch);
    free(V.fe_rgba);
    free(V.ptr_rgba);
    free(V.ui_rgba);
    free(V.overlay_rgba);
    free(V.overlay_cpu_mask);
    free(V.overlay_new_mask);
    free(V.overlay_gpu_mask);
    free(V.overlay_upload_mask);
    free(V.overlay_hash);
    free(V.sub_rgba);
    free(V.still_rgba);
    free(V.readback_scratch);
    free(V.wave_buf);
    for (int i = 0; i < V.tex_count; i++) free(V.tex_entries[i]);
    free(V.tex_entries);
    memset(&V, 0, sizeof V);
}

void vk_win_test_set_filter(int jpt_filter) {
    release_vk_instance();
    V.filter_cfg = jpt_filter;
    V.filter_d3dr = jpt_filter == JPT_FILTER_ORIGINAL ? D3DR_FILTER_ORIGINAL
                    : jpt_filter == JPT_FILTER_BILINEAR ? D3DR_FILTER_BILINEAR
                                                        : D3DR_FILTER_ANISOTROPIC;
}

void vk_win_test_account(vka_t* out) {
    if (out) *out = V.acc;
}

int vk_win_test_readback(uint8_t* rgba, size_t bytes, uint32_t* width, uint32_t* height) {
    return V.vk && jpt_vk_renderer_readback_rgba(V.vk, rgba, width, height, bytes);
}

void vk_win_test_stats(jpt_vk_renderer_stats_t* out) {
    if (out) stats_snapshot(out);
}

void vk_win_test_bridge_stats(vk_win_test_bridge_stats_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->guest_draws = V.guest_draws;
    for (int i = 0; i < BR_SKIP_COUNT; i++) out->bridge_skipped += V.bridge_skips[i];
    out->texture_skips = V.bridge_skips[BR_SKIP_TEXTURE];
    out->untracked_writes = V.tex_untracked_writes;
    out->palette_refreshes = V.tex_palette_refresh;
    out->water_requested = V.water_requested;
    out->sky_requested = V.sky_requested;
    out->composites = V.sync_composites;
    out->uploads = V.sync_uploads;
    out->readbacks = V.sync_readbacks;
    out->tiles_read = V.tiles_read;
    out->tiles_kept = V.tiles_kept;
    out->readbacks_avoided = V.tile_reads_avoided;
    out->readback_rects = V.tile_rects;
    out->readback_failures = V.sync_readback_failures;
    out->overlay_failures = V.sync_overlay_failures;
    out->cpu_polygons = V.cpu_polygons;
    out->last_frame_reads = V.last_frame_reads;
    out->still_draws = V.still_draws;
    out->mipped_textures = V.mipped_textures;
    out->still_uploads = V.still_uploads;
    out->validation_errors = V.vk ? jpt_vk_renderer_validation_errors(V.vk) : 0;
    out->validation_enabled = V.vk ? jpt_vk_renderer_validation_enabled(V.vk) : 0;
}

void vk_win_test_set_ui_state(int ao_scene) {
    if (V.active_target) V.active_target->ao_scene = ao_scene;
}

const jpt_win_backend_ops_t jpt_vulkan_backend_ops = {
    .id = JPT_RENDERER_VULKAN,
    .key = "vulkan",
    .short_name = "Vulkan",
    .hal_description = "Vulkan hardware (Trespasser compatibility layer)",
    .configure = vk_win_configure,
    .startup = vk_win_startup,
    .hardware = vk_win_hardware,
    .hardware_frames = vk_win_hardware_frames,
    .name = vk_win_name,
    .scene_view = vk_win_scene_view,
    .render_box = vk_win_render_box,
    .frame_size = vk_win_frame_size,
    .set_debug_output = vk_win_set_debug_output,
    .configure_ao = vk_win_configure_ao,
    .configure_effects = vk_win_configure_effects,
    .sky_begin = vk_win_sky_begin,
    .sky_end = vk_win_sky_end,
    .summary = vk_win_summary,
    .device_lost = vk_win_device_lost,
    .attach = vk_win_attach,
    .detach = vk_win_detach,
    .begin_scene = vk_win_begin_scene,
    .end_scene = vk_win_end_scene,
    .state_will_change = vk_win_state_will_change,
    .state_kept_batch = vk_win_state_kept_batch,
    .draw = vk_win_draw,
    .clear = vk_win_clear,
    .texture_format_known = vk_win_texture_format_known,
    .surface_cpu_access = vk_win_surface_cpu_access,
    .surface_cpu_written = vk_win_surface_cpu_written,
    .surface_unlocked = vk_win_surface_unlocked,
    .surface_fill = vk_win_surface_fill,
    .flip = vk_win_flip,
    .frame_presented = vk_win_frame_presented,
    .surface_freed = vk_win_surface_freed,
    .texture_loaded = vk_win_texture_loaded,
    .set_ui_layer = vk_win_set_ui_layer,
    .set_frontend = vk_win_set_frontend,
    .set_frontend_pointer = vk_win_set_frontend_pointer,
    .frontend_failed = vk_win_frontend_failed,
    .frontend_release = vk_win_frontend_release,
    .draw_still_picture = vk_win_draw_still_picture,
    .text_box = vk_win_text_box,
    .cpu_polygon = vk_win_cpu_polygon,
    .cpu_polygon_done = vk_win_cpu_polygon_done,
    .texture_polygon_prepare = vk_win_texture_polygon_prepare,
    .texture_polygon = vk_win_texture_polygon,
    .texture_polygon_reset = vk_win_texture_polygon_reset,
    .texture_polygon_warm = vk_win_texture_polygon_warm,
    .lock_caller = vk_win_lock_caller,
    .present_adapter = vk_win_present_adapter,
    .present_frame = vk_win_present_frame,
    .present_release = vk_win_present_release,
    .present_set_vsync = vk_win_present_set_vsync,
    .present_set_graphics_card = vk_win_present_set_graphics_card,
    .present_on_resize = vk_win_present_on_resize,
    .present_set_fullscreen = vk_win_present_set_fullscreen,
    .present_video_hd = vk_win_present_video_hd,
    .present_video_frame = vk_win_present_video_frame,
};
