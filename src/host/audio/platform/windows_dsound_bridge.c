#include "windows_dsound_bridge.h"

#include <malloc.h>
#include <string.h>

#include "../../abi/static_assert.h"

/* A 32-bit guest address and a host pointer are the same number only in a 32-bit process: this file is the 32-bit
 * Windows binding and refuses to build anywhere else. */
JPT_STATIC_ASSERT(sizeof(void*) == 4, windows_x86_bridge_is_32_bit);

#define STUB_PARAMS_1 uint32_t a0
#define STUB_PARAMS_2 STUB_PARAMS_1, uint32_t a1
#define STUB_PARAMS_3 STUB_PARAMS_2, uint32_t a2
#define STUB_PARAMS_4 STUB_PARAMS_3, uint32_t a3
#define STUB_PARAMS_5 STUB_PARAMS_4, uint32_t a4
#define STUB_PARAMS_6 STUB_PARAMS_5, uint32_t a5
#define STUB_PARAMS_7 STUB_PARAMS_6, uint32_t a6
#define STUB_PARAMS_8 STUB_PARAMS_7, uint32_t a7

#define STUB_LIST_1 a0
#define STUB_LIST_2 STUB_LIST_1, a1
#define STUB_LIST_3 STUB_LIST_2, a2
#define STUB_LIST_4 STUB_LIST_3, a3
#define STUB_LIST_5 STUB_LIST_4, a4
#define STUB_LIST_6 STUB_LIST_5, a5
#define STUB_LIST_7 STUB_LIST_6, a6
#define STUB_LIST_8 STUB_LIST_7, a7

#define DEFINE_STUB(name, words)                                                                                        \
    static uint32_t WINAPI stub_##name(STUB_PARAMS_##words) {                                                           \
        const uint32_t args[words] = { STUB_LIST_##words };                                                             \
        return dsound_compat_call(DSM_##name, args);                                                                    \
    }
DSOUND_ALL_METHODS(DEFINE_STUB)
#undef DEFINE_STUB

static const uint32_t k_entries[DSM_COUNT] = {
#define STUB_ENTRY(name, words) (uint32_t)(uintptr_t)stub_##name,
    DSOUND_ALL_METHODS(STUB_ENTRY)
#undef STUB_ENTRY
};

uint32_t jpt_dsound_bridge_entry(dsound_method_t method) { return (unsigned)method < DSM_COUNT ? k_entries[method] : 0; }

static uint32_t method_address(void* context, dsound_method_t method) {
    (void)context;
    return jpt_dsound_bridge_entry(method);
}

static void* identity_resolve(void* context, uint32_t address, uint32_t size) {
    (void)context;
    if (address < 0x10000u || size > 0xFFFFFFFFu - address) return NULL;
    return (void*)(uintptr_t)address;
}

static uint32_t identity_alloc(void* context, uint32_t size) {
    void* memory;
    (void)context;
    memory = _aligned_malloc(size ? size : 16u, 16);
    if (!memory) return 0;
    memset(memory, 0, size ? size : 16u);
    return (uint32_t)(uintptr_t)memory;
}

static void identity_release(void* context, uint32_t address) {
    (void)context;
    _aligned_free((void*)(uintptr_t)address);
}

static const jpt_guest_ops_t k_guest_ops = { NULL, identity_resolve, identity_alloc, identity_release };

const jpt_guest_ops_t* jpt_dsound_bridge_guest_ops(void) { return &k_guest_ops; }

int jpt_dsound_bridge_install(const audio_backend_t* backend) {
    dsound_binding_t binding;
    binding.guest = k_guest_ops;
    binding.method_address = method_address;
    binding.context = NULL;
    return dsound_compat_install(backend, &binding);
}

HRESULT WINAPI jpt_dsound_create(const void* device, void** out, void* outer) {
    return (HRESULT)stub_Create((uint32_t)(uintptr_t)device, (uint32_t)(uintptr_t)out, (uint32_t)(uintptr_t)outer);
}
