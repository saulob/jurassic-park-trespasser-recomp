/* Headless checks of ambient occlusion: the setting, the Intel ASSAO presets behind each level, the projection the
 * game's depth really has, the frame policy, and the ASSAO wrapper on WARP devices (no window): lifecycle, the Off
 * path, the stencil mask, multisampled depth, every aspect ratio, ordering before the UI and the failure paths. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "test_profile.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "ASSAO.h"
#include "ao_policy.h"
#include "assao_host.h"
#include "jpt_config.h"

static int g_checks, g_failures;
static std::vector<std::string> g_log;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

extern "C" void host_log(const char* tag, const char* fmt, ...) {
    char line[2048];
    va_list ap;
    int n = _snprintf(line, sizeof line - 1, "[%s] ", tag);
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof line - 1 - n, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    g_log.push_back(line);
}

static int log_count(const char* text) {
    int n = 0;
    for (const std::string& line : g_log) n += line.find(text) != std::string::npos;
    return n;
}

template <class T> static void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

/* ---- settings, presets, projection, policy ---- */

static void test_setting() {
    jpt_config_t c;
    jpt_config_defaults(&c);
    CHECK(c.ambient_occlusion == JPT_AO_MEDIUM, "Ambient Occlusion defaults to Medium");
    CHECK(!strcmp(jpt_default_value(JPT_KEY_AMBIENT_OCCLUSION), "medium"), "the settings table says medium");
    for (int i = 0; i < JPT_AO_COUNT; i++) {
        CHECK(jpt_config_apply(&c, JPT_KEY_AMBIENT_OCCLUSION, jpt_ao_keys[i]) && c.ambient_occlusion == i, "%s parses",
              jpt_ao_keys[i]);
    }
    CHECK(jpt_config_apply(&c, JPT_KEY_AMBIENT_OCCLUSION, "High") && c.ambient_occlusion == JPT_AO_HIGH,
          "values ignore case");
    CHECK(!jpt_config_valid(JPT_KEY_AMBIENT_OCCLUSION, "ssao") && !jpt_config_valid(JPT_KEY_AMBIENT_OCCLUSION, "2"),
          "anything else is refused");
    CHECK(!strcmp(jpt_ao_names[JPT_AO_ULTRA], "Ultra") && !strcmp(jpt_ao_names[JPT_AO_OFF], "Off"), "names");
}

static void test_presets() {
    const ASSAO_Settings upstream;
    ao_preset_t p;
    const int expected[JPT_AO_COUNT] = { -9, 0, 1, 2, 3 };
    CHECK(!ao_preset(JPT_AO_OFF, &p) && !ao_preset(JPT_AO_COUNT, &p) && !ao_preset(-1, &p), "Off has no preset");
    for (int level = JPT_AO_LOW; level <= JPT_AO_ULTRA; level++) {
        CHECK(ao_preset(level, &p), "%s has a preset", jpt_ao_names[level]);
        CHECK(p.quality_level == expected[level], "%s = ASSAO QualityLevel %d (got %d)", jpt_ao_names[level],
              expected[level], p.quality_level);
        CHECK(p.blur_pass_count == upstream.BlurPassCount && p.shadow_multiplier == upstream.ShadowMultiplier &&
                  p.shadow_power == upstream.ShadowPower && p.shadow_clamp == upstream.ShadowClamp &&
                  p.horizon_angle_threshold == upstream.HorizonAngleThreshold &&
                  p.adaptive_quality_limit == upstream.AdaptiveQualityLimit && p.sharpness == upstream.Sharpness &&
                  p.detail_shadow_strength == upstream.DetailShadowStrength,
              "%s keeps ASSAO's own defaults", jpt_ao_names[level]);
        CHECK(p.radius == 1.0f && p.fade_out_from == 40.0f && p.fade_out_to == 150.0f,
              "%s: 1 m radius, faded out 40-150 m", jpt_ao_names[level]);
    }
}

static float near_rel(float a, float b) { return fabsf(a - b) / fmaxf(fabsf(b), 1e-6f); }

static void test_projection() {
    float m[16];
    const float distances[] = { 0.2f, 0.5f, 1.0f, 3.0f, 10.0f, 75.0f, 400.0f, 2000.0f };
    ao_projection(0.75f, 0.5f, m);
    CHECK(fabsf(1.0f / m[5] - 0.5f) < 1e-6f && fabsf(1.0f / m[0] - 0.75f) < 1e-6f,
          "ASSAO reads the tangents back as 1/_22 and 1/_11");
    CHECK(m[10] == 0.0f && m[11] == 1.0f && m[14] == AO_DEPTH_SCALE && m[15] == 0.0f,
          "depth row: z_ndc = 0.1 / z (reversed, infinite far)");
    for (float z : distances) {
        float sz = fminf(AO_DEPTH_SCALE / z, 0.99f); /* the game's vertex: fGetScreenZ */
        CHECK(near_rel(ao_decoded_depth(m, sz), z) < 1e-5f, "%.1f m decodes back (got %f)", z, ao_decoded_depth(m, sz));
    }
    CHECK(near_rel(ao_decoded_depth(m, AO_DEPTH_FLOOR), AO_FAR_LIMIT) < 1e-5f, "a cleared pixel is %.0f m away",
          AO_FAR_LIMIT);
    CHECK(ao_decoded_depth(m, 0.99f) > 0.1f, "the 0.99 clamp stays in front of the camera");
}

static void test_aspects() {
    const struct {
        const char* name;
        unsigned w, h;
    } shapes[] = { { "4:3", 1440, 1080 }, { "16:9", 1920, 1080 }, { "16:10", 1920, 1200 }, { "21:9", 2560, 1080 },
                   { "32:9", 3840, 1080 } };
    const float vertical = 0.41421356f; /* the 4:3 original's tan of half the vertical view is kept (Hor+) */
    float last_x = 0.0f, x, y;
    for (const auto& s : shapes) {
        CHECK(ao_view_tangents(vertical, s.w, s.h, &x, &y), "%s tangents", s.name);
        CHECK(y == vertical && fabsf(x / y - (float)s.w / s.h) < 1e-6f, "%s: vertical kept, horizontal by the target",
              s.name);
        CHECK(x > last_x || !strcmp(s.name, "16:10"), "%s widens the horizontal view", s.name);
        last_x = x;
    }
    CHECK(!ao_view_tangents(0.0f, 1920, 1080, &x, &y) && !ao_view_tangents(NAN, 1920, 1080, &x, &y) &&
              !ao_view_tangents(0.5f, 0, 1080, &x, &y),
          "no camera or no target: no tangents");
}

static void test_frame_policy() {
    ao_frame_t f = { JPT_AO_HIGH, 1, 1, 1, 1, 0.41f };
    CHECK(ao_frame_wanted(&f), "a gameplay frame with a scene gets AO");
    f.level = JPT_AO_OFF;
    CHECK(!ao_frame_wanted(&f), "Off: no effect");
    f.level = JPT_AO_LOW;
    f.scene = 0;
    CHECK(!ao_frame_wanted(&f), "no depth-writing draw this frame (a paused menu's frozen picture): no effect");
    f.scene = 1;
    f.gameplay = 0;
    CHECK(!ao_frame_wanted(&f), "menus, loading and videos: no effect");
    f.gameplay = 1;
    f.masked = 0;
    CHECK(!ao_frame_wanted(&f), "a frame drawn before the stencil marked: no effect");
    f.masked = 1;
    f.ready = 0;
    CHECK(!ao_frame_wanted(&f), "no effect or no readable depth: no effect");
    f.ready = 1;
    f.tan_half_h = 0.0f;
    CHECK(!ao_frame_wanted(&f), "no camera: no effect");
}

/* ---- WARP ---- */

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static std::string g_hlsl;

static bool make_device(D3D_FEATURE_LEVEL level, ID3D11Device** dev, ID3D11DeviceContext** ctx) {
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, dev,
                                       nullptr, ctx));
}

static bool load_hlsl() {
    FILE* f = fopen(JPT_SOURCE_DIR "/third_party/assao/ASSAO.hlsl", "rb");
    char buf[4096];
    size_t n;
    if (!f) return false;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) g_hlsl.append(buf, n);
    fclose(f);
    return !g_hlsl.empty();
}

static const char k_scene_hlsl[] =
    "cbuffer Scene : register(b0) { float2 size; float tan_x; float tan_y; float fixed_depth; float3 pad; };\n"
    "float4 vs_full(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 k = float2((id << 1) & 2, id & 2);\n"
    "  return float4(k * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    /* A floor 1.5 m below the camera meeting a wall 6 m ahead, as the game stores depth: min(0.1 / z, 0.99). */
    "float ps_scene(float4 pos : SV_Position) : SV_Depth {\n"
    "  if (fixed_depth >= 0.0) return fixed_depth;\n"
    "  float2 ndc = float2(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0);\n"
    "  float dy = ndc.y * tan_y;\n"
    "  float z = 6.0;\n"
    "  if (dy < 0.0) z = min(z, -1.5 / dy);\n"
    "  return min(0.1 / z, 0.99);\n"
    "}\n"
    "float4 ps_white(float4 pos : SV_Position) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n";

struct SceneGpu {
    ID3D11VertexShader* vs;
    ID3D11PixelShader* depth_ps;
    ID3D11PixelShader* white_ps;
    ID3D11Buffer* cb;
    ID3D11DepthStencilState* zero;
    ID3D11DepthStencilState* mark;
    ID3D11RasterizerState* scissored;
};

static SceneGpu g_s;

static ID3DBlob* compile_scene(const char* entry, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(k_scene_hlsl, sizeof k_scene_hlsl - 1, "ao_tests", nullptr, nullptr, entry, profile, 0, 0, &code,
                            &errors);
    if (FAILED(hr)) printf("scene shader %s: %s\n", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
    release(errors);
    return code;
}

static bool make_scene_gpu() {
    ID3DBlob* vs = compile_scene("vs_full", "vs_5_0");
    ID3DBlob* ps = compile_scene("ps_scene", "ps_5_0");
    ID3DBlob* white = compile_scene("ps_white", "ps_5_0");
    bool ok = vs && ps && white &&
              SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_s.vs)) &&
              SUCCEEDED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_s.depth_ps)) &&
              SUCCEEDED(g_dev->CreatePixelShader(white->GetBufferPointer(), white->GetBufferSize(), nullptr, &g_s.white_ps));
    release(vs);
    release(ps);
    release(white);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_ZERO;
    dd.BackFace = dd.FrontFace;
    D3D11_DEPTH_STENCIL_DESC marked = dd;
    marked.FrontFace.StencilPassOp = marked.BackFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    return ok && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_s.cb)) &&
           SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_s.zero)) &&
           SUCCEEDED(g_dev->CreateDepthStencilState(&marked, &g_s.mark)) &&
           SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_s.scissored));
}

static void release_scene_gpu() {
    release(g_s.vs);
    release(g_s.depth_ps);
    release(g_s.white_ps);
    release(g_s.cb);
    release(g_s.zero);
    release(g_s.mark);
    release(g_s.scissored);
}

/* The renderer's target as d3d11_renderer.c makes it: R24G8_TYPELESS, a D24_UNORM_S8_UINT DSV, depth and stencil
 * SRVs; and a white B8G8R8A8 picture standing for the back buffer. */
struct Target {
    unsigned w, h, samples;
    float tan_x, tan_y;
    ID3D11Texture2D* depth;
    ID3D11DepthStencilView* dsv;
    ID3D11ShaderResourceView* depth_srv;
    ID3D11ShaderResourceView* stencil_srv;
    ID3D11Texture2D* color;
    ID3D11RenderTargetView* rtv;
    ID3D11Texture2D* staging;
};

static void release_target(Target& t) {
    release(t.staging);
    release(t.rtv);
    release(t.color);
    release(t.stencil_srv);
    release(t.depth_srv);
    release(t.dsv);
    release(t.depth);
}

static bool make_target(Target& t, unsigned w, unsigned h, unsigned samples) {
    memset(&t, 0, sizeof t);
    t.w = w;
    t.h = h;
    t.samples = samples;
    ao_view_tangents(0.5f, w, h, &t.tan_x, &t.tan_y);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.depth)) || FAILED(g_dev->CreateDepthStencilView(t.depth, &dv, &t.dsv)))
        return false;
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (FAILED(g_dev->CreateShaderResourceView(t.depth, &sv, &t.depth_srv))) return false;
    sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    if (FAILED(g_dev->CreateShaderResourceView(t.depth, &sv, &t.stencil_srv))) return false;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.color)) || FAILED(g_dev->CreateRenderTargetView(t.color, nullptr, &t.rtv)))
        return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &t.staging));
}

static void scene_pass(const Target& t, ID3D11DepthStencilState* state, const D3D11_RECT& box, float fixed_depth) {
    const float cb[8] = { (float)t.w, (float)t.h, t.tan_x, t.tan_y, fixed_depth, 0, 0, 0 };
    D3D11_VIEWPORT vp = { 0, 0, (float)t.w, (float)t.h, 0, 1 };
    g_ctx->UpdateSubresource(g_s.cb, 0, nullptr, cb, 0, 0);
    g_ctx->OMSetRenderTargets(0, nullptr, t.dsv);
    g_ctx->OMSetDepthStencilState(state, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_s.vs, nullptr, 0);
    g_ctx->PSSetShader(g_s.depth_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_s.cb);
    g_ctx->RSSetState(g_s.scissored);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetScissorRects(1, &box);
    g_ctx->Draw(3, 0);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

/* The scene everywhere, then a CPU-composited block (stencil marked) at the left and an undrawn block (depth 0, the
 * clear) at the right, both across the floor-wall corner. */
static void draw_scene(const Target& t) {
    const float white[4] = { 1, 1, 1, 1 };
    D3D11_RECT all = { 0, 0, (LONG)t.w, (LONG)t.h };
    D3D11_RECT left = { 0, (LONG)(t.h * 6 / 10), (LONG)(t.w / 5), (LONG)(t.h * 9 / 10) };
    D3D11_RECT right = { (LONG)(t.w * 4 / 5), (LONG)(t.h * 6 / 10), (LONG)t.w, (LONG)(t.h * 9 / 10) };
    g_ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    scene_pass(t, g_s.zero, all, -1.0f);
    scene_pass(t, g_s.mark, left, -1.0f);
    scene_pass(t, g_s.zero, right, 0.0f);
    g_ctx->ClearRenderTargetView(t.rtv, white);
}

static std::vector<unsigned char> read_red(const Target& t) {
    std::vector<unsigned char> red((size_t)t.w * t.h);
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->CopyResource(t.staging, t.color);
    if (FAILED(g_ctx->Map(t.staging, 0, D3D11_MAP_READ, 0, &m))) return red;
    for (unsigned y = 0; y < t.h; y++)
        for (unsigned x = 0; x < t.w; x++) red[(size_t)y * t.w + x] = ((const unsigned char*)m.pData)[y * m.RowPitch + x * 4 + 2];
    g_ctx->Unmap(t.staging, 0);
    return red;
}

static assao_scene_t scene_of(const Target& t) {
    assao_scene_t s = {};
    s.depth = t.depth_srv;
    s.stencil = t.stencil_srv;
    s.width = t.w;
    s.height = t.h;
    s.samples = t.samples;
    s.tan_half_x = t.tan_x;
    s.tan_half_y = t.tan_y;
    return s;
}

struct Region {
    unsigned x0, y0, x1, y1;
};

static void range(const std::vector<unsigned char>& red, const Target& t, Region r, int* lo, int* hi) {
    *lo = 255;
    *hi = 0;
    for (unsigned y = r.y0; y < r.y1; y++)
        for (unsigned x = r.x0; x < r.x1; x++) {
            int v = red[(size_t)y * t.w + x];
            if (v < *lo) *lo = v;
            if (v > *hi) *hi = v;
        }
}

/* The corner where the floor meets the wall darkens; the open floor and the wall's middle stay near white; marked
 * and undrawn pixels are untouched. */
static void check_picture(const Target& t, const char* what) {
    std::vector<unsigned char> red = read_red(t);
    unsigned crease = t.h * 3 / 4, mid_x0 = t.w * 2 / 5, mid_x1 = t.w * 3 / 5;
    int lo, hi, open_lo, open_hi, wall_lo, wall_hi;
    range(red, t, { mid_x0, crease - t.h / 30, mid_x1, crease + t.h / 30 + 1 }, &lo, &hi);
    CHECK(lo < 215, "%s: the floor-wall corner is darkened (darkest %d)", what, lo);
    range(red, t, { mid_x0, t.h * 95 / 100 - 2, mid_x1, t.h * 95 / 100 + 2 }, &open_lo, &open_hi);
    CHECK(open_lo > 225, "%s: the open floor stays light (darkest %d)", what, open_lo);
    range(red, t, { mid_x0, t.h * 3 / 10, mid_x1, t.h * 4 / 10 }, &wall_lo, &wall_hi);
    CHECK(wall_lo > 225, "%s: the flat wall stays light (darkest %d)", what, wall_lo);
    printf("%-28s corner %3d, open floor %3d, wall %3d\n", what, lo, open_lo, wall_lo);
    range(red, t, { 0, t.h * 6 / 10, t.w / 5, t.h * 9 / 10 }, &lo, &hi);
    CHECK(lo == 255, "%s: stencil-marked (CPU) pixels are untouched (darkest %d)", what, lo);
    range(red, t, { t.w * 4 / 5 + 1, t.h * 6 / 10, t.w, t.h * 9 / 10 }, &lo, &hi);
    CHECK(lo == 255, "%s: undrawn pixels are untouched (darkest %d)", what, lo);
}

static void test_lifecycle() {
    Target t;
    CHECK(!assao_ready(), "nothing exists before init");
    CHECK(make_target(t, 320, 180, 1), "WARP target");
    assao_scene_t s = scene_of(t);
    draw_scene(t);
    assao_set_quality(JPT_AO_HIGH);
    CHECK(!assao_render(g_ctx, &s, t.rtv), "no effect before init: nothing drawn");

    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    CHECK(assao_init(g_dev, g_hlsl.data(), g_hlsl.size()), "ASSAO.hlsl and the glue passes compile, the effect is created");
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    printf("ASSAO init (all shaders) %.0f ms\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
    CHECK(assao_ready() && log_count("Intel ASSAO 1.0 ready") == 1, "ready and logged");
    CHECK(assao_init(g_dev, g_hlsl.data(), g_hlsl.size()) && log_count("Intel ASSAO 1.0 ready") == 1,
          "a second init on the same device keeps the effect");

    assao_set_quality(JPT_AO_OFF);
    unsigned long long frames = assao_frames();
    CHECK(!assao_render(g_ctx, &s, t.rtv) && assao_frames() == frames, "Off: no effect pass");
    std::vector<unsigned char> red = read_red(t);
    int lo, hi;
    range(red, t, { 0, 0, t.w, t.h }, &lo, &hi);
    CHECK(lo == 255, "Off: the picture is untouched (darkest %d)", lo);

    for (int level = JPT_AO_LOW; level <= JPT_AO_ULTRA; level++) {
        char what[64];
        draw_scene(t);
        assao_set_quality(level);
        CHECK(assao_render(g_ctx, &s, t.rtv), "%s renders", jpt_ao_names[level]);
        snprintf(what, sizeof what, "%s 320x180", jpt_ao_names[level]);
        check_picture(t, what);
    }

    g_log.clear();
    assao_set_quality(JPT_AO_HIGH);
    for (int i = 0; i < 3; i++) {
        draw_scene(t);
        assao_render(g_ctx, &s, t.rtv);
    }
    CHECK(log_count("[assao] High 320x180") == 1 && log_count("ASSAO quality level 2") == 1,
          "one log line per quality or size change, none per frame");

    /* Runtime toggles: High -> Off -> High -> Low -> Ultra on the same effect. */
    const int toggles[] = { JPT_AO_OFF, JPT_AO_HIGH, JPT_AO_LOW, JPT_AO_ULTRA, JPT_AO_OFF };
    for (int level : toggles) {
        draw_scene(t);
        assao_set_quality(level);
        int drawn = assao_render(g_ctx, &s, t.rtv);
        CHECK(drawn == (level != JPT_AO_OFF), "toggle to %s", jpt_ao_names[level]);
    }

    /* The UI is drawn after: render leaves the output bound, and what is drawn next is not darkened. */
    draw_scene(t);
    assao_set_quality(JPT_AO_HIGH);
    assao_render(g_ctx, &s, t.rtv);
    ID3D11RenderTargetView* bound = nullptr;
    ID3D11DepthStencilView* bound_dsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &bound, &bound_dsv);
    CHECK(bound == t.rtv && !bound_dsv, "the picture is left bound for the UI passes");
    release(bound);
    release(bound_dsv);
    {
        D3D11_VIEWPORT vp = { 0, 0, (float)t.w, (float)t.h, 0, 1 };
        D3D11_RECT crease_box = { 0, 0, (LONG)t.w, (LONG)t.h };
        g_ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_s.vs, nullptr, 0);
        g_ctx->PSSetShader(g_s.white_ps, nullptr, 0);
        g_ctx->RSSetState(g_s.scissored);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetScissorRects(1, &crease_box);
        g_ctx->Draw(3, 0);
        red = read_red(t);
        range(red, t, { 0, 0, t.w, t.h }, &lo, &hi);
        CHECK(lo == 255, "a UI layer drawn after the effect is not darkened (darkest %d)", lo);
    }
    release_target(t);
}

static void test_msaa_and_aspects() {
    const struct {
        const char* name;
        unsigned w, h;
    } shapes[] = { { "4:3", 320, 240 }, { "16:9", 384, 216 }, { "16:10", 384, 240 }, { "21:9", 504, 216 },
                   { "32:9", 768, 216 } };
    UINT quality = 0;
    assao_set_quality(JPT_AO_HIGH);
    for (const auto& shape : shapes) {
        Target t;
        char what[64];
        CHECK(make_target(t, shape.w, shape.h, 1), "%s target", shape.name);
        assao_scene_t s = scene_of(t);
        draw_scene(t);
        CHECK(assao_render(g_ctx, &s, t.rtv), "%s renders after a resize", shape.name);
        snprintf(what, sizeof what, "High %s %ux%u", shape.name, shape.w, shape.h);
        check_picture(t, what);
        release_target(t);
    }
    if (FAILED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4, &quality)) || !quality) {
        printf("WARP has no 4x MSAA depth: multisampled case skipped\n");
        return;
    }
    for (int level = JPT_AO_LOW; level <= JPT_AO_ULTRA; level += 3) {
        Target t;
        char what[64];
        CHECK(make_target(t, 384, 216, 4), "4x MSAA target");
        assao_scene_t s = scene_of(t);
        draw_scene(t);
        assao_set_quality(level);
        CHECK(assao_render(g_ctx, &s, t.rtv), "%s renders from a 4x multisampled depth buffer", jpt_ao_names[level]);
        snprintf(what, sizeof what, "%s 4x MSAA", jpt_ao_names[level]);
        check_picture(t, what);
        release_target(t);
    }
}

static void test_failures() {
    ID3D11Device* dev10 = nullptr;
    ID3D11DeviceContext* ctx10 = nullptr;
    Target t;
    g_log.clear();
    assao_shutdown();
    CHECK(!assao_ready(), "shutdown releases the effect");
    CHECK(make_target(t, 160, 90, 1), "small target");
    assao_scene_t s = scene_of(t);
    assao_set_quality(JPT_AO_HIGH);
    CHECK(!assao_render(g_ctx, &s, t.rtv), "after shutdown nothing is drawn");
    CHECK(assao_quality() == JPT_AO_HIGH, "the chosen level survives a shutdown");

    CHECK(!assao_init(nullptr, g_hlsl.data(), g_hlsl.size()) && !assao_init(g_dev, nullptr, 0), "no device or shader");
    const char broken[] = "float4 PSApply( in float4 inPos : SV_POSITION ) : SV_Target { return undefined_name; }";
    CHECK(!assao_init(g_dev, broken, sizeof broken - 1) && !assao_ready(), "a shader that does not compile: no effect");
    CHECK(log_count("ASSAO shader compilation failed") >= 1, "the compiler's message goes to the log, not a message box");
    if (make_device(D3D_FEATURE_LEVEL_10_0, &dev10, &ctx10)) {
        CHECK(!assao_init(dev10, g_hlsl.data(), g_hlsl.size()) && log_count("feature level 11.0") == 1,
              "feature level 10.0: refused and logged");
    }
    release(ctx10);
    release(dev10);
    s.stencil = nullptr;
    CHECK(assao_init(g_dev, g_hlsl.data(), g_hlsl.size()), "init again after failures");
    CHECK(!assao_render(g_ctx, &s, t.rtv), "a target without a readable stencil gets no effect");
    s = scene_of(t);
    s.tan_half_y = 0.0f;
    CHECK(!assao_render(g_ctx, &s, t.rtv), "no camera tangents: no effect");
    release_target(t);

    /* A lost device: everything goes with it and the next device gets a new effect. */
    ID3D11Device* dev2 = nullptr;
    ID3D11DeviceContext* ctx2 = nullptr;
    assao_shutdown();
    release_scene_gpu();
    release(g_ctx);
    release(g_dev);
    CHECK(make_device(D3D_FEATURE_LEVEL_11_0, &dev2, &ctx2), "a new WARP device");
    g_dev = dev2;
    g_ctx = ctx2;
    CHECK(make_scene_gpu() && assao_init(g_dev, g_hlsl.data(), g_hlsl.size()), "the effect on the new device");
    CHECK(make_target(t, 320, 180, 1), "target on the new device");
    s = scene_of(t);
    draw_scene(t);
    CHECK(assao_render(g_ctx, &s, t.rtv), "renders after the device was recreated");
    check_picture(t, "High after device recreation");
    release_target(t);
}

int main() {
    test_profile_activate();
    test_setting();
    test_presets();
    test_projection();
    test_aspects();
    test_frame_policy();
    if (!load_hlsl()) {
        CHECK(0, "third_party/assao/ASSAO.hlsl is readable");
    } else if (!make_device(D3D_FEATURE_LEVEL_11_0, &g_dev, &g_ctx) || !make_scene_gpu()) {
        CHECK(0, "a WARP device at feature level 11.0 and the test scene");
    } else {
        test_lifecycle();
        test_msaa_and_aspects();
        test_failures();
        assao_shutdown();
        release_scene_gpu();
        release(g_ctx);
        release(g_dev);
    }
    printf("ao_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
