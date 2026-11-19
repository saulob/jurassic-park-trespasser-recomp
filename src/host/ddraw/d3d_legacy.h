#ifndef TRESPASSER_D3D_LEGACY_H
#define TRESPASSER_D3D_LEGACY_H

#include "ddraw_compat.h"
#include "host_policy.h"

#define D3D_RENDER_STATES 256
#define D3D_STAGE_STATES 32
#define D3D_TEXTURE_STAGES 8

typedef struct d3d_viewport d3d_viewport_t;
typedef struct d3d_device d3d_device_t;
typedef struct d3dr_target d3dr_target_t;

struct d3d_viewport {
    dd_face_t face;
    LONG refs;
    d3d_device_t* device;
    d3d_viewport_t* next;
    D3DVIEWPORT2 data;
    int data_set;
};

struct d3d_device {
    dd_face_t face;
    LONG refs;
    dd_device_t* dd;
    dd_surface_t* target;
    d3d_viewport_t* viewports;
    d3d_viewport_t* current;
    int in_scene;
    DWORD render[D3D_RENDER_STATES];
    DWORD stage[D3D_TEXTURE_STAGES][D3D_STAGE_STATES];
    int texture_blend_set;
    dd_surface_t* texture;
    dd_surface_t* upper_textures[D3D_TEXTURE_STAGES];
    D3DMATRIX transforms[3];
    d3dr_target_t* backend;
};

/* d3d_objects.c */
void d3d_init_direct3d_face(dd_device_t* d);
HRESULT d3d_query_direct3d(dd_device_t* d, REFIID riid, void** out);
int d3d_is_texture_iid(REFIID riid);
void d3d_surface_init_texture_face(dd_surface_t* s);
dd_surface_t* d3d_surface_from_texture(void* iface);
int d3d_vtables_complete(void);
void d3d_fill_device_desc(D3DDEVICEDESC* desc);
void d3d_objects_counts(unsigned long* render_states, unsigned long* stage_states, unsigned long* failed_calls);
void d3d_objects_load_counts(unsigned long long* loads, unsigned long long* unchanged, unsigned long long* texels,
                             unsigned long long* changed_texels);

/* d3d11_renderer.c */
enum {
    D3DR_FILTER_ORIGINAL = D3DT_FILTER_ORIGINAL,
    D3DR_FILTER_POINT = D3DT_FILTER_POINT,
    D3DR_FILTER_BILINEAR = D3DT_FILTER_BILINEAR,
    D3DR_FILTER_TRILINEAR = D3DT_FILTER_TRILINEAR,
    D3DR_FILTER_ANISOTROPIC = D3DT_FILTER_ANISOTROPIC
};

int d3dr_hardware(void);
int d3dr_startup(void);
void d3dr_summary(void);
void d3dr_device_lost(void);

HRESULT d3dr_attach(d3d_device_t* dev);
void d3dr_detach(d3d_device_t* dev);
void d3dr_begin_scene(d3d_device_t* dev);
void d3dr_end_scene(d3d_device_t* dev);
/* A state the open batch was drawn with is about to change: D3DR_CHANGE_* and, for render or stage states, which. */
enum { D3DR_CHANGE_RENDER, D3DR_CHANGE_STAGE, D3DR_CHANGE_VIEWPORT, D3DR_CHANGES };
void d3dr_state_will_change(d3d_device_t* dev, int change, DWORD state, DWORD value);
/* A render state change that kept the open batch. */
void d3dr_state_kept_batch(d3d_device_t* dev);
HRESULT d3dr_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                  DWORD index_count);
HRESULT d3dr_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z);
int d3dr_texture_format_known(const DDPIXELFORMAT* pf);

void d3dr_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access);
void d3dr_surface_cpu_written(dd_surface_t* s, const RECT* r);
int d3dr_surface_unlocked(dd_surface_t* s);
int d3dr_surface_fill(dd_surface_t* s, const RECT* r, DWORD color);
int d3dr_flip(dd_surface_t* front, int vsync);
void d3dr_frame_presented(double present_ms);
void d3dr_surface_freed(dd_surface_t* s);
/* IDirect3DTexture::Load, before it copies src into dst. */
void d3dr_texture_loaded(dd_surface_t* dst, const dd_surface_t* src);
/* The game's UI canvas (RGB565, TSYNC_UNWRITTEN texels transparent) drawn over every presented frame; NULL ends it. */
void d3dr_set_ui_layer(const uint16_t* bits, long pitch, int guest_w, int guest_h);
void d3dr_present_dib(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                      const DDGAMMARAMP* gamma, int vsync);

/* d3d11_present.c, shared with the renderer */
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
int present_device(struct ID3D11Device** device, struct ID3D11DeviceContext** context);
unsigned present_device_generation(void);
struct ID3D11RenderTargetView* present_target(HWND window, UINT width, UINT height);
int present_submit(int vsync);

#endif
