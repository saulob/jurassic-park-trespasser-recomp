/*
 * Surfaces of the virtual display: pixels in a DIB section, so Lock and GetDC see the same
 * memory, in the format the game asked for (RGB565 unless it named another). The primary is the
 * front of the flip chain; Flip exchanges buffers and every change to the front is presented.
 */
#include "d3d_legacy.h"
#include "host.h"
#include "page_arena.h"
#include "page_guard.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SURFACE_SIDE 8192u

void* dd_surface_face(dd_surface_t* s, int version) {
    switch (version) {
    case 2: return &s->s2;
    case 3: return &s->s3;
    case 4: return &s->s4;
    default: return &s->s1;
    }
}

int dd_is_surface_face(const void* iface) {
    const void* vtbl = iface ? ((const dd_face_t*)iface)->vtbl : NULL;
    return vtbl == &dd_surface1_vtbl || vtbl == &dd_surface2_vtbl || vtbl == &dd_surface3_vtbl || vtbl == &dd_surface4_vtbl;
}

dd_surface_t* dd_surface_from(void* iface) { return dd_is_surface_face(iface) ? DD_OWNER(dd_surface_t, iface) : NULL; }

#define surface_from dd_surface_from

int dd_surface_is_zbuffer(const dd_surface_t* s) { return (s->caps & DDSCAPS_ZBUFFER) != 0; }

static int pixel_bytes(const dd_surface_t* s) {
    if (dd_surface_is_zbuffer(s)) return s->format.dwZBufferBitDepth > 16 ? 4 : 2;
    return dd_format_bytes(&s->format);
}

/* Textures and z-buffers can be numerous and never need GDI until GetDC, so they start in plain memory.
 * Texture memory is write-watched, so the renderer sees writes that no Unlock announced. */
static int allocate_heap_pixels(dd_surface_t* s) {
    size_t bytes;
    s->pitch = (LONG)(((s->width * (DWORD)pixel_bytes(s)) + 3) & ~3u);
    bytes = (size_t)s->pitch * s->height;
    if ((s->caps & DDSCAPS_TEXTURE) && bytes <= PAGE_ARENA_MAX_BYTES) {
        s->heap_bits = (BYTE*)page_arena_alloc(bytes);
        s->arena = s->heap_bits != NULL;
    }
    if ((s->caps & DDSCAPS_TEXTURE) && !s->heap_bits)
        s->heap_bits = (BYTE*)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    s->watched = s->heap_bits != NULL;
    if (!s->heap_bits) s->heap_bits = (BYTE*)calloc(bytes, 1);
    s->bits = s->heap_bits;
    return s->bits != NULL;
}

static void free_heap_pixels(dd_surface_t* s) {
    if (s->arena) page_arena_free(s->heap_bits, (size_t)s->pitch * s->height);
    else if (s->watched) VirtualFree(s->heap_bits, 0, MEM_RELEASE);
    else free(s->heap_bits);
    s->heap_bits = NULL;
    s->watched = 0;
    s->arena = 0;
}

static int map_dib_section(dd_surface_t* s, const BITMAPINFO* info);

static int allocate_pixels(dd_surface_t* s) {
    struct {
        BITMAPINFOHEADER header;
        DWORD extra[256];
    } info;
    void* bits = NULL;
    memset(&info, 0, sizeof info);
    info.header.biSize = sizeof info.header;
    info.header.biWidth = (LONG)s->width;
    info.header.biHeight = -(LONG)s->height;
    info.header.biPlanes = 1;
    info.header.biBitCount = (WORD)s->format.dwRGBBitCount;
    if (s->format.dwRGBBitCount == 8) {
        info.header.biCompression = BI_RGB;
        info.header.biClrUsed = 256;
    } else {
        info.header.biCompression = BI_BITFIELDS;
        info.extra[0] = s->format.dwRBitMask;
        info.extra[1] = s->format.dwGBitMask;
        info.extra[2] = s->format.dwBBitMask;
    }
    if (!map_dib_section(s, (BITMAPINFO*)&info)) {
        s->dib = CreateDIBSection(NULL, (BITMAPINFO*)&info, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!s->dib || !bits) return 0;
        s->view = (BYTE*)bits;
    }
    if (s->heap_bits) {
        memcpy(s->view, s->heap_bits, (size_t)s->pitch * s->height);
        free_heap_pixels(s);
    }
    s->bits = s->view;
    s->pitch = (LONG)(((s->width * s->format.dwRGBBitCount + 31) / 32) * 4);
    return 1;
}

/* GDI pins the memory of a DIB it allocates, so its protection cannot change; a DIB over a section of
 * ours leaves this process its own view, which the renderer can make inaccessible while a Lock waits
 * for its first touch. GDI keeps writing through its view of the same pages. */
static int map_dib_section(dd_surface_t* s, const BITMAPINFO* info) {
    DWORD bytes = (DWORD)((((s->width * s->format.dwRGBBitCount + 31) / 32) * 4) * s->height);
    void* gdi_bits = NULL;
    s->section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, NULL);
    if (!s->section) return 0;
    s->view = (BYTE*)MapViewOfFile(s->section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (s->view) s->dib = CreateDIBSection(NULL, info, DIB_RGB_COLORS, &gdi_bits, s->section, 0);
    if (s->dib && gdi_bits) return 1;
    if (s->dib) DeleteObject(s->dib);
    if (s->view) UnmapViewOfFile(s->view);
    CloseHandle(s->section);
    s->dib = NULL;
    s->view = NULL;
    s->section = NULL;
    return 0;
}

static void free_surface(dd_surface_t* s) {
    dd_device_t* d = s->device;
    d3dr_surface_freed(s);
    if (s->zbuffer) dd_surface_release(s->zbuffer);
    free_heap_pixels(s);
    if (s->clipper) dd_clipper_release(s->clipper);
    if (s->palette) dd_palette_release(s->palette);
    if (s->dc) {
        if (s->dc_out) SelectObject(s->dc, s->dc_previous);
        DeleteDC(s->dc);
    }
    if (s->dib) DeleteObject(s->dib);
    page_alias_destroy(s->guard_alias);
    if (s->section) {
        UnmapViewOfFile(s->view);
        CloseHandle(s->section);
    }
    free(s);
    dd_surface_removed(d);
}

static dd_surface_t* new_surface(dd_device_t* d, DWORD width, DWORD height, const DDPIXELFORMAT* format, DWORD caps) {
    dd_surface_t* s = (dd_surface_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->s1.vtbl = &dd_surface1_vtbl;
    s->s2.vtbl = &dd_surface2_vtbl;
    s->s3.vtbl = &dd_surface3_vtbl;
    s->s4.vtbl = &dd_surface4_vtbl;
    s->gamma.vtbl = &dd_gamma_vtbl;
    s->s1.owner = s->s2.owner = s->s3.owner = s->s4.owner = s->gamma.owner = s;
    s->s1.version = 1;
    s->s2.version = 2;
    s->s3.version = 3;
    s->s4.version = 4;
    d3d_surface_init_texture_face(s);
    s->device = d;
    s->display_generation = dd_display_generation();
    s->width = width;
    s->height = height;
    s->format = *format;
    s->caps = caps;
    dd_surface_added(d);
    if (!((caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER)) ? allocate_heap_pixels(s) : allocate_pixels(s))) {
        free_surface(s);
        return NULL;
    }
    return s;
}

static int is_front(const dd_surface_t* s) { return s->device && !s->device->released && s->device->primary == s; }

void dd_present(dd_surface_t* front, int vsync) {
    dd_device_t* d = front->device;
    RECT source = { 0, 0, (LONG)front->width, (LONG)front->height };
    HWND window = d->window;
    if (!is_front(front) || front->lost) return;
    d3dr_surface_cpu_access(front, 0, CPU_ACCESS_FRONT);
    if (!((d->coop & DDSCL_FULLSCREEN) && d->mode_set)) {
        RECT client;
        POINT origin = { 0, 0 };
        RECT bounds = source;
        if (front->clipper && front->clipper->window) window = front->clipper->window;
        if (!window || !GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return;
        OffsetRect(&client, origin.x, origin.y);
        if (!IntersectRect(&source, &client, &bounds)) return;
    }
    if (!window) return;
    GdiFlush();
    present_frame(window, front->bits, front->pitch, &source, &front->format, &d->gamma, d->gamma_changed, vsync,
                  front->display_generation);
    d->gamma_changed = 0;
}

void dd_device_mode_changed(dd_device_t* d) {
    DWORD width, height;
    dd_surface_t* s = d->primary;
    if (!s) return;
    dd_display_size(d, &width, &height);
    if (s->width == width && s->height == height) return;
    do {
        s->lost = 1;
        s = s->next;
    } while (s && s != d->primary);
}

static DWORD memory_caps(DWORD requested) {
    return (requested & DDSCAPS_SYSTEMMEMORY) ? DDSCAPS_SYSTEMMEMORY : (DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
}

static HRESULT create_flip_chain(dd_device_t* d, dd_surface_t* front, DWORD count, DWORD caps) {
    dd_surface_t* last = front;
    DWORD member_caps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | memory_caps(caps);
    for (DWORD i = 0; i < count; i++) {
        dd_surface_t* back = new_surface(d, front->width, front->height, &front->format,
                                         member_caps | (i == 0 ? DDSCAPS_BACKBUFFER : 0));
        if (!back) {
            last->next = front;
            return DDERR_OUTOFMEMORY;
        }
        back->root = front;
        last->next = back;
        last = back;
    }
    last->next = front;
    front->back_buffers = count;
    return DD_OK;
}

static void destroy_root(dd_surface_t* root) {
    dd_surface_t* m = root->next;
    while (m && m != root) {
        dd_surface_t* next = m->next;
        m->next = NULL;
        if (m->refs <= 0) free_surface(m);
        else m->orphaned = 1;
        m = next;
    }
    if (root->device->primary == root) root->device->primary = NULL;
    free_surface(root);
}

/* Once: what the game's textures look like, and whether it asks for mip levels (only the top level
 * is emulated). */
static void note_texture_request(const DDSURFACEDESC* desc, DWORD caps, const dd_surface_t* s) {
    static int first_logged, mips_logged;
    DWORD levels = (desc->dwFlags & DDSD_MIPMAPCOUNT) ? desc->dwMipMapCount : 1;
    if (!first_logged) {
        first_logged = 1;
        dd_log("first texture surface: %lux%lu, caps 0x%08lX, %lu bpp %lX/%lX/%lX/%lX, %s memory%s, %lu mip level%s",
               s->width, s->height, caps, s->format.dwRGBBitCount, s->format.dwRBitMask, s->format.dwGBitMask,
               s->format.dwBBitMask, (s->format.dwFlags & DDPF_ALPHAPIXELS) ? s->format.dwRGBAlphaBitMask : 0,
               (caps & DDSCAPS_SYSTEMMEMORY) ? "system" : "video", s->watched ? ", write-watched" : "", levels,
               levels == 1 ? "" : "s");
    }
    if (!mips_logged && ((caps & DDSCAPS_MIPMAP) || levels > 1)) {
        mips_logged = 1;
        dd_log("texture %lux%lu asks for %lu mip levels (caps 0x%08lX): only the top level is emulated", s->width,
               s->height, levels, caps);
    }
}

HRESULT dd_create_surface(dd_device_t* d, const DDSURFACEDESC* desc, int version, void** out) {
    DWORD expected = version == 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    DWORD caps, width, height;
    DDPIXELFORMAT format;
    dd_surface_t* s;
    if (!out || !desc || desc->dwSize != expected) return DDERR_INVALIDPARAMS;
    *out = NULL;
    caps = (desc->dwFlags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : DDSCAPS_OFFSCREENPLAIN;
    if ((caps & (DDSCAPS_OVERLAY | DDSCAPS_MODEX)) || ((caps & DDSCAPS_ZBUFFER) && !d3dr_hardware())) {
        dd_log("CreateSurface caps 0x%08lX: overlays and Mode X are not emulated; z-buffers only with --renderer d3d11",
               caps);
        return DDERR_INVALIDCAPS;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (!d->coop) return DDERR_NOCOOPERATIVELEVELSET;
        if (d->primary) return DDERR_PRIMARYSURFACEALREADYEXISTS;
        if (desc->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT)) return DDERR_INVALIDPARAMS;
        dd_display_size(d, &width, &height);
        dd_format_rgb565(&format);
    } else {
        if ((desc->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) != (DDSD_WIDTH | DDSD_HEIGHT) || !desc->dwWidth ||
            !desc->dwHeight)
            return DDERR_INVALIDPARAMS;
        width = desc->dwWidth;
        height = desc->dwHeight;
        if (caps & DDSCAPS_ZBUFFER) {
            memset(&format, 0, sizeof format);
            format.dwSize = sizeof format;
            format.dwFlags = DDPF_ZBUFFER;
            format.dwZBufferBitDepth = 16;
            format.dwZBitMask = 0xFFFF;
            if ((desc->dwFlags & DDSD_PIXELFORMAT) && (desc->ddpfPixelFormat.dwFlags & DDPF_ZBUFFER)) {
                format = desc->ddpfPixelFormat;
                format.dwSize = sizeof format;
            }
            if (format.dwZBufferBitDepth != 16 && format.dwZBufferBitDepth != 24 && format.dwZBufferBitDepth != 32)
                return DDERR_INVALIDPIXELFORMAT;
        } else if (desc->dwFlags & DDSD_PIXELFORMAT) {
            format = desc->ddpfPixelFormat;
            format.dwSize = sizeof format;
            if (!dd_format_supported(&format)) {
                dd_log("CreateSurface: pixel format flags 0x%lX, %lu bits is not emulated", format.dwFlags,
                       format.dwRGBBitCount);
                return DDERR_INVALIDPIXELFORMAT;
            }
        } else {
            dd_format_rgb565(&format);
        }
    }
    if (width > MAX_SURFACE_SIDE || height > MAX_SURFACE_SIDE) return DDERR_TOOBIGSIZE;
    if (caps & DDSCAPS_FLIP) {
        if (!(caps & DDSCAPS_COMPLEX) || !(desc->dwFlags & DDSD_BACKBUFFERCOUNT) || !desc->dwBackBufferCount)
            return DDERR_INVALIDCAPS;
        if ((caps & DDSCAPS_PRIMARYSURFACE) && !(d->coop & DDSCL_EXCLUSIVE)) return DDERR_NOEXCLUSIVEMODE;
    }
    if ((caps & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) && !d3dr_hardware())
        dd_log("CreateSurface caps 0x%08lX: Direct3D needs --renderer d3d11, so this is a plain memory surface", caps);
    s = new_surface(d, width, height, &format,
                    caps | memory_caps(caps) |
                        ((caps & DDSCAPS_PRIMARYSURFACE) ? DDSCAPS_VISIBLE | ((caps & DDSCAPS_FLIP) ? DDSCAPS_FRONTBUFFER : 0) : 0));
    if (!s) return DDERR_OUTOFMEMORY;
    s->refs = 1;
    if (caps & DDSCAPS_FLIP) {
        HRESULT hr = create_flip_chain(d, s, desc->dwBackBufferCount, caps);
        if (FAILED(hr)) {
            destroy_root(s);
            return hr;
        }
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) d->primary = s;
    if (caps & DDSCAPS_TEXTURE) note_texture_request(desc, caps, s);
    if (caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER)) {
        *out = dd_surface_face(s, version);
        return DD_OK;
    }
    dd_log("CreateSurface 0x%08X: caps 0x%08lX, %lux%lu, %lu bpp RGB %lX/%lX/%lX, pitch %ld, %lu back buffers",
           (uint32_t)(uintptr_t)s, s->caps, width, height, format.dwRGBBitCount, format.dwRBitMask, format.dwGBitMask,
           format.dwBBitMask, s->pitch, s->back_buffers);
    *out = dd_surface_face(s, version);
    return DD_OK;
}

static void describe(const dd_surface_t* s, DDSURFACEDESC2* desc, DWORD size) {
    memset(desc, 0, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwWidth = s->width;
    desc->dwHeight = s->height;
    desc->lPitch = s->pitch;
    desc->ddpfPixelFormat = s->format;
    desc->ddsCaps.dwCaps = s->caps;
    if (s->back_buffers) {
        desc->dwFlags |= DDSD_BACKBUFFERCOUNT;
        desc->dwBackBufferCount = s->back_buffers;
    }
    if (s->has_src_key) {
        desc->dwFlags |= DDSD_CKSRCBLT;
        desc->ddckCKSrcBlt = s->src_key;
    }
    if (s->has_dst_key) {
        desc->dwFlags |= DDSD_CKDESTBLT;
        desc->ddckCKDestBlt = s->dst_key;
    }
}

static DWORD desc_size(const void* This) { return DD_VERSION(This) == 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC); }

static int rect_inside(const RECT* r, const dd_surface_t* s) {
    return r->left >= 0 && r->top >= 0 && r->left < r->right && r->top < r->bottom && (DWORD)r->right <= s->width &&
           (DWORD)r->bottom <= s->height;
}

static BYTE* pixel_address(const dd_surface_t* s, LONG x, LONG y) {
    return s->bits + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)pixel_bytes(s);
}

static DWORD read_pixel(const BYTE* p, int bytes) {
    switch (bytes) {
    case 1: return *p;
    case 2: return *(const WORD*)p;
    default: return *(const DWORD*)p;
    }
}

static void write_pixel(BYTE* p, int bytes, DWORD v) {
    switch (bytes) {
    case 1: *p = (BYTE)v; break;
    case 2: *(WORD*)p = (WORD)v; break;
    default: *(DWORD*)p = v; break;
    }
}

static DWORD channel_to_8(DWORD pixel, DWORD mask) {
    DWORD shift = 0, max;
    if (!mask) return 0;
    while (!((mask >> shift) & 1)) shift++;
    max = mask >> shift;
    return (((pixel & mask) >> shift) * 255 + max / 2) / max;
}

static DWORD channel_from_8(DWORD value, DWORD mask) {
    DWORD shift = 0, max;
    if (!mask) return 0;
    while (!((mask >> shift) & 1)) shift++;
    max = mask >> shift;
    return (((value * max + 127) / 255) << shift) & mask;
}

static DWORD convert_pixel(DWORD v, const DDPIXELFORMAT* from, const DDPIXELFORMAT* to) {
    return channel_from_8(channel_to_8(v, from->dwRBitMask), to->dwRBitMask) |
           channel_from_8(channel_to_8(v, from->dwGBitMask), to->dwGBitMask) |
           channel_from_8(channel_to_8(v, from->dwBBitMask), to->dwBBitMask);
}

static void fill_rect(dd_surface_t* s, const RECT* r, DWORD color) {
    int bytes = dd_format_bytes(&s->format);
    for (LONG y = r->top; y < r->bottom; y++) {
        BYTE* p = pixel_address(s, r->left, y);
        for (LONG x = r->left; x < r->right; x++, p += bytes) write_pixel(p, bytes, color);
    }
}

typedef struct {
    int use_key;
    DWORD key_low, key_high;
} blit_key_t;

static HRESULT copy_rect(dd_surface_t* dst, const RECT* d, dd_surface_t* src, const RECT* sr, const blit_key_t* key) {
    LONG dw = d->right - d->left, dh = d->bottom - d->top;
    LONG sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    int sb = dd_format_bytes(&src->format), db = dd_format_bytes(&dst->format);
    int same_format = dd_format_equal(&src->format, &dst->format);
    const BYTE* src_bits = src->bits;
    LONG src_pitch = src->pitch;
    BYTE* temp = NULL;
    if (!same_format && (src->format.dwRGBBitCount == 8 || dst->format.dwRGBBitCount == 8)) return DDERR_INVALIDPIXELFORMAT;
    if (src == dst) {
        RECT overlap;
        if (IntersectRect(&overlap, d, sr)) {
            temp = (BYTE*)malloc((size_t)src->pitch * src->height);
            if (!temp) return DDERR_OUTOFMEMORY;
            memcpy(temp, src->bits, (size_t)src->pitch * src->height);
            src_bits = temp;
        }
    }
    if (same_format && !key->use_key && dw == sw && dh == sh) {
        for (LONG y = 0; y < dh; y++)
            memcpy(pixel_address(dst, d->left, d->top + y),
                   src_bits + (size_t)(sr->top + y) * src_pitch + (size_t)sr->left * sb, (size_t)dw * db);
    } else {
        for (LONG y = 0; y < dh; y++) {
            LONG sy = sr->top + (LONG)(((int64_t)y * sh) / dh);
            const BYTE* srow = src_bits + (size_t)sy * src_pitch;
            BYTE* drow = pixel_address(dst, d->left, d->top + y);
            for (LONG x = 0; x < dw; x++) {
                LONG sx = sr->left + (LONG)(((int64_t)x * sw) / dw);
                DWORD v = read_pixel(srow + (size_t)sx * sb, sb);
                if (key->use_key && v >= key->key_low && v <= key->key_high) continue;
                write_pixel(drow + (size_t)x * db, db, same_format ? v : convert_pixel(v, &src->format, &dst->format));
            }
        }
    }
    free(temp);
    return DD_OK;
}

/* A clipper limits a blit to its window's client area, in the screen coordinates the primary uses. */
static int clip_to_clipper(const dd_surface_t* dst, RECT* d, RECT* s) {
    RECT client, clipped, bounds = { 0, 0, (LONG)dst->width, (LONG)dst->height };
    POINT origin = { 0, 0 };
    LONG dw = d->right - d->left, dh = d->bottom - d->top;
    if (dst->clipper && dst->clipper->window && GetClientRect(dst->clipper->window, &client) &&
        ClientToScreen(dst->clipper->window, &origin)) {
        OffsetRect(&client, origin.x, origin.y);
        IntersectRect(&bounds, &bounds, &client);
    }
    if (!IntersectRect(&clipped, d, &bounds)) return 0;
    if (s && dw > 0 && dh > 0) {
        LONG sw = s->right - s->left, sh = s->bottom - s->top;
        RECT adjusted;
        adjusted.left = s->left + (LONG)(((int64_t)(clipped.left - d->left) * sw) / dw);
        adjusted.right = s->left + (LONG)(((int64_t)(clipped.right - d->left) * sw) / dw);
        adjusted.top = s->top + (LONG)(((int64_t)(clipped.top - d->top) * sh) / dh);
        adjusted.bottom = s->top + (LONG)(((int64_t)(clipped.bottom - d->top) * sh) / dh);
        if (adjusted.left >= adjusted.right || adjusted.top >= adjusted.bottom) return 0;
        *s = adjusted;
    }
    *d = clipped;
    return 1;
}

#define BLT_SUPPORTED (DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT | DDBLT_COLORFILL | DDBLT_KEYSRC | \
                       DDBLT_KEYSRCOVERRIDE | DDBLT_ROP | DDBLT_DDFX)

static HRESULT blt(dd_surface_t* dst, const RECT* dst_rect, dd_surface_t* src, const RECT* src_rect, DWORD flags,
                   const DDBLTFX* fx) {
    RECT d = { 0, 0, (LONG)dst->width, (LONG)dst->height }, s;
    blit_key_t key = { 0, 0, 0 };
    HRESULT hr;
    if (flags & ~BLT_SUPPORTED) {
        dd_log("Blt flags 0x%08lX: only colour fill, source colour keys and SRCCOPY are emulated", flags);
        return DDERR_UNSUPPORTED;
    }
    if ((flags & (DDBLT_COLORFILL | DDBLT_KEYSRCOVERRIDE | DDBLT_ROP | DDBLT_DDFX)) && (!fx || fx->dwSize != sizeof *fx))
        return DDERR_INVALIDPARAMS;
    if (((flags & DDBLT_ROP) && fx->dwROP != SRCCOPY) || ((flags & DDBLT_DDFX) && fx->dwDDFX)) {
        dd_log("Blt raster operation 0x%08lX / effects 0x%08lX are not emulated", (flags & DDBLT_ROP) ? fx->dwROP : 0,
               (flags & DDBLT_DDFX) ? fx->dwDDFX : 0);
        return DDERR_UNSUPPORTED;
    }
    if (dst->lost || (src && src->lost)) return DDERR_SURFACELOST;
    if (dst->locked || dst->dc_out || (src && (src->locked || src->dc_out))) return DDERR_SURFACEBUSY;
    if (dst_rect) d = *dst_rect;
    if (!dst->clipper && !rect_inside(&d, dst)) return DDERR_INVALIDRECT;
    if (flags & DDBLT_COLORFILL) {
        if (!clip_to_clipper(dst, &d, NULL)) return DD_OK;
        if (!d3dr_surface_fill(dst, &d, fx->dwFillColor)) {
            d3dr_surface_cpu_access(dst, 1, CPU_ACCESS_BLT_DEST);
            fill_rect(dst, &d, fx->dwFillColor);
            d3dr_surface_cpu_written(dst, &d);
        }
    } else {
        if (!src) return DDERR_INVALIDPARAMS;
        s.left = 0;
        s.top = 0;
        s.right = (LONG)src->width;
        s.bottom = (LONG)src->height;
        if (src_rect) s = *src_rect;
        if (!rect_inside(&s, src)) return DDERR_INVALIDRECT;
        if (flags & DDBLT_KEYSRCOVERRIDE) {
            key.use_key = 1;
            key.key_low = fx->ddckSrcColorkey.dwColorSpaceLowValue;
            key.key_high = fx->ddckSrcColorkey.dwColorSpaceHighValue;
        } else if ((flags & DDBLT_KEYSRC) && src->has_src_key) {
            key.use_key = 1;
            key.key_low = src->src_key.dwColorSpaceLowValue;
            key.key_high = src->src_key.dwColorSpaceHighValue;
        }
        if (!clip_to_clipper(dst, &d, &s)) return DD_OK;
        d3dr_surface_cpu_access(src, 0, CPU_ACCESS_BLT_SOURCE);
        d3dr_surface_cpu_access(dst, 1, src == dst ? CPU_ACCESS_BLT_SOURCE : CPU_ACCESS_BLT_DEST);
        hr = copy_rect(dst, &d, src, &s, &key);
        if (FAILED(hr)) return hr;
        d3dr_surface_cpu_written(dst, &d);
    }
    if (is_front(dst)) dd_present(dst, 0);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE s_QueryInterface(IDirectDrawSurface* This, REFIID riid, LPVOID* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::QueryInterface");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectDrawSurface)) *out = &s->s1;
    else if (IsEqualGUID(riid, &IID_IDirectDrawSurface2)) *out = &s->s2;
    else if (IsEqualGUID(riid, &IID_IDirectDrawSurface3)) *out = &s->s3;
    else if (IsEqualGUID(riid, &IID_IDirectDrawSurface4)) *out = &s->s4;
    else if (IsEqualGUID(riid, &IID_IDirectDrawGammaControl) && is_front(s)) *out = &s->gamma;
    else if (d3dr_hardware() && (s->caps & DDSCAPS_TEXTURE) && IsEqualGUID(riid, &IID_IDirect3DTexture2)) *out = &s->tex2;
    if (*out) {
        InterlockedIncrement(&s->refs);
        DD_RETURN(DD_OK);
    }
    if (IsEqualGUID(riid, &IID_IDirect3DTexture) || IsEqualGUID(riid, &IID_IDirect3DTexture2) ||
        IsEqualGUID(riid, &IID_IDirect3DHALDevice) || IsEqualGUID(riid, &IID_IDirect3DRGBDevice) ||
        IsEqualGUID(riid, &IID_IDirect3DMMXDevice) || IsEqualGUID(riid, &IID_IDirect3DRampDevice))
        dd_log("surface QueryInterface for {%08lX-...} (Direct3D) from sub_%08X: only IDirect3DTexture2 on texture "
               "surfaces with --renderer d3d11", riid->Data1, g_cur_func);
    else if (!IsEqualGUID(riid, &IID_IDirectDrawGammaControl))
        dd_log("surface QueryInterface for {%08lX-%04X-%04X-...}: not implemented", riid->Data1, riid->Data2, riid->Data3);
    DD_RETURN(E_NOINTERFACE);
}

static ULONG STDMETHODCALLTYPE s_AddRef(IDirectDrawSurface* This) {
    return (ULONG)InterlockedIncrement(&DD_OWNER(dd_surface_t, This)->refs);
}

void dd_surface_addref(dd_surface_t* s) { InterlockedIncrement(&s->refs); }

/* Back buffers belong to the front of their chain and go with it, as in DirectDraw; one the game
 * still holds then lives on, detached, until its own last Release. */
static ULONG STDMETHODCALLTYPE s_Release(IDirectDrawSurface* This) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    LONG refs;
    dd_lock();
    refs = InterlockedDecrement(&s->refs);
    if (refs <= 0) {
        if (s->orphaned) free_surface(s);
        else if (!s->root) destroy_root(s);
        refs = 0;
    }
    dd_unlock();
    return (ULONG)refs;
}

void dd_surface_release(dd_surface_t* s) { s_Release((IDirectDrawSurface*)&s->s1); }

static HRESULT STDMETHODCALLTYPE s_AddAttachedSurface(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE surface) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    dd_surface_t* z = surface_from(surface);
    DD_METHOD("IDirectDrawSurface::AddAttachedSurface");
    if (!z) DD_RETURN(DDERR_INVALIDPARAMS);
    if (!d3dr_hardware() || !dd_surface_is_zbuffer(z)) {
        dd_log("AddAttachedSurface caps 0x%08lX from sub_%08X: only z-buffers, with --renderer d3d11", z->caps, g_cur_func);
        DD_RETURN(DDERR_CANNOTATTACHSURFACE);
    }
    if (s->zbuffer) DD_RETURN(DDERR_SURFACEALREADYATTACHED);
    if (z->width != s->width || z->height != s->height) DD_RETURN(DDERR_CANNOTATTACHSURFACE);
    dd_surface_addref(z);
    s->zbuffer = z;
    dd_log("z-buffer %lux%lu, %lu bits attached to surface 0x%08X", z->width, z->height, z->format.dwZBufferBitDepth,
           (uint32_t)(uintptr_t)s);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_AddOverlayDirtyRect(IDirectDrawSurface* This, LPRECT rect) {
    (void)This; (void)rect;
    DD_UNSUPPORTED("IDirectDrawSurface::AddOverlayDirtyRect", "no overlays", DDERR_NOOVERLAYHW);
}

static HRESULT STDMETHODCALLTYPE s_Blt(IDirectDrawSurface* This, LPRECT dst_rect, LPDIRECTDRAWSURFACE source,
                                       LPRECT src_rect, DWORD flags, LPDDBLTFX fx) {
    dd_surface_t* src = source ? surface_from(source) : NULL;
    DD_METHOD("IDirectDrawSurface::Blt");
    if (source && !src) DD_RETURN(DDERR_INVALIDOBJECT);
    DD_RETURN(blt(DD_OWNER(dd_surface_t, This), dst_rect, src, src_rect, flags, fx));
}

static HRESULT STDMETHODCALLTYPE s_BltBatch(IDirectDrawSurface* This, LPDDBLTBATCH batch, DWORD count, DWORD flags) {
    (void)This; (void)batch; (void)count; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface::BltBatch", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s_BltFast(IDirectDrawSurface* This, DWORD x, DWORD y, LPDIRECTDRAWSURFACE source,
                                           LPRECT src_rect, DWORD trans) {
    dd_surface_t* dst = DD_OWNER(dd_surface_t, This);
    dd_surface_t* src = surface_from(source);
    RECT s, d;
    DDBLTFX fx;
    DWORD flags = 0;
    DD_METHOD("IDirectDrawSurface::BltFast");
    if (!src) DD_RETURN(DDERR_INVALIDPARAMS);
    if (dst->clipper) DD_RETURN(DDERR_BLTFASTCANTCLIP);
    if (trans & DDBLTFAST_DESTCOLORKEY) {
        dd_log("BltFast with a destination colour key is not emulated");
        DD_RETURN(DDERR_UNSUPPORTED);
    }
    s.left = 0;
    s.top = 0;
    s.right = (LONG)src->width;
    s.bottom = (LONG)src->height;
    if (src_rect) s = *src_rect;
    d.left = (LONG)x;
    d.top = (LONG)y;
    d.right = d.left + (s.right - s.left);
    d.bottom = d.top + (s.bottom - s.top);
    memset(&fx, 0, sizeof fx);
    fx.dwSize = sizeof fx;
    if (trans & DDBLTFAST_SRCCOLORKEY) flags |= DDBLT_KEYSRC;
    DD_RETURN(blt(dst, &d, src, &s, flags, &fx));
}

static HRESULT STDMETHODCALLTYPE s_DeleteAttachedSurface(IDirectDrawSurface* This, DWORD flags, LPDIRECTDRAWSURFACE surface) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    dd_surface_t* z = surface ? surface_from(surface) : s->zbuffer;
    (void)flags;
    DD_METHOD("IDirectDrawSurface::DeleteAttachedSurface");
    if (!z || z != s->zbuffer) {
        dd_log("DeleteAttachedSurface from sub_%08X: flip chains are fixed at creation; only z-buffers detach", g_cur_func);
        DD_RETURN(DDERR_CANNOTDETACHSURFACE);
    }
    s->zbuffer = NULL;
    dd_surface_release(z);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_EnumAttachedSurfaces(IDirectDrawSurface* This, LPVOID context,
                                                        LPDDENUMSURFACESCALLBACK callback) {
    (void)This; (void)context; (void)callback;
    DD_UNSUPPORTED("IDirectDrawSurface::EnumAttachedSurfaces", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s_EnumOverlayZOrders(IDirectDrawSurface* This, DWORD flags, LPVOID context,
                                                      LPDDENUMSURFACESCALLBACK callback) {
    (void)This; (void)flags; (void)context; (void)callback;
    DD_UNSUPPORTED("IDirectDrawSurface::EnumOverlayZOrders", "no overlays", DDERR_NOOVERLAYHW);
}

static void exchange_pixels(dd_surface_t* a, dd_surface_t* b) {
    BYTE* bits = a->bits;
    BYTE* view = a->view;
    HBITMAP dib = a->dib;
    HANDLE section = a->section;
    int alias = a->guard_alias;
    a->bits = b->bits;
    a->view = b->view;
    a->dib = b->dib;
    a->section = b->section;
    a->guard_alias = b->guard_alias;
    b->bits = bits;
    b->view = view;
    b->dib = dib;
    b->section = section;
    b->guard_alias = alias;
}

static HRESULT STDMETHODCALLTYPE s_Flip(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE target_iface, DWORD flags) {
    dd_surface_t* front = DD_OWNER(dd_surface_t, This);
    dd_surface_t* target = target_iface ? surface_from(target_iface) : NULL;
    dd_surface_t* m;
    DD_METHOD("IDirectDrawSurface::Flip");
    if (!(front->caps & DDSCAPS_FLIP) || front->root || !front->next) DD_RETURN(DDERR_NOTFLIPPABLE);
    if (target_iface && (!target || target->root != front)) DD_RETURN(DDERR_INVALIDPARAMS);
    for (m = front;;) {
        if (m->lost) DD_RETURN(DDERR_SURFACELOST);
        if (m->locked || m->dc_out) DD_RETURN(DDERR_SURFACEBUSY);
        m = m->next;
        if (m == front) break;
    }
    if (is_front(front) && d3dr_flip(front, !(flags & DDFLIP_NOVSYNC))) {
        if (target) exchange_pixels(front, target);
        else
            for (m = front; m->next != front; m = m->next) exchange_pixels(m, m->next);
        DD_RETURN(DD_OK);
    }
    if (target) {
        exchange_pixels(front, target);
    } else {
        for (m = front; m->next != front; m = m->next) exchange_pixels(m, m->next);
    }
    if (is_front(front)) dd_present(front, !(flags & DDFLIP_NOVSYNC));
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetAttachedSurface(IDirectDrawSurface* This, LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetAttachedSurface");
    if (!caps || !out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (s->zbuffer && (caps->dwCaps & DDSCAPS_ZBUFFER)) {
        dd_surface_addref(s->zbuffer);
        *out = (LPDIRECTDRAWSURFACE)dd_surface_face(s->zbuffer, DD_VERSION(This));
        DD_RETURN(DD_OK);
    }
    for (dd_surface_t* m = s->next; m && m != s; m = m->next) {
        if ((m->caps & caps->dwCaps) == caps->dwCaps) {
            InterlockedIncrement(&m->refs);
            *out = (LPDIRECTDRAWSURFACE)dd_surface_face(m, DD_VERSION(This));
            DD_RETURN(DD_OK);
        }
    }
    DD_RETURN(DDERR_NOTFOUND);
}

/* Blits and flips complete before the call returns. */
static HRESULT STDMETHODCALLTYPE s_GetBltStatus(IDirectDrawSurface* This, DWORD flags) {
    (void)This; (void)flags;
    DD_METHOD("IDirectDrawSurface::GetBltStatus");
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetCaps(IDirectDrawSurface* This, LPDDSCAPS caps) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetCaps");
    if (!caps) DD_RETURN(DDERR_INVALIDPARAMS);
    if (DD_VERSION(This) == 4) memset(caps, 0, sizeof(DDSCAPS2));
    caps->dwCaps = s->caps;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetClipper(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetClipper");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!s->clipper) DD_RETURN(DDERR_NOCLIPPERATTACHED);
    dd_clipper_addref(s->clipper);
    *out = (LPDIRECTDRAWCLIPPER)&s->clipper->face;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetColorKey(IDirectDrawSurface* This, DWORD flags, LPDDCOLORKEY key) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetColorKey");
    if (!key) DD_RETURN(DDERR_INVALIDPARAMS);
    if (flags == DDCKEY_SRCBLT && s->has_src_key) *key = s->src_key;
    else if (flags == DDCKEY_DESTBLT && s->has_dst_key) *key = s->dst_key;
    else DD_RETURN(DDERR_NOCOLORKEY);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetDC(IDirectDrawSurface* This, HDC* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetDC");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    if (s->lost) DD_RETURN(DDERR_SURFACELOST);
    if (s->dc_out) DD_RETURN(DDERR_DCALREADYCREATED);
    if (s->locked) DD_RETURN(DDERR_SURFACEBUSY);
    if (!s->dib && (dd_surface_is_zbuffer(s) || !allocate_pixels(s))) DD_RETURN(DDERR_CANTCREATEDC);
    d3dr_surface_cpu_access(s, 1, CPU_ACCESS_GETDC);
    if (!s->dc) s->dc = CreateCompatibleDC(NULL);
    if (!s->dc) DD_RETURN(DDERR_CANTCREATEDC);
    s->dc_previous = SelectObject(s->dc, s->dib);
    s->dc_out = 1;
    *out = s->dc;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetFlipStatus(IDirectDrawSurface* This, DWORD flags) {
    (void)This; (void)flags;
    DD_METHOD("IDirectDrawSurface::GetFlipStatus");
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetOverlayPosition(IDirectDrawSurface* This, LPLONG x, LPLONG y) {
    (void)This; (void)x; (void)y;
    DD_UNSUPPORTED("IDirectDrawSurface::GetOverlayPosition", "no overlays", DDERR_NOTAOVERLAYSURFACE);
}

static HRESULT STDMETHODCALLTYPE s_GetPalette(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::GetPalette");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    *out = NULL;
    if (!s->palette) DD_RETURN(DDERR_NOPALETTEATTACHED);
    dd_palette_addref(s->palette);
    *out = (LPDIRECTDRAWPALETTE)&s->palette->face;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetPixelFormat(IDirectDrawSurface* This, LPDDPIXELFORMAT format) {
    DD_METHOD("IDirectDrawSurface::GetPixelFormat");
    if (!format) DD_RETURN(DDERR_INVALIDPARAMS);
    *format = DD_OWNER(dd_surface_t, This)->format;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_GetSurfaceDesc(IDirectDrawSurface* This, LPDDSURFACEDESC desc) {
    DWORD size = desc_size(This);
    DD_METHOD("IDirectDrawSurface::GetSurfaceDesc");
    if (!desc || desc->dwSize != size) DD_RETURN(DDERR_INVALIDPARAMS);
    describe(DD_OWNER(dd_surface_t, This), (DDSURFACEDESC2*)desc, size);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_Initialize(IDirectDrawSurface* This, LPDIRECTDRAW dd, LPDDSURFACEDESC desc) {
    (void)This; (void)dd; (void)desc;
    DD_METHOD("IDirectDrawSurface::Initialize");
    DD_RETURN(DDERR_ALREADYINITIALIZED);
}

static HRESULT STDMETHODCALLTYPE s_IsLost(IDirectDrawSurface* This) {
    DD_METHOD("IDirectDrawSurface::IsLost");
    DD_RETURN(DD_OWNER(dd_surface_t, This)->lost ? DDERR_SURFACELOST : DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_Lock(IDirectDrawSurface* This, LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE event) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DWORD size = desc_size(This);
    RECT whole = { 0, 0, (LONG)s->width, (LONG)s->height };
    DDSURFACEDESC2* out = (DDSURFACEDESC2*)desc;
    (void)event;
    DD_METHOD("IDirectDrawSurface::Lock");
    if (!desc || desc->dwSize != size) DD_RETURN(DDERR_INVALIDPARAMS);
    if (s->lost) DD_RETURN(DDERR_SURFACELOST);
    if (s->locked || s->dc_out) DD_RETURN(DDERR_SURFACEBUSY);
    if (rect && !rect_inside(rect, s)) DD_RETURN(DDERR_INVALIDRECT);
    if (!rect) rect = &whole;
    s->lock_write = !(flags & DDLOCK_READONLY);
    s->lock_flags = flags;
    s->lock_rect = *rect;
    GdiFlush();
    s->lock_alias = 0;
    d3dr_surface_cpu_access(s, s->lock_write, CPU_ACCESS_LOCK);
    describe(s, out, size);
    out->dwFlags |= DDSD_LPSURFACE;
    out->lpSurface = pixel_address(s, rect->left, rect->top);
    if (s->lock_alias) out->lpSurface = page_alias_view(s->guard_alias) + ((BYTE*)out->lpSurface - s->bits);
    if (s->watched) s->exposed = 1;
    s->locked = 1;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_ReleaseDC(IDirectDrawSurface* This, HDC dc) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface::ReleaseDC");
    if (!s->dc_out || dc != s->dc) DD_RETURN(DDERR_INVALIDPARAMS);
    GdiFlush();
    SelectObject(s->dc, s->dc_previous);
    s->dc_out = 0;
    d3dr_surface_cpu_written(s, NULL);
    if (is_front(s)) dd_present(s, 0);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_Restore(IDirectDrawSurface* This) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DWORD width, height;
    DD_METHOD("IDirectDrawSurface::Restore");
    if (!s->lost) DD_RETURN(DD_OK);
    dd_display_size(s->device, &width, &height);
    if (s->width != width || s->height != height) DD_RETURN(DDERR_WRONGMODE);
    s->lost = 0;
    s->display_generation = dd_display_generation();
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_SetClipper(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER clipper_iface) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    dd_clipper_t* clipper = clipper_iface ? dd_clipper_from(clipper_iface) : NULL;
    DD_METHOD("IDirectDrawSurface::SetClipper");
    if (clipper_iface && !clipper) DD_RETURN(DDERR_INVALIDOBJECT);
    if (clipper) dd_clipper_addref(clipper);
    if (s->clipper) dd_clipper_release(s->clipper);
    s->clipper = clipper;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_SetColorKey(IDirectDrawSurface* This, DWORD flags, LPDDCOLORKEY key) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DWORD which = flags & ~DDCKEY_COLORSPACE;
    DDCOLORKEY value;
    DD_METHOD("IDirectDrawSurface::SetColorKey");
    if (which != DDCKEY_SRCBLT && which != DDCKEY_DESTBLT) {
        dd_log("SetColorKey flags 0x%lX: overlay colour keys are not emulated", flags);
        DD_RETURN(DDERR_UNSUPPORTED);
    }
    if (key) {
        value = *key;
        if (!(flags & DDCKEY_COLORSPACE)) value.dwColorSpaceHighValue = value.dwColorSpaceLowValue;
    }
    if (which == DDCKEY_SRCBLT) {
        d3dr_surface_cpu_access(s, 1, CPU_ACCESS_BLT_DEST);
        s->has_src_key = key != NULL;
        if (key) s->src_key = value;
        d3dr_surface_cpu_written(s, NULL);
    } else {
        s->has_dst_key = key != NULL;
        if (key) s->dst_key = value;
    }
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_SetOverlayPosition(IDirectDrawSurface* This, LONG x, LONG y) {
    (void)This; (void)x; (void)y;
    DD_UNSUPPORTED("IDirectDrawSurface::SetOverlayPosition", "no overlays", DDERR_NOTAOVERLAYSURFACE);
}

static void apply_palette(dd_surface_t* s) {
    RGBQUAD colors[256];
    if (!s->palette || !s->dc) return;
    for (int i = 0; i < 256; i++) {
        colors[i].rgbRed = s->palette->entries[i].peRed;
        colors[i].rgbGreen = s->palette->entries[i].peGreen;
        colors[i].rgbBlue = s->palette->entries[i].peBlue;
        colors[i].rgbReserved = 0;
    }
    if (!s->dc_out) s->dc_previous = SelectObject(s->dc, s->dib);
    SetDIBColorTable(s->dc, 0, 256, colors);
    if (!s->dc_out) SelectObject(s->dc, s->dc_previous);
}

static HRESULT STDMETHODCALLTYPE s_SetPalette(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE palette_iface) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    dd_palette_t* palette = palette_iface ? dd_palette_from(palette_iface) : NULL;
    DD_METHOD("IDirectDrawSurface::SetPalette");
    if (palette_iface && !palette) DD_RETURN(DDERR_INVALIDOBJECT);
    if (palette && s->format.dwRGBBitCount != 8) DD_RETURN(DDERR_INVALIDPIXELFORMAT);
    if (palette) dd_palette_addref(palette);
    if (s->palette) dd_palette_release(s->palette);
    s->palette = palette;
    if (!s->dc) s->dc = CreateCompatibleDC(NULL);
    apply_palette(s);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_Unlock(IDirectDrawSurface* This, LPVOID pointer) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    (void)pointer;
    DD_METHOD("IDirectDrawSurface::Unlock");
    if (!s->locked) DD_RETURN(DDERR_NOTLOCKED);
    s->locked = 0;
    s->lock_alias = 0;
    if (!d3dr_surface_unlocked(s) && s->lock_write) d3dr_surface_cpu_written(s, &s->lock_rect);
    if (is_front(s)) dd_present(s, 0);
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s_UpdateOverlay(IDirectDrawSurface* This, LPRECT src, LPDIRECTDRAWSURFACE dst,
                                                 LPRECT dst_rect, DWORD flags, LPDDOVERLAYFX fx) {
    (void)This; (void)src; (void)dst; (void)dst_rect; (void)flags; (void)fx;
    DD_UNSUPPORTED("IDirectDrawSurface::UpdateOverlay", "no overlays", DDERR_NOOVERLAYHW);
}

static HRESULT STDMETHODCALLTYPE s_UpdateOverlayDisplay(IDirectDrawSurface* This, DWORD flags) {
    (void)This; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface::UpdateOverlayDisplay", "no overlays", DDERR_NOOVERLAYHW);
}

static HRESULT STDMETHODCALLTYPE s_UpdateOverlayZOrder(IDirectDrawSurface* This, DWORD flags, LPDIRECTDRAWSURFACE reference) {
    (void)This; (void)flags; (void)reference;
    DD_UNSUPPORTED("IDirectDrawSurface::UpdateOverlayZOrder", "no overlays", DDERR_NOOVERLAYHW);
}

static HRESULT STDMETHODCALLTYPE s2_GetDDInterface(IDirectDrawSurface2* This, LPVOID* out) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    DD_METHOD("IDirectDrawSurface2::GetDDInterface");
    if (!out) DD_RETURN(DDERR_INVALIDPARAMS);
    InterlockedIncrement(&s->device->refs);
    *out = &s->device->dd1;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE s2_PageLock(IDirectDrawSurface2* This, DWORD flags) {
    (void)This; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface2::PageLock", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s2_PageUnlock(IDirectDrawSurface2* This, DWORD flags) {
    (void)This; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface2::PageUnlock", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s3_SetSurfaceDesc(IDirectDrawSurface3* This, LPDDSURFACEDESC desc, DWORD flags) {
    (void)This; (void)desc; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface3::SetSurfaceDesc", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s4_SetPrivateData(IDirectDrawSurface4* This, REFGUID tag, LPVOID data, DWORD size, DWORD flags) {
    (void)This; (void)tag; (void)data; (void)size; (void)flags;
    DD_UNSUPPORTED("IDirectDrawSurface4::SetPrivateData", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s4_GetPrivateData(IDirectDrawSurface4* This, REFGUID tag, LPVOID data, LPDWORD size) {
    (void)This; (void)tag; (void)data; (void)size;
    DD_UNSUPPORTED("IDirectDrawSurface4::GetPrivateData", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s4_FreePrivateData(IDirectDrawSurface4* This, REFGUID tag) {
    (void)This; (void)tag;
    DD_UNSUPPORTED("IDirectDrawSurface4::FreePrivateData", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s4_GetUniquenessValue(IDirectDrawSurface4* This, LPDWORD value) {
    (void)This; (void)value;
    DD_UNSUPPORTED("IDirectDrawSurface4::GetUniquenessValue", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE s4_ChangeUniquenessValue(IDirectDrawSurface4* This) {
    (void)This;
    DD_UNSUPPORTED("IDirectDrawSurface4::ChangeUniquenessValue", "unused by the game", DDERR_UNSUPPORTED);
}

static HRESULT STDMETHODCALLTYPE g_QueryInterface(IDirectDrawGammaControl* This, REFIID riid, LPVOID* out) {
    return s_QueryInterface((IDirectDrawSurface*)This, riid, out);
}

static HRESULT STDMETHODCALLTYPE g_GetGammaRamp(IDirectDrawGammaControl* This, DWORD flags, LPDDGAMMARAMP ramp) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    (void)flags;
    DD_METHOD("IDirectDrawGammaControl::GetGammaRamp");
    if (!ramp) DD_RETURN(DDERR_INVALIDPARAMS);
    *ramp = s->device->gamma;
    DD_RETURN(DD_OK);
}

static HRESULT STDMETHODCALLTYPE g_SetGammaRamp(IDirectDrawGammaControl* This, DWORD flags, LPDDGAMMARAMP ramp) {
    dd_surface_t* s = DD_OWNER(dd_surface_t, This);
    (void)flags;
    DD_METHOD("IDirectDrawGammaControl::SetGammaRamp");
    if (!ramp) DD_RETURN(DDERR_INVALIDPARAMS);
    s->device->gamma = *ramp;
    s->device->gamma_changed = 1;
    if (is_front(s) && !s->locked && !s->dc_out) dd_present(s, 0);
    DD_RETURN(DD_OK);
}

/* Versions 2 to 4 add methods at the end; the shared ones differ only in pointer types. */
#define SHARED(fn) ((void*)(fn))

#define SURFACE_V1_METHODS(X)                                                                                         \
    .QueryInterface = X(s_QueryInterface), .AddRef = X(s_AddRef), .Release = X(s_Release),                          \
    .AddAttachedSurface = X(s_AddAttachedSurface), .AddOverlayDirtyRect = X(s_AddOverlayDirtyRect), .Blt = X(s_Blt), \
    .BltBatch = X(s_BltBatch), .BltFast = X(s_BltFast), .DeleteAttachedSurface = X(s_DeleteAttachedSurface),         \
    .EnumAttachedSurfaces = X(s_EnumAttachedSurfaces), .EnumOverlayZOrders = X(s_EnumOverlayZOrders),                \
    .Flip = X(s_Flip), .GetAttachedSurface = X(s_GetAttachedSurface), .GetBltStatus = X(s_GetBltStatus),             \
    .GetCaps = X(s_GetCaps), .GetClipper = X(s_GetClipper), .GetColorKey = X(s_GetColorKey), .GetDC = X(s_GetDC),    \
    .GetFlipStatus = X(s_GetFlipStatus), .GetOverlayPosition = X(s_GetOverlayPosition), .GetPalette = X(s_GetPalette), \
    .GetPixelFormat = X(s_GetPixelFormat), .GetSurfaceDesc = X(s_GetSurfaceDesc), .Initialize = X(s_Initialize),     \
    .IsLost = X(s_IsLost), .Lock = X(s_Lock), .ReleaseDC = X(s_ReleaseDC), .Restore = X(s_Restore),                  \
    .SetClipper = X(s_SetClipper), .SetColorKey = X(s_SetColorKey), .SetOverlayPosition = X(s_SetOverlayPosition),   \
    .SetPalette = X(s_SetPalette), .Unlock = X(s_Unlock), .UpdateOverlay = X(s_UpdateOverlay),                       \
    .UpdateOverlayDisplay = X(s_UpdateOverlayDisplay), .UpdateOverlayZOrder = X(s_UpdateOverlayZOrder)

#define AS_IS(fn) fn

const IDirectDrawSurfaceVtbl dd_surface1_vtbl = { SURFACE_V1_METHODS(AS_IS) };

const IDirectDrawSurface2Vtbl dd_surface2_vtbl = {
    SURFACE_V1_METHODS(SHARED), .GetDDInterface = s2_GetDDInterface, .PageLock = s2_PageLock, .PageUnlock = s2_PageUnlock,
};

const IDirectDrawSurface3Vtbl dd_surface3_vtbl = {
    SURFACE_V1_METHODS(SHARED), .GetDDInterface = SHARED(s2_GetDDInterface), .PageLock = SHARED(s2_PageLock),
    .PageUnlock = SHARED(s2_PageUnlock), .SetSurfaceDesc = s3_SetSurfaceDesc,
};

const IDirectDrawSurface4Vtbl dd_surface4_vtbl = {
    SURFACE_V1_METHODS(SHARED), .GetDDInterface = SHARED(s2_GetDDInterface), .PageLock = SHARED(s2_PageLock),
    .PageUnlock = SHARED(s2_PageUnlock), .SetSurfaceDesc = SHARED(s3_SetSurfaceDesc), .SetPrivateData = s4_SetPrivateData,
    .GetPrivateData = s4_GetPrivateData, .FreePrivateData = s4_FreePrivateData,
    .GetUniquenessValue = s4_GetUniquenessValue, .ChangeUniquenessValue = s4_ChangeUniquenessValue,
};

const IDirectDrawGammaControlVtbl dd_gamma_vtbl = {
    .QueryInterface = g_QueryInterface, .AddRef = SHARED(s_AddRef), .Release = SHARED(s_Release),
    .GetGammaRamp = g_GetGammaRamp, .SetGammaRamp = g_SetGammaRamp,
};

C_ASSERT(sizeof(IDirectDrawSurfaceVtbl) == 36 * sizeof(void*));
C_ASSERT(sizeof(IDirectDrawSurface2Vtbl) == 39 * sizeof(void*));
C_ASSERT(sizeof(IDirectDrawSurface3Vtbl) == 40 * sizeof(void*));
C_ASSERT(sizeof(IDirectDrawSurface4Vtbl) == 45 * sizeof(void*));
C_ASSERT(sizeof(IDirectDrawGammaControlVtbl) == 5 * sizeof(void*));
C_ASSERT(sizeof(DDSURFACEDESC) == 0x6C && sizeof(DDSURFACEDESC2) == 0x7C);
C_ASSERT(offsetof(DDSURFACEDESC, ddpfPixelFormat) == offsetof(DDSURFACEDESC2, ddpfPixelFormat));
C_ASSERT(offsetof(DDSURFACEDESC, ddsCaps) == offsetof(DDSURFACEDESC2, ddsCaps));
C_ASSERT(offsetof(DDSURFACEDESC, lpSurface) == offsetof(DDSURFACEDESC2, lpSurface));

int dd_surface_vtables_complete(void) {
    static const struct { const void* table; size_t size; } k_tables[] = {
        { &dd_surface1_vtbl, sizeof dd_surface1_vtbl }, { &dd_surface2_vtbl, sizeof dd_surface2_vtbl },
        { &dd_surface3_vtbl, sizeof dd_surface3_vtbl }, { &dd_surface4_vtbl, sizeof dd_surface4_vtbl },
        { &dd_gamma_vtbl, sizeof dd_gamma_vtbl },
    };
    for (int t = 0; t < (int)(sizeof k_tables / sizeof k_tables[0]); t++) {
        void* const* slots = (void* const*)k_tables[t].table;
        for (size_t i = 0; i < k_tables[t].size / sizeof(void*); i++)
            if (!slots[i]) return 0;
    }
    return 1;
}
