#ifndef TRESPASSER_RENDERER_BACKEND_H
#define TRESPASSER_RENDERER_BACKEND_H

#include "d3d_legacy.h"
#include "host.h"
#include "jpt_renderer.h"
#include "vk_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    jpt_renderer_t requested;
    int system_ddraw;
    const char* gpu_request;
    const char* cache_path;
    unsigned box_w, box_h;
    int filter;
    unsigned anisotropy, msaa;
    int vsync, stretch, aspect;
    int ao_level;
    int water_refraction;
    int enhanced_sky;
} jpt_win_renderer_cfg_t;

typedef struct {
    jpt_renderer_t id;
    const char* key;
    const char* short_name;
    const char* hal_description;

    void (*configure)(const jpt_win_renderer_cfg_t* cfg);
    int (*startup)(void);
    int (*hardware)(void);
    unsigned long long (*hardware_frames)(void);
    const char* (*name)(void);
    int (*scene_view)(float out[4]);
    void (*render_box)(unsigned* w, unsigned* h);
    int (*frame_size)(int guest_w, int guest_h, int* w, int* h);
    int (*set_debug_output)(const char* mode);
    void (*configure_ao)(int level);
    void (*configure_effects)(int water_refraction, int enhanced_sky);
    void (*sky_begin)(const float sky[5]);
    void (*sky_end)(void);
    void (*summary)(void);
    void (*device_lost)(void);

    HRESULT (*attach)(d3d_device_t* dev);
    void (*detach)(d3d_device_t* dev);
    void (*begin_scene)(d3d_device_t* dev);
    void (*end_scene)(d3d_device_t* dev);
    void (*state_will_change)(d3d_device_t* dev, int change, DWORD state, DWORD value);
    void (*state_kept_batch)(d3d_device_t* dev);
    HRESULT (*draw)(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                    DWORD index_count);
    HRESULT (*clear)(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z);
    int (*texture_format_known)(const DDPIXELFORMAT* pf);

    void (*surface_cpu_access)(dd_surface_t* s, int will_write, cpu_access_t access);
    void (*surface_cpu_written)(dd_surface_t* s, const RECT* r);
    int (*surface_unlocked)(dd_surface_t* s);
    int (*surface_fill)(dd_surface_t* s, const RECT* r, DWORD color);
    int (*flip)(dd_surface_t* front, int vsync);
    void (*frame_presented)(double present_ms);
    void (*surface_freed)(dd_surface_t* s);
    void (*texture_loaded)(dd_surface_t* dst, const dd_surface_t* src);

    void (*set_ui_layer)(const uint16_t* bits, long pitch, int guest_w, int guest_h);
    void (*set_frontend)(const uint32_t* pixels, int w, int h, const RECT* dirty);
    void (*set_frontend_pointer)(const uint32_t* sprite, int size, unsigned generation, int x, int y);
    int (*frontend_failed)(void);
    void (*frontend_release)(void);
    int (*draw_still_picture)(void* surface, const uint16_t* bits, long pitch, int w, int h);
    void (*text_box)(HDC dc, const RECT* box);

    void (*cpu_polygon)(void* surface_iface, const long box[4], uint32_t origin_bytes, int reads);
    void (*cpu_polygon_done)(void);
    gpu_polygon_status_t (*texture_polygon_prepare)(void* surface_iface, const gpu_texture_polygon_t* p);
    int (*texture_polygon)(void* surface_iface, const gpu_texture_polygon_t* p);
    void (*texture_polygon_reset)(void);
    void (*texture_polygon_warm)(void);
    uint32_t (*lock_caller)(void);

    int (*present_adapter)(dd_adapter_t* out);
    void (*present_frame)(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                          const DDGAMMARAMP* gamma, int gamma_changed, int vsync, unsigned generation);
    void (*present_release)(void);
    void (*present_set_vsync)(int on);
    void (*present_set_graphics_card)(const char* name);
    void (*present_on_resize)(uint32_t w, uint32_t h);
    void (*present_set_fullscreen)(int on);
    int (*present_video_hd)(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                            const video_subtitle_image_t* subtitle);
    int (*present_video_frame)(HWND window, const jpt_vk_video_frame_t* frame, const video_subtitle_image_t* subtitle);
} jpt_win_backend_ops_t;

typedef struct {
    jpt_renderer_t requested;
    jpt_renderer_t chosen;
    jpt_renderer_t active;
    int initialized;
    int fallback;
    int probe_failed;
    char reason[192];
    const jpt_win_backend_ops_t* ops;
} jpt_win_renderer_state_t;

extern const jpt_win_backend_ops_t jpt_d3d11_backend_ops;
extern const jpt_win_backend_ops_t jpt_vulkan_backend_ops;
extern const jpt_win_backend_ops_t jpt_software_backend_ops;

/* Pure backend factory resolution; returns NULL for JPT_RENDERER_AUTO or invalid values. */
const jpt_win_backend_ops_t* renderer_backend_resolve_ops(jpt_renderer_t backend);

/* Resolves requested/chosen policy and selects the concrete backend vtable without starting it yet. */
void renderer_backend_configure_with_probe(const jpt_win_renderer_cfg_t* cfg, int d3d11_available, int vulkan_available,
                                           const char* vulkan_probe_summary);
void renderer_backend_configure(const jpt_win_renderer_cfg_t* cfg);
int renderer_backend_startup(void);
void renderer_backend_reset_for_test(int suppress_assert);

const jpt_win_renderer_state_t* renderer_backend_state(void);
jpt_renderer_t renderer_backend_requested(void);
jpt_renderer_t renderer_backend_chosen(void);
jpt_renderer_t renderer_backend_active(void);
const char* renderer_backend_key(void);
const char* renderer_backend_short_name(void);
const char* renderer_backend_hal_description(void);

/* Returns 0 (and asserts in debug) if D3D11 init is attempted while Vulkan is selected. */
int renderer_backend_guard_d3d11(const char* caller);

void present_on_resize(uint32_t w, uint32_t h);
void present_set_fullscreen(int on);
int present_video_frame_gpu(HWND window, const jpt_vk_video_frame_t* frame, const video_subtitle_image_t* subtitle);
int present_video_hd_rgba(HWND window, const uint8_t* rgba, int w, int h, const float norm_rect[4],
                          const video_subtitle_image_t* subtitle);

#ifdef __cplusplus
}
#endif

#endif
