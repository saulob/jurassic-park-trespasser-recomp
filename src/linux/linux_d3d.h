/* Direct3D 6 for the guest on Linux: IDirect3D3 (a face of the DirectDraw object), IDirect3DDevice3, IDirect3DViewport3 and
 * IDirect3DTexture2 (a face of the texture surface), with the vtables of Microsoft's d3d.h. linux_d3d.c holds the objects and the
 * state, linux_d3d_target.c draws with it through the GPU operations below (src/vulkan in the game, a recorder in the tests). */
#ifndef JPT_LINUX_D3D_H
#define JPT_LINUX_D3D_H

#include <stddef.h>
#include <stdint.h>

#include "guest_thunks.h"
#include "host_policy.h"
#include "jpt_config.h"
#include "jpt_render_seam.h"

#define LX_GPU_WAVE_SLOTS 4u

typedef struct {
    const char* name;
    /* The renderer can draw this run; asked before the window exists. */
    int (*available)(void);
    /* Sizes the target for a legacy_w x legacy_h picture and opens the frame when none is open; 0 when nothing can be drawn. */
    int (*begin)(uint32_t legacy_w, uint32_t legacy_h);
    void (*target_size)(uint32_t* w, uint32_t* h);
    int (*uses_mips)(void);
    uint64_t (*texture_create)(const jpt_render_tex_desc_t* desc, const void* pixels);
    int (*texture_update)(uint64_t id, const jpt_render_tex_desc_t* desc, const void* pixels, const jpt_render_rect_t* dirty);
    int (*texture_alive)(uint64_t id);
    void (*texture_destroy)(uint64_t id);
    void (*scissor)(int enabled, int32_t x, int32_t y, uint32_t w, uint32_t h);
    int (*draw)(const jpt_render_state_t* state, int prim, const jpt_render_vertex_t* vertices, uint32_t count,
                const uint16_t* indices, uint32_t index_count);
    void (*clear)(int color, uint32_t argb, int depth, float z);
    void (*select_waves)(uint32_t slot);
    void (*water_params)(float strength, float rhw_metre, const float bounds[4], const float* waves, uint32_t w, uint32_t h);
    void (*sky_params)(const float sky[5]);
    void (*ao_params)(int level, float tan_half_height, int gameplay);
    /* The scene so far, in legacy pixels. */
    int (*read_scene)(const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h, uint16_t* out, size_t pitch_pixels);
    /* The last scene presented. */
    int (*read_last)(uint16_t* out, uint32_t w, uint32_t h, size_t pitch_pixels);
    int (*overlay)(const uint32_t* bgra, size_t pitch_pixels, const jpt_render_rect_t* box, uint32_t legacy_w, uint32_t legacy_h,
                   int opaque);
    /* Changes when the renderer, its textures and its target were lost. */
    unsigned (*generation)(void);
} lx_gpu_ops_t;

void lx_d3d_set_gpu(const lx_gpu_ops_t* ops);
const lx_gpu_ops_t* lx_d3d_gpu(void);
/* Direct3D is offered to the guest. */
int lx_d3d_hardware(void);
void lx_d3d_reset(void);

typedef enum { LX_D3D_DIRECT3D3, LX_D3D_DEVICE3, LX_D3D_VIEWPORT3, LX_D3D_TEXTURE2, LX_D3D_INTERFACES } lx_d3d_interface_t;
typedef struct {
    const char* name;
    ht_fn_t fn; /* NULL: refused with DDERR_UNSUPPORTED */
    int argc;   /* with `this`: the dwords the method pops */
} lx_d3d_method_t;
const lx_d3d_method_t* lx_d3d_methods(lx_d3d_interface_t which, unsigned* count);
/* The interface's vtable in guest memory. */
uint32_t lx_d3d_vtable(lx_d3d_interface_t which);

typedef struct {
    unsigned devices, viewports;
    unsigned long long draws, skipped_draws, clears, refused_states, unsupported_calls, water_draws, wave_uploads, sky_draws,
        texture_uploads, partial_uploads, dynamic_textures, readbacks, composites, uploads, gpu_frames, cpu_frames, cpu_polygons;
} lx_d3d_stats_t;
void lx_d3d_stats(lx_d3d_stats_t* out);
void lx_d3d_log_summary(void);

/* What the DirectDraw surfaces tell the target and the textures. Slots are linux_ddraw.c's. */
void lx_d3d_cpu_access(int slot, cpu_access_t access);
/* rect: left, top, right, bottom; NULL for the whole surface. */
void lx_d3d_cpu_written(int slot, const int32_t* rect);
/* Unlock: 1 when the Lock was one that touches no pixel. */
int lx_d3d_unlocked(int slot);
/* A colour fill of a render target; 0 when the caller fills the pixels. */
int lx_d3d_fill(int slot, const int32_t rect[4], uint32_t color);
void lx_d3d_surface_freed(int slot);
/* A custom level's picture stands for the texture until it is written again; 0: none does. */
void lx_d3d_texture_override(uint32_t iface, int id);
/* Before the buffers of the chain are exchanged: 1 when the GPU's scene is the picture. The CPU copy is complete otherwise. */
int lx_d3d_flip(const int* chain, unsigned count);
void lx_d3d_flipped(int front, const int* chain, unsigned count);

/* The launcher's effects. */
void lx_d3d_configure_effects(int water_refraction, int enhanced_sky, int ao_level);
void lx_d3d_sky_begin(const float sky[5]);
void lx_d3d_sky_end(void);
/* The next draw is the layers of the custom sky instead. */
void lx_d3d_sky_layers(int on);
void lx_d3d_sky_free(void* gpu);
/* A row of a water texture was written: the main screen Lock that follows it touches no pixel. */
void lx_d3d_water_row(void);
/* Called at BeginScene; gives the level's fog colour, 0 when there is none. */
void lx_d3d_set_fog_sync(int (*sync)(uint32_t* color));
/* Whether the scene may be shown without the CPU picture (no menu or loading layer over it), and the camera's tan(half height). */
void lx_d3d_set_view(int (*scene_shown)(void), float (*tan_half_height)(void));
/* The guest function that locks the screen, for target_access_reads. */
void lx_d3d_set_lock_caller(uint32_t (*caller)(void));

/* A polygon the game rasterises itself: box in its raster's pixels, the raster's offset in the surface in bytes, and whether it reads
 * what is under it. The Lock that follows is limited to the box. */
void lx_d3d_cpu_polygon(uint32_t surface_iface, const long box[4], uint32_t origin_bytes, int reads);
void lx_d3d_cpu_polygon_done(void);

/* linux_d3d_guest.c: the guest hooks behind the effects and the software polygons (sky, water rows, DrawPolygon). */
void lx_d3d_guest_install(const jpt_config_t* cfg);
void lx_d3d_guest_remove(void);

#endif
