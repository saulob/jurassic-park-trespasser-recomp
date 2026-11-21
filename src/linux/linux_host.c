#include <string.h>

#include "linux_host.h"

#include "guest_abi.h"
#include "guest_memory.h"
#include "jpt_vpath.h"
#include "linux_hle.h"
#include "linux_spine.h"

#define SCRATCH_SLOTS 4
#define KEEP_SLOTS 2048
#define KEEP_CHUNK 0x10000u

int host_perf_logging(void) { return 0; }

static struct {
    const void* host;
    uint32_t guest;
} s_keep[KEEP_SLOTS];
static uint32_t s_keep_chunk, s_keep_used;

uint32_t guest_keep(const void* data, uint32_t size) {
    uint32_t slot = (uint32_t)(((uintptr_t)data >> 4) * 2654435761u) % KEEP_SLOTS, va;
    for (uint32_t probe = 0; probe < KEEP_SLOTS; probe++, slot = (slot + 1) % KEEP_SLOTS) {
        if (s_keep[slot].host == data) return s_keep[slot].guest;
        if (!s_keep[slot].host) break;
    }
    if (!data || !size || size > KEEP_CHUNK || s_keep[slot].host) return 0;
    size = (size + 15u) & ~15u;
    if (!s_keep_chunk || s_keep_used + size > KEEP_CHUNK) {
        s_keep_chunk = gm_alloc_pages(0, KEEP_CHUNK, "host kept bytes");
        s_keep_used = 0;
        if (!s_keep_chunk) return 0;
    }
    va = s_keep_chunk + s_keep_used;
    memcpy(gm_ptr(va, size), data, size);
    s_keep_used += size;
    s_keep[slot].host = data;
    s_keep[slot].guest = va;
    return va;
}

int guest_host_path(const char* guest_path, char* out, size_t cap) {
    return hle_vpath() && guest_path && jpt_vpath_resolve(hle_vpath(), guest_path, 0, out, cap) == JPT_VP_OK;
}

int guest_read32(uint32_t va, uint32_t* out) {
    const void* p = gm_ptr_checked(va, 4, 0);
    if (!p) return 0;
    memcpy(out, p, 4);
    return 1;
}

int guest_readable(uint32_t va, uint32_t bytes) { return bytes == 0 || gm_ptr_checked(va, bytes, 0) != NULL; }

int guest_write32(uint32_t va, uint32_t value) {
    void* p = gm_ptr_checked(va, 4, 1);
    if (!p) return 0;
    memcpy(p, &value, 4);
    return 1;
}

int guest_read_string(uint32_t va, char* out, int cap) {
    int n = 0;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    while (n < cap - 1) {
        uint32_t at = va + (uint32_t)n, room = GM_PAGE - (at & (GM_PAGE - 1u)), want = (uint32_t)(cap - 1 - n);
        const char* src;
        if (want > room) want = room;
        src = (const char*)gm_ptr_checked(at, want, 0);
        if (!src) return 0;
        for (uint32_t i = 0; i < want; i++, n++) {
            out[n] = src[i];
            if (!src[i]) return 1;
        }
    }
    out[n] = 0;
    return 1;
}

int guest_call(uint32_t va, uint32_t self, const uint32_t* args, int count, int caller_pops, uint32_t* result) {
    uint32_t eax;
    if (!va || !lx_find_entry(va)) return 0;
    eax = lx_call_guest(va, caller_pops ? LX_CDECL : LX_THISCALL, count, args, self, NULL);
    if (result) *result = eax;
    return 1;
}

int guest_call_virtual(uint32_t self, uint32_t slot, const uint32_t* args, int count, uint32_t* result) {
    uint32_t vtable, fn;
    if (!self || !guest_read32(self, &vtable) || !guest_read32(vtable + slot, &fn)) return 0;
    return guest_call(fn, self, args, count, 0, result);
}

static uint32_t s_va[SCRATCH_SLOTS], s_size[SCRATCH_SLOTS];

uint32_t guest_scratch(unsigned slot, uint32_t size) {
    if (slot >= SCRATCH_SLOTS || !size) return 0;
    if (s_size[slot] < size) {
        uint32_t va = gm_alloc_pages(0, (size + GM_PAGE - 1u) & ~(GM_PAGE - 1u), "host scratch");
        if (!va) return 0;
        if (s_va[slot]) gm_release(s_va[slot]);
        s_va[slot] = va;
        s_size[slot] = (size + GM_PAGE - 1u) & ~(GM_PAGE - 1u);
    }
    return s_va[slot];
}

void lx_host_reset(void) {
    memset(s_keep, 0, sizeof s_keep);
    s_keep_chunk = s_keep_used = 0;
    memset(s_va, 0, sizeof s_va);
    memset(s_size, 0, sizeof s_size);
}
