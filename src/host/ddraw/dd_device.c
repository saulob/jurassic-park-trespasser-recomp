/*
 * IDirectDraw, IDirectDraw2 and IDirectDraw4 over a virtual 16 bpp display. The desktop keeps its
 * own mode; the display the game sees exists only here and reaches the screen through D3D11.
 */
#include "d3d_legacy.h"
#include "host.h"

#include <stdio.h>

static const struct { DWORD width, height; } k_modes[] = {
    { 320, 240 }, { 400, 300 }, { 512, 384 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
    { 1152, 864 }, { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
};
#define MODE_COUNT ((int)(sizeof k_modes / sizeof k_modes[0]))
#define DISPLAY_BPP 16

static const IDirectDrawVtbl s_dd1_vtbl;
static const IDirectDraw2Vtbl s_dd2_vtbl;
static const IDirectDraw4Vtbl s_dd4_vtbl;

static void destroy_device_if_unused(dd_device_t* d) {
    if (d->released && d->surfaces == 0) {
        free(d);
    }
}

void dd_surface_added(dd_device_t* d) {
    d->surfaces++;
}

void dd_surface_removed(dd_device_t* d) {
    d->surfaces--;
    destroy_device_if_unused(d);
}

static int desktop_frequency(void) {
    DEVMODEA mode;
    memset(&mode, 0, sizeof mode);
    mode.dmSize = sizeof mode;
    return EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode) ? (int)mode.dmDisplayFrequency : 60;
}

static unsigned s_display_generation;

unsigned dd_display_generation(void) { return s_display_generation; }

void dd_display_size(const dd_device_t* d, DWORD* width, DWORD* height) {
    if (d->mode_set) {
        *width = d->mode_width;
        *height = d->mode_height;
    } else {
        *width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
        *height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    }
}

static int mode_fits(DWORD width, DWORD height) {
    return width <= (DWORD)GetSystemMetrics(SM_CXSCREEN) && height <= (DWORD)GetSystemMetrics(SM_CYSCREEN);
}

#define GAMEPLAY_MODES 2
static struct { DWORD width, height; } s_gameplay[GAMEPLAY_MODES];

void dd_set_gameplay_modes(DWORD hardware_w, DWORD hardware_h, DWORD software_w, DWORD software_h) {
    s_gameplay[0].width = hardware_w;
    s_gameplay[0].height = hardware_h;
    s_gameplay[1].width = software_w;
    s_gameplay[1].height = software_h;
    if (software_w == hardware_w && software_h == hardware_h) s_gameplay[1].width = s_gameplay[1].height = 0;
}

static int gameplay_mode(DWORD width, DWORD height) {
    for (int i = 0; i < GAMEPLAY_MODES; i++)
        if (s_gameplay[i].width && s_gameplay[i].width == width && s_gameplay[i].height == height) return 1;
    return 0;
}

static int mode_listed(DWORD width, DWORD height) {
    if (gameplay_mode(width, height)) return 1;
    for (int i = 0; i < MODE_COUNT; i++)
        if (k_modes[i].width == width && k_modes[i].height == height) return mode_fits(width, height);
    return 0;
}

/* The game's mode table (0x006D0B48) holds 60 sizes and its options list (0x0066DCA0) 20 up to 800 wide, both
 * unchecked: these 12 at most stay within both. */
static int mode_enumerated(int i, DWORD* width, DWORD* height) {
    if (i < MODE_COUNT) {
        *width = k_modes[i].width;
        *height = k_modes[i].height;
        return mode_fits(*width, *height) && !gameplay_mode(*width, *height);
    }
    i -= MODE_COUNT;
    if (i >= GAMEPLAY_MODES) return 0;
    *width = s_gameplay[i].width;
    *height = s_gameplay[i].height;
    return *width != 0;
}

static void fill_mode_desc(DDSURFACEDESC2* desc, DWORD size, DWORD width, DWORD height, int refresh) {
    memset(desc, 0, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwWidth = width;
    desc->dwHeight = height;
    desc->lPitch = (LONG)(width * DISPLAY_BPP / 8);
    dd_format_rgb565(&desc->ddpfPixelFormat);
    if (refresh) {
        desc->dwFlags |= DDSD_REFRESHRATE;
        desc->dwRefreshRate = (DWORD)refresh;
    }
}

/* The game believes it owns a full screen of the virtual mode's size; the window host decides what the
 * window really is and maps the game's screen onto it. */
static void sync_window_display(dd_device_t* d) {
    if (!d->window) return;
    if ((d->coop & DDSCL_FULLSCREEN) && d->mode_set)
        window_host_set_display(d->window, (int)d->mode_width, (int)d->mode_height, s_display_generation);
    else
        window_host_clear_display(d->window);
}

static HRESULT device_query(dd_device_t* d, REFIID riid, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectDraw)) *out = &d->dd1;
    else if (IsEqualGUID(riid, &IID_IDirectDraw2)) *out = &d->dd2;
    else if (IsEqualGUID(riid, &IID_IDirectDraw4)) *out = &d->dd4;
    if (*out) {
        InterlockedIncrement(&d->refs);
        return DD_OK;
    }
    if (IsEqualGUID(riid, &IID_IDirect3D) || IsEqualGUID(riid, &IID_IDirect3D2) || IsEqualGUID(riid, &IID_IDirect3D3)) {
        if (d3dr_hardware()) return d3d_query_direct3d(d, riid, out);
        dd_log("Direct3D was requested from sub_%08X with --renderer software: refused, the game keeps its software "
               "renderer", g_cur_func);
    } else
        dd_log("QueryInterface for {%08lX-%04X-%04X-...} on the DirectDraw object: not implemented", riid->Data1,
               riid->Data2, riid->Data3);
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dd_QueryInterface(IDirectDraw* This, REFIID riid, LPVOID* out) {
    DD_METHOD("IDirectDraw::QueryInterface");
    DD_RETURN(device_query(DD_OWNER(dd_device_t, This), riid, out));
}

static ULONG STDMETHODCALLTYPE dd_AddRef(IDirectDraw* This) {
    return (ULONG)InterlockedIncrement(&DD_OWNER(dd_device_t, This)->refs);
}

static ULONG STDMETHODCALLTYPE dd_Release(IDirectDraw* This) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    LONG refs = InterlockedDecrement(&d->refs);
    if (refs > 0) return (ULONG)refs;
    dd_lock();
    d->mode_set = 0;
    sync_window_display(d);
    d->released = 1;
    dd_log("DirectDraw object 0x%08X released (%d surfaces still alive)", (uint32_t)(uintptr_t)d, d->surfaces);
    destroy_device_if_unused(d);
    dd_unlock();
    return 0;
}

static HRESULT STDMETHODCALLTYPE dd_Compact(IDirectDraw* This) {
    (void)This;
    DD_UNSUPPORTED("IDirectDraw::Compact", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE dd_CreateClipper(IDirectDraw* This, DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
    (void)This;
    DD_METHOD("IDirectDraw::CreateClipper");
    if (outer) DD_RETURN(CLASS_E_NOAGGREGATION);
    DD_RETURN(dd_create_clipper(flags, out));
}

static HRESULT STDMETHODCALLTYPE dd_CreatePalette(IDirectDraw* This, DWORD flags, LPPALETTEENTRY entries,
                                                  LPDIRECTDRAWPALETTE* out, IUnknown* outer) {
    (void)This;
    DD_METHOD("IDirectDraw::CreatePalette");
    if (outer) DD_RETURN(CLASS_E_NOAGGREGATION);
    DD_RETURN(dd_create_palette(flags, entries, out));
}

static dd_method_t s_create_surface1 = { "IDirectDraw::CreateSurface", 0 };
static dd_method_t s_create_surface4 = { "IDirectDraw4::CreateSurface", 0 };

static HRESULT create_surface(void* This, const DDSURFACEDESC* desc, void** out, IUnknown* outer, int version) {
    DD_BEGIN(version == 4 ? &s_create_surface4 : &s_create_surface1);
    if (outer) DD_RETURN(CLASS_E_NOAGGREGATION);
    DD_RETURN(dd_create_surface(DD_OWNER(dd_device_t, This), desc, version, out));
}

static HRESULT STDMETHODCALLTYPE dd_CreateSurface(IDirectDraw* This, LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out,
                                                  IUnknown* outer) {
    return create_surface(This, desc, (void**)out, outer, 1);
}

static HRESULT STDMETHODCALLTYPE dd4_CreateSurface(IDirectDraw4* This, LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE4* out,
                                                   IUnknown* outer) {
    return create_surface(This, (const DDSURFACEDESC*)desc, (void**)out, outer, 4);
}

static HRESULT STDMETHODCALLTYPE dd_DuplicateSurface(IDirectDraw* This, LPDIRECTDRAWSURFACE source, LPDIRECTDRAWSURFACE* out) {
    (void)This; (void)source; (void)out;
    DD_UNSUPPORTED("IDirectDraw::DuplicateSurface", "unused by the game", DDERR_UNSUPPORTED);
}

static dd_method_t s_enum_modes1 = { "IDirectDraw::EnumDisplayModes", 0 };
static dd_method_t s_enum_modes4 = { "IDirectDraw4::EnumDisplayModes", 0 };

/* The callback is the game's: calling it faults into native32's trampoline like any Windows callback. */
static HRESULT enum_modes(void* This, DWORD flags, const DDSURFACEDESC2* filter, void* context, void* callback, int version) {
    DWORD size = version == 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    int refresh = (flags & DDEDM_REFRESHRATES) ? desktop_frequency() : 0;
    DDSURFACEDESC2 desc;
    (void)This;
    DD_BEGIN(version == 4 ? &s_enum_modes4 : &s_enum_modes1);
    if (!callback) DD_RETURN(DDERR_INVALIDPARAMS);
    if (filter && filter->dwSize != size) DD_RETURN(DDERR_INVALIDPARAMS);
    for (int i = 0; i < MODE_COUNT + GAMEPLAY_MODES; i++) {
        DWORD w, h;
        if (!mode_enumerated(i, &w, &h)) continue;
        if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != w) continue;
        if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != h) continue;
        if (filter && (filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != DISPLAY_BPP) continue;
        fill_mode_desc(&desc, size, w, h, refresh);
        if (((HRESULT(WINAPI*)(DDSURFACEDESC2*, LPVOID))callback)(&desc, context) == DDENUMRET_CANCEL) break;
    }
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd_EnumDisplayModes(IDirectDraw* This, DWORD flags, LPDDSURFACEDESC filter, LPVOID context,
                                                     LPDDENUMMODESCALLBACK callback) {
    return enum_modes(This, flags, (const DDSURFACEDESC2*)filter, context, (void*)callback, 1);
}

static HRESULT STDMETHODCALLTYPE dd4_EnumDisplayModes(IDirectDraw4* This, DWORD flags, LPDDSURFACEDESC2 filter,
                                                      LPVOID context, LPDDENUMMODESCALLBACK2 callback) {
    return enum_modes(This, flags, filter, context, (void*)callback, 4);
}

static HRESULT STDMETHODCALLTYPE dd_EnumSurfaces(IDirectDraw* This, DWORD flags, LPDDSURFACEDESC desc, LPVOID context,
                                                 LPDDENUMSURFACESCALLBACK callback) {
    (void)This; (void)flags; (void)desc; (void)context; (void)callback;
    DD_UNSUPPORTED("IDirectDraw::EnumSurfaces", "unused by the game", DDERR_UNSUPPORTED);
}

/* GDI is always visible: the virtual display is composed into a window, never scanned out. */
static HRESULT STDMETHODCALLTYPE dd_FlipToGDISurface(IDirectDraw* This) {
    (void)This;
    DD_METHOD("IDirectDraw::FlipToGDISurface");
    DD_RETURN(DD_OK);
}

static void fill_caps(DDCAPS* caps, const dd_adapter_t* adapter) {
    DWORD memory = adapter->dedicated_video_memory > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)adapter->dedicated_video_memory;
    DWORD surface_caps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP |
                         DDSCAPS_COMPLEX | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY |
                         DDSCAPS_LOCALVIDMEM | DDSCAPS_PALETTE | DDSCAPS_VISIBLE;
    memset(caps, 0, sizeof *caps);
    caps->dwSize = sizeof *caps;
    caps->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM;
    caps->dwCaps2 = DDCAPS2_PRIMARYGAMMA | DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED;
    caps->dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_SRCBLTCLRSPACE;
    caps->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
    caps->dwPalCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
    if (d3dr_hardware()) {
        caps->dwCaps |= DDCAPS_3D;
        caps->dwZBufferBitDepths = DDBD_16 | DDBD_24;
        surface_caps |= DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER;
    }
    caps->dwVidMemTotal = memory;
    caps->dwVidMemFree = memory;
    caps->ddsCaps.dwCaps = surface_caps;
    caps->dwSVBCaps = caps->dwVSBCaps = caps->dwSSBCaps = caps->dwCaps;
    caps->dwSVBCKeyCaps = caps->dwVSBCKeyCaps = caps->dwSSBCKeyCaps = caps->dwCKeyCaps;
    caps->dwSVBFXCaps = caps->dwVSBFXCaps = caps->dwSSBFXCaps = caps->dwFXCaps;
    caps->ddsOldCaps.dwCaps = surface_caps;
}

static int caps_size_known(DWORD size) {
    return size == sizeof(DDCAPS_DX1) || size == sizeof(DDCAPS_DX3) || size == sizeof(DDCAPS_DX5) ||
           size == sizeof(DDCAPS_DX6) || size == sizeof(DDCAPS_DX7);
}

static HRESULT STDMETHODCALLTYPE dd_GetCaps(IDirectDraw* This, LPDDCAPS driver, LPDDCAPS hel) {
    static int reported;
    dd_adapter_t adapter;
    DDCAPS caps;
    (void)This;
    DD_METHOD("IDirectDraw::GetCaps");
    if ((!driver && !hel) || (driver && !caps_size_known(driver->dwSize)) || (hel && !caps_size_known(hel->dwSize)))
        DD_RETURN(DDERR_INVALIDPARAMS);
    present_adapter(&adapter);
    fill_caps(&caps, &adapter);
    if (driver) {
        DWORD size = driver->dwSize;
        memcpy(driver, &caps, size);
        driver->dwSize = size;
    }
    if (hel) {
        DWORD size = hel->dwSize;
        memcpy(hel, &caps, size);
        hel->dwSize = size;
    }
    if (!reported) {
        reported = 1;
        dd_log("GetCaps: caps 0x%08lX, caps2 0x%08lX (no DDCAPS2_CERTIFIED: this is not certified display hardware)",
               caps.dwCaps, caps.dwCaps2);
    }
    DD_RETURN(DD_OK);
}

static dd_method_t s_get_mode1 = { "IDirectDraw::GetDisplayMode", 0 };
static dd_method_t s_get_mode4 = { "IDirectDraw4::GetDisplayMode", 0 };

static HRESULT get_display_mode(void* This, DDSURFACEDESC2* desc, DWORD size) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    DWORD width, height;
    DD_BEGIN(size == sizeof(DDSURFACEDESC2) ? &s_get_mode4 : &s_get_mode1);
    if (!desc || desc->dwSize != size) DD_RETURN(DDERR_INVALIDPARAMS);
    dd_display_size(d, &width, &height);
    fill_mode_desc(desc, size, width, height, desktop_frequency());
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd_GetDisplayMode(IDirectDraw* This, LPDDSURFACEDESC desc) {
    return get_display_mode(This, (DDSURFACEDESC2*)desc, sizeof(DDSURFACEDESC));
}

static HRESULT STDMETHODCALLTYPE dd4_GetDisplayMode(IDirectDraw4* This, LPDDSURFACEDESC2 desc) {
    return get_display_mode(This, desc, sizeof(DDSURFACEDESC2));
}

static HRESULT STDMETHODCALLTYPE dd_GetFourCCCodes(IDirectDraw* This, LPDWORD count, LPDWORD codes) {
    (void)This; (void)codes;
    DD_METHOD("IDirectDraw::GetFourCCCodes");
    if (!count) DD_RETURN(DDERR_INVALIDPARAMS);
    *count = 0;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd_GetGDISurface(IDirectDraw* This, LPDIRECTDRAWSURFACE* out) {
    (void)This; (void)out;
    DD_UNSUPPORTED("IDirectDraw::GetGDISurface", "GDI never draws into the virtual display", DDERR_NOTFOUND);
}

static HRESULT STDMETHODCALLTYPE dd_GetMonitorFrequency(IDirectDraw* This, LPDWORD frequency) {
    (void)This;
    DD_METHOD("IDirectDraw::GetMonitorFrequency");
    if (!frequency) DD_RETURN(DDERR_INVALIDPARAMS);
    *frequency = (DWORD)desktop_frequency();
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd_GetScanLine(IDirectDraw* This, LPDWORD line) {
    (void)This; (void)line;
    DD_UNSUPPORTED("IDirectDraw::GetScanLine", "the virtual display has no scan-out", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE dd_GetVerticalBlankStatus(IDirectDraw* This, LPBOOL in_blank) {
    (void)This; (void)in_blank;
    DD_UNSUPPORTED("IDirectDraw::GetVerticalBlankStatus", "the virtual display has no scan-out", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE dd_Initialize(IDirectDraw* This, GUID* guid) {
    (void)This; (void)guid;
    DD_METHOD("IDirectDraw::Initialize");
    DD_RETURN(DDERR_ALREADYINITIALIZED);
}

static HRESULT STDMETHODCALLTYPE dd_RestoreDisplayMode(IDirectDraw* This) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    DD_METHOD("IDirectDraw::RestoreDisplayMode");
    if (d->mode_set) {
        d->mode_set = 0;
        dd_device_mode_changed(d);
        sync_window_display(d);
        dd_log("RestoreDisplayMode: virtual mode cleared");
    }
    DD_RETURN(DD_OK);
}

static const char* coop_text(DWORD flags) {
    if ((flags & (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) == (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) return "FULLSCREEN|EXCLUSIVE";
    if (flags & DDSCL_NORMAL) return "NORMAL";
    return "other";
}

static HRESULT STDMETHODCALLTYPE dd_SetCooperativeLevel(IDirectDraw* This, HWND window, DWORD flags) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    DD_METHOD("IDirectDraw::SetCooperativeLevel");
    if ((flags & DDSCL_EXCLUSIVE) && (!(flags & DDSCL_FULLSCREEN) || !window)) DD_RETURN(DDERR_INVALIDPARAMS);
    if ((flags & DDSCL_NORMAL) && (flags & DDSCL_EXCLUSIVE)) DD_RETURN(DDERR_INVALIDPARAMS);
    if (!(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_SETFOCUSWINDOW | DDSCL_SETDEVICEWINDOW)))
        DD_RETURN(DDERR_INVALIDPARAMS);
    if (window && !IsWindow(window)) DD_RETURN(DDERR_INVALIDPARAMS);
    if (window) d->window = window;
    d->coop = flags;
    dd_log("SetCooperativeLevel(hwnd 0x%08X, %s, flags 0x%lX): exclusive mode is emulated, never taken",
           (uint32_t)(uintptr_t)window, coop_text(flags), flags);
    sync_window_display(d);
    DD_RETURN(DD_OK);
}

static HRESULT set_display_mode(void* This, DWORD width, DWORD height, DWORD bpp, DWORD refresh) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    DD_METHOD("IDirectDraw::SetDisplayMode");
    if (bpp != DISPLAY_BPP || !mode_listed(width, height)) {
        dd_log("SetDisplayMode %lux%lux%lu: not a virtual mode (16 bpp, at most the desktop's size)", width, height, bpp);
        DD_RETURN(DDERR_INVALIDMODE);
    }
    if (!d->mode_set || d->mode_width != width || d->mode_height != height) {
        d->mode_width = width;
        d->mode_height = height;
        d->mode_set = 1;
        s_display_generation++;
        dd_device_mode_changed(d);
    }
    dd_log("SetDisplayMode %lux%lux%lu (refresh %lu): virtual mode set, RGB565", width, height, bpp, refresh);
    sync_window_display(d);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd_SetDisplayMode(IDirectDraw* This, DWORD width, DWORD height, DWORD bpp) {
    return set_display_mode(This, width, height, bpp, 0);
}

static HRESULT STDMETHODCALLTYPE dd2_SetDisplayMode(IDirectDraw2* This, DWORD width, DWORD height, DWORD bpp,
                                                    DWORD refresh, DWORD flags) {
    (void)flags;
    return set_display_mode(This, width, height, bpp, refresh);
}

static HRESULT STDMETHODCALLTYPE dd_WaitForVerticalBlank(IDirectDraw* This, DWORD flags, HANDLE event) {
    (void)This; (void)flags; (void)event;
    DD_UNSUPPORTED("IDirectDraw::WaitForVerticalBlank", "presentation is paced by the D3D11 swap chain", DDERR_UNSUPPORTED);
}

static HRESULT available_memory(DWORD* total, DWORD* free_bytes) {
    dd_adapter_t adapter;
    DWORD memory;
    DD_METHOD("IDirectDraw2::GetAvailableVidMem");
    if (!total && !free_bytes) DD_RETURN(DDERR_INVALIDPARAMS);
    present_adapter(&adapter);
    memory = adapter.dedicated_video_memory > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)adapter.dedicated_video_memory;
    if (total) *total = memory;
    if (free_bytes) *free_bytes = memory;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd2_GetAvailableVidMem(IDirectDraw2* This, LPDDSCAPS caps, LPDWORD total, LPDWORD free_bytes) {
    (void)This; (void)caps;
    return available_memory(total, free_bytes);
}

static HRESULT STDMETHODCALLTYPE dd4_GetAvailableVidMem(IDirectDraw4* This, LPDDSCAPS2 caps, LPDWORD total, LPDWORD free_bytes) {
    (void)This; (void)caps;
    return available_memory(total, free_bytes);
}

static HRESULT STDMETHODCALLTYPE dd4_GetSurfaceFromDC(IDirectDraw4* This, HDC dc, LPDIRECTDRAWSURFACE4* out) {
    (void)This; (void)dc; (void)out;
    DD_UNSUPPORTED("IDirectDraw4::GetSurfaceFromDC", "unused by the game", DDERR_NOTFOUND);
}

/* Surfaces live in system memory and are never lost to another application. */
static HRESULT STDMETHODCALLTYPE dd4_RestoreAllSurfaces(IDirectDraw4* This) {
    (void)This;
    DD_METHOD("IDirectDraw4::RestoreAllSurfaces");
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE dd4_TestCooperativeLevel(IDirectDraw4* This) {
    dd_device_t* d = DD_OWNER(dd_device_t, This);
    DD_METHOD("IDirectDraw4::TestCooperativeLevel");
    DD_RETURN(d->coop ? DD_OK : DDERR_NOCOOPERATIVELEVELSET);
}

static HRESULT STDMETHODCALLTYPE dd4_GetDeviceIdentifier(IDirectDraw4* This, LPDDDEVICEIDENTIFIER id, DWORD flags) {
    dd_adapter_t adapter;
    (void)This; (void)flags;
    DD_METHOD("IDirectDraw4::GetDeviceIdentifier");
    if (!id) DD_RETURN(DDERR_INVALIDPARAMS);
    memset(id, 0, sizeof *id);
    if (!present_adapter(&adapter)) DD_RETURN(DDERR_GENERIC);
    strcpy(id->szDriver, "D3D11 (Trespasser DirectDraw compatibility layer)");
    _snprintf(id->szDescription, sizeof id->szDescription - 1, "%s", adapter.description);
    id->liDriverVersion = adapter.driver_version;
    id->dwVendorId = adapter.vendor;
    id->dwDeviceId = adapter.device;
    id->dwSubSysId = adapter.subsystem;
    id->dwRevision = adapter.revision;
    dd_log("GetDeviceIdentifier: %s, vendor 0x%04X, device 0x%04X", adapter.description, adapter.vendor, adapter.device);
    DD_RETURN(DD_OK);
}

/* The surface parameters differ only in pointer type between the three versions. */
#define SHARED(fn) ((void*)(fn))

static const IDirectDrawVtbl s_dd1_vtbl = {
    .QueryInterface = dd_QueryInterface, .AddRef = dd_AddRef, .Release = dd_Release, .Compact = dd_Compact,
    .CreateClipper = dd_CreateClipper, .CreatePalette = dd_CreatePalette, .CreateSurface = dd_CreateSurface,
    .DuplicateSurface = dd_DuplicateSurface, .EnumDisplayModes = dd_EnumDisplayModes, .EnumSurfaces = dd_EnumSurfaces,
    .FlipToGDISurface = dd_FlipToGDISurface, .GetCaps = dd_GetCaps, .GetDisplayMode = dd_GetDisplayMode,
    .GetFourCCCodes = dd_GetFourCCCodes, .GetGDISurface = dd_GetGDISurface,
    .GetMonitorFrequency = dd_GetMonitorFrequency, .GetScanLine = dd_GetScanLine,
    .GetVerticalBlankStatus = dd_GetVerticalBlankStatus, .Initialize = dd_Initialize,
    .RestoreDisplayMode = dd_RestoreDisplayMode, .SetCooperativeLevel = dd_SetCooperativeLevel,
    .SetDisplayMode = dd_SetDisplayMode, .WaitForVerticalBlank = dd_WaitForVerticalBlank,
};

static const IDirectDraw2Vtbl s_dd2_vtbl = {
    .QueryInterface = SHARED(dd_QueryInterface), .AddRef = SHARED(dd_AddRef), .Release = SHARED(dd_Release),
    .Compact = SHARED(dd_Compact), .CreateClipper = SHARED(dd_CreateClipper), .CreatePalette = SHARED(dd_CreatePalette),
    .CreateSurface = SHARED(dd_CreateSurface), .DuplicateSurface = SHARED(dd_DuplicateSurface),
    .EnumDisplayModes = SHARED(dd_EnumDisplayModes), .EnumSurfaces = SHARED(dd_EnumSurfaces),
    .FlipToGDISurface = SHARED(dd_FlipToGDISurface), .GetCaps = SHARED(dd_GetCaps),
    .GetDisplayMode = SHARED(dd_GetDisplayMode), .GetFourCCCodes = SHARED(dd_GetFourCCCodes),
    .GetGDISurface = SHARED(dd_GetGDISurface), .GetMonitorFrequency = SHARED(dd_GetMonitorFrequency),
    .GetScanLine = SHARED(dd_GetScanLine), .GetVerticalBlankStatus = SHARED(dd_GetVerticalBlankStatus),
    .Initialize = SHARED(dd_Initialize), .RestoreDisplayMode = SHARED(dd_RestoreDisplayMode),
    .SetCooperativeLevel = SHARED(dd_SetCooperativeLevel), .SetDisplayMode = dd2_SetDisplayMode,
    .WaitForVerticalBlank = SHARED(dd_WaitForVerticalBlank), .GetAvailableVidMem = dd2_GetAvailableVidMem,
};

static const IDirectDraw4Vtbl s_dd4_vtbl = {
    .QueryInterface = SHARED(dd_QueryInterface), .AddRef = SHARED(dd_AddRef), .Release = SHARED(dd_Release),
    .Compact = SHARED(dd_Compact), .CreateClipper = SHARED(dd_CreateClipper), .CreatePalette = SHARED(dd_CreatePalette),
    .CreateSurface = dd4_CreateSurface, .DuplicateSurface = SHARED(dd_DuplicateSurface),
    .EnumDisplayModes = dd4_EnumDisplayModes, .EnumSurfaces = SHARED(dd_EnumSurfaces),
    .FlipToGDISurface = SHARED(dd_FlipToGDISurface), .GetCaps = SHARED(dd_GetCaps),
    .GetDisplayMode = dd4_GetDisplayMode, .GetFourCCCodes = SHARED(dd_GetFourCCCodes),
    .GetGDISurface = SHARED(dd_GetGDISurface), .GetMonitorFrequency = SHARED(dd_GetMonitorFrequency),
    .GetScanLine = SHARED(dd_GetScanLine), .GetVerticalBlankStatus = SHARED(dd_GetVerticalBlankStatus),
    .Initialize = SHARED(dd_Initialize), .RestoreDisplayMode = SHARED(dd_RestoreDisplayMode),
    .SetCooperativeLevel = SHARED(dd_SetCooperativeLevel), .SetDisplayMode = SHARED(dd2_SetDisplayMode),
    .WaitForVerticalBlank = SHARED(dd_WaitForVerticalBlank), .GetAvailableVidMem = dd4_GetAvailableVidMem,
    .GetSurfaceFromDC = dd4_GetSurfaceFromDC, .RestoreAllSurfaces = dd4_RestoreAllSurfaces,
    .TestCooperativeLevel = dd4_TestCooperativeLevel, .GetDeviceIdentifier = dd4_GetDeviceIdentifier,
};

C_ASSERT(sizeof(IDirectDrawVtbl) == 23 * sizeof(void*));
C_ASSERT(sizeof(IDirectDraw2Vtbl) == 24 * sizeof(void*));
C_ASSERT(sizeof(IDirectDraw4Vtbl) == 28 * sizeof(void*));

static const void* const k_vtables[] = { &s_dd1_vtbl, &s_dd2_vtbl, &s_dd4_vtbl };
static const size_t k_vtable_sizes[] = { sizeof s_dd1_vtbl, sizeof s_dd2_vtbl, sizeof s_dd4_vtbl };

int dd_device_vtables_complete(void) {
    for (int t = 0; t < 3; t++) {
        void* const* slots = (void* const*)k_vtables[t];
        for (size_t i = 0; i < k_vtable_sizes[t] / sizeof(void*); i++)
            if (!slots[i]) return 0;
    }
    return 1;
}

static void describe_device_guid(const GUID* guid, char* out, size_t cap) {
    if (!guid) _snprintf(out, cap - 1, "primary display driver");
    else if (guid == (const GUID*)DDCREATE_EMULATIONONLY) _snprintf(out, cap - 1, "emulation only");
    else if (guid == (const GUID*)DDCREATE_HARDWAREONLY) _snprintf(out, cap - 1, "hardware only");
    else _snprintf(out, cap - 1, "device {%08lX-%04X-%04X-...}", guid->Data1, guid->Data2, guid->Data3);
    out[cap - 1] = 0;
}

HRESULT WINAPI compat_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer) {
    dd_device_t* d;
    char device[64];
    DD_METHOD("DirectDrawCreate");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (outer) DD_RETURN(CLASS_E_NOAGGREGATION);
    d = (dd_device_t*)calloc(1, sizeof *d);
    if (!d) DD_RETURN(DDERR_OUTOFMEMORY);
    d->dd1.vtbl = &s_dd1_vtbl;
    d->dd2.vtbl = &s_dd2_vtbl;
    d->dd4.vtbl = &s_dd4_vtbl;
    d->dd1.owner = d->dd2.owner = d->dd4.owner = d;
    d->dd1.version = 1;
    d->dd2.version = 2;
    d->dd4.version = 4;
    d3d_init_direct3d_face(d);
    d->refs = 1;
    for (int i = 0; i < 256; i++)
        d->gamma.red[i] = d->gamma.green[i] = d->gamma.blue[i] = (WORD)(i * 0x101);
    *out = (LPDIRECTDRAW)&d->dd1;
    describe_device_guid(guid, device, sizeof device);
    dd_log("DirectDrawCreate(%s) -> compatibility object 0x%08X: virtual 16 bpp display presented with D3D11",
           device, (uint32_t)(uintptr_t)d);
    DD_RETURN(DD_OK);
}

HRESULT WINAPI compat_DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context) {
    DD_METHOD("DirectDrawEnumerateA");
    if (!callback) DD_RETURN(DDERR_INVALIDPARAMS);
    callback(NULL, "Primary Display Driver", "display", context);
    DD_RETURN(DD_OK);
}

HRESULT WINAPI compat_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, LPVOID context, DWORD flags) {
    (void)flags;
    DD_METHOD("DirectDrawEnumerateExA");
    if (!callback) DD_RETURN(DDERR_INVALIDPARAMS);
    callback(NULL, "Primary Display Driver", "display", context, NULL);
    DD_RETURN(DD_OK);
}

HRESULT WINAPI compat_DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
    DD_METHOD("DirectDrawCreateClipper");
    if (outer) DD_RETURN(CLASS_E_NOAGGREGATION);
    DD_RETURN(dd_create_clipper(flags, out));
}

HRESULT WINAPI compat_DirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID riid, IUnknown* outer) {
    (void)guid; (void)riid; (void)outer;
    if (out) *out = NULL;
    DD_UNSUPPORTED("DirectDrawCreateEx", "IDirectDraw7 is not implemented", DDERR_UNSUPPORTED);
}
