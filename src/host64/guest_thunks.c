#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "guest_thunks.h"

#define MAX_THUNKS ((GM_THUNK_HI - GM_THUNK_LO) / HT_STRIDE)
#define MAX_OBJECTS 65536
#define HANDLE_TAG 0x48000000u /* handles never look like small integers or guest addresses */

typedef struct {
    const char* name;
    ht_fn_t fn;
    void* user;
    int argc, conv;
} thunk_t;

typedef struct {
    void* host;
    uint32_t guest;
} object_t;

static thunk_t* s_thunks;
static int s_nthunks;
static object_t s_objects[MAX_OBJECTS];
static int s_nobjects, s_live;
static SRWLOCK s_lock = SRWLOCK_INIT;

int ht_init(void) {
    if (!s_thunks) s_thunks = (thunk_t*)calloc(MAX_THUNKS, sizeof *s_thunks);
    return s_thunks != NULL;
}

void ht_reset(void) {
    AcquireSRWLockExclusive(&s_lock);
    if (s_thunks) memset(s_thunks, 0, MAX_THUNKS * sizeof *s_thunks);
    s_nthunks = 0;
    memset(s_objects, 0, sizeof s_objects);
    s_nobjects = s_live = 0;
    ReleaseSRWLockExclusive(&s_lock);
}

uint32_t ht_register(const char* name, ht_fn_t fn, int argc, int conv, void* user) {
    uint32_t va = 0;
    if (!fn || argc < 0 || argc > HT_MAX_ARGS || !ht_init()) return 0;
    AcquireSRWLockExclusive(&s_lock);
    if ((uint32_t)s_nthunks < MAX_THUNKS) {
        thunk_t* t = &s_thunks[s_nthunks];
        t->name = name;
        t->fn = fn;
        t->user = user;
        t->argc = argc;
        t->conv = conv;
        va = GM_THUNK_LO + HT_STRIDE * (uint32_t)s_nthunks++;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return va;
}

static const thunk_t* lookup(uint32_t va) {
    uint32_t i;
    if (va < GM_THUNK_LO || va >= GM_THUNK_HI || (va - GM_THUNK_LO) % HT_STRIDE) return NULL;
    i = (va - GM_THUNK_LO) / HT_STRIDE;
    return s_thunks && i < (uint32_t)s_nthunks ? &s_thunks[i] : NULL;
}

int ht_is_thunk(uint32_t va) { return lookup(va) != NULL; }

const char* ht_name(uint32_t va) {
    const thunk_t* t = lookup(va);
    return t ? t->name : NULL;
}

int ht_count(void) { return s_nthunks; }

int ht_invoke(uint32_t va, uint32_t* esp, uint32_t* eax, uint32_t* edx) {
    const thunk_t* t = lookup(va);
    uint32_t args[HT_MAX_ARGS];
    uint64_t r;
    if (!t) return 0;
    if (!gp_copy_in(args, *esp + 4, 4u * (uint32_t)t->argc) && t->argc) return 0;
    r = t->fn(args, t->user);
    *eax = (uint32_t)r;
    if (edx) *edx = (uint32_t)(r >> 32);
    *esp += 4u + (t->conv == HT_STDCALL ? 4u * (uint32_t)t->argc : 0u);
    return 1;
}

uint32_t ht_vtable(const ht_method_t* methods, int n, void* user) {
    uint32_t vt = gm_heap_alloc(4u * (uint32_t)n, 1);
    int i;
    if (!vt) return 0;
    for (i = 0; i < n; i++) {
        uint32_t va = ht_register(methods[i].name, methods[i].fn, methods[i].argc, HT_STDCALL, user);
        if (!va) {
            gm_heap_free(vt);
            return 0;
        }
        gp_write_u32(vt + 4u * (uint32_t)i, va);
    }
    return vt;
}

uint32_t ht_object_new(uint32_t vtable, uint32_t guest_size, void* host_obj) {
    uint32_t obj, slot = 0;
    int i;
    if (guest_size < 8 || !host_obj) return 0;
    obj = gm_heap_alloc(guest_size, 1);
    if (!obj) return 0;
    AcquireSRWLockExclusive(&s_lock);
    for (i = 0; i < s_nobjects; i++)
        if (!s_objects[i].host) break;
    if (i == s_nobjects && s_nobjects < MAX_OBJECTS) s_nobjects++;
    if (i < s_nobjects) {
        s_objects[i].host = host_obj;
        s_objects[i].guest = obj;
        s_live++;
        slot = HANDLE_TAG | (uint32_t)i;
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (!slot) {
        gm_heap_free(obj);
        return 0;
    }
    gp_write_u32(obj, vtable);
    gp_write_u32(obj + 4, slot);
    return obj;
}

static int slot_of(uint32_t guest_obj) {
    uint32_t h;
    if (!gp_read_u32(guest_obj + 4, &h) || !gm_ptr_checked(guest_obj, 8, 0)) return -1;
    if ((h & 0xFF000000u) != HANDLE_TAG || (h & 0x00FFFFFFu) >= (uint32_t)s_nobjects) return -1;
    return (int)(h & 0x00FFFFFFu);
}

void* ht_object_host(uint32_t guest_obj) {
    void* host = NULL;
    int i;
    AcquireSRWLockShared(&s_lock);
    i = slot_of(guest_obj);
    if (i >= 0 && s_objects[i].guest == guest_obj) host = s_objects[i].host;
    ReleaseSRWLockShared(&s_lock);
    return host;
}

uint32_t ht_object_of(const void* host_obj) {
    uint32_t g = 0;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < s_nobjects; i++)
        if (s_objects[i].host == host_obj) {
            g = s_objects[i].guest;
            break;
        }
    ReleaseSRWLockShared(&s_lock);
    return g;
}

int ht_object_free(uint32_t guest_obj) {
    int i, ok = 0;
    AcquireSRWLockExclusive(&s_lock);
    i = slot_of(guest_obj);
    if (i >= 0 && s_objects[i].guest == guest_obj) {
        s_objects[i].host = NULL;
        s_objects[i].guest = 0;
        s_live--;
        ok = 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (ok) {
        gp_write_u32(guest_obj + 4, 0);
        gm_heap_free(guest_obj);
    }
    return ok;
}

int ht_object_count(void) { return s_live; }
