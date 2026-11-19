/* The shared harness of the DirectSound tests: guest memory in an arena whose addresses are not the host's, a mixer
 * backend with no device, and calls made through the guest vtables as the retail code makes them. */
#ifndef TRESPASSER_DSOUND_GUEST_HARNESS_H
#define TRESPASSER_DSOUND_GUEST_HARNESS_H

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include "audio_sync.h"
#include "dsound_compat.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

/* ---- guest memory: an arena whose addresses have nothing to do with the host's ---------------------------------- */

#define GUEST_BASE 0x20000000u
#define ARENA_BYTES (8u << 20)
#define SCRATCH_BYTES (1u << 20)
#define BLOCK_LIMIT 8192
#define THUNK_BASE 0x7F000000u

static struct {
    uint8_t* memory;
    uint32_t used;
    struct {
        uint32_t address, size;
        int live;
    } blocks[BLOCK_LIMIT];
    int block_count, live_blocks, fail_alloc;
} G;

static inline void* arena_resolve(void* context, uint32_t address, uint32_t size) {
    (void)context;
    if (address < GUEST_BASE || address - GUEST_BASE > ARENA_BYTES || size > ARENA_BYTES - (address - GUEST_BASE)) return NULL;
    return G.memory + (address - GUEST_BASE);
}

static inline uint32_t arena_alloc(void* context, uint32_t size) {
    uint32_t at;
    (void)context;
    if (G.fail_alloc || G.block_count >= BLOCK_LIMIT) return 0;
    size = (size + 15u) & ~15u;
    if (size > ARENA_BYTES - SCRATCH_BYTES - G.used) return 0;
    at = G.used;
    G.used += size;
    G.blocks[G.block_count].address = GUEST_BASE + at;
    G.blocks[G.block_count].size = size;
    G.blocks[G.block_count].live = 1;
    G.block_count++;
    G.live_blocks++;
    return GUEST_BASE + at;
}

static inline void arena_release(void* context, uint32_t address) {
    (void)context;
    for (int i = 0; i < G.block_count; i++) {
        if (G.blocks[i].address != address || !G.blocks[i].live) continue;
        G.blocks[i].live = 0;
        G.live_blocks--;
        memset(G.memory + (address - GUEST_BASE), 0xDD, G.blocks[i].size);
        return;
    }
}

static inline uint32_t thunk_address(void* context, dsound_method_t method) {
    (void)context;
    return THUNK_BASE + (uint32_t)method * 16u;
}

static inline void put32(uint32_t address, uint32_t value) {
    void* host = arena_resolve(NULL, address, 4);
    if (host) memcpy(host, &value, 4);
}

static inline uint32_t get32(uint32_t address) {
    uint32_t value = 0xCDCDCDCDu;
    void* host = arena_resolve(NULL, address, 4);
    if (host) memcpy(&value, host, 4);
    return value;
}

/* Memory for the test's own arguments, outside the accounting of the layer's blocks. */
static inline uint32_t scratch(uint32_t bytes) {
    static uint32_t cursor;
    uint32_t at;
    bytes = (bytes + 15u) & ~15u;
    if (cursor + bytes > SCRATCH_BYTES) cursor = 0;
    at = GUEST_BASE + (ARENA_BYTES - SCRATCH_BYTES) + cursor;
    memset(G.memory + (at - GUEST_BASE), 0, bytes);
    cursor += bytes;
    return at;
}

static inline uint32_t float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

/* ---- a backend without a device: the test pulls the mixer, under a lock like SDL's callback thread would ---------- */

static struct {
    int opens, closes, fail_open, rate_changes;
    uint32_t last_rate;
    audio_mutex_t* lock;
} B;

static inline void test_lock(void* context) {
    (void)context;
    audio_mutex_lock(B.lock);
}

static inline void test_unlock(void* context) {
    (void)context;
    audio_mutex_unlock(B.lock);
}

static inline audio_mixer_t* backend_open(uint32_t rate) {
    audio_lock_ops_t ops = { NULL, test_lock, test_unlock };
    if (B.fail_open) return NULL;
    if (!B.lock) B.lock = audio_mutex_create();
    B.opens++;
    B.last_rate = rate;
    return audio_mixer_create(rate, &ops);
}

static inline void backend_set_rate(uint32_t rate) {
    B.rate_changes++;
    B.last_rate = rate;
    audio_mixer_set_rate(dsound_compat_mixer(), rate);
}

static inline void backend_close(void) {
    B.closes++;
    audio_mixer_destroy(dsound_compat_mixer());
}

static inline const char* backend_name(void) { return "test"; }

static const audio_backend_t k_backend = { backend_open, backend_set_rate, backend_close, backend_name };

static inline dsound_binding_t binding(void) {
    dsound_binding_t b;
    b.guest.context = NULL;
    b.guest.resolve = arena_resolve;
    b.guest.alloc = arena_alloc;
    b.guest.release = arena_release;
    b.method_address = thunk_address;
    b.context = NULL;
    return b;
}

/* ---- calling like the guest does -------------------------------------------------------------------------------- */

static const ds_guid_t k_iid_unknown = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const ds_guid_t k_iid_buffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_listener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_buffer_3d = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_ks_property_set = { 0x31EFAC30, 0x515C, 0x11D0, { 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93 } };
static const ds_guid_t k_iid_notify = { 0xB0210783, 0x89CD, 0x11D0, { 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 } };

static inline uint32_t guid_in_guest(const ds_guid_t* guid) {
    uint32_t at = scratch(sizeof *guid);
    void* host = arena_resolve(NULL, at, sizeof *guid);
    memcpy(host, guid, sizeof *guid);
    return at;
}

static inline uint32_t call_method(dsound_method_t method, uint32_t first, ...) {
    uint32_t words[8] = { first };
    va_list ap;
    uint32_t count = dsound_method_words(method);
    va_start(ap, first);
    for (uint32_t i = 1; i < count; i++) words[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return dsound_compat_call(method, words);
}

/* The retail call: this = a face, the method = the entry its vtable holds at the slot. */
static inline dsound_method_t method_at(uint32_t face, unsigned slot) {
    uint32_t vtable = get32(face), entry = get32(vtable + slot * 4u);
    return (dsound_method_t)((entry - THUNK_BASE) / 16u);
}

static inline uint32_t vcall(uint32_t face, unsigned slot, ...) {
    uint32_t words[8] = { face };
    va_list ap;
    dsound_method_t method = method_at(face, slot);
    uint32_t count = dsound_method_words(method);
    va_start(ap, slot);
    for (uint32_t i = 1; i < count; i++) words[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return dsound_compat_call(method, words);
}

#define DEV_QI 0
#define DEV_ADDREF 1
#define DEV_RELEASE 2
#define DEV_CREATE_BUFFER 3
#define DEV_GET_CAPS 4
#define DEV_DUPLICATE 5
#define DEV_SET_COOP 6
#define DEV_GET_SPEAKERS 8
#define DEV_SET_SPEAKERS 9
#define BUF_QI 0
#define BUF_ADDREF 1
#define BUF_RELEASE 2
#define BUF_GET_CAPS 3
#define BUF_GET_POSITION 4
#define BUF_GET_VOLUME 6
#define BUF_GET_PAN 7
#define BUF_GET_STATUS 9
#define BUF_LOCK 11
#define BUF_PLAY 12
#define BUF_SET_POSITION 13
#define BUF_SET_FORMAT 14
#define BUF_SET_VOLUME 15
#define BUF_SET_PAN 16
#define BUF_SET_FREQUENCY 17
#define BUF_STOP 18
#define BUF_UNLOCK 19
#define BUF_RESTORE 20
#define S3D_SET_CONE_ANGLES 13
#define S3D_SET_CONE_ORIENTATION 14
#define S3D_SET_CONE_OUTSIDE 15
#define S3D_SET_MIN_DISTANCE 17
#define S3D_SET_POSITION 19
#define LST_SET_ORIENTATION 13
#define LST_SET_POSITION 14
#define LST_COMMIT 17

static inline uint32_t make_device(void) {
    uint32_t out = scratch(4);
    uint32_t hr = call_method(DSM_Create, 0, out, 0);
    uint32_t device = get32(out);
    CHECK(hr == DS_OK && device, "DirectSoundCreate (%08X)", hr);
    return device;
}

static inline uint32_t make_secondary(uint32_t device, uint32_t flags, uint32_t rate, uint16_t channels, uint16_t bits, uint32_t bytes,
                               uint32_t* result) {
    uint32_t wfx = scratch(sizeof(ds_waveformatex_t)), desc = scratch(sizeof(ds_bufferdesc_t)), out = scratch(4);
    ds_waveformatex_t f;
    ds_bufferdesc_t d;
    uint32_t hr;
    memset(&f, 0, sizeof f);
    f.format_tag = DS_WAVE_FORMAT_PCM;
    f.channels = channels;
    f.samples_per_second = rate;
    f.bits_per_sample = bits;
    f.block_align = (uint16_t)(channels * bits / 8);
    f.bytes_per_second = rate * f.block_align;
    memcpy(arena_resolve(NULL, wfx, sizeof f), &f, sizeof f);
    memset(&d, 0, sizeof d);
    d.size = DS_BUFFERDESC_V1_SIZE;
    d.flags = flags;
    d.buffer_bytes = bytes;
    d.format_address = wfx;
    memcpy(arena_resolve(NULL, desc, sizeof d), &d, sizeof d);
    hr = vcall(device, DEV_CREATE_BUFFER, desc, out, 0u);
    if (result) *result = hr;
    return get32(out);
}

static inline uint32_t make_primary(uint32_t device, uint32_t flags, uint32_t* result) {
    uint32_t desc = scratch(sizeof(ds_bufferdesc_t)), out = scratch(4);
    ds_bufferdesc_t d;
    uint32_t hr;
    memset(&d, 0, sizeof d);
    d.size = DS_BUFFERDESC_V1_SIZE;
    d.flags = flags;
    memcpy(arena_resolve(NULL, desc, sizeof d), &d, sizeof d);
    hr = vcall(device, DEV_CREATE_BUFFER, desc, out, 0u);
    if (result) *result = hr;
    return get32(out);
}

static inline uint32_t lock_buffer(uint32_t b, uint32_t offset, uint32_t bytes, uint32_t flags, uint32_t* p1, uint32_t* n1, uint32_t* p2,
                            uint32_t* n2) {
    uint32_t a = scratch(16), hr = vcall(b, BUF_LOCK, offset, bytes, a, a + 4, a + 8, a + 12, flags);
    *p1 = get32(a);
    *n1 = get32(a + 4);
    *p2 = get32(a + 8);
    *n2 = get32(a + 12);
    return hr;
}

static inline void fill16(uint32_t b, uint32_t bytes, int16_t value) {
    uint32_t p1, n1, p2, n2;
    uint32_t hr = lock_buffer(b, 0, bytes, 0, &p1, &n1, &p2, &n2);
    CHECK(hr == DS_OK && p1 && n1 == bytes, "Lock for filling");
    if (hr == DS_OK) {
        int16_t* samples = (int16_t*)arena_resolve(NULL, p1, n1);
        for (uint32_t i = 0; i < n1 / 2; i++) samples[i] = value;
    }
    CHECK(vcall(b, BUF_UNLOCK, p1, n1, p2, n2) == DS_OK, "Unlock");
}

static inline uint32_t status_of(uint32_t b) {
    uint32_t at = scratch(4);
    CHECK(vcall(b, BUF_GET_STATUS, at) == DS_OK, "GetStatus");
    return get32(at);
}

static inline uint32_t position_of(uint32_t b, uint32_t* write) {
    uint32_t at = scratch(8);
    uint32_t hr = vcall(b, BUF_GET_POSITION, at, write ? at + 4 : 0u);
    CHECK(hr == DS_OK, "GetCurrentPosition");
    if (write) *write = get32(at + 4);
    return get32(at);
}

static inline void mix(float* out, unsigned frames) { audio_mixer_mix(dsound_compat_mixer(), out, frames); }
static inline int near_to(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

#endif
