/*
 * ps_fixed's fog, ps_water and ps_sky drawn offscreen on a WARP device (no window): the water shows the scene behind displaced by the
 * wave differences under its own colour, the sky fogs by view direction, and a multisampled scene resolves into the
 * copy the water samples. The composition tests prove the screen position, the shift in pixels and what reaches the
 * target through the retail blend, and that no offset brings the black outside the 3D viewport into the picture.
 * Build: CMake target ce_gpu_tests.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ce_effects.h"
#include "d3d11_shaders.h"

static int s_failures, s_checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

#define SIZE 64
#define F_FOG 16u
#define F_AO_CLOSED 1024u
#define FULL_BOUNDS 0.5f / SIZE, 0.5f / SIZE, (SIZE - 0.5f) / SIZE, (SIZE - 0.5f) / SIZE

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float u, v;
    uint32_t layer;
} vertex_t;

static ID3D11Device* dev;
static ID3D11BlendState* s_blend;
static uint32_t s_specular = 0xFF000000u;
static ID3D11DeviceContext* ctx;
static compile_fn compile;

static ID3DBlob* build(const char* entry, const char* profile) {
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(d3d11_shader_source, strlen(d3d11_shader_source), "ce", NULL, NULL, entry, profile, 0, 0, &code,
                         &errors);
    if (FAILED(hr)) printf("%s: %s\n", entry, errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "?");
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(hr) ? code : NULL;
}

static ID3D11Buffer* constants(const void* data, UINT size) {
    D3D11_BUFFER_DESC d = { size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer* b = NULL;
    ID3D11Device_CreateBuffer(dev, &d, &init, &b);
    return b;
}

static ID3D11Texture2D* texture(UINT w, UINT h, DXGI_FORMAT format, UINT samples, UINT bind, const void* pixels, UINT pitch) {
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init = { pixels, pitch, 0 };
    ID3D11Texture2D* t = NULL;
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ID3D11Device_CreateTexture2D(dev, &d, pixels ? &init : NULL, &t);
    return t;
}

static ID3D11ShaderResourceView* view(ID3D11Texture2D* t) {
    ID3D11ShaderResourceView* v = NULL;
    if (t) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)t, NULL, &v);
    return v;
}

/* One pixel of a single-sample B8G8R8A8 texture, as 0xAARRGGBB. */
static uint32_t pixel(ID3D11Texture2D* t, UINT x, UINT y) {
    ID3D11Texture2D* staging;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t p = 0;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging))) return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        p = *(const uint32_t*)((const BYTE*)m.pData + y * m.RowPitch + x * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return p;
}

static void read_all(ID3D11Texture2D* t, uint32_t* out) {
    ID3D11Texture2D* staging;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    memset(out, 0, (size_t)d.Width * d.Height * 4);
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) memcpy(out + y * d.Width, (const BYTE*)m.pData + y * m.RowPitch, d.Width * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

static int channel(uint32_t p, int shift) { return (int)((p >> shift) & 0xFF); }

static int close_to(uint32_t p, int r, int g, int b) {
    return abs(channel(p, 16) - r) <= 3 && abs(channel(p, 8) - g) <= 3 && abs(channel(p, 0) - b) <= 3;
}

/* A full-target quad of pre-transformed vertices (the game's), drawn with the given pixel shader. */
static void draw(ID3D11RenderTargetView* rtv, ID3DBlob* vs_code, ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                 uint32_t diffuse, float u, float v, float rhw, const uint32_t draw_cb[8], const float fx[20],
                 ID3D11ShaderResourceView* waves, ID3D11ShaderResourceView* behind, ID3D11SamplerState* sampler) {
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    vertex_t q[6];
    float corners[6][2] = { { 0, 0 }, { SIZE, 0 }, { 0, SIZE }, { SIZE, 0 }, { SIZE, SIZE }, { 0, SIZE } };
    float frame[4] = { SIZE, SIZE, 1.0f / SIZE, 1.0f / SIZE };
    D3D11_BUFFER_DESC bd = { sizeof q, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { q, 0, 0 };
    D3D11_VIEWPORT vp = { 0, 0, SIZE, SIZE, 0, 1 };
    ID3D11Buffer *vb = NULL, *cb[4];
    ID3D11InputLayout* layout = NULL;
    ID3D11ShaderResourceView* views[2] = { waves, behind };
    UINT stride = sizeof(vertex_t), offset = 0;
    float zero[12] = { 0 };
    for (int i = 0; i < 6; i++) {
        q[i].x = corners[i][0];
        q[i].y = corners[i][1];
        q[i].z = 0.5f;
        q[i].rhw = rhw;
        q[i].diffuse = diffuse;
        q[i].specular = s_specular;
        q[i].u = u;
        q[i].v = v;
        q[i].layer = 0;
    }
    ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);
    ID3D11Device_CreateInputLayout(dev, elements, 5, ID3D10Blob_GetBufferPointer(vs_code), ID3D10Blob_GetBufferSize(vs_code),
                                   &layout);
    cb[0] = constants(frame, sizeof frame);
    cb[1] = constants(draw_cb, 32);
    cb[2] = constants(zero, 48);
    cb[3] = constants(fx, 80);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_blend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb[0]);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 4, cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 2, views);
    ID3D11DeviceContext_PSSetSamplers(ctx, 5, 1, &sampler);
    ID3D11DeviceContext_Draw(ctx, 6, 0);
    for (int i = 0; i < 4; i++)
        if (cb[i]) ID3D11Buffer_Release(cb[i]);
    if (layout) ID3D11InputLayout_Release(layout);
    if (vb) ID3D11Buffer_Release(vb);
}

/* A wave texture of one difference everywhere. */
static ID3D11ShaderResourceView* uniform_waves(float a, float b, ID3D11Texture2D** t) {
    float w[4 * 4 * 2];
    for (int i = 0; i < 16; i++) w[i * 2] = a, w[i * 2 + 1] = b;
    *t = texture(4, 4, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE, w, 32);
    return view(*t);
}

static int mix(int water, int behind, int alpha) { return (water * alpha + behind * (255 - alpha) + 127) / 255; }

static void refill(ID3D11Texture2D* target, const uint32_t* frame) {
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)target, 0, NULL, frame, SIZE * 4, 0);
}

/* The water draw as the renderer makes it: SRCALPHA / INVSRCALPHA over the frame, whose copy is the scene behind. */
static void composition_tests(ID3DBlob* vs_code, ID3D11VertexShader* vs, ID3D11PixelShader* water, ID3D11PixelShader* fixed,
                              ID3D11SamplerState* point) {
    static uint32_t ramp[SIZE * SIZE], checker[SIZE * SIZE], out[SIZE * SIZE], off[SIZE * SIZE];
    const float strength = 0.35f, four = 4.0f / (SIZE * strength); /* 4 pixels within a metre */
    float fx[20] = { 1.0f / SIZE, 1.0f / SIZE, strength, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 10.0f, 0, 0, 0, FULL_BOUNDS };
    uint32_t cb[8] = { 0 };
    D3D11_BLEND_DESC bd;
    ID3D11Texture2D *ramp_t, *checker_t, *target, *zero_t, *right_t, *down_t;
    ID3D11ShaderResourceView *ramp_v, *checker_v, *zero_v, *right_v, *down_v;
    ID3D11RenderTargetView* rtv = NULL;
    int exact = 1, shifted = 1, down = 1, same = 1, moved = 0, flips = 0, expected = 1, worst = 0;
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++) {
            ramp[y * SIZE + x] = 0xFF000000u | (uint32_t)(x * 4) << 16 | (uint32_t)(y * 4) << 8;
            checker[y * SIZE + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    ramp_t = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, ramp, SIZE * 4);
    checker_t = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, checker, SIZE * 4);
    target = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, NULL, 0);
    ramp_v = view(ramp_t);
    checker_v = view(checker_t);
    zero_v = uniform_waves(0.0f, 0.0f, &zero_t);
    right_v = uniform_waves(four, 0.0f, &right_t);
    down_v = uniform_waves(0.0f, four, &down_t);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)target, NULL, &rtv);

    /* Screen position: clear water (alpha 0) shows exactly the copy's pixel under it, then the one 4 pixels over. */
    draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, fx, zero_v, ramp_v, point);
    read_all(target, out);
    for (int i = 0; i < SIZE * SIZE; i++) exact &= (out[i] | 0xFF000000u) == ramp[i];
    CHECK(exact, "zero waves sample the scene copy at the pixel's own position");
    draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, ramp_v, point);
    read_all(target, out);
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE - 4; x++)
            shifted &= channel(out[y * SIZE + x], 16) == (x + 4) * 4 && channel(out[y * SIZE + x], 8) == y * 4;
    CHECK(shifted, "a known difference samples exactly 4 pixels to the right: 0x%08X at 0", out[0]);
    draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, fx, down_v, ramp_v, point);
    read_all(target, out);
    for (int y = 0; y < SIZE - 4; y++)
        for (int x = 0; x < SIZE; x++)
            down &= channel(out[y * SIZE + x], 8) == (y + 4) * 4 && channel(out[y * SIZE + x], 16) == x * 4;
    CHECK(down, "and exactly 4 pixels down for the other difference");

    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bd, &s_blend);

    /* Off, the retail draw: half-alpha green blended over the checker frame. */
    refill(target, checker);
    draw(rtv, vs_code, vs, fixed, 0x8000FF00u, 0.5f, 0.5f, 0.1f, cb, fx, NULL, NULL, point);
    read_all(target, off);
    CHECK(channel(off[0], 8) == mix(255, 0, 128) && channel(off[8], 16) == mix(0, 255, 128), "retail blend: 0x%08X", off[8]);
    /* On with still water: the same picture. */
    refill(target, checker);
    draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 0.1f, cb, fx, zero_v, checker_v, point);
    read_all(target, out);
    for (int i = 0; i < SIZE * SIZE; i++)
        for (int c = 0; c < 24; c += 8) same &= abs(channel(out[i], c) - channel(off[i], c)) <= 1;
    CHECK(same, "still water On equals Off through the blend");
    /* On with a 4-pixel shift: what is under the water is the shifted copy, and nothing of the unshifted frame. */
    refill(target, checker);
    draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
    read_all(target, out);
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE - 4; x++) {
            int behind = channel(checker[y * SIZE + x + 4], 16), r = channel(out[y * SIZE + x], 16),
                g = channel(out[y * SIZE + x], 8);
            int dr = abs(r - mix(0, behind, 128)), dg = abs(g - mix(255, behind, 128));
            if (dr > worst) worst = dr;
            if (dg > worst) worst = dg;
            expected &= dr <= 1 && dg <= 1;
            moved += abs(r - channel(off[y * SIZE + x], 16)) > 8;
            flips += checker[y * SIZE + x] != checker[y * SIZE + x + 4];
        }
    CHECK(expected, "shifted water = water x alpha + shifted scene x (1 - alpha), off by %d at most", worst);
    CHECK(moved == flips && flips > 0, "exactly the pixels whose shifted scene differs change: %d of %d", moved, flips);
    /* The refracted scene's share is 1 - alpha and the unshifted frame's none: pixel (4,0) is black, the one 4 over white. */
    {
        static const unsigned alphas[] = { 0x40, 0x80, 0xCC, 0xFF };
        for (int i = 0; i < 4; i++) {
            uint32_t diffuse = alphas[i] << 24 | 0x0000FF00u, p;
            double refracted, water_share;
            refill(target, checker);
            draw(rtv, vs_code, vs, water, diffuse, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
            p = pixel(target, 4, 0);
            refracted = channel(p, 16) / 255.0;
            water_share = channel(p, 8) / 255.0 - refracted;
            printf("alpha %.2f: refracted scene %.0f%%, water %.0f%%, unshifted frame %.0f%%\n", alphas[i] / 255.0,
                   refracted * 100.0, water_share * 100.0, (1.0 - refracted - water_share) * 100.0);
            CHECK(fabs(refracted - (1.0 - alphas[i] / 255.0)) <= 1.5 / 255.0 && fabs(water_share - alphas[i] / 255.0) <= 1.5 / 255.0,
                  "alpha 0x%02X: refracted %.3f", alphas[i], refracted);
        }
    }
    /* The stages at one pixel: scene, refracted scene, water colour, shader output, after the blend. */
    {
        uint32_t original, refracted, colour, shader, blended;
        ID3D11BlendState* keep = s_blend;
        s_blend = NULL;
        draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, fx, zero_v, checker_v, point);
        original = pixel(target, 4, 0);
        draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
        refracted = pixel(target, 4, 0);
        draw(rtv, vs_code, vs, water, 0xFF00FF00u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
        colour = pixel(target, 4, 0);
        draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
        shader = pixel(target, 4, 0);
        s_blend = keep;
        refill(target, checker);
        draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 0.1f, cb, fx, right_v, checker_v, point);
        blended = pixel(target, 4, 0);
        printf("pixel (4,0): scene 0x%08X, refracted 0x%08X, water 0x%08X, shader 0x%08X, after the blend 0x%08X\n", original,
               refracted, colour, shader, blended);
        CHECK(original == 0xFF000000u && refracted == 0xFFFFFFFFu && channel(shader, 24) == 255 && blended == shader,
              "the shader's alpha is 1, so the blend keeps its colour");
    }
    ID3D11BlendState_Release(s_blend);
    s_blend = NULL;
    ID3D11RenderTargetView_Release(rtv);
    ID3D11ShaderResourceView_Release(ramp_v);
    ID3D11ShaderResourceView_Release(checker_v);
    ID3D11ShaderResourceView_Release(zero_v);
    ID3D11ShaderResourceView_Release(right_v);
    ID3D11ShaderResourceView_Release(down_v);
    ID3D11Texture2D_Release(ramp_t);
    ID3D11Texture2D_Release(checker_t);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(zero_t);
    ID3D11Texture2D_Release(right_t);
    ID3D11Texture2D_Release(down_t);
}

/* A scene whose picture fills only the game's viewport (the target outside it is black): water at every edge and corner,
 * with offsets in all eight directions, must never show the black, through a real bilinear sampler. */
static void edge_tests(ID3DBlob* vs_code, ID3D11VertexShader* vs, ID3D11PixelShader* water) {
    enum { LEFT = 8, TOP = 8, RIGHT = 56, BOTTOM = 56 };
    static uint32_t scene[SIZE * SIZE], out[SIZE * SIZE];
    static const int dirs[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
    D3D11_SAMPLER_DESC sd;
    ID3D11SamplerState* bilinear = NULL;
    ID3D11Texture2D *scene_t, *target, *waves_t;
    ID3D11ShaderResourceView *scene_v, *waves_v;
    ID3D11RenderTargetView* rtv = NULL;
    uint32_t cb[8] = { 0 };
    float bounds[4], clip[20] = { 1.0f / SIZE, 1.0f / SIZE, 0.35f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 10.0f, 0, 0, 0 };
    float whole[20] = { 1.0f / SIZE, 1.0f / SIZE, 0.35f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 10.0f, 0, 0, 0, FULL_BOUNDS };
    int black_with_clip = 0, black_without = 0, identity = 1, region_pixels = 0, corners_ok = 1;
    for (int y = TOP; y < BOTTOM; y++)
        for (int x = LEFT; x < RIGHT; x++)
            scene[y * SIZE + x] = 0xFF000000u | (uint32_t)(100 + x * 2) << 16 | (uint32_t)(100 + y * 2) << 8 | 200u;
    for (int i = 0; i < SIZE * SIZE; i++)
        if (!scene[i]) scene[i] = 0xFF000000u;
    fx_water_scene_bounds(LEFT, TOP, RIGHT, BOTTOM, SIZE, SIZE, bounds);
    memcpy(clip + 16, bounds, sizeof bounds);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 0.0f;
    ID3D11Device_CreateSamplerState(dev, &sd, &bilinear);
    scene_t = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, scene, SIZE * 4);
    target = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, NULL, 0);
    scene_v = view(scene_t);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)target, NULL, &rtv);
    for (int d = 0; d < 8; d++) {
        waves_v = uniform_waves((float)dirs[d][0], (float)dirs[d][1], &waves_t);
        /* The whole copy as the sampling area: the edge pixels pull the black margin in (what the log 123 screenshot shows). */
        draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, whole, waves_v, scene_v, bilinear);
        read_all(target, out);
        for (int y = TOP; y < BOTTOM; y++)
            for (int x = LEFT; x < RIGHT; x++) black_without += channel(out[y * SIZE + x], 8) < 90;
        /* The viewport as the sampling area. */
        draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, clip, waves_v, scene_v, bilinear);
        read_all(target, out);
        for (int y = TOP; y < BOTTOM; y++)
            for (int x = LEFT; x < RIGHT; x++) {
                int g = channel(out[y * SIZE + x], 8), b = channel(out[y * SIZE + x], 0);
                black_with_clip += g < 98 || b < 198;
                region_pixels++;
            }
        corners_ok &= channel(out[TOP * SIZE + LEFT], 8) >= 98 && channel(out[(BOTTOM - 1) * SIZE + RIGHT - 1], 8) >= 98 &&
                      channel(out[TOP * SIZE + RIGHT - 1], 8) >= 98 && channel(out[(BOTTOM - 1) * SIZE + LEFT], 8) >= 98;
        ID3D11ShaderResourceView_Release(waves_v);
        ID3D11Texture2D_Release(waves_t);
    }
    CHECK(black_without > 0, "the test scene does pull black in when the whole copy is sampled: %d", black_without);
    CHECK(black_with_clip == 0 && region_pixels == 8 * (RIGHT - LEFT) * (BOTTOM - TOP), "no black enters the viewport: %d of %d",
          black_with_clip, region_pixels);
    CHECK(corners_ok, "every corner of the viewport keeps scene content");
    /* Waves that stay inside are unchanged by the bounds: the interior pixel shifted 4 px reads exactly that pixel. */
    waves_v = uniform_waves(4.0f / (SIZE * 0.35f), 0.0f, &waves_t);
    draw(rtv, vs_code, vs, water, 0x00000000u, 0.5f, 0.5f, 0.1f, cb, clip, waves_v, scene_v, bilinear);
    read_all(target, out);
    for (int y = TOP + 2; y < BOTTOM - 2; y++)
        for (int x = LEFT + 2; x < RIGHT - 6; x++) identity &= (out[y * SIZE + x] & 0xFFFFFF) == (scene[y * SIZE + x + 4] & 0xFFFFFF);
    CHECK(identity, "an offset that stays inside the viewport is applied whole");
    ID3D11ShaderResourceView_Release(waves_v);
    ID3D11Texture2D_Release(waves_t);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11ShaderResourceView_Release(scene_v);
    ID3D11Texture2D_Release(scene_t);
    ID3D11Texture2D_Release(target);
    ID3D11SamplerState_Release(bilinear);
}

/* The ambient occlusion variants: the colours of the plain shaders and, in the alpha, the share the fog leaves visible. */
static void ao_tests(ID3DBlob* vs_code, ID3D11VertexShader* vs, ID3D11PixelShader* const plain[3], ID3D11SamplerState* sampler) {
    static const char* const entries[3] = { "ps_fixed_ao", "ps_water_ao", "ps_sky_ao" };
    static uint32_t scene[SIZE * SIZE];
    const float fog_color[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float fx[20] = { 1.0f / SIZE, 1.0f / SIZE, 0.35f, 1.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.75f, 0, 0, 0, 10.0f, 0, 0, 0, FULL_BOUNDS };
    ID3D11PixelShader* ao[3] = { NULL, NULL, NULL };
    ID3D11Texture2D *target = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, NULL, 0), *behind, *still;
    ID3D11ShaderResourceView *behind_v, *still_v = uniform_waves(0.0f, 0.0f, &still);
    ID3D11RenderTargetView* rtv = NULL;
    for (int i = 0; i < SIZE * SIZE; i++) scene[i] = 0x40FF0000u;
    behind = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, scene, SIZE * 4);
    behind_v = view(behind);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)target, NULL, &rtv);
    for (int k = 0; k < 3; k++) {
        ID3DBlob* code = build(entries[k], "ps_4_0");
        CHECK(code != NULL, "%s compiles", entries[k]);
        if (!code) return;
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &ao[k]);
        ID3D10Blob_Release(code);
    }
    for (unsigned fog = 0; fog <= 255; fog += 51)
        for (int k = 0; k < 3; k++) {
            uint32_t cb[8] = { F_FOG, 0, 0, 0 }, off, on;
            const uint32_t diffuse = k == 1 ? 0x80FF8040u : 0xFFFF8040u;
            int expected = (int)fog;
            memcpy(&cb[4], fog_color, sizeof fog_color);
            s_specular = fog << 24;
            draw(rtv, vs_code, vs, plain[k], diffuse, 0.0f, 0.0f, 1.0f, cb, fx, still_v, behind_v, sampler);
            off = pixel(target, 10, 10);
            draw(rtv, vs_code, vs, ao[k], diffuse, 0.0f, 0.0f, 1.0f, cb, fx, still_v, behind_v, sampler);
            on = pixel(target, 10, 10);
            CHECK(((off ^ on) & 0xFFFFFF) == 0, "%s keeps the colours: 0x%08X, 0x%08X", entries[k], off, on);
            if (k == 1) expected = mix((int)fog, 0x40, 128);
            if (k == 2) expected = 255; /* overhead: no directional fog */
            CHECK(abs(channel(on, 24) - expected) <= 1, "%s at fog %u: visibility %d of %d", entries[k], fog, channel(on, 24), expected);
            cb[0] = 0;
            draw(rtv, vs_code, vs, ao[0], diffuse, 0.0f, 0.0f, 1.0f, cb, fx, still_v, behind_v, sampler);
            CHECK(channel(pixel(target, 10, 10), 24) == 255, "no fog state: all visible");
            cb[0] = F_FOG | F_AO_CLOSED;
            draw(rtv, vs_code, vs, ao[k == 1 ? 0 : k], diffuse, 0.0f, 0.0f, 1.0f, cb, fx, still_v, behind_v, sampler);
            CHECK(channel(pixel(target, 10, 10), 24) == 0, "a closed draw leaves nothing to darken");
        }
    s_specular = 0xFF000000u;
    for (int k = 0; k < 3; k++) ID3D11PixelShader_Release(ao[k]);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11ShaderResourceView_Release(behind_v);
    ID3D11ShaderResourceView_Release(still_v);
    ID3D11Texture2D_Release(behind);
    ID3D11Texture2D_Release(still);
    ID3D11Texture2D_Release(target);
}

int main(void) {
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    D3D_FEATURE_LEVEL level;
    ID3DBlob *vs_code, *water_code, *sky_code, *fixed_code;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader *water = NULL, *sky = NULL, *fixed = NULL;
    ID3D11SamplerState* linear = NULL;
    D3D11_SAMPLER_DESC sd;
    static uint32_t scene[SIZE * SIZE];
    float waves_right[4 * 4 * 2], waves_none[4 * 4 * 2];
    compile = dll ? (compile_fn)GetProcAddress(dll, "D3DCompile") : NULL;
    CHECK(compile != NULL, "d3dcompiler_47");
    if (!compile || FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) {
        printf("ce_gpu_tests: no WARP device\n");
        return 1;
    }
    vs_code = build("vs_tl", "vs_4_0");
    water_code = build("ps_water", "ps_4_0");
    sky_code = build("ps_sky", "ps_4_0");
    fixed_code = build("ps_fixed", "ps_4_0");
    CHECK(vs_code && water_code && sky_code && fixed_code, "the shaders compile");
    if (!vs_code || !water_code || !sky_code || !fixed_code) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_code), ID3D10Blob_GetBufferSize(vs_code), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(water_code), ID3D10Blob_GetBufferSize(water_code), NULL,
                                   &water);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(sky_code), ID3D10Blob_GetBufferSize(sky_code), NULL, &sky);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(fixed_code), ID3D10Blob_GetBufferSize(fixed_code), NULL,
                                   &fixed);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 0.0f;
    ID3D11Device_CreateSamplerState(dev, &sd, &linear);
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++) scene[y * SIZE + x] = x < SIZE / 2 ? 0xFFFF0000u : 0xFF0000FFu;
    for (int i = 0; i < 16; i++) {
        waves_right[i * 2] = 1.0f;
        waves_right[i * 2 + 1] = 0;
        waves_none[i * 2] = waves_none[i * 2 + 1] = 0;
    }
    {
        ID3D11Texture2D* behind = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, scene, SIZE * 4);
        ID3D11Texture2D* right = texture(4, 4, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE, waves_right, 32);
        ID3D11Texture2D* still = texture(4, 4, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE, waves_none, 32);
        ID3D11Texture2D* target = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, NULL, 0);
        ID3D11ShaderResourceView *behind_v = view(behind), *right_v = view(right), *still_v = view(still);
        ID3D11RenderTargetView* rtv = NULL;
        uint32_t draw_cb[8] = { 0 };
        float fx[20] = { 1.0f / SIZE, 1.0f / SIZE, 0.35f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 10.0f, 0, 0, 0, FULL_BOUNDS };
        uint32_t p;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)target, NULL, &rtv);
        /* Still water: half its green over the red scene behind it, as alpha blending the retail way. */
        draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 1.0f, draw_cb, fx, still_v, behind_v, linear);
        p = pixel(target, 10, 32);
        CHECK(close_to(p, 127, 128, 0) && channel(p, 24) == 255, "still water: 0x%08X", p);
        /* Waves to the right shift the scene by 0.35 of the target within a metre (rhw 0.1 and up): the blue half shows. */
        draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 1.0f, draw_cb, fx, right_v, behind_v, linear);
        p = pixel(target, 10, 32);
        CHECK(close_to(p, 0, 128, 127), "displaced: 0x%08X", p);
        /* Water 10 m away (rhw 0.01) moves it a tenth as far: 2 pixels, still red. */
        draw(rtv, vs_code, vs, water, 0x8000FF00u, 0.5f, 0.5f, 0.01f, draw_cb, fx, right_v, behind_v, linear);
        p = pixel(target, 10, 32);
        CHECK(close_to(p, 127, 128, 0), "far water: 0x%08X", p);
        /* Opaque water hides the scene; clear water shows it. */
        draw(rtv, vs_code, vs, water, 0xFF00FF00u, 0.5f, 0.5f, 1.0f, draw_cb, fx, right_v, behind_v, linear);
        CHECK(close_to(pixel(target, 10, 32), 0, 255, 0), "opaque water");
        draw(rtv, vs_code, vs, water, 0x0000FF00u, 0.5f, 0.5f, 1.0f, draw_cb, fx, still_v, behind_v, linear);
        CHECK(close_to(pixel(target, 10, 32), 255, 0, 0), "clear water");
        /* The game's vertex fog: the specular alpha is the share of the colour that is kept. */
        {
            static const uint32_t alphas[3] = { 0xFFu, 0x80u, 0x00u };
            const float fog_color[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            uint32_t fog_cb[8] = { F_FOG, 0, 0, 0 };
            float none[20] = { 0 };
            memcpy(&fog_cb[4], fog_color, sizeof fog_color);
            for (int i = 0; i < 3; i++) {
                s_specular = alphas[i] << 24;
                draw(rtv, vs_code, vs, fixed, 0xFFFF0000u, 0.0f, 0.0f, 1.0f, fog_cb, none, NULL, NULL, linear);
                p = pixel(target, 10, 10);
                CHECK(close_to(p, (int)alphas[i], 0, 255 - (int)alphas[i]), "fog at specular alpha %u: 0x%08X", alphas[i], p);
            }
            fog_cb[0] = 0;
            draw(rtv, vs_code, vs, fixed, 0xFFFF0000u, 0.0f, 0.0f, 1.0f, fog_cb, none, NULL, NULL, linear);
            p = pixel(target, 10, 10);
            CHECK(close_to(p, 255, 0, 0), "no fog without the state: 0x%08X", p);
            s_specular = 0xFF000000u;
        }
        /* The sky: black under white fog, v = 1/sqrt(2) between the band's edges 0.25 and 0.75 -> 0.914 fog. */
        {
            float sky_fx[20] = { 0, 0, 0, 0, 0.0f, 0.0f, 1.0f, 0.25f, 0.75f, 0, 0, 0, 0, 0, 0, 0, FULL_BOUNDS };
            float fog_white[4] = { 1, 1, 1, 1 };
            uint32_t sky_cb[8] = { F_FOG, 0, 0, 0 };
            memcpy(&sky_cb[4], fog_white, sizeof fog_white);
            draw(rtv, vs_code, vs, sky, 0xFF000000u, 1.0f, 0.0f, 1.0f, sky_cb, sky_fx, NULL, NULL, linear);
            p = pixel(target, 32, 32);
            CHECK(abs(channel(p, 16) - (int)lround(255.0 * (0.70710678 - 0.25) / 0.5)) <= 3, "sky fog by direction: 0x%08X", p);
            /* Overhead (v = 0): no fog. Equal edges: a step. */
            draw(rtv, vs_code, vs, sky, 0xFF000000u, 0.0f, 0.0f, 1.0f, sky_cb, sky_fx, NULL, NULL, linear);
            CHECK(close_to(pixel(target, 32, 32), 0, 0, 0), "overhead: no fog");
            sky_fx[7] = sky_fx[8] = 0.5f;
            draw(rtv, vs_code, vs, sky, 0xFF000000u, 1.0f, 0.0f, 1.0f, sky_cb, sky_fx, NULL, NULL, linear);
            CHECK(close_to(pixel(target, 32, 32), 255, 255, 255), "equal edges: full fog past them");
            sky_fx[7] = 0.9f;
            sky_fx[8] = 0.5f;
            draw(rtv, vs_code, vs, sky, 0xFF000000u, 1.0f, 0.0f, 1.0f, sky_cb, sky_fx, NULL, NULL, linear);
            CHECK(close_to(pixel(target, 32, 32), 0, 0, 0), "inverted edges: none below the low edge");
        }
        if (rtv) ID3D11RenderTargetView_Release(rtv);
        ID3D11ShaderResourceView_Release(behind_v);
        ID3D11ShaderResourceView_Release(right_v);
        ID3D11ShaderResourceView_Release(still_v);
        ID3D11Texture2D_Release(behind);
        ID3D11Texture2D_Release(right);
        ID3D11Texture2D_Release(still);
        ID3D11Texture2D_Release(target);
    }
    composition_tests(vs_code, vs, water, fixed, linear);
    edge_tests(vs_code, vs, water);
    {
        ID3D11PixelShader* const plain[3] = { fixed, water, sky };
        ao_tests(vs_code, vs, plain, linear);
    }
    /* The copy the water samples, from a multisampled scene: resolved, never read back by the game's path. */
    for (UINT samples = 1; samples <= 8; samples *= 2) {
        UINT quality = 0;
        ID3D11Texture2D *ms, *copy;
        ID3D11RenderTargetView* rtv = NULL;
        float red[4] = { 1, 0, 0, 1 };
        if (samples > 1 && (FAILED(ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_B8G8R8A8_UNORM, samples, &quality)) ||
                            !quality))
            continue;
        for (int round = 0; round < 3; round++) {
            ms = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, samples, D3D11_BIND_RENDER_TARGET, NULL, 0);
            copy = texture(SIZE, SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, NULL, 0);
            CHECK(ms && copy, "%ux targets", samples);
            if (!ms || !copy) break;
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)ms, NULL, &rtv);
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, red);
            if (samples > 1)
                ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource*)copy, 0, (ID3D11Resource*)ms, 0,
                                                       DXGI_FORMAT_B8G8R8A8_UNORM);
            else
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)copy, (ID3D11Resource*)ms);
            CHECK(close_to(pixel(copy, 5, 5), 255, 0, 0), "%ux scene copy, round %d", samples, round);
            ID3D11RenderTargetView_Release(rtv);
            CHECK(ID3D11Texture2D_Release(ms) == 0 && ID3D11Texture2D_Release(copy) == 0, "%ux: nothing kept alive", samples);
        }
    }
    ID3D11SamplerState_Release(linear);
    ID3D11PixelShader_Release(water);
    ID3D11PixelShader_Release(sky);
    ID3D11PixelShader_Release(fixed);
    ID3D10Blob_Release(fixed_code);
    ID3D11VertexShader_Release(vs);
    ID3D10Blob_Release(vs_code);
    ID3D10Blob_Release(water_code);
    ID3D10Blob_Release(sky_code);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf("ce_gpu_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
