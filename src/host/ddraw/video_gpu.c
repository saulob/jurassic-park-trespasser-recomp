/*
 * The video passes, at the picture's own size then at the output's: the decoder's RGB565 codes back to the palette's
 * colours, the rows of half-height files rebuilt, the codec's block edges softened where the picture is smooth, a
 * light smoothing of the palette dither, the side glow, and the Catmull-Rom scale onto the target.
 */
#define COBJMACROS
#include "video_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLOW_STEPS 32
#define GLOW_ROWS 96
#define MAX_SIDE 4096
#define SMOOTH_FULL (16.0f / 255.0f)
#define SMOOTH_NONE (28.0f / 255.0f)
#define DEBLOCK_STEP_FULL (12.0f / 255.0f)
#define DEBLOCK_STEP_NONE (32.0f / 255.0f)
#define DEBLOCK_FLAT_FULL (4.0f / 255.0f)
#define DEBLOCK_FLAT_NONE (10.0f / 255.0f)
#define GLOW_NEAR 0.5f
#define GLOW_FAR 0.12f

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);

enum { PS_CONVERT, PS_SMOOTH, PS_DEBLOCK_X, PS_DEBLOCK_Y, PS_GLOW, PS_FINAL, PS_COUNT };

static const char* const k_entries[1 + PS_COUNT] = { "vs_full",      "ps_convert", "ps_smooth", "ps_deblock_x",
                                                     "ps_deblock_y", "ps_glow",    "ps_final" };

static const char k_source[] =
    "cbuffer Video : register(b0) {\n"
    "  int4 src; int4 opt; float4 video; float4 outp; float4 tune; float4 deblock; int4 stray;\n"
    "};\n"
    "Texture2D<uint> codes : register(t0);\n"
    "Texture2D<float4> palette_map : register(t1);\n"
    "Texture2D<float4> frame : register(t2);\n"
    "Texture2D<float4> glow : register(t3);\n"
    "Texture2D<float4> gamma_lut : register(t4);\n"
    "SamplerState lin : register(s0);\n"
    "#define GLOW_STEPS 32\n"
    "#define GLOW_ROWS 96\n"
    "#define GLOW_BAND 8\n"
    "float4 vs_full(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 t = float2((id << 1) & 2, id & 2);\n"
    "  return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
    "}\n"
    "float3 expand(uint c) {\n"
    "  if (opt.z) return float3((c >> 11) & 31, (c >> 5) & 63, c & 31) / float3(31.0, 63.0, 31.0);\n"
    "  return float3((c >> 10) & 31, (c >> 5) & 31, c & 31) / 31.0;\n"
    "}\n"
    "uint code_at(int x, int y) { return codes.Load(int3(clamp(x, 0, src.x - 1), clamp(y, 0, src.y - 1), 0)); }\n"
    "float3 stored(int x, int y) {\n"
    "  uint c = code_at(x, y);\n"
    "  if (opt.y) {\n"
    "    float4 m = palette_map.Load(int3((int)(c & 255), (int)(c >> 8), 0));\n"
    "    if (m.a > 0.5) return m.rgb;\n"
    "  }\n"
    "  return expand(c);\n"
    "}\n"
    "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    /* A missing row between stored rows s and s + 1: the direction (up to two pixels either way) whose pixels agree
     * best, when it agrees clearly better than straight down; each choice averages two real pixels. */
    "float3 between(int x, int s) {\n"
    "  float vertical = 0.0, best = 1e9;\n"
    "  int pick = 0;\n"
    "  [loop] for (int k = -1; k <= 1; k++) vertical += abs(luma(stored(x + k, s)) - luma(stored(x + k, s + 1)));\n"
    "  [loop] for (int d = -2; d <= 2; d++) {\n"
    "    if (d != 0) {\n"
    "      float cost = 0.0;\n"
    "      [loop] for (int j = -1; j <= 1; j++)\n"
    "        cost += abs(luma(stored(x + d + j, s)) - luma(stored(x - d + j, s + 1)));\n"
    "      if (cost < best) { best = cost; pick = d; }\n"
    "    }\n"
    "  }\n"
    "  if (vertical < 0.06 || best >= 0.5 * vertical) pick = 0;\n"
    "  return 0.5 * (stored(x + pick, s) + stored(x - pick, s + 1));\n"
    "}\n"
    /* A stray line along the bottom edge: a lit pixel of the last row whose own codec block above it is black takes the
     * row above. */
    "int stray_row(int x, int y) {\n"
    "  if (stray.x == 0 || y != src.y - 1 || y < 3 || code_at(x, y) == 0) return y;\n"
    "  return (code_at(x, y - 1) | code_at(x, y - 2) | code_at(x, y - 3)) == 0 ? y - 1 : y;\n"
    "}\n"
    "float4 ps_convert(float4 pos : SV_Position) : SV_Target {\n"
    "  int x = (int)pos.x, y = (int)pos.y;\n"
    "  if (src.w == 0) return float4(stored(x, stray_row(x, y)), 1.0);\n"
    "  int r = y - opt.x;\n"
    "  if (r < 0) return float4(stored(x, 0), 1.0);\n"
    "  if ((r & 1) == 0) return float4(stored(x, r >> 1), 1.0);\n"
    "  return float4(between(x, r >> 1), 1.0);\n"
    "}\n"
    /* The palette's dither: a pixel above or below both neighbours of a pair by up to its step. Such pairs are averaged
     * in (fading out before real edges), ramps and edges never; the weights halve a 2x2 dither checkerboard. */
    "float smooth_weight(float3 c, float3 n) {\n"
    "  float3 e = abs(n - c);\n"
    "  return saturate((tune.w - max(e.r, max(e.g, e.b))) / (tune.w - tune.x));\n"
    "}\n"
    "float4 ps_smooth(float4 pos : SV_Position) : SV_Target {\n"
    "  static const int2 pairs[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };\n"
    "  int2 p = int2(pos.xy), last = int2(src.x - 1, src.z - 1);\n"
    "  float3 c = frame.Load(int3(p, 0)).rgb, sum = c;\n"
    "  float lc = luma(c), total = 1.0;\n"
    "  [unroll] for (int i = 0; i < 4; i++) {\n"
    "    float3 a = frame.Load(int3(clamp(p - pairs[i], int2(0, 0), last), 0)).rgb;\n"
    "    float3 b = frame.Load(int3(clamp(p + pairs[i], int2(0, 0), last), 0)).rgb;\n"
    "    float da = luma(a) - lc, db = luma(b) - lc, k = i < 2 ? 1.0 / 6.0 : 0.25;\n"
    "    bool extremum = da * db > 0.0;\n"
    "    float wa = (extremum || abs(da) < 0.002) ? smooth_weight(c, a) * k : 0.0;\n"
    "    float wb = (extremum || abs(db) < 0.002) ? smooth_weight(c, b) * k : 0.0;\n"
    "    sum += a * wa + b * wb;\n"
    "    total += wa + wb;\n"
    "  }\n"
    "  return float4(sum / total, 1.0);\n"
    "}\n"
    "float3 px(int2 p) { return frame.Load(int3(clamp(p, int2(0, 0), int2(src.x - 1, src.z - 1)), 0)).rgb; }\n"
    "float chmax(float3 v) { return max(v.r, max(v.g, v.b)); }\n"
    "float calm(float3 a, float3 b, float3 c) { return max(chmax(abs(b - a)), chmax(abs(c - b))); }\n"
    /* Smacker codes 4x4 blocks, mostly of one colour where the picture is smooth. An edge between two blocks is judged
     * over its four lines: a small step, larger than every change inside both blocks and never against black (text and
     * its background), is spread over them as between their centres; real edges and busy blocks keep theirs. */
    "float3 block_step(int2 e, int2 d) {\n"
    "  int2 along = d.yx, first = e - (dot(e, along) & 3) * along;\n"
    "  float step = 0.0, inner = 0.0, darkest = 1.0;\n"
    "  [unroll] for (int k = 0; k < 4; k++) {\n"
    "    int2 l = first + k * along;\n"
    "    float3 p0 = px(l - d), p1 = px(l - 2 * d), p2 = px(l - 3 * d);\n"
    "    float3 q0 = px(l), q1 = px(l + d), q2 = px(l + 2 * d);\n"
    "    step = max(step, chmax(abs(q0 - p0)));\n"
    "    inner = max(inner, max(calm(p0, p1, p2), calm(q0, q1, q2)));\n"
    "    darkest = min(darkest, min(chmax(p0), chmax(q0)));\n"
    "  }\n"
    "  if (darkest <= 0.0 || step <= inner) return 0.0;\n"
    "  float w = (1.0 - smoothstep(deblock.x, deblock.y, step)) * (1.0 - smoothstep(deblock.z, deblock.w, inner));\n"
    "  return (px(e) - px(e - d)) * w;\n"
    "}\n"
    "float4 deblock_along(int2 p, int2 d) {\n"
    "  int i = dot(p, d) & 3, n = dot(int2(src.x, src.z), d);\n"
    "  int2 first = p - i * d;\n"
    "  float3 c = px(p);\n"
    "  if (i < 2 && dot(first, d) > 0) c -= (i == 0 ? 0.375 : 0.125) * block_step(first, d);\n"
    "  if (i >= 2 && dot(first, d) + 4 < n) c += (i == 3 ? 0.375 : 0.125) * block_step(first + 4 * d, d);\n"
    "  return float4(c, 1.0);\n"
    "}\n"
    "float4 ps_deblock_x(float4 pos : SV_Position) : SV_Target { return deblock_along(int2(pos.xy), int2(1, 0)); }\n"
    "float4 ps_deblock_y(float4 pos : SV_Position) : SV_Target { return deblock_along(int2(pos.xy), int2(0, 1)); }\n"
    "float3 band(float row, int right) {\n"
    "  float r = clamp(row, 0.0, (float)(src.z - 1));\n"
    "  int y0 = (int)r, y1 = min(y0 + 1, src.z - 1);\n"
    "  float3 a = 0.0, b = 0.0;\n"
    "  [loop] for (int k = 0; k < GLOW_BAND; k++) {\n"
    "    int x = right ? src.x - 1 - k : k;\n"
    "    a += frame.Load(int3(x, y0, 0)).rgb;\n"
    "    b += frame.Load(int3(x, y1, 0)).rgb;\n"
    "  }\n"
    "  return lerp(a, b, r - (float)y0) / GLOW_BAND;\n"
    "}\n"
    /* Columns 0-31 the left side, 32-63 the right, from the picture's edge outwards: its edge colours, blurred more the
     * further out they reach. */
    "float4 ps_glow(float4 pos : SV_Position) : SV_Target {\n"
    "  int gx = (int)pos.x, gy = (int)pos.y;\n"
    "  int right = gx >= GLOW_STEPS ? 1 : 0;\n"
    "  float dist = ((float)(gx - right * GLOW_STEPS) + 0.5) / GLOW_STEPS;\n"
    "  float rows = (float)src.z;\n"
    "  float centre = ((float)gy + 0.5) / GLOW_ROWS * rows - 0.5;\n"
    "  float sigma = rows * (0.03 + 0.12 * dist);\n"
    "  float3 sum = 0.0;\n"
    "  float total = 0.0;\n"
    "  [loop] for (int t = -12; t <= 12; t++) {\n"
    "    float o = (float)t / 5.0, w = exp(-0.5 * o * o);\n"
    "    sum += band(centre + o * sigma, right) * w;\n"
    "    total += w;\n"
    "  }\n"
    "  return float4(sum / total, 1.0);\n"
    "}\n"
    /* Catmull-Rom, kept inside the four nearest texels so edges get no halo. */
    "float3 catmull(float2 s) {\n"
    "  float2 t = s - 0.5, f = floor(t), a = t - f;\n"
    "  int2 b = int2(f), last = int2(src.x - 1, src.z - 1);\n"
    "  float2 w0 = ((-0.5 * a + 1.0) * a - 0.5) * a, w1 = (1.5 * a - 2.5) * a * a + 1.0;\n"
    "  float2 w2 = ((-1.5 * a + 2.0) * a + 0.5) * a, w3 = (0.5 * a - 0.5) * a * a;\n"
    "  float4 wx = float4(w0.x, w1.x, w2.x, w3.x), wy = float4(w0.y, w1.y, w2.y, w3.y);\n"
    "  float3 sum = 0.0, lo = 1e9, hi = -1e9;\n"
    "  [unroll] for (int j = 0; j < 4; j++) {\n"
    "    float3 row = 0.0;\n"
    "    [unroll] for (int k = 0; k < 4; k++) {\n"
    "      float3 c = frame.Load(int3(clamp(b + int2(k - 1, j - 1), int2(0, 0), last), 0)).rgb;\n"
    "      row += c * wx[k];\n"
    "      if ((j == 1 || j == 2) && (k == 1 || k == 2)) { lo = min(lo, c); hi = max(hi, c); }\n"
    "    }\n"
    "    sum += row * wy[j];\n"
    "  }\n"
    "  return clamp(sum, lo, hi);\n"
    "}\n"
    "float3 ramp(float3 c) {\n"
    "  if (!opt.w) return c;\n"
    "  int3 k = int3(round(c * 255.0));\n"
    "  return float3(gamma_lut.Load(int3(k.r, 0, 0)).r, gamma_lut.Load(int3(k.g, 0, 0)).g,\n"
    "                gamma_lut.Load(int3(k.b, 0, 0)).b);\n"
    "}\n"
    "static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    "float4 ps_final(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = pos.xy;\n"
    "  float3 c = 0.0;\n"
    "  bool rows = p.y >= video.y && p.y < video.y + video.w;\n"
    "  if (rows && p.x >= video.x && p.x < video.x + video.z) {\n"
    "    c = catmull((p - video.xy) * float2(src.x, src.z) / video.zw);\n"
    "  } else if (rows && outp.w > 0.0) {\n"
    "    bool right = p.x >= video.x + video.z;\n"
    "    float dist = saturate((right ? p.x - video.x - video.z : video.x - p.x) / max(outp.z, 1.0));\n"
    "    float u = (right ? 0.5 : 0.0) + clamp(dist, 0.5 / GLOW_STEPS, 1.0 - 0.5 / GLOW_STEPS) * 0.5;\n"
    "    float3 g = glow.SampleLevel(lin, float2(u, (p.y - video.y) / video.w), 0).rgb;\n"
    "    int2 q = int2(p) & 3;\n"
    "    c = g * lerp(tune.y, tune.z, smoothstep(0.0, 1.0, dist)) + (bayer[q.y * 4 + q.x] / 16.0 - 0.46875) / 255.0;\n"
    "  }\n"
    "  return float4(ramp(saturate(c)), 1.0);\n"
    "}\n";

typedef struct {
    int32_t src[4], opt[4];
    float video[4], outp[4], tune[4], deblock[4];
    int32_t stray[4];
} constants_t;

typedef struct {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView* rtv;
    int w, h;
} target_t;

struct video_gpu {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps[PS_COUNT];
    ID3D11Buffer* cb;
    ID3D11SamplerState* linear;
    ID3D11RasterizerState* raster;
    ID3D11Texture2D* codes;
    ID3D11ShaderResourceView* codes_srv;
    int codes_w, codes_h;
    ID3D11Texture2D* map;
    ID3D11ShaderResourceView* map_srv;
    int map_loaded;
    ID3D11Texture2D* gamma;
    ID3D11ShaderResourceView* gamma_srv;
    uint16_t gamma_last[768];
    int gamma_valid, gamma_on;
    target_t frame, smooth, glow;
};

static ID3DBlob* s_code[1 + PS_COUNT];
static int s_compiled;
static char s_compile_error[192];
static char s_error[192];
static long s_live;

const char* video_gpu_error(void) { return s_error; }

long video_gpu_live_objects(void) { return s_live; }

static int fail(const char* what, HRESULT hr) {
    _snprintf(s_error, sizeof s_error - 1, "%s (HRESULT 0x%08lX)", what, (unsigned long)hr);
    s_error[sizeof s_error - 1] = 0;
    return 0;
}

static int counted(HRESULT hr) {
    if (SUCCEEDED(hr)) s_live++;
    return SUCCEEDED(hr);
}

static void drop(void* slot) {
    IUnknown** p = (IUnknown**)slot;
    if (!*p) return;
    IUnknown_Release(*p);
    *p = NULL;
    s_live--;
}

static void drop_target(target_t* t) {
    drop(&t->rtv);
    drop(&t->srv);
    drop(&t->tex);
    t->w = t->h = 0;
}

static BOOL CALLBACK compile_once(PINIT_ONCE once, PVOID param, PVOID* context) {
    HMODULE dll = LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    compile_fn compile;
    (void)once, (void)param, (void)context;
    if (!dll) dll = LoadLibraryA("d3dcompiler_47.dll");
    compile = dll ? (compile_fn)GetProcAddress(dll, "D3DCompile") : NULL;
    if (!compile) {
        strcpy(s_compile_error, "d3dcompiler_47.dll has no D3DCompile");
        return TRUE;
    }
    for (int i = 0; i < 1 + PS_COUNT; i++) {
        ID3DBlob* errors = NULL;
        HRESULT hr = compile(k_source, sizeof k_source - 1, "trespasser_video", NULL, NULL, k_entries[i],
                             i ? "ps_4_0" : "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &s_code[i], &errors);
        if (FAILED(hr)) {
            _snprintf(s_compile_error, sizeof s_compile_error - 1, "shader %s: %.120s", k_entries[i],
                      errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "no message");
            if (errors) ID3D10Blob_Release(errors);
            return TRUE;
        }
        if (errors) ID3D10Blob_Release(errors);
    }
    s_compiled = 1;
    return TRUE;
}

/* Once per process, whichever thread asks first; the others wait for it. */
static int compile_all(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, compile_once, NULL, NULL);
    if (!s_compiled) strcpy(s_error, s_compile_error);
    return s_compiled;
}

static DWORD WINAPI prepare_thread(LPVOID unused) {
    (void)unused;
    compile_all();
    return 0;
}

void video_gpu_prepare(void) {
    HANDLE thread = CreateThread(NULL, 0, prepare_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

static int make_texture(video_gpu_t* g, int w, int h, DXGI_FORMAT format, D3D11_USAGE usage, UINT bind,
                        ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, ID3D11RenderTargetView** rtv) {
    D3D11_TEXTURE2D_DESC d;
    HRESULT hr;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    hr = ID3D11Device_CreateTexture2D(g->dev, &d, NULL, tex);
    if (!counted(hr)) return fail("video texture", hr);
    hr = ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource*)*tex, NULL, srv);
    if (!counted(hr)) return fail("video texture view", hr);
    if (!rtv) return 1;
    hr = ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource*)*tex, NULL, rtv);
    if (!counted(hr)) return fail("video render target", hr);
    return 1;
}

static int ensure_target(video_gpu_t* g, target_t* t, int w, int h) {
    if (t->tex && t->w == w && t->h == h) return 1;
    drop_target(t);
    if (!make_texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_USAGE_DEFAULT,
                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &t->tex, &t->srv, &t->rtv)) {
        drop_target(t);
        return 0;
    }
    t->w = w;
    t->h = h;
    return 1;
}

int video_gpu_create(ID3D11Device* device, ID3D11DeviceContext* context, video_gpu_t** out) {
    video_gpu_t* g;
    D3D11_BUFFER_DESC b;
    D3D11_SAMPLER_DESC s;
    D3D11_RASTERIZER_DESC r;
    HRESULT hr;
    *out = NULL;
    s_error[0] = 0;
    if (!device || !context || !compile_all()) return 0;
    g = (video_gpu_t*)calloc(1, sizeof *g);
    if (!g) return fail("out of memory", E_OUTOFMEMORY);
    g->dev = device;
    g->ctx = context;
    ID3D11Device_AddRef(device);
    ID3D11DeviceContext_AddRef(context);
    hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(s_code[0]),
                                         ID3D10Blob_GetBufferSize(s_code[0]), NULL, &g->vs);
    if (!counted(hr)) goto failed;
    for (int i = 0; i < PS_COUNT; i++) {
        hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(s_code[1 + i]),
                                            ID3D10Blob_GetBufferSize(s_code[1 + i]), NULL, &g->ps[i]);
        if (!counted(hr)) goto failed;
    }
    memset(&b, 0, sizeof b);
    b.ByteWidth = sizeof(constants_t);
    b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(device, &b, NULL, &g->cb);
    if (!counted(hr)) goto failed;
    memset(&s, 0, sizeof s);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = ID3D11Device_CreateSamplerState(device, &s, &g->linear);
    if (!counted(hr)) goto failed;
    memset(&r, 0, sizeof r);
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(device, &r, &g->raster);
    if (!counted(hr)) goto failed;
    if (!make_texture(g, 256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, &g->map,
                      &g->map_srv, NULL) ||
        !make_texture(g, 256, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, &g->gamma,
                      &g->gamma_srv, NULL) ||
        !ensure_target(g, &g->glow, 2 * GLOW_STEPS, GLOW_ROWS)) {
        video_gpu_destroy(g);
        return 0;
    }
    *out = g;
    return 1;
failed:
    fail("video shader or state", hr);
    video_gpu_destroy(g);
    return 0;
}

void video_gpu_destroy(video_gpu_t* g) {
    if (!g) return;
    drop_target(&g->frame);
    drop_target(&g->smooth);
    drop_target(&g->glow);
    drop(&g->gamma_srv);
    drop(&g->gamma);
    drop(&g->map_srv);
    drop(&g->map);
    drop(&g->codes_srv);
    drop(&g->codes);
    drop(&g->raster);
    drop(&g->linear);
    drop(&g->cb);
    for (int i = 0; i < PS_COUNT; i++) drop(&g->ps[i]);
    drop(&g->vs);
    if (g->ctx) ID3D11DeviceContext_Release(g->ctx);
    if (g->dev) ID3D11Device_Release(g->dev);
    free(g);
}

static int upload_codes(video_gpu_t* g, const video_gpu_frame_t* f) {
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    int rows = f->policy.stored_rows;
    if (!g->codes || g->codes_w != f->width || g->codes_h != rows) {
        drop(&g->codes_srv);
        drop(&g->codes);
        g->codes_w = g->codes_h = 0;
        if (!make_texture(g, f->width, rows, DXGI_FORMAT_R16_UINT, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                          &g->codes, &g->codes_srv, NULL))
            return 0;
        g->codes_w = f->width;
        g->codes_h = rows;
    }
    hr = ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource*)g->codes, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) return fail("video frame upload", hr);
    for (int y = 0; y < rows; y++)
        memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, f->rows + (size_t)y * (size_t)f->stride, (size_t)f->width * 2);
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource*)g->codes, 0);
    return 1;
}

static void update_gamma(video_gpu_t* g, const uint16_t* ramp) {
    uint32_t lut[256];
    int identity = 1;
    if (!ramp) {
        g->gamma_on = 0;
        return;
    }
    if (g->gamma_valid && !memcmp(g->gamma_last, ramp, sizeof g->gamma_last)) return;
    for (int i = 0; i < 256; i++) {
        uint32_t r = ramp[i] >> 8, gr = ramp[256 + i] >> 8, b = ramp[512 + i] >> 8;
        lut[i] = r | gr << 8 | b << 16 | 0xFF000000u;
        if (r != (uint32_t)i || gr != (uint32_t)i || b != (uint32_t)i) identity = 0;
    }
    ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource*)g->gamma, 0, NULL, lut, sizeof lut, 0);
    memcpy(g->gamma_last, ramp, sizeof g->gamma_last);
    g->gamma_valid = 1;
    g->gamma_on = !identity;
}

static int set_constants(video_gpu_t* g, const video_gpu_frame_t* f, const vl_layout_t* l, int out_w, int out_h,
                         int glow, int progressive) {
    D3D11_MAPPED_SUBRESOURCE m;
    constants_t c;
    HRESULT hr = ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource*)g->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) return fail("video constants", hr);
    memset(&c, 0, sizeof c);
    c.src[0] = f->width;
    c.src[1] = f->policy.stored_rows;
    c.src[2] = f->policy.shown_rows;
    c.src[3] = f->policy.rows == VL_ROWS_REBUILD;
    c.opt[0] = f->policy.parity;
    c.opt[1] = f->palette_map != NULL;
    c.opt[2] = f->rgb565 != 0;
    c.opt[3] = g->gamma_on;
    c.video[0] = (float)l->video.x;
    c.video[1] = (float)l->video.y;
    c.video[2] = (float)l->video.w;
    c.video[3] = (float)l->video.h;
    c.outp[0] = (float)out_w;
    c.outp[1] = (float)out_h;
    c.outp[2] = (float)l->side;
    c.outp[3] = glow ? 1.0f : 0.0f;
    c.tune[0] = SMOOTH_FULL;
    c.tune[1] = GLOW_NEAR;
    c.tune[2] = GLOW_FAR;
    c.tune[3] = SMOOTH_NONE;
    c.deblock[0] = DEBLOCK_STEP_FULL;
    c.deblock[1] = DEBLOCK_STEP_NONE;
    c.deblock[2] = DEBLOCK_FLAT_FULL;
    c.deblock[3] = DEBLOCK_FLAT_NONE;
    c.stray[0] = progressive && f->stray_bottom_line;
    memcpy(m.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource*)g->cb, 0);
    return 1;
}

/* Nothing of the passes stays bound: the renderer rebinds its own state after them. */
static void unbind(video_gpu_t* g) {
    ID3D11ShaderResourceView* none[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11Buffer* no_buffer = NULL;
    ID3D11SamplerState* no_sampler = NULL;
    ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 5, none);
    ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 0, NULL, NULL);
    ID3D11DeviceContext_PSSetConstantBuffers(g->ctx, 0, 1, &no_buffer);
    ID3D11DeviceContext_PSSetSamplers(g->ctx, 0, 1, &no_sampler);
    ID3D11DeviceContext_PSSetShader(g->ctx, NULL, NULL, 0);
    ID3D11DeviceContext_VSSetShader(g->ctx, NULL, NULL, 0);
    ID3D11DeviceContext_RSSetState(g->ctx, NULL);
}

static void pass(video_gpu_t* g, int ps, ID3D11RenderTargetView* target, int w, int h,
                 ID3D11ShaderResourceView* const views[5]) {
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    ID3D11ShaderResourceView* none[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 5, none);
    ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 1, &target, NULL);
    ID3D11DeviceContext_RSSetViewports(g->ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(g->ctx, g->ps[ps], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g->ctx, 0, 5, views);
    ID3D11DeviceContext_Draw(g->ctx, 3, 0);
}

/* One pass at the picture's size from *picture into *spare, which becomes the picture. */
static void picture_pass(video_gpu_t* g, int ps, target_t** picture, target_t** spare) {
    ID3D11ShaderResourceView* views[5] = { NULL, NULL, (*picture)->srv, NULL, NULL };
    target_t* out = *spare;
    pass(g, ps, out->rtv, out->w, out->h, views);
    *spare = *picture;
    *picture = out;
}

int video_gpu_draw(video_gpu_t* g, const video_gpu_frame_t* f, const vl_layout_t* l, int out_w, int out_h,
                   ID3D11RenderTargetView* target) {
    float blend[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int glow = l->side >= 2, shown = f->policy.shown_rows, progressive = f->policy.rows == VL_ROWS_PROGRESSIVE;
    int deblock = f->deblock && progressive;
    ID3D11ShaderResourceView* views[5];
    target_t *picture, *spare;
    if (!g || !f->rows || f->width <= 0 || f->width > MAX_SIDE || shown <= 0 || shown > MAX_SIDE ||
        f->policy.stored_rows <= 0 || out_w <= 0 || out_h <= 0 || l->video.w <= 0 || l->video.h <= 0 || !target)
        return fail("video frame outside the supported sizes", E_INVALIDARG);
    if (!upload_codes(g, f) || !ensure_target(g, &g->frame, f->width, shown) ||
        ((f->smooth || deblock) && !ensure_target(g, &g->smooth, f->width, shown)))
        return 0;
    picture = &g->frame;
    spare = &g->smooth;
    if (f->palette_map && (f->palette_changed || !g->map_loaded)) {
        ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource*)g->map, 0, NULL, f->palette_map, 256 * 4, 0);
        g->map_loaded = 1;
    }
    update_gamma(g, f->gamma);
    if (!set_constants(g, f, l, out_w, out_h, glow, progressive)) return 0;
    ID3D11DeviceContext_IASetInputLayout(g->ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g->ctx, g->vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g->ctx, 0, 1, &g->cb);
    ID3D11DeviceContext_PSSetSamplers(g->ctx, 0, 1, &g->linear);
    ID3D11DeviceContext_RSSetState(g->ctx, g->raster);
    ID3D11DeviceContext_OMSetBlendState(g->ctx, NULL, blend, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g->ctx, NULL, 0);
    memset(views, 0, sizeof views);
    views[0] = g->codes_srv;
    views[1] = g->map_srv;
    pass(g, PS_CONVERT, g->frame.rtv, f->width, shown, views);
    if (deblock) {
        picture_pass(g, PS_DEBLOCK_X, &picture, &spare);
        picture_pass(g, PS_DEBLOCK_Y, &picture, &spare);
    }
    if (f->smooth) picture_pass(g, PS_SMOOTH, &picture, &spare);
    memset(views, 0, sizeof views);
    views[2] = picture->srv;
    if (glow) pass(g, PS_GLOW, g->glow.rtv, g->glow.w, g->glow.h, views);
    views[3] = g->glow.srv;
    views[4] = g->gamma_srv;
    pass(g, PS_FINAL, target, out_w, out_h, views);
    unbind(g);
    return 1;
}

void video_gpu_draw_black(video_gpu_t* g, ID3D11RenderTargetView* target) {
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (g && target) ID3D11DeviceContext_ClearRenderTargetView(g->ctx, target, black);
}
