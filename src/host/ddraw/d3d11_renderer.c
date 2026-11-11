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
#include "page_guard.h"
#include "target_sync.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

#define VB_INITIAL (4u << 20)
#define IB_INITIAL (2u << 20)
#define SUMMARY_INTERVAL 60.0
#define PERF_INTERVAL 5.0
#define CALIBRATION_CALLER 0x00513660u
#define MAX_BIAS_STEPS 17
#define MAX_STATE_CLASSES 48
#define MAX_READBACK_CALLERS 24
#define MAX_UNKNOWN_FORMATS 16
#define MAX_DARK_SAMPLES 16
/* CRasterVid::Lock and CRasterWin::Lock (RTTI vtables 0x0064DAE4 / 0x0064DB54): every CPU access to
 * the game's rasters goes through them, so a readback is charged to the function that called them. */
#define RASTER_LOCK 0x00503400u
#define RASTER_WIN_LOCK 0x00504D20u
#define RASTER_WIN_LOCK_THUNK 0x00504D40u
#define UNTRACKED_LOG_LIMIT 8
#define BLACK_WRITE_REPORT 256
#define BLACK_WRITE_LOG_LIMIT 8
#define WATCH_PAGES 1024
#define STATE_TEX_NONE 0u
#define STATE_TEX_UNUSABLE 0xFu

enum { PERF_TRANSLATE, PERF_DRAW, PERF_UPLOAD, PERF_READBACK, PERF_OVERLAY, PERF_PRESENT, PERF_COUNT };

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
    unsigned long long draws, triangles, target_accesses, composites, composited_pixels, composited_black, uploads;
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

typedef struct d3dr_texture {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    UINT width, height, levels;
    int format;
    unsigned generation;
    size_t bytes;
    uint32_t palette_sum;
    unsigned long untracked;
    unsigned long uploads;
    int black;
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
    int frame_started;
};

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);

static struct {
    int requested, hardware, prepared, prepare_failed;
    int filter;
    unsigned msaa_request, fixed_w, fixed_h;
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
    ID3D11BlendState *blend_overlay, *blend_none, *blend_nowrite;
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
    unsigned long long dark_draws[4];
    uint32_t dark_keys[MAX_DARK_SAMPLES];
    unsigned dark_samples;
    unsigned long long draw_sequence;
    int generate_mips;
    unsigned devices, textures_alive;
    unsigned long long untracked_writes;
    unsigned untracked_logged, black_writes_logged;
    /* [0] menus, in-game dialogs, videos and loading, [1] gameplay */
    unsigned long long cpu_pixels[2], black_over_gpu[2], black_over_unknown[2];
    uint32_t target_writer;
    cpu_access_t target_access;
    pending_switch_t pending;
    int debug_output;
    unsigned char sampler_logged[2][2][5][5];
    uint32_t* upload_buffer;
    size_t upload_capacity;
    d3dr_target_t* deferred;
} R;

static const char* const k_filter_names[] = { "point", "bilinear", "trilinear", "anisotropic" };

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

void d3dr_configure(int hardware, unsigned width, unsigned height, int filter, unsigned msaa) {
    R.requested = hardware;
    R.fixed_w = width;
    R.fixed_h = height;
    R.filter = filter;
    R.msaa_request = msaa;
}

int d3dr_hardware(void) { return R.hardware; }

unsigned d3dr_device_count(void) { return R.devices; }

void d3dr_set_generate_mips(int on) { R.generate_mips = on; }

void d3dr_defer_switch(int hardware) { pending_switch_set(&R.pending, hardware); }

void d3dr_defer_switch_cancel(void) { pending_switch_cancel(&R.pending); }

unsigned d3dr_textures_alive(void) { return R.textures_alive; }

int d3dr_pending_switch(void) { return pending_switch_peek(&R.pending); }

void d3dr_directdraw_created(void) {
    int to_hardware = pending_switch_take(&R.pending);
    if (to_hardware < 0) return;
    host_log("renderer", "the game is restarting DirectDraw (its renderer change point): the switch to %s waiting since "
             "F5 applies now", to_hardware ? "D3D11" : "Software");
    if (!d3dr_set_hardware(to_hardware)) host_log("renderer", "the switch failed; the renderer stays %s", d3dr_name());
}

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
        hr = ID3D11Device_CreateInputLayout(R.dev, elements, 4, ID3D10Blob_GetBufferPointer(R.code[D3D11_SHADER_VS_TL]),
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

static void log_renderer_line(void) {
    if (!R.hardware) {
        host_log("renderer", "Renderer: Software RGB565: SelfMod rasterises, D3D11 presents");
        return;
    }
    host_log("renderer", "Renderer: D3D11 hardware (Direct3D 6 translated: IDirect3D3/Device3/Viewport3/Texture2)");
    if (R.fixed_w)
        host_log("renderer", "render resolution %ux%u, texture filter %s, MSAA requested %ux", R.fixed_w, R.fixed_h,
                 k_filter_names[R.filter], R.msaa_request);
    else
        host_log("renderer", "render resolution auto (4:3 covering the presentation area), texture filter %s, MSAA "
                 "requested %ux", k_filter_names[R.filter], R.msaa_request);
}

int d3dr_startup(void) {
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

/* Called by the window host between the game's own display release and rebuild (F5). */
int d3dr_set_hardware(int hardware) {
    if (hardware == R.hardware) return 1;
    if (hardware && !prepare_hardware()) return 0;
    if (R.nindices) flush();
    R.nverts = R.nindices = 0;
    R.batch_dev = NULL;
    R.hardware = hardware;
    R.draw_cb_valid = R.frame_cb_valid = 0;
    log_renderer_line();
    return 1;
}

/* ---- per-surface GPU objects ---- */

static void release_texture(dd_surface_t* s) {
    d3dr_texture_t* t = (d3dr_texture_t*)s->gpu_texture;
    if (!t) return;
    SAFE_RELEASE(t->srv);
    SAFE_RELEASE(t->tex);
    R.texture_memory -= t->bytes;
    R.textures_alive--;
    free(t);
    s->gpu_texture = NULL;
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
        *w = R.fixed_w;
        *h = R.fixed_h;
        return;
    }
    window_host_view_size(&view_w, &view_h);
    if (view_w <= 0 || view_h <= 0) {
        view_w = (int)legacy_w;
        view_h = (int)legacy_h;
    }
    d3dt_auto_resolution((unsigned)view_w, (unsigned)view_h, legacy_w, legacy_h, w, h);
}

static int create_target_resources(d3dr_target_t* t) {
    float clear_color[4] = { 0, 0, 0, 1 };
    UINT samples = supported_samples();
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
    memset(&t->dirty, 0, sizeof t->dirty);
    host_log("d3d11", "render target %ux%u B8G8R8A8 for the game's %ux%u back buffer, %ux MSAA%s", w, h, t->legacy_w,
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
    host_log("d3d", "Direct3D device #%u: %lux%lu target at %ux%u, %ux MSAA, %u textures alive, texture filter %s, "
             "mipmaps %s", R.devices, s->width, s->height, t->width, t->height, t->samples, R.textures_alive,
             k_filter_names[R.filter], R.generate_mips ? "generated" : "none (as the game's textures)");
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
    release_target_resources(t);
    if (t->surface) t->surface->target = NULL;
    free(t->reference);
    free(t->scratch);
    free(t);
    dev->backend = NULL;
}

void d3dr_surface_freed(dd_surface_t* s) {
    if (s->gpu_texture) release_texture(s);
    if (s->target) {
        drop_deferred(s->target);
        s->target->surface = NULL;
    }
}

void d3dr_device_lost(void) {
    drop_deferred(NULL);
    R.nverts = R.nindices = 0;
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
    charge_caller(R.perf.callers, readback_caller(), reason, ms, bytes);
}

/* The GPU picture at the legacy size, packed to RGB565 on the GPU. Point sampling returns exactly the
 * pixels the CPU composited; the pixel calibration keeps the filtered edges it measures. Mapping waits
 * for the GPU, which is what makes reading the target expensive. */
static int map_target(d3dr_target_t* t, int filtered, D3D11_MAPPED_SUBRESOURCE* m) {
    ID3D11ShaderResourceView* srv = resolved_view(t);
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->packed_rtv, NULL);
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PACK565], srv, filtered ? R.linear_clamp : R.point_clamp, R.blend_none, NULL,
              t->legacy_w, t->legacy_h, NULL);
    unbind_views();
    ID3D11DeviceContext_CopyResource(R.ctx, (ID3D11Resource*)t->staging, (ID3D11Resource*)t->packed);
    return SUCCEEDED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)t->staging, 0, D3D11_MAP_READ, 0, m));
}

static d3dt_rect_t whole_target(const d3dr_target_t* t) {
    d3dt_rect_t r = { 0, 0, (long)t->legacy_w, (long)t->legacy_h };
    return r;
}

static void readback(d3dr_target_t* t, readback_reason_t reason) {
    D3D11_MAPPED_SUBRESOURCE m;
    dd_surface_t* s = t->surface;
    int64_t start = ticks();
    int filtered = reason == READBACK_PIXEL_CALIBRATION && (t->width != t->legacy_w || t->height != t->legacy_h);
    if (!target_usable(t) || !map_target(t, filtered, &m)) return;
    for (UINT y = 0; y < t->legacy_h; y++) {
        const uint16_t* in = (const uint16_t*)((const BYTE*)m.pData + (size_t)y * m.RowPitch);
        memcpy(s->bits + (size_t)y * s->pitch, in, (size_t)t->legacy_w * 2);
        memcpy(t->reference + (size_t)y * t->legacy_w, in, (size_t)t->legacy_w * 2);
    }
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)t->staging, 0);
    note_readback(reason, elapsed_ms(start), (size_t)t->legacy_w * t->legacy_h * 2);
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
    set_pass(rect, uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_COPY], t->picture_srv, opaque ? R.linear_clamp : R.point_clamp,
              opaque ? R.blend_none : R.blend_overlay, NULL, t->width, t->height, NULL);
    unbind_views();
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

static const char* access_name(cpu_access_t access) {
    switch (access) {
    case CPU_ACCESS_LOCK: return "Lock";
    case CPU_ACCESS_GETDC: return "GetDC";
    case CPU_ACCESS_BLT_SOURCE: return "Blt source";
    case CPU_ACCESS_BLT_DEST: return "Blt";
    default: return "other";
    }
}

static int gameplay_now(void) {
    guest_ui_view_t v;
    guest_ui_probe(&v);
    return v.ui == GUEST_UI_GAMEPLAY;
}

/* Pixels the CPU composited, by screen kind. Black ones are reported when the CPU copy held the GPU
 * picture before the writes: only then is "black over the GPU picture" a fact. */
static void note_cpu_pixels(const d3dt_rect_t* box, const tsync_counts_t* c, int exact) {
    int gameplay = gameplay_now();
    R.cpu_pixels[gameplay] += c->changed;
    if (exact) R.black_over_gpu[gameplay] += c->black;
    else R.black_over_unknown[gameplay] += c->black;
    if (!exact || c->black < BLACK_WRITE_REPORT || R.black_writes_logged >= BLACK_WRITE_LOG_LIMIT) return;
    R.black_writes_logged++;
    host_log("d3d", "%s: the CPU wrote %lu black pixels over non-black GPU pixels in (%ld,%ld)-(%ld,%ld) (%lu pixels "
             "changed), writer sub_%08X by %s", gameplay ? "gameplay" : "menu", c->black, box->left, box->top,
             box->right, box->bottom, c->changed, R.target_writer, access_name(R.target_access));
}

/* CPU -> GPU: only pixels that differ from the reference, uploaded as the box that holds them. */
static void composite(d3dr_target_t* t, int exact) {
    dd_surface_t* s = t->surface;
    d3dt_rect_t region = dirty_region(t), box;
    tsync_counts_t counts = { 0, 0 };
    int64_t start = ticks();
    memset(&t->dirty, 0, sizeof t->dirty);
    if (!target_usable(t)) return;
    if (tsync_changed_box(s->bits, s->pitch, t->reference, t->legacy_w, &region, &box)) {
        UINT bw = (UINT)(box.right - box.left);
        D3D11_BOX upload = { (UINT)box.left, (UINT)box.top, 0, (UINT)box.right, (UINT)box.bottom, 1 };
        tsync_overlay_box(s->bits, s->pitch, t->reference, t->legacy_w, &box, t->scratch, &counts);
        ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)t->picture, 0, &upload, t->scratch, bw * 4, 0);
        draw_picture_box(t, &box, 0);
        R.overlays++;
        R.perf.composites++;
        R.perf.composited_pixels += counts.changed;
        R.perf.composited_black += counts.black;
        note_cpu_pixels(&box, &counts, exact);
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
    ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)t->picture, 0, NULL, t->scratch, t->legacy_w * 4, 0);
    draw_picture_box(t, &all, 1);
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
        tsync_fill(s->bits, s->pitch, t->reference, t->legacy_w, &all, t->sync.color);
        R.perf.synthesized++;
    }
    if (acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL)) {
        if (!(acts & TSYNC_CAPTURE_ALL) && region) {
            r = *region;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > all.right) r.right = all.right;
            if (r.bottom > all.bottom) r.bottom = all.bottom;
        }
        tsync_fill(s->bits, s->pitch, t->reference, t->legacy_w, &r, TSYNC_UNWRITTEN);
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

/* The game's point filtering stays point; where it asks for linear the configured quality applies. */
static ID3D11SamplerState* sampler_for(const d3d_device_t* dev) {
    int mag = d3dt_mag_linear(dev->stage[0][D3DTSS_MAGFILTER]) == 1 && R.filter != D3DR_FILTER_POINT;
    int min = d3dt_min_linear(dev->stage[0][D3DTSS_MINFILTER]) == 1 && R.filter != D3DR_FILTER_POINT;
    int u = d3dt_address(dev->stage[0][D3DTSS_ADDRESSU]), v = d3dt_address(dev->stage[0][D3DTSS_ADDRESSV]);
    ID3D11SamplerState** slot;
    if (!u) u = D3D11_TEXTURE_ADDRESS_WRAP;
    if (!v) v = D3D11_TEXTURE_ADDRESS_WRAP;
    slot = &R.sampler[mag][min][u][v];
    if (!*slot) {
        int mips = R.generate_mips && R.filter >= D3DR_FILTER_TRILINEAR;
        D3D11_FILTER f;
        UINT anisotropy = 1;
        if (mag && min && R.filter == D3DR_FILTER_ANISOTROPIC) {
            f = D3D11_FILTER_ANISOTROPIC;
            anisotropy = 16;
        } else if (mag && min) {
            f = mips ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        } else if (mag) {
            f = mips ? D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR : D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
        } else if (min) {
            f = mips ? D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR : D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
        } else {
            f = D3D11_FILTER_MIN_MAG_MIP_POINT;
        }
        *slot = make_sampler(f, u, v, (mag || min) ? D3D11_FLOAT32_MAX : 0.0f, anisotropy);
    }
    if (*slot && !R.sampler_logged[mag][min][u][v]) {
        D3D11_SAMPLER_DESC d;
        R.sampler_logged[mag][min][u][v] = 1;
        ID3D11SamplerState_GetDesc(*slot, &d);
        host_log("d3d11", "sampler bound for the game's MAG %lu MIN %lu MIP %lu, address %lu/%lu: D3D11 filter 0x%X, "
                 "MaxAnisotropy %u, address %d/%d", dev->stage[0][D3DTSS_MAGFILTER], dev->stage[0][D3DTSS_MINFILTER],
                 dev->stage[0][D3DTSS_MIPFILTER], dev->stage[0][D3DTSS_ADDRESSU], dev->stage[0][D3DTSS_ADDRESSV],
                 (unsigned)d.Filter, d.MaxAnisotropy, (int)d.AddressU, (int)d.AddressV);
    }
    return *slot;
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

static UINT mip_levels(UINT w, UINT h) {
    UINT levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        levels++;
    }
    return levels;
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
        int mips = R.generate_mips && R.filter >= D3DR_FILTER_TRILINEAR && s->width > 1 && s->height > 1;
        UINT levels = mips ? mip_levels(s->width, s->height) : 1;
        t = (d3dr_texture_t*)calloc(1, sizeof *t);
        if (!t) return NULL;
        t->tex = make_texture(s->width, s->height, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0), 0, levels,
                              mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0);
        if (!t->tex || FAILED(ID3D11Device_CreateShaderResourceView(R.dev, (ID3D11Resource*)t->tex, NULL, &t->srv))) {
            SAFE_RELEASE(t->tex);
            free(t);
            return NULL;
        }
        t->width = s->width;
        t->height = s->height;
        t->levels = levels;
        t->format = format;
        t->generation = R.generation;
        t->bytes = (size_t)s->width * s->height * 4 * (levels > 1 ? 4 : 3) / 3;
        t->palette_sum = palette_sum;
        R.texture_memory += t->bytes;
        R.textures_alive++;
        s->gpu_texture = t;
        if (s->watched) collect_written_rows(s, NULL);
        s->texture_dirty = full;
    }
    if (s->watched) collect_written_rows(s, t);
    if (format == D3DT_FMT_PAL8 && t->palette_sum != palette_sum) {
        t->palette_sum = palette_sum;
        s->texture_dirty = full;
    }
    if (d3dt_rect_empty(&s->texture_dirty)) return t;
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
    rows = upload_rows((size_t)(box.right - box.left) * (box.bottom - box.top));
    if (rows) {
        UINT w = box.right - box.left;
        int bytes = d3dt_format_bytes(format), black = 1;
        for (UINT y = box.top; y < box.bottom; y++) {
            const BYTE* in = s->bits + (size_t)y * s->pitch + (size_t)box.left * bytes;
            uint32_t* out = rows + (size_t)(y - box.top) * w;
            if (format == D3DT_FMT_PAL8)
                d3dt_convert_row_pal8(in, out, w, palette, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                                      s->src_key.dwColorSpaceHighValue);
            else
                d3dt_convert_row(format, in, out, w, s->has_src_key, s->src_key.dwColorSpaceLowValue,
                                 s->src_key.dwColorSpaceHighValue);
            if (black) black = d3dt_row_black(out, w);
        }
        if (box.left == 0 && box.top == 0 && box.right == s->width && box.bottom == s->height) t->black = black;
        else if (!black) t->black = 0;
        t->uploads++;
        ID3D11DeviceContext_UpdateSubresource(R.ctx, (ID3D11Resource*)t->tex, 0, &box, rows, w * 4, 0);
        if (t->levels > 1) ID3D11DeviceContext_GenerateMips(R.ctx, t->srv);
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

enum { DARK_TEXTURE_BLACK, DARK_TEXTURE_UNUSABLE, DARK_DIFFUSE_BLACK, DARK_FOG_BLACK };

static const char* const k_dark_reasons[] = { "texture content is black", "texture format unusable (drawn untextured)",
                                              "vertex colour black", "fully fogged to a black fog colour" };

static int batch_rgb_zero(int specular_alpha) {
    for (UINT i = 0; i < R.nverts; i++) {
        uint32_t c = specular_alpha ? (R.verts[i].specular >> 24) : (R.verts[i].diffuse & 0xFFFFFFu);
        if (c) return 0;
    }
    return R.nverts != 0;
}

/* Draws whose pixels can only come out black: a limited sample with what decided it. */
static void sample_dark_draw(const d3d_device_t* dev, const d3dr_texture_t* tex, uint32_t key) {
    const DWORD* r = dev->render;
    DWORD blend = r[D3DRENDERSTATE_TEXTUREMAPBLEND];
    int reason = -1;
    int uses_diffuse = !tex || !(blend == 1 || blend == 5 || blend == 7);
    if (tex && tex->black) reason = DARK_TEXTURE_BLACK;
    else if (!tex && dev->texture) reason = DARK_TEXTURE_UNUSABLE;
    else if (uses_diffuse && !r[D3DRENDERSTATE_SPECULARENABLE] && batch_rgb_zero(0)) reason = DARK_DIFFUSE_BLACK;
    else if (r[D3DRENDERSTATE_FOGENABLE] && !(r[D3DRENDERSTATE_FOGCOLOR] & 0xFFFFFFu) && batch_rgb_zero(1))
        reason = DARK_FOG_BLACK;
    if (reason < 0) return;
    R.dark_draws[reason]++;
    for (unsigned i = 0; i < R.dark_samples; i++)
        if (R.dark_keys[i] == (key ^ ((uint32_t)reason << 28))) return;
    if (R.dark_samples == MAX_DARK_SAMPLES) return;
    R.dark_keys[R.dark_samples++] = key ^ ((uint32_t)reason << 28);
    host_log("d3d", "dark draw #%llu: %s; texture %s (%s %lux%lu, %lu CPU writes, %lu uploads), blend %s %lu/%lu, z %s%s "
             "func %lu, zbias %lu, tmb %lu, fog %s 0x%06lX, specular %s, colour key %s, diffuse0 0x%08X, specular0 0x%08X, "
             "%u vertices, sz %.5f, from sub_%08X",
             R.draw_sequence, k_dark_reasons[reason], dev->texture ? "set" : "none",
             dev->texture ? d3dt_format_name(pixelformat_texture_format(&dev->texture->format)) : "-",
             dev->texture ? dev->texture->width : 0, dev->texture ? dev->texture->height : 0,
             dev->texture ? dev->texture->cpu_writes : 0, tex ? tex->uploads : 0,
             r[D3DRENDERSTATE_ALPHABLENDENABLE] ? "on" : "off", r[D3DRENDERSTATE_SRCBLEND], r[D3DRENDERSTATE_DESTBLEND],
             r[D3DRENDERSTATE_ZENABLE] ? "on" : "off", r[D3DRENDERSTATE_ZWRITEENABLE] ? "+write" : "", r[D3DRENDERSTATE_ZFUNC],
             r[D3DRENDERSTATE_ZBIAS], blend, r[D3DRENDERSTATE_FOGENABLE] ? "on" : "off",
             r[D3DRENDERSTATE_FOGCOLOR] & 0xFFFFFFu, r[D3DRENDERSTATE_SPECULARENABLE] ? "on" : "off",
             r[D3DRENDERSTATE_COLORKEYENABLE] ? "on" : "off", R.nverts ? R.verts[0].diffuse : 0,
             R.nverts ? R.verts[0].specular : 0, R.nverts, R.nverts ? R.verts[0].z : 0.0f, g_cur_func);
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

static void flush(void) {
    d3d_device_t* dev = R.batch_dev;
    d3dr_target_t* t = dev ? dev->backend : NULL;
    draw_constants_t dc;
    d3dr_texture_t* tex = NULL;
    UINT vb_offset = 0, ib_offset = 0, stride = sizeof(tl_vertex_t);
    D3D11_VIEWPORT vp;
    D3D11_RECT scissor;
    ID3D11SamplerState* sampler = NULL;
    ID3D11ShaderResourceView* srv = NULL;
    float factor[4] = { 0, 0, 0, 0 };
    int has_depth;
    int64_t start;
    if (!R.nindices || !t || !t->surface || !target_ready(t)) {
        R.nverts = R.nindices = 0;
        return;
    }
    start = ticks();
    if (dev->texture) {
        tex = texture_for(dev->texture);
        if (tex) {
            srv = tex->srv;
            sampler = sampler_for(dev);
            R.textures_bound++;
        }
    }
    memset(&dc, 0, sizeof dc);
    if (tex) {
        dc.flags |= D3D11_PS_TEXTURED;
        if (d3dt_format_has_alpha(tex->format)) dc.flags |= D3D11_PS_TEX_ALPHA;
        if (dev->render[D3DRENDERSTATE_COLORKEYENABLE] && dev->texture->has_src_key) dc.flags |= D3D11_PS_KEY_TEST;
    }
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
        R.nverts = R.nindices = 0;
        return;
    }
    has_depth = dev->target->zbuffer != NULL;
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)t->width;
    vp.Height = (float)t->height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    scissor = viewport_scissor(dev, t);
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &t->rtv, has_depth ? t->dsv : NULL);
    ID3D11DeviceContext_IASetInputLayout(R.ctx, R.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(R.ctx, R.batch_topology);
    ID3D11DeviceContext_IASetVertexBuffers(R.ctx, 0, 1, &R.vb, &stride, &vb_offset);
    ID3D11DeviceContext_IASetIndexBuffer(R.ctx, R.ib, DXGI_FORMAT_R32_UINT, ib_offset);
    ID3D11DeviceContext_VSSetShader(R.ctx, R.vs_tl, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(R.ctx, 0, 1, &R.cb_frame);
    ID3D11DeviceContext_PSSetShader(R.ctx, R.ps[D3D11_SHADER_PS_FIXED], NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(R.ctx, 1, 1, &R.cb_draw);
    ID3D11DeviceContext_PSSetShaderResources(R.ctx, 0, 1, &srv);
    if (sampler) ID3D11DeviceContext_PSSetSamplers(R.ctx, 0, 1, &sampler);
    ID3D11DeviceContext_RSSetState(R.ctx, raster_for(dev, t->samples > 1));
    ID3D11DeviceContext_RSSetViewports(R.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(R.ctx, 1, &scissor);
    ID3D11DeviceContext_OMSetBlendState(R.ctx, blend_for(dev), factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(R.ctx, depth_for(dev, has_depth), 0);
    ID3D11DeviceContext_DrawIndexed(R.ctx, R.nindices, 0, 0);
    if (has_depth && dev->render[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE && !R.depth_active) {
        R.depth_active = 1;
        host_log("renderer", "depth_buffer_active = true (z-tested draws into the %ux%u depth buffer)", t->width, t->height);
    }
    R.draw_calls++;
    R.perf.draws++;
    R.draw_sequence++;
    R.vertices += R.nverts;
    R.index_count += R.nindices;
    if (R.batch_topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        R.triangles += R.nindices / 3;
        R.perf.triangles += R.nindices / 3;
    }
    {
        uint32_t key = state_key(dev, tex ? (uint32_t)tex->format : dev->texture ? STATE_TEX_UNUSABLE : STATE_TEX_NONE);
        count_state_class(key, R.batch_topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ? R.nindices / 3 : 0);
        sample_dark_draw(dev, tex, key);
    }
    R.nverts = R.nindices = 0;
    perf_add(PERF_DRAW, start);
}

void d3dr_state_will_change(d3d_device_t* dev) {
    if (R.batch_dev == dev && R.nindices) flush();
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
    UINT base, n = 0;
    const BYTE* src = (const BYTE*)vertices;
    int64_t start = ticks();
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
    if (R.batch_dev != dev || R.batch_topology != topology) {
        flush();
        R.batch_dev = dev;
        R.batch_topology = topology;
    }
    gpu_write(t);
    if (!ensure_capacity(count, (indices ? index_count : count) * 3)) return DDERR_OUTOFMEMORY;
    base = R.nverts;
    for (DWORD i = 0; i < count; i++, src += stride) {
        tl_vertex_t* v = &R.verts[base + i];
        memcpy(&v->x, src, 16);
        v->diffuse = diffuse ? *(const uint32_t*)(src + diffuse) : 0xFFFFFFFFu;
        v->specular = specular ? *(const uint32_t*)(src + specular) : 0xFF000000u;
        if (uv) memcpy(&v->u, src + uv, 8);
        else v->u = v->v = 0.0f;
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
    perf_add(PERF_TRANSLATE, start);
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
        if ((flags & D3DCLEAR_TARGET) && covers_surface(t, x, y, w, h)) tsync_gpu_clear(&t->sync, d3dt_bgra_to_rgb565(color));
        else if (flags & D3DCLEAR_TARGET) gpu_write(t);
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
    if (R.batch_dev == dev) flush();
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
        R.front_down = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 1, 0);
        R.front_staging = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 1, 0);
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
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_COPY], R.last_frame_srv, R.linear_clamp, R.blend_none, NULL, w, h, NULL);
    unbind_views();
    ID3D11DeviceContext_CopyResource(R.ctx, (ID3D11Resource*)R.front_staging, (ID3D11Resource*)R.front_down);
    if (FAILED(ID3D11DeviceContext_Map(R.ctx, (ID3D11Resource*)R.front_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    for (UINT y = 0; y < h; y++) {
        const uint32_t* in = (const uint32_t*)((const BYTE*)m.pData + (size_t)y * m.RowPitch);
        uint16_t* out = (uint16_t*)(s->bits + (size_t)y * s->pitch);
        for (UINT x = 0; x < w; x++) out[x] = d3dt_bgra_to_rgb565(in[x]);
    }
    ID3D11DeviceContext_Unmap(R.ctx, (ID3D11Resource*)R.front_staging, 0);
    note_readback(READBACK_FRONT_BUFFER, elapsed_ms(start), (size_t)w * h * 4);
}

/* ---- Locks that wait for the first touch ----
 * DDLOCK_WAIT alone does not say whether the game will read, write or only wait for the GPU, so the
 * CPU copy of a locked render target is made inaccessible instead: its first touch faults, and only
 * then does the Lock read back, capture or recreate the picture, exactly as it would have at Lock time.
 * A Lock that ends untouched was synchronisation only and leaves the target as it was. */

/* DEBUG_SAULO: the first deferral decisions and how their Locks ended. */
#define DEFER_LOG_LIMIT 5
static unsigned s_defer_logged;
static int s_defer_log_open;

static void log_defer(const dd_surface_t* s, const char* outcome, int open) {
    MEMORY_BASIC_INFORMATION mbi;
    if (s_defer_logged >= DEFER_LOG_LIMIT) return;
    s_defer_logged++;
    s_defer_log_open = open;
    memset(&mbi, 0, sizeof mbi);
    VirtualQuery(s->bits, &mbi, sizeof mbi);
    host_log("d3d", "DEBUG_SAULO Lock #%u: surface 0x%08X caps 0x%08lX, own section %s, bits 0x%08X, allocation 0x%08X "
             "size 0x%lX type 0x%lX protection 0x%lX, %ld bytes, flags 0x%lX, by sub_%08X: %s", s_defer_logged,
             (uint32_t)(uintptr_t)s, s->caps, s->section ? "yes" : "no", (uint32_t)(uintptr_t)s->bits,
             (uint32_t)(uintptr_t)mbi.AllocationBase, (unsigned long)mbi.RegionSize, mbi.Type, mbi.Protect,
             s->pitch * (long)s->height, s->lock_flags, readback_caller(), outcome);
}

static void log_unlock(int touched) {
    if (!s_defer_log_open) return;
    s_defer_log_open = 0;
    host_log("d3d", "DEBUG_SAULO Lock #%u ended %s", s_defer_logged, touched ? "touched" : "untouched");
}

static void deferred_touched(void* context) {
    d3dr_target_t* t = (d3dr_target_t*)context;
    R.deferred = NULL;
    R.perf.deferred_touched++;
    log_unlock(1);
    apply_sync(t, tsync_lock_touched(&t->deferred), &t->deferred_region, t->deferred_reason, t->sync.exact);
}

/* Anything else that reads or changes the target first gives a waiting Lock the picture of its time. */
static void settle_deferred(void) {
    if (R.deferred) page_guard_touch();
}

/* 0 leaves the Lock to the immediate path. */
static int defer_lock(d3dr_target_t* t, tsync_access_t kind, const d3dt_rect_t* region, readback_reason_t reason,
                      int exact) {
    dd_surface_t* s = t->surface;
    page_guard_result_t armed;
    apply_sync(t, tsync_lock_deferred(&t->sync, kind, &t->deferred), region, reason, exact);
    if (!t->deferred.acts) {
        log_defer(s, "nothing to wait for, the CPU copy is current", 0);
        return 1;
    }
    t->deferred_region = *region;
    t->deferred_reason = reason;
    armed = page_guard_arm(s->bits, (size_t)s->pitch * s->height, deferred_touched, t);
    log_defer(s, page_guard_result_name(armed), armed == PAGE_GUARD_ARMED);
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
    if (!t || R.deferred != t || !page_guard_disarm()) return 0;
    R.deferred = NULL;
    tsync_lock_untouched(&t->sync, &t->deferred);
    R.perf.deferred_untouched++;
    log_unlock(0);
    return 1;
}

/* A Lock's target going away: its pages become ordinary memory again, nothing is read. */
static void drop_deferred(const d3dr_target_t* t) {
    if (!R.deferred || (t && R.deferred != t)) return;
    page_guard_disarm();
    tsync_lock_untouched(&R.deferred->sync, &R.deferred->deferred);
    R.deferred = NULL;
}

/* Before the game touches a render target's pixels: writes still pending reach the GPU first, then
 * the CPU copy becomes the GPU picture only if this access reads it. */
static void target_cpu_access(d3dr_target_t* t, int will_write, cpu_access_t access) {
    dd_surface_t* s = t->surface;
    uint32_t caller = readback_caller();
    int reads = target_access_reads(access, s->lock_flags, caller), exact;
    tsync_access_t kind = reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ;
    readback_reason_t reason = readback_classify(access, !will_write, g_cur_func, CALIBRATION_CALLER);
    d3dt_rect_t region = { 0, 0, (long)s->width, (long)s->height };
    settle_deferred();
    if (R.batch_dev == t->device) flush();
    if (!target_ready(t)) return;
    if (access == CPU_ACCESS_LOCK) {
        region.left = s->lock_rect.left;
        region.top = s->lock_rect.top;
        region.right = s->lock_rect.right;
        region.bottom = s->lock_rect.bottom;
    }
    check_front(t);
    exact = t->sync.exact;
    if (access != CPU_ACCESS_LOCK || !defer_lock(t, kind, &region, reason, exact))
        apply_sync(t, tsync_cpu_access(&t->sync, kind), &region, reason, exact);
    R.perf.target_accesses++;
    if (will_write) {
        R.target_writer = caller;
        R.target_access = access;
    }
}

void d3dr_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access) {
    if (!R.hardware) return;
    if (s->target && s->target->device) target_cpu_access(s->target, will_write, access);
    if (s->front_from_gpu) {
        s->front_from_gpu = 0;
        readback_last_frame(s);
    }
    if (will_write && R.nindices && R.batch_dev && R.batch_dev->texture == s) flush();
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
    front_written(s);
    if (s->target) {
        d3dt_rect_t add = { 0, 0, (long)s->width, (long)s->height };
        if (r) {
            add.left = r->left;
            add.top = r->top;
            add.right = r->right;
            add.bottom = r->bottom;
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

static int present_view(HWND window, ID3D11ShaderResourceView* srv, UINT width, UINT height, ID3D11SamplerState* sampler,
                        const DDGAMMARAMP* gamma, int vsync) {
    ID3D11RenderTargetView* rtv;
    if (!window_host_frames_allowed()) return 1;
    rtv = present_target(window, width, height);
    if (!rtv) return 0;
    update_gamma(gamma);
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 1, &rtv, NULL);
    set_pass(k_full_rect, k_full_uv, NULL, 0.0f);
    draw_pass(R.ps[D3D11_SHADER_PS_PRESENT], srv, sampler, R.blend_none, NULL, width, height, NULL);
    unbind_views();
    ID3D11DeviceContext_OMSetRenderTargets(R.ctx, 0, NULL, NULL);
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

static void log_metric_lines(const char* when) {
    host_log("renderer", "%s", when);
    host_log("renderer", "frames %llu (hardware %llu, DirectDraw pictures %llu)", R.frames_hw + R.frames_dib, R.frames_hw,
             R.frames_dib);
    host_log("renderer", "hardware_draw_calls %llu", R.draw_calls);
    host_log("renderer", "hardware_triangles %llu", R.triangles);
    host_log("renderer", "vertices %llu", R.vertices);
    host_log("renderer", "indices %llu", R.index_count);
    host_log("renderer", "textures_bound %llu", R.textures_bound);
    host_log("renderer", "texture_uploads %llu", R.texture_uploads);
    host_log("renderer", "texture_memory %.1f MB", R.texture_memory / (1024.0 * 1024.0));
    host_log("renderer", "depth_buffer_active = %s", R.depth_active ? "true" : "false");
    host_log("renderer", "render-target readbacks %llu, CPU overlays %llu, clears %llu", R.readbacks, R.overlays, R.clears);
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
    host_log("perf", "%s per frame: D3D draws %.1f, triangles %.0f | CPU accesses to the target %.2f, CPU pixels "
             "composited %.0f (black %.0f) in %.2f composites, full CPU pictures uploaded %llu", when,
             p->draws / frames, p->triangles / frames, p->target_accesses / frames, p->composited_pixels / frames,
             p->composited_black / frames, p->composites / frames, p->uploads);
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
    total->target_accesses += p->target_accesses;
    total->composites += p->composites;
    total->composited_pixels += p->composited_pixels;
    total->composited_black += p->composited_black;
    total->uploads += p->uploads;
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

static void log_perf_window(double seconds) {
    perf_window_t* p = &R.perf;
    double frames = p->frames ? (double)p->frames : 1.0, host = 0.0;
    double frame_ms = seconds * 1000.0 / frames;
    for (int i = 0; i < PERF_COUNT; i++) host += p->ms[i];
    host_log("perf", "%s | FPS %.1f | frame %.1f ms | guest %.1f | translate %.2f | draws %.2f | uploads %.2f | "
             "readbacks %.2f | overlays %.2f | present %.2f ms per frame", d3dr_name(), p->frames / seconds, frame_ms,
             frame_ms - host / frames > 0.0 ? frame_ms - host / frames : 0.0, p->ms[PERF_TRANSLATE] / frames,
             p->ms[PERF_DRAW] / frames, p->ms[PERF_UPLOAD] / frames, p->ms[PERF_READBACK] / frames,
             p->ms[PERF_OVERLAY] / frames, p->ms[PERF_PRESENT] / frames);
    if (R.hardware) {
        log_readback_detail(p, "last 5 s");
        log_hybrid(p, "last 5 s");
    }
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
        log_perf_window(now - R.perf_start);
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
        log_hybrid(&R.total, "session");
        for (int i = 0; i < MAX_READBACK_CALLERS; i++)
            if (R.total.callers[i].count)
                host_log("perf", "readback caller sub_%08X %s: %llu, %.1f MB, %.1f ms", R.total.callers[i].caller,
                         readback_reason_name(R.total.callers[i].reason), R.total.callers[i].count,
                         R.total.callers[i].bytes / (1024.0 * 1024.0), R.total.callers[i].ms);
        log_state_classes();
        for (int i = 0; i < 4; i++)
            if (R.dark_draws[i]) host_log("d3d", "dark draws (%s): %llu", k_dark_reasons[i], R.dark_draws[i]);
        if (R.unknown_count) host_log("d3d", "unknown texture formats: %u (listed above when first drawn)", R.unknown_count);
        host_log("d3d", "textures changed without Lock/Unlock: %llu", R.untracked_writes);
        host_log("d3d", "CPU pixels composited over the GPU target: gameplay %llu, menus/dialogs/videos %llu; black ones "
                 "over known non-black GPU pixels: gameplay %llu, menus %llu; black ones over a capture (GPU pixels "
                 "unknown): gameplay %llu, menus %llu", R.cpu_pixels[1], R.cpu_pixels[0], R.black_over_gpu[1],
                 R.black_over_gpu[0], R.black_over_unknown[1], R.black_over_unknown[0]);
    }
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
    } else if (now - R.last_summary >= SUMMARY_INTERVAL) {
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
