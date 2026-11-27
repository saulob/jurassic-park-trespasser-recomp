/*
 * IDirect3D3, IDirect3DDevice3, IDirect3DViewport3 and IDirect3DTexture2 as the game uses them
 * (docs/D3D_LEGACY_INVENTORY.md). They hold Direct3D 6 state; d3d11_renderer.c draws with it.
 * IDirect3D3 is a face of the DirectDraw object and IDirect3DTexture2 a face of the texture
 * surface, sharing their reference counts as the aggregated Direct3D 6 objects did.
 */
#include "d3d_legacy.h"
#include "d3d_translate.h"
#include "host.h"

#include <stdio.h>

#define LOG_LIMIT 300

static const IDirect3D3Vtbl s_d3d3_vtbl;
static const IDirect3DDevice3Vtbl s_device3_vtbl;
static const IDirect3DViewport3Vtbl s_viewport3_vtbl;
static const IDirect3DTexture2Vtbl s_texture2_vtbl;

static int s_lines;
static unsigned char s_unknown_render[D3D_RENDER_STATES / 8], s_unknown_stage[D3D_TEXTURE_STAGES][D3D_STAGE_STATES / 8];
static unsigned long s_refused_render, s_refused_stage, s_failed_calls;
static void (*s_fog_sync)(d3d_device_t*);

void d3d_set_fog_sync(void (*sync)(d3d_device_t*)) { s_fog_sync = sync; }

void d3d_objects_counts(unsigned long* render_states, unsigned long* stage_states, unsigned long* failed_calls) {
    *render_states = s_refused_render;
    *stage_states = s_refused_stage;
    *failed_calls = s_failed_calls;
}

static void d3d_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    if (s_lines >= LOG_LIMIT) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    host_log("d3d", "%s", line);
    if (++s_lines == LOG_LIMIT) host_log("d3d", "line limit reached; later Direct3D messages are not logged");
}

static HRESULT d3d_done(dd_method_t* m, HRESULT hr) {
    char buf[40];
    dd_returned(m, hr);
    if (FAILED(hr)) s_failed_calls++;
    if (m->calls++ == 0)
        d3d_log("first call to %s from sub_%08X -> %s", m->name, g_cur_func, ddraw_hresult_text((uint32_t)hr, buf, sizeof buf));
    else if (FAILED(hr))
        d3d_log("%s from sub_%08X FAILED -> %s", m->name, g_cur_func, ddraw_hresult_text((uint32_t)hr, buf, sizeof buf));
    return hr;
}

#define D3D_METHOD(label) static dd_method_t d3d_label_ = { label, 0 }; dd_method_t* d3d_method_ = &d3d_label_; \
    dd_begin(d3d_method_); dd_lock()
#define D3D_RETURN(hr) do { HRESULT d3d_hr_ = (hr); dd_unlock(); return d3d_done(d3d_method_, d3d_hr_); } while (0)
/* Never a false success: what the game does not use is refused, with the caller logged once. */
#define D3D_UNSUPPORTED(label) do { static dd_method_t d3d_m_ = { label, 0 }; dd_begin(&d3d_m_); \
    if (d3d_m_.calls == 0) d3d_log("%s is not implemented (the game was not seen calling it)", label); \
    return d3d_done(&d3d_m_, DDERR_UNSUPPORTED); } while (0)

static int first_time(unsigned char* seen, DWORD index) {
    if (seen[index / 8] & (1u << (index % 8))) return 0;
    seen[index / 8] |= (unsigned char)(1u << (index % 8));
    return 1;
}

static void refuse_render_state(DWORD state, DWORD value) {
    s_refused_render++;
    if (state < D3D_RENDER_STATES && first_time(s_unknown_render, state))
        d3d_log("render state %lu (D3DRENDERSTATE_%s) = 0x%lX from sub_%08X is not translated: refused", state,
                d3dt_render_state_name(state), value, g_cur_func);
}

static void refuse_stage_state(DWORD stage, DWORD type, DWORD value) {
    s_refused_stage++;
    if (stage < D3D_TEXTURE_STAGES && type < D3D_STAGE_STATES && first_time(s_unknown_stage[stage], type))
        d3d_log("texture stage %lu state %lu (D3DTSS_%s) = 0x%lX from sub_%08X is not translated: refused", stage, type,
                d3dt_stage_state_name(type), value, g_cur_func);
}

/* ---- IDirect3D3 (a face of the DirectDraw object) ---- */

void d3d_init_direct3d_face(dd_device_t* d) {
    d->d3d3.vtbl = &s_d3d3_vtbl;
    d->d3d3.owner = d;
    d->d3d3.version = 3;
}

HRESULT d3d_query_direct3d(dd_device_t* d, REFIID riid, void** out) {
    if (!IsEqualGUID(riid, &IID_IDirect3D3)) {
        d3d_log("QueryInterface for IDirect3D or IDirect3D2 from sub_%08X: only IDirect3D3 is implemented", g_cur_func);
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&d->refs);
    *out = &d->d3d3;
    return DD_OK;
}

#define D3D3_OWNER(This) DD_OWNER(dd_device_t, This)

static HRESULT STDMETHODCALLTYPE d3_QueryInterface(IDirect3D3* This, REFIID riid, LPVOID* out) {
    dd_device_t* d = D3D3_OWNER(This);
    D3D_METHOD("IDirect3D3::QueryInterface");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D3)) {
        InterlockedIncrement(&d->refs);
        *out = This;
        D3D_RETURN(DD_OK);
    }
    D3D_RETURN(((IDirectDraw*)&d->dd1)->lpVtbl->QueryInterface((IDirectDraw*)&d->dd1, riid, out));
}

static ULONG STDMETHODCALLTYPE d3_AddRef(IDirect3D3* This) {
    dd_device_t* d = D3D3_OWNER(This);
    return ((IDirectDraw*)&d->dd1)->lpVtbl->AddRef((IDirectDraw*)&d->dd1);
}

static ULONG STDMETHODCALLTYPE d3_Release(IDirect3D3* This) {
    dd_device_t* d = D3D3_OWNER(This);
    return ((IDirectDraw*)&d->dd1)->lpVtbl->Release((IDirectDraw*)&d->dd1);
}

/* What the game's acceptance test requires (sub_00408530), and true of any D3D11 GPU. */
void d3d_fill_device_desc(D3DDEVICEDESC* desc) {
    D3DPRIMCAPS* tri = &desc->dpcTriCaps;
    memset(desc, 0, sizeof *desc);
    desc->dwSize = sizeof *desc;
    desc->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                    D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                    D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    desc->dcmColorModel = D3DCOLOR_RGB;
    desc->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                      D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                      D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_HWRASTERIZATION;
    desc->dtcTransformCaps.dwSize = sizeof desc->dtcTransformCaps;
    desc->dlcLightingCaps.dwSize = sizeof desc->dlcLightingCaps;
    tri->dwSize = sizeof *tri;
    tri->dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_MASKZ;
    tri->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_ZTEST |
                        D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_ANISOTROPY;
    tri->dwZCmpCaps = tri->dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                                            D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL |
                                            D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    tri->dwSrcBlendCaps = tri->dwDestBlendCaps = 0x1FFF;
    tri->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARFLATRGB |
                       D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND |
                       D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
    tri->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_ALPHA |
                         D3DPTEXTURECAPS_TRANSPARENCY;
    tri->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MINFPOINT |
                               D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    tri->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                              D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    tri->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                                D3DPTADDRESSCAPS_INDEPENDENTUV;
    desc->dpcLineCaps = *tri;
    desc->dwDeviceRenderBitDepth = DDBD_16;
    desc->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24;
    desc->dwMaxBufferSize = 0;
    desc->dwMaxVertexCount = 65535;
    desc->dwMinTextureWidth = desc->dwMinTextureHeight = 1;
    desc->dwMaxTextureWidth = desc->dwMaxTextureHeight = 2048;
    desc->dwMaxTextureRepeat = 2048;
    desc->dwMaxTextureAspectRatio = 2048;
    desc->dwMaxAnisotropy = 16;
    desc->dvGuardBandLeft = desc->dvGuardBandTop = -2048.0f;
    desc->dvGuardBandRight = desc->dvGuardBandBottom = 2047.0f;
    desc->dwFVFCaps = 1;
    desc->dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 |
                            D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_BLENDTEXTUREALPHA;
    desc->wMaxTextureBlendStages = 1;
    desc->wMaxSimultaneousTextures = 1;
}

static HRESULT STDMETHODCALLTYPE d3_EnumDevices(IDirect3D3* This, LPD3DENUMDEVICESCALLBACK callback, LPVOID context) {
    D3DDEVICEDESC hal, hel;
    GUID guid = IID_IDirect3DHALDevice;
    (void)This;
    D3D_METHOD("IDirect3D3::EnumDevices");
    if (!callback) D3D_RETURN(DDERR_INVALIDPARAMS);
    d3d_fill_device_desc(&hal);
    memset(&hel, 0, sizeof hel);
    hel.dwSize = sizeof hel;
    {
        char desc_name[96];
        _snprintf(desc_name, sizeof desc_name - 1, "%s hardware (Trespasser compatibility layer)", d3dr_name());
        desc_name[sizeof desc_name - 1] = 0;
        callback(&guid, desc_name, "Direct3D HAL", &hal, &hel, context);
    }
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE d3_CreateLight(IDirect3D3* This, LPDIRECT3DLIGHT* out, LPUNKNOWN outer) {
    (void)This; (void)out; (void)outer;
    D3D_UNSUPPORTED("IDirect3D3::CreateLight");
}

static HRESULT STDMETHODCALLTYPE d3_CreateMaterial(IDirect3D3* This, LPDIRECT3DMATERIAL3* out, LPUNKNOWN outer) {
    (void)This; (void)out; (void)outer;
    D3D_UNSUPPORTED("IDirect3D3::CreateMaterial");
}

static HRESULT STDMETHODCALLTYPE d3_CreateViewport(IDirect3D3* This, LPDIRECT3DVIEWPORT3* out, LPUNKNOWN outer) {
    d3d_viewport_t* v;
    (void)This;
    D3D_METHOD("IDirect3D3::CreateViewport");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (outer) D3D_RETURN(CLASS_E_NOAGGREGATION);
    v = (d3d_viewport_t*)calloc(1, sizeof *v);
    if (!v) D3D_RETURN(DDERR_OUTOFMEMORY);
    v->face.vtbl = &s_viewport3_vtbl;
    v->face.owner = v;
    v->face.version = 3;
    v->refs = 1;
    *out = (LPDIRECT3DVIEWPORT3)&v->face;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE d3_FindDevice(IDirect3D3* This, LPD3DFINDDEVICESEARCH search, LPD3DFINDDEVICERESULT result) {
    (void)This; (void)search; (void)result;
    D3D_UNSUPPORTED("IDirect3D3::FindDevice");
}

static HRESULT STDMETHODCALLTYPE d3_CreateDevice(IDirect3D3* This, REFCLSID guid, LPDIRECTDRAWSURFACE4 surface,
                                                 LPDIRECT3DDEVICE3* out, LPUNKNOWN outer) {
    dd_device_t* d = D3D3_OWNER(This);
    dd_surface_t* target = surface ? dd_surface_from(surface) : NULL;
    d3d_device_t* dev;
    HRESULT hr;
    D3D_METHOD("IDirect3D3::CreateDevice");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (outer) D3D_RETURN(CLASS_E_NOAGGREGATION);
    if (!target) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (!IsEqualGUID(guid, &IID_IDirect3DHALDevice))
        d3d_log("CreateDevice for {%08lX-%04X-...}: answered with the %s hardware device", guid->Data1, guid->Data2,
                d3dr_name());
    dev = (d3d_device_t*)calloc(1, sizeof *dev);
    if (!dev) D3D_RETURN(DDERR_OUTOFMEMORY);
    dev->face.vtbl = &s_device3_vtbl;
    dev->face.owner = dev;
    dev->face.version = 3;
    dev->refs = 1;
    dev->dd = d;
    dev->target = target;
    dd_surface_addref(target);
    InterlockedIncrement(&d->refs);
    dev->render[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
    dev->render[D3DRENDERSTATE_ZENABLE] = target->zbuffer ? D3DZB_TRUE : D3DZB_FALSE;
    dev->render[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
    dev->render[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    dev->render[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    dev->render[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    dev->render[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    dev->render[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    dev->render[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    dev->render[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    dev->render[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    dev->render[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_NEAREST;
    dev->render[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_NEAREST;
    dev->render[D3DRENDERSTATE_TEXTUREADDRESS] = D3DTADDRESS_WRAP;
    dev->render[D3DRENDERSTATE_TEXTUREADDRESSU] = D3DTADDRESS_WRAP;
    dev->render[D3DRENDERSTATE_TEXTUREADDRESSV] = D3DTADDRESS_WRAP;
    for (DWORD k = 0; k < D3D_TEXTURE_STAGES; k++) {
        DWORD* st = dev->stage[k];
        st[D3DTSS_COLOROP] = k ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        st[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        st[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        st[D3DTSS_ALPHAOP] = k ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        st[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        st[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        st[D3DTSS_TEXCOORDINDEX] = k;
        st[D3DTSS_ADDRESS] = st[D3DTSS_ADDRESSU] = st[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        st[D3DTSS_MAGFILTER] = D3DTFG_POINT;
        st[D3DTSS_MINFILTER] = D3DTFN_POINT;
        st[D3DTSS_MIPFILTER] = D3DTFP_NONE;
        st[D3DTSS_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) (&dev->transforms[i]._11)[k * 5] = 1.0f;
    hr = d3dr_attach(dev);
    if (FAILED(hr)) {
        dd_surface_release(target);
        InterlockedDecrement(&d->refs);
        free(dev);
        D3D_RETURN(hr);
    }
    d3d_log("CreateDevice: IDirect3DDevice3 0x%08X renders into surface 0x%08X (%lux%lu)%s", (uint32_t)(uintptr_t)dev,
            (uint32_t)(uintptr_t)target, target->width, target->height, target->zbuffer ? " with its z-buffer" : "");
    *out = (LPDIRECT3DDEVICE3)&dev->face;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE d3_CreateVertexBuffer(IDirect3D3* This, LPD3DVERTEXBUFFERDESC desc,
                                                       LPDIRECT3DVERTEXBUFFER* out, DWORD flags, LPUNKNOWN outer) {
    (void)This; (void)desc; (void)out; (void)flags; (void)outer;
    D3D_UNSUPPORTED("IDirect3D3::CreateVertexBuffer");
}

static HRESULT STDMETHODCALLTYPE d3_EnumZBufferFormats(IDirect3D3* This, REFCLSID guid, LPD3DENUMPIXELFORMATSCALLBACK callback,
                                                       LPVOID context) {
    static const DWORD depths[] = { 16, 24 };
    (void)This; (void)guid;
    D3D_METHOD("IDirect3D3::EnumZBufferFormats");
    if (!callback) D3D_RETURN(DDERR_INVALIDPARAMS);
    for (int i = 0; i < 2; i++) {
        DDPIXELFORMAT pf;
        memset(&pf, 0, sizeof pf);
        pf.dwSize = sizeof pf;
        pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = depths[i];
        pf.dwZBitMask = depths[i] == 16 ? 0xFFFFu : 0xFFFFFFu;
        if (callback(&pf, context) == D3DENUMRET_CANCEL) break;
    }
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE d3_EvictManagedTextures(IDirect3D3* This) {
    (void)This;
    D3D_METHOD("IDirect3D3::EvictManagedTextures");
    D3D_RETURN(DD_OK);
}

/* ---- IDirect3DViewport3 ---- */

#define VP(This) DD_OWNER(d3d_viewport_t, This)

static HRESULT STDMETHODCALLTYPE vp_QueryInterface(IDirect3DViewport3* This, REFIID riid, LPVOID* out) {
    D3D_METHOD("IDirect3DViewport3::QueryInterface");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDirect3DViewport3)) D3D_RETURN(E_NOINTERFACE);
    InterlockedIncrement(&VP(This)->refs);
    *out = This;
    D3D_RETURN(DD_OK);
}

static ULONG STDMETHODCALLTYPE vp_AddRef(IDirect3DViewport3* This) { return (ULONG)InterlockedIncrement(&VP(This)->refs); }

static ULONG STDMETHODCALLTYPE vp_Release(IDirect3DViewport3* This) {
    d3d_viewport_t* v = VP(This);
    LONG refs = InterlockedDecrement(&v->refs);
    if (refs <= 0) {
        free(v);
        return 0;
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE vp_Initialize(IDirect3DViewport3* This, LPDIRECT3D d3d) {
    (void)This; (void)d3d;
    D3D_METHOD("IDirect3DViewport3::Initialize");
    D3D_RETURN(DDERR_ALREADYINITIALIZED);
}

static HRESULT STDMETHODCALLTYPE vp_GetViewport(IDirect3DViewport3* This, LPD3DVIEWPORT data) {
    d3d_viewport_t* v = VP(This);
    D3D_METHOD("IDirect3DViewport3::GetViewport");
    if (!data || data->dwSize != sizeof *data) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (!v->data_set) D3D_RETURN(D3DERR_VIEWPORTDATANOTSET);
    data->dwX = v->data.dwX;
    data->dwY = v->data.dwY;
    data->dwWidth = v->data.dwWidth;
    data->dwHeight = v->data.dwHeight;
    data->dvScaleX = v->data.dwWidth * 0.5f;
    data->dvScaleY = v->data.dwHeight * 0.5f;
    data->dvMaxX = v->data.dvClipWidth * 0.5f;
    data->dvMaxY = v->data.dvClipHeight * 0.5f;
    data->dvMinZ = v->data.dvMinZ;
    data->dvMaxZ = v->data.dvMaxZ;
    D3D_RETURN(DD_OK);
}

static void viewport_changed(d3d_viewport_t* v) {
    if (v->device && v->device->current == v) d3dr_state_will_change(v->device, D3DR_CHANGE_VIEWPORT, 0, 0);
}

static HRESULT STDMETHODCALLTYPE vp_SetViewport(IDirect3DViewport3* This, LPD3DVIEWPORT data) {
    d3d_viewport_t* v = VP(This);
    D3D_METHOD("IDirect3DViewport3::SetViewport");
    if (!data || data->dwSize != sizeof *data) D3D_RETURN(DDERR_INVALIDPARAMS);
    viewport_changed(v);
    memset(&v->data, 0, sizeof v->data);
    v->data.dwSize = sizeof v->data;
    v->data.dwX = data->dwX;
    v->data.dwY = data->dwY;
    v->data.dwWidth = data->dwWidth;
    v->data.dwHeight = data->dwHeight;
    v->data.dvClipWidth = data->dvMaxX * 2.0f;
    v->data.dvClipHeight = data->dvMaxY * 2.0f;
    v->data.dvMinZ = data->dvMinZ;
    v->data.dvMaxZ = data->dvMaxZ;
    v->data_set = 1;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE vp_TransformVertices(IDirect3DViewport3* This, DWORD count, LPD3DTRANSFORMDATA data,
                                                      DWORD flags, LPDWORD offscreen) {
    (void)This; (void)count; (void)data; (void)flags; (void)offscreen;
    D3D_UNSUPPORTED("IDirect3DViewport3::TransformVertices");
}

static HRESULT STDMETHODCALLTYPE vp_LightElements(IDirect3DViewport3* This, DWORD count, LPD3DLIGHTDATA data) {
    (void)This; (void)count; (void)data;
    D3D_UNSUPPORTED("IDirect3DViewport3::LightElements");
}

static HRESULT STDMETHODCALLTYPE vp_SetBackground(IDirect3DViewport3* This, D3DMATERIALHANDLE material) {
    (void)This; (void)material;
    D3D_UNSUPPORTED("IDirect3DViewport3::SetBackground");
}

static HRESULT STDMETHODCALLTYPE vp_GetBackground(IDirect3DViewport3* This, LPD3DMATERIALHANDLE material, LPBOOL valid) {
    (void)This; (void)material; (void)valid;
    D3D_UNSUPPORTED("IDirect3DViewport3::GetBackground");
}

static HRESULT STDMETHODCALLTYPE vp_SetBackgroundDepth(IDirect3DViewport3* This, LPDIRECTDRAWSURFACE surface) {
    (void)This; (void)surface;
    D3D_UNSUPPORTED("IDirect3DViewport3::SetBackgroundDepth");
}

static HRESULT STDMETHODCALLTYPE vp_GetBackgroundDepth(IDirect3DViewport3* This, LPDIRECTDRAWSURFACE* surface, LPBOOL valid) {
    (void)This; (void)surface; (void)valid;
    D3D_UNSUPPORTED("IDirect3DViewport3::GetBackgroundDepth");
}

static HRESULT STDMETHODCALLTYPE vp_Clear(IDirect3DViewport3* This, DWORD count, LPD3DRECT rects, DWORD flags) {
    (void)This; (void)count; (void)rects; (void)flags;
    D3D_UNSUPPORTED("IDirect3DViewport3::Clear");
}

static HRESULT STDMETHODCALLTYPE vp_AddLight(IDirect3DViewport3* This, LPDIRECT3DLIGHT light) {
    (void)This; (void)light;
    D3D_UNSUPPORTED("IDirect3DViewport3::AddLight");
}

static HRESULT STDMETHODCALLTYPE vp_DeleteLight(IDirect3DViewport3* This, LPDIRECT3DLIGHT light) {
    (void)This; (void)light;
    D3D_UNSUPPORTED("IDirect3DViewport3::DeleteLight");
}

static HRESULT STDMETHODCALLTYPE vp_NextLight(IDirect3DViewport3* This, LPDIRECT3DLIGHT light, LPDIRECT3DLIGHT* out, DWORD flags) {
    (void)This; (void)light; (void)out; (void)flags;
    D3D_UNSUPPORTED("IDirect3DViewport3::NextLight");
}

static HRESULT STDMETHODCALLTYPE vp_GetViewport2(IDirect3DViewport3* This, LPD3DVIEWPORT2 data) {
    d3d_viewport_t* v = VP(This);
    D3D_METHOD("IDirect3DViewport3::GetViewport2");
    if (!data || data->dwSize != sizeof *data) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (!v->data_set) D3D_RETURN(D3DERR_VIEWPORTDATANOTSET);
    *data = v->data;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE vp_SetViewport2(IDirect3DViewport3* This, LPD3DVIEWPORT2 data) {
    d3d_viewport_t* v = VP(This);
    D3D_METHOD("IDirect3DViewport3::SetViewport2");
    if (!data || data->dwSize != sizeof *data) D3D_RETURN(DDERR_INVALIDPARAMS);
    viewport_changed(v);
    v->data = *data;
    v->data_set = 1;
    if (d3d_method_->calls == 0)
        d3d_log("viewport %lu,%lu %lux%lu, z %.2f..%.2f", data->dwX, data->dwY, data->dwWidth, data->dwHeight,
                data->dvMinZ, data->dvMaxZ);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE vp_SetBackgroundDepth2(IDirect3DViewport3* This, LPDIRECTDRAWSURFACE4 surface) {
    (void)This; (void)surface;
    D3D_UNSUPPORTED("IDirect3DViewport3::SetBackgroundDepth2");
}

static HRESULT STDMETHODCALLTYPE vp_GetBackgroundDepth2(IDirect3DViewport3* This, LPDIRECTDRAWSURFACE4* surface, LPBOOL valid) {
    (void)This; (void)surface; (void)valid;
    D3D_UNSUPPORTED("IDirect3DViewport3::GetBackgroundDepth2");
}

static HRESULT STDMETHODCALLTYPE vp_Clear2(IDirect3DViewport3* This, DWORD count, LPD3DRECT rects, DWORD flags,
                                           D3DCOLOR color, D3DVALUE z, DWORD stencil) {
    d3d_viewport_t* v = VP(This);
    (void)stencil;
    D3D_METHOD("IDirect3DViewport3::Clear2");
    if (!v->device) D3D_RETURN(D3DERR_VIEWPORTHASNODEVICE);
    if ((count && !rects) || !(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
        D3D_RETURN(DDERR_INVALIDPARAMS);
    D3D_RETURN(d3dr_clear(v->device, count, rects, flags, color, z));
}

/* ---- IDirect3DDevice3 ---- */

#define DEV(This) DD_OWNER(d3d_device_t, This)

static HRESULT STDMETHODCALLTYPE dv_QueryInterface(IDirect3DDevice3* This, REFIID riid, LPVOID* out) {
    D3D_METHOD("IDirect3DDevice3::QueryInterface");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDirect3DDevice3)) D3D_RETURN(E_NOINTERFACE);
    InterlockedIncrement(&DEV(This)->refs);
    *out = This;
    D3D_RETURN(DD_OK);
}

static ULONG STDMETHODCALLTYPE dv_AddRef(IDirect3DDevice3* This) { return (ULONG)InterlockedIncrement(&DEV(This)->refs); }

static void drop_viewport(d3d_device_t* dev, d3d_viewport_t* v) {
    d3d_viewport_t** link = &dev->viewports;
    while (*link && *link != v) link = &(*link)->next;
    if (!*link) return;
    *link = v->next;
    v->next = NULL;
    v->device = NULL;
    if (dev->current == v) dev->current = NULL;
    vp_Release((IDirect3DViewport3*)&v->face);
}

static ULONG STDMETHODCALLTYPE dv_Release(IDirect3DDevice3* This) {
    d3d_device_t* dev = DEV(This);
    LONG refs = InterlockedDecrement(&dev->refs);
    if (refs > 0) return (ULONG)refs;
    dd_lock();
    d3dr_detach(dev);
    while (dev->viewports) drop_viewport(dev, dev->viewports);
    if (dev->texture) dd_surface_release(dev->texture);
    for (int k = 1; k < D3D_TEXTURE_STAGES; k++)
        if (dev->upper_textures[k]) dd_surface_release(dev->upper_textures[k]);
    dd_surface_release(dev->target);
    ((IDirectDraw*)&dev->dd->dd1)->lpVtbl->Release((IDirectDraw*)&dev->dd->dd1);
    d3d_log("IDirect3DDevice3 0x%08X released", (uint32_t)(uintptr_t)dev);
    free(dev);
    dd_unlock();
    return 0;
}

static HRESULT STDMETHODCALLTYPE dv_GetCaps(IDirect3DDevice3* This, LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) {
    (void)This;
    D3D_METHOD("IDirect3DDevice3::GetCaps");
    if (!hal || !hel || hal->dwSize != sizeof *hal || hel->dwSize != sizeof *hel) D3D_RETURN(DDERR_INVALIDPARAMS);
    d3d_fill_device_desc(hal);
    memset(hel, 0, sizeof *hel);
    hel->dwSize = sizeof *hel;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_GetStats(IDirect3DDevice3* This, LPD3DSTATS stats) {
    (void)This; (void)stats;
    D3D_UNSUPPORTED("IDirect3DDevice3::GetStats");
}

static d3d_viewport_t* viewport_from(void* iface) {
    return iface && ((dd_face_t*)iface)->vtbl == &s_viewport3_vtbl ? VP(iface) : NULL;
}

static HRESULT STDMETHODCALLTYPE dv_AddViewport(IDirect3DDevice3* This, LPDIRECT3DVIEWPORT3 viewport) {
    d3d_device_t* dev = DEV(This);
    d3d_viewport_t* v = viewport_from(viewport);
    D3D_METHOD("IDirect3DDevice3::AddViewport");
    if (!v) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (v->device) D3D_RETURN(v->device == dev ? DD_OK : DDERR_INVALIDPARAMS);
    vp_AddRef(viewport);
    v->device = dev;
    v->next = dev->viewports;
    dev->viewports = v;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_DeleteViewport(IDirect3DDevice3* This, LPDIRECT3DVIEWPORT3 viewport) {
    d3d_device_t* dev = DEV(This);
    d3d_viewport_t* v = viewport_from(viewport);
    D3D_METHOD("IDirect3DDevice3::DeleteViewport");
    if (!v || v->device != dev) D3D_RETURN(DDERR_INVALIDPARAMS);
    d3dr_state_will_change(dev, D3DR_CHANGE_VIEWPORT, 0, 0);
    drop_viewport(dev, v);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_NextViewport(IDirect3DDevice3* This, LPDIRECT3DVIEWPORT3 viewport, LPDIRECT3DVIEWPORT3* out,
                                                 DWORD flags) {
    d3d_device_t* dev = DEV(This);
    d3d_viewport_t* v = viewport_from(viewport);
    d3d_viewport_t* next = NULL;
    D3D_METHOD("IDirect3DDevice3::NextViewport");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (flags == D3DNEXT_HEAD) next = dev->viewports;
    else if (flags == D3DNEXT_NEXT && v) next = v->next;
    else if (flags == D3DNEXT_TAIL)
        for (next = dev->viewports; next && next->next;) next = next->next;
    else D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = next ? (LPDIRECT3DVIEWPORT3)&next->face : NULL;
    D3D_RETURN(next ? DD_OK : D3DERR_NOVIEWPORTS);
}

/* RGB565, ARGB1555 and ARGB4444: the game's three slots (opaque, one-bit alpha, most alpha). */
static HRESULT STDMETHODCALLTYPE dv_EnumTextureFormats(IDirect3DDevice3* This, LPD3DENUMPIXELFORMATSCALLBACK callback,
                                                       LPVOID context) {
    static const DWORD masks[3][4] = {
        { 0xF800, 0x07E0, 0x001F, 0 }, { 0x7C00, 0x03E0, 0x001F, 0x8000 }, { 0x0F00, 0x00F0, 0x000F, 0xF000 },
    };
    (void)This;
    D3D_METHOD("IDirect3DDevice3::EnumTextureFormats");
    if (!callback) D3D_RETURN(DDERR_INVALIDPARAMS);
    for (int i = 0; i < 3; i++) {
        DDPIXELFORMAT pf;
        memset(&pf, 0, sizeof pf);
        pf.dwSize = sizeof pf;
        pf.dwFlags = DDPF_RGB | (masks[i][3] ? DDPF_ALPHAPIXELS : 0);
        pf.dwRGBBitCount = 16;
        pf.dwRBitMask = masks[i][0];
        pf.dwGBitMask = masks[i][1];
        pf.dwBBitMask = masks[i][2];
        pf.dwRGBAlphaBitMask = masks[i][3];
        if (callback(&pf, context) == D3DENUMRET_CANCEL) break;
    }
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_BeginScene(IDirect3DDevice3* This) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::BeginScene");
    if (dev->in_scene) D3D_RETURN(D3DERR_SCENE_IN_SCENE);
    dev->in_scene = 1;
    if (s_fog_sync) s_fog_sync(dev);
    d3dr_begin_scene(dev);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_EndScene(IDirect3DDevice3* This) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::EndScene");
    if (!dev->in_scene) D3D_RETURN(D3DERR_SCENE_NOT_IN_SCENE);
    dev->in_scene = 0;
    d3dr_end_scene(dev);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_GetDirect3D(IDirect3DDevice3* This, LPDIRECT3D3* out) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::GetDirect3D");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    InterlockedIncrement(&dev->dd->refs);
    *out = (LPDIRECT3D3)&dev->dd->d3d3;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_SetCurrentViewport(IDirect3DDevice3* This, LPDIRECT3DVIEWPORT3 viewport) {
    d3d_device_t* dev = DEV(This);
    d3d_viewport_t* v = viewport_from(viewport);
    D3D_METHOD("IDirect3DDevice3::SetCurrentViewport");
    if (!v || v->device != dev) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (dev->current != v) d3dr_state_will_change(dev, D3DR_CHANGE_VIEWPORT, 0, 0);
    dev->current = v;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_GetCurrentViewport(IDirect3DDevice3* This, LPDIRECT3DVIEWPORT3* out) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::GetCurrentViewport");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!dev->current) D3D_RETURN(D3DERR_NOCURRENTVIEWPORT);
    vp_AddRef((IDirect3DViewport3*)&dev->current->face);
    *out = (LPDIRECT3DVIEWPORT3)&dev->current->face;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_SetRenderTarget(IDirect3DDevice3* This, LPDIRECTDRAWSURFACE4 surface, DWORD flags) {
    (void)This; (void)surface; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::SetRenderTarget");
}

static HRESULT STDMETHODCALLTYPE dv_GetRenderTarget(IDirect3DDevice3* This, LPDIRECTDRAWSURFACE4* out) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::GetRenderTarget");
    if (!out) D3D_RETURN(DDERR_INVALIDPARAMS);
    dd_surface_addref(dev->target);
    *out = (LPDIRECTDRAWSURFACE4)dd_surface_face(dev->target, 4);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_Begin(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf, DWORD flags) {
    (void)This; (void)type; (void)fvf; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::Begin");
}

static HRESULT STDMETHODCALLTYPE dv_BeginIndexed(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices,
                                                 DWORD count, DWORD flags) {
    (void)This; (void)type; (void)fvf; (void)vertices; (void)count; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::BeginIndexed");
}

static HRESULT STDMETHODCALLTYPE dv_Vertex(IDirect3DDevice3* This, LPVOID vertex) {
    (void)This; (void)vertex;
    D3D_UNSUPPORTED("IDirect3DDevice3::Vertex");
}

static HRESULT STDMETHODCALLTYPE dv_Index(IDirect3DDevice3* This, WORD index) {
    (void)This; (void)index;
    D3D_UNSUPPORTED("IDirect3DDevice3::Index");
}

static HRESULT STDMETHODCALLTYPE dv_End(IDirect3DDevice3* This, DWORD flags) {
    (void)This; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::End");
}

static HRESULT STDMETHODCALLTYPE dv_GetRenderState(IDirect3DDevice3* This, D3DRENDERSTATETYPE state, LPDWORD value) {
    D3D_METHOD("IDirect3DDevice3::GetRenderState");
    if (!value || (DWORD)state >= D3D_RENDER_STATES) D3D_RETURN(DDERR_INVALIDPARAMS);
    *value = DEV(This)->render[state];
    D3D_RETURN(DD_OK);
}

/* The render states the translation honours; the rest are refused with one log line each. */
static int render_state_known(DWORD state) {
    switch (state) {
    case D3DRENDERSTATE_TEXTUREHANDLE:
    case D3DRENDERSTATE_TEXTUREADDRESS:
    case D3DRENDERSTATE_TEXTUREPERSPECTIVE:
    case D3DRENDERSTATE_ZENABLE:
    case D3DRENDERSTATE_FILLMODE:
    case D3DRENDERSTATE_SHADEMODE:
    case D3DRENDERSTATE_ZWRITEENABLE:
    case D3DRENDERSTATE_ALPHATESTENABLE:
    case D3DRENDERSTATE_LASTPIXEL:
    case D3DRENDERSTATE_TEXTUREMAG:
    case D3DRENDERSTATE_TEXTUREMIN:
    case D3DRENDERSTATE_SRCBLEND:
    case D3DRENDERSTATE_DESTBLEND:
    case D3DRENDERSTATE_TEXTUREMAPBLEND:
    case D3DRENDERSTATE_CULLMODE:
    case D3DRENDERSTATE_ZFUNC:
    case D3DRENDERSTATE_ALPHAREF:
    case D3DRENDERSTATE_ALPHAFUNC:
    case D3DRENDERSTATE_DITHERENABLE:
    case D3DRENDERSTATE_ALPHABLENDENABLE:
    case D3DRENDERSTATE_FOGENABLE:
    case D3DRENDERSTATE_SPECULARENABLE:
    case D3DRENDERSTATE_SUBPIXEL:
    case D3DRENDERSTATE_SUBPIXELX:
    case D3DRENDERSTATE_FOGCOLOR:
    case D3DRENDERSTATE_FOGTABLEMODE:
    case D3DRENDERSTATE_COLORKEYENABLE:
    case D3DRENDERSTATE_TEXTUREADDRESSU:
    case D3DRENDERSTATE_TEXTUREADDRESSV:
    case D3DRENDERSTATE_RANGEFOGENABLE:
    case D3DRENDERSTATE_STIPPLEDALPHA:
    case D3DRENDERSTATE_ZBIAS:
        return 1;
    default:
        return 0;
    }
}

static int render_value_valid(DWORD state, DWORD value) {
    switch (state) {
    case D3DRENDERSTATE_SRCBLEND:
    case D3DRENDERSTATE_DESTBLEND: return value >= 1 && value <= 13;
    case D3DRENDERSTATE_ZFUNC:
    case D3DRENDERSTATE_ALPHAFUNC: return d3dt_compare(value) != 0;
    case D3DRENDERSTATE_CULLMODE: return d3dt_cull(value) != 0;
    case D3DRENDERSTATE_FILLMODE: return d3dt_fill(value) != 0;
    case D3DRENDERSTATE_TEXTUREMAPBLEND: return value >= 1 && value <= 8;
    case D3DRENDERSTATE_ZENABLE: return value <= 2;
    case D3DRENDERSTATE_FOGTABLEMODE: return value == D3DFOG_NONE;
    case D3DRENDERSTATE_TEXTUREHANDLE: return value == 0;
    case D3DRENDERSTATE_ZBIAS: return value <= 16;
    default: return 1;
    }
}

static HRESULT STDMETHODCALLTYPE dv_SetRenderState(IDirect3DDevice3* This, D3DRENDERSTATETYPE state, DWORD value) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::SetRenderState");
    if ((DWORD)state >= D3D_RENDER_STATES || !render_state_known(state) || !render_value_valid(state, value)) {
        refuse_render_state(state, value);
        D3D_RETURN(DDERR_INVALIDPARAMS);
    }
    if (dev->render[state] != value) {
        if (d3dt_render_change_flushes((uint32_t)state, dev->render[state], value))
            d3dr_state_will_change(dev, D3DR_CHANGE_RENDER, (DWORD)state, value);
        else
            d3dr_state_kept_batch(dev);
        dev->render[state] = value;
        switch (state) {
        case D3DRENDERSTATE_TEXTUREADDRESS:
            dev->stage[0][D3DTSS_ADDRESSU] = dev->stage[0][D3DTSS_ADDRESSV] = value;
            break;
        case D3DRENDERSTATE_TEXTUREADDRESSU: dev->stage[0][D3DTSS_ADDRESSU] = value; break;
        case D3DRENDERSTATE_TEXTUREADDRESSV: dev->stage[0][D3DTSS_ADDRESSV] = value; break;
        case D3DRENDERSTATE_TEXTUREMAG:
            dev->stage[0][D3DTSS_MAGFILTER] = value == D3DFILTER_NEAREST ? D3DTFG_POINT : D3DTFG_LINEAR;
            break;
        case D3DRENDERSTATE_TEXTUREMIN:
            dev->stage[0][D3DTSS_MINFILTER] =
                (value == D3DFILTER_NEAREST || value == D3DFILTER_MIPNEAREST) ? D3DTFN_POINT : D3DTFN_LINEAR;
            break;
        case D3DRENDERSTATE_TEXTUREMAPBLEND: dev->texture_blend_set = 1; break;
        default: break;
        }
    }
    D3D_RETURN(DD_OK);
}

int d3dr_set_fog_color(d3d_device_t* dev, DWORD color) {
    DWORD old;
    if (!dev) return 0;
    color = (color & 0xFFFFFFu) | 0xFF000000u;
    old = dev->render[D3DRENDERSTATE_FOGCOLOR];
    if (old == color) return 0;
    dv_SetRenderState((IDirect3DDevice3*)&dev->face, D3DRENDERSTATE_FOGCOLOR, color);
    return 1;
}

static HRESULT STDMETHODCALLTYPE dv_GetLightState(IDirect3DDevice3* This, D3DLIGHTSTATETYPE state, LPDWORD value) {
    (void)This; (void)state; (void)value;
    D3D_UNSUPPORTED("IDirect3DDevice3::GetLightState");
}

static HRESULT STDMETHODCALLTYPE dv_SetLightState(IDirect3DDevice3* This, D3DLIGHTSTATETYPE state, DWORD value) {
    (void)This; (void)state; (void)value;
    D3D_UNSUPPORTED("IDirect3DDevice3::SetLightState");
}

static D3DMATRIX* transform_slot(d3d_device_t* dev, D3DTRANSFORMSTATETYPE type) {
    if (type == D3DTRANSFORMSTATE_WORLD) return &dev->transforms[0];
    if (type == D3DTRANSFORMSTATE_VIEW) return &dev->transforms[1];
    if (type == D3DTRANSFORMSTATE_PROJECTION) return &dev->transforms[2];
    return NULL;
}

/* Stored faithfully for Get/Multiply; drawing untransformed vertices is refused (the game never does). */
static HRESULT STDMETHODCALLTYPE dv_SetTransform(IDirect3DDevice3* This, D3DTRANSFORMSTATETYPE type, LPD3DMATRIX matrix) {
    D3DMATRIX* slot = transform_slot(DEV(This), type);
    D3D_METHOD("IDirect3DDevice3::SetTransform");
    if (!slot || !matrix) D3D_RETURN(DDERR_INVALIDPARAMS);
    *slot = *matrix;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_GetTransform(IDirect3DDevice3* This, D3DTRANSFORMSTATETYPE type, LPD3DMATRIX matrix) {
    D3DMATRIX* slot = transform_slot(DEV(This), type);
    D3D_METHOD("IDirect3DDevice3::GetTransform");
    if (!slot || !matrix) D3D_RETURN(DDERR_INVALIDPARAMS);
    *matrix = *slot;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_MultiplyTransform(IDirect3DDevice3* This, D3DTRANSFORMSTATETYPE type, LPD3DMATRIX matrix) {
    D3DMATRIX* slot = transform_slot(DEV(This), type);
    D3D_METHOD("IDirect3DDevice3::MultiplyTransform");
    if (!slot || !matrix) D3D_RETURN(DDERR_INVALIDPARAMS);
    d3dt_matrix_multiply(&matrix->_11, &slot->_11, &slot->_11);
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_DrawPrimitive(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf, LPVOID vertices,
                                                  DWORD count, DWORD flags) {
    (void)flags;
    D3D_METHOD("IDirect3DDevice3::DrawPrimitive");
    if (!vertices) D3D_RETURN(DDERR_INVALIDPARAMS);
    D3D_RETURN(d3dr_draw(DEV(This), (int)type, fvf, vertices, count, NULL, 0));
}

static HRESULT STDMETHODCALLTYPE dv_DrawIndexedPrimitive(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf,
                                                         LPVOID vertices, DWORD count, LPWORD indices, DWORD index_count,
                                                         DWORD flags) {
    (void)flags;
    D3D_METHOD("IDirect3DDevice3::DrawIndexedPrimitive");
    if (!vertices || !indices) D3D_RETURN(DDERR_INVALIDPARAMS);
    D3D_RETURN(d3dr_draw(DEV(This), (int)type, fvf, vertices, count, indices, index_count));
}

static HRESULT STDMETHODCALLTYPE dv_SetClipStatus(IDirect3DDevice3* This, LPD3DCLIPSTATUS status) {
    (void)This; (void)status;
    D3D_UNSUPPORTED("IDirect3DDevice3::SetClipStatus");
}

static HRESULT STDMETHODCALLTYPE dv_GetClipStatus(IDirect3DDevice3* This, LPD3DCLIPSTATUS status) {
    (void)This; (void)status;
    D3D_UNSUPPORTED("IDirect3DDevice3::GetClipStatus");
}

static HRESULT STDMETHODCALLTYPE dv_DrawPrimitiveStrided(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf,
                                                         LPD3DDRAWPRIMITIVESTRIDEDDATA data, DWORD count, DWORD flags) {
    (void)This; (void)type; (void)fvf; (void)data; (void)count; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::DrawPrimitiveStrided");
}

static HRESULT STDMETHODCALLTYPE dv_DrawIndexedPrimitiveStrided(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, DWORD fvf,
                                                                LPD3DDRAWPRIMITIVESTRIDEDDATA data, DWORD count,
                                                                LPWORD indices, DWORD index_count, DWORD flags) {
    (void)This; (void)type; (void)fvf; (void)data; (void)count; (void)indices; (void)index_count; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::DrawIndexedPrimitiveStrided");
}

static HRESULT STDMETHODCALLTYPE dv_DrawPrimitiveVB(IDirect3DDevice3* This, D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER vb,
                                                    DWORD start, DWORD count, DWORD flags) {
    (void)This; (void)type; (void)vb; (void)start; (void)count; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::DrawPrimitiveVB");
}

static HRESULT STDMETHODCALLTYPE dv_DrawIndexedPrimitiveVB(IDirect3DDevice3* This, D3DPRIMITIVETYPE type,
                                                           LPDIRECT3DVERTEXBUFFER vb, LPWORD indices, DWORD count, DWORD flags) {
    (void)This; (void)type; (void)vb; (void)indices; (void)count; (void)flags;
    D3D_UNSUPPORTED("IDirect3DDevice3::DrawIndexedPrimitiveVB");
}

static HRESULT STDMETHODCALLTYPE dv_ComputeSphereVisibility(IDirect3DDevice3* This, LPD3DVECTOR centers, LPD3DVALUE radii,
                                                            DWORD count, DWORD flags, LPDWORD results) {
    (void)This; (void)centers; (void)radii; (void)count; (void)flags; (void)results;
    D3D_UNSUPPORTED("IDirect3DDevice3::ComputeSphereVisibility");
}

static dd_surface_t** texture_slot(d3d_device_t* dev, DWORD stage) {
    return stage ? &dev->upper_textures[stage] : &dev->texture;
}

static HRESULT STDMETHODCALLTYPE dv_GetTexture(IDirect3DDevice3* This, DWORD stage, LPDIRECT3DTEXTURE2* out) {
    d3d_device_t* dev = DEV(This);
    dd_surface_t* s;
    D3D_METHOD("IDirect3DDevice3::GetTexture");
    if (!out || stage >= D3D_TEXTURE_STAGES) D3D_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    s = *texture_slot(dev, stage);
    if (s) {
        dd_surface_addref(s);
        *out = (LPDIRECT3DTEXTURE2)&s->tex2;
    }
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_SetTexture(IDirect3DDevice3* This, DWORD stage, LPDIRECT3DTEXTURE2 texture) {
    d3d_device_t* dev = DEV(This);
    dd_surface_t* s = texture ? d3d_surface_from_texture(texture) : NULL;
    dd_surface_t** slot;
    D3D_METHOD("IDirect3DDevice3::SetTexture");
    if ((texture && !s) || stage >= D3D_TEXTURE_STAGES) D3D_RETURN(DDERR_INVALIDPARAMS);
    slot = texture_slot(dev, stage);
    if (s == *slot) D3D_RETURN(DD_OK);
    if (s) dd_surface_addref(s);
    if (*slot) dd_surface_release(*slot);
    *slot = s;
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_GetTextureStageState(IDirect3DDevice3* This, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
                                                         LPDWORD value) {
    D3D_METHOD("IDirect3DDevice3::GetTextureStageState");
    if (!value || stage >= D3D_TEXTURE_STAGES || (DWORD)type >= D3D_STAGE_STATES) D3D_RETURN(DDERR_INVALIDPARAMS);
    *value = DEV(This)->stage[stage][type];
    D3D_RETURN(DD_OK);
}

/* Sampling states apply to every stage. Stages above 0 keep COLOROP/ALPHAOP DISABLE, so the
 * translation draws stage 0 alone, which is exactly what those states mean. */
static int stage_value_valid(DWORD stage, DWORD type, DWORD value) {
    switch (type) {
    case D3DTSS_ADDRESS:
    case D3DTSS_ADDRESSU:
    case D3DTSS_ADDRESSV: return d3dt_address(value) != 0 && value != D3DTADDRESS_BORDER;
    case D3DTSS_MAGFILTER: return d3dt_mag_linear(value) >= 0;
    case D3DTSS_MINFILTER: return d3dt_min_linear(value) >= 0;
    case D3DTSS_MIPFILTER: return d3dt_mip_mode(value) >= 0;
    case D3DTSS_COLOROP:
    case D3DTSS_ALPHAOP: return stage > 0 && value == D3DTOP_DISABLE;
    case D3DTSS_TEXCOORDINDEX: return value == stage;
    default: return 0;
    }
}

/* Wrap and clamp travel in each world vertex (d3dt_vertex_sampler), so changing between them keeps the batch. */
static int address_joins(const d3d_device_t* dev, DWORD type, DWORD value) {
    switch (type) {
    case D3DTSS_ADDRESS:
        return d3dt_address_joins(dev->stage[0][D3DTSS_ADDRESSU], value) &&
               d3dt_address_joins(dev->stage[0][D3DTSS_ADDRESSV], value);
    case D3DTSS_ADDRESSU:
    case D3DTSS_ADDRESSV: return d3dt_address_joins(dev->stage[0][type], value);
    default: return 0;
    }
}

static HRESULT STDMETHODCALLTYPE dv_SetTextureStageState(IDirect3DDevice3* This, DWORD stage, D3DTEXTURESTAGESTATETYPE type,
                                                         DWORD value) {
    d3d_device_t* dev = DEV(This);
    D3D_METHOD("IDirect3DDevice3::SetTextureStageState");
    if (stage >= D3D_TEXTURE_STAGES || (DWORD)type >= D3D_STAGE_STATES || !stage_value_valid(stage, type, value)) {
        refuse_stage_state(stage, type, value);
        D3D_RETURN(DDERR_INVALIDPARAMS);
    }
    if (dev->stage[stage][type] != value) {
        if (!stage && !address_joins(dev, type, value)) d3dr_state_will_change(dev, D3DR_CHANGE_STAGE, (DWORD)type, 0);
        dev->stage[stage][type] = value;
        if (type == D3DTSS_ADDRESS) dev->stage[stage][D3DTSS_ADDRESSU] = dev->stage[stage][D3DTSS_ADDRESSV] = value;
    }
    D3D_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dv_ValidateDevice(IDirect3DDevice3* This, LPDWORD passes) {
    (void)This;
    D3D_METHOD("IDirect3DDevice3::ValidateDevice");
    if (!passes) D3D_RETURN(DDERR_INVALIDPARAMS);
    *passes = 1;
    D3D_RETURN(DD_OK);
}

/* ---- IDirect3DTexture2 (a face of the texture surface) ---- */

void d3d_surface_init_texture_face(dd_surface_t* s) {
    s->tex2.vtbl = &s_texture2_vtbl;
    s->tex2.owner = s;
    s->tex2.version = 2;
}

dd_surface_t* d3d_surface_from_texture(void* iface) {
    return iface && ((dd_face_t*)iface)->vtbl == &s_texture2_vtbl ? DD_OWNER(dd_surface_t, iface) : NULL;
}

void d3dr_texture_override(uint32_t iface, int id) {
    dd_surface_t* s = d3d_surface_from_texture((void*)(uintptr_t)iface);
    if (s) s->override_id = id;
}

int d3d_is_texture_iid(REFIID riid) { return IsEqualGUID(riid, &IID_IDirect3DTexture2); }

static HRESULT STDMETHODCALLTYPE tx_QueryInterface(IDirect3DTexture2* This, REFIID riid, LPVOID* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    IDirectDrawSurface* surface = (IDirectDrawSurface*)&s->s1;
    return surface->lpVtbl->QueryInterface(surface, riid, out);
}

static ULONG STDMETHODCALLTYPE tx_AddRef(IDirect3DTexture2* This) {
    return (ULONG)InterlockedIncrement(&DD_OWNER(dd_surface_t, This)->refs);
}

static ULONG STDMETHODCALLTYPE tx_Release(IDirect3DTexture2* This) {
    IDirectDrawSurface* surface = (IDirectDrawSurface*)&DD_OWNER(dd_surface_t, This)->s1;
    return surface->lpVtbl->Release(surface);
}

static HRESULT STDMETHODCALLTYPE tx_GetHandle(IDirect3DTexture2* This, LPDIRECT3DDEVICE2 device, LPD3DTEXTUREHANDLE handle) {
    (void)This; (void)device; (void)handle;
    D3D_UNSUPPORTED("IDirect3DTexture2::GetHandle");
}

static HRESULT STDMETHODCALLTYPE tx_PaletteChanged(IDirect3DTexture2* This, DWORD start, DWORD count) {
    (void)This; (void)start; (void)count;
    D3D_UNSUPPORTED("IDirect3DTexture2::PaletteChanged");
}

/* Once per kind of Load: what the game copies into its Direct3D textures and from where. */
static void note_texture_load(const dd_surface_t* src, const dd_surface_t* dst) {
    static struct {
        DWORD bits, rmask, amask, src_caps, dst_caps;
    } seen[8];
    static int count;
    DWORD amask = (dst->format.dwFlags & DDPF_ALPHAPIXELS) ? dst->format.dwRGBAlphaBitMask : 0;
    for (int i = 0; i < count; i++)
        if (seen[i].bits == dst->format.dwRGBBitCount && seen[i].rmask == dst->format.dwRBitMask &&
            seen[i].amask == amask && seen[i].src_caps == src->caps && seen[i].dst_caps == dst->caps)
            return;
    if (count == 8) return;
    seen[count].bits = dst->format.dwRGBBitCount;
    seen[count].rmask = dst->format.dwRBitMask;
    seen[count].amask = amask;
    seen[count].src_caps = src->caps;
    seen[count].dst_caps = dst->caps;
    count++;
    d3d_log("texture Load %lux%lu, %lu bpp R %lX A %lX, pitch %ld -> %ld: source caps 0x%08lX (%lu CPU writes, key %s) -> "
            "texture caps 0x%08lX, from sub_%08X", dst->width, dst->height, dst->format.dwRGBBitCount,
            dst->format.dwRBitMask, amask, src->pitch, dst->pitch, src->caps, src->cpu_writes,
            src->has_src_key ? "set" : "none", dst->caps, g_cur_func);
}

/* Texture Loads and how much of them changed (--perf-log). */
static unsigned long long s_loads, s_loads_unchanged, s_load_texels, s_load_changed_texels;

void d3d_objects_load_counts(unsigned long long* loads, unsigned long long* unchanged, unsigned long long* texels,
                             unsigned long long* changed_texels) {
    *loads = s_loads, *unchanged = s_loads_unchanged, *texels = s_load_texels, *changed_texels = s_load_changed_texels;
    s_loads = s_loads_unchanged = s_load_texels = s_load_changed_texels = 0;
}

/* Load copies the pixels (and the source colour key) at once, as the game's own test expects. */
static HRESULT STDMETHODCALLTYPE tx_Load(IDirect3DTexture2* This, LPDIRECT3DTEXTURE2 source) {
    dd_surface_t* dst = DD_OWNER(dd_surface_t, This);
    dd_surface_t* src = d3d_surface_from_texture(source);
    d3dt_rect_t changed;
    int key_changed;
    D3D_METHOD("IDirect3DTexture2::Load");
    if (!src) D3D_RETURN(DDERR_INVALIDPARAMS);
    if (src == dst) D3D_RETURN(DD_OK);
    if (src->width != dst->width || src->height != dst->height || !dd_format_equal(&src->format, &dst->format) ||
        (src->format.dwFlags & DDPF_ALPHAPIXELS) != (dst->format.dwFlags & DDPF_ALPHAPIXELS) ||
        src->format.dwRGBAlphaBitMask != dst->format.dwRGBAlphaBitMask)
        D3D_RETURN(DDERR_INVALIDPARAMS);
    if (dst->locked || dst->dc_out || src->locked || src->dc_out) D3D_RETURN(DDERR_SURFACEBUSY);
    note_texture_load(src, dst);
    d3dr_texture_loaded(dst, src);
    d3dr_surface_cpu_access(src, 0, CPU_ACCESS_BLT_SOURCE);
    d3dr_surface_cpu_access(dst, 1, CPU_ACCESS_BLT_DEST);
    key_changed = src->has_src_key && (!dst->has_src_key || memcmp(&dst->src_key, &src->src_key, sizeof src->src_key));
    if (src->has_src_key) {
        dst->has_src_key = 1;
        dst->src_key = src->src_key;
    }
    s_loads++;
    s_load_texels += (unsigned long long)dst->width * dst->height;
    /* Only what differs from the texture's own copy is copied and uploaded: the image cache Loads whole pages. */
    if (d3dt_copy_changed(dst->bits, dst->pitch, src->bits, src->pitch, dst->width, dst->height,
                          (unsigned)dd_format_bytes(&dst->format), &changed)) {
        RECT r = { changed.left, changed.top, changed.right, changed.bottom };
        s_load_changed_texels += (unsigned long long)(r.right - r.left) * (r.bottom - r.top);
        d3dr_surface_cpu_written(dst, key_changed ? NULL : &r);
    } else if (key_changed) {
        d3dr_surface_cpu_written(dst, NULL);
    } else {
        s_loads_unchanged++;
    }
    D3D_RETURN(DD_OK);
}

/* ---- vtables ---- */

static const IDirect3D3Vtbl s_d3d3_vtbl = {
    .QueryInterface = d3_QueryInterface, .AddRef = d3_AddRef, .Release = d3_Release, .EnumDevices = d3_EnumDevices,
    .CreateLight = d3_CreateLight, .CreateMaterial = d3_CreateMaterial, .CreateViewport = d3_CreateViewport,
    .FindDevice = d3_FindDevice, .CreateDevice = d3_CreateDevice, .CreateVertexBuffer = d3_CreateVertexBuffer,
    .EnumZBufferFormats = d3_EnumZBufferFormats, .EvictManagedTextures = d3_EvictManagedTextures,
};

static const IDirect3DViewport3Vtbl s_viewport3_vtbl = {
    .QueryInterface = vp_QueryInterface, .AddRef = vp_AddRef, .Release = vp_Release, .Initialize = vp_Initialize,
    .GetViewport = vp_GetViewport, .SetViewport = vp_SetViewport, .TransformVertices = vp_TransformVertices,
    .LightElements = vp_LightElements, .SetBackground = vp_SetBackground, .GetBackground = vp_GetBackground,
    .SetBackgroundDepth = vp_SetBackgroundDepth, .GetBackgroundDepth = vp_GetBackgroundDepth, .Clear = vp_Clear,
    .AddLight = vp_AddLight, .DeleteLight = vp_DeleteLight, .NextLight = vp_NextLight, .GetViewport2 = vp_GetViewport2,
    .SetViewport2 = vp_SetViewport2, .SetBackgroundDepth2 = vp_SetBackgroundDepth2,
    .GetBackgroundDepth2 = vp_GetBackgroundDepth2, .Clear2 = vp_Clear2,
};

static const IDirect3DDevice3Vtbl s_device3_vtbl = {
    .QueryInterface = dv_QueryInterface, .AddRef = dv_AddRef, .Release = dv_Release, .GetCaps = dv_GetCaps,
    .GetStats = dv_GetStats, .AddViewport = dv_AddViewport, .DeleteViewport = dv_DeleteViewport,
    .NextViewport = dv_NextViewport, .EnumTextureFormats = dv_EnumTextureFormats, .BeginScene = dv_BeginScene,
    .EndScene = dv_EndScene, .GetDirect3D = dv_GetDirect3D, .SetCurrentViewport = dv_SetCurrentViewport,
    .GetCurrentViewport = dv_GetCurrentViewport, .SetRenderTarget = dv_SetRenderTarget,
    .GetRenderTarget = dv_GetRenderTarget, .Begin = dv_Begin, .BeginIndexed = dv_BeginIndexed, .Vertex = dv_Vertex,
    .Index = dv_Index, .End = dv_End, .GetRenderState = dv_GetRenderState, .SetRenderState = dv_SetRenderState,
    .GetLightState = dv_GetLightState, .SetLightState = dv_SetLightState, .SetTransform = dv_SetTransform,
    .GetTransform = dv_GetTransform, .MultiplyTransform = dv_MultiplyTransform, .DrawPrimitive = dv_DrawPrimitive,
    .DrawIndexedPrimitive = dv_DrawIndexedPrimitive, .SetClipStatus = dv_SetClipStatus,
    .GetClipStatus = dv_GetClipStatus, .DrawPrimitiveStrided = dv_DrawPrimitiveStrided,
    .DrawIndexedPrimitiveStrided = dv_DrawIndexedPrimitiveStrided, .DrawPrimitiveVB = dv_DrawPrimitiveVB,
    .DrawIndexedPrimitiveVB = dv_DrawIndexedPrimitiveVB, .ComputeSphereVisibility = dv_ComputeSphereVisibility,
    .GetTexture = dv_GetTexture, .SetTexture = dv_SetTexture, .GetTextureStageState = dv_GetTextureStageState,
    .SetTextureStageState = dv_SetTextureStageState, .ValidateDevice = dv_ValidateDevice,
};

static const IDirect3DTexture2Vtbl s_texture2_vtbl = {
    .QueryInterface = tx_QueryInterface, .AddRef = tx_AddRef, .Release = tx_Release, .GetHandle = tx_GetHandle,
    .PaletteChanged = tx_PaletteChanged, .Load = tx_Load,
};

/* Slot counts and offsets the game's calls were recovered against (docs/D3D_LEGACY_INVENTORY.md). */
C_ASSERT(sizeof(IDirect3D3Vtbl) == 12 * sizeof(void*));
C_ASSERT(sizeof(IDirect3DDevice3Vtbl) == 42 * sizeof(void*));
C_ASSERT(sizeof(IDirect3DViewport3Vtbl) == 21 * sizeof(void*));
C_ASSERT(sizeof(IDirect3DTexture2Vtbl) == 6 * sizeof(void*));
C_ASSERT(offsetof(IDirect3D3Vtbl, CreateDevice) == 0x20 && offsetof(IDirect3D3Vtbl, EnumZBufferFormats) == 0x28);
C_ASSERT(offsetof(IDirect3D3Vtbl, CreateViewport) == 0x18 && offsetof(IDirect3D3Vtbl, EvictManagedTextures) == 0x2C);
C_ASSERT(offsetof(IDirect3DDevice3Vtbl, BeginScene) == 0x24 && offsetof(IDirect3DDevice3Vtbl, EndScene) == 0x28);
C_ASSERT(offsetof(IDirect3DDevice3Vtbl, SetRenderState) == 0x58 && offsetof(IDirect3DDevice3Vtbl, DrawPrimitive) == 0x70);
C_ASSERT(offsetof(IDirect3DDevice3Vtbl, SetTexture) == 0x98 && offsetof(IDirect3DDevice3Vtbl, SetTextureStageState) == 0xA0);
C_ASSERT(offsetof(IDirect3DDevice3Vtbl, GetCaps) == 0x0C && offsetof(IDirect3DDevice3Vtbl, AddViewport) == 0x14);
C_ASSERT(offsetof(IDirect3DDevice3Vtbl, EnumTextureFormats) == 0x20 && offsetof(IDirect3DDevice3Vtbl, SetCurrentViewport) == 0x30);
C_ASSERT(offsetof(IDirect3DViewport3Vtbl, SetViewport2) == 0x44 && offsetof(IDirect3DViewport3Vtbl, Clear2) == 0x50);
C_ASSERT(offsetof(IDirect3DTexture2Vtbl, Load) == 0x14);
C_ASSERT(sizeof(D3DDEVICEDESC) == 0xFC && offsetof(D3DDEVICEDESC, dpcTriCaps) == 0x64);
C_ASSERT(offsetof(D3DDEVICEDESC, dwDeviceRenderBitDepth) == 0x9C && offsetof(D3DDEVICEDESC, dwTextureOpCaps) == 0xF4);
C_ASSERT(sizeof(D3DTLVERTEX) == 32 && sizeof(D3DVIEWPORT2) == 44 && sizeof(D3DRECT) == 16);

/* Called per polygon or per batch, and only host logic: no window, no wait on another thread, no call back into the
 * game. */
int d3d_method_in_place(uint32_t va) {
    static const void* const k_methods[] = {
        (const void*)dv_SetTexture, (const void*)dv_GetTexture, (const void*)dv_DrawPrimitive,
        (const void*)dv_DrawIndexedPrimitive, (const void*)dv_SetRenderState, (const void*)dv_GetRenderState,
        (const void*)dv_SetTextureStageState, (const void*)dv_GetTextureStageState, (const void*)dv_BeginScene,
        (const void*)dv_EndScene, (const void*)dv_SetCurrentViewport, (const void*)dv_GetCurrentViewport,
        (const void*)dv_SetTransform, (const void*)dv_GetTransform, (const void*)vp_Clear, (const void*)vp_Clear2,
        (const void*)tx_GetHandle, (const void*)tx_Load,
    };
    for (size_t i = 0; i < sizeof k_methods / sizeof k_methods[0]; i++)
        if ((uint32_t)(uintptr_t)k_methods[i] == va) return 1;
    return 0;
}

int d3d_vtables_complete(void) {
    static const struct { const void* table; size_t size; } k_tables[] = {
        { &s_d3d3_vtbl, sizeof s_d3d3_vtbl },
        { &s_device3_vtbl, sizeof s_device3_vtbl },
        { &s_viewport3_vtbl, sizeof s_viewport3_vtbl },
        { &s_texture2_vtbl, sizeof s_texture2_vtbl },
    };
    for (int t = 0; t < (int)(sizeof k_tables / sizeof k_tables[0]); t++) {
        void* const* slots = (void* const*)k_tables[t].table;
        for (size_t i = 0; i < k_tables[t].size / sizeof(void*); i++)
            if (!slots[i]) return 0;
    }
    return 1;
}
