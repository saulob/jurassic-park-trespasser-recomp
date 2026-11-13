/*
 * Direct3D 6 rendering on the D3D11 device the presenter owns. A device's render target surface
 * gets a GPU colour target (at the internal resolution, optionally multisampled) and a depth
 * buffer; the surface's own pixels stay the CPU copy the game Locks. target_sync.c decides where
 * the picture is: the GPU target once Direct3D draws, the CPU copy only after a readback, a
 * clear recreated on the CPU or a copy of a CPU-made frame. Only pixels the CPU really changed
 * are composited over the GPU target, so untouched pixels keep the internal resolution and a
 * stale CPU copy never replaces the GPU picture. Everything runs on the guest thread, which owns
 * the immediate context.
 */
#define COBJMACROS
#include "d3d_legacy.h"
#include "d3d11_shaders.h"
#include "host.h"
#include "jpt_display.h"
#include "loading_hd.h"
#include "page_arena.h"
#include "page_guard.h"
#include "target_sync.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

#define VB_INITIAL (4u << 20)
#define IB_INITIAL (2u << 20)
#define SUMMARY_INTERVAL 60.0
#define PERF_INTERVAL 5.0
#define MAX_BIAS_STEPS 17
#define MAX_STATE_CLASSES 48
#define MAX_READBACK_CALLERS 24
#define MAX_UNKNOWN_FORMATS 16
/* CRasterVid::Lock and CRasterWin::Lock (RTTI vtables 0x0064DAE4 / 0x0064DB54): every CPU access to
 * the game's rasters goes through them, so a readback is charged to the function that called them. */
#define RASTER_LOCK 0x00503400u
#define RASTER_WIN_LOCK 0x00504D20u
#define RASTER_WIN_LOCK_THUNK 0x00504D40u
#define UNTRACKED_LOG_LIMIT 8
#define WATCH_PAGES 1024
#define STATE_TEX_NONE 0u
#define STATE_TEX_UNUSABLE 0xFu

enum { PERF_TRANSLATE, PERF_DRAW, PERF_UPLOAD, PERF_READBACK, PERF_OVERLAY, PERF_PRESENT, PERF_COUNT };
/* DrawPrimitive runs thousands of times a frame: one call in TRANSLATE_SAMPLE is timed and counted for all of them. */
#define TRANSLATE_SAMPLE 8u

typedef struct {
    uint32_t caller;
    readback_reason_t reason;
    unsigned long long count, bytes;
    double ms;
} readback_caller_t;

typedef struct {
    double ms[PERF_COUNT];
    unsigned long long count[PERF_COUNT];
    unsigned long long readback_n[READBACK_REASON_COUNT];
    double readback_ms[READBACK_REASON_COUNT];
    unsigned long long readback_bytes;
    unsigned long long frames, no_read_accesses, synthesized, front_copies;
    unsigned long long deferred_locks, deferred_untouched, deferred_touched;
    unsigned long long draws, triangles, terrain_triangles, target_accesses, composites, composited_pixels;
    unsigned long long composited_black, uploads;
    unsigned long long cpu_polygons, cpu_polygon_locks, cpu_polygon_readbacks, cpu_polygon_readback_pixels;
    unsigned long long cpu_polygon_adopted, inert_locks, filled_pixels, scanned_pixels, text_boxes, watch_reads;
    unsigned long long texture_joins;
    readback_caller_t callers[MAX_READBACK_CALLERS];
} perf_window_t;

typedef struct {
    uint32_t key;
    unsigned long long draws, triangles;
} state_class_t;

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float u, v;
    uint32_t layer; /* the texture's slice in its array */
} tl_vertex_t;

typedef struct {
    uint32_t flags, blend_mode, alpha_func;
    float alpha_ref;
    float fog[4];
} draw_constants_t;

typedef struct {
    float rect[4], uv[4], color[4];
    float depth, pad[3];
} pass_constants_t;

typedef struct texture_pool texture_pool_t;

/* A texture is a slice of a pool's array, or its own one-slice array (dynamic ones, and when no pool can take it). */
typedef struct d3dr_texture {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    texture_pool_t* pool;
    UINT slice;
    unsigned long long batch; /* the batch that last drew it */
    UINT width, height, levels;
    int format;
    int alpha_kind;
    d3dt_upload_history_t history;
    unsigned generation;
    size_t bytes;
    uint32_t palette_sum;
    unsigned long untracked;
    unsigned long uploads;
} d3dr_texture_t;

struct d3dr_target {
    d3d_device_t* device;
    dd_surface_t* surface;
    UINT legacy_w, legacy_h, width, height, samples;
    unsigned generation;
    ID3D11Texture2D* color;
    ID3D11RenderTargetView* rtv;
    ID3D11Texture2D* single;
    ID3D11ShaderResourceView* single_srv;
    ID3D11Texture2D* depth;
    ID3D11DepthStencilView* dsv;
    ID3D11Texture2D* packed;
    ID3D11RenderTargetView* packed_rtv;
    ID3D11Texture2D* staging;
    ID3D11Texture2D* picture;
    ID3D11ShaderResourceView* picture_srv;
    uint16_t* reference;
    uint32_t* scratch;
    d3dt_rect_t dirty;
    tsync_t sync;
    tsync_deferred_t deferred;
    d3dt_rect_t deferred_region;
    readback_reason_t deferred_reason;
    uint32_t deferred_caller;
    int inert_lock;
    /* After a Flip the reference no longer matches the CPU buffer: in a lazy capture only the tiles in fresh were
     * captured since, and nothing else is compared or uploaded. */
    int lazy;
    tsync_tiles_t fresh;
    int frame_started;
    /* Since the last full clear (gpu_box_known), the GPU has drawn only inside gpu_box (legacy pixels); the rest of
     * the target holds gpu_clear. A readback then needs only the box. */
    int gpu_box_known;
    d3dt_rect_t gpu_box;
    uint16_t gpu_clear;
};

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);

static struct {
    int requested, hardware, prepared, prepare_failed;
    int filter;
    unsigned msaa_request, fixed_w, fixed_h, anisotropy;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    unsigned generation;
    compile_fn compile;
    ID3DBlob* code[D3D11_SHADER_COUNT];
    ID3D11VertexShader* vs_tl;
    ID3D11VertexShader* vs_quad;
    ID3D11PixelShader* ps[D3D11_SHADER_COUNT];
    ID3D11InputLayout* layout;
    ID3D11Buffer *vb, *ib, *cb_frame, *cb_draw, *cb_pass;
    UINT vb_size, ib_size, vb_pos, ib_pos;
    ID3D11BlendState* blend[2][14][14];
    ID3D11BlendState *blend_overlay, *blend_none, *blend_nowrite, *blend_premultiplied;
    ID3D11DepthStencilState* depth[2][2][9];
    ID3D11DepthStencilState* depth_write_always;
    ID3D11RasterizerState* raster[4][4][2][MAX_BIAS_STEPS];
    ID3D11RasterizerState* raster_pass;
    ID3D11SamplerState* sampler[2][2][5][5];
    ID3D11SamplerState *point_clamp, *linear_clamp;
    ID3D11Texture2D* gamma_tex;
    ID3D11ShaderResourceView* gamma_srv;
    WORD gamma_last[3][256];
    int gamma_valid;
    ID3D11Texture2D* screen;
    ID3D11ShaderResourceView* screen_srv;
    UINT screen_w, screen_h;
    uint32_t* screen_scratch;
    ID3D11Texture2D* ui_tex;
    ID3D11ShaderResourceView* ui_srv;
    ID3D11Texture2D* still_tex;
    ID3D11ShaderResourceView* still_srv;
    UINT still_w, still_h;
    const void* still_bits;
    uint32_t still_key;
    unsigned long long still_uploads, still_draws;
    uint32_t ui_pixels[JPT_UI_CANVAS_W * JPT_UI_CANVAS_H];
    struct {
        const uint16_t* bits;
        long pitch;
        int guest_w, guest_h;
    } ui;
    struct {
        const uint32_t* pixels;
        int w, h, failed, pending;
        RECT dirty;
        ID3D11Texture2D* tex;
        ID3D11ShaderResourceView* srv;
        int tex_w, tex_h;
        const uint32_t* pointer;
        int pointer_size, pointer_x, pointer_y;
        unsigned pointer_generation, pointer_uploaded;
        ID3D11Texture2D* pointer_tex;
        ID3D11ShaderResourceView* pointer_srv;
        int pointer_tex_size;
    } fe;
    ID3D11Texture2D* last_frame;
    ID3D11ShaderResourceView* last_frame_srv;
    UINT last_w, last_h, last_legacy_w, last_legacy_h;
    int last_valid;
    ID3D11Texture2D *front_down, *front_staging;
    ID3D11RenderTargetView* front_rtv;
    UINT front_w, front_h;
    draw_constants_t draw_cb;
    int draw_cb_valid;
    float frame_cb[4];
    int frame_cb_valid;
    d3d_device_t* batch_dev;
    D3D11_PRIMITIVE_TOPOLOGY batch_topology;
    /* Draws join the open batch while their texture's array is one of the D3D11_WORLD_ARRAYS it binds; the slice,
     * the array and the texture's flags travel in each vertex. */
    unsigned long long batch_serial;
    ID3D11ShaderResourceView* batch_srv[D3D11_WORLD_ARRAYS];
    unsigned batch_arrays;
    d3dr_texture_t* batch_tex;
    int batch_has_texture;
    /* What flush() last bound; any other binding clears valid. */
    struct {
        int valid;
        ID3D11RenderTargetView* rtv;
        ID3D11DepthStencilView* dsv;
        ID3D11Buffer *vb, *ib;
        ID3D11ShaderResourceView* srv[D3D11_WORLD_ARRAYS];
        unsigned srvs;
        ID3D11SamplerState* sampler[D3D11_DEVICE_SAMPLER + 1];
        ID3D11RasterizerState* raster;
        ID3D11BlendState* blend;
        ID3D11DepthStencilState* depth;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        D3D11_VIEWPORT vp;
        D3D11_RECT scissor;
    } bound;
    unsigned pool_pages, pool_pages_peak;
    unsigned translate_calls;
    unsigned long long texture_joins;
    /* Uploads in the window by kind (UPLOAD_*): count, texels, ms. */
    struct {
        unsigned long long n, texels;
        double ms;
    } uploads_by[3];
    tl_vertex_t* verts;
    uint32_t* indices;
    UINT nverts, nindices, cap_verts, cap_indices;
    uint32_t* rgb565_table;
    unsigned long long frames_hw, frames_dib, draw_calls, triangles, vertices, index_count, textures_bound;
    unsigned long long texture_uploads, readbacks, overlays, clears, selfmod_hw;
    size_t texture_memory;
    int depth_active, first_frame_logged;
    unsigned long selfmod_mark;
    double last_summary;
    unsigned char fvf_logged, type_logged, scene_logged;
    double qpc_ms;
    perf_window_t perf, total;
    double perf_start, last_present;
    state_class_t classes[MAX_STATE_CLASSES];
    unsigned class_count;
    struct {
        uint32_t flags, bits, r, g, b, a, caps, width, height, caller;
    } unknown[MAX_UNKNOWN_FORMATS];
    unsigned unknown_count;
    unsigned devices, textures_alive;
    unsigned textures_peak, textures_created_window, textures_released_window;
    unsigned textures_reused_window, textures_parked_window;
    unsigned long long textures_reused;
    uint32_t* mip_chain;
    size_t mip_capacity;
    unsigned long long textures_mipped, textures_dynamic, mip_texels;
    double mip_ms;
    unsigned anisotropy_logged, msaa_logged;
    unsigned long long va_total, va_used, va_peak;
    uint32_t charge_caller;
    unsigned long long untracked_writes;
    unsigned untracked_logged;
    /* [0] menus, in-game dialogs, videos and loading, [1] gameplay */
    unsigned long long cpu_pixels[2], black_over_gpu[2], black_over_unknown[2];
    int debug_output;
    unsigned char sampler_logged[2][2][5][5];
    uint32_t* upload_buffer;
    size_t upload_capacity;
    d3dr_target_t* deferred;
    int capture_text;
    d3dr_target_t* text_target;
} R;

static const char* const k_filter_names[] = { "original", "point", "bilinear", "trilinear", "anisotropic" };

static void log_hr(const char* what, HRESULT hr) { host_log("d3d11", "%s failed, HRESULT 0x%08lX", what, (unsigned long)hr); }

static double now_seconds(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static int64_t ticks(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double elapsed_ms(int64_t start) {
    if (R.qpc_ms == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        R.qpc_ms = 1000.0 / (double)f.QuadPart;
    }
    return (double)(ticks() - start) * R.qpc_ms;
}

static void perf_add(int category, int64_t start) {
    double ms = elapsed_ms(start);
    R.perf.ms[category] += ms;
    R.perf.count[category]++;
}

#define SAFE_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

/* ---- configuration and start-up ---- */

void d3dr_configure(int hardware, unsigned box_w, unsigned box_h, int filter, unsigned anisotropy, unsigned msaa) {
    R.requested = hardware;
    R.fixed_w = box_w;
    R.fixed_h = box_h;
    R.filter = filter;
    R.anisotropy = d3dt_pick_anisotropy(anisotropy < 2 ? 2 : anisotropy, D3D11_REQ_MAXANISOTROPY);
    R.msaa_request = msaa;
}

int d3dr_hardware(void) { return R.hardware; }

int d3dr_set_debug_output(const char* mode) {
    static const char* const modes[] = { "texture", "vertex-color", "depth" };
    for (int i = 0; i < 3; i++)
        if (!strcmp(mode, modes[i])) {
            R.debug_output = i + 1;
            return 1;
        }
    return 0;
}

const char* d3dr_name(void) { return R.hardware ? "D3D11" : "Software"; }

static int compile_shaders(void) {
    HMODULE dll = LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) dll = LoadLibraryA("d3dcompiler_47.dll");
    R.compile = dll ? (compile_fn)GetProcAddress(dll, "D3DCompile") : NULL;
    if (!R.compile) {
        host_log("d3d11", "d3dcompiler_47.dll (D3DCompile) is unavailable");
        return 0;
    }
    for (int i = 0; i < D3D11_SHADER_COUNT; i++) {
        ID3DBlob* errors = NULL;
        HRESULT hr = R.compile(d3d11_shader_source, strlen(d3d11_shader_source), "trespasser_d3d6", NULL, NULL,
                               d3d11_shader_entries[i].entry, d3d11_shader_entries[i].profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &R.code[i], &errors);
        if (FAILED(hr)) {
            host_log("d3d11", "shader %s failed to compile: %s", d3d11_shader_entries[i].entry,
                     errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "no message");
            SAFE_RELEASE(errors);
            return 0;
        }
        SAFE_RELEASE(errors);
    }
    return 1;
}

static ID3D11Buffer* make_buffer(UINT size, UINT bind) {
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer* buffer = NULL;
    memset(&desc, 0, sizeof desc);
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(R.dev, &desc, NULL, &buffer))) return NULL;
    return buffer;
}

static ID3D11BlendState* make_blend(int enable, int src, int dst, int src_a, int dst_a, UINT8 mask) {
    D3D11_BLEND_DESC desc;
    ID3D11BlendState* state = NULL;
    memset(&desc, 0, sizeof desc);
    desc.RenderTarget[0].BlendEnable = enable;
    desc.RenderTarget[0].SrcBlend = (D3D11_BLEND)src;
    desc.RenderTarget[0].DestBlend = (D3D11_BLEND)dst;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = (D3D11_BLEND)src_a;
    desc.RenderTarget[0].DestBlendAlpha = (D3D11_BLEND)dst_a;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = mask;
    ID3D11Device_CreateBlendState(R.dev, &desc, &state);
    return state;
}

static ID3D11SamplerState* make_sampler(D3D11_FILTER filter, int address_u, int address_v, float max_lod, UINT anisotropy) {
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState* state = NULL;
    memset(&desc, 0, sizeof desc);
    desc.Filter = filter;
    desc.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)address_u;
    desc.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)address_v;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxAnisotropy = anisotropy;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = max_lod;
    ID3D11Device_CreateSamplerState(R.dev, &desc, &state);
    return state;
}

static int create_static_objects(void) {
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(R.dev, ID3D10Blob_GetBufferPointer(R.code[D3D11_SHADER_VS_TL]),
                                         ID3D10Blob_GetBufferSize(R.code[D3D11_SHADER_VS_TL]), NULL, &R.vs_tl);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(R.dev, ID3D10Blob_GetBufferPointer(R.code[D3D11_SHADER_VS_QUAD]),
                                             ID3D10Blob_GetBufferSize(R.code[D3D11_SHADER_VS_QUAD]), NULL, &R.vs_quad);
    for (int i = D3D11_SHADER_PS_FIXED; i < D3D11_SHADER_COUNT && SUCCEEDED(hr); i++)
        if (i != D3D11_SHADER_VS_QUAD)
            hr = ID3D11Device_CreatePixelShader(R.dev, ID3D10Blob_GetBufferPointer(R.code[i]),
                                                ID3D10Blob_GetBufferSize(R.code[i]), NULL, &R.ps[i]);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(R.dev, elements, 5, ID3D10Blob_GetBufferPointer(R.code[D3D11_SHADER_VS_TL]),
                                            ID3D10Blob_GetBufferSize(R.code[D3D11_SHADER_VS_TL]), &R.layout);
    if (FAILED(hr)) {
        log_hr("shader object creation", hr);
        return 0;
    }
    R.vb_size = VB_INITIAL;
    R.ib_size = IB_INITIAL;
    R.vb = make_buffer(R.vb_size, D3D11_BIND_VERTEX_BUFFER);
    R.ib = make_buffer(R.ib_size, D3D11_BIND_INDEX_BUFFER);
    R.cb_frame = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER);
    R.cb_draw = make_buffer(sizeof(draw_constants_t), D3D11_BIND_CONSTANT_BUFFER);
    R.cb_pass = make_buffer(sizeof(pass_constants_t), D3D11_BIND_CONSTANT_BUFFER);
    R.vb_pos = R.ib_pos = 0;
    R.blend_none = make_blend(0, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                              D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE);
    R.blend_overlay = make_blend(1, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                                 D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                     D3D11_COLOR_WRITE_ENABLE_BLUE);
    R.blend_nowrite = make_blend(0, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0);
    R.blend_premultiplied = make_blend(1, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                                       D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                           D3D11_COLOR_WRITE_ENABLE_BLUE);
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(R.dev, &dd, &R.depth_write_always);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(R.dev, &rd, &R.raster_pass);
    R.point_clamp = make_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 1);
    R.linear_clamp = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 1);
    memset(&td, 0, sizeof td);
    td.Width = 256;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(R.dev, &td, NULL, &R.gamma_tex)))
        ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)R.gamma_tex, NULL, &R.gamma_srv);
    R.gamma_valid = 0;
    R.draw_cb_valid = R.frame_cb_valid = 0;
    if (!R.vb || !R.ib || !R.cb_frame || !R.cb_draw || !R.cb_pass || !R.blend_none || !R.blend_overlay || !R.blend_nowrite ||
        !R.depth_write_always || !R.raster_pass || !R.point_clamp || !R.linear_clamp || !R.gamma_srv) {
        host_log("d3d11", "creating the renderer's buffers and states failed");
        return 0;
    }
    return 1;
}

static void release_static_objects(void) {
    SAFE_RELEASE(R.vs_tl);
    SAFE_RELEASE(R.vs_quad);
    for (int i = 0; i < D3D11_SHADER_COUNT; i++) SAFE_RELEASE(R.ps[i]);
    SAFE_RELEASE(R.layout);
    SAFE_RELEASE(R.vb);
    SAFE_RELEASE(R.ib);
    SAFE_RELEASE(R.cb_frame);
    SAFE_RELEASE(R.cb_draw);
    SAFE_RELEASE(R.cb_pass);
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 14; b++)
            for (int c = 0; c < 14; c++) SAFE_RELEASE(R.blend[a][b][c]);
    SAFE_RELEASE(R.blend_overlay);
    SAFE_RELEASE(R.blend_none);
    SAFE_RELEASE(R.blend_nowrite);
    SAFE_RELEASE(R.blend_premultiplied);
    SAFE_RELEASE(R.ui_srv);
    SAFE_RELEASE(R.ui_tex);
    d3dr_frontend_release();
    SAFE_RELEASE(R.still_srv);
    SAFE_RELEASE(R.still_tex);
    R.still_w = R.still_h = 0;
    R.still_bits = NULL;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            for (int c = 0; c < 9; c++) SAFE_RELEASE(R.depth[a][b][c]);
    SAFE_RELEASE(R.depth_write_always);
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
            for (int c = 0; c < 2; c++)
                for (int d = 0; d < MAX_BIAS_STEPS; d++) SAFE_RELEASE(R.raster[a][b][c][d]);
    SAFE_RELEASE(R.raster_pass);
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            for (int c = 0; c < 5; c++)
                for (int d = 0; d < 5; d++) SAFE_RELEASE(R.sampler[a][b][c][d]);
    SAFE_RELEASE(R.point_clamp);
    SAFE_RELEASE(R.linear_clamp);
    SAFE_RELEASE(R.gamma_srv);
    SAFE_RELEASE(R.gamma_tex);
    SAFE_RELEASE(R.screen_srv);
    SAFE_RELEASE(R.screen);
    R.screen_w = R.screen_h = 0;
    SAFE_RELEASE(R.last_frame_srv);
    SAFE_RELEASE(R.last_frame);
    R.last_valid = 0;
    SAFE_RELEASE(R.front_rtv);
    SAFE_RELEASE(R.front_down);
    SAFE_RELEASE(R.front_staging);
    R.front_w = R.front_h = 0;
}

static int ensure_device(void) {
    unsigned generation;
    if (!R.prepared) return 0;
    generation = present_device_generation();
    if (R.dev && R.generation == generation) return 1;
    if (!present_device(&R.dev, &R.ctx)) return 0;
    ID3D11Device_AddRef(R.dev);
    ID3D11DeviceContext_AddRef(R.ctx);
    R.generation = present_device_generation();
    if (!create_static_objects()) {
        release_static_objects();
        return 0;
    }
    return 1;
}

/* Shaders, device and tables, once, whichever renderer the game starts with. */
static int prepare_hardware(void) {
    D3D_FEATURE_LEVEL level;
    if (R.prepared) return 1;
    if (R.prepare_failed) return 0;
    R.prepare_failed = 1;
    if (!compile_shaders()) return 0;
    R.prepared = 1;
    if (!ensure_device()) {
        R.prepared = 0;
        return 0;
    }
    level = ID3D11Device_GetFeatureLevel(R.dev);
    if (level < D3D_FEATURE_LEVEL_10_0) {
        R.prepared = 0;
        host_log("renderer", "feature level %u.%u is below the 10.0 the shaders need", ((unsigned)level >> 12) & 0xF,
                 ((unsigned)level >> 8) & 0xF);
        return 0;
    }
    R.rgb565_table = (uint32_t*)malloc(65536 * sizeof(uint32_t));
    if (!R.rgb565_table) {
        R.prepared = 0;
        return 0;
    }
    for (uint32_t v = 0; v < 65536; v++) R.rgb565_table[v] = d3dt_rgb565_to_bgra((uint16_t)v);
    R.prepare_failed = 0;
    return 1;
}

static const char* anisotropy_text(void) {
    static char text[16];
    if (R.filter != D3DR_FILTER_ANISOTROPIC) return "";
    _snprintf(text, sizeof text - 1, " %ux", R.anisotropy);
    return text;
}

static void log_renderer_line(void) {
    if (!R.hardware) {
        host_log("renderer", "Renderer: Software RGB565: SelfMod rasterises, D3D11 presents");
        return;
    }
    host_log("renderer", "Renderer: D3D11 hardware (Direct3D 6 translated: IDirect3D3/Device3/Viewport3/Texture2)");
    if (R.fixed_w)
        host_log("renderer", "render box %ux%u (each target keeps its surface's shape inside it), texture filter %s%s, "
                 "MSAA requested %ux", R.fixed_w, R.fixed_h, k_filter_names[R.filter], anisotropy_text(), R.msaa_request);
    else
        host_log("renderer", "render resolution original (compatibility size covering the presentation area), texture "
                 "filter %s%s, MSAA requested %ux", k_filter_names[R.filter], anisotropy_text(), R.msaa_request);
}

int d3dr_startup(void) {
    R.batch_serial = 1;
    R.selfmod_mark = selfmod_entries();
    R.last_summary = R.perf_start = now_seconds();
    if (R.requested) {
        R.hardware = prepare_hardware();
        if (!R.hardware) host_log("renderer", "the D3D11 hardware path could not start (see the lines above)");
    }
    log_renderer_line();
    return 1;
}

static void flush(void);
static void apply_sync(d3dr_target_t* t, unsigned acts, const d3dt_rect_t* region, readback_reason_t reason, int exact);
static void check_front(d3dr_target_t* t);
static void settle_deferred(void);
static void drop_deferred(const d3dr_target_t* t);
static void end_polygon_run(const d3dr_target_t* t);

/* ---- per-surface GPU objects ---- */

/* Texture arrays by size and mip count, POOL_SLICES slices each; a page goes when its last slice does. */
#define POOL_SLICES 64u
#define MAX_POOLS 128u

struct texture_pool {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    UINT width, height, levels;
    unsigned generation, used;
    uint32_t free_slices[POOL_SLICES / 32];
};

static texture_pool_t s_pools[MAX_POOLS];

static ID3D11ShaderResourceView* array_view(ID3D11Texture2D* tex, UINT levels, UINT slices) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    ID3D11ShaderResourceView* srv = NULL;
    memset(&desc, 0, sizeof desc);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipLevels = levels;
    desc.Texture2DArray.ArraySize = slices;
    if (FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)tex, &desc, &srv))) return NULL;
    return srv;
}

static ID3D11Texture2D* make_array(UINT w, UINT h, UINT levels, UINT slices) {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D* tex = NULL;
    memset(&desc, 0, sizeof desc);
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = levels;
    desc.ArraySize = slices;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(R.dev, &desc, NULL, &tex))) return NULL;
    return tex;
}

static int free_slice(const texture_pool_t* p) {
    for (unsigned w = 0; w < POOL_SLICES / 32; w++) {
        unsigned long bit;
        if (_BitScanForward(&bit, p->free_slices[w])) return (int)(w * 32 + bit);
    }
    return -1;
}

static int pool_take(d3dr_texture_t* t, UINT w, UINT h, UINT levels) {
    texture_pool_t* p = NULL;
    int slice = -1;
    for (unsigned i = 0; i < MAX_POOLS && slice < 0; i++) {
        texture_pool_t* c = &s_pools[i];
        if (c->tex && c->generation == R.generation && c->width == w && c->height == h && c->levels == levels &&
            (slice = free_slice(c)) >= 0)
            p = c;
    }
    if (!p) {
        for (unsigned i = 0; i < MAX_POOLS && !p; i++)
            if (!s_pools[i].tex) p = &s_pools[i];
        if (!p || !(p->tex = make_array(w, h, levels, POOL_SLICES))) return 0;
        if (!(p->srv = array_view(p->tex, levels, POOL_SLICES))) {
            SAFE_RELEASE(p->tex);
            return 0;
        }
        p->width = w;
        p->height = h;
        p->levels = levels;
        p->generation = R.generation;
        p->used = 0;
        memset(p->free_slices, 0xFF, sizeof p->free_slices);
        slice = 0;
        if (++R.pool_pages > R.pool_pages_peak) R.pool_pages_peak = R.pool_pages;
    }
    p->free_slices[slice / 32] &= ~(1u << (slice % 32));
    p->used++;
    t->pool = p;
    t->slice = (UINT)slice;
    return 1;
}

static void pool_give_back(d3dr_texture_t* t) {
    texture_pool_t* p = t->pool;
    if (!p) return;
    p->free_slices[t->slice / 32] |= 1u << (t->slice % 32);
    t->pool = NULL;
    if (--p->used) return;
    SAFE_RELEASE(p->srv);
    SAFE_RELEASE(p->tex);
    R.pool_pages--;
}

static ID3D11Resource* texture_resource(const d3dr_texture_t* t) {
    return (ID3D11Resource*)(t->pool ? t->pool->tex : t->tex);
}

static UINT texture_subresource(const d3dr_texture_t* t, UINT level) {
    return t->pool ? level + t->slice * t->pool->levels : level;
}

static ID3D11ShaderResourceView* texture_view(const d3dr_texture_t* t) { return t->pool ? t->pool->srv : t->srv; }

/* The open batch still has to draw what this texture holds now. */
static void settle_batch_texture(const d3dr_texture_t* t) {
    if (t && R.nindices && t->batch == R.batch_serial) flush();
}

static void destroy_texture(d3dr_texture_t* t) {
    settle_batch_texture(t);
    pool_give_back(t);
    SAFE_RELEASE(t->srv);
    SAFE_RELEASE(t->tex);
    R.texture_memory -= t->bytes;
    R.textures_alive--;
    R.textures_released_window++;
    free(t);
}

static void release_texture(dd_surface_t* s) {
    d3dr_texture_t* t = (d3dr_texture_t*)s->gpu_texture;
    if (!t) return;
    destroy_texture(t);
    s->gpu_texture = NULL;
}

/* The game drops every texture surface when it releases its Direct3D device (focus loss, menus) and loads the same
 * pixels into new surfaces right after. A texture whose GPU copy matches its surface is parked under the surface's
 * content and adopted by a new surface with the same content, size and conversion; the real device is the same. */
#define PARK_SLOTS 2048u
#define PARK_MAX_BYTES ((size_t)256 << 20)
#define PARK_MAX_SECONDS 30.0

typedef struct {
    d3dr_texture_t* texture;
    uint64_t hash;
    DWORD width, height, key_low, key_high;
    int format, has_key;
    uint32_t palette_sum;
    double parked_at;
} parked_texture_t;

static parked_texture_t s_parked[PARK_SLOTS];
static unsigned s_parked_count;
static size_t s_parked_bytes;

static uint64_t content_hash(const dd_surface_t* s) {
    uint32_t a = 2166136261u, b = 0x9E3779B9u;
    size_t row_bytes = (size_t)s->width * ((s->format.dwRGBBitCount + 7) / 8);
    for (DWORD y = 0; y < s->height; y++) {
        const BYTE* row = s->bits + (size_t)y * (size_t)s->pitch;
        size_t i = 0;
        uint32_t w0, w1;
        for (; i + 8 <= row_bytes; i += 8) {
            memcpy(&w0, row + i, 4);
            memcpy(&w1, row + i + 4, 4);
            a = (a ^ w0) * 16777619u;
            b = (b ^ w1) * 2246822519u;
        }
        for (; i < row_bytes; i++) a = (a ^ row[i]) * 16777619u;
        b = (b ^ y) * 2246822519u;
    }
    return (uint64_t)a << 32 | b;
}

static parked_texture_t parked_key(const dd_surface_t* s, int format, uint32_t palette_sum) {
    parked_texture_t k;
    memset(&k, 0, sizeof k);
    k.hash = content_hash(s);
    k.width = s->width;
    k.height = s->height;
    k.format = format;
    k.palette_sum = palette_sum;
    k.has_key = s->has_src_key;
    if (s->has_src_key) {
        k.key_low = s->src_key.dwColorSpaceLowValue;
        k.key_high = s->src_key.dwColorSpaceHighValue;
    }
    return k;
}

static void unpark(unsigned i, int destroy) {
    s_parked_bytes -= s_parked[i].texture->bytes;
    if (destroy) destroy_texture(s_parked[i].texture);
    s_parked[i] = s_parked[--s_parked_count];
}

static void unpark_oldest(void) {
    unsigned oldest = 0;
    for (unsigned i = 1; i < s_parked_count; i++)
        if (s_parked[i].parked_at < s_parked[oldest].parked_at) oldest = i;
    unpark(oldest, 1);
}

static void flush_parked(void) {
    while (s_parked_count) unpark(s_parked_count - 1, 1);
}

static void age_parked(double now) {
    for (unsigned i = 0; i < s_parked_count;)
        if (now - s_parked[i].parked_at > PARK_MAX_SECONDS) unpark(i, 1);
        else i++;
}

/* The surface was written after its last upload (pages the write watch has not handed over yet). */
static int written_since_upload(dd_surface_t* s) {
    static PVOID pages[1];
    ULONG_PTR count = 1;
    ULONG granularity = 0;
    if (!s->watched || !s->exposed) return 0;
    return GetWriteWatch(0, s->bits, (size_t)s->pitch * s->height, pages, &count, &granularity) || count != 0;
}

static int park_texture(dd_surface_t* s) {
    d3dr_texture_t* t = (d3dr_texture_t*)s->gpu_texture;
    parked_texture_t k;
    if (!R.dev || !R.hardware || t->generation != R.generation || t->history.dynamic || !s->bits) return 0;
    if (!d3dt_rect_empty(&s->texture_dirty) || written_since_upload(s) || t->bytes > PARK_MAX_BYTES) return 0;
    k = parked_key(s, t->format, t->palette_sum);
    while (s_parked_count == PARK_SLOTS || s_parked_bytes + t->bytes > PARK_MAX_BYTES) unpark_oldest();
    k.texture = t;
    k.parked_at = now_seconds();
    s_parked[s_parked_count++] = k;
    s_parked_bytes += t->bytes;
    R.textures_parked_window++;
    s->gpu_texture = NULL;
    return 1;
}

static d3dr_texture_t* adopt_parked(const dd_surface_t* s, int format, UINT levels, uint32_t palette_sum) {
    parked_texture_t k;
    if (!s_parked_count || !s->bits) return NULL;
    k = parked_key(s, format, palette_sum);
    for (unsigned i = 0; i < s_parked_count; i++) {
        const parked_texture_t* p = &s_parked[i];
        d3dr_texture_t* t = p->texture;
        if (p->hash != k.hash || p->width != k.width || p->height != k.height || p->format != k.format ||
            p->palette_sum != k.palette_sum || p->has_key != k.has_key || p->key_low != k.key_low ||
            p->key_high != k.key_high || t->levels != levels || t->generation != R.generation)
            continue;
        unpark(i, 0);
        R.textures_reused++;
        R.textures_reused_window++;
        return t;
    }
    return NULL;
}

static void release_target_resources(d3dr_target_t* t) {
    SAFE_RELEASE(t->rtv);
    SAFE_RELEASE(t->color);
    SAFE_RELEASE(t->single_srv);
    SAFE_RELEASE(t->single);
    SAFE_RELEASE(t->dsv);
    SAFE_RELEASE(t->depth);
    SAFE_RELEASE(t->packed_rtv);
    SAFE_RELEASE(t->packed);
    SAFE_RELEASE(t->staging);
    SAFE_RELEASE(t->picture_srv);
    SAFE_RELEASE(t->picture);
    t->width = t->height = 0;
}

static ID3D11Texture2D* make_texture(UINT w, UINT h, DXGI_FORMAT format, UINT samples, D3D11_USAGE usage, UINT bind,
                                     UINT cpu, UINT levels, UINT misc) {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D* tex = NULL;
    UINT quality = 0;
    memset(&desc, 0, sizeof desc);
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = levels;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.SampleDesc.Quality = quality;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = cpu;
    desc.MiscFlags = misc;
    if (FAILED(ID3D11Device_CreateTexture2D(R.dev, &desc, NULL, &tex))) return NULL;
    return tex;
}

static UINT supported_samples(void) {
    uint32_t supported = 0;
    for (UINT n = 2; n <= 16; n++) {
        UINT color = 0, depth = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(R.dev, DXGI_FORMAT_B8G8R8A8_UNORM, n, &color)) && color &&
            SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(R.dev, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &depth)) && depth)
            supported |= 1u << n;
    }
    return d3dt_pick_msaa(R.msaa_request, supported);
}

static void choose_size(UINT legacy_w, UINT legacy_h, UINT* w, UINT* h) {
    int view_w = 0, view_h = 0;
    if (R.fixed_w) {
        jpt_size_t box = { (int)R.fixed_w, (int)R.fixed_h };
        jpt_size_t size = jpt_render_target(box, (int)legacy_w, (int)legacy_h);
        *w = (UINT)size.width;
        *h = (UINT)size.height;
        return;
    }
    window_host_view_size(&view_w, &view_h);
    if (view_w <= 0 || view_h <= 0) {
        view_w = (int)legacy_w;
        view_h = (int)legacy_h;
    }
    d3dt_auto_resolution((unsigned)view_w, (unsigned)view_h, legacy_w, legacy_h, w, h);
}

void d3dr_render_box(unsigned* w, unsigned* h) {
    *w = R.fixed_w;
    *h = R.fixed_h;
}

int d3dr_frame_size(int guest_w, int guest_h, int* w, int* h) {
    UINT out_w, out_h;
    if (!R.hardware || guest_w <= 0 || guest_h <= 0) return 0;
    choose_size((UINT)guest_w, (UINT)guest_h, &out_w, &out_h);
    if (!out_w || !out_h) return 0;
    *w = (int)out_w;
    *h = (int)out_h;
    return 1;
}

static int create_target_resources_with(d3dr_target_t* t, UINT samples);

static int create_target_resources(d3dr_target_t* t) {
    UINT samples = supported_samples();
    if (samples < R.msaa_request && !R.msaa_logged) {
        R.msaa_logged = 1;
        host_log("d3d11", "MSAA %ux is not supported by this graphics card for the game's formats: %s instead",
                 R.msaa_request, samples > 1 ? (samples == 4 ? "4x" : "2x") : "off");
    }
    if (create_target_resources_with(t, samples)) return 1;
    if (samples == 1) return 0;
    host_log("d3d11", "the %ux multisampled render target could not be created: the target is created without MSAA",
             samples);
    return create_target_resources_with(t, 1);
}

static int create_target_resources_with(d3dr_target_t* t, UINT samples) {
    float clear_color[4] = { 0, 0, 0, 1 };
    UINT w, h;
    HRESULT hr = S_OK;
    choose_size(t->legacy_w, t->legacy_h, &w, &h);
    t->samples = samples;
    t->color = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, samples, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0), 0, 1, 0);
    t->single = samples == 1 ? t->color : make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                                        D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
    if (samples == 1 && t->single) ID3D11Texture2D_AddRef(t->single);
    t->depth = make_texture(w, h, DXGI_FORMAT_D24_UNORM_S8_UINT, samples, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 1, 0);
    t->packed = make_texture(t->legacy_w, t->legacy_h, DXGI_FORMAT_R16_UINT, 1, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET, 0, 1, 0);
    t->staging = make_texture(t->legacy_w, t->legacy_h, DXGI_FORMAT_R16_UINT, 1, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 1, 0);
    t->picture = make_texture(t->legacy_w, t->legacy_h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
    if (!t->color || !t->single || !t->depth || !t->packed || !t->staging || !t->picture) {
        host_log("d3d11", "render target %ux%u (%ux MSAA) could not be created", w, h, samples);
        release_target_resources(t);
        return 0;
    }
    hr = ID3D11Device_CreateRenderTargetView(R.dev, (ID3D11Resource*)t->color, NULL, &t->rtv);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)t->single, NULL, &t->single_srv);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilView(R.dev, (ID3D11Resource*)t->depth, NULL, &t->dsv);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(R.dev, (ID3D11Resource*)t->packed, NULL, &t->packed_rtv);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)t->picture, NULL, &t->picture_srv);
    if (FAILED(hr)) {
        log_hr("render target views", hr);
        release_target_resources(t);
        return 0;
    }
    ID3D11DeviceContext_ClearRenderTargetView(R.ctx, t->rtv, clear_color);
    ID3D11DeviceContext_ClearDepthStencilView(R.ctx, t->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    t->width = w;
    t->height = h;
    t->generation = R.generation;
    t->gpu_box_known = 0;
    memset(&t->dirty, 0, sizeof t->dirty);
    host_log("d3d11", "3D render target %ux%u B8G8R8A8 (rasterised at this size) for the game's %ux%u screen, %ux MSAA%s", w, h, t->legacy_w,
             t->legacy_h, samples, samples < R.msaa_request ? " (requested level unsupported: reduced)" : "");
    host_log("d3d11", "depth buffer %ux%u D24_UNORM_S8_UINT", w, h);
    return 1;
}

static int target_usable(const d3dr_target_t* t) { return R.dev && t->color && t->generation == R.generation; }

static int target_ready(d3dr_target_t* t) {
    if (!ensure_device()) return 0;
    if (target_usable(t)) return 1;
    if (t->color) tsync_gpu_gone(&t->sync);
    release_target_resources(t);
    return create_target_resources(t);
}

HRESULT d3dr_attach(d3d_device_t* dev) {
    dd_surface_t* s = dev->target;
    d3dr_target_t* t;
    if (!R.hardware || !ensure_device()) return DDERR_GENERIC;
    if (s->target) {
        host_log("d3d", "CreateDevice: surface 0x%08X already has a Direct3D device", (uint32_t)(uintptr_t)s);
        return DDERR_INVALIDPARAMS;
    }
    if (s->format.dwRGBBitCount != 16 || s->format.dwRBitMask != 0xF800 || s->format.dwGBitMask != 0x07E0) {
        host_log("d3d", "CreateDevice: render target format %lu bpp %lX/%lX/%lX is not RGB565", s->format.dwRGBBitCount,
                 s->format.dwRBitMask, s->format.dwGBitMask, s->format.dwBBitMask);
        return DDERR_INVALIDPIXELFORMAT;
    }
    t = (d3dr_target_t*)calloc(1, sizeof *t);
    if (!t) return DDERR_OUTOFMEMORY;
    t->device = dev;
    t->surface = s;
    t->legacy_w = s->width;
    t->legacy_h = s->height;
    t->reference = (uint16_t*)malloc((size_t)s->width * s->height * sizeof(uint16_t));
    t->scratch = (uint32_t*)malloc((size_t)s->width * s->height * sizeof(uint32_t));
    if (!t->reference || !t->scratch || !create_target_resources(t)) {
        free(t->reference);
        free(t->scratch);
        free(t);
        return DDERR_OUTOFMEMORY;
    }
    tsync_attach(&t->sync);
    s->target = t;
    dev->backend = t;
    R.devices++;
    host_log("d3d", "Direct3D device #%u: %lux%lu target at %ux%u, %ux MSAA, %u textures alive, texture filter %s%s, "
             "mipmaps %s", R.devices, s->width, s->height, t->width, t->height, t->samples, R.textures_alive,
             k_filter_names[R.filter], anisotropy_text(),
             d3dt_filter_uses_mips(R.filter) ? "generated for the 3D world's textures" : "none (as the game's textures)");
    return DD_OK;
}

/* The surface keeps the last picture when its device goes, as DirectDraw memory would. */
void d3dr_detach(d3d_device_t* dev) {
    d3dr_target_t* t = dev->backend;
    settle_deferred();
    if (R.batch_dev == dev) flush();
    if (R.batch_dev == dev) R.batch_dev = NULL;
    if (!t) return;
    if (t->surface && target_usable(t)) {
        int exact = t->sync.exact;
        check_front(t);
        apply_sync(t, tsync_gpu_release(&t->sync), NULL, READBACK_OTHER, exact);
    }
    end_polygon_run(t);
    release_target_resources(t);
    if (t->surface) t->surface->target = NULL;
    free(t->reference);
    free(t->scratch);
    free(t);
    dev->backend = NULL;
}

void d3dr_surface_freed(dd_surface_t* s) {
    if (s->gpu_texture && !park_texture(s)) release_texture(s);
    if (s->target) {
        drop_deferred(s->target);
        end_polygon_run(s->target);
        s->target->surface = NULL;
    }
}

void d3dr_device_lost(void) {
    R.nverts = R.nindices = 0;
    R.batch_arrays = 0;
    R.bound.valid = 0;
    flush_parked();
    drop_deferred(NULL);
    end_polygon_run(NULL);
    R.batch_dev = NULL;
    release_static_objects();
    SAFE_RELEASE(R.ctx);
    SAFE_RELEASE(R.dev);
}

/* ---- passes ---- */

static void set_pass(const float rect[4], const float uv[4], const float color[4], float depth) {
    D3D11_MAPPED_SUBRESOURCE m;
    pass_constants_t c;
    memset(&c, 0, sizeof c);
    memcpy(c.rect, rect, sizeof c.rect);
    if (uv) memcpy(c.uv, uv, sizeof c.uv);
    if (color) memcpy(c.color, color, sizeof c.color);
    c.depth = depth;
    if (SUCCEEDED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)R.cb_pass, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)R.cb_pass, 0);
    }
}

/* Draws a rectangle (NDC) with a quad pass on the bound render target. */
static void draw_pass(ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, ID3D11SamplerState* sampler,
                      ID3D11BlendState* blend, ID3D11DepthStencilState* depth, UINT width, UINT height, const D3D11_RECT* scissor) {
    D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    D3D11_RECT full = { 0, 0, (LONG)width, (LONG)height };
    ID3D11ShaderResourceView* views[2] = { srv, R.gamma_srv };
    float factor[4] = { 0, 0, 0, 0 };
    R.bound.valid = 0;
    ID3D11DeviceContext_IASetInputLayout(R.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(R.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(R.ctx, R.vs_quad, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(R.ctx, 2, 1, &R.cb_pass);
    ID3D11DeviceContext_PSSetShader(R.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(R.ctx, 2, 1, &R.cb_pass);
    ID3D11DeviceContext_PSSetShaderResources(R.ctx, 0, 2, views);
    if (sampler) ID3D11DeviceContext_PSSetSamplers(R.ctx, 0, 1, &sampler);
    ID3D11DeviceContext_RSSetState(R.ctx, R.raster_pass);
    ID3D11DeviceContext_RSSetViewports(R.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(R.ctx, 1, scissor ? scissor : &full);
    ID3D11DeviceContext_OMSetBlendState(R.ctx, blend, factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(R.ctx, depth, 0);
    ID3D11DeviceContext_Draw(R.ctx, 4, 0);
}

static void unbind_views(void) {
    ID3D11ShaderResourceView* none[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(R.ctx, 0, 2, none);
}

static const float k_full_rect[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
static const float k_full_uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

static void upload_picture(ID3D11Texture2D* tex, const uint32_t* pixels, UINT w, UINT h) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (UINT y = 0; y < h; y++) memcpy((BYTE*)m.pData + (size_t)y * m.RowPitch, pixels + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)tex, 0);
}

static ID3D11ShaderResourceView* resolved_view(d3dr_target_t* t) {
    if (t->samples > 1)
        ID3D11DeviceContext_ResolveSubresource(R.ctx, (ID3D11Resource*)t->single, 0, (ID3D11Resource*)t->color, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
    return t->single_srv;
}

/* The game function behind a raster Lock: the newest virtual call into a Lock wrapper names it. */
static uint32_t readback_caller(void) {
    static const uint32_t wrappers[] = { RASTER_LOCK, RASTER_WIN_LOCK, RASTER_WIN_LOCK_THUNK };
    uint32_t caller = g_cur_func, found;
    if (caller != RASTER_LOCK && caller != RASTER_WIN_LOCK && caller != RASTER_WIN_LOCK_THUNK) return caller;
    found = icall_ring_caller(g_icall_trace, g_icall_from, g_icall_trace_idx, ICALL_TRACE_SIZE, wrappers, 3);
    return found ? found : caller;
}

static readback_caller_t* caller_slot(readback_caller_t* table, uint32_t caller, readback_reason_t reason) {
    readback_caller_t* free_slot = NULL;
    for (int i = 0; i < MAX_READBACK_CALLERS; i++) {
        if (table[i].count && table[i].caller == caller && table[i].reason == reason) return &table[i];
        if (!table[i].count && !free_slot) free_slot = &table[i];
    }
    if (free_slot) {
        free_slot->caller = caller;
        free_slot->reason = reason;
    }
    return free_slot;
}

static void charge_caller(readback_caller_t* table, uint32_t caller, readback_reason_t reason, double ms, size_t bytes) {
    readback_caller_t* slot = caller_slot(table, caller, reason);
    if (!slot) return;
    slot->count++;
    slot->bytes += bytes;
    slot->ms += ms;
}

static void note_readback(readback_reason_t reason, double ms, size_t bytes) {
    R.readbacks++;
    R.perf.readback_n[reason]++;
    R.perf.readback_ms[reason] += ms;
    R.perf.readback_bytes += bytes;
    R.perf.ms[PERF_READBACK] += ms;
    R.perf.count[PERF_READBACK]++;
    charge_caller(R.perf.callers, R.charge_caller ? R.charge_caller : readback_caller(), reason, ms, bytes);
}

/* The GPU picture at the legacy size, packed to RGB565 on the GPU. Point sampling returns exactly the
 * pixels the CPU composited; the pixel calibration keeps the filtered edges it measures. Mapping waits
 * for the GPU, which is what makes reading the target expensive. */
static int map_target(d3dr_target_t* t, int filtered, const d3dt_rect_t* box, D3D11_MAPPED_SUBRESOURCE* m) {
    ID3D11ShaderResourceView* srv = resolved_view(t);
    D3D11_BOX region = { (UINT)box->left, (UINT)box->top, 0, (UINT)box->right, (UINT)box->bottom, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->packed_rtv, NULL);
    R.bound.valid = 0;
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PACK565], srv, filtered ? R.linear_clamp : R.point_clamp, R.blend_none, NULL,
              t->legacy_w, t->legacy_h, NULL);
    unbind_views();
    ID3D11DeviceContext_CopySubresourceRegion(R.ctx, (ID3D11Resource*)t->staging, 0, region.left, region.top, 0,
                                              (ID3D11Resource*)t->packed, 0, &region);
    return SUCCEEDED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)t->staging, 0, D3D11_MAP_READ, 0, m));
}

static d3dt_rect_t whole_target(const d3dr_target_t* t) {
    d3dt_rect_t r = { 0, 0, (long)t->legacy_w, (long)t->legacy_h };
    return r;
}

/* The CPU copy and its reference over r := value. */
static void fill_copy(d3dr_target_t* t, const d3dt_rect_t* r, uint16_t value) {
    tsync_fill(t->surface->bits, t->surface->pitch, t->reference, t->legacy_w, r, value);
    R.perf.filled_pixels += (unsigned long long)(r->right - r->left) * (unsigned long long)(r->bottom - r->top);
}

static void capture_tile(void* context, const d3dt_rect_t* r) { fill_copy((d3dr_target_t*)context, r, TSYNC_UNWRITTEN); }

static void mark_fresh(void* context, const d3dt_rect_t* r) {
    (void)context;
    (void)r;
}

/* Tiles of r not captured since the last Flip become captured. */
static void claim_fresh(d3dr_target_t* t, const d3dt_rect_t* r) {
    if (t->lazy) tsync_tiles_claim(&t->fresh, r, capture_tile, t);
}

static void start_lazy_capture(d3dr_target_t* t) {
    d3dt_rect_t all = { 0, 0, (long)t->legacy_w, (long)t->legacy_h };
    t->lazy = tsync_tiles_reset(&t->fresh, t->legacy_w, t->legacy_h);
    if (!t->lazy) fill_copy(t, &all, TSYNC_UNWRITTEN);
}

static void readback(d3dr_target_t* t, readback_reason_t reason) {
    D3D11_MAPPED_SUBRESOURCE m;
    dd_surface_t* s = t->surface;
    int64_t start = ticks();
    int filtered = reason == READBACK_PIXEL_CALIBRATION && (t->width != t->legacy_w || t->height != t->legacy_h);
    d3dt_rect_t all = whole_target(t);
    d3dt_rect_t box = d3dt_readback_box(t->gpu_box_known, &t->gpu_box, t->legacy_w, t->legacy_h);
    size_t row_bytes = (size_t)(box.right - box.left) * 2;
    if (!target_usable(t)) return;
    if (d3dt_rect_empty(&box)) {
        fill_copy(t, &all, t->gpu_clear);
        t->lazy = 0;
        R.perf.synthesized++;
        return;
    }
    if (!map_target(t, filtered, &box, &m)) return;
    t->lazy = 0;
    if (box.left || box.top || box.right != all.right || box.bottom != all.bottom)
        fill_copy(t, &all, t->gpu_clear);
    for (long y = box.top; y < box.bottom; y++) {
        const uint16_t* in = (const uint16_t*)((const BYTE*)m.pData + (size_t)y * m.RowPitch) + box.left;
        memcpy(s->bits + (size_t)y * s->pitch + (size_t)box.left * 2, in, row_bytes);
        memcpy(t->reference + (size_t)y * t->legacy_w + box.left, in, row_bytes);
    }
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)t->staging, 0);
    note_readback(reason, elapsed_ms(start), row_bytes * (size_t)(box.bottom - box.top));
}

static int front_mirrors_target(const d3dr_target_t* t) {
    const dd_surface_t* s = t->surface;
    const dd_surface_t* front = s ? s->root : NULL;
    return front && front->bits && front->width == s->width && front->height == s->height &&
           front->format.dwRGBBitCount == 16;
}

/* After a frame made from the CPU copy, the front buffer holds what the GPU target shows. */
static void copy_front(d3dr_target_t* t) {
    dd_surface_t* s = t->surface;
    const dd_surface_t* front = s->root;
    t->lazy = 0;
    for (UINT y = 0; y < t->legacy_h; y++) {
        const BYTE* in = front->bits + (size_t)y * front->pitch;
        memcpy(s->bits + (size_t)y * s->pitch, in, (size_t)t->legacy_w * 2);
        memcpy(t->reference + (size_t)y * t->legacy_w, in, (size_t)t->legacy_w * 2);
    }
    R.perf.front_copies++;
}

static void draw_picture_box(d3dr_target_t* t, const d3dt_rect_t* box, int opaque) {
    float w = (float)t->legacy_w, h = (float)t->legacy_h;
    float rect[4] = { box->left / w * 2.0f - 1.0f, 1.0f - box->top / h * 2.0f, box->right / w * 2.0f - 1.0f,
                      1.0f - box->bottom / h * 2.0f };
    float uv[4] = { box->left / w, box->top / h, box->right / w, box->bottom / h };
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->rtv, NULL);
    R.bound.valid = 0;
    set_pass(rect, uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_COPY], t->picture_srv, opaque ? R.linear_clamp : R.point_clamp,
              opaque ? R.blend_none : R.blend_overlay, NULL, t->width, t->height, NULL);
    unbind_views();
}

static void gpu_box_add(d3dr_target_t* t, const d3dt_rect_t* r) {
    if (t->gpu_box_known) d3dt_rect_union(&t->gpu_box, r);
}

static void gpu_box_cleared(d3dr_target_t* t, uint16_t color) {
    t->gpu_box_known = 1;
    t->gpu_clear = color;
    memset(&t->gpu_box, 0, sizeof t->gpu_box);
}

/* Where the CPU may have written since the last sync, inside the surface. */
static d3dt_rect_t dirty_region(const d3dr_target_t* t) {
    d3dt_rect_t r = t->dirty;
    if (d3dt_rect_empty(&r)) return whole_target(t);
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (long)t->legacy_w) r.right = (long)t->legacy_w;
    if (r.bottom > (long)t->legacy_h) r.bottom = (long)t->legacy_h;
    return r;
}

static int gameplay_now(void) {
    guest_ui_view_t v;
    guest_ui_probe(&v);
    return v.ui == GUEST_UI_GAMEPLAY;
}

/* Pixels the CPU composited, by screen kind. Black ones count as over the GPU picture only when the CPU copy held it
 * before the writes. */
static void note_cpu_pixels(const tsync_counts_t* c, int exact) {
    int gameplay = gameplay_now();
    R.cpu_pixels[gameplay] += c->changed;
    if (exact) R.black_over_gpu[gameplay] += c->black;
    else R.black_over_unknown[gameplay] += c->black;
}

/* CPU -> GPU: only pixels that differ from the reference, uploaded as the box that holds them. */
static void composite(d3dr_target_t* t, int exact) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t region = dirty_region(t), box;
    tsync_counts_t counts = { 0, 0 };
    int64_t start = ticks();
    memset(&t->dirty, 0, sizeof t->dirty);
    if (!target_usable(t)) return;
    if (t->lazy) region = tsync_tiles_clip(&t->fresh, &region);
    R.perf.scanned_pixels +=
        (unsigned long long)(region.right - region.left) * (unsigned long long)(region.bottom - region.top);
    if (tsync_changed_box_in(s->bits, s->pitch, t->reference, t->legacy_w, &region, t->lazy ? &t->fresh : NULL,
                             &box)) {
        /* The unchanged texels around the box upload transparent: an edge sample that rounds outside the box at a
         * non-integer scale would otherwise show an old picture as a line. */
        d3dt_rect_t apron = d3dt_overlay_upload_rect(&box, t->legacy_w, t->legacy_h);
        UINT bw = (UINT)(apron.right - apron.left);
        D3D11_BOX upload = { (UINT)apron.left, (UINT)apron.top, 0, (UINT)apron.right, (UINT)apron.bottom, 1 };
        tsync_overlay_box_in(s->bits, s->pitch, t->reference, t->legacy_w, &apron, t->lazy ? &t->fresh : NULL,
                             t->scratch, &counts);
        ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)t->picture, 0, &upload, t->scratch, bw * 4, 0);
        draw_picture_box(t, &box, 0);
        gpu_box_add(t, &apron);
        R.overlays++;
        R.perf.composites++;
        R.perf.composited_pixels += counts.changed;
        R.perf.composited_black += counts.black;
        note_cpu_pixels(&counts, exact);
    }
    perf_add(PERF_OVERLAY, start);
}

/* The CPU copy is the whole picture: a device on a surface the game drew, or a recreated target. */
static void upload_all(d3dr_target_t* t) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t all = whole_target(t);
    int64_t start = ticks();
    if (!target_usable(t)) return;
    for (UINT y = 0; y < t->legacy_h; y++) {
        const uint16_t* row = (const uint16_t*)(s->bits + (size_t)y * s->pitch);
        uint32_t* out = t->scratch + (size_t)y * t->legacy_w;
        for (UINT x = 0; x < t->legacy_w; x++) out[x] = R.rgb565_table[row[x]];
        memcpy(t->reference + (size_t)y * t->legacy_w, row, (size_t)t->legacy_w * 2);
    }
    t->lazy = 0;
    ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)t->picture, 0, NULL, t->scratch, t->legacy_w * 4, 0);
    draw_picture_box(t, &all, 1);
    gpu_box_add(t, &all);
    R.overlays++;
    R.perf.uploads++;
    perf_add(PERF_OVERLAY, start);
}

/* exact: whether the reference held the GPU picture when the pending writes were made. */
static void apply_sync(d3dr_target_t* t, unsigned acts, const d3dt_rect_t* region, readback_reason_t reason, int exact) {
    d3dt_rect_t all = whole_target(t), r = all;
    dd_surface_t* s = t->surface;
    if (!acts || !s) return;
    if (acts & TSYNC_COMPOSITE) composite(t, exact);
    if (acts & TSYNC_UPLOAD) upload_all(t);
    if (acts & TSYNC_READBACK) readback(t, reason);
    if (acts & TSYNC_COPY_FRONT) copy_front(t);
    if (acts & TSYNC_SYNTHESIZE) {
        fill_copy(t, &all, t->sync.color);
        t->lazy = 0;
        R.perf.synthesized++;
    }
    if (acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL)) {
        if (R.capture_text && tsync_tiles_reset(&t->fresh, t->legacy_w, t->legacy_h)) {
            /* Only TextOutA writes under the text overlay's DC: each string's box is captured before it is drawn. */
            t->lazy = 1;
            R.text_target = t;
        } else {
            if (!(acts & TSYNC_CAPTURE_ALL) && region) {
                r = *region;
                if (r.left < 0) r.left = 0;
                if (r.top < 0) r.top = 0;
                if (r.right > all.right) r.right = all.right;
                if (r.bottom > all.bottom) r.bottom = all.bottom;
            }
            if (acts & TSYNC_CAPTURE_ALL) t->lazy = 0;
            claim_fresh(t, &r);
            fill_copy(t, &r, TSYNC_UNWRITTEN);
        }
        R.perf.no_read_accesses++;
    }
}

/* The front buffer is checked before deciding, so a mirror the game has since changed is not used. */
static void check_front(d3dr_target_t* t) {
    if (t->sync.front_mirror && !front_mirrors_target(t)) tsync_front_changed(&t->sync);
}

/* Before the GPU target is drawn into (not a full clear). */
static void gpu_write(d3dr_target_t* t) {
    int exact;
    unsigned acts;
    settle_deferred();
    exact = t->sync.exact;
    acts = tsync_gpu_draw(&t->sync);
    if (acts && R.nindices && R.batch_dev == t->device) flush();
    apply_sync(t, acts, NULL, READBACK_OTHER, exact);
}

/* ---- state objects (cached; created on first use) ---- */

static ID3D11BlendState* blend_for(const d3d_device_t* dev) {
    DWORD src = dev->render[D3DRENDERSTATE_SRCBLEND], dst = dev->render[D3DRENDERSTATE_DESTBLEND];
    int s11, d11, sa, da;
    ID3D11BlendState** slot;
    if (!dev->render[D3DRENDERSTATE_ALPHABLENDENABLE] || src > 13 || dst > 13) return R.blend_none;
    slot = &R.blend[1][src][dst];
    if (!*slot && d3dt_blend_pair(src, dst, &s11, &d11, &sa, &da))
        *slot = make_blend(1, s11, d11, sa, da,
                           D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE);
    return *slot ? *slot : R.blend_none;
}

static ID3D11DepthStencilState* depth_for(const d3d_device_t* dev, int has_depth) {
    int enable = has_depth && dev->render[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE;
    int write = dev->render[D3DRENDERSTATE_ZWRITEENABLE] != 0;
    int func = d3dt_compare(dev->render[D3DRENDERSTATE_ZFUNC]);
    ID3D11DepthStencilState** slot = &R.depth[enable][write][func];
    if (!*slot) {
        D3D11_DEPTH_STENCIL_DESC d;
        memset(&d, 0, sizeof d);
        d.DepthEnable = enable;
        d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = func ? (D3D11_COMPARISON_FUNC)func : D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(R.dev, &d, slot);
    }
    return *slot;
}

static ID3D11RasterizerState* raster_for(const d3d_device_t* dev, int multisample) {
    int cull = d3dt_cull(dev->render[D3DRENDERSTATE_CULLMODE]);
    int fill = d3dt_fill(dev->render[D3DRENDERSTATE_FILLMODE]);
    DWORD zbias = dev->render[D3DRENDERSTATE_ZBIAS] < MAX_BIAS_STEPS ? dev->render[D3DRENDERSTATE_ZBIAS] : MAX_BIAS_STEPS - 1;
    int towards_greater = dev->render[D3DRENDERSTATE_ZFUNC] == D3DCMP_GREATER || dev->render[D3DRENDERSTATE_ZFUNC] == D3DCMP_GREATEREQUAL;
    ID3D11RasterizerState** slot;
    if (!cull) cull = D3D11_CULL_NONE;
    if (!fill) fill = D3D11_FILL_SOLID;
    slot = &R.raster[cull][fill][multisample != 0][zbias];
    if (*slot && zbias) {
        D3D11_RASTERIZER_DESC current;
        ID3D11RasterizerState_GetDesc(*slot, &current);
        if ((current.DepthBias > 0) != towards_greater) SAFE_RELEASE(*slot);
    }
    if (!*slot) {
        D3D11_RASTERIZER_DESC d;
        memset(&d, 0, sizeof d);
        d.FillMode = (D3D11_FILL_MODE)fill;
        d.CullMode = (D3D11_CULL_MODE)cull;
        d.DepthBias = d3dt_depth_bias(zbias, dev->render[D3DRENDERSTATE_ZFUNC]);
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        d.MultisampleEnable = multisample != 0;
        ID3D11Device_CreateRasterizerState(R.dev, &d, slot);
    }
    return *slot;
}

static ID3D11SamplerState* make_texture_sampler(const d3dt_sampler_choice_t* c, int u, int v) {
    UINT anisotropy = c->anisotropy;
    ID3D11SamplerState* state = make_sampler((D3D11_FILTER)c->filter, u, v, c->mips ? D3D11_FLOAT32_MAX : 0.0f, anisotropy);
    while (!state && anisotropy > 1) {
        anisotropy /= 2;
        state = make_sampler(anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR, u, v,
                             D3D11_FLOAT32_MAX, anisotropy > 1 ? anisotropy : 1);
    }
    if (anisotropy != c->anisotropy && !R.anisotropy_logged) {
        R.anisotropy_logged = 1;
        host_log("d3d11", "anisotropic filtering %ux is not available: %ux instead", c->anisotropy, anisotropy);
    }
    return state;
}

static ID3D11SamplerState* sampler_with(const d3d_device_t* dev, DWORD address_u, DWORD address_v) {
    int game_mag = d3dt_mag_linear(dev->stage[0][D3DTSS_MAGFILTER]) == 1;
    int game_min = d3dt_min_linear(dev->stage[0][D3DTSS_MINFILTER]) == 1;
    int u = d3dt_address(address_u), v = d3dt_address(address_v);
    ID3D11SamplerState** slot;
    if (!u) u = D3D11_TEXTURE_ADDRESS_WRAP;
    if (!v) v = D3D11_TEXTURE_ADDRESS_WRAP;
    slot = &R.sampler[game_mag][game_min][u][v];
    if (!*slot) {
        d3dt_sampler_choice_t c = d3dt_choose_sampler(R.filter, game_mag, game_min, R.anisotropy);
        *slot = make_texture_sampler(&c, u, v);
    }
    if (*slot && !R.sampler_logged[game_mag][game_min][u][v]) {
        D3D11_SAMPLER_DESC d;
        R.sampler_logged[game_mag][game_min][u][v] = 1;
        ID3D11SamplerState_GetDesc(*slot, &d);
        host_log("d3d11", "sampler bound for the game's MAG %lu MIN %lu MIP %lu, address %lu/%lu: D3D11 filter 0x%X, "
                 "MaxAnisotropy %u, %s, address %d/%d", dev->stage[0][D3DTSS_MAGFILTER], dev->stage[0][D3DTSS_MINFILTER],
                 dev->stage[0][D3DTSS_MIPFILTER], dev->stage[0][D3DTSS_ADDRESSU], dev->stage[0][D3DTSS_ADDRESSV],
                 (unsigned)d.Filter, d.MaxAnisotropy, d.MaxLOD > 0.0f ? "generated mipmaps" : "level 0 only",
                 (int)d.AddressU, (int)d.AddressV);
    }
    return *slot;
}

/* s0-s3: wrap or clamp per axis (d3dt_vertex_sampler), s4: the device's own address modes. */
static int samplers_for(const d3d_device_t* dev, ID3D11SamplerState** out) {
    static const DWORD k_u[4] = { 1, 3, 1, 3 }, k_v[4] = { 1, 1, 3, 3 };
    for (int i = 0; i < D3D11_DEVICE_SAMPLER; i++)
        if (!(out[i] = sampler_with(dev, k_u[i], k_v[i]))) return 0;
    out[D3D11_DEVICE_SAMPLER] = sampler_with(dev, dev->stage[0][D3DTSS_ADDRESSU], dev->stage[0][D3DTSS_ADDRESSV]);
    return out[D3D11_DEVICE_SAMPLER] != NULL;
}

/* ---- textures ---- */

static int pixelformat_texture_format(const DDPIXELFORMAT* pf) {
    return d3dt_format_from_pixelformat(pf->dwFlags, pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask,
                                        pf->dwRGBAlphaBitMask);
}

int d3dr_texture_format_known(const DDPIXELFORMAT* pf) { return pixelformat_texture_format(pf) != D3DT_FMT_UNKNOWN; }

/* Once per distinct layout: what the game asked for, so the format can be added. */
static void log_unknown_format(const dd_surface_t* s) {
    const DDPIXELFORMAT* pf = &s->format;
    unsigned i;
    for (i = 0; i < R.unknown_count; i++)
        if (R.unknown[i].flags == pf->dwFlags && R.unknown[i].bits == pf->dwRGBBitCount && R.unknown[i].r == pf->dwRBitMask &&
            R.unknown[i].g == pf->dwGBitMask && R.unknown[i].b == pf->dwBBitMask && R.unknown[i].a == pf->dwRGBAlphaBitMask)
            return;
    if (R.unknown_count == MAX_UNKNOWN_FORMATS) return;
    R.unknown[i].flags = pf->dwFlags;
    R.unknown[i].bits = pf->dwRGBBitCount;
    R.unknown[i].r = pf->dwRBitMask;
    R.unknown[i].g = pf->dwGBitMask;
    R.unknown[i].b = pf->dwBBitMask;
    R.unknown[i].a = pf->dwRGBAlphaBitMask;
    R.unknown[i].caps = s->caps;
    R.unknown[i].width = s->width;
    R.unknown[i].height = s->height;
    R.unknown[i].caller = g_cur_func;
    R.unknown_count++;
    host_log("d3d", "unknown texture format: flags 0x%lX, %lu bits, R %08lX G %08lX B %08lX A %08lX, caps 0x%08lX, %lux%lu, "
             "palette %s, source key %s, drawn from sub_%08X: drawn untextured",
             pf->dwFlags, pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask, pf->dwRGBAlphaBitMask, s->caps,
             s->width, s->height, s->palette ? "set" : "none", s->has_src_key ? "set" : "none", g_cur_func);
}

static uint32_t palette_bgra(const dd_surface_t* s, uint32_t* out) {
    uint32_t sum = 2166136261u;
    for (int i = 0; i < 256; i++) {
        const PALETTEENTRY* e = s->palette ? &s->palette->entries[i] : NULL;
        out[i] = e ? 0xFF000000u | ((uint32_t)e->peRed << 16) | ((uint32_t)e->peGreen << 8) | e->peBlue : 0xFF000000u;
        sum = (sum ^ out[i]) * 16777619u;
    }
    return sum;
}

static uint32_t* upload_rows(size_t texels) {
    if (texels > R.upload_capacity) {
        uint32_t* grown = (uint32_t*)realloc(R.upload_buffer, texels * 4);
        if (!grown) return NULL;
        R.upload_buffer = grown;
        R.upload_capacity = texels;
    }
    return R.upload_buffer;
}

static uint32_t* mip_chain(size_t texels) {
    if (texels > R.mip_capacity) {
        uint32_t* grown = (uint32_t*)realloc(R.mip_chain, texels * 4);
        if (!grown) return NULL;
        R.mip_chain = grown;
        R.mip_capacity = texels;
    }
    return R.mip_chain;
}

static void convert_rows(const dd_surface_t* s, int format, const uint32_t* palette, const D3D11_BOX* box, uint32_t* out) {
    UINT w = box->right - box->left;
    int bytes = d3dt_format_bytes(format);
    for (UINT y = box->top; y < box->bottom; y++) {
        const BYTE* in = s->bits + (size_t)y * s->pitch + (size_t)box->left * bytes;
        uint32_t* row = out + (size_t)(y - box->top) * w;
        if (format == D3DT_FMT_PAL8)
            d3dt_convert_row_pal8(in, row, w, palette, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                                  s->src_key.dwColorSpaceHighValue);
        else
            d3dt_convert_row(format, in, row, w, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                             s->src_key.dwColorSpaceHighValue);
    }
}

/* Level 0 only from now on: the texture changes too often for its generated levels to be worth building. It moves to a
 * one-level array of its size, which other textures of that size share in a batch. */
static void make_dynamic(d3dr_texture_t* t) {
    ID3D11ShaderResourceView* top;
    d3dr_texture_t moved;
    memset(&moved, 0, sizeof moved);
    if (pool_take(&moved, t->width, t->height, 1)) {
        settle_batch_texture(t);
        ID3D11DeviceContext_CopySubresourceRegion(R.ctx, texture_resource(&moved), texture_subresource(&moved, 0), 0, 0,
                                                  0, texture_resource(t), texture_subresource(t, 0), NULL);
        if (t->pool) {
            pool_give_back(t);
        } else {
            SAFE_RELEASE(t->srv);
            SAFE_RELEASE(t->tex);
        }
        t->pool = moved.pool;
        t->slice = moved.slice;
        t->levels = 1;
        R.textures_dynamic++;
        return;
    }
    if (t->pool) {
        ID3D11Texture2D* own = make_texture(t->width, t->height, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
        if (!own || !(top = array_view(own, 1, 1))) {
            SAFE_RELEASE(own);
            return;
        }
        settle_batch_texture(t);
        ID3D11DeviceContext_CopySubresourceRegion(R.ctx, (ID3D11Resource*)own, 0, 0, 0, 0, texture_resource(t),
                                                  texture_subresource(t, 0), NULL);
        pool_give_back(t);
        t->tex = own;
        t->levels = 1;
    } else if (!(top = array_view(t->tex, 1, 1))) {
        return;
    }
    SAFE_RELEASE(t->srv);
    t->srv = top;
    R.textures_dynamic++;
}

/* The whole texture with every generated level: a changed row changes each level above it. */
static void upload_with_mips(dd_surface_t* s, d3dr_texture_t* t, int format, const uint32_t* palette) {
    D3D11_BOX all = { 0, 0, 0, t->width, t->height, 1 };
    uint32_t* chain = mip_chain(d3dt_mip_chain_texels(t->width, t->height, t->levels));
    uint32_t* level;
    int64_t start = ticks();
    if (!chain) return;
    convert_rows(s, format, palette, &all, chain);
    t->alpha_kind = d3dt_alpha_kind(chain, (size_t)t->width * t->height);
    d3dt_build_mips(chain, t->width, t->height, t->levels, t->alpha_kind);
    level = chain;
    for (UINT i = 0; i < t->levels; i++) {
        unsigned lw, lh;
        d3dt_mip_size(t->width, t->height, i, &lw, &lh);
        ID3D11DeviceContext_UpdateSubresource(R.ctx, texture_resource(t), texture_subresource(t, i), NULL, level, lw * 4, 0);
        level += (size_t)lw * lh;
    }
    R.mip_texels += d3dt_mip_chain_texels(t->width, t->height, t->levels) - (size_t)t->width * t->height;
    R.mip_ms += elapsed_ms(start);
}

/* Texture memory is write-watched: pages the game changed without a Lock/Unlock, Blt or Load that
 * announced it (a kept pointer, another thread) are found here and uploaded too. */
static void collect_written_rows(dd_surface_t* s, d3dr_texture_t* t) {
    static PVOID pages[WATCH_PAGES];
    ULONG_PTR count = WATCH_PAGES;
    ULONG granularity = 0;
    size_t bytes = (size_t)s->pitch * s->height;
    d3dt_rect_t rows = { 0, 0, (long)s->width, 0 };
    int announced = !d3dt_rect_empty(&s->texture_dirty);
    R.perf.watch_reads++;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, s->bits, bytes, pages, &count, &granularity) || !count) return;
    rows.top = (long)s->height;
    if (count == WATCH_PAGES) {
        rows.top = 0;
        rows.bottom = (long)s->height;
    }
    for (ULONG_PTR i = 0; i < count && count < WATCH_PAGES; i++) {
        size_t off = (size_t)((BYTE*)pages[i] - s->bits);
        long first = (long)(off / (size_t)s->pitch), end = (long)((off + granularity + (size_t)s->pitch - 1) / (size_t)s->pitch);
        if (first < rows.top) rows.top = first;
        if (end > rows.bottom) rows.bottom = end;
    }
    if (rows.bottom > (long)s->height) rows.bottom = (long)s->height;
    d3dt_rect_union(&s->texture_dirty, &rows);
    if (announced || !t) return;
    t->untracked++;
    R.untracked_writes++;
    if (R.untracked_logged >= UNTRACKED_LOG_LIMIT) return;
    R.untracked_logged++;
    host_log("d3d", "texture %lux%lu %s changed without Lock/Unlock: rows %ld-%ld uploaded again (%lu CPU writes "
             "announced, seen from sub_%08X)", s->width, s->height, d3dt_format_name(t->format), rows.top, rows.bottom,
             s->cpu_writes, g_cur_func);
}

/* Unannounced writes need a pointer the guest got from a Lock: such a surface's watch is read once a frame, and with
 * each upload so the rows it announced are not uploaded again. */
static int watch_due(dd_surface_t* s) {
    if (!s->watched || !s->exposed) return 0;
    if (s->watch_frame == R.frames_hw + 1 && d3dt_rect_empty(&s->texture_dirty)) return 0;
    s->watch_frame = R.frames_hw + 1;
    return 1;
}

enum { UPLOAD_FIRST, UPLOAD_WITH_MIPS, UPLOAD_REGION };

static void count_upload(int kind, size_t texels, int64_t start) {
    R.uploads_by[kind].n++;
    R.uploads_by[kind].texels += texels;
    R.uploads_by[kind].ms += elapsed_ms(start);
}

static d3dr_texture_t* texture_for(dd_surface_t* s) {
    d3dr_texture_t* t = (d3dr_texture_t*)s->gpu_texture;
    int format = pixelformat_texture_format(&s->format);
    d3dt_rect_t full = { 0, 0, (long)s->width, (long)s->height };
    D3D11_BOX box;
    uint32_t* rows;
    uint32_t palette[256], palette_sum = 0;
    int64_t upload_start;
    if (!format) {
        log_unknown_format(s);
        return NULL;
    }
    if (format == D3DT_FMT_PAL8) palette_sum = palette_bgra(s, palette);
    if (t && t->generation != R.generation) {
        release_texture(s);
        t = NULL;
    }
    if (!t) {
        UINT levels = d3dt_filter_uses_mips(R.filter) ? d3dt_mip_count(s->width, s->height) : 1;
        if ((t = adopt_parked(s, format, levels, palette_sum)) != NULL) {
            s->gpu_texture = t;
            if (s->watched && s->exposed) collect_written_rows(s, NULL);
            memset(&s->texture_dirty, 0, sizeof s->texture_dirty);
            return t;
        }
        t = (d3dr_texture_t*)calloc(1, sizeof *t);
        if (!t) return NULL;
        if (!pool_take(t, s->width, s->height, levels)) {
            t->tex = make_texture(s->width, s->height, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, levels, 0);
            if (!t->tex || !(t->srv = array_view(t->tex, levels, 1))) {
                SAFE_RELEASE(t->tex);
                free(t);
                return NULL;
            }
        }
        t->width = s->width;
        t->height = s->height;
        t->levels = levels;
        t->format = format;
        t->generation = R.generation;
        t->bytes = d3dt_mip_chain_texels(s->width, s->height, levels) * 4;
        t->palette_sum = palette_sum;
        R.texture_memory += t->bytes;
        R.textures_alive++;
        R.textures_created_window++;
        if (R.textures_alive > R.textures_peak) R.textures_peak = R.textures_alive;
        if (levels > 1) R.textures_mipped++;
        s->gpu_texture = t;
        if (s->watched && s->exposed) collect_written_rows(s, NULL);
        s->texture_dirty = full;
    }
    if (watch_due(s)) collect_written_rows(s, t);
    if (format == D3DT_FMT_PAL8 && t->palette_sum != palette_sum) {
        t->palette_sum = palette_sum;
        s->texture_dirty = full;
    }
    if (d3dt_rect_empty(&s->texture_dirty)) return t;
    settle_batch_texture(t);
    if (s->texture_dirty.left < 0) s->texture_dirty.left = 0;
    if (s->texture_dirty.top < 0) s->texture_dirty.top = 0;
    if (s->texture_dirty.right > (long)s->width) s->texture_dirty.right = (long)s->width;
    if (s->texture_dirty.bottom > (long)s->height) s->texture_dirty.bottom = (long)s->height;
    box.left = (UINT)s->texture_dirty.left;
    box.top = (UINT)s->texture_dirty.top;
    box.right = (UINT)s->texture_dirty.right;
    box.bottom = (UINT)s->texture_dirty.bottom;
    box.front = 0;
    box.back = 1;
    upload_start = ticks();
    if (t->levels > 1 && d3dt_note_upload(&t->history, R.frames_hw)) make_dynamic(t);
    if (t->levels > 1 && !t->history.dynamic) {
        upload_with_mips(s, t, format, palette);
        count_upload(t->uploads ? UPLOAD_WITH_MIPS : UPLOAD_FIRST, d3dt_mip_chain_texels(t->width, t->height, t->levels),
                     upload_start);
        t->uploads++;
        R.texture_uploads++;
        perf_add(PERF_UPLOAD, upload_start);
    } else if ((rows = upload_rows((size_t)(box.right - box.left) * (box.bottom - box.top))) != NULL) {
        convert_rows(s, format, palette, &box, rows);
        t->uploads++;
        ID3D11DeviceContext_UpdateSubresource(R.ctx, texture_resource(t), texture_subresource(t, 0), &box, rows,
                                              (box.right - box.left) * 4, 0);
        count_upload(t->uploads > 1 ? UPLOAD_REGION : UPLOAD_FIRST,
                     (size_t)(box.right - box.left) * (box.bottom - box.top), upload_start);
        R.texture_uploads++;
        perf_add(PERF_UPLOAD, upload_start);
    }
    memset(&s->texture_dirty, 0, sizeof s->texture_dirty);
    return t;
}

/* ---- batching and drawing ---- */

static int ensure_capacity(UINT verts, UINT indices) {
    if (R.nverts + verts > R.cap_verts) {
        UINT cap = R.cap_verts ? R.cap_verts : 8192;
        tl_vertex_t* grown;
        while (cap < R.nverts + verts) cap *= 2;
        grown = (tl_vertex_t*)realloc(R.verts, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        R.verts = grown;
        R.cap_verts = cap;
    }
    if (R.nindices + indices > R.cap_indices) {
        UINT cap = R.cap_indices ? R.cap_indices : 16384;
        uint32_t* grown;
        while (cap < R.nindices + indices) cap *= 2;
        grown = (uint32_t*)realloc(R.indices, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        R.indices = grown;
        R.cap_indices = cap;
    }
    return 1;
}

static int grow_buffer(ID3D11Buffer** buffer, UINT* size, UINT needed, UINT bind) {
    UINT grown = *size;
    while (grown < needed) grown *= 2;
    SAFE_RELEASE(*buffer);
    *buffer = make_buffer(grown, bind);
    *size = *buffer ? grown : 0;
    return *buffer != NULL;
}

/* Ring use of the dynamic buffers: NO_OVERWRITE while space lasts, DISCARD when it wraps. */
static int stream(ID3D11Buffer** buffer, UINT* size, UINT* pos, UINT bind, const void* data, UINT bytes, UINT* offset) {
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (bytes > *size && !grow_buffer(buffer, size, bytes, bind)) return 0;
    if (*pos + bytes > *size) {
        *pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)*buffer, 0, mode, 0, &m))) return 0;
    memcpy((BYTE*)m.pData + *pos, data, bytes);
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)*buffer, 0);
    *offset = *pos;
    *pos += bytes;
    return 1;
}

static void update_constants(ID3D11Buffer* buffer, const void* data, UINT size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, data, size);
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)buffer, 0);
}

static D3D11_RECT viewport_scissor(const d3d_device_t* dev, const d3dr_target_t* t) {
    D3D11_RECT r = { 0, 0, (LONG)t->width, (LONG)t->height };
    if (dev->current && dev->current->data_set) {
        const D3DVIEWPORT2* v = &dev->current->data;
        d3dt_rect_t s = d3dt_scale_rect((long)v->dwX, (long)v->dwY, (long)v->dwWidth, (long)v->dwHeight, t->legacy_w,
                                        t->legacy_h, t->width, t->height);
        r.left = s.left;
        r.top = s.top;
        r.right = s.right;
        r.bottom = s.bottom;
    }
    return r;
}

/* blend src/dst, alpha test, fog, specular, flat, texture map blend, texture format, z enable/write, z func */
static uint32_t state_key(const d3d_device_t* dev, uint32_t format) {
    const DWORD* r = dev->render;
    uint32_t blend = r[D3DRENDERSTATE_ALPHABLENDENABLE] ? ((r[D3DRENDERSTATE_SRCBLEND] & 0xF) << 4) | (r[D3DRENDERSTATE_DESTBLEND] & 0xF) : 0;
    return blend | (r[D3DRENDERSTATE_ALPHATESTENABLE] ? 1u << 8 : 0) | (r[D3DRENDERSTATE_FOGENABLE] ? 1u << 9 : 0) |
           (r[D3DRENDERSTATE_SPECULARENABLE] ? 1u << 10 : 0) | (r[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT ? 1u << 11 : 0) |
           ((r[D3DRENDERSTATE_TEXTUREMAPBLEND] & 0xF) << 12) | (((uint32_t)format & 0xF) << 16) |
           (r[D3DRENDERSTATE_ZENABLE] ? 1u << 20 : 0) | (r[D3DRENDERSTATE_ZWRITEENABLE] ? 1u << 21 : 0) |
           ((r[D3DRENDERSTATE_ZFUNC] & 0xF) << 22);
}

static void count_state_class(uint32_t key, unsigned long long triangles) {
    for (unsigned i = 0; i < R.class_count; i++) {
        if (R.classes[i].key == key) {
            R.classes[i].draws++;
            R.classes[i].triangles += triangles;
            return;
        }
    }
    if (R.class_count < MAX_STATE_CLASSES) {
        R.classes[R.class_count].key = key;
        R.classes[R.class_count].draws = 1;
        R.classes[R.class_count].triangles = triangles;
        R.class_count++;
    }
}

/* The draw states flush() binds, each set only when it differs from what flush() last bound. */
static void bind_draw_states(d3d_device_t* dev, d3dr_target_t* t, int has_depth, ID3D11SamplerState* const* samplers,
                             const D3D11_VIEWPORT* vp, const D3D11_RECT* scissor) {
    ID3D11DepthStencilView* dsv = has_depth ? t->dsv : NULL;
    ID3D11RasterizerState* raster = raster_for(dev, t->samples > 1);
    ID3D11BlendState* blend = blend_for(dev);
    ID3D11DepthStencilState* depth = depth_for(dev, has_depth);
    float factor[4] = { 0, 0, 0, 0 };
    if (!R.bound.valid) {
        memset(&R.bound, 0, sizeof R.bound);
        ID3D11DeviceContext_IASetInputLayout(R.ctx, R.layout);
        ID3D11DeviceContext_VSSetShader(R.ctx, R.vs_tl, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(R.ctx, 0, 1, &R.cb_frame);
        ID3D11DeviceContext_PSSetShader(R.ctx, R.ps[D3D11_SHADER_PS_FIXED], NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(R.ctx, 1, 1, &R.cb_draw);
        R.bound.valid = 1;
        R.bound.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        R.bound.vp.Width = -1.0f;
        R.bound.scissor.left = -1;
    }
    if (R.bound.rtv != t->rtv || R.bound.dsv != dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->rtv, dsv);
        R.bound.rtv = t->rtv, R.bound.dsv = dsv;
    }
    if (R.bound.topology != R.batch_topology) {
        ID3D11DeviceContext_IASetPrimitiveTopology(R.ctx, R.batch_topology);
        R.bound.topology = R.batch_topology;
    }
    if (R.bound.vb != R.vb) {
        UINT stride = sizeof(tl_vertex_t), zero = 0;
        ID3D11DeviceContext_IASetVertexBuffers(R.ctx, 0, 1, &R.vb, &stride, &zero);
        R.bound.vb = R.vb;
    }
    if (R.bound.ib != R.ib) {
        ID3D11DeviceContext_IASetIndexBuffer(R.ctx, R.ib, DXGI_FORMAT_R32_UINT, 0);
        R.bound.ib = R.ib;
    }
    if (R.batch_arrays &&
        (R.bound.srvs < R.batch_arrays || memcmp(R.bound.srv, R.batch_srv, R.batch_arrays * sizeof R.batch_srv[0]))) {
        ID3D11DeviceContext_PSSetShaderResources(R.ctx, 2, R.batch_arrays, R.batch_srv);
        memcpy(R.bound.srv, R.batch_srv, R.batch_arrays * sizeof R.batch_srv[0]);
        if (R.bound.srvs < R.batch_arrays) R.bound.srvs = R.batch_arrays;
    }
    if (samplers && memcmp(R.bound.sampler, samplers, sizeof R.bound.sampler)) {
        ID3D11DeviceContext_PSSetSamplers(R.ctx, 0, D3D11_DEVICE_SAMPLER + 1, samplers);
        memcpy(R.bound.sampler, samplers, sizeof R.bound.sampler);
    }
    if (R.bound.raster != raster) {
        ID3D11DeviceContext_RSSetState(R.ctx, raster);
        R.bound.raster = raster;
    }
    if (memcmp(&R.bound.vp, vp, sizeof *vp)) {
        ID3D11DeviceContext_RSSetViewports(R.ctx, 1, vp);
        R.bound.vp = *vp;
    }
    if (memcmp(&R.bound.scissor, scissor, sizeof *scissor)) {
        ID3D11DeviceContext_RSSetScissorRects(R.ctx, 1, scissor);
        R.bound.scissor = *scissor;
    }
    if (R.bound.blend != blend) {
        ID3D11DeviceContext_OMSetBlendState(R.ctx, blend, factor, 0xFFFFFFFF);
        R.bound.blend = blend;
    }
    if (R.bound.depth != depth) {
        ID3D11DeviceContext_OMSetDepthStencilState(R.ctx, depth, 0);
        R.bound.depth = depth;
    }
}

static void end_batch(void) {
    R.nverts = R.nindices = 0;
    R.batch_arrays = 0;
    R.batch_has_texture = 0;
    R.batch_serial++;
}

static void flush(void) {
    d3d_device_t* dev = R.batch_dev;
    d3dr_target_t* t = dev ? dev->backend : NULL;
    draw_constants_t dc;
    d3dr_texture_t* tex = R.batch_tex;
    UINT vb_offset = 0, ib_offset = 0, stride = sizeof(tl_vertex_t);
    D3D11_VIEWPORT vp;
    D3D11_RECT scissor;
    ID3D11SamplerState* samplers[D3D11_DEVICE_SAMPLER + 1];
    int has_depth, sampled = 0;
    int64_t start;
    if (!R.nindices || !t || !t->surface || !target_ready(t)) {
        end_batch();
        return;
    }
    start = ticks();
    if (R.batch_arrays) {
        sampled = samplers_for(dev, samplers);
        R.textures_bound++;
    }
    memset(&dc, 0, sizeof dc);
    if (dev->render[D3DRENDERSTATE_ALPHATESTENABLE]) dc.flags |= D3D11_PS_ALPHA_TEST;
    if (dev->render[D3DRENDERSTATE_FOGENABLE]) dc.flags |= D3D11_PS_FOG;
    if (dev->render[D3DRENDERSTATE_SPECULARENABLE]) dc.flags |= D3D11_PS_SPECULAR;
    if (dev->render[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT) dc.flags |= D3D11_PS_FLAT;
    if (R.debug_output) dc.flags |= D3D11_PS_DEBUG_TEXTURE << (R.debug_output - 1);
    dc.blend_mode = dev->render[D3DRENDERSTATE_TEXTUREMAPBLEND];
    dc.alpha_func = dev->render[D3DRENDERSTATE_ALPHAFUNC];
    dc.alpha_ref = (float)d3dt_alpha_ref(dev->render[D3DRENDERSTATE_ALPHAREF]) / 255.0f;
    dc.fog[0] = ((dev->render[D3DRENDERSTATE_FOGCOLOR] >> 16) & 0xFF) / 255.0f;
    dc.fog[1] = ((dev->render[D3DRENDERSTATE_FOGCOLOR] >> 8) & 0xFF) / 255.0f;
    dc.fog[2] = (dev->render[D3DRENDERSTATE_FOGCOLOR] & 0xFF) / 255.0f;
    dc.fog[3] = 1.0f;
    if (!R.draw_cb_valid || memcmp(&dc, &R.draw_cb, sizeof dc)) {
        update_constants(R.cb_draw, &dc, sizeof dc);
        R.draw_cb = dc;
        R.draw_cb_valid = 1;
    }
    if (!R.frame_cb_valid || R.frame_cb[0] != (float)t->legacy_w || R.frame_cb[1] != (float)t->legacy_h) {
        float frame[4] = { (float)t->legacy_w, (float)t->legacy_h, 1.0f / t->legacy_w, 1.0f / t->legacy_h };
        update_constants(R.cb_frame, frame, sizeof frame);
        memcpy(R.frame_cb, frame, sizeof frame);
        R.frame_cb_valid = 1;
    }
    if (!stream(&R.vb, &R.vb_size, &R.vb_pos, D3D11_BIND_VERTEX_BUFFER, R.verts, R.nverts * stride, &vb_offset) ||
        !stream(&R.ib, &R.ib_size, &R.ib_pos, D3D11_BIND_INDEX_BUFFER, R.indices, R.nindices * 4, &ib_offset)) {
        host_log("d3d11", "vertex or index buffer upload failed; %u vertices dropped", R.nverts);
        end_batch();
        return;
    }
    has_depth = dev->target->zbuffer != NULL;
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)t->width;
    vp.Height = (float)t->height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    scissor = viewport_scissor(dev, t);
    bind_draw_states(dev, t, has_depth, sampled ? samplers : NULL, &vp, &scissor);
    ID3D11DeviceContext_DrawIndexed(R.ctx, R.nindices, ib_offset / 4u, (INT)(vb_offset / stride));
    if (has_depth && dev->render[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE && !R.depth_active) {
        R.depth_active = 1;
        host_log("renderer", "depth_buffer_active = true (z-tested draws into the %ux%u depth buffer)", t->width, t->height);
    }
    R.draw_calls++;
    R.perf.draws++;
    R.vertices += R.nverts;
    R.index_count += R.nindices;
    if (R.batch_topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        R.triangles += R.nindices / 3;
        R.perf.triangles += R.nindices / 3;
        if (dc.blend_mode == 1) R.perf.terrain_triangles += R.nindices / 3; /* DECAL: only the terrain batches */
    }
    if (host_perf_logging())
        count_state_class(state_key(dev, tex ? (uint32_t)tex->format : R.batch_has_texture ? STATE_TEX_UNUSABLE
                                                                                           : STATE_TEX_NONE),
                          R.batch_topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ? R.nindices / 3 : 0);
    end_batch();
    perf_add(PERF_DRAW, start);
}

static int array_slot(ID3D11ShaderResourceView* srv) {
    return d3dt_batch_slot((const void**)R.batch_srv, &R.batch_arrays, D3D11_WORLD_ARRAYS, srv);
}

/* What the pixel shader takes from the texture: whether it has alpha, and the colour key. */
static uint32_t texture_flags(const d3d_device_t* dev, const d3dr_texture_t* tex) {
    uint32_t flags;
    if (!tex) return 0;
    flags = D3D11_PS_TEXTURED;
    if (d3dt_format_has_alpha(tex->format)) flags |= D3D11_PS_TEX_ALPHA;
    if (dev->render[D3DRENDERSTATE_COLORKEYENABLE] && dev->texture->has_src_key) flags |= D3D11_PS_KEY_TEST;
    return flags;
}

void d3dr_state_will_change(d3d_device_t* dev) {
    if (R.batch_dev != dev || !R.nindices) return;
    flush();
}

static void log_once(unsigned char* flag, const char* fmt, DWORD a, DWORD b) {
    if (*flag) return;
    *flag = 1;
    host_log("d3d", fmt, a, b, g_cur_func);
}

HRESULT d3dr_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                  DWORD index_count) {
    d3dr_target_t* t = dev->backend;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    unsigned stride, diffuse, specular, uv;
    UINT base, n = 0, layer;
    d3dr_texture_t* tex;
    ID3D11ShaderResourceView* srv;
    uint32_t tex_flags;
    int slot = 0;
    const BYTE* src = (const BYTE*)vertices;
    int64_t start = (++R.translate_calls & (TRANSLATE_SAMPLE - 1u)) ? 0 : ticks();
    if (!d3dt_fvf_layout(fvf, &stride, &diffuse, &specular, &uv)) {
        log_once(&R.fvf_logged, "DrawPrimitive vertex format 0x%lX (type %lu) from sub_%08X: only pre-transformed "
                 "(D3DFVF_XYZRHW) vertices are translated; refused", fvf, (DWORD)type);
        return DDERR_INVALIDPARAMS;
    }
    if (!dev->in_scene) {
        log_once(&R.scene_logged, "DrawPrimitive outside BeginScene/EndScene (type %lu, %lu vertices) from sub_%08X",
                 (DWORD)type, count);
        return D3DERR_SCENE_NOT_IN_SCENE;
    }
    switch (type) {
    case D3DT_PT_POINTLIST: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case D3DT_PT_LINELIST:
    case D3DT_PT_LINESTRIP: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case D3DT_PT_TRIANGLELIST:
    case D3DT_PT_TRIANGLESTRIP:
    case D3DT_PT_TRIANGLEFAN: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    default:
        log_once(&R.type_logged, "DrawPrimitive primitive type %lu (%lu vertices) from sub_%08X: invalid", (DWORD)type, count);
        return DDERR_INVALIDPARAMS;
    }
    if (!t || !d3dt_primitive_count(type, indices ? index_count : count)) return DD_OK;
    if (!target_ready(t)) return DDERR_GENERIC;
    if (indices)
        for (DWORD i = 0; i < index_count; i++)
            if (indices[i] >= count) return DDERR_INVALIDPARAMS;
    tex = dev->texture ? texture_for(dev->texture) : NULL;
    srv = tex ? texture_view(tex) : NULL;
    tex_flags = texture_flags(dev, tex);
    if (R.batch_dev != dev || R.batch_topology != topology) {
        flush();
        R.batch_dev = dev;
        R.batch_topology = topology;
    }
    if (srv && (slot = array_slot(srv)) < 0) {
        flush();
        slot = array_slot(srv);
    }
    if (R.nindices && tex != R.batch_tex) {
        R.perf.texture_joins++;
        R.texture_joins++;
    }
    gpu_write(t);
    R.batch_tex = tex;
    if (dev->texture) R.batch_has_texture = 1;
    if (tex) tex->batch = R.batch_serial;
    layer = d3dt_world_layer(tex && tex->pool ? tex->slice : 0, (uint32_t)slot, srv ? tex_flags : 0,
                             d3dt_vertex_sampler(dev->stage[0][D3DTSS_ADDRESSU], dev->stage[0][D3DTSS_ADDRESSV]));
    if (!ensure_capacity(count, (indices ? index_count : count) * 3)) return DDERR_OUTOFMEMORY;
    base = R.nverts;
    for (DWORD i = 0; i < count; i++, src += stride) {
        tl_vertex_t* v = &R.verts[base + i];
        memcpy(&v->x, src, 16);
        v->diffuse = diffuse ? *(const uint32_t*)(src + diffuse) : 0xFFFFFFFFu;
        v->specular = specular ? *(const uint32_t*)(src + specular) : 0xFF000000u;
        if (uv) memcpy(&v->u, src + uv, 8);
        else v->u = v->v = 0.0f;
        v->layer = layer;
    }
    if (t->gpu_box_known) {
        d3dt_rect_t drawn = d3dt_vertex_box(&R.verts[base].x, sizeof(tl_vertex_t), count, t->legacy_w, t->legacy_h);
        gpu_box_add(t, &drawn);
    }
    R.nverts += count;
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        if (indices) n = d3dt_indexed_triangles(type, indices, index_count, base, R.indices + R.nindices);
        else if (type == D3DT_PT_TRIANGLEFAN) n = d3dt_fan_indices(base, count, R.indices + R.nindices);
        else if (type == D3DT_PT_TRIANGLESTRIP) n = d3dt_strip_indices(base, count, R.indices + R.nindices);
        else n = d3dt_list_indices(base, count, R.indices + R.nindices);
    } else {
        DWORD total = indices ? index_count : count;
        DWORD limit = type == D3DT_PT_LINELIST ? total & ~1u : total;
        for (DWORD i = 0; i < limit; i++) {
            uint32_t a = base + (indices ? indices[i] : i);
            if (type == D3DT_PT_LINESTRIP) {
                if (i + 1 >= total) break;
                R.indices[R.nindices + n++] = a;
                R.indices[R.nindices + n++] = base + (indices ? indices[i + 1] : i + 1);
            } else {
                R.indices[R.nindices + n++] = a;
            }
        }
    }
    R.nindices += n;
    if (start) {
        R.perf.ms[PERF_TRANSLATE] += elapsed_ms(start) * TRANSLATE_SAMPLE;
        R.perf.count[PERF_TRANSLATE] += TRANSLATE_SAMPLE;
    }
    return DD_OK;
}

static void clear_rect(d3dr_target_t* t, const d3dt_rect_t* r, DWORD flags, D3DCOLOR color, float z) {
    D3D11_RECT scissor = { r->left, r->top, r->right, r->bottom };
    float rgba[4] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, 1.0f };
    int full = r->left <= 0 && r->top <= 0 && r->right >= (long)t->width && r->bottom >= (long)t->height;
    int target = (flags & D3DCLEAR_TARGET) != 0, depth = (flags & D3DCLEAR_ZBUFFER) != 0;
    if (full) {
        if (target) ID3D11DeviceContext_ClearRenderTargetView(R.ctx, t->rtv, rgba);
        if (depth) ID3D11DeviceContext_ClearDepthStencilView(R.ctx, t->dsv, D3D11_CLEAR_DEPTH, z, 0);
        return;
    }
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->rtv, depth ? t->dsv : NULL);
    R.bound.valid = 0;
    set_pass(k_full_rect, NULL, rgba, z);
    draw_pass(R.ps[D3D11_SHADER_PS_FILL], NULL, NULL, target ? R.blend_none : R.blend_nowrite,
              depth ? R.depth_write_always : NULL, t->width, t->height, &scissor);
}

static int covers_surface(const d3dr_target_t* t, long x, long y, long w, long h) {
    return x <= 0 && y <= 0 && x + w >= (long)t->legacy_w && y + h >= (long)t->legacy_h;
}

HRESULT d3dr_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    d3dr_target_t* t = dev->backend;
    settle_deferred();
    if (!t || !target_ready(t)) return DDERR_GENERIC;
    if (R.batch_dev == dev) flush();
    for (DWORD i = 0; i < count; i++) {
        long x = rects[i].x1, y = rects[i].y1, w = rects[i].x2 - rects[i].x1, h = rects[i].y2 - rects[i].y1;
        d3dt_rect_t r = d3dt_scale_rect(x, y, w, h, t->legacy_w, t->legacy_h, t->width, t->height);
        if (d3dt_rect_empty(&r)) continue;
        if ((flags & D3DCLEAR_TARGET) && covers_surface(t, x, y, w, h)) {
            tsync_gpu_clear(&t->sync, d3dt_bgra_to_rgb565(color));
            gpu_box_cleared(t, d3dt_bgra_to_rgb565(color));
        } else if (flags & D3DCLEAR_TARGET) {
            d3dt_rect_t legacy = { x, y, x + w, y + h };
            gpu_write(t);
            gpu_box_add(t, &legacy);
        }
        clear_rect(t, &r, flags, color, z);
        R.clears++;
    }
    return DD_OK;
}

/* A colour fill of the device's target is a GPU clear; the colour is in the surface's RGB565. */
int d3dr_surface_fill(dd_surface_t* s, const RECT* r, DWORD color) {
    d3dr_target_t* t = s->target;
    D3DRECT rect;
    uint32_t bgra;
    if (!R.hardware || !t || !t->device) return 0;
    rect.x1 = r->left;
    rect.y1 = r->top;
    rect.x2 = r->right;
    rect.y2 = r->bottom;
    bgra = d3dt_rgb565_to_bgra((uint16_t)color);
    return SUCCEEDED(d3dr_clear(t->device, 1, &rect, D3DCLEAR_TARGET, bgra, 0.0f));
}

/* ---- scenes, coherence hooks and presentation ---- */

void d3dr_begin_scene(d3d_device_t* dev) {
    d3dr_target_t* t = dev->backend;
    UINT w, h;
    settle_deferred();
    if (!t || !ensure_device()) return;
    if (!t->frame_started && !R.fixed_w && t->color) {
        choose_size(t->legacy_w, t->legacy_h, &w, &h);
        if (w != t->width || h != t->height) {
            int exact = t->sync.exact;
            if (R.batch_dev == dev) flush();
            if (target_usable(t)) {
                check_front(t);
                apply_sync(t, tsync_gpu_release(&t->sync), NULL, READBACK_OTHER, exact);
            }
            release_target_resources(t);
        }
    }
    t->frame_started = 1;
    target_ready(t);
}

void d3dr_end_scene(d3d_device_t* dev) {
    if (R.batch_dev != dev) return;
    flush();
}

/* The front buffer of a hardware frame is the last presented picture, read back only if the game asks. */
static void readback_last_frame(dd_surface_t* s) {
    D3D11_MAPPED_SUBRESOURCE m;
    UINT w = s->width, h = s->height;
    int64_t start = ticks();
    if (!R.last_valid || !R.last_frame_srv || R.last_legacy_w != w || R.last_legacy_h != h ||
        s->format.dwRGBBitCount != 16 || !ensure_device())
        return;
    if (!R.front_down || R.front_w != w || R.front_h != h) {
        SAFE_RELEASE(R.front_rtv);
        SAFE_RELEASE(R.front_down);
        SAFE_RELEASE(R.front_staging);
        R.front_down = make_texture(w, h, DXGI_FORMAT_R16_UINT, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 1, 0);
        R.front_staging = make_texture(w, h, DXGI_FORMAT_R16_UINT, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 1, 0);
        if (!R.front_down || !R.front_staging ||
            FAILED(ID3D11Device_CreateRenderTargetView(R.dev, (ID3D11Resource*)R.front_down, NULL, &R.front_rtv))) {
            SAFE_RELEASE(R.front_down);
            SAFE_RELEASE(R.front_staging);
            return;
        }
        R.front_w = w;
        R.front_h = h;
    }
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &R.front_rtv, NULL);
    R.bound.valid = 0;
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PACK565], R.last_frame_srv, R.linear_clamp, R.blend_none, NULL, w, h, NULL);
    unbind_views();
    ID3D11DeviceContext_CopyResource(R.ctx, (ID3D11Resource*)R.front_staging, (ID3D11Resource*)R.front_down);
    if (FAILED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)R.front_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    for (UINT y = 0; y < h; y++)
        memcpy(s->bits + (size_t)y * s->pitch, (const BYTE*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 2);
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)R.front_staging, 0);
    note_readback(READBACK_FRONT_BUFFER, elapsed_ms(start), (size_t)w * h * 2);
}

/* ---- Locks that wait for the first touch ----
 * DDLOCK_WAIT alone does not say whether the game will read, write or only wait for the GPU, so the
 * CPU copy of a locked render target is made inaccessible instead: its first touch faults, and only
 * then does the Lock read back, capture or recreate the picture, exactly as it would have at Lock time.
 * A Lock that ends untouched was synchronisation only and leaves the target as it was. */

static void deferred_touched(void* context) {
    d3dr_target_t* t = (d3dr_target_t*)context;
    R.deferred = NULL;
    R.perf.deferred_touched++;
    R.charge_caller = t->deferred_caller;
    apply_sync(t, tsync_lock_touched(&t->deferred), &t->deferred_region, t->deferred_reason, t->sync.exact);
    R.charge_caller = 0;
}

/* Anything else that reads or changes the target first gives a waiting Lock the picture of its time. */
static void settle_deferred(void) {
    if (R.deferred) page_guard_touch();
}

/* The Lock hands out the surface's alias view, which stays closed between Locks: no protection change unless it is
 * touched. A surface without a section of its own is guarded in place. */
static page_guard_result_t arm_lock_guard(d3dr_target_t* t) {
    dd_surface_t* s = t->surface;
    size_t bytes = (size_t)s->pitch * s->height;
    if (!s->guard_alias && s->section) s->guard_alias = page_alias_create(s->section, bytes);
    if (s->guard_alias && page_guard_arm_alias(s->guard_alias, deferred_touched, t) == PAGE_GUARD_ARMED) {
        s->lock_alias = 1;
        return PAGE_GUARD_ARMED;
    }
    return page_guard_arm(s->bits, bytes, deferred_touched, t);
}

/* 0 leaves the Lock to the immediate path. */
static int defer_lock(d3dr_target_t* t, tsync_access_t kind, const d3dt_rect_t* region, readback_reason_t reason,
                      int exact, uint32_t caller) {
    page_guard_result_t armed;
    apply_sync(t, tsync_lock_deferred(&t->sync, kind, &t->deferred), region, reason, exact);
    if (!t->deferred.acts) return 1;
    t->deferred_region = *region;
    t->deferred_reason = reason;
    t->deferred_caller = caller;
    armed = arm_lock_guard(t);
    if (armed != PAGE_GUARD_ARMED) {
        apply_sync(t, tsync_lock_touched(&t->deferred), region, reason, t->sync.exact);
        return 1;
    }
    R.deferred = t;
    R.perf.deferred_locks++;
    return 1;
}

/* 1 when the Lock ended with its pixels untouched: nothing was read or written. */
int d3dr_surface_unlocked(dd_surface_t* s) {
    d3dr_target_t* t = s->target;
    if (t && t->inert_lock) {
        t->inert_lock = 0;
        return 1;
    }
    if (!t || R.deferred != t || !page_guard_disarm()) return 0;
    R.deferred = NULL;
    tsync_lock_untouched(&t->sync, &t->deferred);
    R.perf.deferred_untouched++;
    return 1;
}

/* A Lock's target going away: its pages become ordinary memory again, nothing is read. */
static void drop_deferred(const d3dr_target_t* t) {
    if (!R.deferred || (t && R.deferred != t)) return;
    page_guard_disarm();
    tsync_lock_untouched(&R.deferred->sync, &R.deferred->deferred);
    R.deferred = NULL;
}

/* Polygons the game rasterises itself into a Direct3D frame (cpu_polygons.c). One Lock spans a run of them; each
 * polygon claims the tiles of its box, so only those are captured, and a primitive not known to only store pixels
 * reads its box from the GPU, never the frame. */
static struct {
    int pending, pending_reads;
    d3dt_rect_t pending_box;
    d3dr_target_t* target;
    int captured;
    d3dt_rect_t dirty;
    tsync_tiles_t tiles;
} s_polys;

static void readback_region(d3dr_target_t* t, const d3dt_rect_t* r) {
    D3D11_MAPPED_SUBRESOURCE m;
    dd_surface_t* s = t->surface;
    int64_t start = ticks();
    size_t pixels = (size_t)(r->right - r->left) * (size_t)(r->bottom - r->top);
    if (!target_usable(t) || d3dt_rect_empty(r) || !map_target(t, 0, r, &m)) return;
    tsync_fill_unwritten(s->bits, s->pitch, t->reference, t->legacy_w, r,
                         (const uint16_t*)((const BYTE*)m.pData + (size_t)r->top * m.RowPitch) + r->left,
                         m.RowPitch / 2);
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)t->staging, 0);
    note_readback(READBACK_LOCK_READ, elapsed_ms(start), pixels * 2);
    R.perf.cpu_polygon_readbacks++;
    R.perf.cpu_polygon_readback_pixels += pixels;
}

static void capture_polygon_tiles(void* context, const d3dt_rect_t* r) {
    d3dr_target_t* t = context;
    fill_copy(t, r, TSYNC_UNWRITTEN);
    if (t->lazy) tsync_tiles_claim(&t->fresh, r, mark_fresh, NULL);
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

static void add_polygon(d3dr_target_t* t, const d3dt_rect_t* box, int reads) {
    d3dt_rect_union(&s_polys.dirty, box);
    R.perf.cpu_polygons++;
    if (!s_polys.captured || d3dt_rect_empty(box)) return;
    tsync_tiles_claim(&s_polys.tiles, box, capture_polygon_tiles, t);
    if (reads) readback_region(t, box);
}

static void polygon_lock(d3dr_target_t* t, uint32_t caller) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t box = clip_to_lock(s, s_polys.pending_box), all = { 0, 0, (long)s->width, (long)s->height };
    int exact = t->sync.exact, reads = s_polys.pending_reads;
    unsigned acts;
    s_polys.pending = 0;
    check_front(t);
    acts = tsync_cpu_access(&t->sync, TSYNC_ACCESS_NO_READ);
    R.charge_caller = caller;
    apply_sync(t, acts & ~(unsigned)(TSYNC_CAPTURE | TSYNC_CAPTURE_ALL), &box, READBACK_LOCK_WRITE, exact);
    s_polys.target = t;
    s_polys.captured = !(t->sync.valid & TSYNC_CPU);
    memset(&s_polys.dirty, 0, sizeof s_polys.dirty);
    if (s_polys.captured) {
        if (!tsync_tiles_reset(&s_polys.tiles, s->width, s->height)) {
            fill_copy(t, &all, TSYNC_UNWRITTEN);
            t->lazy = 0;
            tsync_tiles_claim_all(&s_polys.tiles);
        } else if (acts & TSYNC_CAPTURE_ALL) {
            start_lazy_capture(t);
        }
        R.perf.no_read_accesses++;
    }
    add_polygon(t, &box, reads);
    R.charge_caller = 0;
    R.perf.cpu_polygon_locks++;
}

void d3dr_cpu_polygon(void* surface_iface, const long raster_box[4], uint32_t origin_bytes, int reads) {
    dd_surface_t* s = R.hardware && surface_iface ? dd_surface_from(surface_iface) : NULL;
    long at[4] = { raster_box[0], raster_box[1], raster_box[2], raster_box[3] };
    d3dt_rect_t box;
    if (!s || !s->target || !s->target->device) return;
    cpu_polygon_to_surface(at, origin_bytes, s->pitch);
    box.left = at[0];
    box.top = at[1];
    box.right = at[2];
    box.bottom = at[3];
    /* Lockers that never draw a polygon (BeginFrame, FlushBatch's wait, fills) keep their own path. */
    if (!target_access_reads(CPU_ACCESS_LOCK, 0, readback_caller(), 1)) return;
    if (!s->locked) {
        s_polys.pending = 1;
        s_polys.pending_reads = reads;
        s_polys.pending_box = box;
        return;
    }
    if (s_polys.target == s->target) {
        box = clip_to_lock(s, box);
        add_polygon(s->target, &box, reads);
    } else if (R.deferred == s->target && page_guard_disarm()) {
        /* A Lock taken to hand the surface on (BeginFrame) and not touched yet: the polygons own it from here. */
        d3dr_target_t* t = s->target;
        R.deferred = NULL;
        tsync_lock_untouched(&t->sync, &t->deferred);
        if (R.batch_dev == t->device) flush();
        s_polys.pending_reads = reads;
        s_polys.pending_box = box;
        polygon_lock(t, readback_caller());
        R.perf.cpu_polygon_adopted++;
    }
}

void d3dr_cpu_polygon_done(void) { s_polys.pending = 0; }

static void end_polygon_run(const d3dr_target_t* t) {
    if (!t || s_polys.target == t) s_polys.target = NULL;
    if (!t || R.text_target == t) R.text_target = NULL;
}

void d3dr_text_box(HDC dc, const RECT* box) {
    d3dr_target_t* t = R.text_target;
    d3dt_rect_t r;
    if (!t || !t->surface || !t->surface->dc_out || t->surface->dc != dc || !t->lazy) return;
    if (box) {
        r.left = box->left;
        r.top = box->top;
        r.right = box->right;
        r.bottom = box->bottom;
    } else {
        r = whole_target(t);
    }
    claim_fresh(t, &r);
    R.perf.text_boxes++;
}

/* Before the game touches a render target's pixels: writes still pending reach the GPU first, then
 * the CPU copy becomes the GPU picture only if this access reads it. */
static void target_cpu_access(d3dr_target_t* t, int will_write, cpu_access_t access) {
    dd_surface_t* s = t->surface;
    uint32_t caller = access == CPU_ACCESS_GETDC && game_text_drawing() ? GETDC_CALLER_TEXT_OVERLAY : readback_caller();
    int reads = target_access_reads(access, s->lock_flags, caller, ui_screen_blt_writes_only()), exact;
    int text_boxes = caller == GETDC_CALLER_TEXT_OVERLAY && game_text_boxes_known();
    tsync_access_t kind = reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ;
    readback_reason_t reason = readback_classify(access, !will_write, g_cur_func);
    d3dt_rect_t region = { 0, 0, (long)s->width, (long)s->height };
    /* FlushBatch locks only to wait for the hardware and unlocks at once: nothing to synchronise or flush. */
    if (access == CPU_ACCESS_LOCK && caller == LOCK_CALLER_FLUSH_BATCH) {
        t->inert_lock = 1;
        R.perf.inert_locks++;
        return;
    }
    settle_deferred();
    if (R.batch_dev == t->device) flush();
    if (!target_ready(t)) return;
    if (access == CPU_ACCESS_LOCK && s_polys.pending) {
        polygon_lock(t, caller);
        R.perf.target_accesses++;
        return;
    }
    if (access == CPU_ACCESS_LOCK) {
        region.left = s->lock_rect.left;
        region.top = s->lock_rect.top;
        region.right = s->lock_rect.right;
        region.bottom = s->lock_rect.bottom;
    }
    check_front(t);
    exact = t->sync.exact;
    if (access != CPU_ACCESS_LOCK || !defer_lock(t, kind, &region, reason, exact, caller)) {
        R.charge_caller = caller;
        R.capture_text = text_boxes;
        apply_sync(t, tsync_cpu_access(&t->sync, kind), &region, reason, exact);
        R.capture_text = 0;
        R.charge_caller = 0;
    }
    R.perf.target_accesses++;
}

void d3dr_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access) {
    if (!R.hardware) return;
    if (s->target && s->target->device) target_cpu_access(s->target, will_write, access);
    if (s->front_from_gpu) {
        s->front_from_gpu = 0;
        readback_last_frame(s);
    }
    if (will_write) settle_batch_texture((const d3dr_texture_t*)s->gpu_texture);
}

/* A change to a flip chain's front buffer ends its use as a copy of the GPU picture. */
static void front_written(dd_surface_t* s) {
    if (s->root || !s->next) return;
    for (dd_surface_t* m = s->next; m && m != s; m = m->next)
        if (m->target) tsync_front_changed(&m->target->sync);
}

void d3dr_surface_cpu_written(dd_surface_t* s, const RECT* r) {
    s->cpu_writes++;
    if (!R.hardware) return;
    if (R.text_target && R.text_target->surface == s) R.text_target = NULL;
    front_written(s);
    if (s->target) {
        d3dt_rect_t add = { 0, 0, (long)s->width, (long)s->height };
        if (r) {
            add.left = r->left;
            add.top = r->top;
            add.right = r->right;
            add.bottom = r->bottom;
        }
        if (s_polys.target == s->target) {
            add = s_polys.dirty;
            end_polygon_run(s->target);
            if (d3dt_rect_empty(&add)) return;
        }
        tsync_cpu_wrote(&s->target->sync);
        d3dt_rect_union(&s->target->dirty, &add);
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

static void update_gamma(const DDGAMMARAMP* gamma) {
    uint32_t lut[256];
    if (R.gamma_valid && !memcmp(R.gamma_last, gamma, sizeof R.gamma_last)) return;
    for (int i = 0; i < 256; i++)
        lut[i] = (uint32_t)(gamma->red[i] >> 8) | ((uint32_t)(gamma->green[i] >> 8) << 8) |
                 ((uint32_t)(gamma->blue[i] >> 8) << 16) | 0xFF000000u;
    ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)R.gamma_tex, 0, NULL, lut, sizeof lut, 0);
    memcpy(R.gamma_last, gamma, sizeof R.gamma_last);
    R.gamma_valid = 1;
}

/* Every 8th row and 4th pixel: the frozen picture is replaced whole when the game captures a new one. */
static uint32_t still_key(const uint16_t* bits, long pitch, int w, int h) {
    uint32_t key = 2166136261u;
    for (int y = 0; y < h; y += 8)
        for (int x = (y / 8) & 3; x < w; x += 4) key = (key ^ bits[(size_t)y * pitch + x]) * 16777619u;
    return key;
}

static int upload_still(const uint16_t* bits, long pitch, int w, int h) {
    uint32_t key = still_key(bits, pitch, w, h);
    uint32_t* rows;
    if (R.still_tex && R.still_w == (UINT)w && R.still_h == (UINT)h && R.still_bits == bits && R.still_key == key)
        return 1;
    if (!R.still_tex || R.still_w != (UINT)w || R.still_h != (UINT)h) {
        SAFE_RELEASE(R.still_srv);
        SAFE_RELEASE(R.still_tex);
        R.still_tex = make_texture((UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
        if (!R.still_tex ||
            FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)R.still_tex, NULL, &R.still_srv))) {
            SAFE_RELEASE(R.still_tex);
            return 0;
        }
        R.still_w = (UINT)w;
        R.still_h = (UINT)h;
    }
    rows = upload_rows((size_t)w * h);
    if (!rows) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) rows[(size_t)y * w + x] = R.rgb565_table[bits[(size_t)y * pitch + x]];
    ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)R.still_tex, 0, NULL, rows, (UINT)w * 4, 0);
    R.still_bits = bits;
    R.still_key = key;
    R.still_uploads++;
    return 1;
}

/* The game's frozen gameplay picture behind its pause menus, drawn on the GPU target instead of being copied into the
 * screen's CPU copy every frame: no Lock, no full-screen compare and no full-screen upload while a menu is open. */
int d3dr_draw_still_picture(void* surface, const uint16_t* bits, long pitch, int w, int h) {
    dd_surface_t* s = dd_surface_from(surface);
    d3dr_target_t* t = s ? s->target : NULL;
    d3dt_rect_t all;
    float rect[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    if (!R.hardware || !t || !t->device || !bits || t->legacy_w != (UINT)w || t->legacy_h != (UINT)h) return 0;
    settle_deferred();
    if (R.batch_dev == t->device) flush();
    if (!target_ready(t)) return 0;
    gpu_write(t);
    if (!upload_still(bits, pitch, w, h)) return 0;
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->rtv, NULL);
    R.bound.valid = 0;
    set_pass(rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_COPY], R.still_srv, R.linear_clamp, R.blend_none, NULL, t->width, t->height, NULL);
    unbind_views();
    all = whole_target(t);
    gpu_box_add(t, &all);
    R.still_draws++;
    return 1;
}

void d3dr_set_ui_layer(const uint16_t* bits, long pitch, int guest_w, int guest_h) {
    R.ui.bits = bits;
    R.ui.pitch = pitch;
    R.ui.guest_w = guest_w;
    R.ui.guest_h = guest_h;
}

/* The game's 640x480 UI canvas over the frame: unwritten texels are transparent, the rest is scaled to the canvas
 * rectangle of the game's screen with the sampler the frontend uses. */
static void draw_ui_layer(UINT width, UINT height) {
    jpt_ui_canvas_t canvas;
    float rect[4];
    if (!R.ui.bits || R.ui.guest_w <= 0 || R.ui.guest_h <= 0 || !R.blend_premultiplied) return;
    if (!R.ui_tex) {
        R.ui_tex = make_texture(JPT_UI_CANVAS_W, JPT_UI_CANVAS_H, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
        if (!R.ui_tex || FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)R.ui_tex, NULL, &R.ui_srv))) {
            SAFE_RELEASE(R.ui_tex);
            return;
        }
    }
    for (int y = 0; y < JPT_UI_CANVAS_H; y++) {
        const uint16_t* in = R.ui.bits + (size_t)y * (size_t)R.ui.pitch;
        uint32_t* out = R.ui_pixels + (size_t)y * JPT_UI_CANVAS_W;
        for (int x = 0; x < JPT_UI_CANVAS_W; x++) out[x] = in[x] == TSYNC_UNWRITTEN ? 0u : R.rgb565_table[in[x]];
    }
    ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)R.ui_tex, 0, NULL, R.ui_pixels,
                                          JPT_UI_CANVAS_W * 4, 0);
    canvas = jpt_ui_canvas(R.ui.guest_w, R.ui.guest_h);
    jpt_ui_canvas_ndc(&canvas, R.ui.guest_w, R.ui.guest_h, rect);
    set_pass(rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PRESENT], R.ui_srv, R.linear_clamp, R.blend_premultiplied, NULL, width, height, NULL);
}

void d3dr_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty) {
    R.fe.pixels = pixels;
    if (!pixels) return;
    R.fe.w = w;
    R.fe.h = h;
    if (!dirty || dirty->right <= dirty->left || dirty->bottom <= dirty->top) return;
    if (!R.fe.pending) R.fe.dirty = *dirty;
    else UnionRect(&R.fe.dirty, &R.fe.dirty, dirty);
    R.fe.pending = 1;
}

void d3dr_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    R.fe.pointer = sprite;
    R.fe.pointer_size = size;
    R.fe.pointer_generation = generation;
    R.fe.pointer_x = x;
    R.fe.pointer_y = y;
}

int d3dr_frontend_failed(void) { return R.fe.failed; }

void d3dr_frontend_release(void) {
    SAFE_RELEASE(R.fe.srv);
    SAFE_RELEASE(R.fe.tex);
    SAFE_RELEASE(R.fe.pointer_srv);
    SAFE_RELEASE(R.fe.pointer_tex);
    R.fe.tex_w = R.fe.tex_h = R.fe.pointer_tex_size = 0;
    R.fe.pending = 0;
}

static int frontend_texture(ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, int w, int h) {
    *tex = make_texture((UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 1,
                        0);
    if (*tex && SUCCEEDED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)*tex, NULL, srv))) return 1;
    SAFE_RELEASE(*tex);
    return 0;
}

static void draw_pointer(UINT width, UINT height) {
    float rect[4];
    int size = R.fe.pointer_size;
    if (!R.fe.pointer || size <= 0) return;
    if (!R.fe.pointer_tex || R.fe.pointer_tex_size != size) {
        SAFE_RELEASE(R.fe.pointer_srv);
        SAFE_RELEASE(R.fe.pointer_tex);
        R.fe.pointer_tex_size = 0;
        if (!frontend_texture(&R.fe.pointer_tex, &R.fe.pointer_srv, size, size)) return;
        R.fe.pointer_tex_size = size;
        R.fe.pointer_uploaded = R.fe.pointer_generation - 1;
    }
    if (R.fe.pointer_uploaded != R.fe.pointer_generation) {
        ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)R.fe.pointer_tex, 0, NULL, R.fe.pointer,
                                              (UINT)size * 4, 0);
        R.fe.pointer_uploaded = R.fe.pointer_generation;
    }
    rect[0] = (float)R.fe.pointer_x / (float)R.fe.w * 2.0f - 1.0f;
    rect[1] = 1.0f - (float)R.fe.pointer_y / (float)R.fe.h * 2.0f;
    rect[2] = (float)(R.fe.pointer_x + size) / (float)R.fe.w * 2.0f - 1.0f;
    rect[3] = 1.0f - (float)(R.fe.pointer_y + size) / (float)R.fe.h * 2.0f;
    set_pass(rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PRESENT], R.fe.pointer_srv, R.point_clamp, R.blend_premultiplied, NULL, width, height,
              NULL);
}

/* The HD menu layer (frontend_hd.c): the game's screen size, premultiplied, only its changed rectangle uploaded. */
static int draw_frontend(UINT width, UINT height) {
    if (!R.fe.pixels || R.fe.w <= 0 || R.fe.h <= 0 || R.fe.failed || !R.blend_premultiplied) return 0;
    if (!R.fe.tex || R.fe.tex_w != R.fe.w || R.fe.tex_h != R.fe.h) {
        SAFE_RELEASE(R.fe.srv);
        SAFE_RELEASE(R.fe.tex);
        R.fe.tex_w = R.fe.tex_h = 0;
        if (!frontend_texture(&R.fe.tex, &R.fe.srv, R.fe.w, R.fe.h)) {
            R.fe.failed = 1;
            host_log("d3d11", "the %dx%d HD menu texture could not be created: the game's own menus are shown", R.fe.w,
                     R.fe.h);
            return 0;
        }
        R.fe.tex_w = R.fe.w;
        R.fe.tex_h = R.fe.h;
        SetRect(&R.fe.dirty, 0, 0, R.fe.w, R.fe.h);
        R.fe.pending = 1;
    }
    if (R.fe.pending) {
        RECT d = R.fe.dirty;
        D3D11_BOX box;
        if (d.left < 0) d.left = 0;
        if (d.top < 0) d.top = 0;
        if (d.right > R.fe.w) d.right = R.fe.w;
        if (d.bottom > R.fe.h) d.bottom = R.fe.h;
        if (d.right > d.left && d.bottom > d.top) {
            box.left = (UINT)d.left;
            box.top = (UINT)d.top;
            box.right = (UINT)d.right;
            box.bottom = (UINT)d.bottom;
            box.front = 0;
            box.back = 1;
            ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)R.fe.tex, 0, &box,
                                                  R.fe.pixels + (size_t)d.top * (size_t)R.fe.w + (size_t)d.left,
                                                  (UINT)R.fe.w * 4, 0);
        }
        R.fe.pending = 0;
    }
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PRESENT], R.fe.srv,
              width == (UINT)R.fe.w && height == (UINT)R.fe.h ? R.point_clamp : R.linear_clamp, R.blend_premultiplied, NULL,
              width, height, NULL);
    draw_pointer(width, height);
    return 1;
}

static int present_view(HWND window, ID3D11ShaderResourceView* srv, UINT width, UINT height, ID3D11SamplerState* sampler,
                        const DDGAMMARAMP* gamma, int vsync) {
    static int s_held_logged;
    ID3D11RenderTargetView* rtv;
    if (!window_host_frames_allowed()) return 1;
    if (ui_canvas_present_held()) {
        if (!s_held_logged++)
            host_log("ui", "a blank present between two menu frames was held: the last menu frame stays on screen");
        return 1;
    }
    if (loading_hd_present_held()) return 1;
    rtv = present_target(window, width, height);
    if (!rtv) return 0;
    update_gamma(gamma);
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &rtv, NULL);
    R.bound.valid = 0;
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PRESENT], srv, sampler, R.blend_none, NULL, width, height, NULL);
    if (!draw_frontend(width, height)) draw_ui_layer(width, height);
    unbind_views();
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 0, NULL, NULL);
    R.bound.valid = 0;
    return present_submit(vsync);
}

static void keep_last_frame(d3dr_target_t* t) {
    if (!R.last_frame || R.last_w != t->width || R.last_h != t->height) {
        SAFE_RELEASE(R.last_frame_srv);
        SAFE_RELEASE(R.last_frame);
        R.last_frame = make_texture(t->width, t->height, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 1, 0);
        if (!R.last_frame ||
            FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)R.last_frame, NULL, &R.last_frame_srv))) {
            SAFE_RELEASE(R.last_frame);
            return;
        }
        R.last_w = t->width;
        R.last_h = t->height;
    }
    ID3D11DeviceContext_CopyResource(R.ctx, (ID3D11Resource*)R.last_frame, (ID3D11Resource*)t->single);
    R.last_legacy_w = t->legacy_w;
    R.last_legacy_h = t->legacy_h;
    R.last_valid = 1;
}

/* The process is 32-bit and not large-address-aware: 2 GB of address space for the guest image, its heaps, the
 * surfaces and the drivers. */
static void sample_address_space(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return;
    R.va_total = m.ullTotalVirtual;
    R.va_used = m.ullTotalVirtual - m.ullAvailVirtual;
    if (R.va_used > R.va_peak) R.va_peak = R.va_used;
}

static void log_address_space(void) {
    size_t reserved = 0, used = 0;
    sample_address_space();
    page_arena_usage(&reserved, &used);
    host_log("renderer", "address space %.0f MB of %.0f MB in use (peak %.0f MB); small textures share %.1f MB of "
             "arenas, %.1f MB of it in use", R.va_used / 1048576.0, R.va_total / 1048576.0, R.va_peak / 1048576.0,
             reserved / 1048576.0, used / 1048576.0);
}

static void log_metric_lines(const char* when) {
    host_log("renderer", "%s", when);
    host_log("renderer", "frames %llu (hardware %llu, DirectDraw pictures %llu)", R.frames_hw + R.frames_dib, R.frames_hw,
             R.frames_dib);
    host_log("renderer", "hardware_draw_calls %llu", R.draw_calls);
    host_log("renderer", "hardware_triangles %llu", R.triangles);
    host_log("renderer", "vertices %llu", R.vertices);
    host_log("renderer", "indices %llu", R.index_count);
    host_log("renderer", "textures_bound %llu", R.textures_bound);
    host_log("renderer", "texture_uploads %llu (textures reused from the game's earlier surfaces: %llu)",
             R.texture_uploads, R.textures_reused);
    host_log("renderer", "texture_memory %.1f MB (%llu textures with generated mipmaps, %llu of them dynamic: level 0 "
             "only; %.1f M mip texels built in %.0f ms)", R.texture_memory / (1024.0 * 1024.0), R.textures_mipped,
             R.textures_dynamic, R.mip_texels / 1e6, R.mip_ms);
    host_log("renderer", "texture arrays: %u pages of %u slices (peak %u); draws that joined a batch with another "
             "texture %llu", R.pool_pages, POOL_SLICES, R.pool_pages_peak, R.texture_joins);
    log_address_space();
    host_log("renderer", "depth_buffer_active = %s", R.depth_active ? "true" : "false");
    host_log("renderer", "render-target readbacks %llu, CPU overlays %llu, clears %llu", R.readbacks, R.overlays, R.clears);
    host_log("renderer", "frozen gameplay picture behind the menus: drawn on the GPU %llu times, uploaded %llu times",
             R.still_draws, R.still_uploads);
    host_log("renderer", "SelfMod entries during hardware frames: %.1f per frame",
             R.frames_hw ? (double)R.selfmod_hw / (double)R.frames_hw : 0.0);
}

static void log_state_classes(void) {
    static const char* const blends[] = { "?", "ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA", "INVSRCALPHA",
                                          "DESTALPHA", "INVDESTALPHA", "DESTCOLOR", "INVDESTCOLOR", "SRCALPHASAT",
                                          "BOTHSRCALPHA", "BOTHINVSRCALPHA", "?", "?" };
    if (!R.class_count) return;
    host_log("d3d", "draw state classes (blended ones are the water/glass candidates):");
    for (unsigned i = 0; i < R.class_count; i++) {
        uint32_t k = R.classes[i].key;
        char blend[48];
        if (k & 0xFF) _snprintf(blend, sizeof blend - 1, "blend %s/%s", blends[(k >> 4) & 0xF], blends[k & 0xF]);
        else strcpy(blend, "opaque");
        blend[sizeof blend - 1] = 0;
        uint32_t fmt = (k >> 16) & 0xF;
        host_log("d3d", "  %-30s tex %-8s tmb %u%s%s%s%s z %s%s func %u: %llu draws, %llu triangles", blend,
                 fmt == STATE_TEX_NONE ? "none" : fmt == STATE_TEX_UNUSABLE ? "unusable" : d3dt_format_name((int)fmt),
                 (k >> 12) & 0xF, (k & (1u << 8)) ? " alphatest" : "",
                 (k & (1u << 9)) ? " fog" : "", (k & (1u << 10)) ? " specular" : "", (k & (1u << 11)) ? " flat" : "",
                 (k & (1u << 20)) ? "on" : "off", (k & (1u << 21)) ? "+write" : "", (k >> 22) & 0xF, R.classes[i].draws,
                 R.classes[i].triangles);
    }
}

static const readback_caller_t* top_caller(const perf_window_t* p) {
    const readback_caller_t* top = NULL;
    for (int i = 0; i < MAX_READBACK_CALLERS; i++)
        if (p->callers[i].count && (!top || p->callers[i].ms > top->ms)) top = &p->callers[i];
    return top;
}

static void log_readback_detail(const perf_window_t* p, const char* when) {
    char line[512];
    int n = 0;
    unsigned long long count = 0;
    double ms = 0.0;
    const readback_caller_t* top = top_caller(p);
    for (int r = 0; r < READBACK_REASON_COUNT; r++) {
        count += p->readback_n[r];
        ms += p->readback_ms[r];
        if (p->readback_n[r])
            n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %llu", n ? ", " : "",
                           readback_reason_name((readback_reason_t)r) + 9, p->readback_n[r]);
    }
    line[sizeof line - 1] = 0;
    if (!count && !p->synthesized && !p->no_read_accesses && !p->front_copies && !p->composites && !p->deferred_locks)
        return;
    if (top)
        host_log("perf", "%s: readbacks %llu | %.1f MB | %.1f ms | top sub_%08X (%s %llu, %.1f ms) | %s | not read: "
                 "write-only %llu, clears recreated %llu, front copies %llu | Locks waiting for a touch %llu: "
                 "untouched %llu, touched %llu", when, count, p->readback_bytes / (1024.0 * 1024.0), ms, top->caller,
                 readback_reason_name(top->reason) + 9, top->count, top->ms, n ? line : "none", p->no_read_accesses,
                 p->synthesized, p->front_copies, p->deferred_locks, p->deferred_untouched, p->deferred_touched);
    else
        host_log("perf", "%s: readbacks 0 | not read: write-only %llu, clears recreated %llu, front copies %llu | Locks "
                 "waiting for a touch %llu: untouched %llu, touched %llu", when, p->no_read_accesses, p->synthesized,
                 p->front_copies, p->deferred_locks, p->deferred_untouched, p->deferred_touched);
}

/* What hardware frames are made of: Direct3D draws versus pixels the CPU wrote into the target. */
static void log_hybrid(const perf_window_t* p, const char* when) {
    double frames = p->frames ? (double)p->frames : 1.0;
    if (!p->draws && !p->target_accesses) return;
    host_log("perf", "%s per frame: D3D draws %.1f, triangles %.0f (terrain %.0f) | CPU accesses to the target %.2f, "
             "CPU pixels composited %.0f (black %.0f) in %.2f composites, full CPU pictures uploaded %llu | CPU polygons "
             "%.1f in %.2f Locks (%.2f taken over from a waiting Lock), box readbacks %.2f (%.0f px) | FlushBatch Locks "
             "skipped %.2f | CPU copy pixels filled %.0f, compared %.0f | text boxes %.1f | texture write-watch reads "
             "%.1f | draws joined across textures %.1f", when,
             p->draws / frames, p->triangles / frames, p->terrain_triangles / frames, p->target_accesses / frames,
             p->composited_pixels / frames,
             p->composited_black / frames, p->composites / frames, p->uploads, p->cpu_polygons / frames,
             p->cpu_polygon_locks / frames, p->cpu_polygon_adopted / frames, p->cpu_polygon_readbacks / frames,
             p->cpu_polygon_readback_pixels / frames, p->inert_locks / frames, p->filled_pixels / frames,
             p->scanned_pixels / frames, p->text_boxes / frames, p->watch_reads / frames, p->texture_joins / frames);
}

static void accumulate(perf_window_t* total, const perf_window_t* p) {
    for (int i = 0; i < PERF_COUNT; i++) {
        total->ms[i] += p->ms[i];
        total->count[i] += p->count[i];
    }
    for (int r = 0; r < READBACK_REASON_COUNT; r++) {
        total->readback_n[r] += p->readback_n[r];
        total->readback_ms[r] += p->readback_ms[r];
    }
    total->readback_bytes += p->readback_bytes;
    total->frames += p->frames;
    total->no_read_accesses += p->no_read_accesses;
    total->synthesized += p->synthesized;
    total->front_copies += p->front_copies;
    total->draws += p->draws;
    total->triangles += p->triangles;
    total->terrain_triangles += p->terrain_triangles;
    total->target_accesses += p->target_accesses;
    total->composites += p->composites;
    total->composited_pixels += p->composited_pixels;
    total->composited_black += p->composited_black;
    total->uploads += p->uploads;
    total->cpu_polygons += p->cpu_polygons;
    total->cpu_polygon_locks += p->cpu_polygon_locks;
    total->cpu_polygon_readbacks += p->cpu_polygon_readbacks;
    total->cpu_polygon_readback_pixels += p->cpu_polygon_readback_pixels;
    total->cpu_polygon_adopted += p->cpu_polygon_adopted;
    total->inert_locks += p->inert_locks;
    total->filled_pixels += p->filled_pixels;
    total->scanned_pixels += p->scanned_pixels;
    total->text_boxes += p->text_boxes;
    total->watch_reads += p->watch_reads;
    total->texture_joins += p->texture_joins;
    total->deferred_locks += p->deferred_locks;
    total->deferred_untouched += p->deferred_untouched;
    total->deferred_touched += p->deferred_touched;
    for (int i = 0; i < MAX_READBACK_CALLERS; i++) {
        const readback_caller_t* c = &p->callers[i];
        readback_caller_t* slot = c->count ? caller_slot(total->callers, c->caller, c->reason) : NULL;
        if (!slot) continue;
        slot->count += c->count;
        slot->bytes += c->bytes;
        slot->ms += c->ms;
    }
}

static void log_uploads(unsigned long long frames) {
    static const char* const kinds[3] = { "first", "whole with mipmaps", "level-0 regions" };
    unsigned long long loads, unchanged, texels, changed;
    double f = frames ? (double)frames : 1.0;
    char line[400];
    int n = 0;
    for (int k = 0; k < 3; k++)
        n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %.1f (%.0f texels, %.2f ms)", k ? ", " : "", kinds[k],
                       R.uploads_by[k].n / f, R.uploads_by[k].texels / f, R.uploads_by[k].ms / f);
    line[sizeof line - 1] = 0;
    d3d_objects_load_counts(&loads, &unchanged, &texels, &changed);
    host_log("textures", "last 5 s per frame: uploads %s; Loads %.1f (unchanged %.1f), texels changed %.0f of %.0f, "
             "dynamic textures so far %llu", line, loads / f, unchanged / f, changed / f, texels / f, R.textures_dynamic);
    memset(R.uploads_by, 0, sizeof R.uploads_by);
}

static void log_perf_window(const perf_window_t* p, double seconds) {
    double frames = p->frames ? (double)p->frames : 1.0, host = 0.0;
    double frame_ms = seconds * 1000.0 / frames;
    for (int i = 0; i < PERF_COUNT; i++) host += p->ms[i];
    host_log("perf", "%s | FPS %.1f | frame %.1f ms | guest %.1f | translate %.2f | draws %.2f | uploads %.2f | "
             "readbacks %.2f | overlays %.2f | present %.2f ms per frame", d3dr_name(), p->frames / seconds, frame_ms,
             frame_ms - host / frames > 0.0 ? frame_ms - host / frames : 0.0, p->ms[PERF_TRANSLATE] / frames,
             p->ms[PERF_DRAW] / frames, p->ms[PERF_UPLOAD] / frames, p->ms[PERF_READBACK] / frames,
             p->ms[PERF_OVERLAY] / frames, p->ms[PERF_PRESENT] / frames);
    if (!R.hardware) return;
    log_readback_detail(p, "last 5 s");
    log_hybrid(p, "last 5 s");
    if (R.textures_created_window || R.textures_released_window || R.textures_parked_window ||
        R.textures_reused_window)
        host_log("textures", "last 5 s: D3D11 textures created %u, released %u, parked %u, reused %u (no upload, no "
                 "mipmaps); alive %u (peak %u), %.1f MB with their mipmaps, of them parked %u (%.1f MB), on game "
                 "device #%u", R.textures_created_window, R.textures_released_window, R.textures_parked_window,
                 R.textures_reused_window, R.textures_alive, R.textures_peak, R.texture_memory / 1048576.0,
                 s_parked_count, s_parked_bytes / 1048576.0, R.devices);
    log_uploads(p->frames);
}

static void end_perf_window(double seconds) {
    perf_window_t* p = &R.perf;
    if (R.hardware) age_parked(now_seconds());
    if (host_perf_logging()) {
        log_perf_window(p, seconds);
        guest_profile_window();
    }
    R.textures_created_window = R.textures_released_window = 0;
    R.textures_parked_window = R.textures_reused_window = 0;
    memset(R.uploads_by, 0, sizeof R.uploads_by);
    sample_address_space();
    accumulate(&R.total, p);
    memset(p, 0, sizeof *p);
}

/* Called for every presented frame, whichever renderer drew it. */
void d3dr_frame_presented(double present_ms) {
    double now = now_seconds();
    R.perf.frames++;
    R.perf.ms[PERF_PRESENT] += present_ms;
    R.perf.count[PERF_PRESENT]++;
    if (R.perf_start == 0.0) R.perf_start = now;
    if (now - R.perf_start >= PERF_INTERVAL) {
        end_perf_window(now - R.perf_start);
        R.perf_start = now;
    }
}

void d3dr_summary(void) {
    unsigned long refused_render = 0, refused_stage = 0, failed = 0;
    d3d_objects_counts(&refused_render, &refused_stage, &failed);
    accumulate(&R.total, &R.perf);
    memset(&R.perf, 0, sizeof R.perf);
    if (R.draw_calls || R.hardware) {
        log_metric_lines("summary at exit:");
        log_readback_detail(&R.total, "session");
        if (host_perf_logging()) {
            log_hybrid(&R.total, "session");
            for (int i = 0; i < MAX_READBACK_CALLERS; i++)
                if (R.total.callers[i].count)
                    host_log("perf", "readback caller sub_%08X %s: %llu, %.1f MB, %.1f ms", R.total.callers[i].caller,
                             readback_reason_name(R.total.callers[i].reason), R.total.callers[i].count,
                             R.total.callers[i].bytes / (1024.0 * 1024.0), R.total.callers[i].ms);
            log_state_classes();
        }
        if (R.unknown_count) host_log("d3d", "unknown texture formats: %u (listed above when first drawn)", R.unknown_count);
        host_log("d3d", "textures changed without Lock/Unlock: %llu", R.untracked_writes);
        host_log("d3d", "CPU pixels composited over the GPU target: gameplay %llu, menus/dialogs/videos %llu; black ones "
                 "over known non-black GPU pixels: gameplay %llu, menus %llu; black ones over a capture (GPU pixels "
                 "unknown): gameplay %llu, menus %llu", R.cpu_pixels[1], R.cpu_pixels[0], R.black_over_gpu[1],
                 R.black_over_gpu[0], R.black_over_unknown[1], R.black_over_unknown[0]);
    }
    engine_limits_summary(R.draw_calls);
    host_log("d3d", "unsupported_render_states %lu", refused_render);
    host_log("d3d", "unsupported_texture_stage_states %lu", refused_stage);
    host_log("d3d", "failed_d3d_calls %lu", failed);
}

int d3dr_flip(dd_surface_t* front, int vsync) {
    d3dr_target_t* t = NULL;
    dd_device_t* d = front->device;
    unsigned long selfmod_now;
    unsigned acts;
    int exact;
    double now;
    if (!R.hardware) return 0;
    settle_deferred();
    for (dd_surface_t* m = front->next; m && m != front; m = m->next)
        if (m->target) t = m->target;
    if (!t || !t->device || !d || !d->window || !target_ready(t)) return 0;
    if (R.batch_dev == t->device) flush();
    exact = t->sync.exact;
    acts = tsync_present(&t->sync);
    if (!tsync_presents_gpu(&t->sync)) {
        /* The CPU copy is the picture: DirectDraw presents the buffer this Flip brings to the front. */
        front->front_from_gpu = 0;
        return 0;
    }
    apply_sync(t, acts, NULL, READBACK_OTHER, exact);
    resolved_view(t);
    keep_last_frame(t);
    present_view(d->window, t->single_srv, t->width, t->height, R.point_clamp, &d->gamma, vsync);
    d->gamma_changed = 0;
    front->front_from_gpu = !(t->sync.valid & TSYNC_CPU);
    tsync_flipped(&t->sync);
    t->frame_started = 0;
    memset(&t->dirty, 0, sizeof t->dirty);
    selfmod_now = selfmod_entries();
    R.selfmod_hw += selfmod_now - R.selfmod_mark;
    R.selfmod_mark = selfmod_now;
    R.frames_hw++;
    now = now_seconds();
    if (!R.first_frame_logged) {
        R.first_frame_logged = 1;
        host_log("renderer", "first hardware frame presented: %llu draw calls, %llu triangles, %llu textures bound so far",
                 R.draw_calls, R.triangles, R.textures_bound);
    } else if (host_perf_logging() && now - R.last_summary >= SUMMARY_INTERVAL) {
        R.last_summary = now;
        log_metric_lines("summary:");
        log_state_classes();
    }
    return 1;
}

void d3dr_present_dib(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                      const DDGAMMARAMP* gamma, int vsync) {
    UINT w = (UINT)(source->right - source->left), h = (UINT)(source->bottom - source->top), out_w, out_h;
    unsigned long selfmod_now = selfmod_entries();
    if (!ensure_device() || format->dwRGBBitCount != 16 || !w || !h) return;
    R.selfmod_mark = selfmod_now;
    if (video_hd_present(window, bits, pitch, source, format->dwRBitMask == 0xF800, (const uint16_t*)gamma)) {
        R.bound.valid = 0;
        return;
    }
    if (!R.screen || R.screen_w != w || R.screen_h != h) {
        SAFE_RELEASE(R.screen_srv);
        SAFE_RELEASE(R.screen);
        free(R.screen_scratch);
        R.screen_scratch = (uint32_t*)malloc((size_t)w * h * 4);
        R.screen = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                                D3D11_CPU_ACCESS_WRITE, 1, 0);
        if (!R.screen || !R.screen_scratch ||
            FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)R.screen, NULL, &R.screen_srv))) {
            SAFE_RELEASE(R.screen);
            return;
        }
        R.screen_w = w;
        R.screen_h = h;
    }
    for (UINT y = 0; y < h; y++) {
        const WORD* in = (const WORD*)(bits + (size_t)(source->top + (LONG)y) * pitch) + source->left;
        uint32_t* out = R.screen_scratch + (size_t)y * w;
        if (format->dwRBitMask == 0xF800)
            for (UINT x = 0; x < w; x++) out[x] = R.rgb565_table[in[x]];
        else
            d3dt_convert_row(D3DT_FMT_XRGB1555, in, out, w, 0, 0, 0);
    }
    upload_picture(R.screen, R.screen_scratch, w, h);
    choose_size(w, h, &out_w, &out_h);
    if (present_view(window, R.screen_srv, out_w, out_h, R.linear_clamp, gamma, vsync)) R.frames_dib++;
}
