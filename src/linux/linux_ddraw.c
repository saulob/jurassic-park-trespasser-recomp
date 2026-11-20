#include "linux_ddraw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "linux_gdi32.h"
#include "linux_log.h"
#include "linux_screen.h"
#include "linux_spine.h"
#include "linux_user32.h"
#include "linux_window.h"

#define MAX_OBJECTS 256
#define MAX_SURFACE_SIDE 8192u
#define FACE_BYTES 16u
#define FACE_MAGIC 0x43464444u /* 'DDFC' */
#define DISPLAY_BPP 16u
#define DEVICE_IDENTIFIER_BYTES 1064u /* DDDEVICEIDENTIFIER: two 512-byte strings, the driver version, four ids and a GUID */

#define DD_OK 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_NOTIMPL_ 0x80004001u
#define E_INVALIDARG_ 0x80070057u
#define E_OUTOFMEMORY_ 0x8007000Eu
#define CLASS_E_NOAGGREGATION_ 0x80040110u
#define DDERR(n) (0x88760000u | (n))
#define DDERR_ALREADYINITIALIZED_ DDERR(5)
#define DDERR_CANNOTATTACHSURFACE_ DDERR(10)
#define DDERR_INVALIDCAPS_ DDERR(100)
#define DDERR_INVALIDMODE_ DDERR(120)
#define DDERR_INVALIDOBJECT_ DDERR(130)
#define DDERR_INVALIDPIXELFORMAT_ DDERR(145)
#define DDERR_INVALIDRECT_ DDERR(150)
#define DDERR_NOCOOPERATIVELEVELSET_ DDERR(212)
#define DDERR_NOCOLORKEY_ DDERR(215)
#define DDERR_NOEXCLUSIVEMODE_ DDERR(225)
#define DDERR_NOTFOUND_ DDERR(255)
#define DDERR_SURFACEBUSY_ DDERR(430)
#define DDERR_SURFACELOST_ DDERR(450)
#define DDERR_TOOBIGSIZE_ DDERR(480)
#define DDERR_PRIMARYSURFACEALREADYEXISTS_ DDERR(564)
#define DDERR_NOCLIPPERATTACHED_ DDERR(568)
#define DDERR_NOPALETTEATTACHED_ DDERR(572)
#define DDERR_BLTFASTCANTCLIP_ DDERR(574)
#define DDERR_NOTFLIPPABLE_ DDERR(582)
#define DDERR_NOTLOCKED_ DDERR(584)
#define DDERR_CANTCREATEDC_ DDERR(585)
#define DDERR_WRONGMODE_ DDERR(587)
#define DDERR_DCALREADYCREATED_ DDERR(620)

#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_OVERLAY 0x80u
#define DDSCAPS_PALETTE 0x100u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_TEXTURE 0x1000u
#define DDSCAPS_3DDEVICE 0x2000u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_VISIBLE 0x8000u
#define DDSCAPS_ZBUFFER 0x20000u
#define DDSCAPS_MODEX 0x200000u
#define DDSCAPS_LOCALVIDMEM 0x10000000u

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_CKDESTBLT 0x4000u
#define DDSD_CKSRCBLT 0x10000u
#define DDSD_REFRESHRATE 0x40000u

#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_RGB 0x40u
#define DDPF_PALETTEINDEXED8 0x20u
#define DDPF_FOURCC 0x4u
#define DDPF_ZBUFFER 0x400u

#define DDSCL_FULLSCREEN 0x1u
#define DDSCL_NORMAL 0x8u
#define DDSCL_EXCLUSIVE 0x10u
#define DDSCL_SETFOCUSWINDOW 0x80u
#define DDSCL_SETDEVICEWINDOW 0x100u

#define DDBLT_ASYNC 0x200u
#define DDBLT_COLORFILL 0x400u
#define DDBLT_DDFX 0x800u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
#define DDBLT_ROP 0x20000u
#define DDBLT_WAIT 0x1000000u
#define DDBLT_DONOTWAIT 0x8000000u
#define BLT_SUPPORTED (DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT | DDBLT_COLORFILL | DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE | DDBLT_ROP | DDBLT_DDFX)
#define SRCCOPY_ 0x00CC0020u

#define DDBLTFAST_SRCCOLORKEY 1u
#define DDBLTFAST_DESTCOLORKEY 2u
#define DDLOCK_READONLY 0x10u
#define DDFLIP_NOVSYNC 8u
#define DDCKEY_COLORSPACE 1u
#define DDCKEY_DESTBLT 2u
#define DDCKEY_SRCBLT 8u
#define DDENUMRET_CANCEL 0u
#define DDEDM_REFRESHRATES 1u

#define DDCAPS_BLT 0x40u
#define DDCAPS_BLTSTRETCH 0x200u
#define DDCAPS_COLORKEY 0x00400000u
#define DDCAPS_BLTCOLORFILL 0x04000000u
#define DDCAPS_CANBLTSYSMEM 0x80000000u
#define DDCKEYCAPS_SRCBLT 0x200u
#define DDCKEYCAPS_SRCBLTCLRSPACE 0x400u
#define DDFXCAPS_BLTSHRINKX 0x400u
#define DDFXCAPS_BLTSHRINKY 0x1000u
#define DDFXCAPS_BLTSTRETCHX 0x4000u
#define DDFXCAPS_BLTSTRETCHY 0x10000u
#define DDPCAPS_8BIT 0x4u
#define DDPCAPS_ALLOW256 0x40u

enum { O_NONE = 0, O_DEVICE, O_SURFACE, O_CLIPPER, O_PALETTE };

typedef struct {
    uint32_t hwnd, coop, mode_w, mode_h;
    int mode_set, primary, surfaces, released;
} device_t;

typedef struct {
    int device, root, next, orphaned, lost, locked, dc_out, has_src_key, has_dst_key, clipper, palette;
    uint32_t caps, back_buffers, width, height, bits, lock_flags;
    int32_t pitch;
    uint32_t pf[8];
    uint32_t src_key[2], dst_key[2], lock_rect[4];
} surface_t;

typedef struct {
    uint32_t hwnd;
} clipper_t;

typedef struct {
    uint32_t caps, entries[256];
} palette_t;

typedef struct {
    int used, kind, nfaces;
    uint32_t generation, faces;
    int32_t refs;
    union {
        device_t device;
        surface_t surface;
        clipper_t clipper;
        palette_t palette;
    } u;
} object_t;

typedef struct {
    const char* name;
    ht_fn_t fn;
    int argc;
} method_t;

static object_t s_obj[MAX_OBJECTS];
static uint32_t s_generation = 1;
static unsigned s_devices, s_surfaces, s_presents, s_restores;
static uint8_t* s_convert;
static size_t s_convert_cap;
static int s_logged_unsupported[64];

static const struct {
    uint32_t w, h;
} k_modes[] = { { 320, 200 },  { 320, 240 },  { 400, 300 },   { 512, 384 },   { 640, 400 },   { 640, 480 },   { 800, 600 },
                { 1024, 768 }, { 1152, 864 }, { 1280, 720 },  { 1280, 768 },  { 1280, 800 },  { 1280, 960 },  { 1280, 1024 },
                { 1360, 768 }, { 1366, 768 }, { 1600, 900 },  { 1600, 1200 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 },
                { 3840, 2160 } };

unsigned lx_ddraw_device_count(void) { return s_devices; }
unsigned lx_ddraw_surface_count(void) { return s_surfaces; }
unsigned lx_ddraw_restore_count(void) { return s_restores; }
unsigned lx_ddraw_present_count(void) { return s_presents; }

/* ---- objects and faces ---- */

static int slot_of_token(uint32_t token) {
    uint32_t slot = token & 0xFFu;
    if (!slot || slot > MAX_OBJECTS) return -1;
    return s_obj[slot - 1].used && s_obj[slot - 1].generation == (token >> 8) ? (int)slot - 1 : -1;
}

static uint32_t token_of(int slot) { return (s_obj[slot].generation << 8) | (uint32_t)(slot + 1); }

static uint32_t vtable_for(int kind, int version);

static int new_object(int kind, int nfaces, const int* versions) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        uint32_t face[4];
        object_t* o = &s_obj[i];
        if (o->used) continue;
        memset(o, 0, sizeof *o);
        o->faces = gm_heap_alloc(FACE_BYTES * (uint32_t)nfaces, 1);
        if (!o->faces) return -1;
        o->used = 1, o->kind = kind, o->nfaces = nfaces, o->refs = 1;
        o->generation = s_generation++ & 0xFFFFFFu;
        if (!o->generation) o->generation = s_generation++;
        for (int f = 0; f < nfaces; f++) {
            face[0] = vtable_for(kind, versions[f]), face[1] = token_of(i), face[2] = (uint32_t)versions[f], face[3] = FACE_MAGIC;
            gp_copy_out(o->faces + FACE_BYTES * (uint32_t)f, face, sizeof face);
        }
        return i;
    }
    return -1;
}

static int object_of(uint32_t face_va, int kind, int* version) {
    uint32_t face[4];
    int slot;
    if (!gp_copy_in(face, face_va, sizeof face) || face[3] != FACE_MAGIC) return -1;
    slot = slot_of_token(face[1]);
    if (slot < 0 || s_obj[slot].kind != kind) return -1;
    if (version) *version = (int)face[2];
    return slot;
}

static uint32_t face_va(int slot, int index) { return s_obj[slot].faces + FACE_BYTES * (uint32_t)index; }

static device_t* dev(int slot) { return &s_obj[slot].u.device; }
static surface_t* surf(int slot) { return &s_obj[slot].u.surface; }

/* ---- pixel formats ---- */

static void format_rgb565(uint32_t pf[8]) {
    memset(pf, 0, 8 * sizeof pf[0]);
    pf[0] = 32, pf[1] = DDPF_RGB, pf[3] = 16, pf[4] = 0xF800, pf[5] = 0x07E0, pf[6] = 0x001F;
}

static int format_bytes(const uint32_t pf[8]) { return (int)((pf[3] + 7) / 8); }

static int format_supported(const uint32_t pf[8]) {
    if (!(pf[1] & DDPF_RGB)) return (pf[1] & DDPF_PALETTEINDEXED8) != 0;
    return pf[3] == 16 || pf[3] == 24 || pf[3] == 32;
}

static int format_equal(const uint32_t a[8], const uint32_t b[8]) { return memcmp(a + 1, b + 1, 7 * sizeof a[0]) == 0; }

static uint32_t channel_to_8(uint32_t pixel, uint32_t mask) {
    uint32_t shift = 0, max;
    if (!mask) return 0;
    while (!((mask >> shift) & 1)) shift++;
    max = mask >> shift;
    return (((pixel & mask) >> shift) * 255 + max / 2) / max;
}

static uint32_t channel_from_8(uint32_t value, uint32_t mask) {
    uint32_t shift = 0, max;
    if (!mask) return 0;
    while (!((mask >> shift) & 1)) shift++;
    max = mask >> shift;
    return (((value * max + 127) / 255) << shift) & mask;
}

static uint32_t convert_pixel(uint32_t v, const uint32_t from[8], const uint32_t to[8]) {
    return channel_from_8(channel_to_8(v, from[4]), to[4]) | channel_from_8(channel_to_8(v, from[5]), to[5]) |
           channel_from_8(channel_to_8(v, from[6]), to[6]);
}

/* ---- surface storage ---- */

static int alloc_pixels(surface_t* s) {
    uint32_t bytes = (uint32_t)format_bytes(s->pf);
    uint64_t size;
    s->pitch = (int32_t)((s->width * bytes + 3u) & ~3u);
    size = (uint64_t)(uint32_t)s->pitch * s->height;
    if (!bytes || size > 0x40000000u) return 0;
    s->bits = gm_heap_alloc((uint32_t)size, 1);
    return s->bits != 0;
}

static int new_surface(int device, uint32_t w, uint32_t h, const uint32_t pf[8], uint32_t caps) {
    static const int versions[4] = { 1, 2, 3, 4 };
    int slot = new_object(O_SURFACE, 4, versions);
    surface_t* s;
    if (slot < 0) return -1;
    s = surf(slot);
    s->device = device, s->root = -1, s->next = -1, s->clipper = -1, s->palette = -1;
    s->caps = caps, s->width = w, s->height = h;
    memcpy(s->pf, pf, sizeof s->pf);
    s->pf[0] = 32;
    if (!alloc_pixels(s)) {
        gm_heap_free(s_obj[slot].faces);
        s_obj[slot].used = 0;
        return -1;
    }
    dev(device)->surfaces++;
    s_surfaces++;
    return slot;
}

static void free_surface(int slot) {
    surface_t* s = surf(slot);
    if (s->bits) gm_heap_free(s->bits);
    if (dev(s->device)->surfaces > 0) dev(s->device)->surfaces--;
    gm_heap_free(s_obj[slot].faces);
    s_obj[slot].used = 0;
    if (s_surfaces) s_surfaces--;
}

static int is_front(int slot) { return (surf(slot)->caps & DDSCAPS_PRIMARYSURFACE) != 0; }

static uint32_t memory_caps(uint32_t requested) {
    return (requested & DDSCAPS_SYSTEMMEMORY) ? DDSCAPS_SYSTEMMEMORY : (DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
}

/* ---- presentation ---- */

static void present_surface(int slot) {
    const surface_t* s = surf(slot);
    lx_frame_t f;
    const uint8_t* mem = (const uint8_t*)gm_ptr_checked(s->bits, (uint32_t)s->pitch * s->height, 0);
    if (!mem) return;
    f.generation = 0;
    f.width = (int)s->width, f.height = (int)s->height;
    if (s->pf[3] == 16 && s->pf[4] == 0xF800 && s->pf[5] == 0x07E0 && s->pf[6] == 0x001F) {
        f.pixels = mem, f.pitch = s->pitch, f.format = LX_PF_RGB565;
    } else {
        size_t need = (size_t)s->width * s->height * 4;
        uint32_t bytes = (uint32_t)format_bytes(s->pf);
        if (s_convert_cap < need) {
            uint8_t* p = (uint8_t*)realloc(s_convert, need);
            if (!p) return;
            s_convert = p, s_convert_cap = need;
        }
        for (uint32_t y = 0; y < s->height; y++) {
            uint32_t* out = (uint32_t*)(s_convert + (size_t)y * s->width * 4);
            const uint8_t* row = mem + (size_t)y * (uint32_t)s->pitch;
            for (uint32_t x = 0; x < s->width; x++) {
                uint32_t v = 0;
                memcpy(&v, row + (size_t)x * bytes, bytes);
                out[x] = (channel_to_8(v, s->pf[4]) << 16) | (channel_to_8(v, s->pf[5]) << 8) | channel_to_8(v, s->pf[6]);
            }
        }
        f.pixels = s_convert, f.pitch = (int)(s->width * 4), f.format = LX_PF_XRGB8888;
    }
    s_presents++;
    lx_screen_present(&f);
}

/* ---- blits ---- */

static int rect_inside(const int32_t r[4], const surface_t* s) {
    return r[0] >= 0 && r[1] >= 0 && r[0] < r[2] && r[1] < r[3] && (uint32_t)r[2] <= s->width && (uint32_t)r[3] <= s->height;
}

static uint8_t* pixel_ptr(const surface_t* s, uint8_t* base, int32_t x, int32_t y) {
    return base + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)format_bytes(s->pf);
}

typedef struct {
    int use_key;
    uint32_t low, high;
} key_t_;

static uint32_t read_pixel(const uint8_t* p, int bytes) {
    uint32_t v = 0;
    memcpy(&v, p, (size_t)bytes);
    return v;
}

static uint32_t copy_rect(surface_t* dst, const int32_t d[4], surface_t* src, const int32_t sr[4], const key_t_* key) {
    int32_t dw = d[2] - d[0], dh = d[3] - d[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    int sb = format_bytes(src->pf), db = format_bytes(dst->pf), same = format_equal(src->pf, dst->pf);
    uint8_t* dbase = (uint8_t*)gm_ptr_checked(dst->bits, (uint32_t)dst->pitch * dst->height, 1);
    uint8_t* sbase = (uint8_t*)gm_ptr_checked(src->bits, (uint32_t)src->pitch * src->height, 0);
    uint8_t* temp = NULL;
    if (!dbase || !sbase) return DDERR_SURFACELOST_;
    if (!same && (src->pf[3] == 8 || dst->pf[3] == 8)) return DDERR_INVALIDPIXELFORMAT_;
    if (src == dst && d[0] < sr[2] && sr[0] < d[2] && d[1] < sr[3] && sr[1] < d[3]) {
        size_t n = (size_t)src->pitch * src->height;
        temp = (uint8_t*)malloc(n);
        if (!temp) return E_OUTOFMEMORY_;
        memcpy(temp, sbase, n);
        sbase = temp;
    }
    if (same && !key->use_key && dw == sw && dh == sh) {
        for (int32_t y = 0; y < dh; y++)
            memcpy(pixel_ptr(dst, dbase, d[0], d[1] + y), pixel_ptr(src, sbase, sr[0], sr[1] + y), (size_t)dw * (size_t)db);
    } else {
        for (int32_t y = 0; y < dh; y++) {
            int32_t sy = sr[1] + (int32_t)(((int64_t)y * sh) / dh);
            for (int32_t x = 0; x < dw; x++) {
                int32_t sx = sr[0] + (int32_t)(((int64_t)x * sw) / dw);
                uint32_t v = read_pixel(pixel_ptr(src, sbase, sx, sy), sb);
                if (key->use_key && v >= key->low && v <= key->high) continue;
                v = same ? v : convert_pixel(v, src->pf, dst->pf);
                memcpy(pixel_ptr(dst, dbase, d[0] + x, d[1] + y), &v, (size_t)db);
            }
        }
    }
    free(temp);
    return DD_OK;
}

static uint32_t fill_rect(surface_t* s, const int32_t r[4], uint32_t color) {
    int bytes = format_bytes(s->pf);
    uint8_t* base = (uint8_t*)gm_ptr_checked(s->bits, (uint32_t)s->pitch * s->height, 1);
    if (!base) return DDERR_SURFACELOST_;
    for (int32_t y = r[1]; y < r[3]; y++) {
        uint8_t* p = pixel_ptr(s, base, r[0], y);
        for (int32_t x = r[0]; x < r[2]; x++, p += bytes) memcpy(p, &color, (size_t)bytes);
    }
    return DD_OK;
}

static uint32_t blt(int dslot, const int32_t* dst_rect, int sslot, const int32_t* src_rect, uint32_t flags, const uint32_t* fx) {
    surface_t* dst = surf(dslot);
    surface_t* src = sslot >= 0 ? surf(sslot) : NULL;
    int32_t d[4] = { 0, 0, (int32_t)dst->width, (int32_t)dst->height }, s[4];
    key_t_ key = { 0, 0, 0 };
    uint32_t hr;
    if (flags & ~(uint32_t)BLT_SUPPORTED) {
        lx_log("Linux DDRAW", "Blt flags 0x%08X: only colour fill, source colour keys and SRCCOPY are emulated", flags);
        return E_NOTIMPL_;
    }
    if ((flags & (DDBLT_COLORFILL | DDBLT_KEYSRCOVERRIDE | DDBLT_ROP | DDBLT_DDFX)) && (!fx || fx[0] != 100)) return E_INVALIDARG_;
    if (((flags & DDBLT_ROP) && fx[2] != SRCCOPY_) || ((flags & DDBLT_DDFX) && fx[1])) {
        lx_log("Linux DDRAW", "Blt raster operation 0x%08X / effects 0x%08X are not emulated", (flags & DDBLT_ROP) ? fx[2] : 0,
               (flags & DDBLT_DDFX) ? fx[1] : 0);
        return E_NOTIMPL_;
    }
    if (dst->lost || (src && src->lost)) return DDERR_SURFACELOST_;
    if (dst->locked || dst->dc_out || (src && (src->locked || src->dc_out))) return DDERR_SURFACEBUSY_;
    if (dst_rect) memcpy(d, dst_rect, sizeof d);
    if (!rect_inside(d, dst)) return DDERR_INVALIDRECT_;
    if (flags & DDBLT_COLORFILL) {
        hr = fill_rect(dst, d, fx[20]);
        if (hr) return hr;
    } else {
        if (!src) return E_INVALIDARG_;
        s[0] = 0, s[1] = 0, s[2] = (int32_t)src->width, s[3] = (int32_t)src->height;
        if (src_rect) memcpy(s, src_rect, sizeof s);
        if (!rect_inside(s, src)) return DDERR_INVALIDRECT_;
        if (flags & DDBLT_KEYSRCOVERRIDE) key.use_key = 1, key.low = fx[23], key.high = fx[24];
        else if ((flags & DDBLT_KEYSRC) && src->has_src_key) key.use_key = 1, key.low = src->src_key[0], key.high = src->src_key[1];
        hr = copy_rect(dst, d, src, s, &key);
        if (hr) return hr;
    }
    if (is_front(dslot)) present_surface(dslot);
    return DD_OK;
}

/* ---- guest structures ---- */

static void describe(const surface_t* s, uint32_t* desc, uint32_t size) {
    memset(desc, 0, size);
    desc[0] = size;
    desc[1] = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc[2] = s->height, desc[3] = s->width, desc[4] = (uint32_t)s->pitch;
    memcpy(desc + 18, s->pf, sizeof s->pf);
    desc[26] = s->caps;
    if (s->back_buffers) desc[1] |= DDSD_BACKBUFFERCOUNT, desc[5] = s->back_buffers;
    if (s->has_src_key) desc[1] |= DDSD_CKSRCBLT, desc[16] = s->src_key[0], desc[17] = s->src_key[1];
    if (s->has_dst_key) desc[1] |= DDSD_CKDESTBLT, desc[12] = s->dst_key[0], desc[13] = s->dst_key[1];
}

static uint32_t desc_size(int version) { return version == 4 ? 124u : 108u; }

static int read_rect(uint32_t va, int32_t r[4]) { return gp_copy_in(r, va, 16); }

/* ---- IUnknown ---- */

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} guid_t;

static int guid_is(uint32_t va, uint32_t d1, uint16_t d2, uint16_t d3, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5,
                   uint8_t b6, uint8_t b7) {
    guid_t g;
    const uint8_t t[8] = { b0, b1, b2, b3, b4, b5, b6, b7 };
    return gp_copy_in(&g, va, sizeof g) && g.d1 == d1 && g.d2 == d2 && g.d3 == d3 && !memcmp(g.d4, t, 8);
}

#define IID_UNKNOWN(v) guid_is(v, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46)
#define IID_DD(v) guid_is(v, 0x6C14DB80, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60)
#define IID_DD2(v) guid_is(v, 0xB3A6F3E0, 0x2B43, 0x11CF, 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56)
#define IID_DD4(v) guid_is(v, 0x9C59509A, 0x39BD, 0x11D1, 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5)
#define IID_SURF1(v) guid_is(v, 0x6C14DB81, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60)
#define IID_SURF2(v) guid_is(v, 0x57805885, 0x6EEC, 0x11CF, 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27)
#define IID_SURF3(v) guid_is(v, 0xDA044E00, 0x69B2, 0x11D0, 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB)
#define IID_SURF4(v) guid_is(v, 0x0B2B8630, 0xAD35, 0x11D0, 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B)

static uint64_t obj_addref(const uint32_t* a, int kind) {
    int slot = object_of(a[0], kind, NULL);
    return slot < 0 ? 0 : (uint64_t)(uint32_t)++s_obj[slot].refs;
}

static uint64_t unsupported(const uint32_t* a, void* user) {
    const char* name = (const char*)user;
    int h = 0;
    (void)a;
    for (const char* p = name; *p; p++) h = (h * 31 + *p) & 63;
    if (!s_logged_unsupported[h]) {
        s_logged_unsupported[h] = 1;
        lx_log("Linux DDRAW", "%s is not implemented", name);
    }
    return E_NOTIMPL_;
}

/* ---- IDirectDraw ---- */

static void destroy_device_if_unused(int slot) {
    device_t* d = dev(slot);
    if (d->released && d->surfaces == 0 && s_obj[slot].used) {
        gm_heap_free(s_obj[slot].faces);
        s_obj[slot].used = 0;
        if (s_devices) s_devices--;
    }
}

static uint64_t dd_QueryInterface(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_DEVICE, NULL), face = -1;
    (void)u;
    if (slot < 0 || !a[2]) return E_INVALIDARG_;
    gp_write_u32(a[2], 0);
    if (IID_UNKNOWN(a[1]) || IID_DD(a[1])) face = 0;
    else if (IID_DD2(a[1])) face = 1;
    else if (IID_DD4(a[1])) face = 2;
    if (face < 0) {
        lx_log("Linux DDRAW", "QueryInterface on the DirectDraw object: interface not provided (Direct3D is not part of the software renderer)");
        return E_NOINTERFACE_;
    }
    s_obj[slot].refs++;
    gp_write_u32(a[2], face_va(slot, face));
    return DD_OK;
}

static uint64_t dd_AddRef(const uint32_t* a, void* u) {
    (void)u;
    return obj_addref(a, O_DEVICE);
}

static uint64_t dd_Release(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_DEVICE, NULL);
    (void)u;
    if (slot < 0) return 0;
    if (--s_obj[slot].refs > 0) return (uint64_t)(uint32_t)s_obj[slot].refs;
    dev(slot)->mode_set = 0;
    dev(slot)->released = 1;
    lx_log("Linux DDRAW", "DirectDraw object released (%d surfaces still alive)", dev(slot)->surfaces);
    destroy_device_if_unused(slot);
    return 0;
}

static uint64_t dd_CreateClipper(const uint32_t* a, void* u) {
    static const int versions[1] = { 1 };
    int slot;
    (void)u;
    if (a[3]) return CLASS_E_NOAGGREGATION_;
    if (!a[2]) return E_INVALIDARG_;
    slot = new_object(O_CLIPPER, 1, versions);
    if (slot < 0) return E_OUTOFMEMORY_;
    gp_write_u32(a[2], face_va(slot, 0));
    return DD_OK;
}

static uint64_t dd_CreatePalette(const uint32_t* a, void* u) {
    static const int versions[1] = { 1 };
    int slot;
    (void)u;
    if (a[4]) return CLASS_E_NOAGGREGATION_;
    if (!a[3]) return E_INVALIDARG_;
    slot = new_object(O_PALETTE, 1, versions);
    if (slot < 0) return E_OUTOFMEMORY_;
    s_obj[slot].u.palette.caps = a[1];
    if (a[2]) gp_copy_in(s_obj[slot].u.palette.entries, a[2], 256 * 4);
    gp_write_u32(a[3], face_va(slot, 0));
    return DD_OK;
}

static uint32_t create_flip_chain(int device, int front, uint32_t count, uint32_t caps) {
    int last = front;
    uint32_t member = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | memory_caps(caps);
    for (uint32_t i = 0; i < count; i++) {
        int back = new_surface(device, surf(front)->width, surf(front)->height, surf(front)->pf, member | (i == 0 ? DDSCAPS_BACKBUFFER : 0u));
        if (back < 0) {
            surf(last)->next = front;
            return E_OUTOFMEMORY_;
        }
        s_obj[back].refs = 0; /* the chain owns its back buffers; only a GetAttachedSurface reference counts */
        surf(back)->root = front;
        surf(last)->next = back;
        last = back;
    }
    surf(last)->next = front;
    surf(front)->back_buffers = count;
    return DD_OK;
}

static void destroy_root(int root) {
    int m = surf(root)->next;
    while (m >= 0 && m != root) {
        int next = surf(m)->next;
        surf(m)->next = -1;
        if (s_obj[m].refs <= 0) free_surface(m);
        else surf(m)->orphaned = 1;
        m = next;
    }
    if (dev(surf(root)->device)->primary == root + 1) dev(surf(root)->device)->primary = 0;
    {
        int device = surf(root)->device;
        free_surface(root);
        destroy_device_if_unused(device);
    }
}

static uint64_t create_surface(const uint32_t* a, int version) {
    int slot = object_of(a[0], O_DEVICE, NULL), s;
    uint32_t desc[31], caps, width, height, pf[8], out_face;
    device_t* d;
    if (slot < 0 || !a[1] || !a[2]) return E_INVALIDARG_;
    if (a[3]) return CLASS_E_NOAGGREGATION_;
    d = dev(slot);
    if (!gp_copy_in(desc, a[1], desc_size(version) < sizeof desc ? desc_size(version) : sizeof desc) || desc[0] != desc_size(version)) return E_INVALIDARG_;
    gp_write_u32(a[2], 0);
    caps = (desc[1] & DDSD_CAPS) ? desc[26] : DDSCAPS_OFFSCREENPLAIN;
    if (caps & (DDSCAPS_OVERLAY | DDSCAPS_MODEX | DDSCAPS_ZBUFFER)) {
        lx_log("Linux DDRAW", "CreateSurface caps 0x%08X: overlays, Mode X and z-buffers are not provided", caps);
        return DDERR_INVALIDCAPS_;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        lx_display_mode_t m = lx_screen_mode();
        if (!d->coop) return DDERR_NOCOOPERATIVELEVELSET_;
        if (d->primary) return DDERR_PRIMARYSURFACEALREADYEXISTS_;
        if (desc[1] & (DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT)) return E_INVALIDARG_;
        width = (uint32_t)m.width, height = (uint32_t)m.height;
        format_rgb565(pf);
    } else {
        if ((desc[1] & (DDSD_WIDTH | DDSD_HEIGHT)) != (DDSD_WIDTH | DDSD_HEIGHT) || !desc[3] || !desc[2]) return E_INVALIDARG_;
        width = desc[3], height = desc[2];
        if (desc[1] & DDSD_PIXELFORMAT) {
            memcpy(pf, desc + 18, sizeof pf);
            pf[0] = 32;
            if (!format_supported(pf)) {
                lx_log("Linux DDRAW", "CreateSurface: pixel format flags 0x%X, %u bits is not provided", pf[1], pf[3]);
                return DDERR_INVALIDPIXELFORMAT_;
            }
        } else {
            format_rgb565(pf);
        }
    }
    if (width > MAX_SURFACE_SIDE || height > MAX_SURFACE_SIDE) return DDERR_TOOBIGSIZE_;
    if (caps & DDSCAPS_FLIP) {
        if (!(caps & DDSCAPS_COMPLEX) || !(desc[1] & DDSD_BACKBUFFERCOUNT) || !desc[5]) return DDERR_INVALIDCAPS_;
        if ((caps & DDSCAPS_PRIMARYSURFACE) && !(d->coop & DDSCL_EXCLUSIVE)) return DDERR_NOEXCLUSIVEMODE_;
    }
    s = new_surface(slot, width, height, pf,
                    caps | memory_caps(caps) | ((caps & DDSCAPS_PRIMARYSURFACE) ? DDSCAPS_VISIBLE | ((caps & DDSCAPS_FLIP) ? DDSCAPS_FRONTBUFFER : 0u) : 0u));
    if (s < 0) return E_OUTOFMEMORY_;
    if (caps & DDSCAPS_FLIP) {
        uint32_t hr = create_flip_chain(slot, s, desc[5], caps);
        if (hr) {
            destroy_root(s);
            return hr;
        }
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) d->primary = s + 1;
    out_face = face_va(s, version == 4 ? 3 : 0);
    lx_log("Linux DDRAW", "%s surface %ux%u, %u bpp RGB %X/%X/%X, pitch %d, caps 0x%08X, %u back buffer%s at guest 0x%08X",
           (caps & DDSCAPS_PRIMARYSURFACE) ? "primary" : "offscreen", width, height, pf[3], pf[4], pf[5], pf[6], surf(s)->pitch, surf(s)->caps,
           surf(s)->back_buffers, surf(s)->back_buffers == 1 ? "" : "s", surf(s)->bits);
    gp_write_u32(a[2], out_face);
    return DD_OK;
}

static uint64_t dd_CreateSurface(const uint32_t* a, void* u) {
    (void)u;
    return create_surface(a, 1);
}

static uint64_t dd4_CreateSurface(const uint32_t* a, void* u) {
    (void)u;
    return create_surface(a, 4);
}

static int mode_listed(uint32_t w, uint32_t h) {
    for (size_t i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++)
        if (k_modes[i].w == w && k_modes[i].h == h) return 1;
    return 0;
}

static uint64_t enum_modes(const uint32_t* a, int version) {
    uint32_t size = desc_size(version), desc_va, filter[31];
    int have_filter = a[2] != 0;
    uint32_t ret = DD_OK;
    if (!a[4]) return E_INVALIDARG_;
    if (have_filter && (!gp_copy_in(filter, a[2], size < sizeof filter ? size : (uint32_t)sizeof filter) || filter[0] != size)) return E_INVALIDARG_;
    desc_va = gm_heap_alloc(size, 1);
    if (!desc_va) return E_OUTOFMEMORY_;
    for (size_t i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        uint32_t desc[31] = { 0 }, args[2];
        if (have_filter && (filter[1] & DDSD_WIDTH) && filter[3] != k_modes[i].w) continue;
        if (have_filter && (filter[1] & DDSD_HEIGHT) && filter[2] != k_modes[i].h) continue;
        if (have_filter && (filter[1] & DDSD_PIXELFORMAT) && filter[21] != DISPLAY_BPP) continue;
        desc[0] = size, desc[1] = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        desc[2] = k_modes[i].h, desc[3] = k_modes[i].w, desc[4] = k_modes[i].w * DISPLAY_BPP / 8;
        format_rgb565(desc + 18);
        if (a[1] & DDEDM_REFRESHRATES) desc[1] |= DDSD_REFRESHRATE, desc[6] = 60;
        gp_copy_out(desc_va, desc, size);
        args[0] = desc_va, args[1] = a[3];
        if (lx_call_guest(a[4], LX_STDCALL, 2, args, 0, NULL) == DDENUMRET_CANCEL) break;
    }
    gm_heap_free(desc_va);
    return ret;
}

static uint64_t dd_EnumDisplayModes(const uint32_t* a, void* u) {
    (void)u;
    return enum_modes(a, 1);
}

static uint64_t dd4_EnumDisplayModes(const uint32_t* a, void* u) {
    (void)u;
    return enum_modes(a, 4);
}

static uint64_t dd_FlipToGDISurface(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DD_OK;
}

static uint64_t dd_GetCaps(const uint32_t* a, void* u) {
    uint32_t caps[79], surface_caps;
    uint32_t size;
    (void)u;
    if (!a[1] && !a[2]) return E_INVALIDARG_;
    memset(caps, 0, sizeof caps);
    surface_caps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_OFFSCREENPLAIN |
                   DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_PALETTE | DDSCAPS_VISIBLE;
    caps[0] = 316;
    caps[1] = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM;
    caps[3] = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_SRCBLTCLRSPACE;
    caps[4] = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
    caps[6] = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
    caps[15] = caps[16] = 0x04000000u;
    caps[33] = surface_caps;
    caps[43] = caps[65] = caps[1], caps[44] = caps[66] = caps[3], caps[45] = caps[67] = caps[4];
    caps[54] = caps[1], caps[55] = caps[3], caps[56] = caps[4];
    for (int i = 0; i < 2; i++) {
        uint32_t va = a[1 + (uint32_t)i];
        if (!va) continue;
        if (!gp_read_u32(va, &size) || size < 116 || size > 512) return E_INVALIDARG_;
        gp_copy_out(va + 4, caps + 1, (size < 316 ? size : 316) - 4);
    }
    return DD_OK;
}

static uint64_t get_display_mode(const uint32_t* a, int version) {
    uint32_t size = desc_size(version), desc[31] = { 0 }, given = 0;
    lx_display_mode_t m = lx_screen_mode();
    if (!a[1] || !gp_read_u32(a[1], &given) || given != size) return E_INVALIDARG_;
    desc[0] = size, desc[1] = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc[2] = (uint32_t)m.height, desc[3] = (uint32_t)m.width, desc[4] = (uint32_t)m.width * DISPLAY_BPP / 8, desc[6] = 60;
    format_rgb565(desc + 18);
    return gp_copy_out(a[1], desc, size) ? DD_OK : E_INVALIDARG_;
}

static uint64_t dd_GetDisplayMode(const uint32_t* a, void* u) {
    (void)u;
    return get_display_mode(a, 1);
}

static uint64_t dd4_GetDisplayMode(const uint32_t* a, void* u) {
    (void)u;
    return get_display_mode(a, 4);
}

static uint64_t dd_GetFourCCCodes(const uint32_t* a, void* u) {
    (void)u;
    if (!a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], 0);
    return DD_OK;
}

static uint64_t dd_GetMonitorFrequency(const uint32_t* a, void* u) {
    (void)u;
    if (!a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], 60);
    return DD_OK;
}

static uint64_t dd_Initialize(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DDERR_ALREADYINITIALIZED_;
}

static uint64_t dd_RestoreDisplayMode(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_DEVICE, NULL);
    (void)u;
    if (slot >= 0 && dev(slot)->mode_set) {
        dev(slot)->mode_set = 0;
        s_restores++;
        lx_log("Linux DDRAW", "RestoreDisplayMode: virtual mode cleared");
    }
    return DD_OK;
}

static uint64_t dd_SetCooperativeLevel(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_DEVICE, NULL);
    uint32_t flags = a[2];
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if ((flags & DDSCL_EXCLUSIVE) && (!(flags & DDSCL_FULLSCREEN) || !a[1])) return E_INVALIDARG_;
    if ((flags & DDSCL_NORMAL) && (flags & DDSCL_EXCLUSIVE)) return E_INVALIDARG_;
    if (!(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_SETFOCUSWINDOW | DDSCL_SETDEVICEWINDOW))) return E_INVALIDARG_;
    if (a[1] && !lx_user32_window_exists(a[1])) return E_INVALIDARG_;
    if (a[1]) dev(slot)->hwnd = a[1];
    dev(slot)->coop = flags;
    lx_log("Linux DDRAW", "SetCooperativeLevel(hwnd 0x%08X, flags 0x%X): exclusive mode is emulated, never taken", a[1], flags);
    return DD_OK;
}

static uint64_t set_display_mode(const uint32_t* a, uint32_t refresh) {
    int slot = object_of(a[0], O_DEVICE, NULL);
    uint32_t w = a[1], h = a[2], bpp = a[3];
    device_t* d;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    d = dev(slot);
    if (bpp != DISPLAY_BPP || !mode_listed(w, h)) {
        lx_log("Linux DDRAW", "SetDisplayMode %ux%ux%u: not a virtual mode (16 bpp, a listed size)", w, h, bpp);
        return DDERR_INVALIDMODE_;
    }
    if (!d->mode_set || d->mode_w != w || d->mode_h != h) {
        d->mode_w = w, d->mode_h = h, d->mode_set = 1;
        lx_screen_set_mode((int)w, (int)h, (int)bpp);
        if (d->primary) {
            int m = d->primary - 1, member = m;
            do {
                if (surf(member)->width != w || surf(member)->height != h) surf(member)->lost = 1;
                member = surf(member)->next;
            } while (member >= 0 && member != m);
        }
    }
    lx_log("Linux DDRAW", "SetDisplayMode %ux%ux%u (refresh %u): virtual mode set, RGB565", w, h, bpp, refresh);
    return DD_OK;
}

static uint64_t dd_SetDisplayMode(const uint32_t* a, void* u) {
    (void)u;
    return set_display_mode(a, 0);
}

static uint64_t dd2_SetDisplayMode(const uint32_t* a, void* u) {
    (void)u;
    return set_display_mode(a, a[4]);
}

static uint64_t dd_WaitForVerticalBlank(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return E_NOTIMPL_;
}

static uint64_t dd2_GetAvailableVidMem(const uint32_t* a, void* u) {
    (void)u;
    if (!a[2] && !a[3]) return E_INVALIDARG_;
    if (a[2]) gp_write_u32(a[2], 0x10000000u);
    if (a[3]) gp_write_u32(a[3], 0x10000000u);
    return DD_OK;
}

static uint64_t dd4_RestoreAllSurfaces(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DD_OK;
}

static uint64_t dd4_TestCooperativeLevel(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_DEVICE, NULL);
    (void)u;
    return slot >= 0 && dev(slot)->coop ? DD_OK : DDERR_NOCOOPERATIVELEVELSET_;
}

static uint64_t dd4_GetDeviceIdentifier(const uint32_t* a, void* u) {
    char* id = (char*)gm_ptr_checked(a[1], DEVICE_IDENTIFIER_BYTES, 1);
    (void)u;
    if (!id) return E_INVALIDARG_;
    memset(id, 0, DEVICE_IDENTIFIER_BYTES);
    snprintf(id, 512, "Linux software DirectDraw compatibility layer");
    snprintf(id + 512, 512, "Virtual RGB565 display");
    return DD_OK;
}

/* ---- IDirectDrawSurface ---- */

static int surface_of(uint32_t face, int* version) { return object_of(face, O_SURFACE, version); }

static uint64_t s_QueryInterface(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), face = -1;
    (void)u;
    if (slot < 0 || !a[2]) return E_INVALIDARG_;
    gp_write_u32(a[2], 0);
    if (IID_UNKNOWN(a[1]) || IID_SURF1(a[1])) face = 0;
    else if (IID_SURF2(a[1])) face = 1;
    else if (IID_SURF3(a[1])) face = 2;
    else if (IID_SURF4(a[1])) face = 3;
    if (face < 0) {
        lx_log("Linux DDRAW", "surface QueryInterface: interface not provided");
        return E_NOINTERFACE_;
    }
    s_obj[slot].refs++;
    gp_write_u32(a[2], face_va(slot, face));
    return DD_OK;
}

static uint64_t s_AddRef(const uint32_t* a, void* u) {
    (void)u;
    return obj_addref(a, O_SURFACE);
}

static uint64_t s_Release(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0) return 0;
    if (--s_obj[slot].refs > 0) return (uint64_t)(uint32_t)s_obj[slot].refs;
    s_obj[slot].refs = 0;
    if (surf(slot)->orphaned) free_surface(slot);
    else if (surf(slot)->root < 0) destroy_root(slot);
    return 0;
}

static uint64_t s_Blt(const uint32_t* a, void* u) {
    int dslot = surface_of(a[0], NULL), sslot = -1;
    int32_t d[4], s[4];
    uint32_t fx[25] = { 0 };
    (void)u;
    if (dslot < 0) return DDERR_INVALIDOBJECT_;
    if (a[2]) {
        sslot = surface_of(a[2], NULL);
        if (sslot < 0) return DDERR_INVALIDOBJECT_;
    }
    if (a[1] && !read_rect(a[1], d)) return E_INVALIDARG_;
    if (a[3] && !read_rect(a[3], s)) return E_INVALIDARG_;
    if (a[5]) gp_copy_in(fx, a[5], sizeof fx);
    return blt(dslot, a[1] ? d : NULL, sslot, a[3] ? s : NULL, a[4], a[5] ? fx : NULL);
}

static uint64_t s_BltFast(const uint32_t* a, void* u) {
    int dslot = surface_of(a[0], NULL), sslot = surface_of(a[3], NULL);
    int32_t s[4], d[4];
    uint32_t fx[25] = { 100 }, flags = 0;
    (void)u;
    if (dslot < 0 || sslot < 0) return E_INVALIDARG_;
    if (surf(dslot)->clipper >= 0) return DDERR_BLTFASTCANTCLIP_;
    if (a[5] & DDBLTFAST_DESTCOLORKEY) {
        lx_log("Linux DDRAW", "BltFast with a destination colour key is not emulated");
        return E_NOTIMPL_;
    }
    s[0] = 0, s[1] = 0, s[2] = (int32_t)surf(sslot)->width, s[3] = (int32_t)surf(sslot)->height;
    if (a[4] && !read_rect(a[4], s)) return E_INVALIDARG_;
    d[0] = (int32_t)a[1], d[1] = (int32_t)a[2], d[2] = d[0] + (s[2] - s[0]), d[3] = d[1] + (s[3] - s[1]);
    if (a[5] & DDBLTFAST_SRCCOLORKEY) flags |= DDBLT_KEYSRC;
    return blt(dslot, d, sslot, s, flags, fx);
}

static uint64_t s_AddAttachedSurface(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    lx_log("Linux DDRAW", "AddAttachedSurface: only z-buffers attach, and the software renderer has none");
    return DDERR_CANNOTATTACHSURFACE_;
}

static uint64_t s_DeleteAttachedSurface(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DDERR_NOTFOUND_;
}

static void exchange_pixels(surface_t* x, surface_t* y) {
    uint32_t bits = x->bits;
    x->bits = y->bits;
    y->bits = bits;
}

static uint64_t s_Flip(const uint32_t* a, void* u) {
    int front = surface_of(a[0], NULL), target = -1, m;
    (void)u;
    if (front < 0) return DDERR_INVALIDOBJECT_;
    if (!(surf(front)->caps & DDSCAPS_FLIP) || surf(front)->root >= 0 || surf(front)->next < 0) return DDERR_NOTFLIPPABLE_;
    if (a[1]) {
        target = surface_of(a[1], NULL);
        if (target < 0 || surf(target)->root != front) return E_INVALIDARG_;
    }
    m = front;
    do {
        if (surf(m)->lost) return DDERR_SURFACELOST_;
        if (surf(m)->locked || surf(m)->dc_out) return DDERR_SURFACEBUSY_;
        m = surf(m)->next;
    } while (m != front);
    if (target >= 0) exchange_pixels(surf(front), surf(target));
    else
        for (m = front; surf(m)->next != front; m = surf(m)->next) exchange_pixels(surf(m), surf(surf(m)->next));
    if (is_front(front)) present_surface(front);
    return DD_OK;
}

static uint64_t s_GetAttachedSurface(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), version = 1;
    uint32_t caps = 0;
    (void)u;
    if (slot < 0 || !a[1] || !a[2]) return E_INVALIDARG_;
    surface_of(a[0], &version);
    gp_write_u32(a[2], 0);
    gp_read_u32(a[1], &caps);
    for (int m = surf(slot)->next; m >= 0 && m != slot; m = surf(m)->next) {
        if ((surf(m)->caps & caps) == caps) {
            s_obj[m].refs++;
            gp_write_u32(a[2], face_va(m, version - 1));
            return DD_OK;
        }
    }
    return DDERR_NOTFOUND_;
}

static uint64_t s_GetBltStatus(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DD_OK;
}

static uint64_t s_GetCaps(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), version = 1;
    uint32_t caps[4] = { 0, 0, 0, 0 };
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    surface_of(a[0], &version);
    caps[0] = surf(slot)->caps;
    return gp_copy_out(a[1], caps, version == 4 ? 16 : 4) ? DD_OK : E_INVALIDARG_;
}

static uint64_t s_GetClipper(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], 0);
    if (surf(slot)->clipper < 0) return DDERR_NOCLIPPERATTACHED_;
    s_obj[surf(slot)->clipper].refs++;
    gp_write_u32(a[1], face_va(surf(slot)->clipper, 0));
    return DD_OK;
}

static uint64_t s_GetColorKey(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0 || !a[2]) return E_INVALIDARG_;
    if (a[1] == DDCKEY_SRCBLT && surf(slot)->has_src_key) return gp_copy_out(a[2], surf(slot)->src_key, 8) ? DD_OK : E_INVALIDARG_;
    if (a[1] == DDCKEY_DESTBLT && surf(slot)->has_dst_key) return gp_copy_out(a[2], surf(slot)->dst_key, 8) ? DD_OK : E_INVALIDARG_;
    return DDERR_NOCOLORKEY_;
}

static uint64_t s_GetFlipStatus(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DD_OK;
}

static uint64_t s_GetPalette(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], 0);
    if (surf(slot)->palette < 0) return DDERR_NOPALETTEATTACHED_;
    s_obj[surf(slot)->palette].refs++;
    gp_write_u32(a[1], face_va(surf(slot)->palette, 0));
    return DD_OK;
}

static uint64_t s_GetPixelFormat(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    return gp_copy_out(a[1], surf(slot)->pf, 32) ? DD_OK : E_INVALIDARG_;
}

static uint64_t s_GetSurfaceDesc(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), version = 1;
    uint32_t desc[31], given = 0, size;
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    surface_of(a[0], &version);
    size = desc_size(version);
    if (!gp_read_u32(a[1], &given) || given != size) return E_INVALIDARG_;
    describe(surf(slot), desc, size);
    return gp_copy_out(a[1], desc, size) ? DD_OK : E_INVALIDARG_;
}

static uint64_t s_Initialize(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return DDERR_ALREADYINITIALIZED_;
}

static uint64_t s_IsLost(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    return slot >= 0 && surf(slot)->lost ? DDERR_SURFACELOST_ : DD_OK;
}

static uint64_t s_Lock(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), version = 1;
    surface_t* s;
    int32_t whole[4], rect[4];
    uint32_t desc[31], given = 0, size;
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    surface_of(a[0], &version);
    size = desc_size(version);
    s = surf(slot);
    if (!a[2] || !gp_read_u32(a[2], &given) || given != size) return E_INVALIDARG_;
    if (s->lost) return DDERR_SURFACELOST_;
    if (s->locked || s->dc_out) return DDERR_SURFACEBUSY_;
    whole[0] = 0, whole[1] = 0, whole[2] = (int32_t)s->width, whole[3] = (int32_t)s->height;
    memcpy(rect, whole, sizeof rect);
    if (a[1] && (!read_rect(a[1], rect) || !rect_inside(rect, s))) return DDERR_INVALIDRECT_;
    s->lock_flags = a[3];
    memcpy(s->lock_rect, rect, sizeof rect);
    describe(s, desc, size);
    desc[1] |= DDSD_LPSURFACE;
    desc[9] = s->bits + (uint32_t)rect[1] * (uint32_t)s->pitch + (uint32_t)rect[0] * (uint32_t)format_bytes(s->pf);
    s->locked = 1;
    return gp_copy_out(a[2], desc, size) ? DD_OK : E_INVALIDARG_;
}

static uint64_t s_Unlock(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if (!surf(slot)->locked) return DDERR_NOTLOCKED_;
    surf(slot)->locked = 0;
    if (is_front(slot)) present_surface(slot);
    return DD_OK;
}

static uint64_t s_GetDC(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    surface_t* s;
    uint32_t dc;
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    s = surf(slot);
    if (!a[1]) return E_INVALIDARG_;
    if (s->lost) return DDERR_SURFACELOST_;
    if (s->dc_out) return DDERR_DCALREADYCREATED_;
    if (s->locked) return DDERR_SURFACEBUSY_;
    dc = lx_gdi_surface_dc(s->bits, (int)s->width, (int)s->height, s->pitch, (int)s->pf[3], s->pf[5]);
    if (!dc) return DDERR_CANTCREATEDC_;
    s->dc_out = (int)dc;
    return gp_write_u32(a[1], dc) ? DD_OK : E_INVALIDARG_;
}

static uint64_t s_ReleaseDC(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    surface_t* s;
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    s = surf(slot);
    if (!s->dc_out || a[1] != (uint32_t)s->dc_out) return E_INVALIDARG_;
    lx_gdi_release_surface_dc(a[1]);
    s->dc_out = 0;
    if (is_front(slot)) present_surface(slot);
    return DD_OK;
}

static uint64_t s_Restore(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    lx_display_mode_t m = lx_screen_mode();
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if (!surf(slot)->lost) return DD_OK;
    if (surf(slot)->width != (uint32_t)m.width || surf(slot)->height != (uint32_t)m.height) return DDERR_WRONGMODE_;
    surf(slot)->lost = 0;
    return DD_OK;
}

static uint64_t s_SetClipper(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), clipper = -1;
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if (a[1]) {
        clipper = object_of(a[1], O_CLIPPER, NULL);
        if (clipper < 0) return DDERR_INVALIDOBJECT_;
        s_obj[clipper].refs++;
    }
    if (surf(slot)->clipper >= 0) s_obj[surf(slot)->clipper].refs--;
    surf(slot)->clipper = clipper;
    return DD_OK;
}

static uint64_t s_SetColorKey(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    uint32_t which = a[1] & ~DDCKEY_COLORSPACE, key[2] = { 0, 0 };
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if (which != DDCKEY_SRCBLT && which != DDCKEY_DESTBLT) {
        lx_log("Linux DDRAW", "SetColorKey flags 0x%X: overlay colour keys are not emulated", a[1]);
        return E_NOTIMPL_;
    }
    if (a[2]) {
        if (!gp_copy_in(key, a[2], 8)) return E_INVALIDARG_;
        if (!(a[1] & DDCKEY_COLORSPACE)) key[1] = key[0];
    }
    if (which == DDCKEY_SRCBLT) {
        surf(slot)->has_src_key = a[2] != 0;
        if (a[2]) memcpy(surf(slot)->src_key, key, sizeof key);
    } else {
        surf(slot)->has_dst_key = a[2] != 0;
        if (a[2]) memcpy(surf(slot)->dst_key, key, sizeof key);
    }
    return DD_OK;
}

static uint64_t s_SetPalette(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL), palette = -1;
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    if (a[1]) {
        palette = object_of(a[1], O_PALETTE, NULL);
        if (palette < 0) return DDERR_INVALIDOBJECT_;
        if (surf(slot)->pf[3] != 8) return DDERR_INVALIDPIXELFORMAT_;
        s_obj[palette].refs++;
    }
    if (surf(slot)->palette >= 0) s_obj[surf(slot)->palette].refs--;
    surf(slot)->palette = palette;
    return DD_OK;
}

static uint64_t s2_GetDDInterface(const uint32_t* a, void* u) {
    int slot = surface_of(a[0], NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    s_obj[surf(slot)->device].refs++;
    gp_write_u32(a[1], face_va(surf(slot)->device, 0));
    return DD_OK;
}

/* ---- IDirectDrawClipper and IDirectDrawPalette ---- */

static uint64_t c_QueryInterface(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return E_NOINTERFACE_;
}

static uint64_t c_AddRef(const uint32_t* a, void* u) {
    (void)u;
    return obj_addref(a, O_CLIPPER);
}

static uint64_t c_Release(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_CLIPPER, NULL);
    (void)u;
    if (slot < 0) return 0;
    if (--s_obj[slot].refs > 0) return (uint64_t)(uint32_t)s_obj[slot].refs;
    gm_heap_free(s_obj[slot].faces);
    s_obj[slot].used = 0;
    return 0;
}

static uint64_t c_SetHWnd(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_CLIPPER, NULL);
    (void)u;
    if (slot < 0) return DDERR_INVALIDOBJECT_;
    s_obj[slot].u.clipper.hwnd = a[2];
    return DD_OK;
}

static uint64_t c_GetHWnd(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_CLIPPER, NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], s_obj[slot].u.clipper.hwnd);
    return DD_OK;
}

static uint64_t p_QueryInterface(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return E_NOINTERFACE_;
}

static uint64_t p_AddRef(const uint32_t* a, void* u) {
    (void)u;
    return obj_addref(a, O_PALETTE);
}

static uint64_t p_Release(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_PALETTE, NULL);
    (void)u;
    if (slot < 0) return 0;
    if (--s_obj[slot].refs > 0) return (uint64_t)(uint32_t)s_obj[slot].refs;
    gm_heap_free(s_obj[slot].faces);
    s_obj[slot].used = 0;
    return 0;
}

static uint64_t p_GetCaps(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_PALETTE, NULL);
    (void)u;
    if (slot < 0 || !a[1]) return E_INVALIDARG_;
    gp_write_u32(a[1], s_obj[slot].u.palette.caps);
    return DD_OK;
}

static uint64_t p_GetEntries(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_PALETTE, NULL);
    (void)u;
    if (slot < 0 || !a[4] || a[2] > 256 || a[3] > 256 - a[2]) return E_INVALIDARG_;
    return gp_copy_out(a[4], s_obj[slot].u.palette.entries + a[2], a[3] * 4) ? DD_OK : E_INVALIDARG_;
}

static uint64_t p_SetEntries(const uint32_t* a, void* u) {
    int slot = object_of(a[0], O_PALETTE, NULL);
    (void)u;
    if (slot < 0 || !a[4] || a[2] > 256 || a[3] > 256 - a[2]) return E_INVALIDARG_;
    return gp_copy_in(s_obj[slot].u.palette.entries + a[2], a[4], a[3] * 4) ? DD_OK : E_INVALIDARG_;
}

/* ---- the vtables ---- */

#define DDM(name, argc) { "IDirectDraw::" #name, dd_##name, argc }
#define DDU(name, argc) { "IDirectDraw::" #name, NULL, argc }
#define SM(name, argc) { "IDirectDrawSurface::" #name, s_##name, argc }
#define SU(name, argc) { "IDirectDrawSurface::" #name, NULL, argc }

static const method_t k_dd1[] = {
    DDM(QueryInterface, 3), DDM(AddRef, 1), DDM(Release, 1), DDU(Compact, 1), DDM(CreateClipper, 4), DDM(CreatePalette, 5),
    DDM(CreateSurface, 4), DDU(DuplicateSurface, 3), DDM(EnumDisplayModes, 5), DDU(EnumSurfaces, 5), DDM(FlipToGDISurface, 1),
    DDM(GetCaps, 3), DDM(GetDisplayMode, 2), DDM(GetFourCCCodes, 3), DDU(GetGDISurface, 2), DDM(GetMonitorFrequency, 2),
    DDU(GetScanLine, 2), DDU(GetVerticalBlankStatus, 2), DDM(Initialize, 2), DDM(RestoreDisplayMode, 1),
    DDM(SetCooperativeLevel, 3), DDM(SetDisplayMode, 4), DDM(WaitForVerticalBlank, 3),
};

static const method_t k_dd2_extra[] = { { "IDirectDraw2::GetAvailableVidMem", dd2_GetAvailableVidMem, 4 } };
static const method_t k_dd4_extra[] = {
    { "IDirectDraw4::GetSurfaceFromDC", NULL, 3 },
    { "IDirectDraw4::RestoreAllSurfaces", dd4_RestoreAllSurfaces, 1 },
    { "IDirectDraw4::TestCooperativeLevel", dd4_TestCooperativeLevel, 1 },
    { "IDirectDraw4::GetDeviceIdentifier", dd4_GetDeviceIdentifier, 3 },
};

static const method_t k_surface1[] = {
    SM(QueryInterface, 3), SM(AddRef, 1), SM(Release, 1), SM(AddAttachedSurface, 2), SU(AddOverlayDirtyRect, 2), SM(Blt, 6),
    SU(BltBatch, 4), SM(BltFast, 6), SM(DeleteAttachedSurface, 3), SU(EnumAttachedSurfaces, 3), SU(EnumOverlayZOrders, 4),
    SM(Flip, 3), SM(GetAttachedSurface, 3), SM(GetBltStatus, 2), SM(GetCaps, 2), SM(GetClipper, 2), SM(GetColorKey, 3),
    SM(GetDC, 2), SM(GetFlipStatus, 2), SU(GetOverlayPosition, 3), SM(GetPalette, 2), SM(GetPixelFormat, 2),
    SM(GetSurfaceDesc, 2), SM(Initialize, 3), SM(IsLost, 1), SM(Lock, 5), SM(ReleaseDC, 2), SM(Restore, 1), SM(SetClipper, 2),
    SM(SetColorKey, 3), SU(SetOverlayPosition, 3), SM(SetPalette, 2), SM(Unlock, 2), SU(UpdateOverlay, 6),
    SU(UpdateOverlayDisplay, 2), SU(UpdateOverlayZOrder, 3),
};

static const method_t k_surface2_extra[] = {
    { "IDirectDrawSurface2::GetDDInterface", s2_GetDDInterface, 2 },
    { "IDirectDrawSurface2::PageLock", NULL, 2 },
    { "IDirectDrawSurface2::PageUnlock", NULL, 2 },
};
static const method_t k_surface3_extra[] = { { "IDirectDrawSurface3::SetSurfaceDesc", NULL, 3 } };
static const method_t k_surface4_extra[] = {
    { "IDirectDrawSurface4::SetPrivateData", NULL, 5 },    { "IDirectDrawSurface4::GetPrivateData", NULL, 4 },
    { "IDirectDrawSurface4::FreePrivateData", NULL, 2 },   { "IDirectDrawSurface4::GetUniquenessValue", NULL, 2 },
    { "IDirectDrawSurface4::ChangeUniquenessValue", NULL, 1 },
};

static const method_t k_clipper[] = {
    { "IDirectDrawClipper::QueryInterface", c_QueryInterface, 3 }, { "IDirectDrawClipper::AddRef", c_AddRef, 1 },
    { "IDirectDrawClipper::Release", c_Release, 1 },               { "IDirectDrawClipper::GetClipList", NULL, 4 },
    { "IDirectDrawClipper::GetHWnd", c_GetHWnd, 2 },               { "IDirectDrawClipper::Initialize", NULL, 3 },
    { "IDirectDrawClipper::IsClipListChanged", NULL, 2 },          { "IDirectDrawClipper::SetClipList", NULL, 3 },
    { "IDirectDrawClipper::SetHWnd", c_SetHWnd, 3 },
};

static const method_t k_palette[] = {
    { "IDirectDrawPalette::QueryInterface", p_QueryInterface, 3 }, { "IDirectDrawPalette::AddRef", p_AddRef, 1 },
    { "IDirectDrawPalette::Release", p_Release, 1 },               { "IDirectDrawPalette::GetCaps", p_GetCaps, 2 },
    { "IDirectDrawPalette::GetEntries", p_GetEntries, 5 },         { "IDirectDrawPalette::Initialize", NULL, 4 },
    { "IDirectDrawPalette::SetEntries", p_SetEntries, 5 },
};

static uint32_t build_vtable(const method_t* const* parts, const size_t* counts, int nparts, const method_t* overrides, size_t noverrides) {
    uint32_t total = 0, va, index = 0;
    for (int p = 0; p < nparts; p++) total += (uint32_t)counts[p];
    va = gm_heap_alloc(4 * total, 1);
    if (!va) return 0;
    for (int p = 0; p < nparts; p++)
        for (size_t i = 0; i < counts[p]; i++, index++) {
            method_t m = parts[p][i];
            for (size_t o = 0; o < noverrides; o++)
                if (!strcmp(overrides[o].name, m.name)) m = overrides[o];
            gp_write_u32(va + 4 * index, ht_register(m.name, m.fn ? m.fn : unsupported, m.argc, HT_STDCALL, (void*)m.name));
        }
    return va;
}

#define PART(a) a, sizeof a / sizeof a[0]

static uint32_t s_vtable[5][5];

static uint32_t vtable_for(int kind, int version) {
    uint32_t* slot = &s_vtable[kind][version < 0 || version > 4 ? 0 : version];
    if (*slot) return *slot;
    switch (kind) {
    case O_DEVICE: {
        static const method_t dd2_override[] = { { "IDirectDraw::SetDisplayMode", dd2_SetDisplayMode, 6 } };
        static const method_t dd4_override[] = { { "IDirectDraw::CreateSurface", dd4_CreateSurface, 4 },
                                                  { "IDirectDraw::EnumDisplayModes", dd4_EnumDisplayModes, 5 },
                                                  { "IDirectDraw::GetDisplayMode", dd4_GetDisplayMode, 2 },
                                                  { "IDirectDraw::SetDisplayMode", dd2_SetDisplayMode, 6 } };
        const method_t* parts[3] = { k_dd1, k_dd2_extra, k_dd4_extra };
        size_t counts[3] = { sizeof k_dd1 / sizeof k_dd1[0], sizeof k_dd2_extra / sizeof k_dd2_extra[0], sizeof k_dd4_extra / sizeof k_dd4_extra[0] };
        if (version == 1) *slot = build_vtable(parts, counts, 1, NULL, 0);
        else if (version == 2) *slot = build_vtable(parts, counts, 2, dd2_override, 1);
        else *slot = build_vtable(parts, counts, 3, dd4_override, 4);
        break;
    }
    case O_SURFACE: {
        const method_t* parts[5] = { k_surface1, k_surface2_extra, k_surface3_extra, k_surface4_extra, NULL };
        size_t counts[4] = { sizeof k_surface1 / sizeof k_surface1[0], sizeof k_surface2_extra / sizeof k_surface2_extra[0],
                             sizeof k_surface3_extra / sizeof k_surface3_extra[0], sizeof k_surface4_extra / sizeof k_surface4_extra[0] };
        int n = version <= 1 ? 1 : version == 2 ? 2 : version == 3 ? 3 : 4;
        *slot = build_vtable(parts, counts, n, NULL, 0);
        break;
    }
    case O_CLIPPER: {
        const method_t* parts[1] = { k_clipper };
        size_t counts[1] = { sizeof k_clipper / sizeof k_clipper[0] };
        *slot = build_vtable(parts, counts, 1, NULL, 0);
        break;
    }
    default: {
        const method_t* parts[1] = { k_palette };
        size_t counts[1] = { sizeof k_palette / sizeof k_palette[0] };
        *slot = build_vtable(parts, counts, 1, NULL, 0);
    }
    }
    return *slot;
}

/* ---- the entry points ---- */

static uint64_t d_DirectDrawCreate(const uint32_t* a, void* u) {
    static const int versions[3] = { 1, 2, 4 };
    int slot;
    (void)u;
    if (!a[1]) return E_INVALIDARG_;
    if (a[2]) return CLASS_E_NOAGGREGATION_;
    slot = new_object(O_DEVICE, 3, versions);
    if (slot < 0) return E_OUTOFMEMORY_;
    s_devices++;
    gp_write_u32(a[1], face_va(slot, 0));
    lx_log("Linux DDRAW", "DirectDrawCreate: DirectDraw object at guest 0x%08X (virtual RGB565 display)", face_va(slot, 0));
    return DD_OK;
}

static uint64_t d_DirectDrawEnumerateA(const uint32_t* a, void* u) {
    uint32_t text, args[4];
    static const char description[] = "Linux virtual display", name[] = "display";
    (void)u;
    if (!a[0]) return E_INVALIDARG_;
    text = gm_heap_alloc(sizeof description + sizeof name, 1);
    if (!text) return E_OUTOFMEMORY_;
    gp_copy_out(text, description, sizeof description);
    gp_copy_out(text + (uint32_t)sizeof description, name, sizeof name);
    args[0] = 0, args[1] = text, args[2] = text + (uint32_t)sizeof description, args[3] = a[1];
    lx_call_guest(a[0], LX_STDCALL, 4, args, 0, NULL);
    gm_heap_free(text);
    return DD_OK;
}

static const hle_entry_t k_table[] = {
    { "ddraw.dll", "DirectDrawCreate", d_DirectDrawCreate, 3, HT_STDCALL },
    { "ddraw.dll", "DirectDrawEnumerateA", d_DirectDrawEnumerateA, 2, HT_STDCALL },
};

const hle_entry_t* hle_ddraw_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}

void hle_ddraw_init(void) {
    memset(s_obj, 0, sizeof s_obj);
    memset(s_vtable, 0, sizeof s_vtable);
    memset(s_logged_unsupported, 0, sizeof s_logged_unsupported);
    s_generation = 1;
    s_devices = s_surfaces = s_presents = s_restores = 0;
}

void hle_ddraw_shutdown(void) {
    free(s_convert);
    s_convert = NULL;
    s_convert_cap = 0;
}
