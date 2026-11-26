/* Direct3D 6 on Linux, called through its guest COM objects the way the game calls it and drawn into a recorder instead of a GPU:
 * the vtables against Microsoft's d3d.h (order and stack size of every method), HRESULTs and guest writes, the device's life, the
 * states and draws (fog, textures, water, sky), and the picture the CPU and the GPU share. */
#include <stdlib.h>

#include "ce_effects.h"
#include "jpt_lit.h"
#include "jpt_texhw.h"
#include "linux_d3d.h"
#include "linux_ddraw.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"
#include "texhw_fixture.h"

#define W 640u
#define H 480u
#define RT_W 1280u
#define RT_H 960u
#define OK 0u
#define INVALIDPARAMS 0x80070057u
#define UNSUPPORTED 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define SCENE_IN_SCENE 0x887602F8u
#define SCENE_NOT_IN_SCENE 0x887602F9u
#define VIEWPORTHASNODEVICE 0x88760306u
#define NOCURRENTVIEWPORT 0x88760307u
#define VIEWPORTDATANOTSET 0x88760305u
#define INVALIDCAPS 0x88760064u
#define CANNOTATTACH 0x8876000Au
#define FVF_TLVERTEX 0x1C4u
#define TEXTURE_SYSTEM 0x1800u
#define TEXTURE_VIDEO 0x5000u
#define ZBUFFER_CAPS 0x24000u
#define FAN 6u

enum { D3_RELEASE = 2, D3_ENUMDEVICES = 3, D3_CREATEVIEWPORT = 6, D3_CREATEDEVICE = 8, D3_ENUMZ = 10, D3_EVICT = 11 };
enum { DV_QI = 0, DV_ADDREF = 1, DV_RELEASE = 2, DV_GETCAPS = 3, DV_ADDVIEWPORT = 5, DV_ENUMTEX = 8, DV_BEGIN = 9, DV_END = 10, DV_GETD3D = 11,
       DV_SETVIEWPORT = 12, DV_GETVIEWPORT = 13, DV_SETTARGET = 14, DV_GETTARGET = 15, DV_GETRS = 21, DV_SETRS = 22, DV_SETTRANSFORM = 25,
       DV_GETTRANSFORM = 26, DV_DRAW = 28, DV_DRAWINDEXED = 29, DV_GETTEXTURE = 37, DV_SETTEXTURE = 38, DV_GETTSS = 39, DV_SETTSS = 40,
       DV_VALIDATE = 41 };
enum { VP_RELEASE = 2, VP_GET2 = 16, VP_SET2 = 17, VP_CLEAR2 = 20 };
enum { TX_RELEASE = 2, TX_LOAD = 5 };
enum { DD_QI = 0, DD_RELEASE = 2, DD_CREATESURFACE = 6, DD_GETCAPS = 11, DD_SETCOOP = 20, DD_SETMODE = 21 };
enum { S_QI = 0, S_RELEASE = 2, S_ADDATTACHED = 3, S_BLT = 5, S_DELATTACHED = 8, S_FLIP = 11, S_GETATTACHED = 12, S_GETDC = 17, S_LOCK = 25,
       S_RELEASEDC = 26, S_SETCOLORKEY = 29, S_UNLOCK = 32 };
enum { RS_ZENABLE = 7, RS_ZWRITE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_ALPHABLEND = 27, RS_FOGENABLE = 28, RS_SPECULAR = 29,
       RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_ZBIAS = 47 };

/* ---- the recorder ---- */

#define MAX_TEX 64
static struct {
    int available, open_ok;
    unsigned generation, begins;
    unsigned draws, clears, overlays, opaque_overlays, reads, last_reads, creates, updates, partials, destroys, waves, sky, ao;
    jpt_render_state_t state;
    jpt_render_vertex_t v[8];
    uint32_t count, index_count, wave_slot, wave_w;
    int prim, scissor[5];
    uint32_t clear_argb;
    float wave0, bounds[4], sky_params[5];
    jpt_render_rect_t overlay, dirty;
    uint32_t overlay_texel, desc_w, desc_levels, desc_key;
    const uint8_t* unused;
    int ao_level, ao_gameplay, has_waves, alive[MAX_TEX];
    uint32_t texel0[MAX_TEX];
    uint16_t scene;
} G;

static int g_available(void) { return G.available; }
static int g_begin(uint32_t w, uint32_t h) {
    (void)w, (void)h;
    G.begins++;
    return G.open_ok;
}
static void g_size(uint32_t* w, uint32_t* h) { *w = RT_W, *h = RT_H; }
static int g_mips(void) { return 1; }
static uint64_t g_create(const jpt_render_tex_desc_t* d, const void* p) {
    uint64_t id = ++G.creates;
    G.desc_w = d->width, G.desc_levels = d->mip_levels, G.desc_key = d->has_colorkey ? d->colorkey_lo : 0xFFFFFFFFu;
    if (id < MAX_TEX) G.alive[id] = 1, memcpy(&G.texel0[id], p, 4);
    return id;
}
static int g_update(uint64_t id, const jpt_render_tex_desc_t* d, const void* p, const jpt_render_rect_t* r) {
    G.updates++;
    G.desc_w = d->width, G.desc_levels = d->mip_levels;
    if (id < MAX_TEX && !r) memcpy(&G.texel0[id], p, 4);
    if (r) G.partials++, G.dirty = *r;
    return 1;
}
static int g_alive(uint64_t id) { return id < MAX_TEX && G.alive[id]; }
static void g_destroy(uint64_t id) {
    G.destroys++;
    if (id < MAX_TEX) G.alive[id] = 0;
}
static void g_scissor(int on, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    G.scissor[0] = on, G.scissor[1] = x, G.scissor[2] = y, G.scissor[3] = (int)w, G.scissor[4] = (int)h;
}
static int g_draw(const jpt_render_state_t* s, int prim, const jpt_render_vertex_t* v, uint32_t n, const uint16_t* ix, uint32_t ixn) {
    (void)ix;
    G.draws++;
    G.state = *s, G.prim = prim, G.count = n, G.index_count = ixn;
    memcpy(G.v, v, (n < 8 ? n : 8) * sizeof *v);
    return 1;
}
static void g_clear(int color, uint32_t argb, int depth, float z) {
    (void)color, (void)depth, (void)z;
    G.clears++;
    G.clear_argb = argb;
}
static void g_select(uint32_t slot) { G.wave_slot = slot; }
static void g_water(float s, float m, const float b[4], const float* waves, uint32_t w, uint32_t h) {
    (void)s, (void)m, (void)h;
    memcpy(G.bounds, b, sizeof G.bounds);
    G.has_waves = waves != NULL;
    G.wave_w = w;
    if (waves) G.waves++, G.wave0 = waves[0];
}
static void g_sky(const float s[5]) {
    G.sky++;
    memcpy(G.sky_params, s, sizeof G.sky_params);
}
static void g_ao(int level, float t, int gameplay) {
    (void)t;
    G.ao++;
    G.ao_level = level, G.ao_gameplay = gameplay;
}
static int g_read(const jpt_render_rect_t* box, uint32_t w, uint32_t h, uint16_t* out, size_t pitch) {
    (void)w, (void)h;
    G.reads++;
    for (uint32_t y = 0; y < box->h; y++)
        for (uint32_t x = 0; x < box->w; x++) out[y * pitch + x] = G.scene;
    return 1;
}
static int g_read_last(uint16_t* out, uint32_t w, uint32_t h, size_t pitch) {
    G.last_reads++;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) out[y * pitch + x] = 0x1111;
    return 1;
}
static int g_overlay(const uint32_t* bgra, size_t pitch, const jpt_render_rect_t* box, uint32_t w, uint32_t h, int opaque) {
    (void)pitch, (void)w, (void)h;
    G.overlays++;
    G.opaque_overlays += opaque != 0;
    G.overlay = *box;
    G.overlay_texel = bgra[0];
    return 1;
}
static unsigned g_generation(void) { return G.generation; }

static const lx_gpu_ops_t k_ops = { "Recorder", g_available, g_begin, g_size, g_mips, g_create, g_update, g_alive, g_destroy, g_scissor, g_draw,
                                    g_clear, g_select, g_water, g_sky, g_ao, g_read, g_read_last, g_overlay, g_generation };

/* ---- helpers ---- */

static uint32_t s_hwnd, s_dd, s_dd4, s_d3d, s_dev, s_vp, s_front, s_back, s_z;
static uint32_t s_out, s_at;

static uint32_t scratch(uint32_t bytes) {
    uint32_t va = s_at;
    s_at += (bytes + 15u) & ~15u;
    return va;
}

static uint32_t guid(uint32_t d1, uint16_t d2, uint16_t d3, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g, uint8_t h) {
    uint8_t v[16] = { 0 };
    const uint8_t tail[8] = { a, b, c, d, e, f, g, h };
    uint32_t at = scratch(16);
    memcpy(v, &d1, 4), memcpy(v + 4, &d2, 2), memcpy(v + 6, &d3, 2), memcpy(v + 8, tail, 8);
    gp_copy_out(at, v, sizeof v);
    return at;
}

static uint32_t iid_d3d3(void) { return guid(0xBB223240, 0xE72B, 0x11D0, 0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E); }
static uint32_t iid_tex2(void) { return guid(0x93281502, 0x8CF8, 0x11D0, 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29); }
static uint32_t iid_dd4(void) { return guid(0x9C59509A, 0x39BD, 0x11D1, 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5); }
static uint32_t iid_surface4(void) { return guid(0x0B2B8630, 0xAD35, 0x11D0, 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B); }
static uint32_t iid_hal(void) { return guid(0x84E63DE0, 0x46AA, 0x11CF, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E); }

static uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* DDSURFACEDESC2 */
static uint32_t desc4(uint32_t flags, uint32_t caps, uint32_t w, uint32_t h, uint32_t back, uint32_t pf_flags, uint32_t bits, uint32_t r, uint32_t g, uint32_t b,
                      uint32_t a) {
    uint32_t d[31] = { 0 }, at = scratch(124);
    d[0] = 124, d[1] = flags, d[2] = h, d[3] = w, d[5] = back;
    d[18] = 32, d[19] = pf_flags, d[21] = bits, d[22] = r, d[23] = g, d[24] = b, d[25] = a, d[26] = caps;
    gp_copy_out(at, d, sizeof d);
    return at;
}

static uint32_t texture(uint32_t caps, uint32_t w, uint32_t h) {
    com(s_dd4, DD_CREATESURFACE, 4, desc4(0x1007u, caps, w, h, 0, 0x41u, 16, 0x0F00, 0x00F0, 0x000F, 0xF000), s_out, 0u);
    return rd(s_out);
}

static uint32_t texture_face(uint32_t surface) {
    return com(surface, S_QI, 3, iid_tex2(), s_out) == OK ? rd(s_out) : 0;
}

static uint32_t lock(uint32_t surface, uint32_t* pitch) {
    uint32_t d[31] = { 124 }, at = scratch(124);
    gp_copy_out(at, d, sizeof d);
    if (com(surface, S_LOCK, 5, 0u, at, 1u, 0u) != OK) return 0;
    if (pitch) *pitch = rd(at + 16);
    return rd(at + 36);
}

static uint16_t* pixels(uint32_t va, uint32_t bytes) { return (uint16_t*)gm_ptr_checked(va, bytes, 1); }

static void write_texel(uint32_t surface, uint32_t x, uint32_t y, uint16_t value) {
    uint32_t pitch = 0, va = lock(surface, &pitch);
    *pixels(va + y * pitch + x * 2, 2) = value;
    com(surface, S_UNLOCK, 2, 0u);
}

static uint32_t vertices(float x, float y, float z, uint32_t diffuse, uint32_t specular) {
    uint32_t at = scratch(4 * 32);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t v[8] = { f2u(x + (i == 1 || i == 2 ? 100.0f : 0.0f)), f2u(y + (i >= 2 ? 100.0f : 0.0f)), f2u(z), f2u(0.5f), diffuse, specular, f2u(0.25f), f2u(0.75f) };
        gp_copy_out(at + i * 32, v, sizeof v);
    }
    return at;
}

static uint32_t draw(void) { return com(s_dev, DV_DRAW, 6, FAN, FVF_TLVERTEX, vertices(10, 20, 0.5f, 0x80FF0000u, 0x40112233u), 4u, 0x1Cu); }
static void state(uint32_t which, uint32_t value) { CHECK(com(s_dev, DV_SETRS, 3, which, value) == OK, "SetRenderState(%u, %u)", which, value); }
static void blend(int on) { state(RS_ALPHABLEND, (uint32_t)on), state(RS_SRCBLEND, 5), state(RS_DESTBLEND, 6); }
static uint32_t clear_all(uint32_t color) {
    uint32_t at = scratch(16), r[4] = { 0, 0, W, H };
    gp_copy_out(at, r, sizeof r);
    return com(s_vp, VP_CLEAR2, 7, 1u, at, 3u, color, f2u(0.0f), 0u);
}

/* ---- A. the ABI ---- */

static const char* const k_golden[LX_D3D_INTERFACES] = {
    /* IDirect3D3 */
    "QueryInterface/3 AddRef/1 Release/1 EnumDevices/3 CreateLight/3 CreateMaterial/3 CreateViewport/3 FindDevice/3 CreateDevice/5 "
    "CreateVertexBuffer/5 EnumZBufferFormats/4 EvictManagedTextures/1 ",
    /* IDirect3DDevice3 */
    "QueryInterface/3 AddRef/1 Release/1 GetCaps/3 GetStats/2 AddViewport/2 DeleteViewport/2 NextViewport/4 EnumTextureFormats/3 BeginScene/1 "
    "EndScene/1 GetDirect3D/2 SetCurrentViewport/2 GetCurrentViewport/2 SetRenderTarget/3 GetRenderTarget/2 Begin/4 BeginIndexed/6 Vertex/2 "
    "Index/2 End/2 GetRenderState/3 SetRenderState/3 GetLightState/3 SetLightState/3 SetTransform/3 GetTransform/3 MultiplyTransform/3 "
    "DrawPrimitive/6 DrawIndexedPrimitive/8 SetClipStatus/2 GetClipStatus/2 DrawPrimitiveStrided/6 DrawIndexedPrimitiveStrided/8 "
    "DrawPrimitiveVB/6 DrawIndexedPrimitiveVB/6 ComputeSphereVisibility/6 GetTexture/3 SetTexture/3 GetTextureStageState/4 "
    "SetTextureStageState/4 ValidateDevice/2 ",
    /* IDirect3DViewport3 */
    "QueryInterface/3 AddRef/1 Release/1 Initialize/2 GetViewport/2 SetViewport/2 TransformVertices/5 LightElements/3 SetBackground/2 "
    "GetBackground/3 SetBackgroundDepth/2 GetBackgroundDepth/3 Clear/4 AddLight/2 DeleteLight/2 NextLight/4 GetViewport2/2 SetViewport2/2 "
    "SetBackgroundDepth2/2 GetBackgroundDepth2/3 Clear2/7 ",
    /* IDirect3DTexture2 */
    "QueryInterface/3 AddRef/1 Release/1 GetHandle/3 PaletteChanged/3 Load/2 ",
};
static const char* const k_interface[LX_D3D_INTERFACES] = { "IDirect3D3::", "IDirect3DDevice3::", "IDirect3DViewport3::", "IDirect3DTexture2::" };

static void test_vtables(void) {
    for (int k = 0; k < LX_D3D_INTERFACES; k++) {
        unsigned n = 0;
        const lx_d3d_method_t* m = lx_d3d_methods((lx_d3d_interface_t)k, &n);
        uint32_t vtable = lx_d3d_vtable((lx_d3d_interface_t)k);
        char text[2048] = "";
        size_t at = 0, prefix = strlen(k_interface[k]);
        for (unsigned i = 0; i < n; i++) {
            CHECK(!strncmp(m[i].name, k_interface[k], prefix), "%s is named after its interface", m[i].name);
            at += (size_t)snprintf(text + at, sizeof text - at, "%s/%d ", m[i].name + prefix, m[i].argc);
        }
        CHECK(!strcmp(text, k_golden[k]), "%s methods, order and stack size are d3d.h's:\n%s", k_interface[k], text);
        CHECK(vtable == lx_d3d_vtable((lx_d3d_interface_t)k), "one vtable per interface");
        for (unsigned i = 0; i < n; i++) {
            uint32_t entry = rd(vtable + 4 * i), esp = g_stack + 0x8000, before = esp, eax = 0, edx = 0;
            CHECK(ht_name(entry) && !strcmp(ht_name(entry), m[i].name), "slot %u of %s is %s (%s)", i, k_interface[k], m[i].name, ht_name(entry) ? ht_name(entry) : "?");
            for (int a = 0; a <= m[i].argc; a++) gp_write_u32(esp + 4 * (uint32_t)a, 0);
            CHECK(ht_invoke(entry, &esp, &eax, &edx) && esp - before == 4u * (uint32_t)(m[i].argc + 1), "%s pops its %d arguments (%u bytes)", m[i].name, m[i].argc,
                  esp - before);
            if (!m[i].fn) CHECK(eax == UNSUPPORTED, "%s is refused, never a false success (0x%08X)", m[i].name, eax);
            else if (i == 1 || i == 2) CHECK(eax == 0, "%s of no object counts nothing", m[i].name);
            else CHECK((int32_t)eax < 0, "%s on no object fails (0x%08X)", m[i].name, eax);
        }
    }
}

static void test_essential_slots(void) {
    static const struct {
        lx_d3d_interface_t which;
        unsigned slot;
        const char* name;
    } k[] = {
        { LX_D3D_DEVICE3, 9, "IDirect3DDevice3::BeginScene" },       { LX_D3D_DEVICE3, 10, "IDirect3DDevice3::EndScene" },
        { LX_D3D_DEVICE3, 22, "IDirect3DDevice3::SetRenderState" },  { LX_D3D_DEVICE3, 28, "IDirect3DDevice3::DrawPrimitive" },
        { LX_D3D_DEVICE3, 29, "IDirect3DDevice3::DrawIndexedPrimitive" }, { LX_D3D_DEVICE3, 38, "IDirect3DDevice3::SetTexture" },
        { LX_D3D_DEVICE3, 40, "IDirect3DDevice3::SetTextureStageState" }, { LX_D3D_VIEWPORT3, 17, "IDirect3DViewport3::SetViewport2" },
        { LX_D3D_VIEWPORT3, 20, "IDirect3DViewport3::Clear2" },      { LX_D3D_TEXTURE2, 5, "IDirect3DTexture2::Load" },
        { LX_D3D_DIRECT3D3, 8, "IDirect3D3::CreateDevice" },         { LX_D3D_DIRECT3D3, 10, "IDirect3D3::EnumZBufferFormats" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        unsigned n;
        const lx_d3d_method_t* m = lx_d3d_methods(k[i].which, &n);
        CHECK(k[i].slot < n && m[k[i].slot].fn && !strcmp(m[k[i].slot].name, k[i].name), "slot %u is %s and is implemented", k[i].slot, k[i].name);
    }
}

/* ---- B. without a GPU, and the device's life ---- */

static void make_window(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 }, name = scratch(32), at = scratch(64);
    wc[9] = put_string(name, "D3dTest");
    gp_copy_out(at, wc, sizeof wc);
    U32(RegisterClassA, 1, at);
    s_hwnd = U32(CreateWindowExA, 12, 0u, name, put_string(scratch(16), "d3d"), 0x90080000u, 0u, 0u, W, H, 0u, 0u, 0x00400000u, 0u);
}

static void test_software(void) {
    uint32_t out = scratch(16), caps = scratch(380);
    CHECK(DDC(DirectDrawCreate, 3, 0u, out, 0u) == OK, "DirectDrawCreate");
    s_dd = rd(out);
    CHECK(com(s_dd, DD_QI, 3, iid_dd4(), out) == OK, "IDirectDraw4");
    s_dd4 = rd(out);
    CHECK(!lx_d3d_hardware() && com(s_dd, DD_QI, 3, iid_d3d3(), out) == E_NOINTERFACE && rd(out) == 0, "no Direct3D without a GPU renderer");
    gp_write_u32(caps, 380);
    CHECK(com(s_dd4, DD_GETCAPS, 3, caps, 0u) == OK && !(rd(caps + 4) & 1u) && rd(caps + 56) == 0 && !(rd(caps + 364) & 0x2000u), "and no 3D caps");
    CHECK(com(s_dd, DD_SETCOOP, 3, s_hwnd, 0x11u) == OK && com(s_dd, DD_SETMODE, 4, W, H, 16u) == OK, "640x480x16");
    CHECK(com(s_dd4, DD_CREATESURFACE, 4, desc4(0x1007u, ZBUFFER_CAPS, W, H, 0, 0x400u, 16, 0, 0xFFFF, 0, 0), out, 0u) == INVALIDCAPS, "and no z-buffer");
    G.available = G.open_ok = 1;
    CHECK(!lx_d3d_hardware(), "still none: no operations are installed");
    lx_d3d_set_gpu(&k_ops);
    CHECK(lx_d3d_hardware(), "a GPU renderer offers Direct3D");
}

static void test_device(void) {
    uint32_t out = scratch(16), caps = scratch(380), hal = scratch(252), hel = scratch(252), vp_data = scratch(44), d[11], zcaps = scratch(16);
    lx_d3d_stats_t st;
    gp_write_u32(caps, 380);
    CHECK(com(s_dd4, DD_GETCAPS, 3, caps, 0u) == OK && (rd(caps + 4) & 1u) && rd(caps + 56) == 0x600u && (rd(caps + 364) & 0x23000u) == 0x23000u, "3D, z-buffer and texture caps");
    CHECK(com(s_dd4, DD_QI, 3, iid_d3d3(), out) == OK, "IDirect3D3");
    s_d3d = rd(out);
    CHECK(com(s_d3d, 0, 3, iid_dd4(), out) == OK && rd(out) == s_dd4 && com(s_dd4, DD_RELEASE, 1) > 0, "the DirectDraw object behind it");
    g_fake_enum_count = 0;
    CHECK(com(s_d3d, D3_ENUMZ, 4, 0u, FAKE_ENUMCB, 0u) == OK && g_fake_enum_count == 2 && g_fake_enum_first_dword[0] == 16 && g_fake_enum_first_dword[1] == 24,
          "EnumZBufferFormats calls back with 16 and 24 bits (%u)", g_fake_enum_count);
    CHECK(com(s_d3d, D3_ENUMZ, 4, 0u, 0u, 0u) == INVALIDPARAMS, "and wants a callback");
    g_fake_enum_device[0] = 0;
    CHECK(com(s_d3d, D3_ENUMDEVICES, 3, FAKE_ENUMDEVICE, 0x77u) == OK && g_fake_enum_device[0] == 1 && g_fake_enum_device[1] == 252 &&
              (g_fake_enum_device[2] & 0x80000u) && g_fake_enum_device[3] == 0x77u && g_fake_enum_device[4] == 252,
          "EnumDevices calls back once with a hardware device of 252-byte descriptions");
    CHECK(com(s_dd4, DD_CREATESURFACE, 4, desc4(0x21u, 0x2218u, 0, 0, 1, 0, 0, 0, 0, 0, 0), out, 0u) == OK, "the primary chain (3DDEVICE)");
    s_front = rd(out);
    gp_write_u32(zcaps, 4);
    CHECK(com(s_front, S_GETATTACHED, 3, zcaps, out) == OK, "the back buffer");
    s_back = rd(out);
    CHECK(com(s_dd4, DD_CREATESURFACE, 4, desc4(0x1007u, ZBUFFER_CAPS, 320, 240, 0, 0x400u, 16, 0, 0xFFFF, 0, 0), out, 0u) == OK, "a small z-buffer");
    s_z = rd(out);
    CHECK(com(s_back, S_ADDATTACHED, 2, s_z) == CANNOTATTACH && com(s_z, S_RELEASE, 1) == 0, "of another size does not attach");
    CHECK(com(s_dd4, DD_CREATESURFACE, 4, desc4(0x1007u, ZBUFFER_CAPS, W, H, 0, 0x400u, 24, 0, 0xFFFFFF, 0, 0), out, 0u) == OK, "a 24-bit z-buffer");
    s_z = rd(out);
    CHECK(com(s_back, S_ADDATTACHED, 2, s_z) == OK && com(s_back, S_ADDATTACHED, 2, s_z) == 0x8876019Au, "attaches once");
    gp_write_u32(zcaps, 0x20000);
    CHECK(com(s_back, S_GETATTACHED, 3, zcaps, out) == OK && rd(out) == s_z && com(s_z, S_RELEASE, 1) == 2, "and is found by its caps");
    CHECK(com(s_d3d, D3_CREATEDEVICE, 5, iid_hal(), 0u, out, 0u) == INVALIDPARAMS && rd(out) == 0, "CreateDevice wants a surface");
    CHECK(com(s_d3d, D3_CREATEDEVICE, 5, iid_hal(), s_back, out, 0u) == OK, "CreateDevice");
    s_dev = rd(out);
    CHECK(com(s_d3d, D3_CREATEDEVICE, 5, iid_hal(), s_back, out, 0u) != OK && rd(out) == 0, "one device per surface");
    CHECK(com(s_dev, DV_GETRS, 3, (uint32_t)RS_ZENABLE, out) == OK && rd(out) == 1 && com(s_dev, DV_GETRS, 3, 23u, out) == OK && rd(out) == 4, "its default states");
    gp_write_u32(hal, 252), gp_write_u32(hel, 252);
    CHECK(com(s_dev, DV_GETCAPS, 3, hal, hel) == OK && rd(hal + 100 + 8) == 0x200B1u && rd(hal + 180) == 2048 && rd(hel + 4) == 0, "GetCaps writes both descriptions");
    gp_write_u32(hal, 200);
    CHECK(com(s_dev, DV_GETCAPS, 3, hal, hel) == INVALIDPARAMS && com(s_dev, DV_GETCAPS, 3, 0u, 0u) == INVALIDPARAMS, "only of the right size");
    g_fake_enum_count = 0;
    CHECK(com(s_dev, DV_ENUMTEX, 3, FAKE_ENUMCB, 0u) == OK && g_fake_enum_count == 3 && g_fake_enum_first_dword[2] == 16, "EnumTextureFormats: three 16-bit formats");
    CHECK(com(s_d3d, D3_CREATEVIEWPORT, 3, out, 0u) == OK, "CreateViewport");
    s_vp = rd(out);
    CHECK(clear_all(0) == VIEWPORTHASNODEVICE, "a viewport of no device cannot clear");
    CHECK(com(s_dev, DV_SETVIEWPORT, 2, s_vp) == INVALIDPARAMS && com(s_dev, DV_GETVIEWPORT, 2, out) == NOCURRENTVIEWPORT && rd(out) == 0, "nor be current");
    CHECK(com(s_dev, DV_ADDVIEWPORT, 2, 0u) == INVALIDPARAMS && com(s_dev, DV_ADDVIEWPORT, 2, s_vp) == OK && com(s_dev, DV_SETVIEWPORT, 2, s_vp) == OK, "AddViewport, SetCurrentViewport");
    gp_write_u32(vp_data, 44);
    CHECK(com(s_vp, VP_GET2, 2, vp_data) == VIEWPORTDATANOTSET, "no data yet");
    memset(d, 0, sizeof d);
    d[0] = 44, d[1] = 10, d[2] = 20, d[3] = 600, d[4] = 400, d[10] = f2u(1.0f);
    gp_copy_out(vp_data, d, sizeof d);
    CHECK(com(s_vp, VP_SET2, 2, vp_data) == OK && com(s_vp, VP_GET2, 2, vp_data) == OK && rd(vp_data + 12) == 600, "SetViewport2");
    gp_write_u32(vp_data, 40);
    CHECK(com(s_vp, VP_SET2, 2, vp_data) == INVALIDPARAMS, "of the right size");
    CHECK(com(s_dev, DV_GETTARGET, 2, out) == OK && com(rd(out), S_RELEASE, 1) > 0, "GetRenderTarget hands a counted reference");
    CHECK(com(s_dev, DV_SETTARGET, 3, s_back, 0u) == UNSUPPORTED && com(s_dev, 4, 2, out) == UNSUPPORTED, "what the game never calls is refused");
    CHECK(com(s_dev, DV_ADDREF, 1) == 2 && com(s_dev, DV_RELEASE, 1) == 1, "reference counts");
    CHECK(com(s_dev, DV_VALIDATE, 2, out) == OK && rd(out) == 1 && com(s_d3d, D3_EVICT, 1) == OK, "ValidateDevice, EvictManagedTextures");
    lx_d3d_stats(&st);
    CHECK(st.devices == 1 && st.viewports == 1, "one device, one viewport");
}

/* ---- C. scenes, states and draws ---- */

static uint32_t s_level_fog;
static int level_fog(uint32_t* color) {
    *color = s_level_fog;
    return 1;
}

static void test_scene_and_draws(void) {
    uint32_t out = scratch(16), at, ix = scratch(16);
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 }, bad[3] = { 0, 1, 4 };
    float m[16];
    lx_d3d_stats_t st;
    CHECK(draw() == SCENE_NOT_IN_SCENE && G.draws == 0, "no draw outside a scene");
    CHECK(com(s_dev, DV_END, 1) == SCENE_NOT_IN_SCENE && com(s_dev, DV_BEGIN, 1) == OK && com(s_dev, DV_BEGIN, 1) == SCENE_IN_SCENE, "BeginScene, EndScene");
    CHECK(draw() == OK && G.draws == 1 && G.prim == (int)FAN && G.count == 4 && G.index_count == 0, "DrawPrimitive");
    CHECK(G.v[0].sx == 20.0f && G.v[0].sy == 40.0f && G.v[0].sz == 0.5f && G.v[0].rhw == 0.5f && G.v[0].diffuse == 0x80FF0000u && G.v[0].specular == 0x40112233u &&
              G.v[0].tu == 0.25f && G.v[0].tv == 0.75f,
          "vertices at the target's scale, colours and the fog factor (specular alpha) untouched");
    CHECK(G.scissor[0] == 1 && G.scissor[1] == 20 && G.scissor[2] == 40 && G.scissor[3] == 1200 && G.scissor[4] == 800, "the viewport is the scissor");
    gp_copy_out(ix, indices, sizeof indices);
    at = vertices(0, 0, 0, 0, 0);
    CHECK(com(s_dev, DV_DRAWINDEXED, 8, 4u, FVF_TLVERTEX, at, 4u, ix, 6u, 0x1Cu) == OK && G.draws == 2 && G.index_count == 6, "DrawIndexedPrimitive");
    gp_copy_out(ix, bad, sizeof bad);
    CHECK(com(s_dev, DV_DRAWINDEXED, 8, 4u, FVF_TLVERTEX, at, 4u, ix, 3u, 0x1Cu) == INVALIDPARAMS && G.draws == 2, "an index past the vertices is refused");
    CHECK(com(s_dev, DV_DRAW, 6, FAN, FVF_TLVERTEX, 0x10u, 4u, 0u) == INVALIDPARAMS && com(s_dev, DV_DRAW, 6, FAN, FVF_TLVERTEX, 0u, 4u, 0u) == INVALIDPARAMS, "and so is memory that is not there");
    CHECK(com(s_dev, DV_DRAW, 6, FAN, 0x112u, at, 4u, 0u) == INVALIDPARAMS && com(s_dev, DV_DRAW, 6, 9u, FVF_TLVERTEX, at, 4u, 0u) == INVALIDPARAMS, "untransformed vertices and unknown primitives too");
    CHECK(com(s_dev, DV_DRAW, 6, FAN, FVF_TLVERTEX, at, 2u, 0u) == OK && G.draws == 2, "two vertices draw nothing");

    CHECK(com(s_dev, DV_SETRS, 3, 200u, 1u) == INVALIDPARAMS && com(s_dev, DV_SETRS, 3, 0x1000u, 1u) == INVALIDPARAMS, "unknown states are refused");
    CHECK(com(s_dev, DV_SETRS, 3, (uint32_t)RS_FOGTABLEMODE, 3u) == INVALIDPARAMS && com(s_dev, DV_SETRS, 3, (uint32_t)RS_ZBIAS, 17u) == INVALIDPARAMS &&
              com(s_dev, DV_SETRS, 3, (uint32_t)RS_SRCBLEND, 0u) == INVALIDPARAMS,
          "and values Direct3D 6 does not have");
    lx_d3d_stats(&st);
    CHECK(st.refused_states == 5, "refused states are counted (%llu)", st.refused_states);

    state(RS_FOGENABLE, 1), state(RS_FOGCOLOR, 0xFF203040u), state(RS_SPECULAR, 1), state(RS_ZWRITE, 0);
    draw();
    CHECK(G.state.fog_enable == 1 && G.state.fog_color == 0xFF203040u && G.state.specular_enable == 1 && G.state.z_write == 0, "fog, its colour, specular and depth reach the draw");
    state(RS_FOGENABLE, 0), state(RS_FOGCOLOR, 0xFF010203u);
    draw();
    CHECK(G.state.fog_enable == 0 && G.state.fog_color == 0xFF010203u, "and change in the middle of a scene");
    state(RS_FOGENABLE, 1), state(RS_SPECULAR, 0), state(RS_ZWRITE, 1);
    CHECK(com(s_dev, DV_END, 1) == OK, "EndScene");
    lx_d3d_set_fog_sync(level_fog);
    s_level_fog = 0x00A0B0C0u;
    com(s_dev, DV_BEGIN, 1);
    draw();
    CHECK(G.state.fog_color == 0xFFA0B0C0u, "a scene starts with the level's fog colour (0x%08X)", G.state.fog_color);
    com(s_dev, DV_END, 1);
    s_level_fog = 0x00112233u;
    com(s_dev, DV_BEGIN, 1);
    draw();
    CHECK(G.state.fog_color == 0xFF112233u, "which follows a level change");
    lx_d3d_set_fog_sync(NULL);

    CHECK(com(s_dev, DV_SETTSS, 4, 0u, 12u, 3u) == OK, "stage 0 ADDRESS clamp");
    draw();
    CHECK(G.state.tex_addr_u == 3 && G.state.tex_addr_v == 3, "sets both axes");
    CHECK(com(s_dev, DV_SETTSS, 4, 0u, 12u, 4u) == INVALIDPARAMS && com(s_dev, DV_SETTSS, 4, 9u, 12u, 1u) == INVALIDPARAMS && com(s_dev, DV_SETTSS, 4, 0u, 1u, 1u) == INVALIDPARAMS, "invalid stage states are refused");
    CHECK(com(s_dev, DV_GETTSS, 4, 0u, 13u, out) == OK && rd(out) == 3, "GetTextureStageState");
    CHECK(com(s_dev, DV_SETTSS, 4, 0u, 12u, 1u) == OK && com(s_dev, DV_SETTSS, 4, 0u, 16u, 2u) == OK && com(s_dev, DV_SETTSS, 4, 0u, 17u, 2u) == OK, "wrap, linear");
    for (int i = 0; i < 16; i++) m[i] = (float)i;
    at = scratch(64);
    gp_copy_out(at, m, sizeof m);
    CHECK(com(s_dev, DV_SETTRANSFORM, 3, 1u, at) == OK && com(s_dev, DV_GETTRANSFORM, 3, 1u, at + 0) == OK && com(s_dev, DV_SETTRANSFORM, 3, 9u, at) == INVALIDPARAMS, "transforms are stored");
    G.open_ok = 0;
    CHECK((int32_t)draw() < 0 && (int32_t)clear_all(0) < 0, "no GPU frame: draws and clears fail instead of pretending");
    G.open_ok = 1;
}

/* ---- D. textures ---- */

static void frame(void) {
    com(s_dev, DV_END, 1);
    com(s_front, S_FLIP, 3, 0u, 1u);
    com(s_dev, DV_BEGIN, 1);
}

static void test_textures(void) {
    uint32_t sys = texture(TEXTURE_SYSTEM, 64, 32), vid = texture(TEXTURE_VIDEO, 64, 32), other = texture(TEXTURE_VIDEO, 32, 32), out = scratch(16), key = scratch(8);
    uint32_t sys_t = texture_face(sys), vid_t = texture_face(vid), other_t = texture_face(other), creates, updates;
    lx_d3d_stats_t st;
    CHECK(sys_t && vid_t && com(s_back, S_QI, 3, iid_tex2(), out) == E_NOINTERFACE, "IDirect3DTexture2 is a face of texture surfaces only");
    CHECK(com(vid_t, 0, 3, iid_surface4(), out) == OK && rd(out) == vid && com(vid, S_RELEASE, 1) == 2, "and shares their references");
    CHECK(com(s_dev, DV_SETTEXTURE, 3, 0u, vid) == INVALIDPARAMS && com(s_dev, DV_SETTEXTURE, 3, 8u, vid_t) == INVALIDPARAMS, "SetTexture wants a texture and a stage");
    CHECK(com(s_dev, DV_SETTEXTURE, 3, 0u, vid_t) == OK && com(s_dev, DV_GETTEXTURE, 3, 0u, out) == OK && rd(out) == vid_t && com(vid_t, TX_RELEASE, 1) == 3, "the device holds a reference");
    CHECK(com(vid_t, TX_LOAD, 2, other_t) == INVALIDPARAMS && com(vid_t, TX_LOAD, 2, 0u) == INVALIDPARAMS && com(vid_t, TX_LOAD, 2, vid_t) == OK, "Load wants a texture of the same shape");
    write_texel(sys, 0, 0, 0xF00F);
    CHECK(com(vid_t, TX_LOAD, 2, sys_t) == OK, "Load");
    creates = G.creates;
    draw();
    CHECK(G.creates == creates + 1 && G.desc_w == 64 && G.desc_levels == 7 && G.texel0[G.creates] == 0xFF0000FFu && G.state.has_texture && G.state.texture_id == G.creates,
          "the first draw uploads the texture with its mip levels (%u levels, 0x%08X)", G.desc_levels, G.texel0[G.creates]);
    updates = G.updates;
    draw();
    CHECK(com(vid_t, TX_LOAD, 2, sys_t) == OK, "the same pixels again");
    draw();
    CHECK(G.updates == updates, "an unchanged Load uploads nothing");
    for (int i = 0; i < 4; i++) {
        frame();
        write_texel(sys, 5, 3, (uint16_t)(0xF100 + i));
        com(vid_t, TX_LOAD, 2, sys_t);
        draw();
    }
    lx_d3d_stats(&st);
    CHECK(st.dynamic_textures == 1 && G.desc_levels <= 1, "a texture loaded every frame (water, terrain shadows) keeps level 0 only");
    frame();
    updates = G.updates;
    G.partials = 0;
    write_texel(sys, 5, 3, 0xF222);
    com(vid_t, TX_LOAD, 2, sys_t);
    draw();
    CHECK(G.partials == 1 && G.updates == updates + 1 && G.dirty.y == 3 && G.dirty.h == 1 && G.dirty.x <= 5 && G.dirty.x + (int32_t)G.dirty.w >= 6,
          "and only the rows that changed are uploaded (%d,%d %ux%u)", G.dirty.x, G.dirty.y, G.dirty.w, G.dirty.h);
    gp_write_u32(key, 0xF222), gp_write_u32(key + 4, 0xF222);
    updates = G.updates;
    CHECK(com(sys, S_SETCOLORKEY, 3, 8u, key) == OK && com(vid_t, TX_LOAD, 2, sys_t) == OK, "a colour key travels with Load");
    draw();
    CHECK(G.updates == updates + 1, "and uploads the texture again");
    write_texel(vid, 1, 1, 0xF333);
    updates = G.updates;
    draw();
    CHECK(G.updates == updates + 1, "a Lock of the drawn texture too");
    G.generation++;
    memset(G.alive, 0, sizeof G.alive);
    creates = G.creates;
    draw();
    CHECK(G.creates == creates + 1, "a renderer created again gets every texture again");
    CHECK(com(s_dev, DV_SETTEXTURE, 3, 0u, 0u) == OK, "SetTexture(NULL)");
    draw();
    CHECK(!G.state.has_texture, "draws untextured");
    G.destroys = 0;
    com(vid_t, TX_RELEASE, 1);
    CHECK(com(vid, S_RELEASE, 1) == 0 && G.destroys == 1, "the last reference destroys the GPU texture");
    com(sys_t, TX_RELEASE, 1), com(sys, S_RELEASE, 1), com(other_t, TX_RELEASE, 1), com(other, S_RELEASE, 1);
}

/* A custom level's picture in place of a texture. */
static void test_override(void) {
    static const char* const k_files[] = { "data/texhw/Map/w/crate.bmp", "data/texhw/Map/w/crate.norm.bmp" };
    const jpt_lit_vertex_t lit[4] = { { { 0, 0, 1, 1 }, { 1, 0, 0, 0 }, { 1, 0 } } };
    jpt_lit_draw_t lit_draw = { 0, 4, 0, 0xFFFFFF, lit };
    static uint8_t file[4096];
    const uint32_t vid = texture(TEXTURE_VIDEO, 64, 32), sys = texture(TEXTURE_SYSTEM, 64, 32), vid_t = texture_face(vid), sys_t = texture_face(sys);
    uint32_t creates;
    char dir[600];
    size_t size = fx_bmp(file, 8, 8, 24, 0x00AABBCCu);
    snprintf(dir, sizeof dir, "/tmp/jpt_texhw_linux/custom_levels/Tex");
    CHECK(fx_file(dir, "texhw/Map/w/crate.bmp", file, size), "a picture");
    size = fx_bmp(file, 4, 4, 24, 0x008080FFu);
    CHECK(fx_file(dir, "texhw/Map/w/crate.norm.bmp", file, size) && fx_activate("/tmp/jpt_texhw_linux", k_files, 2), "the package");
    jpt_texhw_name(1, "Map\\w\\crate.bmp");
    jpt_texhw_frame();
    CHECK(com(s_dev, DV_SETTEXTURE, 3, 0u, vid_t) == OK && com(vid_t, TX_LOAD, 2, sys_t) == OK, "a texture");
    creates = G.creates;
    lx_d3d_texture_override(vid_t, jpt_texhw_find(1));
    draw();
    CHECK(G.creates == creates + 1 && G.desc_w == 8 && G.texel0[G.creates] == 0xFFAABBCCu && G.state.texture_id == G.creates, "the picture is drawn in its place (0x%08X)", G.texel0[G.creates]);
    draw();
    CHECK(G.creates == creates + 1 && !G.state.lit, "and uploaded once");
    lit_draw.normal = jpt_texhw_normal(1);
    jpt_lit_set_draw(&lit_draw);
    draw();
    CHECK(G.creates == creates + 2 && G.desc_w == 4 && G.state.lit == &lit_draw && G.state.normal_id == G.creates && G.state.texture_id == creates + 1, "a normal map lights it");
    jpt_lit_set_draw(NULL);
    draw();
    CHECK(G.creates == creates + 2 && !G.state.lit, "only the polygon it is for");
    creates++;
    write_texel(sys, 0, 0, 0xF00F);
    com(vid_t, TX_LOAD, 2, sys_t);
    draw();
    CHECK(G.creates == creates + 2 && G.desc_w == 64, "another texture in the surface is its own again");
    lx_d3d_texture_override(vid_t, jpt_texhw_find(1));
    jpt_custom_level_deactivate();
    draw();
    CHECK(G.creates == creates + 2 && G.desc_w == 64, "and so it is after the session");
    com(s_dev, DV_SETTEXTURE, 3, 0u, 0u);
    com(vid_t, TX_RELEASE, 1), com(vid, S_RELEASE, 1), com(sys_t, TX_RELEASE, 1), com(sys, S_RELEASE, 1);
}

static void test_many_textures(void) {
    static uint32_t many[1200];
    unsigned before = lx_ddraw_surface_count(), made = 0;
    for (int i = 0; i < 1200; i++) many[i] = texture(TEXTURE_VIDEO, 8, 8), made += many[i] != 0;
    CHECK(made == 1200 && lx_ddraw_surface_count() == before + 1200, "a level's worth of textures (%u)", made);
    for (int i = 0; i < 1200; i++) com(many[i], S_RELEASE, 1);
    CHECK(lx_ddraw_surface_count() == before, "all released");
}

/* ---- E. water and sky ---- */

static void water_texture(uint32_t surface, float a) {
    uint32_t pitch, va = lock(surface, &pitch);
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++) fx_water_store((uintptr_t)gm_ptr_checked(va + y * pitch + x * 2, 2, 1), a, -a);
    *pixels(va, 2) = (uint16_t)(0x8000u | (unsigned)(a * 100.0f));
    com(surface, S_UNLOCK, 2, 0u);
}

static void test_water(void) {
    uint32_t sys_a = texture(TEXTURE_SYSTEM, 16, 16), vid_a = texture(TEXTURE_VIDEO, 16, 16), sys_b = texture(TEXTURE_SYSTEM, 16, 16), vid_b = texture(TEXTURE_VIDEO, 16, 16);
    uint32_t sys_c = texture(TEXTURE_SYSTEM, 16, 16), vid_c = texture(TEXTURE_VIDEO, 16, 16);
    uint32_t sa = texture_face(sys_a), va = texture_face(vid_a), sb = texture_face(sys_b), vb = texture_face(vid_b), sc = texture_face(sys_c), vc = texture_face(vid_c);
    uint32_t slot_a;
    lx_d3d_configure_effects(1, 1, 2);
    blend(1);
    water_texture(sys_a, 0.25f), water_texture(sys_b, 0.5f);
    write_texel(sys_c, 0, 0, 0x8123);
    com(va, TX_LOAD, 2, sa), com(vb, TX_LOAD, 2, sb), com(vc, TX_LOAD, 2, sc);
    G.waves = 0;
    com(s_dev, DV_SETTEXTURE, 3, 0u, va);
    draw();
    slot_a = G.wave_slot;
    CHECK(G.state.water_mode == 1 && G.waves == 1 && G.wave_w == 16 && G.wave0 == 0.25f, "a blended draw of a texture the water rows wrote is refracted, with its waves (%g)", (double)G.wave0);
    CHECK(G.bounds[0] > 0.0f && G.bounds[2] < 1.0f, "sampling stays inside the game's view");
    draw();
    CHECK(G.state.water_mode == 1 && G.waves == 1 && !G.has_waves, "waves that did not change are not uploaded again");
    com(s_dev, DV_SETTEXTURE, 3, 0u, vb);
    draw();
    CHECK(G.state.water_mode == 1 && G.wave_slot != slot_a && G.wave0 == 0.5f, "another water keeps its own wave field");
    com(s_dev, DV_SETTEXTURE, 3, 0u, va);
    G.waves = 0;
    draw();
    CHECK(G.wave_slot == slot_a && G.waves == 0, "and the first one's is still there");
    water_texture(sys_a, 0.75f);
    com(va, TX_LOAD, 2, sa);
    draw();
    CHECK(G.waves == 1 && G.wave0 == 0.75f, "new rows reach their own field");
    com(s_dev, DV_SETTEXTURE, 3, 0u, vc);
    draw();
    CHECK(G.state.water_mode == 0 && G.state.alpha_blend, "another transparent texture is not water");
    com(s_dev, DV_SETTEXTURE, 3, 0u, va);
    blend(0);
    draw();
    CHECK(G.state.water_mode == 0, "nor an opaque draw");
    blend(1);
    lx_d3d_configure_effects(0, 1, 2);
    draw();
    CHECK(G.state.water_mode == 0 && G.state.has_texture, "Water refraction Off draws the retail water");
    lx_d3d_configure_effects(1, 1, 2);
    draw();
    CHECK(G.state.water_mode == 1, "and On refracts it again");
    com(va, TX_LOAD, 2, sc);
    draw();
    CHECK(G.state.water_mode == 0, "a texture loaded from elsewhere stops being water");
    com(va, TX_LOAD, 2, sa);
    draw();
    CHECK(G.state.water_mode == 1, "until the rows' source is loaded again");
    com(sa, TX_RELEASE, 1), com(sys_a, S_RELEASE, 1);
    draw();
    CHECK(G.state.water_mode == 0, "a freed source is no wave field");
    com(s_dev, DV_SETTEXTURE, 3, 0u, 0u);
    blend(0);
    com(va, TX_RELEASE, 1), com(vid_a, S_RELEASE, 1), com(sb, TX_RELEASE, 1), com(sys_b, S_RELEASE, 1), com(vb, TX_RELEASE, 1), com(vid_b, S_RELEASE, 1);
    com(sc, TX_RELEASE, 1), com(sys_c, S_RELEASE, 1), com(vc, TX_RELEASE, 1), com(vid_c, S_RELEASE, 1);
}

static void test_sky(void) {
    const float sky[5] = { 1.5f, 0.25f, 0.5f, 0.1f, 0.9f };
    float bad[5];
    memcpy(bad, sky, sizeof bad);
    bad[3] = 50.0f;
    draw();
    CHECK(G.state.sky_mode == 0, "no sky outside the sky's draws");
    lx_d3d_sky_begin(sky);
    draw();
    CHECK(G.state.sky_mode == 1 && G.state.water_mode == 0 && !memcmp(G.sky_params, sky, sizeof sky), "the sky's draws take the enhanced sky and its parameters");
    lx_d3d_sky_end();
    draw();
    CHECK(G.state.sky_mode == 0, "and nothing after them");
    lx_d3d_sky_begin(bad);
    draw();
    CHECK(G.state.sky_mode == 0, "bounds it cannot use draw the retail sky");
    lx_d3d_sky_end();
    lx_d3d_configure_effects(1, 0, 2);
    lx_d3d_sky_begin(sky);
    draw();
    CHECK(G.state.sky_mode == 0, "Enhanced sky Off draws the retail sky");
    lx_d3d_sky_end();
    lx_d3d_configure_effects(1, 1, 2);
}

/* ---- F. the picture the CPU and the GPU share ---- */

static int s_shown = 1;
static int scene_shown(void) { return s_shown; }

static void test_hybrid(void) {
    uint32_t pitch = 0, va, fx = scratch(100), r = scratch(16), box4[4] = { 0, 0, W, H };
    const lx_virtual_state_t* v = lx_virtual_state();
    const long poly[4] = { 100, 50, 120, 70 };
    lx_d3d_stats_t before, after;
    unsigned presents, scenes;

    CHECK(clear_all(0xFF0000FFu) == OK && G.clear_argb == 0xFF0000FFu && G.scissor[0] == 0, "Clear2");
    G.reads = 0;
    va = lock(s_back, &pitch);
    CHECK(va && *pixels(va, 2) == 0x001F && G.reads == 0, "a Lock after a clear needs no readback (0x%04X)", *pixels(va, 2));
    com(s_back, S_UNLOCK, 2, 0u);

    draw();
    G.scene = 0x4321;
    va = lock(s_back, &pitch);
    CHECK(G.reads == 1 && *pixels(va + 10 * pitch + 20, 2) == 0x4321, "a Lock after a draw reads the scene back");
    *pixels(va + 60 * pitch + 2 * 200, 2) = 0xFFFF;
    com(s_back, S_UNLOCK, 2, 0u);
    G.overlays = G.opaque_overlays = 0;
    draw();
    CHECK(G.overlays == 1 && G.overlay.x <= 200 && G.overlay.x + (int32_t)G.overlay.w > 200 && G.overlay.y <= 60 && G.overlay.w <= 3 && G.overlay.h <= 3 && G.opaque_overlays == 0,
          "the next draw composites only what the CPU wrote (%d,%d %ux%u)", G.overlay.x, G.overlay.y, G.overlay.w, G.overlay.h);

    G.reads = G.overlays = 0;
    lx_d3d_water_row();
    va = lock(s_back, &pitch);
    com(s_back, S_UNLOCK, 2, 0u);
    draw();
    CHECK(va && G.reads == 0 && G.overlays == 0, "the Lock after the water's rows touches nothing");
    va = lock(s_back, &pitch);
    com(s_back, S_UNLOCK, 2, 0u);
    CHECK(G.reads == 1, "the one after it does");
    draw();

    G.reads = G.overlays = 0;
    lx_d3d_stats(&before);
    lx_d3d_cpu_polygon(s_back, poly, 0, 0);
    va = lock(s_back, &pitch);
    for (uint32_t y = 55; y < 60; y++) *pixels(va + y * pitch + 2 * 105, 2) = 0x07E0;
    com(s_back, S_UNLOCK, 2, 0u);
    lx_d3d_cpu_polygon_done();
    draw();
    lx_d3d_stats(&after);
    CHECK(G.reads == 0 && G.overlays == 1 && after.cpu_polygons == before.cpu_polygons + 1 && G.overlay.x >= 100 && G.overlay.x + (int32_t)G.overlay.w <= 121,
          "a polygon the game rasterises reaches the GPU without a readback (%u reads, %d..%d)", G.reads, G.overlay.x, G.overlay.x + (int)G.overlay.w);
    lx_d3d_cpu_polygon(s_back, poly, 0, 1);
    va = lock(s_back, &pitch);
    CHECK(G.reads == 1 && *pixels(va + 60 * pitch + 2 * 110, 2) == 0x4321, "one that reads what is under it gets its box");
    com(s_back, S_UNLOCK, 2, 0u);
    lx_d3d_cpu_polygon_done();
    draw();

    gp_copy_out(r, box4, sizeof box4);
    gp_write_u32(fx, 100), gp_write_u32(fx + 80, 0xF800);
    G.clears = 0;
    CHECK(com(s_back, S_BLT, 6, r, 0u, 0u, 0x1000400u, fx) == OK && G.clears == 1 && G.clear_argb == 0xFFFF0000u, "a colour fill of the target is a clear (0x%08X)", G.clear_argb);
    draw();

    lx_d3d_set_view(scene_shown, NULL);
    com(s_dev, DV_END, 1);
    presents = v->presents, scenes = v->scene_presents;
    G.reads = 0;
    CHECK(com(s_front, S_FLIP, 3, 0u, 1u) == OK && v->scene_presents == scenes + 1 && v->presents == presents && G.reads == 0 && G.ao_level == 2 && G.ao_gameplay == 1,
          "Flip shows the GPU's scene, with ambient occlusion, and reads nothing back");
    va = lock(s_front, &pitch);
    CHECK(va && G.last_reads == 1 && *pixels(va, 2) == 0x1111, "the front buffer of such a frame is the last scene");
    com(s_front, S_UNLOCK, 2, 0u);
    va = lock(s_front, &pitch);
    com(s_front, S_UNLOCK, 2, 0u);
    CHECK(G.last_reads == 1, "read once");

    com(s_dev, DV_BEGIN, 1);
    draw();
    com(s_dev, DV_END, 1);
    s_shown = 0;
    presents = v->presents, scenes = v->scene_presents;
    G.reads = 0;
    G.scene = 0x2222;
    CHECK(com(s_front, S_FLIP, 3, 0u, 1u) == OK && v->presents > presents && v->scene_presents == scenes && G.reads == 1, "under a menu the CPU's picture is complete and shown");
    va = lock(s_front, &pitch);
    CHECK(*pixels(va + 5 * pitch, 2) == 0x2222, "with the scene in it (0x%04X)", *pixels(va + 5 * pitch, 2));
    com(s_front, S_UNLOCK, 2, 0u);
    s_shown = 1;
    va = lock(s_back, &pitch);
    *pixels(va, 2) = 0xABCD;
    com(s_back, S_UNLOCK, 2, 0u);
    presents = v->presents;
    com(s_front, S_FLIP, 3, 0u, 1u);
    CHECK(v->presents == presents + 1, "a frame with no draws is the CPU's");
    com(s_dev, DV_BEGIN, 1);
    G.opaque_overlays = 0;
    draw();
    CHECK(G.opaque_overlays == 1, "the first draw after it starts from that picture");
}

static void test_cleanup(void) {
    lx_d3d_stats_t st;
    com(s_dev, DV_END, 1);
    CHECK(com(s_vp, VP_RELEASE, 1) == 1 && com(s_dev, DV_RELEASE, 1) == 0, "the device releases its viewport and target");
    {
        const uint32_t begin_args[1] = { s_dev }, clear_args[7] = { s_vp, 0, 0, 1, 0, 0, 0 };
        CHECK(call_n(rd(lx_d3d_vtable(LX_D3D_DEVICE3) + 4 * DV_BEGIN), 1, begin_args) == INVALIDPARAMS &&
                  call_n(rd(lx_d3d_vtable(LX_D3D_VIEWPORT3) + 4 * VP_CLEAR2), 7, clear_args) == INVALIDPARAMS,
              "released objects answer nothing");
    }
    lx_d3d_stats(&st);
    CHECK(st.devices == 0 && st.viewports == 0, "no object is left");
    CHECK(com(s_back, S_DELATTACHED, 3, 0u, s_z) == OK && com(s_z, S_RELEASE, 1) == 0, "the z-buffer detaches");
    com(s_back, S_RELEASE, 1);
    CHECK(com(s_front, S_RELEASE, 1) == 0 && lx_ddraw_surface_count() == 0, "no surface is left (%u)", lx_ddraw_surface_count());
    com(s_d3d, D3_RELEASE, 1);
    com(s_dd4, DD_RELEASE, 1);
    CHECK(com(s_dd, DD_RELEASE, 1) == 0 && lx_ddraw_device_count() == 0, "no DirectDraw object is left");
    CHECK(gp_leak_count() == 0, "no host value was stored in guest memory");
}

int main(void) {
    if (!harness_start()) return 2;
    s_at = g_scratch + 0x1000;
    s_out = scratch(16);
    make_window();
    test_vtables();
    test_essential_slots();
    test_software();
    test_device();
    test_scene_and_draws();
    s_at = g_scratch + 0x20000;
    test_textures();
    test_override();
    s_at = g_scratch + 0x40000;
    test_many_textures();
    s_at = g_scratch + 0x20000;
    test_water();
    test_sky();
    test_hybrid();
    test_cleanup();
    lx_d3d_set_gpu(NULL);
    harness_stop();
    printf("linux_d3d_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
