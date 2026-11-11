/*
 * Marshalling between guest memory (32-bit addresses) and host code (64-bit pointers).
 *
 * Rules the 64-bit host keeps:
 *   - a pointer the guest holds is a guest_ptr_t; it becomes a host pointer only through
 *     gp_to_host, which checks the range;
 *   - a host pointer is stored into guest memory only through gp_store_ptr, which accepts it
 *     only if it points inside the guest window (so its VA is meaningful) and otherwise refuses
 *     and counts a leak; a raw 64-bit value goes through gp_store_u32, which refuses anything
 *     above 0xFFFFFFFF;
 *   - structures cross by value (gp_copy_in / gp_copy_out) using guest layouts, never by
 *     aliasing a host struct whose size or alignment differs on x64.
 */
#ifndef HOST64_GUEST_PTR_H
#define HOST64_GUEST_PTR_H

#include <stdint.h>
#include <string.h>

#include "guest_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t guest_ptr_t;
#define GUEST_NULL ((guest_ptr_t)0)

#define GP_STATIC_ASSERT(cond, name) typedef char gp_static_assert_##name[(cond) ? 1 : -1]

/* A guest structure's layout: 4-byte pointers and handles, i386 alignment. */
#define GP_GUEST_LAYOUT(type, size) GP_STATIC_ASSERT(sizeof(type) == (size), type##_size)

/* Refused stores of host values into guest memory (see gp_store_ptr / gp_store_u32). */
unsigned gp_leak_count(void);
void gp_leak_reset(void);
void gp_note_leak(const char* what, uint64_t value, guest_ptr_t where);
/* The last refused store, for a report. */
const char* gp_last_leak(uint64_t* value, guest_ptr_t* where);

static inline void* gp_to_host(guest_ptr_t va, uint32_t len) { return gm_ptr(va, len); }
static inline void* gp_to_host_checked(guest_ptr_t va, uint32_t len, int writable) {
    return gm_ptr_checked(va, len, writable);
}
static inline guest_ptr_t gp_to_guest(const void* p) { return gm_va(p); }
static inline int gp_is_guest(const void* p, size_t len) { return gm_contains(p, len); }

static inline int gp_read_u32(guest_ptr_t va, uint32_t* out) {
    const void* p = gm_ptr(va, 4);
    if (!p) return 0;
    memcpy(out, p, 4);
    return 1;
}

static inline int gp_write_u32(guest_ptr_t va, uint32_t v) {
    void* p = gm_ptr(va, 4);
    if (!p) return 0;
    memcpy(p, &v, 4);
    return 1;
}

/* A value the host computed, stored into a 32-bit guest slot. Anything that does not fit is
 * refused: on x64 that is how a host pointer would be truncated into the guest. */
static inline int gp_store_u32(guest_ptr_t dst, uint64_t value) {
    if (value > 0xFFFFFFFFull) {
        gp_note_leak("64-bit value", value, dst);
        return 0;
    }
    return gp_write_u32(dst, (uint32_t)value);
}

/* A host pointer stored as a guest pointer: only one that lies in guest memory. */
static inline int gp_store_ptr(guest_ptr_t dst, const void* host_ptr) {
    if (!host_ptr) return gp_write_u32(dst, 0);
    if (!gm_contains(host_ptr, 1)) {
        gp_note_leak("host pointer", (uint64_t)(uintptr_t)host_ptr, dst);
        return 0;
    }
    return gp_write_u32(dst, gm_va(host_ptr));
}

static inline int gp_copy_in(void* host_dst, guest_ptr_t src, uint32_t n) {
    const void* p = gm_ptr_checked(src, n, 0);
    if (!p) return 0;
    memcpy(host_dst, p, n);
    return 1;
}

static inline int gp_copy_out(guest_ptr_t dst, const void* host_src, uint32_t n) {
    void* p = gm_ptr_checked(dst, n, 1);
    if (!p) return 0;
    memcpy(p, host_src, n);
    return 1;
}

#define GP_COPY_IN(var, va) gp_copy_in(&(var), (va), (uint32_t)sizeof(var))
#define GP_COPY_OUT(va, var) gp_copy_out((va), &(var), (uint32_t)sizeof(var))

/* A NUL-terminated guest string of at most max bytes (terminator included), checked page by
 * page; NULL if unterminated or unmapped. */
const char* gp_string(guest_ptr_t va, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif
