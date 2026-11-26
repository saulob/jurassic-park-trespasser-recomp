#ifndef TRESPASSER_DDRAW_COMPAT_H
#define TRESPASSER_DDRAW_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stddef.h>
#include <stdint.h>

#include "d3d_translate.h"

/* Every interface pointer handed to the game is a face: lpVtbl first, as COM requires. */
typedef struct {
    const void* vtbl;
    void* owner;
    int version;
} dd_face_t;

#define DD_OWNER(type, This) ((type*)((dd_face_t*)(This))->owner)
#define DD_VERSION(This) (((dd_face_t*)(This))->version)

typedef struct dd_device dd_device_t;
typedef struct dd_surface dd_surface_t;
typedef struct dd_clipper dd_clipper_t;
typedef struct dd_palette dd_palette_t;

struct dd_device {
    dd_face_t dd1, dd2, dd4, d3d3;
    LONG refs;
    int released;
    int surfaces;
    HWND window;
    DWORD coop;
    DWORD mode_width, mode_height;
    int mode_set;
    dd_surface_t* primary;
    DDGAMMARAMP gamma;
    int gamma_changed;
};

struct dd_surface {
    dd_face_t s1, s2, s3, s4, gamma, tex2;
    LONG refs;
    dd_device_t* device;
    dd_surface_t* root;
    dd_surface_t* next;
    int orphaned;
    int lost;
    unsigned display_generation;
    DWORD caps;
    DWORD back_buffers;
    DWORD width, height;
    LONG pitch;
    DDPIXELFORMAT format;
    BYTE* bits;
    HBITMAP dib;
    HANDLE section;
    BYTE* view;
    HDC dc;
    HGDIOBJ dc_previous;
    int dc_out;
    int locked;
    int has_src_key, has_dst_key;
    DDCOLORKEY src_key, dst_key;
    dd_clipper_t* clipper;
    dd_palette_t* palette;
    int lock_write;
    DWORD lock_flags;
    RECT lock_rect;
    BYTE* heap_bits;
    dd_surface_t* zbuffer;
    void* gpu_texture;
    d3dt_rect_t texture_dirty;
    struct d3dr_target* target;
    int front_from_gpu;
    unsigned long cpu_writes;
    int watched;
    int arena; /* heap_bits come from page_arena.c */
    /* Only a Lock hands the guest a pointer into the pixels, so only then can they change unannounced. */
    int exposed;
    unsigned long long watch_frame;
    int override_id; /* jpt_texhw.h */
    int guard_alias;  /* page_guard.c alias of section; moves with it at Flip */
    int lock_alias;   /* this Lock handed out the alias */
};

struct dd_clipper {
    dd_face_t face;
    LONG refs;
    HWND window;
};

struct dd_palette {
    dd_face_t face;
    LONG refs;
    DWORD caps;
    PALETTEENTRY entries[256];
};

typedef struct dd_method {
    const char* name;
    unsigned long calls;
    /* One call in eight is timed (from dd_begin, before the layer's lock, to the return). */
    long long started;
    double sampled_ms;
    struct dd_method* next;
} dd_method_t;

void dd_lock(void);
void dd_unlock(void);
void dd_begin(dd_method_t* m);
HRESULT dd_done(dd_method_t* m, HRESULT hr);
void dd_returned(dd_method_t* m, HRESULT hr);
HRESULT dd_unsupported(dd_method_t* m, const char* reason, HRESULT hr);
void dd_log(const char* fmt, ...);
/* The layer's methods by time (sampled), once at exit. */
void dd_methods_summary(unsigned long long hardware_frames);

/* A method body runs under the layer's lock; the macros keep begin, unlock and done paired. */
#define DD_BEGIN(method) dd_method_t* dd_method_ = (method); dd_begin(dd_method_); dd_lock()
#define DD_METHOD(label) static dd_method_t dd_label_ = { label, 0 }; DD_BEGIN(&dd_label_)
#define DD_RETURN(hr) do { HRESULT dd_hr_ = (hr); dd_unlock(); return dd_done(dd_method_, dd_hr_); } while (0)
#define DD_UNSUPPORTED(label, reason, hr) do { static dd_method_t dd_method_ = { label, 0 }; dd_begin(&dd_method_); \
    return dd_unsupported(&dd_method_, reason, hr); } while (0)

void dd_format_rgb565(DDPIXELFORMAT* pf);
int dd_format_supported(const DDPIXELFORMAT* pf);
int dd_format_bytes(const DDPIXELFORMAT* pf);
int dd_format_equal(const DDPIXELFORMAT* a, const DDPIXELFORMAT* b);

void dd_display_size(const dd_device_t* d, DWORD* width, DWORD* height);
/* Changes with every new virtual display size; a surface carries the one it was made or restored under. */
unsigned dd_display_generation(void);
HRESULT dd_create_surface(dd_device_t* d, const DDSURFACEDESC* desc, int version, void** out);
void* dd_surface_face(dd_surface_t* s, int version);
void dd_surface_added(dd_device_t* d);
void dd_surface_removed(dd_device_t* d);
void dd_device_mode_changed(dd_device_t* d);
void dd_present(dd_surface_t* front, int vsync);
dd_surface_t* dd_surface_from(void* iface);
void dd_surface_addref(dd_surface_t* s);
void dd_surface_release(dd_surface_t* s);
int dd_surface_is_zbuffer(const dd_surface_t* s);

HRESULT dd_create_clipper(DWORD flags, IDirectDrawClipper** out);
HRESULT dd_create_palette(DWORD flags, const PALETTEENTRY* entries, IDirectDrawPalette** out);
void dd_clipper_addref(dd_clipper_t* c);
void dd_clipper_release(dd_clipper_t* c);
void dd_palette_addref(dd_palette_t* p);
void dd_palette_release(dd_palette_t* p);
dd_clipper_t* dd_clipper_from(void* iface);
dd_palette_t* dd_palette_from(void* iface);

extern const IDirectDrawSurfaceVtbl dd_surface1_vtbl;
extern const IDirectDrawSurface2Vtbl dd_surface2_vtbl;
extern const IDirectDrawSurface3Vtbl dd_surface3_vtbl;
extern const IDirectDrawSurface4Vtbl dd_surface4_vtbl;
extern const IDirectDrawGammaControlVtbl dd_gamma_vtbl;
int dd_is_surface_face(const void* iface);
int dd_device_vtables_complete(void);
int dd_surface_vtables_complete(void);
int dd_object_vtables_complete(void);

HRESULT WINAPI compat_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer);
HRESULT WINAPI compat_DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context);
HRESULT WINAPI compat_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, LPVOID context, DWORD flags);
HRESULT WINAPI compat_DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer);
HRESULT WINAPI compat_DirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID riid, IUnknown* outer);

/* D3D11 presentation (d3d11_present.c). */
typedef struct {
    char description[128];
    UINT vendor, device, subsystem, revision;
    LARGE_INTEGER driver_version;
    SIZE_T dedicated_video_memory;
} dd_adapter_t;

int present_adapter(dd_adapter_t* out);
void present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                   const DDGAMMARAMP* gamma, int gamma_changed, int vsync, unsigned generation);
void present_release(void);

#endif
