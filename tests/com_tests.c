/*
 * Headless checks of the DirectDraw/Direct3D 6 COM objects through their vtables, as the game calls
 * them. The window host, the presenter and the D3D11 renderer are replaced by the stubs below,
 * so no window, device or game is involved. Build: CMake target com_tests (docs/BUILD.md, "Headless tests").
 */
#include "ddraw/d3d_legacy.h"
#include "host.h"

#include <stdio.h>

uint32_t g_cur_func;
static int s_failures, s_checks, s_draws, s_clears, s_attached;

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void host_log(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; }
const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap) {
    _snprintf(buf, cap - 1, "0x%08X", hr);
    buf[cap - 1] = 0;
    return buf;
}
void window_host_set_display(HWND window, int width, int height) { (void)window; (void)width; (void)height; }
void window_host_clear_display(HWND window) { (void)window; }
int present_adapter(dd_adapter_t* out) {
    memset(out, 0, sizeof *out);
    strcpy(out->description, "test adapter");
    return 1;
}
void present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                   const DDGAMMARAMP* gamma, int gamma_changed, int vsync) {
    (void)window; (void)bits; (void)pitch; (void)source; (void)format; (void)gamma; (void)gamma_changed; (void)vsync;
}
int d3dr_hardware(void) { return 1; }
HRESULT d3dr_attach(d3d_device_t* dev) {
    s_attached++;
    dev->backend = NULL;
    return DD_OK;
}
void d3dr_detach(d3d_device_t* dev) { (void)dev; s_attached--; }
void d3dr_begin_scene(d3d_device_t* dev) { (void)dev; }
void d3dr_end_scene(d3d_device_t* dev) { (void)dev; }
void d3dr_state_will_change(d3d_device_t* dev) { (void)dev; }
HRESULT d3dr_draw(d3d_device_t* dev, int type, DWORD fvf, const void* vertices, DWORD count, const WORD* indices,
                  DWORD index_count) {
    (void)dev; (void)vertices; (void)indices; (void)index_count;
    if (type != D3DPT_TRIANGLEFAN || fvf != D3DFVF_TLVERTEX || count < 3) return DDERR_INVALIDPARAMS;
    s_draws++;
    return DD_OK;
}
HRESULT d3dr_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    (void)dev; (void)count; (void)rects; (void)flags; (void)color; (void)z;
    s_clears++;
    return DD_OK;
}
void d3dr_surface_cpu_access(dd_surface_t* s, int will_write, cpu_access_t access) {
    (void)s; (void)will_write; (void)access;
}
void d3dr_surface_cpu_written(dd_surface_t* s, const RECT* r) { (void)s; (void)r; }
int d3dr_surface_fill(dd_surface_t* s, const RECT* r, DWORD color) { (void)s; (void)r; (void)color; return 0; }
int d3dr_flip(dd_surface_t* front, int vsync) { (void)front; (void)vsync; return 0; }
void d3dr_surface_freed(dd_surface_t* s) { (void)s; }
void d3dr_directdraw_created(void) {}

static int s_formats, s_zformats, s_devices;
static DDPIXELFORMAT s_alpha_format;

static HRESULT CALLBACK on_texture_format(LPDDPIXELFORMAT pf, LPVOID context) {
    (void)context;
    s_formats++;
    if (pf->dwRGBAlphaBitMask == 0xF000) s_alpha_format = *pf;
    return D3DENUMRET_OK;
}

static HRESULT CALLBACK on_z_format(LPDDPIXELFORMAT pf, LPVOID context) {
    DDPIXELFORMAT* best = (DDPIXELFORMAT*)context;
    s_zformats++;
    if (pf->dwFlags == DDPF_ZBUFFER && pf->dwZBufferBitDepth > best->dwZBufferBitDepth) *best = *pf;
    return D3DENUMRET_OK;
}

/* The game's own acceptance test (sub_00408530) applied to what EnumDevices reports. */
static HRESULT CALLBACK on_device(GUID* guid, LPSTR description, LPSTR name, LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel,
                                  LPVOID context) {
    (void)description; (void)name; (void)hel; (void)context;
    s_devices++;
    CHECK(guid && IsEqualGUID(guid, &IID_IDirect3DHALDevice));
    CHECK(hal && hal->dcmColorModel == D3DCOLOR_RGB);
    CHECK(hal->dwDeviceRenderBitDepth & DDBD_16);
    CHECK(hal->dpcTriCaps.dwShadeCaps & D3DPSHADECAPS_COLORGOURAUDRGB);
    CHECK(hal->dwDeviceZBufferBitDepth != 0);
    CHECK(hal->dpcTriCaps.dwAlphaCmpCaps & D3DPCMPCAPS_GREATER);
    CHECK(hal->dpcTriCaps.dwZCmpCaps & D3DPCMPCAPS_GREATEREQUAL);
    CHECK(hal->dwTextureOpCaps & D3DTEXOPCAPS_MODULATE);
    return D3DENUMRET_OK;
}

static IDirectDrawSurface4* make_surface(IDirectDraw4* dd4, DWORD caps, DWORD w, DWORD h, const DDPIXELFORMAT* pf) {
    DDSURFACEDESC2 desc;
    IDirectDrawSurface4* s = NULL;
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof desc;
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0);
    desc.ddsCaps.dwCaps = caps;
    desc.dwWidth = w;
    desc.dwHeight = h;
    if (pf) desc.ddpfPixelFormat = *pf;
    CHECK(SUCCEEDED(dd4->lpVtbl->CreateSurface(dd4, &desc, &s, NULL)));
    return s;
}

int main(void) {
    IDirectDraw* dd = NULL;
    IDirectDraw4* dd4 = NULL;
    IDirect3D3* d3d = NULL;
    IDirect3D* d3d1 = NULL;
    IDirect3DDevice3* dev = NULL;
    IDirect3DViewport3* vp = NULL;
    IDirectDrawSurface4 *back, *z, *sys, *vid;
    IDirect3DTexture2 *sys_tex = NULL, *vid_tex = NULL, *got = NULL;
    DDPIXELFORMAT zpf, rgb565;
    D3DVIEWPORT2 vp2;
    D3DTLVERTEX fan[4];
    D3DRECT rect = { 0, 0, 640, 480 };
    DDSURFACEDESC2 lock;
    DWORD value = 0;
    ULONG refs;
    unsigned long refused_render = 0, refused_stage = 0, failed = 0;

    CHECK(dd_device_vtables_complete() && dd_surface_vtables_complete() && dd_object_vtables_complete());
    CHECK(d3d_vtables_complete());
    CHECK(SUCCEEDED(compat_DirectDrawCreate(NULL, &dd, NULL)));
    CHECK(SUCCEEDED(dd->lpVtbl->QueryInterface(dd, &IID_IDirectDraw4, (void**)&dd4)));
    CHECK(SUCCEEDED(dd4->lpVtbl->QueryInterface(dd4, &IID_IDirect3D3, (void**)&d3d)));
    CHECK(dd4->lpVtbl->QueryInterface(dd4, &IID_IDirect3D, (void**)&d3d1) == E_NOINTERFACE && !d3d1);
    refs = d3d->lpVtbl->AddRef(d3d);
    CHECK(refs == 4);
    CHECK(d3d->lpVtbl->Release(d3d) == 3);

    CHECK(SUCCEEDED(d3d->lpVtbl->EnumDevices(d3d, on_device, NULL)) && s_devices == 1);
    memset(&zpf, 0, sizeof zpf);
    zpf.dwSize = sizeof zpf;
    zpf.dwFlags = DDPF_ZBUFFER;
    CHECK(SUCCEEDED(d3d->lpVtbl->EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, on_z_format, &zpf)));
    CHECK(s_zformats == 2 && zpf.dwZBufferBitDepth == 24);

    memset(&rgb565, 0, sizeof rgb565);
    rgb565.dwSize = sizeof rgb565;
    rgb565.dwFlags = DDPF_RGB;
    rgb565.dwRGBBitCount = 16;
    rgb565.dwRBitMask = 0xF800;
    rgb565.dwGBitMask = 0x07E0;
    rgb565.dwBBitMask = 0x001F;
    back = make_surface(dd4, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE, 640, 480, &rgb565);
    z = make_surface(dd4, DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY, 640, 480, &zpf);
    CHECK(back && z);
    CHECK(SUCCEEDED(back->lpVtbl->AddAttachedSurface(back, (IDirectDrawSurface4*)z)));
    CHECK(back->lpVtbl->AddAttachedSurface(back, (IDirectDrawSurface4*)z) == DDERR_SURFACEALREADYATTACHED);

    CHECK(SUCCEEDED(d3d->lpVtbl->CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev, NULL)) && s_attached == 1);
    CHECK(SUCCEEDED(dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZENABLE, &value)) && value == D3DZB_TRUE);
    CHECK(SUCCEEDED(dev->lpVtbl->EnumTextureFormats(dev, on_texture_format, NULL)) && s_formats == 3);
    CHECK(s_alpha_format.dwFlags & DDPF_ALPHAPIXELS);

    CHECK(SUCCEEDED(d3d->lpVtbl->CreateViewport(d3d, &vp, NULL)));
    CHECK(SUCCEEDED(dev->lpVtbl->AddViewport(dev, vp)));
    memset(&vp2, 0, sizeof vp2);
    vp2.dwSize = sizeof vp2;
    vp2.dwWidth = 640;
    vp2.dwHeight = 480;
    vp2.dvClipX = -1.0f;
    vp2.dvClipY = 1.0f;
    vp2.dvClipWidth = 2.0f;
    vp2.dvClipHeight = 2.0f;
    vp2.dvMaxZ = 1.0f;
    CHECK(SUCCEEDED(vp->lpVtbl->SetViewport2(vp, &vp2)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetCurrentViewport(dev, vp)));
    CHECK(SUCCEEDED(vp->lpVtbl->Clear2(vp, 1, &rect, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 0.0f, 0)) && s_clears == 1);

    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZFUNC, D3DCMP_GREATEREQUAL)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ALPHAREF, 0x1000)));
    CHECK(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZFUNC, 0) == DDERR_INVALIDPARAMS);
    CHECK(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_STENCILENABLE, 1) == DDERR_INVALIDPARAMS);
    CHECK(SUCCEEDED(dev->lpVtbl->GetRenderState(dev, D3DRENDERSTATE_ZFUNC, &value)) && value == D3DCMP_GREATEREQUAL);
    CHECK(SUCCEEDED(dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP)));
    CHECK(SUCCEEDED(dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_ADDRESSV, &value)) && value == D3DTADDRESS_CLAMP);
    CHECK(dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_ADD) == DDERR_INVALIDPARAMS);

    /* What sub_00516190 does for every stage when a level starts. */
    for (DWORD k = 0; k < 8; k++) {
        CHECK(SUCCEEDED(dev->lpVtbl->SetTexture(dev, k, NULL)));
        CHECK(SUCCEEDED(dev->lpVtbl->SetTextureStageState(dev, k, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP)));
        CHECK(SUCCEEDED(dev->lpVtbl->SetTextureStageState(dev, k, D3DTSS_MAGFILTER, D3DTFG_POINT)));
        CHECK(SUCCEEDED(dev->lpVtbl->SetTextureStageState(dev, k, D3DTSS_MINFILTER, D3DTFN_POINT)));
    }
    CHECK(SUCCEEDED(dev->lpVtbl->GetTextureStageState(dev, 5, D3DTSS_ADDRESSU, &value)) && value == D3DTADDRESS_CLAMP);
    CHECK(SUCCEEDED(dev->lpVtbl->GetTextureStageState(dev, 3, D3DTSS_COLOROP, &value)) && value == D3DTOP_DISABLE);
    CHECK(SUCCEEDED(dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLOROP, &value)) && value == D3DTOP_MODULATE);
    CHECK(dev->lpVtbl->SetTextureStageState(dev, 8, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP) == DDERR_INVALIDPARAMS);
    CHECK(dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_MODULATE) == DDERR_INVALIDPARAMS);
    CHECK(SUCCEEDED(dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZBIAS, 0)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZBIAS, 16)));
    CHECK(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZBIAS, 17) == DDERR_INVALIDPARAMS);
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_ZBIAS, 0)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_RANGEFOGENABLE, 0)));
    CHECK(SUCCEEDED(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE)));
    CHECK(dev->lpVtbl->SetRenderState(dev, D3DRENDERSTATE_TEXTUREMAPBLEND, 9) == DDERR_INVALIDPARAMS);
    d3d_objects_counts(&refused_render, &refused_stage, &failed);
    CHECK(refused_render == 4 && refused_stage == 3);
    CHECK(failed >= refused_render + refused_stage);

    sys = make_surface(dd4, DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 64, 64, &s_alpha_format);
    vid = make_surface(dd4, DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 64, 64, &s_alpha_format);
    CHECK(SUCCEEDED(sys->lpVtbl->QueryInterface(sys, &IID_IDirect3DTexture2, (void**)&sys_tex)));
    CHECK(SUCCEEDED(vid->lpVtbl->QueryInterface(vid, &IID_IDirect3DTexture2, (void**)&vid_tex)));
    CHECK(sys->lpVtbl->AddRef(sys) == 3);
    CHECK(sys->lpVtbl->Release(sys) == 2);
    memset(&lock, 0, sizeof lock);
    lock.dwSize = sizeof lock;
    CHECK(SUCCEEDED(sys->lpVtbl->Lock(sys, NULL, &lock, DDLOCK_WAIT, NULL)));
    ((WORD*)lock.lpSurface)[0] = 0xF123;
    CHECK(SUCCEEDED(sys->lpVtbl->Unlock(sys, NULL)));
    CHECK(SUCCEEDED(vid_tex->lpVtbl->Load(vid_tex, sys_tex)));
    CHECK(SUCCEEDED(vid->lpVtbl->Lock(vid, NULL, &lock, DDLOCK_WAIT | DDLOCK_READONLY, NULL)));
    CHECK(((WORD*)lock.lpSurface)[0] == 0xF123);
    CHECK(SUCCEEDED(vid->lpVtbl->Unlock(vid, NULL)));

    CHECK(SUCCEEDED(dev->lpVtbl->SetTexture(dev, 0, vid_tex)));
    CHECK(SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &got)) && got == vid_tex);
    got->lpVtbl->Release(got);
    CHECK(SUCCEEDED(dev->lpVtbl->SetTexture(dev, 2, vid_tex)));
    CHECK(SUCCEEDED(dev->lpVtbl->GetTexture(dev, 2, &got)) && got == vid_tex);
    got->lpVtbl->Release(got);
    CHECK(SUCCEEDED(dev->lpVtbl->SetTexture(dev, 2, NULL)));
    CHECK(dev->lpVtbl->SetTexture(dev, 8, NULL) == DDERR_INVALIDPARAMS);
    memset(fan, 0, sizeof fan);
    CHECK(SUCCEEDED(dev->lpVtbl->BeginScene(dev)));
    CHECK(dev->lpVtbl->BeginScene(dev) == D3DERR_SCENE_IN_SCENE);
    CHECK(SUCCEEDED(dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLEFAN, D3DFVF_TLVERTEX, fan, 4, 0x1C)) && s_draws >= 1);
    CHECK(SUCCEEDED(dev->lpVtbl->EndScene(dev)));
    CHECK(dev->lpVtbl->DrawPrimitiveVB(dev, D3DPT_TRIANGLEFAN, NULL, 0, 0, 0) == DDERR_UNSUPPORTED);

    CHECK(SUCCEEDED(dev->lpVtbl->SetTexture(dev, 0, NULL)));
    CHECK(vid_tex->lpVtbl->Release(vid_tex) == 1);
    CHECK(sys_tex->lpVtbl->Release(sys_tex) == 1);
    CHECK(vid->lpVtbl->Release(vid) == 0);
    CHECK(sys->lpVtbl->Release(sys) == 0);
    CHECK(vp->lpVtbl->Release(vp) == 1);
    CHECK(dev->lpVtbl->Release(dev) == 0 && s_attached == 0);
    CHECK(z->lpVtbl->Release(z) == 1);
    CHECK(back->lpVtbl->Release(back) == 0);
    CHECK(d3d->lpVtbl->Release(d3d) == 2);
    CHECK(dd4->lpVtbl->Release(dd4) == 1);
    CHECK(dd->lpVtbl->Release(dd) == 0);
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}
