/* Calls into lifted code from the host, at a point where the game itself could make them (its UI loop). */
#include "host.h"

#include <stdlib.h>
#include <string.h>

#define CALL_LOG_LIMIT 8

static int s_logs;

int guest_call(uint32_t va, uint32_t self, const uint32_t* args, int count, int caller_pops, uint32_t* result) {
    recomp_func_t f = va ? recomp_resolve(va) : NULL;
    uint32_t esp = g_esp, ecx = g_ecx, edx = g_edx, current = g_cur_func;
    if (!f) {
        if (s_logs++ < CALL_LOG_LIMIT) host_log("guest-call", "sub_%08X is not lifted", va);
        return 0;
    }
    for (int i = count - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    if (caller_pops) g_esp += 4u * (uint32_t)count;
    if (result) *result = g_eax;
    g_ecx = ecx;
    g_edx = edx;
    g_cur_func = current;
    if (g_esp == esp) return 1;
    if (s_logs++ < CALL_LOG_LIMIT)
        host_log("guest-call", "sub_%08X moved the game's stack by %d bytes", va, (int)(g_esp - esp));
    g_esp = esp;
    return 0;
}

int guest_call_virtual(uint32_t self, uint32_t slot, const uint32_t* args, int count, uint32_t* result) {
    uint32_t vtable, fn;
    if (!self || !guest_read32(self, &vtable) || !guest_read32(vtable + slot, &fn)) return 0;
    return guest_call(fn, self, args, count, 0, result);
}

uint32_t guest_scratch(unsigned slot, uint32_t size) {
    static uint8_t* s_buffer[4];
    static uint32_t s_size[4];
    if (slot >= 4 || !size) return 0;
    if (s_size[slot] < size) {
        free(s_buffer[slot]);
        s_buffer[slot] = (uint8_t*)malloc(size);
        s_size[slot] = s_buffer[slot] ? size : 0;
    }
    return (uint32_t)(uintptr_t)s_buffer[slot];
}

uint32_t guest_keep(const void* data, uint32_t size) {
    (void)size;
    return (uint32_t)(uintptr_t)data;
}

int guest_host_path(const char* guest_path, char* out, size_t cap) {
    size_t n = guest_path ? strlen(guest_path) : 0;
    if (n >= cap) return 0;
    memcpy(out, guest_path, n + 1);
    return 1;
}
