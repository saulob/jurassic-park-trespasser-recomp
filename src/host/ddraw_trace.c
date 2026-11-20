/*
 * Observes the game's DirectDraw calls at the indirect-call dispatch: DirectDrawCreate and
 * DirectDrawEnumerateA by address, then the COM methods of every interface they hand back.
 * Each observed call goes to the real ddraw.dll through native32's bridge, unchanged; only
 * the arguments and the HRESULT are read. The game discards DirectDraw failures silently
 * (0x005053A0 reports them to a function that returns at once), so this is where they show.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

#include "host.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum { F_EXPORT, F_DDRAW, F_SURFACE, F_CLIPPER, F_PALETTE } family_t;

typedef struct {
    const char* name;
    family_t family;
    int methods;
} iface_t;

static const iface_t k_export = { "ddraw.dll", F_EXPORT, 0 };
static const iface_t k_ddraw1 = { "IDirectDraw", F_DDRAW, 23 };
static const iface_t k_ddraw2 = { "IDirectDraw2", F_DDRAW, 24 };
static const iface_t k_ddraw4 = { "IDirectDraw4", F_DDRAW, 28 };
static const iface_t k_surface1 = { "IDirectDrawSurface", F_SURFACE, 36 };
static const iface_t k_surface2 = { "IDirectDrawSurface2", F_SURFACE, 39 };
static const iface_t k_surface3 = { "IDirectDrawSurface3", F_SURFACE, 40 };
static const iface_t k_surface4 = { "IDirectDrawSurface4", F_SURFACE, 45 };
static const iface_t k_clipper = { "IDirectDrawClipper", F_CLIPPER, 9 };
static const iface_t k_palette = { "IDirectDrawPalette", F_PALETTE, 7 };

static const char* const k_ddraw_methods[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
    "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface",
    "GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency",
    "GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem",
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier",
};
static const char* const k_surface_methods[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt",
    "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
    "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC",
    "GetFlipStatus", "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc",
    "Initialize", "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey",
    "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay",
    "UpdateOverlayZOrder", "GetDDInterface", "PageLock", "PageUnlock", "SetSurfaceDesc",
    "SetPrivateData", "GetPrivateData", "FreePrivateData", "GetUniquenessValue",
    "ChangeUniquenessValue",
};
static const char* const k_clipper_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize",
    "IsClipListChanged", "SetClipList", "SetHWnd",
};
static const char* const k_palette_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries",
};

typedef struct {
    GUID iid;
    const char* name;
    const iface_t* watch;
} known_iid_t;

static const known_iid_t k_iids[] = {
    { { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IDirectDraw", &k_ddraw1 },
    { { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } }, "IDirectDraw2", &k_ddraw2 },
    { { 0x9C59509A, 0x39BD, 0x11D1, { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 } }, "IDirectDraw4", &k_ddraw4 },
    { { 0x6C14DB81, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IDirectDrawSurface", &k_surface1 },
    { { 0x57805885, 0x6EEC, 0x11CF, { 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27 } }, "IDirectDrawSurface2", &k_surface2 },
    { { 0xDA044E00, 0x69B2, 0x11D0, { 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB } }, "IDirectDrawSurface3", &k_surface3 },
    { { 0x0B2B8630, 0xAD35, 0x11D0, { 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } }, "IDirectDrawSurface4", &k_surface4 },
    { { 0x6C14DB84, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IDirectDrawPalette", &k_palette },
    { { 0x6C14DB85, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IDirectDrawClipper", &k_clipper },
    { { 0x4B9F0EE0, 0x0D7E, 0x11D0, { 0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8 } }, "IDirectDrawColorControl", NULL },
    { { 0x69C11C3E, 0xB46B, 0x11D1, { 0xAD, 0x7A, 0x00, 0xC0, 0x4F, 0xC2, 0x9B, 0x4E } }, "IDirectDrawGammaControl", NULL },
    { { 0x3BBA0080, 0x2421, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } }, "IDirect3D", NULL },
    { { 0x6AAE1EC1, 0x662A, 0x11D0, { 0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A } }, "IDirect3D2", NULL },
    { { 0xBB223240, 0xE72B, 0x11D0, { 0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E } }, "IDirect3D3", NULL },
    { { 0x2CDCD9E0, 0x25A0, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } }, "IDirect3DTexture", NULL },
    { { 0x93281502, 0x8CF8, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } }, "IDirect3DTexture2", NULL },
    { { 0x84E63DE0, 0x46AA, 0x11CF, { 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E } }, "IID_IDirect3DHALDevice", NULL },
    { { 0xA4665C60, 0x2673, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } }, "IID_IDirect3DRGBDevice", NULL },
    { { 0x881949A1, 0xD6F3, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } }, "IID_IDirect3DMMXDevice", NULL },
    { { 0xF2086B20, 0x259F, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } }, "IID_IDirect3DRampDevice", NULL },
};

typedef enum {
    R_GENERIC, R_AMBIGUOUS, R_CREATE, R_ENUMERATE, R_QUERY_INTERFACE,
    R_CREATE_CLIPPER, R_CREATE_PALETTE, R_CREATE_SURFACE, R_GET_CAPS, R_GET_MODE,
    R_RESTORE_MODE, R_SET_COOP, R_SET_MODE3, R_SET_MODE5,
    R_BLT, R_BLTFAST, R_FLIP, R_GET_ATTACHED, R_GET_DESC, R_IS_LOST, R_LOCK, R_RESTORE, R_UNLOCK
} role_t;

typedef struct {
    uint32_t va;
    const iface_t* iface;
    const char* method;
    role_t role;
    int called;
} entry_t;

#define ENTRY_SLOTS 1024u
#define ENTRY_LIMIT (ENTRY_SLOTS * 3 / 4)
#define MAX_VTABLES 64
#define MAX_SURFACES 128
#define FAILURE_LINE_LIMIT 100
#define DETAIL_LINE_LIMIT 64
#define SURFACE_LINE_LIMIT 48
#define PRESENT_SUMMARY_AT 1000

typedef struct {
    uint32_t ptr;
    unsigned char primary, desc_logged;
} surface_t;

static entry_t s_entries[ENTRY_SLOTS];
static unsigned s_entry_count;
static struct { uint32_t vtbl; const iface_t* iface; } s_vtables[MAX_VTABLES];
static int s_vtable_count;
static surface_t s_surfaces[MAX_SURFACES];
static int s_surface_count;

static entry_t* s_pending;
static const entry_t* s_last;
static uint32_t s_last_hr, s_last_from;
static int s_last_returned;

static int s_failure_lines, s_detail_lines, s_surface_lines;
static unsigned long s_presents, s_locks, s_unlocks, s_blts, s_flips, s_failures;

static const char* method_name(const iface_t* f, int slot) {
    switch (f->family) {
    case F_DDRAW: return k_ddraw_methods[slot];
    case F_SURFACE: return k_surface_methods[slot];
    case F_CLIPPER: return k_clipper_methods[slot];
    case F_PALETTE: return k_palette_methods[slot];
    default: return "?";
    }
}

static role_t role_of(const iface_t* f, int slot) {
    if (slot == 0) return R_QUERY_INTERFACE;
    if (f->family == F_DDRAW) {
        switch (slot) {
        case 4: return R_CREATE_CLIPPER;
        case 5: return R_CREATE_PALETTE;
        case 6: return R_CREATE_SURFACE;
        case 11: return R_GET_CAPS;
        case 12: return R_GET_MODE;
        case 19: return R_RESTORE_MODE;
        case 20: return R_SET_COOP;
        case 21: return f->methods == k_ddraw1.methods ? R_SET_MODE3 : R_SET_MODE5;
        }
    } else if (f->family == F_SURFACE) {
        switch (slot) {
        case 5: return R_BLT;
        case 7: return R_BLTFAST;
        case 11: return R_FLIP;
        case 12: return R_GET_ATTACHED;
        case 22: return R_GET_DESC;
        case 24: return R_IS_LOST;
        case 25: return R_LOCK;
        case 27: return R_RESTORE;
        case 32: return R_UNLOCK;
        }
    }
    return R_GENERIC;
}

static unsigned slot_of(uint32_t va) { return (va * 2654435761u) >> 22; }

static entry_t* find_entry(uint32_t va) {
    for (unsigned i = slot_of(va), n = 0; n < ENTRY_SLOTS; i = (i + 1) & (ENTRY_SLOTS - 1), n++) {
        if (s_entries[i].va == va) return &s_entries[i];
        if (!s_entries[i].va) return NULL;
    }
    return NULL;
}

static void add_entry(uint32_t va, const iface_t* f, const char* method, role_t role) {
    entry_t* e;
    unsigned i;
    if (va < 0x10000u) return;
    e = find_entry(va);
    if (e) {
        if (e->role != role) e->role = R_AMBIGUOUS;
        return;
    }
    if (s_entry_count == ENTRY_LIMIT) {
        host_log("ddraw", "observer table full; later DirectDraw methods are not observed");
        s_entry_count++;
        return;
    }
    if (s_entry_count > ENTRY_LIMIT) return;
    for (i = slot_of(va); s_entries[i].va; i = (i + 1) & (ENTRY_SLOTS - 1)) {}
    s_entries[i].va = va;
    s_entries[i].iface = f;
    s_entries[i].method = method;
    s_entries[i].role = role;
    s_entry_count++;
}

/* AddRef and Release are left to run unobserved: they say nothing about the video path. */
static void watch_interface(uint32_t object, const iface_t* f) {
    uint32_t vtbl;
    if (!object || !f) return;
    vtbl = MEM32(object);
    for (int i = 0; i < s_vtable_count; i++)
        if (s_vtables[i].vtbl == vtbl && s_vtables[i].iface == f) return;
    if (s_vtable_count < MAX_VTABLES) {
        s_vtables[s_vtable_count].vtbl = vtbl;
        s_vtables[s_vtable_count++].iface = f;
    }
    for (int slot = 0; slot < f->methods; slot++)
        if (slot != 1 && slot != 2) add_entry(MEM32(vtbl + 4 * slot), f, method_name(f, slot), role_of(f, slot));
}

static surface_t* surface(uint32_t ptr, int add) {
    for (int i = 0; i < s_surface_count; i++)
        if (s_surfaces[i].ptr == ptr) return &s_surfaces[i];
    if (!add || !ptr || s_surface_count == MAX_SURFACES) return NULL;
    memset(&s_surfaces[s_surface_count], 0, sizeof s_surfaces[0]);
    s_surfaces[s_surface_count].ptr = ptr;
    return &s_surfaces[s_surface_count++];
}

static int is_primary(uint32_t ptr) {
    surface_t* s = surface(ptr, 0);
    return s && s->primary;
}

#define HR(name) { (uint32_t)(name), #name }
static const struct { uint32_t hr; const char* name; } k_hresults[] = {
    HR(DD_OK), HR(DDERR_GENERIC), HR(DDERR_INVALIDPARAMS), HR(DDERR_OUTOFMEMORY), HR(DDERR_UNSUPPORTED),
    HR(E_NOINTERFACE), HR(DDERR_INVALIDOBJECT), HR(DDERR_INVALIDMODE), HR(DDERR_INVALIDPIXELFORMAT),
    HR(DDERR_INVALIDCAPS), HR(DDERR_NOEXCLUSIVEMODE), HR(DDERR_EXCLUSIVEMODEALREADYSET),
    HR(DDERR_HWNDALREADYSET), HR(DDERR_HWNDSUBCLASSED), HR(DDERR_NOCOOPERATIVELEVELSET),
    HR(DDERR_SURFACELOST), HR(DDERR_SURFACEBUSY), HR(DDERR_WASSTILLDRAWING), HR(DDERR_OUTOFVIDEOMEMORY),
    HR(DDERR_NOFLIPHW), HR(DDERR_NOTFLIPPABLE), HR(DDERR_UNSUPPORTEDMODE),
    HR(DDERR_PRIMARYSURFACEALREADYEXISTS), HR(DDERR_NODIRECTDRAWHW), HR(DDERR_NOTLOCKED),
    HR(DDERR_CANTLOCKSURFACE), HR(DDERR_NOBLTHW), HR(DDERR_INVALIDRECT), HR(DDERR_NOTFOUND),
    HR(DDERR_CANTCREATEDC), HR(DDERR_DCALREADYCREATED), HR(DDERR_WRONGMODE), HR(DDERR_NOCLIPPERATTACHED),
    HR(DDERR_CLIPPERISUSINGHWND), HR(DDERR_NOHWND), HR(DDERR_NOPALETTEATTACHED),
    HR(DDERR_DIRECTDRAWALREADYCREATED), HR(DDERR_NODIRECTDRAWSUPPORT), HR(DDERR_INCOMPATIBLEPRIMARY),
    HR(DDERR_NOTPALETTIZED), HR(DDERR_INVALIDDIRECTDRAWGUID), HR(DDERR_NOTINITIALIZED),
};

const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap) {
    for (int i = 0; i < (int)(sizeof k_hresults / sizeof k_hresults[0]); i++)
        if (k_hresults[i].hr == hr) return k_hresults[i].name;
    _snprintf(buf, cap - 1, "HRESULT 0x%08X", hr);
    buf[cap - 1] = 0;
    return buf;
}

typedef struct { DWORD bit; const char* name; } flag_name_t;

static const flag_name_t k_scaps[] = {
    { DDSCAPS_PRIMARYSURFACE, "PRIMARYSURFACE" }, { DDSCAPS_BACKBUFFER, "BACKBUFFER" },
    { DDSCAPS_FRONTBUFFER, "FRONTBUFFER" }, { DDSCAPS_FLIP, "FLIP" }, { DDSCAPS_COMPLEX, "COMPLEX" },
    { DDSCAPS_OFFSCREENPLAIN, "OFFSCREENPLAIN" }, { DDSCAPS_SYSTEMMEMORY, "SYSTEMMEMORY" },
    { DDSCAPS_VIDEOMEMORY, "VIDEOMEMORY" }, { DDSCAPS_LOCALVIDMEM, "LOCALVIDMEM" },
    { DDSCAPS_NONLOCALVIDMEM, "NONLOCALVIDMEM" }, { DDSCAPS_3DDEVICE, "3DDEVICE" },
    { DDSCAPS_TEXTURE, "TEXTURE" }, { DDSCAPS_ZBUFFER, "ZBUFFER" }, { DDSCAPS_MIPMAP, "MIPMAP" },
    { DDSCAPS_OWNDC, "OWNDC" }, { DDSCAPS_MODEX, "MODEX" }, { DDSCAPS_PALETTE, "PALETTE" },
};
static const flag_name_t k_coop[] = {
    { DDSCL_FULLSCREEN, "FULLSCREEN" }, { DDSCL_ALLOWREBOOT, "ALLOWREBOOT" },
    { DDSCL_NOWINDOWCHANGES, "NOWINDOWCHANGES" }, { DDSCL_NORMAL, "NORMAL" },
    { DDSCL_EXCLUSIVE, "EXCLUSIVE" }, { DDSCL_ALLOWMODEX, "ALLOWMODEX" },
    { DDSCL_SETFOCUSWINDOW, "SETFOCUSWINDOW" }, { DDSCL_SETDEVICEWINDOW, "SETDEVICEWINDOW" },
    { DDSCL_CREATEDEVICEWINDOW, "CREATEDEVICEWINDOW" }, { DDSCL_MULTITHREADED, "MULTITHREADED" },
    { DDSCL_FPUSETUP, "FPUSETUP" }, { DDSCL_FPUPRESERVE, "FPUPRESERVE" },
};

static void flags_text(DWORD value, const flag_name_t* names, int count, char* out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < count && len + 1 < cap; i++) {
        if (!(value & names[i].bit)) continue;
        int n = _snprintf(out + len, cap - 1 - len, "%s%s", len ? "|" : "", names[i].name);
        if (n < 0) break;
        len += (size_t)n;
        value &= ~names[i].bit;
    }
    if (value && len + 1 < cap) _snprintf(out + len, cap - 1 - len, "%s0x%lX", len ? "|" : "", value);
    if (!out[0]) strcpy(out, "0");
    out[cap - 1] = 0;
}

static void pixel_format_text(const DDPIXELFORMAT* pf, char* out, size_t cap) {
    if (pf->dwFlags & DDPF_PALETTEINDEXED8)
        _snprintf(out, cap - 1, "8 bpp palettized");
    else if (pf->dwFlags & DDPF_RGB)
        _snprintf(out, cap - 1, "%lu bpp RGB %lX/%lX/%lX", pf->dwRGBBitCount, pf->dwRBitMask,
                  pf->dwGBitMask, pf->dwBBitMask);
    else
        _snprintf(out, cap - 1, "pixel format flags 0x%lX, %lu bits", pf->dwFlags, pf->dwRGBBitCount);
    out[cap - 1] = 0;
}

/* DDSURFACEDESC and DDSURFACEDESC2 share every field read here. */
static const DDSURFACEDESC* surface_desc(uint32_t ptr) {
    const DDSURFACEDESC* d = (const DDSURFACEDESC*)(uintptr_t)ptr;
    if (!d || (d->dwSize != sizeof(DDSURFACEDESC) && d->dwSize != sizeof(DDSURFACEDESC2))) return NULL;
    return d;
}

static void append(char* out, size_t cap, const char* fmt, ...) {
    size_t len = strlen(out);
    va_list ap;
    if (len + 1 >= cap) return;
    va_start(ap, fmt);
    _vsnprintf(out + len, cap - 1 - len, fmt, ap);
    va_end(ap);
    out[cap - 1] = 0;
}

static void desc_text(const DDSURFACEDESC* d, char* out, size_t cap) {
    char caps[192] = "", format[96] = "";
    if (d->dwFlags & DDSD_CAPS) flags_text(d->ddsCaps.dwCaps, k_scaps, (int)(sizeof k_scaps / sizeof k_scaps[0]), caps, sizeof caps);
    if (d->dwFlags & DDSD_PIXELFORMAT) pixel_format_text(&d->ddpfPixelFormat, format, sizeof format);
    out[0] = 0;
    append(out, cap, "%s", caps[0] ? caps : "caps unspecified");
    if ((d->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) == (DDSD_WIDTH | DDSD_HEIGHT))
        append(out, cap, ", %lux%lu", d->dwWidth, d->dwHeight);
    if (d->dwFlags & DDSD_PITCH) append(out, cap, ", pitch %ld", d->lPitch);
    if (format[0]) append(out, cap, ", %s", format);
    if (d->dwFlags & DDSD_BACKBUFFERCOUNT) append(out, cap, ", %lu back buffers", d->dwBackBufferCount);
}

static int failed(uint32_t hr) { return (hr & 0x80000000u) != 0; }

static int detail_allowed(void) { return s_detail_lines++ < DETAIL_LINE_LIMIT; }

static void present(const entry_t* e, uint32_t target, uint32_t caller) {
    char used[1024];
    size_t len = 0;
    if (++s_presents != 1 && s_presents != PRESENT_SUMMARY_AT) return;
    if (s_presents == 1)
        host_log("ddraw", "first frame presented: %s::%s on surface 0x%08X, from sub_%08X", e->iface->name,
                 e->method, target, caller);
    host_log("ddraw", "%lu presents: %lu locks, %lu unlocks, %lu blts, %lu flips, %lu failed calls", s_presents,
             s_locks, s_unlocks, s_blts, s_flips, s_failures);
    used[0] = 0;
    for (unsigned i = 0; i < ENTRY_SLOTS && len + 48 < sizeof used; i++) {
        const entry_t* u = &s_entries[i];
        if (!u->va || !u->called) continue;
        int n = _snprintf(used + len, sizeof used - 1 - len, "%s%s::%s", len ? " " : "", u->iface->name, u->method);
        if (n > 0) len += (size_t)n;
    }
    used[sizeof used - 1] = 0;
    host_log("ddraw", "methods used so far: %s", used);
}

static int report_create(const uint32_t* a, uint32_t hr, const char* hrs) {
    const GUID* g = (const GUID*)(uintptr_t)a[0];
    uint32_t object = (!failed(hr) && a[1]) ? MEM32(a[1]) : 0;
    char device[64];
    if (a[0] == 0) strcpy(device, "primary display driver");
    else if (a[0] == (uint32_t)(uintptr_t)DDCREATE_EMULATIONONLY) strcpy(device, "emulation only");
    else if (a[0] == (uint32_t)(uintptr_t)DDCREATE_HARDWAREONLY) strcpy(device, "hardware only");
    else
        _snprintf(device, sizeof device - 1, "device {%08lX-%04X-%04X-%02X%02X-...}", g->Data1, g->Data2,
                  g->Data3, g->Data4[0], g->Data4[1]);
    device[sizeof device - 1] = 0;
    host_log("ddraw", "DirectDrawCreate(%s) -> %s, object 0x%08X", device, hrs, object);
    watch_interface(object, &k_ddraw1);
    return 1;
}

static int report_query_interface(const entry_t* e, const uint32_t* a, uint32_t hr, const char* hrs) {
    const GUID* iid = (const GUID*)(uintptr_t)a[1];
    const known_iid_t* k = NULL;
    uint32_t object = (!failed(hr) && a[2]) ? MEM32(a[2]) : 0;
    for (int i = 0; iid && i < (int)(sizeof k_iids / sizeof k_iids[0]); i++)
        if (IsEqualGUID(iid, &k_iids[i].iid)) k = &k_iids[i];
    if (detail_allowed()) {
        if (k)
            host_log("ddraw", "%s::QueryInterface(%s) on 0x%08X -> %s, 0x%08X", e->iface->name, k->name, a[0], hrs, object);
        else if (iid)
            host_log("ddraw", "%s::QueryInterface({%08lX-%04X-%04X-...}) on 0x%08X -> %s, 0x%08X", e->iface->name,
                     iid->Data1, iid->Data2, iid->Data3, a[0], hrs, object);
    }
    if (object && k) {
        watch_interface(object, k->watch);
        if (k->watch && k->watch->family == F_SURFACE && is_primary(a[0])) {
            surface_t* s = surface(object, 1);
            if (s) s->primary = 1;
        }
    }
    return 1;
}

static int report_caps(const uint32_t* a, uint32_t hr, const char* hrs) {
    static int reported;
    const DDCAPS* c = (const DDCAPS*)(uintptr_t)a[1];
    if (reported) return 0;
    reported = 1;
    if (failed(hr) || !c || c->dwSize < offsetof(DDCAPS, dwVidMemFree) + sizeof c->dwVidMemFree) {
        host_log("ddraw", "GetCaps -> %s", hrs);
        return 1;
    }
    host_log("ddraw", "GetCaps: caps 0x%08lX, caps2 0x%08lX; DDCAPS2_CERTIFIED %s, DDCAPS_NOHARDWARE %s; "
             "video memory %lu KB total, %lu KB free", c->dwCaps, c->dwCaps2,
             (c->dwCaps2 & DDCAPS2_CERTIFIED) ? "set" : "NOT set (the game's \"Direct Draw NOT certified\" dialog)",
             (c->dwCaps & DDCAPS_NOHARDWARE) ? "set" : "not set", c->dwVidMemTotal / 1024, c->dwVidMemFree / 1024);
    return 1;
}

static int report_display_mode(const uint32_t* a, uint32_t hr, const char* hrs) {
    static uint32_t last_w, last_h, last_bpp;
    const DDSURFACEDESC* d = surface_desc(a[1]);
    if (failed(hr) || !d) {
        host_log("ddraw", "GetDisplayMode -> %s", hrs);
        return 1;
    }
    if (d->dwWidth == last_w && d->dwHeight == last_h && d->ddpfPixelFormat.dwRGBBitCount == last_bpp) return 1;
    last_w = d->dwWidth;
    last_h = d->dwHeight;
    last_bpp = d->ddpfPixelFormat.dwRGBBitCount;
    host_log("ddraw", "GetDisplayMode: effective mode %lux%lu, %lu bpp", d->dwWidth, d->dwHeight,
             d->ddpfPixelFormat.dwRGBBitCount);
    return 1;
}

static int report_set_mode(const entry_t* e, const uint32_t* a, uint32_t hr, const char* hrs) {
    (void)hr;
    if (e->role == R_SET_MODE5)
        host_log("ddraw", "%s::SetDisplayMode %ux%ux%u, refresh %u, flags 0x%X -> %s", e->iface->name, a[1], a[2],
                 a[3], a[4], a[5], hrs);
    else
        host_log("ddraw", "%s::SetDisplayMode %ux%ux%u -> %s", e->iface->name, a[1], a[2], a[3], hrs);
    return 1;
}

static int report_create_surface(const entry_t* e, const uint32_t* a, uint32_t hr, const char* hrs, uint32_t caller) {
    const DDSURFACEDESC* d = surface_desc(a[1]);
    uint32_t object = (!failed(hr) && a[2]) ? MEM32(a[2]) : 0;
    char text[384] = "unreadable description";
    if (d) desc_text(d, text, sizeof text);
    if (failed(hr) || s_surface_lines++ < SURFACE_LINE_LIMIT)
        host_log("ddraw", "CreateSurface(%s) -> %s, surface 0x%08X, from sub_%08X", text, hrs, object, caller);
    if (!object) return 1;
    watch_interface(object, e->iface->methods == k_ddraw4.methods ? &k_surface4 : &k_surface1);
    if (d && (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        surface_t* s = surface(object, 1);
        if (s) s->primary = 1;
    }
    return 1;
}

static int report_surface_desc(const uint32_t* a, uint32_t hr) {
    const DDSURFACEDESC* d = surface_desc(a[1]);
    surface_t* s = surface(a[0], 1);
    char text[384];
    if (failed(hr) || !d || !s || s->desc_logged) return 0;
    s->desc_logged = 1;
    desc_text(d, text, sizeof text);
    host_log("ddraw", "surface 0x%08X%s: %s", a[0], s->primary ? " (primary)" : "", text);
    return 1;
}

static int report_lock(const uint32_t* a, uint32_t hr) {
    s_locks++;
    return !failed(hr) && surface_desc(a[2]);
}

static int report_details(const entry_t* e, const uint32_t* a, uint32_t hr, uint32_t caller) {
    char buf[32], text[192];
    const char* hrs = ddraw_hresult_text(hr, buf, sizeof buf);
    switch (e->role) {
    case R_CREATE: return report_create(a, hr, hrs);
    case R_ENUMERATE:
        if (e->called) return 0;
        host_log("ddraw", "DirectDrawEnumerateA -> %s", hrs);
        return 1;
    case R_QUERY_INTERFACE: return report_query_interface(e, a, hr, hrs);
    case R_GET_CAPS: return report_caps(a, hr, hrs);
    case R_GET_MODE: return report_display_mode(a, hr, hrs);
    case R_SET_MODE3:
    case R_SET_MODE5: return report_set_mode(e, a, hr, hrs);
    case R_SET_COOP:
        if (!detail_allowed()) return 0;
        flags_text(a[2], k_coop, (int)(sizeof k_coop / sizeof k_coop[0]), text, sizeof text);
        host_log("ddraw", "%s::SetCooperativeLevel(hwnd 0x%08X, %s) -> %s", e->iface->name, a[1], text, hrs);
        return 1;
    case R_RESTORE_MODE:
        if (!detail_allowed()) return 0;
        host_log("ddraw", "%s::RestoreDisplayMode -> %s", e->iface->name, hrs);
        return 1;
    case R_CREATE_SURFACE: return report_create_surface(e, a, hr, hrs, caller);
    case R_CREATE_CLIPPER:
    case R_CREATE_PALETTE: {
        uint32_t out = e->role == R_CREATE_CLIPPER ? a[2] : a[3];
        uint32_t object = (!failed(hr) && out) ? MEM32(out) : 0;
        watch_interface(object, e->role == R_CREATE_CLIPPER ? &k_clipper : &k_palette);
        if (!failed(hr) && !detail_allowed()) return 1;
        host_log("ddraw", "%s::%s(flags 0x%X) -> %s, 0x%08X", e->iface->name, e->method, a[1], hrs, object);
        return 1;
    }
    case R_GET_ATTACHED: {
        uint32_t object = (!failed(hr) && a[2]) ? MEM32(a[2]) : 0;
        watch_interface(object, e->iface);
        if (!failed(hr) && !detail_allowed()) return 1;
        host_log("ddraw", "%s::GetAttachedSurface(caps 0x%08X) on 0x%08X -> %s, 0x%08X", e->iface->name,
                 a[1] ? MEM32(a[1]) : 0, a[0], hrs, object);
        return 1;
    }
    case R_GET_DESC: return report_surface_desc(a, hr);
    case R_LOCK: return report_lock(a, hr);
    case R_UNLOCK:
        s_unlocks++;
        return 0;
    case R_FLIP:
        s_flips++;
        if (!failed(hr)) present(e, a[0], caller);
        return 0;
    case R_BLT:
    case R_BLTFAST:
        s_blts++;
        if (!failed(hr) && is_primary(a[0])) present(e, a[0], caller);
        return 0;
    case R_RESTORE:
        if (!detail_allowed()) return 0;
        host_log("ddraw", "%s::Restore on 0x%08X -> %s", e->iface->name, a[0], hrs);
        return 1;
    case R_IS_LOST:
    default:
        return 0;
    }
}

static void after_call(entry_t* e, const uint32_t* a, uint32_t hr, uint32_t caller) {
    int logged = report_details(e, a, hr, caller);
    if (failed(hr)) s_failures++;
    if (!logged && failed(hr) && s_failure_lines < FAILURE_LINE_LIMIT) {
        char buf[32];
        host_log("ddraw", "%s::%s on 0x%08X FAILED -> %s, from sub_%08X", e->iface->name, e->method, a[0],
                 ddraw_hresult_text(hr, buf, sizeof buf), caller);
        if (++s_failure_lines == FAILURE_LINE_LIMIT)
            host_log("ddraw", "failure line limit reached; later DirectDraw failures are only counted");
    } else if (!logged && !e->called && (e->role == R_GENERIC || e->role == R_AMBIGUOUS)) {
        char buf[32];
        host_log("ddraw", "first call to %s::%s%s -> %s", e->iface->name, e->method,
                 e->role == R_AMBIGUOUS ? " (address shared with another method)" : "", ddraw_hresult_text(hr, buf, sizeof buf));
    }
    e->called = 1;
}

#define OBSERVED_ARGS 6

/* Entered from the dispatch with the machine lock held, like any import bridge; native32's
 * own bridge makes the call, so the purge and the guest's last error are handled as always. */
static void observed_call(void) {
    entry_t* e = s_pending;
    uint32_t a[OBSERVED_ARGS];
    uint32_t caller = g_cur_func;
    memcpy(a, (const void*)(uintptr_t)(g_esp + 4), sizeof a);
    s_last = e;
    s_last_from = caller;
    s_last_returned = 0;
    recomp_lookup_import(e->va)();
    s_last_hr = g_eax;
    s_last_returned = 1;
    after_call(e, a, g_eax, caller);
}

unsigned ddraw_trace_entry_count(void) { return s_entry_count; }

recomp_func_t ddraw_trace_lookup(uint32_t va) {
    entry_t* e;
    if (!s_entry_count || va < 0x10000u) return NULL;
    e = find_entry(va);
    if (!e) return NULL;
    s_pending = e;
    return observed_call;
}

int ddraw_trace_name(uint32_t va, char* out, int cap) {
    const entry_t* e = (s_entry_count && va >= 0x10000u) ? find_entry(va) : NULL;
    if (!e || cap <= 0) return 0;
    _snprintf(out, (size_t)cap - 1, "%s::%s", e->iface->name, e->method);
    out[cap - 1] = 0;
    return 1;
}

int ddraw_trace_last_call(char* out, int cap) {
    char buf[32];
    if (!s_last || cap <= 0) return 0;
    if (s_last_returned)
        _snprintf(out, (size_t)cap - 1, "%s::%s from sub_%08X -> %s", s_last->iface->name, s_last->method,
                  s_last_from, ddraw_hresult_text(s_last_hr, buf, sizeof buf));
    else
        _snprintf(out, (size_t)cap - 1, "%s::%s from sub_%08X (had not returned)", s_last->iface->name,
                  s_last->method, s_last_from);
    out[cap - 1] = 0;
    return 1;
}

void ddraw_trace_init(void) {
    char path[MAX_PATH];
    DEVMODEA mode;
    HMODULE ddraw = GetModuleHandleA("ddraw.dll");
    memset(&mode, 0, sizeof mode);
    mode.dmSize = sizeof mode;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode))
        host_log("ddraw", "desktop %lux%lu, %lu bpp, %lu Hz", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel,
                 mode.dmDisplayFrequency);
    if (!ddraw) {
        host_log("ddraw", "ddraw.dll is not loaded after binding; DirectDraw calls are not observed");
        return;
    }
    if (GetModuleFileNameA(ddraw, path, sizeof path)) host_log("ddraw", "ddraw.dll is %s", path);
    add_entry((uint32_t)(uintptr_t)GetProcAddress(ddraw, "DirectDrawCreate"), &k_export, "DirectDrawCreate", R_CREATE);
    add_entry((uint32_t)(uintptr_t)GetProcAddress(ddraw, "DirectDrawEnumerateA"), &k_export, "DirectDrawEnumerateA",
              R_ENUMERATE);
}
