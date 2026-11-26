/*
 * Ambient occlusion against the game's fog, through the real Direct3D 11 renderer on a WARP device: Direct3D 6 draws,
 * stencil marks, fog visibility, Intel ASSAO and the composition, from attach to the presented picture. The back buffer
 * is an offscreen texture: no window, no swapchain.
 */
#define COBJMACROS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <d3d11.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/d3d_translate.h"
#include "ddraw/renderer_backend.h"
#include "ddraw/sky_draw.h"
#include "guest_ui_model.h"
#include "jpt_config.h"
#include "jpt_lit.h"
#include "jpt_texhw.h"
#include "texhw_fixture.h"

#define W 320
#define H 240
#define FVF (D3DT_FVF_XYZRHW | D3DT_FVF_DIFFUSE | D3DT_FVF_SPECULAR | (1u << D3DT_FVF_TEXCOUNT_SHIFT))
#define TAN_HALF 0.57735f
#define WALL_Z 6.0f
#define WALL_BOTTOM 172.0f
#define FLOOR_Z (1.5f / TAN_HALF)
#define FOG_COLOR 0xFFC8D2DCu
#define DARK 0xFF203018u
#define LEAF_TEXELS 16

extern int g_stub_frames_allowed;
extern guest_ui_t g_stub_ui;

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the presentation the renderer draws to ---- */

static struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    unsigned generation;
    ID3D11Texture2D* back;
    ID3D11RenderTargetView* rtv;
    UINT w, h;
    unsigned long submits;
} P;

static void release_back(void) {
    if (P.rtv) ID3D11RenderTargetView_Release(P.rtv);
    if (P.back) ID3D11Texture2D_Release(P.back);
    P.rtv = NULL;
    P.back = NULL;
    P.w = P.h = 0;
}

static int make_device(void) {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    P.generation++;
    return SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &level, 1, D3D11_SDK_VERSION, &P.dev, NULL, &P.ctx));
}

static void release_device(void) {
    release_back();
    if (P.ctx) ID3D11DeviceContext_Release(P.ctx);
    if (P.dev) ID3D11Device_Release(P.dev);
    P.ctx = NULL;
    P.dev = NULL;
}

int present_device(ID3D11Device** device, ID3D11DeviceContext** context) {
    *device = P.dev;
    *context = P.ctx;
    return P.dev != NULL;
}

unsigned present_device_generation(void) { return P.generation; }

ID3D11RenderTargetView* present_target(HWND window, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC d;
    (void)window;
    if (P.rtv && P.w == width && P.h == height) return P.rtv;
    release_back();
    memset(&d, 0, sizeof d);
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(P.dev, &d, NULL, &P.back)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(P.dev, (ID3D11Resource*)P.back, NULL, &P.rtv)))
        return NULL;
    P.w = width;
    P.h = height;
    return P.rtv;
}

int present_submit(int vsync, unsigned generation) {
    (void)vsync;
    (void)generation;
    P.submits++;
    return 1;
}

int present_take_text_layer(void** texture, int* lw, int* lh, int* l, int* t, int* r, int* b) {
    (void)texture; (void)lw; (void)lh; (void)l; (void)t; (void)r; (void)b;
    return 0;
}

void d3d11_present_set_vsync(int on) { (void)on; }
void d3d11_present_set_graphics_card(const char* name) { (void)name; }
int d3d11_present_adapter(dd_adapter_t* out) { (void)out; return 0; }
void d3d11_present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                         const DDGAMMARAMP* gamma, int gamma_changed, int vsync, unsigned generation) {
    (void)window; (void)bits; (void)pitch; (void)source; (void)format; (void)gamma; (void)gamma_changed; (void)vsync;
    (void)generation;
}
void d3d11_present_release(void) {}
dd_surface_t* dd_surface_from(void* iface) { (void)iface; return NULL; }

/* ---- the game's side ---- */

typedef struct {
    float x, y, z, rhw;
    DWORD color, specular;
    float u, v;
} vertex_t;

static const jpt_win_backend_ops_t* ops = &jpt_d3d11_backend_ops;
static struct {
    dd_device_t dd;
    dd_surface_t front, target, zbuffer, leaf;
    d3d_device_t dev;
    uint16_t leaf_texels[LEAF_TEXELS * LEAF_TEXELS];
} G;

static void configure(unsigned box_w, unsigned box_h, unsigned msaa) {
    jpt_win_renderer_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.requested = JPT_RENDERER_D3D11;
    cfg.filter = D3DR_FILTER_POINT;
    cfg.box_w = box_w;
    cfg.box_h = box_h;
    cfg.msaa = msaa;
    ops->configure(&cfg);
}

static void gamma_ramp(double exponent) {
    for (int i = 0; i < 256; i++)
        G.dd.gamma.red[i] = G.dd.gamma.green[i] = G.dd.gamma.blue[i] = (WORD)(pow(i / 255.0, exponent) * 255.0 + 0.5) * 257;
}

/* Binary leaves in stripes (ARGB1555): opaque green, transparent gaps. */
static void make_leaf(void) {
    for (int y = 0; y < LEAF_TEXELS; y++)
        for (int x = 0; x < LEAF_TEXELS; x++)
            G.leaf_texels[y * LEAF_TEXELS + x] = ((x / 2 + y / 2) & 1) ? 0x8000u | (4 << 10) | (8 << 5) | 3 : 0u;
    G.leaf.width = G.leaf.height = LEAF_TEXELS;
    G.leaf.caps = DDSCAPS_TEXTURE;
    G.leaf.format.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    G.leaf.format.dwRGBBitCount = 16;
    G.leaf.format.dwRBitMask = 0x7C00;
    G.leaf.format.dwGBitMask = 0x03E0;
    G.leaf.format.dwBBitMask = 0x001F;
    G.leaf.format.dwRGBAlphaBitMask = 0x8000;
    G.leaf.pitch = LEAF_TEXELS * 2;
    G.leaf.bits = (BYTE*)G.leaf_texels;
    G.leaf.device = &G.dd;
}

static void set(DWORD state, DWORD value) {
    if (G.dev.render[state] == value) return;
    if (d3dt_render_change_flushes(state, G.dev.render[state], value)) ops->state_will_change(&G.dev, D3DR_CHANGE_RENDER, state, value);
    else ops->state_kept_batch(&G.dev);
    G.dev.render[state] = value;
}

static void default_states(void) {
    G.dev.texture = NULL;
    set(D3DRENDERSTATE_ZENABLE, 1);
    set(D3DRENDERSTATE_ZWRITEENABLE, 1);
    set(D3DRENDERSTATE_ZFUNC, 7);
    set(D3DRENDERSTATE_CULLMODE, 1);
    set(D3DRENDERSTATE_FILLMODE, 3);
    set(D3DRENDERSTATE_SHADEMODE, 2);
    set(D3DRENDERSTATE_TEXTUREMAPBLEND, 2);
    set(D3DRENDERSTATE_SRCBLEND, 5);
    set(D3DRENDERSTATE_DESTBLEND, 6);
    set(D3DRENDERSTATE_ALPHABLENDENABLE, 0);
    set(D3DRENDERSTATE_ALPHATESTENABLE, 0);
    set(D3DRENDERSTATE_ALPHAFUNC, 5);
    set(D3DRENDERSTATE_ALPHAREF, 0x1000);
    set(D3DRENDERSTATE_FOGENABLE, 1);
    set(D3DRENDERSTATE_FOGCOLOR, FOG_COLOR);
    G.dev.stage[0][D3DTSS_MAGFILTER] = G.dev.stage[0][D3DTSS_MINFILTER] = 1;
    G.dev.stage[0][D3DTSS_ADDRESSU] = G.dev.stage[0][D3DTSS_ADDRESSV] = 1;
}

static int open_game(void) {
    memset(&G.dev, 0, sizeof G.dev);
    memset(&G.target, 0, sizeof G.target);
    G.target.device = G.front.device = &G.dd;
    G.target.width = G.front.width = W;
    G.target.height = G.front.height = H;
    G.target.pitch = W * 2;
    G.target.bits = (BYTE*)calloc(W * H, 2);
    G.target.lock_rect.right = W;
    G.target.lock_rect.bottom = H;
    G.target.format.dwFlags = DDPF_RGB;
    G.target.format.dwRGBBitCount = 16;
    G.target.format.dwRBitMask = 0xF800;
    G.target.format.dwGBitMask = 0x07E0;
    G.target.format.dwBBitMask = 0x001F;
    G.target.zbuffer = &G.zbuffer;
    G.target.next = &G.front;
    G.front.next = &G.target;
    G.dd.window = (HWND)(UINT_PTR)1;
    G.dev.dd = &G.dd;
    G.dev.target = &G.target;
    return SUCCEEDED(ops->attach(&G.dev));
}

static void close_game(void) {
    ops->detach(&G.dev);
    free(G.target.bits);
    G.target.bits = NULL;
}

/* A quad of depths z_top (its upper edge) to z_bottom metres, its vertices stored as the game stores them. */
static void quad(float x0, float y0, float x1, float y1, float z_top, float z_bottom, DWORD color, unsigned top, unsigned bottom) {
    static const WORD indices[6] = { 0, 1, 2, 0, 2, 3 };
    const float s0 = 0.1f / z_top, s1 = 0.1f / z_bottom;
    vertex_t v[4] = { { x0, y0, s0, s0, color, top << 24, 0, 0 },
                      { x1, y0, s0, s0, color, top << 24, 8, 0 },
                      { x1, y1, s1, s1, color, bottom << 24, 8, 8 },
                      { x0, y1, s1, s1, color, bottom << 24, 0, 8 } };
    ops->draw(&G.dev, D3DT_PT_TRIANGLELIST, FVF, v, 4, indices, 6);
}

static void room(DWORD color, unsigned visible) {
    quad(0, 0, W, WALL_BOTTOM, WALL_Z, WALL_Z, color, visible, visible);
    quad(0, WALL_BOTTOM, W, H, WALL_Z, FLOOR_Z, color, visible, visible);
}

typedef void (*scene_fn)(unsigned parameter);
static uint32_t* g_picture[2];
static size_t g_picture_pixels;

static void frame(scene_fn scene, unsigned parameter) {
    D3DRECT full = { 0, 0, W, H };
    G.dev.in_scene = 1;
    default_states();
    ops->begin_scene(&G.dev);
    ops->clear(&G.dev, 1, &full, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, FOG_COLOR, 0.0f);
    scene(parameter);
    ops->end_scene(&G.dev);
    G.dev.in_scene = 0;
    ops->flip(&G.front, 0);
}

static int read_back(int slot) {
    ID3D11Texture2D* staging = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!P.back) return 0;
    ID3D11Texture2D_GetDesc(P.back, &d);
    if (g_picture_pixels != (size_t)d.Width * d.Height) {
        g_picture_pixels = (size_t)d.Width * d.Height;
        for (int i = 0; i < 2; i++) g_picture[i] = (uint32_t*)realloc(g_picture[i], g_picture_pixels * 4);
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(P.dev, &d, NULL, &staging))) return 0;
    ID3D11DeviceContext_CopyResource(P.ctx, (ID3D11Resource*)staging, (ID3D11Resource*)P.back);
    if (SUCCEEDED(ID3D11DeviceContext_Map(P.ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++)
            memcpy(g_picture[slot] + (size_t)y * d.Width, (const BYTE*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
        ID3D11DeviceContext_Unmap(P.ctx, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return 1;
}

/* The scene's second frame at an AO level: a level only applies to a frame drawn wholly with it. */
static int render(int slot, int level, scene_fn scene, unsigned parameter) {
    ops->configure_ao(level);
    frame(scene, parameter);
    frame(scene, parameter);
    return read_back(slot);
}

static int channel(uint32_t p, int shift) { return (int)((p >> shift) & 0xFF); }

/* The largest difference between the two pictures inside a box given in the game's pixels. */
static int difference(int x0, int y0, int x1, int y1) {
    const int sx = (int)(P.w / W), sy = (int)(P.h / H);
    int worst = 0;
    for (int y = y0 * sy; y < y1 * sy; y++)
        for (int x = x0 * sx; x < x1 * sx; x++)
            for (int c = 0; c < 24; c += 8) {
                int d = abs(channel(g_picture[0][y * P.w + x], c) - channel(g_picture[1][y * P.w + x], c));
                if (d > worst) worst = d;
            }
    return worst;
}

static int darkening(void) { return difference(W / 3, (int)WALL_BOTTOM - 6, W * 2 / 3, (int)WALL_BOTTOM + 6); }
static int anywhere(void) { return difference(0, 0, W, H); }

static void pair(int level, scene_fn scene, unsigned parameter) {
    CHECK(render(0, JPT_AO_OFF, scene, parameter) && render(1, level, scene, parameter), "both frames were presented");
}

/* ---- scenes ---- */

static DWORD s_room_color = DARK;

static void scene_room(unsigned visible) { room(s_room_color, visible); }

static void scene_room_no_fog(unsigned visible) {
    set(D3DRENDERSTATE_FOGENABLE, 0);
    room(s_room_color, visible);
}

/* The room half near and clear, half behind the fog. */
static void scene_halves(unsigned unused) {
    (void)unused;
    quad(0, 0, W / 2, WALL_BOTTOM, WALL_Z, WALL_Z, DARK, 255, 255);
    quad(0, WALL_BOTTOM, W / 2, H, WALL_Z, FLOOR_Z, DARK, 255, 255);
    quad(W / 2, 0, W, WALL_BOTTOM, WALL_Z, WALL_Z, DARK, 0, 0);
    quad(W / 2, WALL_BOTTOM, W, H, WALL_Z, FLOOR_Z, DARK, 0, 0);
}

enum { LEAVES_VISIBLE = 0xFF, LEAVES_BLEND = 0x100, LEAVES_OVER_ROOM = 0x200 };

static void scene_leaves(unsigned parameter) {
    unsigned visible = parameter & LEAVES_VISIBLE;
    if (parameter & LEAVES_OVER_ROOM) room(DARK, visible);
    set(D3DRENDERSTATE_ALPHATESTENABLE, 1);
    set(D3DRENDERSTATE_ALPHABLENDENABLE, (parameter & LEAVES_BLEND) != 0);
    G.dev.texture = &G.leaf;
    quad(60, 40, 260, 200, 3.0f, 3.0f, 0xFFFFFFFFu, visible, visible);
    quad(80, 30, 240, 190, 2.7f, 3.3f, 0xFFFFFFFFu, visible, visible);
}

/* A depth-writing translucent pane (water, glass) in front of a room. */
static void scene_pane(unsigned visible) {
    room(DARK, visible);
    set(D3DRENDERSTATE_ALPHABLENDENABLE, 1);
    quad(40, 60, 280, WALL_BOTTOM, 5.8f, 5.8f, 0x80406080u, visible, visible);
    quad(40, WALL_BOTTOM, 280, 220, 5.8f, 3.0f, 0x80406080u, visible, visible);
}

/* The sky and other draws without depth writes over the whole room, opaque then translucent. */
static void scene_closed(unsigned blend) {
    room(DARK, 255);
    set(D3DRENDERSTATE_ZWRITEENABLE, 0);
    set(D3DRENDERSTATE_ZFUNC, 8);
    set(D3DRENDERSTATE_ALPHABLENDENABLE, blend != 0);
    quad(0, 0, W, H, 4.0f, 4.0f, blend ? 0x60FF8020u : 0xFF6080FFu, 200, 90);
}

/* Slanted posts of a fully fogged colour before a sky that is not the fog's colour: only their edges could show. */
static void scene_posts(unsigned visible) {
    static const WORD indices[6] = { 0, 1, 2, 0, 2, 3 };
    set(D3DRENDERSTATE_ZWRITEENABLE, 0);
    quad(0, 0, W, H, 900.0f, 900.0f, 0xFF3060A0u, 255, 255);
    set(D3DRENDERSTATE_ZWRITEENABLE, 1);
    for (int i = 0; i < 6; i++) {
        const float x = 20.0f + 48.0f * (float)i, s = 0.1f / (4.0f + 0.2f * (float)(i & 1));
        vertex_t v[4] = { { x, 10, s, s, DARK, visible << 24, 0, 0 }, { x + 22, 10, s, s, DARK, visible << 24, 0, 0 },
                          { x + 45, 230, s, s, DARK, visible << 24, 0, 0 }, { x + 23, 230, s, s, DARK, visible << 24, 0, 0 } };
        ops->draw(&G.dev, D3DT_PT_TRIANGLELIST, FVF, v, 4, indices, 6);
    }
}

static void scene_crossing_posts(unsigned visible) {
    scene_posts(visible);
    quad(0, 100, W, 140, 3.9f, 3.9f, DARK, visible, visible);
}

static void scene_clear_only(unsigned unused) { (void)unused; }

/* ---- tests ---- */

static void test_fog_levels(void) {
    for (int level = JPT_AO_LOW; level <= JPT_AO_ULTRA; level++) {
        int last = 256, full = 0;
        pair(level, scene_room_no_fog, 255);
        full = darkening();
        CHECK(full >= 4, "%s, fog off: the corner darkens (%d)", jpt_ao_names[level], full);
        for (int visible = 255; visible >= 0; visible -= 51) {
            int d;
            pair(level, scene_room, (unsigned)visible);
            d = darkening();
            CHECK(d <= last, "%s: darkening fades with the visibility (%d after %d at %d)", jpt_ao_names[level], d, last, visible);
            if (visible == 255) CHECK(d == full, "%s: unfogged geometry keeps its whole occlusion (%d, %d)", jpt_ao_names[level], d, full);
            if (visible == 0) CHECK(anywhere() == 0, "%s: full fog is left as it is (%d)", jpt_ao_names[level], anywhere());
            last = d;
        }
    }
}

/* The picture AO before the fog would give: the object's light occluded, the fog's light whole. */
static void test_fog_exact(void) {
    static double ao[W * H][3];
    static const DWORD colors[] = { DARK, 0xFFFFFFFFu, 0xFF806040u };
    for (int k = 0; k < 3; k++) {
        s_room_color = colors[k];
        pair(JPT_AO_HIGH, scene_room, 255);
        for (int i = 0; i < W * H; i++)
            for (int c = 0; c < 3; c++) {
                double off = channel(g_picture[0][i], c * 8);
                ao[i][c] = off > 0 ? channel(g_picture[1][i], c * 8) / off : 1.0;
            }
        for (unsigned visible = 32; visible <= 224; visible += 64) {
            double over = 0, under = 0;
            pair(JPT_AO_HIGH, scene_room, visible);
            for (int i = 0; i < W * H; i++)
                for (int c = 0; c < 3; c++) {
                    double object = channel(colors[k], c * 8) * visible / 255.0, v = visible / 255.0;
                    double off = channel(g_picture[0][i], c * 8), on = channel(g_picture[1][i], c * 8);
                    double exact = object * (1.0 - ao[i][c]), bounded = fmin(exact, off * v * (1.0 - ao[i][c]));
                    over = fmax(over, (off - on) - exact);
                    under = fmax(under, bounded - (off - on));
                }
            CHECK(over <= 4.0, "colour %06lX, visibility %u: the fog itself is never darkened (%.1f too dark)",
                  (unsigned long)(colors[k] & 0xFFFFFFu), visible, over);
            CHECK(under <= 4.0, "colour %06lX, visibility %u: the visible part keeps its occlusion (%.1f missing)",
                  (unsigned long)(colors[k] & 0xFFFFFFu), visible, under);
        }
    }
    s_room_color = DARK;
}

static void test_near_and_far(void) {
    pair(JPT_AO_MEDIUM, scene_halves, 0);
    CHECK(difference(40, (int)WALL_BOTTOM - 6, 120, (int)WALL_BOTTOM + 6) >= 4, "the near half keeps its occlusion");
    CHECK(difference(W / 2 + 1, 0, W, H) == 0, "the fogged half shows none: %d", difference(W / 2 + 1, 0, W, H));
}

static void test_vegetation(void) {
    for (unsigned blend = 0; blend <= LEAVES_BLEND; blend += LEAVES_BLEND)
        for (unsigned over = 0; over <= LEAVES_OVER_ROOM; over += LEAVES_OVER_ROOM) {
            const char* what = blend ? "blended leaves" : "alpha-tested leaves";
            int clear, half;
            pair(JPT_AO_MEDIUM, scene_leaves, 255 | blend | over);
            clear = difference(70, 50, 250, 180);
            CHECK(clear >= 3, "%s: occluded where they are visible (%d)", what, clear);
            pair(JPT_AO_MEDIUM, scene_leaves, 128 | blend | over);
            half = difference(70, 50, 250, 180);
            CHECK(half <= clear, "%s: less in fog (%d, %d)", what, half, clear);
            pair(JPT_AO_MEDIUM, scene_leaves, 0 | blend | over);
            CHECK(anywhere() == 0, "%s: none in full fog (%d)", what, anywhere());
        }
}

static void test_translucent(void) {
    int clear;
    pair(JPT_AO_HIGH, scene_pane, 255);
    clear = darkening();
    CHECK(clear >= 3, "a translucent pane that writes depth keeps its occlusion (%d)", clear);
    pair(JPT_AO_HIGH, scene_pane, 100);
    CHECK(darkening() < clear, "and less of it in fog (%d, %d)", darkening(), clear);
    pair(JPT_AO_HIGH, scene_pane, 0);
    CHECK(anywhere() == 0, "and none in full fog (%d)", anywhere());
}

/* A custom level's picture in place of a texture: told for the surface, gone when it is written, and kept to its format's alpha. */
static dd_surface_t s_crate;
static uint16_t s_crate_texels[16 * 16];

static void scene_crate(unsigned alpha) {
    set(D3DRENDERSTATE_ALPHATESTENABLE, alpha);
    set(D3DRENDERSTATE_ALPHAFUNC, 5);
    set(D3DRENDERSTATE_ALPHAREF, 0);
    G.dev.texture = &s_crate;
    quad(0, 0, W, H, WALL_Z, WALL_Z, 0xFFFFFFFFu, 255, 255);
}

static uint32_t centre(void) { return g_picture[0][(size_t)(P.h / 2) * P.w + P.w / 2] & 0xFFFFFFu; }
static int near_colour(uint32_t colour) {
    for (int c = 0; c < 24; c += 8)
        if (abs(channel(centre(), c) - channel(colour, c)) > 8) return 0;
    return 1;
}

/* Upright, so u goes right and v down; the light's direction differs from corner to corner. */
static jpt_lit_vertex_t s_lit[4];
static jpt_lit_draw_t s_lit_draw = { 0, 4, 0x00101820u, 0x00F0E0D0u, s_lit };
static int s_lit_on;

static void scene_lit(unsigned fog) {
    const float s = 0.1f / WALL_Z;
    const vertex_t v[4] = { { 0, 0, s, s, 0xFF808080u, fog << 24, 0, 0 }, { W, 0, s, s, 0xFF808080u, fog << 24, 8, 0 }, { W, H, s, s, 0xFF808080u, fog << 24, 8, 8 }, { 0, H, s, s, 0xFF808080u, fog << 24, 0, 8 } };
    set(D3DRENDERSTATE_ALPHATESTENABLE, 0);
    G.dev.texture = &s_crate;
    jpt_lit_set_draw(s_lit_on ? &s_lit_draw : NULL);
    ops->draw(&G.dev, D3DT_PT_TRIANGLEFAN, FVF, v, 4, NULL, 0);
    jpt_lit_set_draw(NULL);
}

static int lit_like(const float texture[4], const float normal[4], unsigned fog) {
    const uint32_t seen = centre();
    float want[3], dark[3], full[3];
    for (int k = 0; k < 3; k++) dark[k] = (float)channel(s_lit_draw.dark, 16 - 8 * k) / 255.0f, full[k] = (float)channel(s_lit_draw.full, 16 - 8 * k) / 255.0f;
    jpt_lit_pixel(&s_lit[0], dark, full, texture, normal, want);
    for (int k = 0; k < 3; k++) {
        const float at = want[k] < 1.0f ? want[k] : 1.0f, there = (float)fog / 255.0f;
        const float shown = at * 255.0f * there + (float)channel(FOG_COLOR, 16 - 8 * k) * (1.0f - there);
        if (fabsf(shown - (float)channel(seen, 16 - 8 * k)) > 4.0f) return 0;
    }
    return 1;
}

/* crate has the normal map (1, 0, 0) with half the highlight, large one straight out. */
static void test_lit(int crate, int large) {
    static const float k_lights[4][4] = { { 0, 0, 1, 0.7f }, { 0.8f, 0, 0.6f, 0.9f }, { -0.8f, 0, 0.6f, 0.9f }, { 0.6f, 0.8f, 0, 0.5f } };
    static const float k_materials[2][4] = { { 0.9f, 0, 0, 0.2f }, { 0.8f, 1.6f, 0.5f, 0.3f } };
    const float blue[4] = { 0, 0, 1, 1 }, green[4] = { 32 / 255.0f, 192 / 255.0f, 64 / 255.0f, 1 }, right[4] = { 1, 128 / 255.0f, 128 / 255.0f, 128 / 255.0f }, out[4] = { 128 / 255.0f, 128 / 255.0f, 1, 1 };
    s_lit_on = 1;
    for (int m = 0; m < 2; m++)
        for (int l = 0; l < 4; l++) {
            for (int i = 0; i < 4; i++) {
                memcpy(s_lit[i].light, k_lights[l], sizeof s_lit[i].light);
                memcpy(s_lit[i].material, k_materials[m], sizeof s_lit[i].material);
                s_lit[i].shade[0] = 0.7f, s_lit[i].shade[1] = 0.15f;
            }
            s_crate.override_id = crate, s_lit_draw.normal = jpt_texhw_normal(1);
            CHECK(render(0, JPT_AO_OFF, scene_lit, 255) && lit_like(blue, right, 255), "light %d, material %d: %06X", l, m, centre());
            s_crate.override_id = large, s_lit_draw.normal = jpt_texhw_normal(3);
            CHECK(render(0, JPT_AO_OFF, scene_lit, 100) && lit_like(green, out, 100), "in fog, light %d, material %d: %06X", l, m, centre());
            CHECK(render(0, JPT_AO_HIGH, scene_lit, 255) && lit_like(green, out, 255), "with Ambient Occlusion, light %d, material %d: %06X", l, m, centre());
        }
    s_lit_draw.normal = 0;
    CHECK(render(0, JPT_AO_OFF, scene_lit, 255) && near_colour(0x106020), "no normal map: the vertices' colours: %06X", centre());
    s_lit_draw.normal = jpt_texhw_normal(3), s_crate.override_id = 0;
    CHECK(render(0, JPT_AO_OFF, scene_lit, 255) && near_colour(0x800000), "the game's own texture: %06X", centre());
    s_lit_on = 0, s_crate.override_id = large;
    CHECK(render(0, JPT_AO_OFF, scene_lit, 255) && near_colour(0x106020), "lit by the engine: %06X", centre());
}

static void test_override(void) {
    static const char* const k_files[] = { "data/texhw/Map/w/crate.dds", "data/texhw/Map/leaf.tga", "data/texhw/Map/large.tga", "data/texhw/Map/w/crate.norm.tga", "data/texhw/Map/large.norm.tga" };
    static uint8_t file[4 * 512 * 512 + 64];
    char user[512], dir[600];
    const char* temp = getenv("TEMP");
    size_t size;
    int crate, leaf, large;
    snprintf(user, sizeof user, "%s/jpt_texhw_warp", temp ? temp : ".");
    snprintf(dir, sizeof dir, "%s/custom_levels/Tex", user);
    size = fx_dds_head(file, 8, 8, 0, "DXT1");
    for (int i = 0; i < 4; i++, size += 8) memset(file + size, 0, 8), fx_put(file + size, 0x001F, 2);
    CHECK(fx_file(dir, "texhw/Map/w/crate.dds", file, size), "a DXT1 picture");
    size = fx_tga(file, 32, 32, 0x2000FF00u);
    CHECK(fx_file(dir, "texhw/Map/leaf.tga", file, size), "a picture that is mostly clear");
    size = fx_tga(file, 512, 512, 0xFF20C040u);
    CHECK(fx_file(dir, "texhw/Map/large.tga", file, size), "a large picture");
    size = fx_tga(file, 16, 16, 0x80FF8080u);
    CHECK(fx_file(dir, "texhw/Map/w/crate.norm.tga", file, size), "a normal map");
    size = fx_tga(file, 512, 512, 0xFF8080FFu);
    CHECK(fx_file(dir, "texhw/Map/large.norm.tga", file, size), "a large one");
    CHECK(fx_activate(user, k_files, 5), "the package");
    jpt_texhw_name(1, "Map\\w\\crate.bmp");
    jpt_texhw_name(2, "Map\\w\\leaf.bmp");
    jpt_texhw_name(3, "Map\\w\\large.bmp");
    crate = jpt_texhw_find(1), leaf = jpt_texhw_find(2), large = jpt_texhw_find(3);
    CHECK(crate && leaf && large, "each has its override");
    jpt_texhw_frame();

    s_crate = G.leaf;
    s_crate.gpu_texture = NULL;
    s_crate.format.dwFlags = DDPF_RGB;
    s_crate.format.dwRBitMask = 0xF800;
    s_crate.format.dwGBitMask = 0x07E0;
    s_crate.format.dwBBitMask = 0x001F;
    s_crate.format.dwRGBAlphaBitMask = 0;
    s_crate.bits = (BYTE*)s_crate_texels;
    for (int i = 0; i < 16 * 16; i++) s_crate_texels[i] = 0xF800;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 0) && near_colour(0xFF0000), "the game's own texture: %06X", centre());
    s_crate.override_id = crate;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 0) && near_colour(0x0000FF), "the picture in its place: %06X", centre());
    s_crate.override_id = large;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 0) && near_colour(0x20C040), "one larger than the shared arrays take: %06X", centre());
    s_crate.override_id = leaf;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 1) && near_colour(0x00FF00), "alpha is nothing to an opaque texture: %06X", centre());
    test_lit(crate, large);
    ops->texture_loaded(&s_crate, &s_crate);
    s_crate.override_id = 0;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 0) && near_colour(0xFF0000), "the game's own again: %06X", centre());

    s_crate.override_id = 0;
    ops->surface_freed(&s_crate);
    s_crate = G.leaf;
    s_crate.gpu_texture = NULL;
    s_crate.override_id = leaf;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 1) && near_colour(FOG_COLOR & 0xFFFFFF), "below half, a cutout is not there: %06X", centre());
    s_crate.override_id = crate;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 1) && near_colour(0x0000FF), "a picture without alpha is solid: %06X", centre());

    close_game();
    ops->device_lost();
    release_device();
    CHECK(make_device() && ops->startup() && open_game(), "another device");
    s_crate.gpu_texture = NULL;
    CHECK(render(0, JPT_AO_OFF, scene_crate, 1) && near_colour(0x0000FF), "the picture on the new device: %06X", centre());
    jpt_custom_level_deactivate();
    CHECK(render(0, JPT_AO_OFF, scene_crate, 0) && !near_colour(0x0000FF), "nothing of it after the session: %06X", centre());
    G.dev.texture = NULL;
    ops->surface_freed(&s_crate);
}

static void test_never_darkened(void) {
    pair(JPT_AO_ULTRA, scene_closed, 0);
    CHECK(anywhere() == 0, "the sky (no depth writes) is identical with AO on: %d", anywhere());
    pair(JPT_AO_ULTRA, scene_closed, 1);
    CHECK(anywhere() == 0, "a blended draw gives the same colours through the second source: %d", anywhere());
    pair(JPT_AO_ULTRA, scene_clear_only, 0);
    CHECK(anywhere() == 0, "a frame without 3D is untouched: %d", anywhere());
    g_stub_ui = GUEST_UI_MENU;
    pair(JPT_AO_ULTRA, scene_room, 255);
    CHECK(anywhere() == 0, "a menu's frame is untouched: %d", anywhere());
    g_stub_ui = GUEST_UI_GAMEPLAY;
}

static void test_off_and_toggles(void) {
    pair(JPT_AO_OFF, scene_room, 160);
    CHECK(anywhere() == 0, "Off is the same picture before and after");
    render(0, JPT_AO_OFF, scene_room, 160);
    render(1, JPT_AO_ULTRA, scene_room, 160);
    render(1, JPT_AO_OFF, scene_room, 160);
    CHECK(anywhere() == 0, "Off after Ultra is Off again: %d", anywhere());
    render(0, JPT_AO_HIGH, scene_room, 160);
    render(1, JPT_AO_HIGH, scene_room, 160);
    CHECK(anywhere() == 0, "the same frame twice is the same picture: %d", anywhere());
}

static void test_gamma(void) {
    gamma_ramp(0.6);
    pair(JPT_AO_MEDIUM, scene_room, 255);
    CHECK(darkening() >= 4, "with a gamma ramp the corner darkens");
    pair(JPT_AO_MEDIUM, scene_room, 0);
    CHECK(anywhere() == 0, "with a gamma ramp full fog is left as it is: %d", anywhere());
    gamma_ramp(1.0);
}

static void test_msaa(void) {
    for (unsigned samples = 1; samples <= 8; samples *= 2) {
        UINT quality = 0;
        int clear;
        if (samples > 1 &&
            (FAILED(ID3D11Device_CheckMultisampleQualityLevels(P.dev, DXGI_FORMAT_B8G8R8A8_UNORM, samples, &quality)) || !quality)) {
            printf("WARP has no %ux MSAA: skipped\n", samples);
            continue;
        }
        close_game();
        configure(W, H, samples);
        CHECK(open_game(), "%ux target", samples);
        pair(JPT_AO_MEDIUM, scene_room, 255);
        CHECK(darkening() >= 4, "%ux: the corner darkens", samples);
        pair(JPT_AO_MEDIUM, scene_room, 0);
        CHECK(anywhere() == 0, "%ux: full fog is left as it is (%d)", samples, anywhere());
        pair(JPT_AO_HIGH, scene_crossing_posts, 255);
        clear = anywhere();
        CHECK(clear >= 1, "%ux: crossing posts occlude each other (%d)", samples, clear);
        pair(JPT_AO_HIGH, scene_crossing_posts, 0);
        CHECK(anywhere() == 0, "%ux: no silhouette edge returns in full fog (%d)", samples, anywhere());
        pair(JPT_AO_HIGH, scene_leaves, 0 | LEAVES_BLEND | LEAVES_OVER_ROOM);
        CHECK(anywhere() == 0, "%ux: leaves stay in full fog (%d)", samples, anywhere());
    }
    close_game();
    configure(W, H, 1);
    CHECK(open_game(), "1x target again");
}

static void test_resize_and_device(void) {
    static const unsigned boxes[][2] = { { 640, 480 }, { 960, 720 }, { 320, 240 } };
    for (int i = 0; i < 3; i++) {
        close_game();
        configure(boxes[i][0], boxes[i][1], 1);
        CHECK(open_game(), "%ux%u target", boxes[i][0], boxes[i][1]);
        pair(JPT_AO_MEDIUM, scene_room, 255);
        CHECK(P.w == boxes[i][0] && darkening() >= 4, "%ux%u: the corner darkens", boxes[i][0], boxes[i][1]);
        pair(JPT_AO_MEDIUM, scene_room, 0);
        CHECK(anywhere() == 0, "%ux%u: full fog is left as it is", boxes[i][0], boxes[i][1]);
    }
    ops->device_lost();
    release_device();
    CHECK(make_device(), "a second device");
    CHECK(render(1, JPT_AO_MEDIUM, scene_room, 255), "frames on the new device");
    pair(JPT_AO_MEDIUM, scene_room, 255);
    CHECK(darkening() >= 4, "the corner darkens on the new device");
    pair(JPT_AO_MEDIUM, scene_leaves, 0 | LEAVES_BLEND);
    CHECK(anywhere() == 0, "full fog is left as it is on the new device");
}

/* The layers of a custom sky, as sky_draw.c gives them to the renderer. */
HRESULT d3dr_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices, DWORD index_count) {
    return ops->draw(dev, type, fvf, vertices, count, indices, index_count);
}
void d3dr_state_will_change(d3d_device_t* dev, int change, DWORD state, DWORD value) { ops->state_will_change(dev, change, state, value); }
void d3dr_surface_freed(dd_surface_t* s) { ops->surface_freed(s); }
int sky_layers_rect(const void* vertices, unsigned count, float rect[4]) {
    (void)vertices;
    rect[0] = rect[1] = 0.0f, rect[2] = W, rect[3] = H;
    return count == 4;
}

static void* g_sky_gpu;

static void sky_quad(sky_emit_fn emit, void* user, jpt_sky_blend_t blend, int fog, const jpt_picture_t* picture, float x0, float x1, uint32_t diffuse, uint32_t clear) {
    const int twice = blend == JPT_SKY_ALPHA_2X;
    const jpt_sky_pass_t pass = { twice ? JPT_SKY_ALPHA : blend, fog, 0, 0, twice };
    const float x[6] = { x0, x1, x1, x0, x1, x0 }, y[6] = { 0, 0, H, 0, H, H };
    jpt_sky_screen_vertex_t v[6];
    sky_draw_t draw;
    for (int i = 0; i < 6; i++) {
        const jpt_sky_screen_vertex_t at = { x[i], y[i], 0.5f, 0.001f, diffuse, clear << 24, 0.5f, 0.5f };
        v[i] = at;
    }
    draw.pass = &pass;
    draw.picture = picture;
    draw.gpu = &g_sky_gpu;
    draw.vertices = v;
    draw.count = 6;
    emit(user, &draw);
}

void sky_layers_emit(const float rect[4], sky_emit_fn emit, void* user) {
    static uint8_t texels[4] = { 0, 128, 0, 128 }; /* half green, half there */
    const jpt_picture_t picture = { 1, 1, 1, 1, texels, sizeof texels };
    (void)rect;
    sky_quad(emit, user, JPT_SKY_OPAQUE, 0, NULL, 0, W, 0xFF404040u, 255);
    sky_quad(emit, user, JPT_SKY_ALPHA, 0, &picture, 0, W / 8, 0xFFFFFFFFu, 255);
    sky_quad(emit, user, JPT_SKY_ALPHA_2X, 0, &picture, W / 8, W / 4, 0xFFFFC0FFu, 255);
    sky_quad(emit, user, JPT_SKY_BURN, 0, &picture, W / 4, W / 2, 0xFFFFFFFFu, 255);
    sky_quad(emit, user, JPT_SKY_OPAQUE, 1, NULL, W / 2, W * 3 / 4, 0xFFFFFFFFu, 0);
}

static int s_sky_kept;

static void scene_sky(unsigned vertices) {
    DWORD render[D3D_RENDER_STATES];
    G.dev.texture = &G.leaf;
    memcpy(render, G.dev.render, sizeof render);
    sky_draw_layers(&G.dev, NULL, vertices);
    s_sky_kept = !memcmp(render, G.dev.render, sizeof render) && G.dev.texture == &G.leaf && G.dev.stage[0][D3DTSS_ADDRESSU] == 1 && G.dev.stage[0][D3DTSS_ADDRESSV] == 1;
}

static uint32_t sky_pixel(int eighth) { return g_picture[0][(size_t)(P.h / 2) * P.w + (size_t)P.w * (unsigned)(2 * eighth + 1) / 16u]; }

static int like(uint32_t pixel, int r, int g, int b) { return abs(channel(pixel, 16) - r) <= 9 && abs(channel(pixel, 8) - g) <= 9 && abs(channel(pixel, 0) - b) <= 9; }

static void test_sky_layers(void) {
    const int f[3] = { (FOG_COLOR >> 16) & 255, (FOG_COLOR >> 8) & 255, FOG_COLOR & 255 };
    CHECK(render(0, JPT_AO_OFF, scene_sky, 4) && s_sky_kept, "the sky's layers leave the device as it was");
    CHECK(like(sky_pixel(0), 0x20, 0x60, 0x20), "alpha: %06X", sky_pixel(0) & 0xFFFFFF);
    CHECK(like(sky_pixel(1), 0x20, 0x80, 0x20), "twice the colour: %06X", sky_pixel(1) & 0xFFFFFF);
    CHECK(like(sky_pixel(3), 0x40, 0x80, 0x40), "burn: %06X", sky_pixel(3) & 0xFFFFFF);
    CHECK(like(sky_pixel(5), f[0], f[1], f[2]), "fog: %06X", sky_pixel(5) & 0xFFFFFF);
    CHECK(like(sky_pixel(7), 0x40, 0x40, 0x40), "opaque: %06X", sky_pixel(7) & 0xFFFFFF);
    CHECK(render(0, JPT_AO_HIGH, scene_sky, 4) && like(sky_pixel(0), 0x20, 0x60, 0x20) && like(sky_pixel(7), 0x40, 0x40, 0x40), "the same with Ambient Occlusion");
    CHECK(render(0, JPT_AO_OFF, scene_sky, 3) && like(sky_pixel(7), f[0], f[1], f[2]), "nothing without the view's corners");
    CHECK(g_sky_gpu != NULL, "the layer's texture is kept");
    sky_draw_free(g_sky_gpu);
    g_sky_gpu = NULL;
    CHECK(render(0, JPT_AO_OFF, scene_sky, 4) && like(sky_pixel(0), 0x20, 0x60, 0x20), "and made again");
    sky_draw_free(g_sky_gpu);
    g_sky_gpu = NULL;
}

int main(void) {
    g_stub_frames_allowed = 1;
    g_stub_ui = GUEST_UI_GAMEPLAY;
    gamma_ramp(1.0);
    make_leaf();
    if (!make_device()) {
        printf("FAIL ao_fog_tests: no WARP device at feature level 11.0\n");
        return 1;
    }
    configure(W, H, 1);
    ops->configure_ao(JPT_AO_MEDIUM);
    ops->configure_effects(0, 0);
    if (!ops->startup() || !open_game()) {
        printf("FAIL ao_fog_tests: the renderer did not start\n");
        return 1;
    }
    test_fog_levels();
    test_fog_exact();
    test_near_and_far();
    test_vegetation();
    test_translucent();
    test_never_darkened();
    test_off_and_toggles();
    test_gamma();
    test_msaa();
    test_resize_and_device();
    test_override();
    test_sky_layers();
    CHECK(P.submits > 0, "frames were presented");
    close_game();
    ops->device_lost();
    release_device();
    printf("ao_fog_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
