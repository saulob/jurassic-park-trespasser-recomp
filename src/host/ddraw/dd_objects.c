/* IDirectDrawClipper and IDirectDrawPalette. */
#include "ddraw_compat.h"
#include "host.h"

#include <stdlib.h>
#include <string.h>

static const IDirectDrawClipperVtbl s_clipper_vtbl;
static const IDirectDrawPaletteVtbl s_palette_vtbl;

dd_clipper_t* dd_clipper_from(void* iface) {
    return (iface && ((dd_face_t*)iface)->vtbl == &s_clipper_vtbl) ? DD_OWNER(dd_clipper_t, iface) : NULL;
}

dd_palette_t* dd_palette_from(void* iface) {
    return (iface && ((dd_face_t*)iface)->vtbl == &s_palette_vtbl) ? DD_OWNER(dd_palette_t, iface) : NULL;
}

void dd_clipper_addref(dd_clipper_t* c) { InterlockedIncrement(&c->refs); }

void dd_clipper_release(dd_clipper_t* c) {
    if (InterlockedDecrement(&c->refs) == 0) free(c);
}

void dd_palette_addref(dd_palette_t* p) { InterlockedIncrement(&p->refs); }

void dd_palette_release(dd_palette_t* p) {
    if (InterlockedDecrement(&p->refs) == 0) free(p);
}

HRESULT dd_create_clipper(DWORD flags, IDirectDrawClipper** out) {
    dd_clipper_t* c;
    if (!out || flags) return DDERR_INVALIDPARAMS;
    c = (dd_clipper_t*)calloc(1, sizeof *c);
    if (!c) return DDERR_OUTOFMEMORY;
    c->face.vtbl = &s_clipper_vtbl;
    c->face.owner = c;
    c->face.version = 1;
    c->refs = 1;
    *out = (IDirectDrawClipper*)&c->face;
    return DD_OK;
}

HRESULT dd_create_palette(DWORD flags, const PALETTEENTRY* entries, IDirectDrawPalette** out) {
    dd_palette_t* p;
    if (!out || !entries) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (!(flags & DDPCAPS_8BIT)) {
        dd_log("CreatePalette flags 0x%lX: only 8-bit palettes are emulated", flags);
        return DDERR_UNSUPPORTED;
    }
    p = (dd_palette_t*)calloc(1, sizeof *p);
    if (!p) return DDERR_OUTOFMEMORY;
    p->face.vtbl = &s_palette_vtbl;
    p->face.owner = p;
    p->face.version = 1;
    p->refs = 1;
    p->caps = flags;
    memcpy(p->entries, entries, sizeof p->entries);
    *out = (IDirectDrawPalette*)&p->face;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE c_QueryInterface(IDirectDrawClipper* This, REFIID riid, LPVOID* out) {
    DD_METHOD("IDirectDrawClipper::QueryInterface");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDirectDrawClipper)) DD_RETURN(E_NOINTERFACE);
    dd_clipper_addref(DD_OWNER(dd_clipper_t, This));
    *out = This;
    DD_RETURN(DD_OK);
}

static ULONG STDMETHODCALLTYPE c_AddRef(IDirectDrawClipper* This) {
    return (ULONG)InterlockedIncrement(&DD_OWNER(dd_clipper_t, This)->refs);
}

static ULONG STDMETHODCALLTYPE c_Release(IDirectDrawClipper* This) {
    dd_clipper_t* c = DD_OWNER(dd_clipper_t, This);
    LONG refs = InterlockedDecrement(&c->refs);
    if (refs == 0) free(c);
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE c_GetClipList(IDirectDrawClipper* This, LPRECT rect, LPRGNDATA list, LPDWORD size) {
    (void)This; (void)rect; (void)list; (void)size;
    DD_UNSUPPORTED("IDirectDrawClipper::GetClipList", "clipping follows the window's client rectangle only", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE c_GetHWnd(IDirectDrawClipper* This, HWND* window) {
    dd_clipper_t* c = DD_OWNER(dd_clipper_t, This);
    DD_METHOD("IDirectDrawClipper::GetHWnd");
    if (!window) DD_RETURN(DDERR_INVALIDPARAMS);
    *window = c->window;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE c_Initialize(IDirectDrawClipper* This, LPDIRECTDRAW dd, DWORD flags) {
    (void)This; (void)dd; (void)flags;
    DD_METHOD("IDirectDrawClipper::Initialize");
    DD_RETURN(DDERR_ALREADYINITIALIZED);
}

static HRESULT STDMETHODCALLTYPE c_IsClipListChanged(IDirectDrawClipper* This, BOOL* changed) {
    (void)This;
    DD_METHOD("IDirectDrawClipper::IsClipListChanged");
    if (!changed) DD_RETURN(DDERR_INVALIDPARAMS);
    *changed = FALSE;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE c_SetClipList(IDirectDrawClipper* This, LPRGNDATA list, DWORD flags) {
    (void)This; (void)list; (void)flags;
    DD_UNSUPPORTED("IDirectDrawClipper::SetClipList", "clipping follows the window's client rectangle only", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE c_SetHWnd(IDirectDrawClipper* This, DWORD flags, HWND window) {
    dd_clipper_t* c = DD_OWNER(dd_clipper_t, This);
    (void)flags;
    DD_METHOD("IDirectDrawClipper::SetHWnd");
    if (window && !IsWindow(window)) DD_RETURN(DDERR_INVALIDPARAMS);
    c->window = window;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE p_QueryInterface(IDirectDrawPalette* This, REFIID riid, LPVOID* out) {
    DD_METHOD("IDirectDrawPalette::QueryInterface");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IDirectDrawPalette)) DD_RETURN(E_NOINTERFACE);
    dd_palette_addref(DD_OWNER(dd_palette_t, This));
    *out = This;
    DD_RETURN(DD_OK);
}

static ULONG STDMETHODCALLTYPE p_AddRef(IDirectDrawPalette* This) {
    return (ULONG)InterlockedIncrement(&DD_OWNER(dd_palette_t, This)->refs);
}

static ULONG STDMETHODCALLTYPE p_Release(IDirectDrawPalette* This) {
    dd_palette_t* p = DD_OWNER(dd_palette_t, This);
    LONG refs = InterlockedDecrement(&p->refs);
    if (refs == 0) free(p);
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE p_GetCaps(IDirectDrawPalette* This, LPDWORD caps) {
    DD_METHOD("IDirectDrawPalette::GetCaps");
    if (!caps) DD_RETURN(DDERR_INVALIDPARAMS);
    *caps = DD_OWNER(dd_palette_t, This)->caps;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE p_GetEntries(IDirectDrawPalette* This, DWORD flags, DWORD base, DWORD count,
                                              LPPALETTEENTRY entries) {
    dd_palette_t* p = DD_OWNER(dd_palette_t, This);
    (void)flags;
    DD_METHOD("IDirectDrawPalette::GetEntries");
    if (!entries || base >= 256 || count > 256 - base) DD_RETURN(DDERR_INVALIDPARAMS);
    memcpy(entries, p->entries + base, count * sizeof *entries);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE p_Initialize(IDirectDrawPalette* This, LPDIRECTDRAW dd, DWORD flags, LPPALETTEENTRY entries) {
    (void)This; (void)dd; (void)flags; (void)entries;
    DD_METHOD("IDirectDrawPalette::Initialize");
    DD_RETURN(DDERR_ALREADYINITIALIZED);
}

/* Surfaces copy the colours into their DIB when the palette is attached; 8-bit surfaces are
 * never presented, so later changes need not reach them. */
static HRESULT STDMETHODCALLTYPE p_SetEntries(IDirectDrawPalette* This, DWORD flags, DWORD base, DWORD count,
                                              LPPALETTEENTRY entries) {
    dd_palette_t* p = DD_OWNER(dd_palette_t, This);
    (void)flags;
    DD_METHOD("IDirectDrawPalette::SetEntries");
    if (!entries || base >= 256 || count > 256 - base) DD_RETURN(DDERR_INVALIDPARAMS);
    memcpy(p->entries + base, entries, count * sizeof *entries);
    DD_RETURN(DD_OK);
}

static const IDirectDrawClipperVtbl s_clipper_vtbl = {
    .QueryInterface = c_QueryInterface, .AddRef = c_AddRef, .Release = c_Release, .GetClipList = c_GetClipList,
    .GetHWnd = c_GetHWnd, .Initialize = c_Initialize, .IsClipListChanged = c_IsClipListChanged,
    .SetClipList = c_SetClipList, .SetHWnd = c_SetHWnd,
};

static const IDirectDrawPaletteVtbl s_palette_vtbl = {
    .QueryInterface = p_QueryInterface, .AddRef = p_AddRef, .Release = p_Release, .GetCaps = p_GetCaps,
    .GetEntries = p_GetEntries, .Initialize = p_Initialize, .SetEntries = p_SetEntries,
};

C_ASSERT(sizeof(IDirectDrawClipperVtbl) == 9 * sizeof(void*));
C_ASSERT(sizeof(IDirectDrawPaletteVtbl) == 7 * sizeof(void*));

int dd_object_vtables_complete(void) {
    void* const* clipper = (void* const*)&s_clipper_vtbl;
    void* const* palette = (void* const*)&s_palette_vtbl;
    for (size_t i = 0; i < sizeof s_clipper_vtbl / sizeof(void*); i++)
        if (!clipper[i]) return 0;
    for (size_t i = 0; i < sizeof s_palette_vtbl / sizeof(void*); i++)
        if (!palette[i]) return 0;
    return 1;
}
