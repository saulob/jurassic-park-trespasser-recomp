/* Shared set-up of the Linux Win32-layer suites: the guest window, the call spine with the fake guest functions of
 * linux_fake_guest.c, one attached guest thread, the HLE with the virtual window backend, and helpers that call an import the
 * way the guest does (arguments on a guest stack, through its thunk). Headless: nothing opens a window. */
#ifndef LINUX_WIN32_HARNESS_H
#define LINUX_WIN32_HARNESS_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "linux_fake_guest.h"
#include "linux_hle.h"
#include "linux_spine.h"
#include "linux_window.h"

#define HARNESS_FN static __attribute__((unused))

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

static uint32_t g_stack, g_scratch;
static lx_thread_t* g_thread;

HARNESS_FN int harness_start(void) {
    static lx_entry_t entries[32];
    lx_table_t table;
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 0;
    }
    for (unsigned i = 0; i < g_fake_table_count; i++) {
        entries[i].va = g_fake_table[i].va;
        entries[i].fn = g_fake_table[i].fn;
    }
    table.entries = entries;
    table.count = g_fake_table_count;
    lx_spine_init(table, (lx_table_t){ NULL, 0 });
    g_stack = gm_alloc_pages(0, 0x10000, "stack");
    g_scratch = gm_alloc_pages(0, 0x100000, "scratch");
    hle_init(NULL, 0x00400000u, "C:\trespass.exe");
    lx_virtual_reset();
    g_thread = lx_thread_new(2 * 1024 * 1024);
    lx_thread_attach(g_thread);
    return 1;
}

HARNESS_FN void harness_stop(void) {
    lx_thread_detach();
    lx_thread_free(g_thread);
    hle_shutdown();
    gm_shutdown();
}

HARNESS_FN uint32_t call_n(uint32_t thunk, int argc, const uint32_t* args) {
    uint32_t esp = g_stack + 0x8000 - 4 * (uint32_t)(argc + 1), eax = 0, edx = 0;
    gp_write_u32(esp, 0x00401000u);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    if (!ht_invoke(thunk, &esp, &eax, &edx)) return 0xBAD0BAD0u;
    return eax;
}

HARNESS_FN uint32_t api(const char* dll, const char* name, int argc, ...) {
    uint32_t args[20], thunk;
    va_list list;
    va_start(list, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(list, uint32_t);
    va_end(list);
    thunk = hle_resolve(dll, name, 0, NULL);
    return thunk ? call_n(thunk, argc, args) : 0xBAD1BAD1u;
}

#define U32(name, argc, ...) api("user32.dll", #name, argc, ##__VA_ARGS__)
#define G32(name, argc, ...) api("gdi32.dll", #name, argc, ##__VA_ARGS__)
#define DDC(name, argc, ...) api("ddraw.dll", #name, argc, ##__VA_ARGS__)

/* A COM method called through the guest object's vtable, the way the guest calls it: this first, then the arguments. */
HARNESS_FN uint32_t com(uint32_t object, int index, int argc, ...) {
    uint32_t args[16], vtbl = 0, method = 0;
    va_list list;
    gp_read_u32(object, &vtbl);
    gp_read_u32(vtbl + 4 * (uint32_t)index, &method);
    args[0] = object;
    va_start(list, argc);
    for (int i = 1; i < argc; i++) args[i] = va_arg(list, uint32_t);
    va_end(list);
    return call_n(method, argc, args);
}

HARNESS_FN uint32_t put_string(uint32_t where, const char* text) {
    gp_copy_out(where, text, (uint32_t)strlen(text) + 1);
    return where;
}

HARNESS_FN uint32_t rd(uint32_t va) {
    uint32_t v = 0;
    gp_read_u32(va, &v);
    return v;
}

HARNESS_FN uint32_t fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

#endif
