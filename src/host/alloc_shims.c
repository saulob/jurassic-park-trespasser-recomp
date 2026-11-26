/* The guest's own allocations: while a custom level plays, a failure ends its load instead of being used as memory. */
#include "engine_mode.h"
#include "host.h"
#include "jpt_custom_level.h"
#include "level_abort.h"

typedef void*(__cdecl* crt_alloc_t)(size_t);
typedef void*(__cdecl* crt_handler_t)(void*);
typedef int(__cdecl* crt_mode_t)(int);

static FARPROC crt(const char* name) {
    HMODULE module = GetModuleHandleA("msvcrt.dll");
    return module ? GetProcAddress(module, name) : NULL;
}

/* A commit past a reservation would take whatever lies behind it. */
static int in_one_reservation(uint32_t va, uint32_t bytes) {
    MEMORY_BASIC_INFORMATION first, last;
    if (!bytes || bytes - 1u > 0xFFFFFFFFu - va) return !bytes;
    if (!VirtualQuery((const void*)(uintptr_t)va, &first, sizeof first) || !VirtualQuery((const void*)(uintptr_t)(va + bytes - 1u), &last, sizeof last)) return 0;
    return first.State != MEM_FREE && last.State != MEM_FREE && first.AllocationBase == last.AllocationBase;
}

int guest_reservation_fits(uint32_t bytes) {
    void* p = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return p != NULL;
}

void shim_VirtualAlloc(void) {
    const uint32_t va = GUEST_ARG(0), type = GUEST_ARG(2), protect = GUEST_ARG(3);
    const uint32_t asked = GUEST_ARG(1);
    const uint32_t bytes = va ? asked : type == MEM_RESERVE ? engine_heaps_region_bytes(asked) : type == (MEM_RESERVE | MEM_COMMIT) ? engine_heaps_buffer_bytes(asked) : asked;
    const int custom = jpt_custom_level_active();
    void* p;
    if (custom && va && (type & MEM_COMMIT) && !(type & MEM_RESERVE) && !in_one_reservation(va, bytes)) level_limit_exceeded("a region of the engine", va, 0, bytes, 0);
    p = VirtualAlloc((void*)(uintptr_t)va, bytes, type, protect);
    if (!p && bytes && custom) level_allocation_failed(type & MEM_RESERVE ? "a reservation of the engine" : "a commit of the engine", va, bytes);
    g_eax = (uint32_t)(uintptr_t)p;
    GUEST_RETURN_STDCALL(4);
}

/* malloc, so that no exception crosses the lifted code; free releases both. */
static void allocate(const char* what, const char* original, crt_alloc_t* found) {
    static crt_alloc_t s_malloc;
    const uint32_t bytes = GUEST_ARG(0);
    const int custom = jpt_custom_level_active();
    void* p;
    if (!s_malloc) s_malloc = (crt_alloc_t)crt("malloc");
    if (!*found) *found = (crt_alloc_t)crt(original);
    p = custom ? s_malloc(bytes) : (*found)(bytes);
    if (!p && custom) level_allocation_failed(what, 0, bytes);
    g_eax = (uint32_t)(uintptr_t)p;
    GUEST_RETURN_STDCALL(0);
}

void shim_operator_new(void) {
    static crt_alloc_t s_new;
    allocate("operator new", "??2@YAPAXI@Z", &s_new);
}

void shim_malloc(void) {
    static crt_alloc_t s_malloc;
    allocate("malloc", "malloc", &s_malloc);
}

/* The game's handlers are guest code that the CRT cannot call. */
static void set_handler(const char* original) {
    g_eax = jpt_custom_level_active() ? 0 : (uint32_t)(uintptr_t)((crt_handler_t)crt(original))((void*)(uintptr_t)GUEST_ARG(0));
    GUEST_RETURN_STDCALL(0);
}

void shim_set_new_handler(void) { set_handler("?set_new_handler@@YAP6AXXZP6AXXZ@Z"); }

void shim__set_new_handler(void) { set_handler("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z"); }

void shim__set_new_mode(void) {
    g_eax = jpt_custom_level_active() ? 0 : (uint32_t)((crt_mode_t)crt("?_set_new_mode@@YAHH@Z"))((int)GUEST_ARG(0));
    GUEST_RETURN_STDCALL(0);
}
