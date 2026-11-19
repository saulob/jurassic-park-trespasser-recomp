#ifndef TRESPASSER_GUEST_OPS_H
#define TRESPASSER_GUEST_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The host's view of guest memory. A guest address is a 32-bit number; a host pointer is whatever the platform gives
 * for it and is never stored in guest-visible memory or squeezed into 32 bits. The 32-bit Windows host maps one onto
 * the other; a 64-bit host resolves through its guest window. Little-endian hosts only. */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the guest ABI is little-endian"
#endif

typedef struct {
    void* context;
    /* A host view of [address, address + size), NULL when any byte is outside guest memory. */
    void* (*resolve)(void* context, uint32_t address, uint32_t size);
    /* Zeroed guest memory aligned to 16 bytes; 0 when none is left. */
    uint32_t (*alloc)(void* context, uint32_t size);
    void (*release)(void* context, uint32_t address);
} jpt_guest_ops_t;

static inline int jpt_guest_ops_valid(const jpt_guest_ops_t* ops) { return ops && ops->resolve && ops->alloc && ops->release; }

static inline int jpt_guest_read(const jpt_guest_ops_t* ops, uint32_t address, void* out, uint32_t size) {
    const void* host = ops->resolve(ops->context, address, size);
    if (!host) return 0;
    memcpy(out, host, size);
    return 1;
}

static inline int jpt_guest_write(const jpt_guest_ops_t* ops, uint32_t address, const void* data, uint32_t size) {
    void* host = ops->resolve(ops->context, address, size);
    if (!host) return 0;
    memcpy(host, data, size);
    return 1;
}

static inline int jpt_guest_read32(const jpt_guest_ops_t* ops, uint32_t address, uint32_t* out) {
    return jpt_guest_read(ops, address, out, sizeof *out);
}

static inline int jpt_guest_write32(const jpt_guest_ops_t* ops, uint32_t address, uint32_t value) {
    return jpt_guest_write(ops, address, &value, sizeof value);
}

/* Copies a NUL-terminated guest string; 0 when it has no terminator within `capacity` bytes or leaves guest memory. */
static inline int jpt_guest_read_string(const jpt_guest_ops_t* ops, uint32_t address, char* out, uint32_t capacity) {
    for (uint32_t i = 0; i < capacity; i++) {
        if (!jpt_guest_read(ops, address + i, &out[i], 1)) return 0;
        if (!out[i]) return 1;
    }
    return 0;
}

static inline float jpt_word_to_float(uint32_t word) {
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

#endif
