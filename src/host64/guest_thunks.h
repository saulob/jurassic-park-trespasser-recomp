/*
 * Host code the guest can call, and host objects the guest can hold, without a host pointer
 * ever entering guest memory.
 *
 * A thunk is a guest VA in [GM_THUNK_LO, GM_THUNK_HI) that is never committed: an import slot,
 * a COM vtable slot or a callback the host hands out holds one, and the dispatcher maps it to
 * a host function through a table. A host COM object is a guest block [vtable VA][handle][...]
 * whose handle indexes a host table; the guest copy of the handle is checked against the
 * object's own VA, so a forged or stale object resolves to nothing.
 */
#ifndef HOST64_GUEST_THUNKS_H
#define HOST64_GUEST_THUNKS_H

#include <stdint.h>

#include "guest_ptr.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { HT_STDCALL = 0, HT_CDECL = 1 };

/* The arguments as the guest pushed them (args[0] is the first, `this` for a COM method).
 * The low 32 bits of the result go to eax, the high 32 to edx. */
typedef uint64_t (*ht_fn_t)(const uint32_t* args, void* user);

#define HT_MAX_ARGS 16
#define HT_STRIDE 16u

int ht_init(void);
void ht_reset(void);

uint32_t ht_register(const char* name, ht_fn_t fn, int argc, int conv, void* user);
int ht_is_thunk(uint32_t va);
const char* ht_name(uint32_t va);
int ht_count(void);

/* Run the thunk at va as the guest's call instruction left it: [*esp] is the return address,
 * the arguments follow. Sets eax/edx and pops like the original convention (stdcall: return
 * address and arguments; cdecl: the return address only). 0 if va is not a thunk or the
 * stack is not readable. */
int ht_invoke(uint32_t va, uint32_t* esp, uint32_t* eax, uint32_t* edx);

/* A vtable in guest memory whose slots are thunks; returns its VA. */
typedef struct {
    const char* name;
    ht_fn_t fn;
    int argc;
} ht_method_t;
uint32_t ht_vtable(const ht_method_t* methods, int n, void* user);

/* Host objects behind guest COM pointers. guest_size covers the vtable pointer and the
 * handle (8 bytes) plus any fields the guest is allowed to read. */
uint32_t ht_object_new(uint32_t vtable, uint32_t guest_size, void* host_obj);
void* ht_object_host(uint32_t guest_obj);
uint32_t ht_object_of(const void* host_obj);
int ht_object_free(uint32_t guest_obj);
int ht_object_count(void);

#ifdef __cplusplus
}
#endif

#endif
