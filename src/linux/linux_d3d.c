#include "linux_d3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "linux_d3d_internal.h"
#include "linux_ddraw.h"
#include "linux_log.h"
#include "linux_spine.h"

#define KIND_DEVICE 0x44334433u
#define KIND_VIEWPORT 0x33505633u
#define DEVICE_DESC_BYTES 252u
#define PIXEL_FORMAT_BYTES 32u
#define VIEWPORT_BYTES 44u
#define MATRIX_BYTES 64u
#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_RGB 0x40u
#define DDPF_ZBUFFER 0x400u
#define DDBD_16 0x400u
#define DDBD_24 0x200u
#define ENUM_CANCEL 0u
#define NEXT_NEXT 1u
#define NEXT_HEAD 2u
#define NEXT_TAIL 4u
#define FAILURE_LOG_LIMIT 64

static const lx_gpu_ops_t* s_gpu;
static uint32_t s_vtable[LX_D3D_INTERFACES];
static unsigned s_devices, s_viewports;
static int (*s_fog_sync)(uint32_t* color);
static int s_log_lines;
static unsigned char s_logged_render[LX_D3D_RENDER_STATES], s_logged_stage[LX_D3D_STAGES][LX_D3D_STAGE_STATES];

void lx_d3d_set_gpu(const lx_gpu_ops_t* ops) { s_gpu = ops; }
const lx_gpu_ops_t* lx_d3d_gpu(void) { return s_gpu; }
int lx_d3d_hardware(void) { return s_gpu && s_gpu->available(); }
void lx_d3d_set_fog_sync(int (*sync)(uint32_t* color)) { s_fog_sync = sync; }

void lx_d3d_reset(void) {
    memset(s_vtable, 0, sizeof s_vtable);
    memset(s_logged_render, 0, sizeof s_logged_render);
    memset(s_logged_stage, 0, sizeof s_logged_stage);
    s_devices = s_viewports = 0;
    s_log_lines = 0;
    lx_target_reset();
}

void lx_d3d_stats(lx_d3d_stats_t* out) {
    *out = *lx_target_stats();
    out->devices = s_devices;
    out->viewports = s_viewports;
}

void lx_d3d_log_summary(void) {
    lx_d3d_stats_t s;
    lx_d3d_stats(&s);
    if (!s.draws && !s.clears) return;
    lx_log("d3d",
           "%llu GPU frames, %llu CPU frames, %llu draws (%llu skipped), %llu clears; textures: %llu uploads (%llu partial), %llu "
           "dynamic; water %llu draws, %llu wave uploads; sky %llu draws; software polygons %llu; %llu readbacks, %llu composites, "
           "%llu uploads of the CPU picture; %llu refused states, %llu unsupported calls",
           s.gpu_frames, s.cpu_frames, s.draws, s.skipped_draws, s.clears, s.texture_uploads, s.partial_uploads, s.dynamic_textures,
           s.water_draws, s.wave_uploads, s.sky_draws, s.cpu_polygons, s.readbacks, s.composites, s.uploads, s.refused_states,
           s.unsupported_calls);
}

static int log_line(void) {
    if (s_log_lines >= FAILURE_LOG_LIMIT) return 0;
    s_log_lines++;
    return 1;
}

static uint64_t done(const char* method, uint32_t hr) {
    if ((int32_t)hr < 0 && log_line()) lx_log("d3d", "%s failed: 0x%08X", method, hr);
    return hr;
}

static uint64_t unsupported(const uint32_t* a, void* user) {
    const char* name = (const char*)user;
    (void)a;
    lx_target_stats()->unsupported_calls++;
    if (log_line()) lx_log("d3d", "%s is not implemented (the game was not seen calling it)", name);
    return LX_DDERR_UNSUPPORTED;
}

/* ---- guest values ---- */

static int out_u32(uint32_t va, uint32_t value) { return va && gp_copy_out(va, &value, sizeof value); }
static int struct_size_is(uint32_t va, uint32_t size) {
    uint32_t given = 0;
    return va && gp_copy_in(&given, va, sizeof given) && given == size;
}

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} guid_t;

static int guid_is(uint32_t va, const guid_t* want) {
    guid_t g;
    return va && gp_copy_in(&g, va, sizeof g) && !memcmp(&g, want, sizeof g);
}

static const guid_t k_iid_unknown = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const guid_t k_iid_direct3d3 = { 0xBB223240, 0xE72B, 0x11D0, { 0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E } };
static const guid_t k_iid_device3 = { 0xB0AB3B60, 0x33D7, 0x11D1, { 0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74 } };
static const guid_t k_iid_viewport3 = { 0xB0AB3B61, 0x33D7, 0x11D1, { 0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74 } };
static const guid_t k_hal_device = { 0x84E63DE0, 0x46AA, 0x11CF, { 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E } };

static void* object_of(uint32_t iface) { return iface && gm_ptr_checked(iface, 8, 0) ? ht_object_host(iface) : NULL; }

static lx_device_t* device_of(uint32_t iface) {
    lx_device_t* d = (lx_device_t*)object_of(iface);
    return d && d->kind == KIND_DEVICE ? d : NULL;
}

static lx_viewport_t* viewport_of(uint32_t iface) {
    lx_viewport_t* v = (lx_viewport_t*)object_of(iface);
    return v && v->kind == KIND_VIEWPORT ? v : NULL;
}

#define DEVICE(method)                                                                                                             \
    lx_device_t* dev = device_of(a[0]);                                                                                            \
    static const char k_method[] = "IDirect3DDevice3::" method;                                                                   \
    (void)u;                                                                                                                       \
    if (!dev) return done(k_method, LX_DDERR_INVALIDPARAMS)
#define VIEWPORT(method)                                                                                                           \
    lx_viewport_t* v = viewport_of(a[0]);                                                                                          \
    static const char k_method[] = "IDirect3DViewport3::" method;                                                                 \
    (void)u;                                                                                                                       \
    if (!v) return done(k_method, LX_DDERR_INVALIDPARAMS)
#define RET(hr) return done(k_method, hr)

/* ---- IDirect3D3 ---- */

static void put32(uint8_t* desc, unsigned offset, uint32_t value) { memcpy(desc + offset, &value, sizeof value); }
static void putf(uint8_t* desc, unsigned offset, float value) { memcpy(desc + offset, &value, sizeof value); }

/* D3DPRIMCAPS at `at`. */
static void fill_primcaps(uint8_t* desc, unsigned at) {
    put32(desc, at, 56);
    put32(desc, at + 4, 0x72);       /* CULLNONE, CULLCW, CULLCCW, MASKZ */
    put32(desc, at + 8, 0x200B1);    /* DITHER, SUBPIXEL, ZTEST, FOGVERTEX, ANISOTROPY */
    put32(desc, at + 12, 0xFF);      /* every depth comparison */
    put32(desc, at + 16, 0x1FFF);    /* every blend factor */
    put32(desc, at + 20, 0x1FFF);
    put32(desc, at + 24, 0xFF);      /* every alpha comparison */
    put32(desc, at + 28, 0xC528A);   /* flat and Gouraud colour, specular, alpha blend, fog */
    put32(desc, at + 32, 0xF);       /* PERSPECTIVE, POW2, ALPHA, TRANSPARENCY */
    put32(desc, at + 36, 0x3000303); /* NEAREST, LINEAR, MIN and MAG point and linear */
    put32(desc, at + 40, 0xCF);      /* DECAL, MODULATE, DECALALPHA, MODULATEALPHA, COPY, ADD */
    put32(desc, at + 44, 0x17);      /* WRAP, MIRROR, CLAMP, INDEPENDENTUV */
}

/* The capabilities of the Windows host (d3d_fill_device_desc): what the game's acceptance test requires. */
static void fill_device_desc(uint8_t desc[DEVICE_DESC_BYTES]) {
    memset(desc, 0, DEVICE_DESC_BYTES);
    put32(desc, 0, DEVICE_DESC_BYTES);
    put32(desc, 4, 0x7FF);
    put32(desc, 8, 2); /* D3DCOLOR_RGB */
    put32(desc, 12, 0x83E51);
    put32(desc, 16, 8);
    put32(desc, 28, 16);
    fill_primcaps(desc, 44);
    fill_primcaps(desc, 100);
    put32(desc, 156, DDBD_16);
    put32(desc, 160, DDBD_16 | DDBD_24);
    put32(desc, 168, 65535);
    put32(desc, 172, 1);
    put32(desc, 176, 1);
    put32(desc, 180, 2048);
    put32(desc, 184, 2048);
    put32(desc, 204, 2048);
    put32(desc, 208, 2048);
    put32(desc, 212, 16);
    putf(desc, 216, -2048.0f);
    putf(desc, 220, -2048.0f);
    putf(desc, 224, 2047.0f);
    putf(desc, 228, 2047.0f);
    put32(desc, 240, 1);
    put32(desc, 244, 0x104F);
    put32(desc, 248, 1u | 1u << 16);
}

static void fill_no_device_desc(uint8_t desc[DEVICE_DESC_BYTES]) {
    memset(desc, 0, DEVICE_DESC_BYTES);
    put32(desc, 0, DEVICE_DESC_BYTES);
}

static uint64_t d3_QueryInterface(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3D3::QueryInterface";
    int dd = lx_ddraw_device_from(a[0]);
    (void)u;
    if (dd < 0 || !out_u32(a[2], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (guid_is(a[1], &k_iid_unknown) || guid_is(a[1], &k_iid_direct3d3)) {
        lx_ddraw_device_addref(dd);
        out_u32(a[2], a[0]);
        RET(LX_D3D_OK);
    }
    RET(lx_ddraw_device_query(dd, a[1], a[2]));
}

static uint64_t d3_AddRef(const uint32_t* a, void* u) {
    int dd = lx_ddraw_device_from(a[0]);
    (void)u;
    return dd < 0 ? 0 : lx_ddraw_device_addref(dd);
}

static uint64_t d3_Release(const uint32_t* a, void* u) {
    int dd = lx_ddraw_device_from(a[0]);
    (void)u;
    return dd < 0 ? 0 : lx_ddraw_device_release(dd);
}

static uint64_t d3_EnumDevices(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3D3::EnumDevices";
    static const char name[] = "Direct3D HAL";
    uint8_t hal[DEVICE_DESC_BYTES], hel[DEVICE_DESC_BYTES];
    char text[96];
    uint32_t block, at_guid, at_text, at_name, at_hal, at_hel, args[6];
    (void)u;
    if (lx_ddraw_device_from(a[0]) < 0 || !a[1]) RET(LX_DDERR_INVALIDPARAMS);
    fill_device_desc(hal);
    fill_no_device_desc(hel);
    snprintf(text, sizeof text, "%s hardware (Trespasser compatibility layer)", s_gpu ? s_gpu->name : "Vulkan");
    block = gm_heap_alloc((uint32_t)(sizeof k_hal_device + sizeof text + sizeof name + 2u * DEVICE_DESC_BYTES + 16u), 1);
    if (!block) RET(LX_DDERR_OUTOFMEMORY);
    at_guid = block;
    at_hal = at_guid + (uint32_t)sizeof k_hal_device;
    at_hel = at_hal + DEVICE_DESC_BYTES;
    at_text = at_hel + DEVICE_DESC_BYTES;
    at_name = at_text + (uint32_t)sizeof text;
    gp_copy_out(at_guid, &k_hal_device, sizeof k_hal_device);
    gp_copy_out(at_hal, hal, sizeof hal);
    gp_copy_out(at_hel, hel, sizeof hel);
    gp_copy_out(at_text, text, sizeof text);
    gp_copy_out(at_name, name, sizeof name);
    args[0] = at_guid, args[1] = at_text, args[2] = at_name, args[3] = at_hal, args[4] = at_hel, args[5] = a[2];
    lx_call_guest(a[1], LX_STDCALL, 6, args, 0, NULL);
    gm_heap_free(block);
    RET(LX_D3D_OK);
}

static uint64_t d3_CreateViewport(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3D3::CreateViewport";
    lx_viewport_t* v;
    (void)u;
    if (lx_ddraw_device_from(a[0]) < 0 || !out_u32(a[1], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (a[2]) RET(LX_CLASS_E_NOAGGREGATION);
    v = (lx_viewport_t*)calloc(1, sizeof *v);
    if (!v) RET(LX_DDERR_OUTOFMEMORY);
    v->kind = KIND_VIEWPORT;
    v->refs = 1;
    v->guest = ht_object_new(lx_d3d_vtable(LX_D3D_VIEWPORT3), 16, v);
    if (!v->guest) {
        free(v);
        RET(LX_DDERR_OUTOFMEMORY);
    }
    s_viewports++;
    out_u32(a[1], v->guest);
    RET(LX_D3D_OK);
}

static void default_states(lx_device_t* dev, int zbuffer) {
    uint32_t* r = dev->render;
    r[LX_RS_TEXTUREPERSPECTIVE] = 1;
    r[LX_RS_ZENABLE] = zbuffer ? 1 : 0;
    r[LX_RS_ZWRITEENABLE] = 1;
    r[LX_RS_ZFUNC] = 4;
    r[LX_RS_FILLMODE] = 3;
    r[LX_RS_SHADEMODE] = 2;
    r[LX_RS_SRCBLEND] = 2;
    r[LX_RS_DESTBLEND] = 1;
    r[LX_RS_TEXTUREMAPBLEND] = 2;
    r[LX_RS_CULLMODE] = 3;
    r[LX_RS_ALPHAFUNC] = 8;
    r[LX_RS_TEXTUREMAG] = r[LX_RS_TEXTUREMIN] = 1;
    r[LX_RS_TEXTUREADDRESS] = r[LX_RS_TEXTUREADDRESSU] = r[LX_RS_TEXTUREADDRESSV] = 1;
    for (uint32_t k = 0; k < LX_D3D_STAGES; k++) {
        uint32_t* st = dev->stage[k];
        st[LX_TSS_COLOROP] = k ? 1 : 4;
        st[LX_TSS_COLORARG1] = st[LX_TSS_ALPHAARG1] = 2;
        st[LX_TSS_COLORARG2] = st[LX_TSS_ALPHAARG2] = 1;
        st[LX_TSS_ALPHAOP] = k ? 1 : 2;
        st[LX_TSS_TEXCOORDINDEX] = k;
        st[LX_TSS_ADDRESS] = st[LX_TSS_ADDRESSU] = st[LX_TSS_ADDRESSV] = 1;
        st[LX_TSS_MAGFILTER] = st[LX_TSS_MINFILTER] = st[LX_TSS_MIPFILTER] = 1;
        st[LX_TSS_MAXANISOTROPY] = 1;
        dev->texture[k] = -1;
    }
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) dev->transforms[i][k * 5] = 1.0f;
}

static uint64_t d3_CreateDevice(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3D3::CreateDevice";
    int dd = lx_ddraw_device_from(a[0]), target = lx_ddraw_surface_from(a[2]);
    lx_surface_view_t view;
    lx_device_t* dev;
    (void)u;
    if (dd < 0 || !out_u32(a[3], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (a[4]) RET(LX_CLASS_E_NOAGGREGATION);
    if (target < 0 || !lx_ddraw_surface_view(target, &view)) RET(LX_DDERR_INVALIDPARAMS);
    if (!lx_d3d_hardware()) RET(LX_DDERR_GENERIC);
    if (!guid_is(a[1], &k_hal_device) && log_line())
        lx_log("d3d", "CreateDevice for another device class: answered with the %s hardware device", s_gpu->name);
    dev = (lx_device_t*)calloc(1, sizeof *dev);
    if (!dev) RET(LX_DDERR_OUTOFMEMORY);
    dev->kind = KIND_DEVICE;
    dev->refs = 1;
    dev->dd = dd;
    dev->target = target;
    default_states(dev, view.zbuffer);
    dev->guest = ht_object_new(lx_d3d_vtable(LX_D3D_DEVICE3), 16, dev);
    if (!dev->guest || !lx_target_attach(dev)) {
        if (dev->guest) ht_object_free(dev->guest);
        free(dev);
        RET(LX_DDERR_OUTOFMEMORY);
    }
    lx_ddraw_surface_addref(target);
    lx_ddraw_device_addref(dd);
    s_devices++;
    lx_log("d3d", "CreateDevice: IDirect3DDevice3 at guest 0x%08X draws into a %ux%u surface%s through %s", dev->guest, view.width,
           view.height, view.zbuffer ? " with its z-buffer" : "", s_gpu->name);
    out_u32(a[3], dev->guest);
    RET(LX_D3D_OK);
}

static uint64_t d3_EnumZBufferFormats(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3D3::EnumZBufferFormats";
    static const uint32_t depths[2] = { 16, 24 };
    uint32_t block;
    (void)u;
    if (lx_ddraw_device_from(a[0]) < 0 || !a[2]) RET(LX_DDERR_INVALIDPARAMS);
    block = gm_heap_alloc(PIXEL_FORMAT_BYTES, 1);
    if (!block) RET(LX_DDERR_OUTOFMEMORY);
    for (int i = 0; i < 2; i++) {
        uint32_t pf[8] = { PIXEL_FORMAT_BYTES, DDPF_ZBUFFER, 0, depths[i], 0, depths[i] == 16 ? 0xFFFFu : 0xFFFFFFu, 0, 0 };
        uint32_t args[2] = { block, a[3] };
        gp_copy_out(block, pf, sizeof pf);
        if (lx_call_guest(a[2], LX_STDCALL, 2, args, 0, NULL) == ENUM_CANCEL) break;
    }
    gm_heap_free(block);
    RET(LX_D3D_OK);
}

/* Textures live in host memory: there is nothing managed to evict. */
static uint64_t d3_EvictManagedTextures(const uint32_t* a, void* u) {
    (void)u;
    return done("IDirect3D3::EvictManagedTextures", lx_ddraw_device_from(a[0]) < 0 ? LX_DDERR_INVALIDPARAMS : LX_D3D_OK);
}

/* ---- IDirect3DViewport3 ---- */

static uint64_t vp_QueryInterface(const uint32_t* a, void* u) {
    VIEWPORT("QueryInterface");
    if (!out_u32(a[2], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (!guid_is(a[1], &k_iid_unknown) && !guid_is(a[1], &k_iid_viewport3)) RET(LX_E_NOINTERFACE);
    v->refs++;
    out_u32(a[2], a[0]);
    RET(LX_D3D_OK);
}

static uint64_t vp_AddRef(const uint32_t* a, void* u) {
    lx_viewport_t* v = viewport_of(a[0]);
    (void)u;
    return v ? (uint32_t)++v->refs : 0;
}

static uint32_t viewport_release(lx_viewport_t* v) {
    if (--v->refs > 0) return (uint32_t)v->refs;
    ht_object_free(v->guest);
    free(v);
    if (s_viewports) s_viewports--;
    return 0;
}

static uint64_t vp_Release(const uint32_t* a, void* u) {
    lx_viewport_t* v = viewport_of(a[0]);
    (void)u;
    return v ? viewport_release(v) : 0;
}

static uint64_t vp_Initialize(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return done("IDirect3DViewport3::Initialize", LX_DDERR_ALREADYINITIALIZED);
}

static float as_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t as_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

static uint64_t vp_GetViewport(const uint32_t* a, void* u) {
    uint32_t d[VIEWPORT_BYTES / 4u];
    VIEWPORT("GetViewport");
    if (!struct_size_is(a[1], VIEWPORT_BYTES)) RET(LX_DDERR_INVALIDPARAMS);
    if (!v->data_set) RET(LX_D3DERR_VIEWPORTDATANOTSET);
    memcpy(d, v->data, 5 * sizeof d[0]);
    d[0] = VIEWPORT_BYTES;
    d[5] = as_bits((float)v->data[3] * 0.5f);
    d[6] = as_bits((float)v->data[4] * 0.5f);
    d[7] = as_bits(as_float(v->data[7]) * 0.5f);
    d[8] = as_bits(as_float(v->data[8]) * 0.5f);
    d[9] = v->data[9];
    d[10] = v->data[10];
    RET(gp_copy_out(a[1], d, sizeof d) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

static uint64_t vp_SetViewport(const uint32_t* a, void* u) {
    uint32_t d[VIEWPORT_BYTES / 4u];
    VIEWPORT("SetViewport");
    if (!struct_size_is(a[1], VIEWPORT_BYTES) || !gp_copy_in(d, a[1], sizeof d)) RET(LX_DDERR_INVALIDPARAMS);
    memset(v->data, 0, sizeof v->data);
    v->data[0] = LX_VIEWPORT2_BYTES;
    memcpy(v->data + 1, d + 1, 4 * sizeof d[0]);
    v->data[7] = as_bits(as_float(d[7]) * 2.0f);
    v->data[8] = as_bits(as_float(d[8]) * 2.0f);
    v->data[9] = d[9];
    v->data[10] = d[10];
    v->data_set = 1;
    RET(LX_D3D_OK);
}

static uint64_t vp_GetViewport2(const uint32_t* a, void* u) {
    VIEWPORT("GetViewport2");
    if (!struct_size_is(a[1], LX_VIEWPORT2_BYTES)) RET(LX_DDERR_INVALIDPARAMS);
    if (!v->data_set) RET(LX_D3DERR_VIEWPORTDATANOTSET);
    RET(gp_copy_out(a[1], v->data, sizeof v->data) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

static uint64_t vp_SetViewport2(const uint32_t* a, void* u) {
    VIEWPORT("SetViewport2");
    if (!struct_size_is(a[1], LX_VIEWPORT2_BYTES) || !gp_copy_in(v->data, a[1], sizeof v->data)) RET(LX_DDERR_INVALIDPARAMS);
    v->data_set = 1;
    RET(LX_D3D_OK);
}

static uint64_t vp_Clear2(const uint32_t* a, void* u) {
    const int32_t* rects = NULL;
    const uint32_t count = a[1], flags = a[3];
    VIEWPORT("Clear2");
    if (!v->device) RET(LX_D3DERR_VIEWPORTHASNODEVICE);
    if (!(flags & (LX_D3DCLEAR_TARGET | LX_D3DCLEAR_ZBUFFER | LX_D3DCLEAR_STENCIL)) || count > 0x10000u) RET(LX_DDERR_INVALIDPARAMS);
    if (count) {
        rects = (const int32_t*)gm_ptr_checked(a[2], count * 16u, 0);
        if (!a[2] || !rects) RET(LX_DDERR_INVALIDPARAMS);
    }
    RET(lx_target_clear(v->device->target, count, rects, flags, a[4], as_float(a[5])));
}

/* ---- IDirect3DDevice3 ---- */

static uint64_t dv_QueryInterface(const uint32_t* a, void* u) {
    DEVICE("QueryInterface");
    if (!out_u32(a[2], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (!guid_is(a[1], &k_iid_unknown) && !guid_is(a[1], &k_iid_device3)) RET(LX_E_NOINTERFACE);
    dev->refs++;
    out_u32(a[2], a[0]);
    RET(LX_D3D_OK);
}

static uint64_t dv_AddRef(const uint32_t* a, void* u) {
    lx_device_t* dev = device_of(a[0]);
    (void)u;
    return dev ? (uint32_t)++dev->refs : 0;
}

static void drop_viewport(lx_device_t* dev, lx_viewport_t* v) {
    lx_viewport_t** link = &dev->viewports;
    while (*link && *link != v) link = &(*link)->next;
    if (!*link) return;
    *link = v->next;
    v->next = NULL;
    v->device = NULL;
    if (dev->current == v) dev->current = NULL;
    viewport_release(v);
}

static uint64_t dv_Release(const uint32_t* a, void* u) {
    lx_device_t* dev = device_of(a[0]);
    (void)u;
    if (!dev) return 0;
    if (--dev->refs > 0) return (uint32_t)dev->refs;
    lx_target_detach(dev);
    while (dev->viewports) drop_viewport(dev, dev->viewports);
    for (uint32_t k = 0; k < LX_D3D_STAGES; k++)
        if (dev->texture[k] >= 0) lx_ddraw_surface_release(dev->texture[k]);
    lx_ddraw_surface_release(dev->target);
    lx_ddraw_device_release(dev->dd);
    lx_log("d3d", "IDirect3DDevice3 at guest 0x%08X released", dev->guest);
    ht_object_free(dev->guest);
    free(dev);
    if (s_devices) s_devices--;
    return 0;
}

static uint64_t dv_GetCaps(const uint32_t* a, void* u) {
    uint8_t hal[DEVICE_DESC_BYTES], hel[DEVICE_DESC_BYTES];
    DEVICE("GetCaps");
    if (!struct_size_is(a[1], DEVICE_DESC_BYTES) || !struct_size_is(a[2], DEVICE_DESC_BYTES)) RET(LX_DDERR_INVALIDPARAMS);
    fill_device_desc(hal);
    fill_no_device_desc(hel);
    RET(gp_copy_out(a[1], hal, sizeof hal) && gp_copy_out(a[2], hel, sizeof hel) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

static uint64_t dv_AddViewport(const uint32_t* a, void* u) {
    lx_viewport_t* v = viewport_of(a[1]);
    DEVICE("AddViewport");
    if (!v) RET(LX_DDERR_INVALIDPARAMS);
    if (v->device) RET(v->device == dev ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
    v->refs++;
    v->device = dev;
    v->next = dev->viewports;
    dev->viewports = v;
    RET(LX_D3D_OK);
}

static uint64_t dv_DeleteViewport(const uint32_t* a, void* u) {
    lx_viewport_t* v = viewport_of(a[1]);
    DEVICE("DeleteViewport");
    if (!v || v->device != dev) RET(LX_DDERR_INVALIDPARAMS);
    drop_viewport(dev, v);
    RET(LX_D3D_OK);
}

static uint64_t dv_NextViewport(const uint32_t* a, void* u) {
    lx_viewport_t *v = viewport_of(a[1]), *next = NULL;
    DEVICE("NextViewport");
    if (!out_u32(a[2], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (a[3] == NEXT_HEAD) next = dev->viewports;
    else if (a[3] == NEXT_NEXT && v) next = v->next;
    else if (a[3] == NEXT_TAIL)
        for (next = dev->viewports; next && next->next;) next = next->next;
    else RET(LX_DDERR_INVALIDPARAMS);
    if (next) out_u32(a[2], next->guest);
    RET(next ? LX_D3D_OK : LX_D3DERR_NOVIEWPORTS);
}

static uint64_t dv_EnumTextureFormats(const uint32_t* a, void* u) {
    static const uint32_t masks[3][4] = {
        { 0xF800, 0x07E0, 0x001F, 0 }, { 0x7C00, 0x03E0, 0x001F, 0x8000 }, { 0x0F00, 0x00F0, 0x000F, 0xF000 },
    };
    uint32_t block;
    DEVICE("EnumTextureFormats");
    if (!a[1]) RET(LX_DDERR_INVALIDPARAMS);
    block = gm_heap_alloc(PIXEL_FORMAT_BYTES, 1);
    if (!block) RET(LX_DDERR_OUTOFMEMORY);
    for (int i = 0; i < 3; i++) {
        const uint32_t* m = masks[i];
        uint32_t pf[8] = { PIXEL_FORMAT_BYTES, DDPF_RGB | (m[3] ? DDPF_ALPHAPIXELS : 0u), 0, 16, m[0], m[1], m[2], m[3] };
        uint32_t args[2] = { block, a[2] };
        gp_copy_out(block, pf, sizeof pf);
        if (lx_call_guest(a[1], LX_STDCALL, 2, args, 0, NULL) == ENUM_CANCEL) break;
    }
    gm_heap_free(block);
    RET(LX_D3D_OK);
}

static void sync_fog_color(lx_device_t* dev) {
    uint32_t color;
    if (s_fog_sync && s_fog_sync(&color)) dev->render[LX_RS_FOGCOLOR] = (color & 0xFFFFFFu) | 0xFF000000u;
}

static uint64_t dv_BeginScene(const uint32_t* a, void* u) {
    DEVICE("BeginScene");
    if (dev->in_scene) RET(LX_D3DERR_SCENE_IN_SCENE);
    dev->in_scene = 1;
    sync_fog_color(dev);
    lx_target_begin_scene(dev);
    RET(LX_D3D_OK);
}

static uint64_t dv_EndScene(const uint32_t* a, void* u) {
    DEVICE("EndScene");
    if (!dev->in_scene) RET(LX_D3DERR_SCENE_NOT_IN_SCENE);
    dev->in_scene = 0;
    RET(LX_D3D_OK);
}

static uint64_t dv_GetDirect3D(const uint32_t* a, void* u) {
    DEVICE("GetDirect3D");
    if (!out_u32(a[1], lx_ddraw_direct3d_face(dev->dd))) RET(LX_DDERR_INVALIDPARAMS);
    lx_ddraw_device_addref(dev->dd);
    RET(LX_D3D_OK);
}

static uint64_t dv_SetCurrentViewport(const uint32_t* a, void* u) {
    lx_viewport_t* v = viewport_of(a[1]);
    DEVICE("SetCurrentViewport");
    if (!v || v->device != dev) RET(LX_DDERR_INVALIDPARAMS);
    dev->current = v;
    RET(LX_D3D_OK);
}

static uint64_t dv_GetCurrentViewport(const uint32_t* a, void* u) {
    DEVICE("GetCurrentViewport");
    if (!out_u32(a[1], 0)) RET(LX_DDERR_INVALIDPARAMS);
    if (!dev->current) RET(LX_D3DERR_NOCURRENTVIEWPORT);
    dev->current->refs++;
    out_u32(a[1], dev->current->guest);
    RET(LX_D3D_OK);
}

static uint64_t dv_GetRenderTarget(const uint32_t* a, void* u) {
    DEVICE("GetRenderTarget");
    if (!out_u32(a[1], lx_ddraw_surface_face(dev->target, 4))) RET(LX_DDERR_INVALIDPARAMS);
    lx_ddraw_surface_addref(dev->target);
    RET(LX_D3D_OK);
}

static uint64_t dv_GetRenderState(const uint32_t* a, void* u) {
    DEVICE("GetRenderState");
    if (a[1] >= LX_D3D_RENDER_STATES || !out_u32(a[2], dev->render[a[1]])) RET(LX_DDERR_INVALIDPARAMS);
    RET(LX_D3D_OK);
}

/* The render states the drawing honours; the rest are refused with one log line each. */
static int render_state_known(uint32_t state) {
    switch (state) {
    case LX_RS_TEXTUREHANDLE:
    case LX_RS_TEXTUREADDRESS:
    case LX_RS_TEXTUREPERSPECTIVE:
    case LX_RS_ZENABLE:
    case LX_RS_FILLMODE:
    case LX_RS_SHADEMODE:
    case LX_RS_ZWRITEENABLE:
    case LX_RS_ALPHATESTENABLE:
    case LX_RS_LASTPIXEL:
    case LX_RS_TEXTUREMAG:
    case LX_RS_TEXTUREMIN:
    case LX_RS_SRCBLEND:
    case LX_RS_DESTBLEND:
    case LX_RS_TEXTUREMAPBLEND:
    case LX_RS_CULLMODE:
    case LX_RS_ZFUNC:
    case LX_RS_ALPHAREF:
    case LX_RS_ALPHAFUNC:
    case LX_RS_DITHERENABLE:
    case LX_RS_ALPHABLENDENABLE:
    case LX_RS_FOGENABLE:
    case LX_RS_SPECULARENABLE:
    case LX_RS_SUBPIXEL:
    case LX_RS_SUBPIXELX:
    case LX_RS_STIPPLEDALPHA:
    case LX_RS_FOGCOLOR:
    case LX_RS_FOGTABLEMODE:
    case LX_RS_COLORKEYENABLE:
    case LX_RS_TEXTUREADDRESSU:
    case LX_RS_TEXTUREADDRESSV:
    case LX_RS_ZBIAS:
    case LX_RS_RANGEFOGENABLE: return 1;
    default: return 0;
    }
}

static int render_value_valid(uint32_t state, uint32_t value) {
    switch (state) {
    case LX_RS_SRCBLEND:
    case LX_RS_DESTBLEND: return value >= 1 && value <= 13;
    case LX_RS_ZFUNC:
    case LX_RS_ALPHAFUNC: return d3dt_compare(value) != 0;
    case LX_RS_CULLMODE: return d3dt_cull(value) != 0;
    case LX_RS_FILLMODE: return d3dt_fill(value) != 0;
    case LX_RS_TEXTUREMAPBLEND: return value >= 1 && value <= 8;
    case LX_RS_ZENABLE: return value <= 2;
    case LX_RS_FOGTABLEMODE: return value == 0;
    case LX_RS_TEXTUREHANDLE: return value == 0;
    case LX_RS_ZBIAS: return value <= 16;
    default: return 1;
    }
}

static uint64_t dv_SetRenderState(const uint32_t* a, void* u) {
    const uint32_t state = a[1], value = a[2];
    DEVICE("SetRenderState");
    if (state >= LX_D3D_RENDER_STATES || !render_state_known(state) || !render_value_valid(state, value)) {
        lx_target_stats()->refused_states++;
        if (state < LX_D3D_RENDER_STATES && !s_logged_render[state]) {
            s_logged_render[state] = 1;
            lx_log("d3d", "render state %u (D3DRENDERSTATE_%s) = 0x%X is not translated: refused", state, d3dt_render_state_name(state),
                   value);
        }
        return LX_DDERR_INVALIDPARAMS;
    }
    dev->render[state] = value;
    switch (state) {
    case LX_RS_TEXTUREADDRESS: dev->stage[0][LX_TSS_ADDRESSU] = dev->stage[0][LX_TSS_ADDRESSV] = value; break;
    case LX_RS_TEXTUREADDRESSU: dev->stage[0][LX_TSS_ADDRESSU] = value; break;
    case LX_RS_TEXTUREADDRESSV: dev->stage[0][LX_TSS_ADDRESSV] = value; break;
    case LX_RS_TEXTUREMAG: dev->stage[0][LX_TSS_MAGFILTER] = value == 1 ? 1 : 2; break;
    case LX_RS_TEXTUREMIN: dev->stage[0][LX_TSS_MINFILTER] = value == 1 || value == 3 ? 1 : 2; break;
    default: break;
    }
    return LX_D3D_OK;
}

static float* transform_of(lx_device_t* dev, uint32_t type) { return type >= 1 && type <= 3 ? dev->transforms[type - 1] : NULL; }

static uint64_t dv_SetTransform(const uint32_t* a, void* u) {
    float* slot;
    DEVICE("SetTransform");
    slot = transform_of(dev, a[1]);
    RET(slot && a[2] && gp_copy_in(slot, a[2], MATRIX_BYTES) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

static uint64_t dv_GetTransform(const uint32_t* a, void* u) {
    float* slot;
    DEVICE("GetTransform");
    slot = transform_of(dev, a[1]);
    RET(slot && a[2] && gp_copy_out(a[2], slot, MATRIX_BYTES) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

static uint64_t dv_MultiplyTransform(const uint32_t* a, void* u) {
    float *slot, m[16];
    DEVICE("MultiplyTransform");
    slot = transform_of(dev, a[1]);
    if (!slot || !a[2] || !gp_copy_in(m, a[2], sizeof m)) RET(LX_DDERR_INVALIDPARAMS);
    d3dt_matrix_multiply(m, slot, slot);
    RET(LX_D3D_OK);
}

static uint64_t dv_DrawPrimitive(const uint32_t* a, void* u) {
    DEVICE("DrawPrimitive");
    if (!a[3]) RET(LX_DDERR_INVALIDPARAMS);
    RET(lx_target_draw(dev, a[1], a[2], a[3], a[4], NULL, 0));
}

static uint64_t dv_DrawIndexedPrimitive(const uint32_t* a, void* u) {
    const uint16_t* indices;
    DEVICE("DrawIndexedPrimitive");
    if (!a[3] || !a[5] || a[6] > 0x100000u) RET(LX_DDERR_INVALIDPARAMS);
    if (!a[6]) RET(LX_D3D_OK);
    indices = (const uint16_t*)gm_ptr_checked(a[5], a[6] * 2u, 0);
    if (!indices) RET(LX_DDERR_INVALIDPARAMS);
    RET(lx_target_draw(dev, a[1], a[2], a[3], a[4], indices, a[6]));
}

static uint64_t dv_GetTexture(const uint32_t* a, void* u) {
    int slot;
    DEVICE("GetTexture");
    if (a[1] >= LX_D3D_STAGES || !out_u32(a[2], 0)) RET(LX_DDERR_INVALIDPARAMS);
    slot = dev->texture[a[1]];
    if (slot >= 0) {
        lx_ddraw_surface_addref(slot);
        out_u32(a[2], lx_ddraw_surface_face(slot, LX_DDRAW_FACE_TEXTURE2));
    }
    RET(LX_D3D_OK);
}

static uint64_t dv_SetTexture(const uint32_t* a, void* u) {
    int slot = a[2] ? lx_ddraw_texture_from(a[2]) : -1, old;
    DEVICE("SetTexture");
    if (a[1] >= LX_D3D_STAGES || (a[2] && slot < 0)) RET(LX_DDERR_INVALIDPARAMS);
    old = dev->texture[a[1]];
    if (slot == old) return LX_D3D_OK;
    if (slot >= 0) lx_ddraw_surface_addref(slot);
    dev->texture[a[1]] = slot;
    if (old >= 0) lx_ddraw_surface_release(old);
    return LX_D3D_OK;
}

static uint64_t dv_GetTextureStageState(const uint32_t* a, void* u) {
    DEVICE("GetTextureStageState");
    if (a[1] >= LX_D3D_STAGES || a[2] >= LX_D3D_STAGE_STATES || !out_u32(a[3], dev->stage[a[1]][a[2]])) RET(LX_DDERR_INVALIDPARAMS);
    RET(LX_D3D_OK);
}

/* Stages above 0 keep COLOROP and ALPHAOP DISABLE, so only stage 0 draws. */
static int stage_value_valid(uint32_t stage, uint32_t type, uint32_t value) {
    switch (type) {
    case LX_TSS_ADDRESS:
    case LX_TSS_ADDRESSU:
    case LX_TSS_ADDRESSV: return value >= 1 && value <= 3;
    case LX_TSS_MAGFILTER: return d3dt_mag_linear(value) >= 0;
    case LX_TSS_MINFILTER: return d3dt_min_linear(value) >= 0;
    case LX_TSS_MIPFILTER: return d3dt_mip_mode(value) >= 0;
    case LX_TSS_COLOROP:
    case LX_TSS_ALPHAOP: return stage > 0 && value == 1;
    case LX_TSS_TEXCOORDINDEX: return value == stage;
    default: return 0;
    }
}

static uint64_t dv_SetTextureStageState(const uint32_t* a, void* u) {
    const uint32_t stage = a[1], type = a[2], value = a[3];
    DEVICE("SetTextureStageState");
    if (stage >= LX_D3D_STAGES || type >= LX_D3D_STAGE_STATES || !stage_value_valid(stage, type, value)) {
        lx_target_stats()->refused_states++;
        if (stage < LX_D3D_STAGES && type < LX_D3D_STAGE_STATES && !s_logged_stage[stage][type]) {
            s_logged_stage[stage][type] = 1;
            lx_log("d3d", "texture stage %u state %u (D3DTSS_%s) = 0x%X is not translated: refused", stage, type,
                   d3dt_stage_state_name(type), value);
        }
        return LX_DDERR_INVALIDPARAMS;
    }
    dev->stage[stage][type] = value;
    if (type == LX_TSS_ADDRESS) dev->stage[stage][LX_TSS_ADDRESSU] = dev->stage[stage][LX_TSS_ADDRESSV] = value;
    return LX_D3D_OK;
}

static uint64_t dv_ValidateDevice(const uint32_t* a, void* u) {
    DEVICE("ValidateDevice");
    RET(out_u32(a[1], 1) ? LX_D3D_OK : LX_DDERR_INVALIDPARAMS);
}

/* ---- IDirect3DTexture2 ---- */

static uint64_t tx_QueryInterface(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3DTexture2::QueryInterface";
    int slot = lx_ddraw_texture_from(a[0]);
    (void)u;
    RET(slot < 0 ? LX_DDERR_INVALIDPARAMS : lx_ddraw_surface_query(slot, a[1], a[2]));
}

static uint64_t tx_AddRef(const uint32_t* a, void* u) {
    int slot = lx_ddraw_texture_from(a[0]);
    (void)u;
    return slot < 0 ? 0 : lx_ddraw_surface_addref(slot);
}

static uint64_t tx_Release(const uint32_t* a, void* u) {
    int slot = lx_ddraw_texture_from(a[0]);
    (void)u;
    return slot < 0 ? 0 : lx_ddraw_surface_release(slot);
}

/* The pixels and the source's colour key, at once; only the rows that differ become dirty. */
static uint64_t tx_Load(const uint32_t* a, void* u) {
    static const char k_method[] = "IDirect3DTexture2::Load";
    int dst = lx_ddraw_texture_from(a[0]), src = lx_ddraw_texture_from(a[1]);
    lx_surface_view_t d, s;
    d3dt_rect_t changed;
    int key_changed;
    (void)u;
    if (dst < 0 || src < 0) RET(LX_DDERR_INVALIDPARAMS);
    if (src == dst) RET(LX_D3D_OK);
    if (!lx_ddraw_surface_view(dst, &d) || !lx_ddraw_surface_view(src, &s)) RET(LX_DDERR_INVALIDPARAMS);
    if (s.width != d.width || s.height != d.height || memcmp(s.pf + 1, d.pf + 1, 7 * sizeof s.pf[0])) RET(LX_DDERR_INVALIDPARAMS);
    if (d.busy || s.busy) RET(LX_DDERR_SURFACEBUSY);
    lx_target_texture_loaded(dst, src);
    key_changed = s.has_src_key && (!d.has_src_key || memcmp(d.src_key, s.src_key, sizeof s.src_key));
    if (s.has_src_key) lx_ddraw_surface_set_src_key(dst, s.src_key);
    if (d3dt_copy_changed(d.bits, d.pitch, s.bits, s.pitch, d.width, d.height, d.bytes, &changed)) {
        int32_t r[4] = { (int32_t)changed.left, (int32_t)changed.top, (int32_t)changed.right, (int32_t)changed.bottom };
        lx_d3d_cpu_written(dst, key_changed ? NULL : r);
    } else if (key_changed) {
        lx_d3d_cpu_written(dst, NULL);
    }
    RET(LX_D3D_OK);
}

/* ---- the vtables, in the order of d3d.h ---- */

#define D3(name, argc) { "IDirect3D3::" #name, d3_##name, argc }
#define D3U(name, argc) { "IDirect3D3::" #name, NULL, argc }
#define DV(name, argc) { "IDirect3DDevice3::" #name, dv_##name, argc }
#define DVU(name, argc) { "IDirect3DDevice3::" #name, NULL, argc }
#define VP(name, argc) { "IDirect3DViewport3::" #name, vp_##name, argc }
#define VPU(name, argc) { "IDirect3DViewport3::" #name, NULL, argc }
#define TX(name, argc) { "IDirect3DTexture2::" #name, tx_##name, argc }
#define TXU(name, argc) { "IDirect3DTexture2::" #name, NULL, argc }

static const lx_d3d_method_t k_direct3d3[] = {
    D3(QueryInterface, 3), D3(AddRef, 1), D3(Release, 1), D3(EnumDevices, 3), D3U(CreateLight, 3), D3U(CreateMaterial, 3),
    D3(CreateViewport, 3), D3U(FindDevice, 3), D3(CreateDevice, 5), D3U(CreateVertexBuffer, 5), D3(EnumZBufferFormats, 4),
    D3(EvictManagedTextures, 1),
};

static const lx_d3d_method_t k_device3[] = {
    DV(QueryInterface, 3), DV(AddRef, 1), DV(Release, 1), DV(GetCaps, 3), DVU(GetStats, 2), DV(AddViewport, 2), DV(DeleteViewport, 2),
    DV(NextViewport, 4), DV(EnumTextureFormats, 3), DV(BeginScene, 1), DV(EndScene, 1), DV(GetDirect3D, 2), DV(SetCurrentViewport, 2),
    DV(GetCurrentViewport, 2), DVU(SetRenderTarget, 3), DV(GetRenderTarget, 2), DVU(Begin, 4), DVU(BeginIndexed, 6), DVU(Vertex, 2),
    DVU(Index, 2), DVU(End, 2), DV(GetRenderState, 3), DV(SetRenderState, 3), DVU(GetLightState, 3), DVU(SetLightState, 3),
    DV(SetTransform, 3), DV(GetTransform, 3), DV(MultiplyTransform, 3), DV(DrawPrimitive, 6), DV(DrawIndexedPrimitive, 8),
    DVU(SetClipStatus, 2), DVU(GetClipStatus, 2), DVU(DrawPrimitiveStrided, 6), DVU(DrawIndexedPrimitiveStrided, 8),
    DVU(DrawPrimitiveVB, 6), DVU(DrawIndexedPrimitiveVB, 6), DVU(ComputeSphereVisibility, 6), DV(GetTexture, 3), DV(SetTexture, 3),
    DV(GetTextureStageState, 4), DV(SetTextureStageState, 4), DV(ValidateDevice, 2),
};

static const lx_d3d_method_t k_viewport3[] = {
    VP(QueryInterface, 3), VP(AddRef, 1), VP(Release, 1), VP(Initialize, 2), VP(GetViewport, 2), VP(SetViewport, 2),
    VPU(TransformVertices, 5), VPU(LightElements, 3), VPU(SetBackground, 2), VPU(GetBackground, 3), VPU(SetBackgroundDepth, 2),
    VPU(GetBackgroundDepth, 3), VPU(Clear, 4), VPU(AddLight, 2), VPU(DeleteLight, 2), VPU(NextLight, 4), VP(GetViewport2, 2),
    VP(SetViewport2, 2), VPU(SetBackgroundDepth2, 2), VPU(GetBackgroundDepth2, 3), VP(Clear2, 7),
};

static const lx_d3d_method_t k_texture2[] = {
    TX(QueryInterface, 3), TX(AddRef, 1), TX(Release, 1), TXU(GetHandle, 3), TXU(PaletteChanged, 3), TX(Load, 2),
};

_Static_assert(sizeof k_direct3d3 / sizeof k_direct3d3[0] == 12, "IDirect3D3 has 12 methods");
_Static_assert(sizeof k_device3 / sizeof k_device3[0] == 42, "IDirect3DDevice3 has 42 methods");
_Static_assert(sizeof k_viewport3 / sizeof k_viewport3[0] == 21, "IDirect3DViewport3 has 21 methods");
_Static_assert(sizeof k_texture2 / sizeof k_texture2[0] == 6, "IDirect3DTexture2 has 6 methods");

const lx_d3d_method_t* lx_d3d_methods(lx_d3d_interface_t which, unsigned* count) {
    switch (which) {
    case LX_D3D_DIRECT3D3: *count = sizeof k_direct3d3 / sizeof k_direct3d3[0]; return k_direct3d3;
    case LX_D3D_DEVICE3: *count = sizeof k_device3 / sizeof k_device3[0]; return k_device3;
    case LX_D3D_VIEWPORT3: *count = sizeof k_viewport3 / sizeof k_viewport3[0]; return k_viewport3;
    case LX_D3D_TEXTURE2: *count = sizeof k_texture2 / sizeof k_texture2[0]; return k_texture2;
    default: *count = 0; return NULL;
    }
}

uint32_t lx_d3d_vtable(lx_d3d_interface_t which) {
    unsigned count;
    const lx_d3d_method_t* m = lx_d3d_methods(which, &count);
    uint32_t va;
    if (!m) return 0;
    if (s_vtable[which]) return s_vtable[which];
    va = gm_heap_alloc(4u * count, 1);
    if (!va) return 0;
    for (unsigned i = 0; i < count; i++)
        gp_write_u32(va + 4u * i, ht_register(m[i].name, m[i].fn ? m[i].fn : unsupported, m[i].argc, HT_STDCALL, (void*)m[i].name));
    s_vtable[which] = va;
    return va;
}
