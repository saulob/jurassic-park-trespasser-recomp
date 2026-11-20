/* The Linux DirectDraw layer, called through its guest COM objects the way the game calls it: a vtable of thunks in guest memory,
 * `this` first, structures at their 32-bit layouts. Covers the device (interfaces, caps, cooperative level, the virtual 16 bpp
 * display and its modes), the primary flip chain, Lock / Unlock / Flip / Blt / BltFast with colour keys, surface loss on a mode
 * change, stale objects, and the presented frame reaching the (virtual) host window unconverted. */
#include <stdlib.h>

#include "linux_ddraw.h"
#include "linux_user32.h"
#include "linux_gdi32.h"
#include "linux_win32_harness.h"

#define W 640
#define H 480
#define E_INVALIDARG 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define DDERR_NOCOOPERATIVELEVELSET 0x887600D4u
#define DDERR_INVALIDMODE 0x88760078u
#define DDERR_INVALIDOBJECT 0x88760082u
#define DDERR_SURFACEBUSY 0x887601AEu
#define DDERR_SURFACELOST 0x887601C2u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_WRONGMODE 0x8876024Bu
#define DDERR_NOTLOCKED 0x88760248u
#define DDERR_DCALREADYCREATED 0x8876026Cu

enum { DD_QI = 0, DD_ADDREF = 1, DD_RELEASE = 2, DD_CREATECLIPPER = 4, DD_CREATESURFACE = 6, DD_ENUMMODES = 8, DD_GETCAPS = 11,
       DD_GETDISPLAYMODE = 12, DD_SETCOOP = 20, DD_SETMODE = 21 };
enum { S_QI = 0, S_RELEASE = 2, S_BLT = 5, S_BLTFAST = 7, S_FLIP = 11, S_GETATTACHED = 12, S_GETDC = 17, S_RELEASEDC = 26, S_GETSURFACEDESC = 22, S_ISLOST = 24, S_LOCK = 25,
       S_RESTORE = 27, S_SETCOLORKEY = 29, S_UNLOCK = 32 };

static uint32_t s_hwnd, s_dd, s_front, s_back, s_stale_lock;

static void make_window(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    wc[9] = put_string(g_scratch, "DdrawTest");
    gp_copy_out(g_scratch + 0x40, wc, sizeof wc);
    U32(RegisterClassA, 1, g_scratch + 0x40);
    s_hwnd = U32(CreateWindowExA, 12, 0u, g_scratch, put_string(g_scratch + 0x100, "dd"), 0x90080000u, 0u, 0u, (uint32_t)W, (uint32_t)H, 0u, 0u,
                 0x00400000u, 0u);
}

static uint32_t put_guid(uint32_t where, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
    uint8_t g[16];
    memcpy(g, &d1, 4), memcpy(g + 4, &d2, 2), memcpy(g + 6, &d3, 2), memcpy(g + 8, d4, 8);
    gp_copy_out(where, g, sizeof g);
    return where;
}

static uint32_t iid_dd4(uint32_t where) {
    static const uint8_t d4[8] = { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 };
    return put_guid(where, 0x9C59509Au, 0x39BD, 0x11D1, d4);
}

static uint32_t desc(uint32_t where, uint32_t flags, uint32_t caps, uint32_t w, uint32_t h, uint32_t back) {
    uint32_t d[27] = { 0 };
    d[0] = 108, d[1] = flags, d[2] = h, d[3] = w, d[5] = back, d[26] = caps;
    gp_copy_out(where, d, sizeof d);
    return where;
}

static uint32_t lock(uint32_t surface, uint32_t out) {
    uint32_t d[27] = { 108 };
    gp_copy_out(out, d, sizeof d);
    return com(surface, S_LOCK, 5, 0u, out, 1u, 0u);
}

static void test_device(void) {
    uint32_t out = g_scratch + 0x1000, vtbl = 0, entry = 0, face2, caps = g_scratch + 0x1100, desc_va = g_scratch + 0x1400, refs;
    CHECK(DDC(DirectDrawCreate, 3, 0u, out, 0u) == 0, "DirectDrawCreate");
    s_dd = rd(out);
    CHECK(s_dd != 0 && gm_heap_ok(s_dd) && lx_ddraw_device_count() == 1, "a DirectDraw object in guest memory (0x%08X)", s_dd);
    gp_read_u32(s_dd, &vtbl);
    gp_read_u32(vtbl, &entry);
    CHECK(entry >= 0x7F000000u && ht_name(entry) && !strcmp(ht_name(entry), "IDirectDraw::QueryInterface"), "the vtable holds thunks of the guest's COM methods (%s)",
          ht_name(entry) ? ht_name(entry) : "?");
    CHECK(com(s_dd, DD_QI, 3, iid_dd4(g_scratch + 0x1200), g_scratch + 0x1240) == 0, "QueryInterface(IID_IDirectDraw4)");
    face2 = rd(g_scratch + 0x1240);
    CHECK(face2 != 0 && face2 != s_dd, "a separate face for the DirectDraw4 interface");
    gp_read_u32(face2, &vtbl);
    gp_read_u32(vtbl + 4 * 27, &entry);
    CHECK(ht_name(entry) && !strcmp(ht_name(entry), "IDirectDraw4::GetDeviceIdentifier"), "its vtable is the DirectDraw4 one (28 methods)");
    {
        uint8_t unknown[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(com(s_dd, DD_QI, 3, put_guid(g_scratch + 0x1300, 0x3BBA0080u, 0x2421, 0x11CF, unknown), g_scratch + 0x1340) == E_NOINTERFACE && rd(g_scratch + 0x1340) == 0,
              "an interface that is not provided is E_NOINTERFACE and the output is cleared");
    }
    refs = com(s_dd, DD_ADDREF, 1);
    CHECK(refs == 3 && com(s_dd, DD_RELEASE, 1) == 2, "reference counts are shared by the faces (%u)", refs);

    gp_write_u32(caps, 316);
    CHECK(com(s_dd, DD_GETCAPS, 3, caps, 0u) == 0 && (rd(caps + 4) & 0x40) && (rd(caps + 4) & 0x04000000u) && (rd(caps + 12) & 0x200),
          "GetCaps: blits, colour fill and source colour keys (0x%08X)", rd(caps + 4));
    gp_write_u32(caps, 12);
    CHECK(com(s_dd, DD_GETCAPS, 3, caps, 0u) == E_INVALIDARG, "GetCaps with an unknown structure size is refused");

    CHECK(com(s_dd, DD_SETCOOP, 3, s_hwnd, 0x10u) == E_INVALIDARG, "exclusive mode without FULLSCREEN is refused");
    CHECK(com(s_dd, DD_SETCOOP, 3, 0x7777u, 0x11u) == E_INVALIDARG, "SetCooperativeLevel with a window that does not exist is refused");
    CHECK(com(s_dd, DD_CREATESURFACE, 4, desc(desc_va, 0x21u, 0x218u, 0, 0, 1), g_scratch + 0x1500, 0u) == DDERR_NOCOOPERATIVELEVELSET,
          "no primary surface before a cooperative level");
    CHECK(com(s_dd, DD_SETCOOP, 3, s_hwnd, 0x11u) == 0, "SetCooperativeLevel(FULLSCREEN | EXCLUSIVE)");

    CHECK(com(s_dd, DD_SETMODE, 4, 640u, 480u, 8u) == DDERR_INVALIDMODE, "8 bpp is not a virtual mode");
    CHECK(com(s_dd, DD_SETMODE, 4, 641u, 480u, 16u) == DDERR_INVALIDMODE, "an unlisted size is not a virtual mode");
    CHECK(com(s_dd, DD_SETMODE, 4, 640u, 480u, 16u) == 0, "SetDisplayMode 640x480x16");
    gp_write_u32(desc_va, 108);
    CHECK(com(s_dd, DD_GETDISPLAYMODE, 2, desc_va) == 0 && rd(desc_va + 12) == W && rd(desc_va + 8) == H && rd(desc_va + 72 + 12) == 16 &&
              rd(desc_va + 72 + 16) == 0xF800 && rd(desc_va + 72 + 20) == 0x07E0 && rd(desc_va + 72 + 24) == 0x001F,
          "GetDisplayMode: 640x480 RGB565");
    g_fake_enum_count = 0;
    CHECK(com(s_dd, DD_ENUMMODES, 5, 0u, 0u, 0u, FAKE_ENUMCB) == 0 && g_fake_enum_count >= 8 && g_fake_enum_first_dword[0] == 320,
          "EnumDisplayModes calls the guest's callback for every virtual mode (%u)", g_fake_enum_count);
}

static void test_flip_chain(void) {
    static uint8_t expect[W * H * 2];
    uint32_t d = g_scratch + 0x2000, out = g_scratch + 0x2100, caps = g_scratch + 0x2200, back_out = g_scratch + 0x2300, locked = g_scratch + 0x2400;
    const lx_virtual_state_t* v = lx_virtual_state();
    uint32_t back_bits, front_bits_before, pitch;
    uint16_t* px;
    CHECK(com(s_dd, DD_CREATESURFACE, 4, desc(d, 0x21u, 0x218u, 0, 0, 1), out, 0u) == 0, "CreateSurface: primary with one back buffer (FLIP | COMPLEX | PRIMARYSURFACE)");
    s_front = rd(out);
    gp_write_u32(caps, 0x4);
    CHECK(com(s_front, S_GETATTACHED, 3, caps, back_out) == 0, "GetAttachedSurface(BACKBUFFER)");
    s_back = rd(back_out);
    CHECK(s_back != 0 && s_back != s_front && lx_ddraw_surface_count() == 2, "the back buffer is a second surface of the chain");

    gp_write_u32(d, 108);
    CHECK(com(s_front, S_GETSURFACEDESC, 2, d) == 0 && rd(d + 12) == W && rd(d + 8) == H && rd(d + 16) == W * 2, "GetSurfaceDesc: 640x480, pitch 1280");
    CHECK(lock(s_back, locked) == 0, "Lock the back buffer");
    back_bits = rd(locked + 36);
    pitch = rd(locked + 16);
    CHECK(pitch == W * 2 && back_bits != 0 && gm_heap_ok(back_bits) && (rd(locked + 4) & 0x800) != 0,
          "Lock hands the guest an address in guest memory with the pitch (0x%08X)", back_bits);
    CHECK(lock(s_back, g_scratch + 0x2500) == DDERR_SURFACEBUSY, "a locked surface is busy");
    CHECK(com(s_front, S_FLIP, 3, 0u, 1u) == DDERR_SURFACEBUSY, "a chain with a locked member does not flip");
    px = (uint16_t*)gm_ptr_checked(back_bits, W * H * 2, 1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t p = (uint16_t)((x * 7 + y * 13) & 0xFFFF);
            px[y * W + x] = p;
            expect[(y * W + x) * 2] = (uint8_t)p, expect[(y * W + x) * 2 + 1] = (uint8_t)(p >> 8);
        }
    CHECK(com(s_back, S_UNLOCK, 2, back_bits) == 0 && com(s_back, S_UNLOCK, 2, back_bits) == DDERR_NOTLOCKED, "Unlock, once");

    lock(s_front, locked);
    front_bits_before = rd(locked + 36);
    com(s_front, S_UNLOCK, 2, front_bits_before);
    {
        unsigned before = v->presents;
        CHECK(com(s_front, S_FLIP, 3, 0u, 1u) == 0, "Flip");
        CHECK(v->presents == before + 1 && v->last.format == LX_PF_RGB565 && v->last.width == W && v->last.height == H && v->last_checksum == fnv(expect, sizeof expect),
              "Flip presents the back buffer's pixels to the host window as RGB565, unconverted (0x%08X)", v->last_checksum);
    }
    lock(s_front, locked);
    CHECK(rd(locked + 36) == back_bits, "the front surface now owns the pixels that were in the back buffer");
    com(s_front, S_UNLOCK, 2, back_bits);
    lock(s_back, locked);
    CHECK(rd(locked + 36) == front_bits_before, "and the old front is the new back buffer");
    com(s_back, S_UNLOCK, 2, front_bits_before);
}

static void test_blits(void) {
    uint32_t d = g_scratch + 0x3000, out = g_scratch + 0x3100, fx = g_scratch + 0x3200, locked = g_scratch + 0x3300, src, back_bits, key = g_scratch + 0x3400;
    const lx_virtual_state_t* v = lx_virtual_state();
    uint16_t* px;
    uint32_t fxv[25] = { 100 };
    unsigned before;
    fxv[20] = 0x07E0;
    gp_copy_out(fx, fxv, sizeof fxv);
    CHECK(com(s_back, S_BLT, 6, 0u, 0u, 0u, 0x01000400u, fx) == 0, "Blt colour fill of the back buffer");
    lock(s_back, locked);
    back_bits = rd(locked + 36);
    px = (uint16_t*)gm_ptr_checked(back_bits, W * H * 2, 0);
    CHECK(px[0] == 0x07E0 && px[W * H - 1] == 0x07E0, "every pixel took the fill colour");
    com(s_back, S_UNLOCK, 2, back_bits);
    {
        uint32_t rc[4] = { 10, 10, 20, 20 };
        gp_copy_out(g_scratch + 0x3500, rc, sizeof rc);
        CHECK(com(s_back, S_BLT, 6, g_scratch + 0x3500, 0u, 0u, 0x01000400u, fx) == 0, "colour fill of a rectangle");
        CHECK(com(s_back, S_BLT, 6, 0u, 0u, 0u, 0x400u, 0u) == E_INVALIDARG, "colour fill needs the DDBLTFX");
        rc[2] = 700;
        gp_copy_out(g_scratch + 0x3500, rc, sizeof rc);
        CHECK(com(s_back, S_BLT, 6, g_scratch + 0x3500, 0u, 0u, 0x01000400u, fx) == 0x88760096u, "a rectangle outside the surface is DDERR_INVALIDRECT");
    }

    CHECK(com(s_dd, DD_CREATESURFACE, 4, desc(d, 0x7u, 0x840u, 4, 4, 0), out, 0u) == 0, "CreateSurface: a 4x4 offscreen system-memory surface");
    src = rd(out);
    lock(src, locked);
    px = (uint16_t*)gm_ptr_checked(rd(locked + 36), 4 * 4 * 2, 1);
    for (int i = 0; i < 16; i++) px[i] = i % 2 ? 0x001F : 0x0000;
    com(src, S_UNLOCK, 2, rd(locked + 36));
    gp_write_u32(key, 0);
    gp_write_u32(key + 4, 0);
    CHECK(com(src, S_SETCOLORKEY, 3, 8u, key) == 0, "SetColorKey(SRCBLT)");
    {
        uint32_t r[4] = { 0, 0, 4, 4 };
        gp_copy_out(g_scratch + 0x3600, r, sizeof r);
        CHECK(com(s_back, S_BLTFAST, 6, 100u, 100u, src, g_scratch + 0x3600, 1u) == 0, "BltFast with the source colour key");
    }
    lock(s_back, locked);
    px = (uint16_t*)gm_ptr_checked(rd(locked + 36), W * H * 2, 0);
    CHECK(px[100 * W + 100] == 0x07E0 && px[100 * W + 101] == 0x001F, "key-coloured pixels stay transparent, the others are copied");
    com(s_back, S_UNLOCK, 2, rd(locked + 36));

    before = v->presents;
    CHECK(com(s_front, S_BLT, 6, 0u, src, 0u, 0x01000000u, 0u) == 0 && v->presents == before + 1, "a Blt into the primary surface presents it (stretched 4x4)");

    gp_write_u32(fx + 4, 0x14);
    CHECK(com(s_back, S_BLT, 6, 0u, 0u, 0u, 0x01000800u, fx) != 0, "an effect that is not emulated is refused");
    {
        uint32_t vtbl = 0, lock_method = 0, args[5];
        gp_read_u32(src, &vtbl);
        gp_read_u32(vtbl + 4 * S_LOCK, &lock_method);
        CHECK(com(src, S_RELEASE, 1) == 0, "the offscreen surface releases");
        args[0] = src, args[1] = 0, args[2] = locked, args[3] = 1, args[4] = 0;
        CHECK(call_n(lock_method, 5, args) == DDERR_INVALIDOBJECT, "a released surface is a stale object");
    }
}

static void test_surface_dc(void) {
    uint32_t out = g_scratch + 0x3000, locked = g_scratch + 0x3100, text = g_scratch + 0x3200, dc = 0, bits;
    const uint16_t* px;
    CHECK(com(s_back, S_GETDC, 2, 0u) == 0x80070057u, "GetDC without a destination is an invalid argument");
    CHECK(com(s_back, S_GETDC, 2, out) == 0 && (dc = rd(out)) != 0 && dc < 0x01000000u, "GetDC returns a device context token, not a host pointer (0x%08X)", dc);
    CHECK(com(s_back, S_GETDC, 2, out) == DDERR_DCALREADYCREATED, "a second GetDC before ReleaseDC is refused");
    CHECK(lock(s_back, locked) == DDERR_SURFACEBUSY, "a surface with a device context out cannot be locked");
    G32(SetBkMode, 2, dc, 1u);
    G32(SetTextColor, 2, dc, 0x0000FFu);
    put_string(text, "Hello");
    CHECK(G32(TextOutA, 5, dc, 20u, 20u, text, 5u) == 1, "text is drawn through the surface's device context");
    CHECK(com(s_back, S_RELEASEDC, 2, dc + 1) == 0x80070057u, "ReleaseDC with another context is refused");
    CHECK(com(s_back, S_RELEASEDC, 2, dc) == 0, "ReleaseDC");
    CHECK(G32(TextOutA, 5, dc, 20u, 20u, text, 5u) == 0, "the context is gone after ReleaseDC");
    lock(s_back, locked);
    bits = rd(locked + 36);
    px = (const uint16_t*)gm_ptr_checked(bits, W * H * 2, 0);
    {
        int red = 0;
        for (int y = 20; y < 40; y++)
            for (int x = 20; x < 70; x++) red += px[y * W + x] == 0xF800;
        CHECK(red > 5, "the text landed in the surface's own pixels as RGB565 red (%d pixels)", red);
    }
    com(s_back, S_UNLOCK, 2, bits);
}

static void test_flip_stress(void) {
    enum { ROUNDS = 4000, FULL_EVERY = 250 };
    static uint8_t expect[W * H * 2];
    uint32_t locked = g_scratch + 0x4000, msg = g_scratch + 0x4200;
    const lx_virtual_state_t* v = lx_virtual_state();
    unsigned before = v->presents, bad_rotation = 0, bad_present = 0, bad_lock = 0, first_bad = 0, first_seen = 0;
    uint32_t previous_back = 0;
    for (int i = 0; i < ROUNDS; i++) {
        uint32_t bits, front_after;
        uint16_t value = (uint16_t)(i * 31 + 7);
        lx_event_t ev = { LX_EV_RESIZE, 640 + (i & 3) * 16, 480 + (i & 1) * 8, 0, 0, 0, 0 };
        if (lock(s_back, locked) != 0) {
            bad_lock++;
            continue;
        }
        bits = rd(locked + 36);
        if (i && bits == previous_back) bad_rotation++;
        {
            uint16_t* px = (uint16_t*)gm_ptr_checked(bits, W * H * 2, 1);
            int whole = i % FULL_EVERY == 0;
            for (int k = 0; k < (whole ? W * H : W); k++) px[k] = value;
            if (whole) {
                for (int k = 0; k < W * H; k++) expect[k * 2] = (uint8_t)value, expect[k * 2 + 1] = (uint8_t)(value >> 8);
            }
        }
        if (com(s_back, S_UNLOCK, 2, bits) != 0) bad_lock++;
        if (i % 3 == 0) lx_virtual_queue_event(&ev);
        U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u);
        if (com(s_front, S_FLIP, 3, 0u, 1u) != 0 || v->presents != before + 2 * (unsigned)i + 1) {
            if (!bad_present) first_bad = (unsigned)i, first_seen = v->presents - before;
            bad_present++;
        }
        if (i % FULL_EVERY == 0 && v->last_checksum != fnv(expect, sizeof expect)) bad_present++;
        lock(s_front, locked);
        front_after = rd(locked + 36);
        com(s_front, S_UNLOCK, 2, front_after);
        if (front_after != bits) bad_rotation++;
        previous_back = bits;
    }
    CHECK(bad_lock == 0, "%d Lock/Unlock pairs in a row on a flip chain all succeed (%u failed)", ROUNDS, bad_lock);
    CHECK(bad_present == 0, "a Flip presents one frame (the Unlock of the front surface another) and the presented pixels are the ones written (%u wrong, the first at round %u after %u presents)", bad_present, first_bad, first_seen);
    CHECK(bad_rotation == 0, "the chain exchanges its two pixel buffers on every Flip (%u wrong)", bad_rotation);
    CHECK(lx_ddraw_surface_count() >= 2, "no surface was lost or leaked by %d flips (%u live)", ROUNDS, lx_ddraw_surface_count());
}

static void test_mode_change_and_release(void) {
    CHECK(com(s_dd, DD_SETMODE, 4, 800u, 600u, 16u) == 0, "SetDisplayMode 800x600x16");
    CHECK(com(s_front, S_ISLOST, 1) == DDERR_SURFACELOST && com(s_back, S_ISLOST, 1) == DDERR_SURFACELOST, "surfaces of the old mode are lost");
    CHECK(com(s_front, S_RESTORE, 1) == DDERR_WRONGMODE, "Restore in a different mode fails");
    CHECK(com(s_dd, DD_SETMODE, 4, 640u, 480u, 16u) == 0 && com(s_front, S_RESTORE, 1) == 0 && com(s_front, S_ISLOST, 1) == 0, "back in 640x480 the surface restores");
    {
        uint32_t clipper_out = g_scratch + 0x3800, clipper;
        CHECK(com(s_dd, DD_CREATECLIPPER, 4, 0u, clipper_out, 0u) == 0, "CreateClipper");
        clipper = rd(clipper_out);
        CHECK(com(clipper, 8, 3, 0u, s_hwnd) == 0 && com(clipper, 4, 2, g_scratch + 0x3900) == 0 && rd(g_scratch + 0x3900) == s_hwnd, "SetHWnd / GetHWnd");
        CHECK(com(clipper, 2, 1) == 0, "the clipper releases");
    }
    {
        uint32_t vtbl = 0;
        gp_read_u32(s_front, &vtbl);
        gp_read_u32(vtbl + 4 * S_LOCK, &s_stale_lock);
    }
    CHECK(com(s_front, S_RELEASE, 1) == 0, "releasing the front surface releases its chain");
    CHECK(lx_ddraw_surface_count() == 1, "the back buffer is still alive for the guest that holds it (%u)", lx_ddraw_surface_count());
    CHECK(com(s_back, S_RELEASE, 1) == 0 && lx_ddraw_surface_count() == 0, "its last Release frees it");
    {
        uint32_t args[5] = { 0, 0, 0, 1, 0 };
        args[0] = s_front, args[2] = g_scratch + 0x3a00;
        CHECK(call_n(s_stale_lock, 5, args) == DDERR_INVALIDOBJECT, "the front token is stale");
    }
    CHECK(com(s_dd, DD_RELEASE, 1) == 1 && com(s_dd, DD_RELEASE, 1) == 0, "the device releases when its last reference goes");
    CHECK(lx_ddraw_device_count() == 0, "no device is left");
}

int main(void) {
    if (!harness_start()) return 2;
    make_window();
    test_device();
    test_flip_chain();
    test_blits();
    test_surface_dc();
    test_flip_stress();
    test_mode_change_and_release();
    harness_stop();
    printf("linux_ddraw_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
