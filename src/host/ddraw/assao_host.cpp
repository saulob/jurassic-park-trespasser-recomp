#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include "ASSAO.h"
#include "ao_policy.h"
#include "assao_host.h"
#include "jpt_config.h"

extern "C" void host_log(const char* tag, const char* fmt, ...);

namespace {

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* The scene's depth goes to ASSAO as the device stores it (0.1 / metres, decoded through ao_projection), sample 0 of a
 * multisampled buffer, with cleared pixels kept finite; the mask is the share of samples AO may darken. */
const char k_glue_hlsl[] =
    "#if AO_MSAA\n"
    "Texture2DMS<float> scene_depth : register(t0);\n"
    "Texture2DMS<uint2> scene_stencil : register(t1);\n"
    "#else\n"
    "Texture2D<float> scene_depth : register(t0);\n"
    "Texture2D<uint2> scene_stencil : register(t1);\n"
    "#endif\n"
    "Texture2D<float> ao_term : register(t2);\n"
    "Texture2D<float> ao_mask : register(t3);\n"
    "Texture2D<float4> scene_color : register(t4);\n"
    "cbuffer Apply : register(b0) { float3 fog_color; float apply_pad; };\n"
    "struct Prepared { float depth : SV_Target0; float mask : SV_Target1; };\n"
    "float4 vs_full(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 k = float2((id << 1) & 2, id & 2);\n"
    "  return float4(k * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "float open_sample(float depth, uint stencil) { return (stencil == 0 && depth > AO_DEPTH_FLOOR) ? 1.0 : 0.0; }\n"
    "Prepared ps_prepare(float4 pos : SV_Position) {\n"
    "  int2 p = int2(pos.xy);\n"
    "  Prepared o;\n"
    "#if AO_MSAA\n"
    "  uint w, h, n;\n"
    "  scene_depth.GetDimensions(w, h, n);\n"
    "  float open = 0.0;\n"
    "  for (uint i = 0; i < n; i++) open += open_sample(scene_depth.Load(p, i), scene_stencil.Load(p, i).g);\n"
    "  o.depth = scene_depth.Load(p, 0);\n"
    "  o.mask = open / n;\n"
    "#else\n"
    "  o.depth = scene_depth.Load(int3(p, 0));\n"
    "  o.mask = open_sample(o.depth, scene_stencil.Load(int3(p, 0)).g);\n"
    "#endif\n"
    "  o.depth = max(o.depth, AO_DEPTH_FLOOR);\n"
    "  return o;\n"
    "}\n"
    /* Only the part of the pixel that is not fog darkens, as if the occlusion had been applied before the fog. */
    "float4 ps_apply(float4 pos : SV_Position) : SV_Target {\n"
    "  int3 p = int3(pos.xy, 0);\n"
    "  float4 scene = scene_color.Load(p);\n"
    "  float open = ao_mask.Load(p), visible = min(open, scene.a);\n"
    "  float3 fog = (open - visible) * fog_color;\n"
    "  float3 weight = min(visible.xxx, saturate(1.0 - fog / max(scene.rgb, 1.0 / 255.0)));\n"
    "  return float4(lerp(float3(1.0, 1.0, 1.0), ao_term.Load(p).xxx, weight), 1.0);\n"
    "}\n";

struct Surface {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView* rtv;
};

struct State {
    ID3D11Device* device;
    ASSAO_Effect* effect;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* prepare[2];
    ID3D11PixelShader* apply;
    ID3D11Buffer* apply_constants;
    ID3D11BlendState* multiply;
    ID3D11RasterizerState* raster;
    ID3D11DepthStencilState* no_depth;
    Surface depth, mask, term;
    unsigned width, height;
    int quality;
    int logged_quality;
    unsigned logged_w, logged_h;
    unsigned long long frames;
};

State S;

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void release_surface(Surface& s) {
    release(s.rtv);
    release(s.srv);
    release(s.tex);
}

void release_surfaces() {
    release_surface(S.depth);
    release_surface(S.mask);
    release_surface(S.term);
    S.width = S.height = 0;
}

bool make_surface(Surface& s, unsigned w, unsigned h, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(S.device->CreateTexture2D(&desc, nullptr, &s.tex)) &&
           SUCCEEDED(S.device->CreateShaderResourceView(s.tex, nullptr, &s.srv)) &&
           SUCCEEDED(S.device->CreateRenderTargetView(s.tex, nullptr, &s.rtv));
}

ID3DBlob* compile(const char* entry, const char* profile, bool msaa) {
    const D3D_SHADER_MACRO macros[] = { { "AO_MSAA", msaa ? "1" : "0" },
                                        { "AO_DEPTH_FLOOR", STRINGIFY(AO_DEPTH_FLOOR) },
                                        { nullptr, nullptr } };
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(k_glue_hlsl, sizeof k_glue_hlsl - 1, "assao_host", macros, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
        host_log("assao", "shader %s failed to compile: %s", entry,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
    release(errors);
    return SUCCEEDED(hr) ? code : nullptr;
}

bool make_pixel_shader(const char* entry, bool msaa, ID3D11PixelShader** out) {
    ID3DBlob* code = compile(entry, "ps_5_0", msaa);
    bool ok = code && SUCCEEDED(S.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out));
    release(code);
    return ok;
}

bool make_glue() {
    ID3DBlob* code = compile("vs_full", "vs_5_0", false);
    bool ok = code && SUCCEEDED(S.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &S.vs));
    release(code);
    ok = ok && make_pixel_shader("ps_prepare", false, &S.prepare[0]) && make_pixel_shader("ps_prepare", true, &S.prepare[1]) &&
         make_pixel_shader("ps_apply", false, &S.apply);
    if (!ok) return false;
    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = 4 * sizeof(float);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    return SUCCEEDED(S.device->CreateBuffer(&constants, nullptr, &S.apply_constants)) &&
           SUCCEEDED(S.device->CreateBlendState(&blend, &S.multiply)) &&
           SUCCEEDED(S.device->CreateRasterizerState(&raster, &S.raster)) &&
           SUCCEEDED(S.device->CreateDepthStencilState(&depth, &S.no_depth));
}

bool compiler_available() {
    HMODULE dll = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) dll = LoadLibraryA("d3dcompiler_47.dll");
    return dll != nullptr;
}

ASSAO_Settings settings_for(const ao_preset_t& p) {
    ASSAO_Settings s;
    s.QualityLevel = p.quality_level;
    s.BlurPassCount = p.blur_pass_count;
    s.Radius = p.radius;
    s.ShadowMultiplier = p.shadow_multiplier;
    s.ShadowPower = p.shadow_power;
    s.ShadowClamp = p.shadow_clamp;
    s.HorizonAngleThreshold = p.horizon_angle_threshold;
    s.FadeOutFrom = p.fade_out_from;
    s.FadeOutTo = p.fade_out_to;
    s.AdaptiveQualityLimit = p.adaptive_quality_limit;
    s.Sharpness = p.sharpness;
    s.DetailShadowStrength = p.detail_shadow_strength;
    return s;
}

void set_viewport(ID3D11DeviceContext* ctx, unsigned w, unsigned h) {
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
}

void fullscreen_pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11BlendState* blend) {
    const float factor[4] = { 0, 0, 0, 0 };
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(S.vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->RSSetState(S.raster);
    ctx->OMSetBlendState(blend, factor, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(S.no_depth, 0);
    ctx->Draw(3, 0);
}

void unbind(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* none[5] = {};
    ID3D11UnorderedAccessView* no_uav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ctx->PSSetShaderResources(0, 5, none);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, no_uav, nullptr);
}

void set_apply_constants(ID3D11DeviceContext* ctx, const float fog[3]) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(S.apply_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, fog, 3 * sizeof(float));
        ctx->Unmap(S.apply_constants, 0);
    }
    ctx->PSSetConstantBuffers(0, 1, &S.apply_constants);
}

void log_quality(unsigned w, unsigned h) {
    ao_preset_t p;
    if (S.logged_quality == S.quality && S.logged_w == w && S.logged_h == h) return;
    if (S.logged_quality != S.quality && ao_preset(S.quality, &p))
        host_log("assao", "%s = ASSAO quality level %d, radius %.1f m, %d blur passes, fade %.0f-%.0f m", jpt_ao_names[S.quality],
                 p.quality_level, p.radius, p.blur_pass_count, p.fade_out_from, p.fade_out_to);
    host_log("assao", "%s %ux%u", jpt_ao_names[S.quality], w, h);
    S.logged_quality = S.quality;
    S.logged_w = w;
    S.logged_h = h;
}

} // namespace

extern "C" void assao_shader_error(const char* message) {
    host_log("assao", "ASSAO shader compilation failed: %s", message);
}

extern "C" int assao_init(ID3D11Device* device, const char* hlsl, size_t hlsl_size) {
    LARGE_INTEGER start, end, freq;
    int major = 0, minor = 0;
    if (S.effect && S.device == device) return 1;
    assao_shutdown();
    if (!device || !hlsl || !hlsl_size) return 0;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        host_log("assao", "ambient occlusion needs Direct3D feature level 11.0: it stays off");
        return 0;
    }
    if (!compiler_available()) {
        host_log("assao", "d3dcompiler_47.dll is unavailable: ambient occlusion stays off");
        return 0;
    }
    QueryPerformanceCounter(&start);
    S.device = device;
    S.device->AddRef();
    if (!make_glue()) {
        host_log("assao", "the depth and apply passes could not be created: ambient occlusion stays off");
        assao_shutdown();
        return 0;
    }
    ASSAO_CreateDescDX11 desc(device, hlsl, hlsl_size);
    S.effect = ASSAO_Effect::CreateInstance(&desc);
    if (!S.effect) {
        host_log("assao", "Intel ASSAO could not be created (see the lines above): ambient occlusion stays off");
        assao_shutdown();
        return 0;
    }
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&freq);
    S.effect->GetVersion(major, minor);
    S.logged_quality = -1;
    host_log("assao", "Intel ASSAO %d.%d ready (GameTechDev/ASSAO c21d57a, MIT), shaders compiled in %.0f ms", major, minor,
             (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart);
    return 1;
}

extern "C" int assao_ready(void) { return S.effect != nullptr; }

extern "C" void assao_set_quality(int level) { S.quality = level > JPT_AO_OFF && level < JPT_AO_COUNT ? level : JPT_AO_OFF; }

extern "C" int assao_quality(void) { return S.quality; }

extern "C" int assao_resize(unsigned width, unsigned height) {
    if (!S.effect || !width || !height) return 0;
    if (S.width == width && S.height == height) return 1;
    release_surfaces();
    if (!make_surface(S.depth, width, height, DXGI_FORMAT_R32_FLOAT) ||
        !make_surface(S.mask, width, height, DXGI_FORMAT_R8_UNORM) ||
        !make_surface(S.term, width, height, DXGI_FORMAT_R8_UNORM)) {
        host_log("assao", "the %ux%u ambient occlusion buffers could not be created", width, height);
        release_surfaces();
        return 0;
    }
    S.width = width;
    S.height = height;
    return 1;
}

extern "C" int assao_render(ID3D11DeviceContext* ctx, const assao_scene_t* scene, ID3D11RenderTargetView* out) {
    static const float k_unoccluded[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ao_preset_t preset;
    float projection[16];
    if (!S.effect || !ctx || !scene || !out || !scene->depth || !scene->stencil || !scene->color ||
        !ao_preset(S.quality, &preset) || !(scene->tan_half_x > 0.0f) || !(scene->tan_half_y > 0.0f) ||
        !assao_resize(scene->width, scene->height))
        return 0;
    log_quality(scene->width, scene->height);

    ID3D11RenderTargetView* prepared[2] = { S.depth.rtv, S.mask.rtv };
    ID3D11ShaderResourceView* sources[2] = { scene->depth, scene->stencil };
    unbind(ctx);
    ctx->OMSetRenderTargets(2, prepared, nullptr);
    set_viewport(ctx, scene->width, scene->height);
    ctx->PSSetShaderResources(0, 2, sources);
    fullscreen_pass(ctx, S.prepare[scene->samples > 1 ? 1 : 0], nullptr);
    unbind(ctx);

    ASSAO_InputsDX11 inputs;
    ao_projection(scene->tan_half_x, scene->tan_half_y, projection);
    inputs.ViewportWidth = static_cast<int>(scene->width);
    inputs.ViewportHeight = static_cast<int>(scene->height);
    inputs.ProjectionMatrix = ASSAO_Float4x4(projection);
    inputs.MatricesRowMajorOrder = true;
    inputs.DrawOpaque = true;
    inputs.DeviceContext = ctx;
    inputs.DepthSRV = S.depth.srv;
    inputs.NormalSRV = nullptr;
    inputs.OverrideOutputRTV = S.term.rtv;
    ctx->ClearRenderTargetView(S.term.rtv, k_unoccluded);
    S.effect->Draw(settings_for(preset), &inputs);
    unbind(ctx);

    ID3D11ShaderResourceView* results[3] = { S.term.srv, S.mask.srv, scene->color };
    ctx->OMSetRenderTargets(1, &out, nullptr);
    set_viewport(ctx, scene->width, scene->height);
    ctx->PSSetShaderResources(2, 3, results);
    set_apply_constants(ctx, scene->fog);
    fullscreen_pass(ctx, S.apply, S.multiply);
    unbind(ctx);
    ctx->OMSetRenderTargets(1, &out, nullptr);
    S.frames++;
    return 1;
}

extern "C" void assao_shutdown(void) {
    int quality = S.quality;
    unsigned long long frames = S.frames;
    if (S.effect) ASSAO_Effect::DestroyInstance(S.effect);
    release_surfaces();
    release(S.vs);
    release(S.prepare[0]);
    release(S.prepare[1]);
    release(S.apply);
    release(S.apply_constants);
    release(S.multiply);
    release(S.raster);
    release(S.no_depth);
    release(S.device);
    S = State();
    S.quality = quality;
    S.frames = frames;
}

extern "C" unsigned long long assao_frames(void) { return S.frames; }
