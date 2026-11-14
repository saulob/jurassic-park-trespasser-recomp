/* Calls into lifted code from the host, at a point where the game itself could make them (its UI loop). */
#include "host.h"

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
