/*
 * Which of the game's own windows is in front, read from its window stack: the gameplay view
 * (CGameWnd), a modal window over it (options, hint, controls, quit...) or a menu of its own.
 * Every pointer is checked first, so a half-built stack reads as "none". The only writes are the
 * game's own EndModal and quit flags, used by the host's close path.
 */
#include "guest_va.h"
#include "host.h"

#include <string.h>

#define DISPLAY_OBJECT GVA(0x0073DC40)
#define DISPLAY_RELEASED GVA(0x00669B90)

const char* guest_ui_class_name(uint32_t vtable) { return guest_ui_model_class_name(jpt_gva, vtable); }

static guest_wnd_t class_kind(uint32_t vtable) { return guest_ui_model_kind(jpt_gva, vtable); }

static int committed(uint32_t va, uint32_t bytes, DWORD wanted) {
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t lo = (uintptr_t)va, hi = lo + bytes;
    if (va < 0x10000u || hi < lo) return 0;
    if (!VirtualQuery((const void*)lo, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(mi.Protect & wanted))
        return 0;
    return hi <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

#define READABLE_PAGES (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)

/* The guest image stays mapped and readable for the whole run: once that is checked, reads in it need no query. */
static int image_readable(uintptr_t lo, uintptr_t hi) {
    static uintptr_t s_lo, s_hi;
    static int s_checked;
    if (!s_checked) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)JPT_GUEST_IMAGE_BASE;
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const BYTE*)dos + dos->e_lfanew);
        uintptr_t at = (uintptr_t)JPT_GUEST_IMAGE_BASE, end = at + nt->OptionalHeader.SizeOfImage;
        MEMORY_BASIC_INFORMATION mi;
        s_checked = 1;
        while (at < end && VirtualQuery((const void*)at, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
               !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mi.Protect & READABLE_PAGES))
            at = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (at >= end) {
            s_lo = (uintptr_t)JPT_GUEST_IMAGE_BASE;
            s_hi = end;
        }
    }
    return s_hi && lo >= s_lo && hi <= s_hi;
}

static int readable(uint32_t va, uint32_t bytes) {
    static uintptr_t s_lo, s_hi;
    uintptr_t lo = (uintptr_t)va, hi = lo + bytes;
    MEMORY_BASIC_INFORMATION mi;
    if (va >= 0x10000u && hi >= lo && ((lo >= s_lo && hi <= s_hi) || image_readable(lo, hi))) return 1;
    if (!committed(va, bytes, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
        return 0;
    VirtualQuery((const void*)lo, &mi, sizeof mi);
    s_lo = (uintptr_t)mi.BaseAddress;
    s_hi = s_lo + mi.RegionSize;
    return 1;
}

#define WRITABLE_PAGES (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)

/* The last writable region found is kept, as readable() does: the guest's own data and heap stay committed. */
static int writable(uint32_t va, uint32_t bytes) {
    static uintptr_t s_lo, s_hi;
    uintptr_t lo = (uintptr_t)va, hi = lo + bytes;
    MEMORY_BASIC_INFORMATION mi;
    if (va >= 0x10000u && hi >= lo && lo >= s_lo && hi <= s_hi) return 1;
    if (!committed(va, bytes, WRITABLE_PAGES)) return 0;
    VirtualQuery((const void*)lo, &mi, sizeof mi);
    s_lo = (uintptr_t)mi.BaseAddress;
    s_hi = s_lo + mi.RegionSize;
    return 1;
}

static int read32(uint32_t va, uint32_t* out) {
    if (!readable(va, 4)) return 0;
    *out = *(const volatile uint32_t*)(uintptr_t)va;
    return 1;
}

int guest_read32(uint32_t va, uint32_t* out) { return read32(va, out); }

int guest_readable(uint32_t va, uint32_t bytes) { return bytes == 0 || readable(va, bytes); }

int guest_read_string(uint32_t va, char* out, int cap) {
    int n = 0;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    while (n < cap - 1) {
        uint32_t at = va + (uint32_t)n, room = 0x1000u - (at & 0xFFFu), want = (uint32_t)(cap - 1 - n);
        const char* src = (const char*)(uintptr_t)at;
        if (want > room) want = room;
        if (!readable(at, want)) return 0;
        for (uint32_t i = 0; i < want; i++, n++) {
            out[n] = src[i];
            if (!src[i]) return 1;
        }
    }
    out[n] = 0;
    return 1;
}

int guest_write32(uint32_t va, uint32_t value) {
    if (!writable(va, 4)) return 0;
    *(volatile uint32_t*)(uintptr_t)va = value;
    return 1;
}

guest_wnd_t guest_ui_window_kind(uint32_t window) {
    uint32_t vtable;
    return window && read32(window, &vtable) ? class_kind(vtable) : GUEST_WND_NONE;
}

/* The game swaps its display object for an empty one while its window is inactive. */
int guest_display_built(void) {
    uint32_t display;
    return read32(DISPLAY_OBJECT, &display) && display && display != DISPLAY_RELEASED;
}

static int model_read(void* context, uint32_t va, uint32_t* out) {
    (void)context;
    return read32(va, out);
}

int guest_ui_probe(guest_ui_view_t* out) { return guest_ui_model_probe(jpt_gva, model_read, NULL, out); }

int guest_writable(uint32_t va, uint32_t bytes) { return bytes == 0 || writable(va, bytes); }
