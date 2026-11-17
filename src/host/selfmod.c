/*
 * SelfMod holds the rasteriser inner loops, which patch constants into their own
 * instructions before running them. A static lift would freeze those constants, so the
 * section keeps its original bytes, writable and executable, and is entered natively with
 * the guest's whole register file and x87 state.
 *
 * The register hand-over follows pcrecomp native32's gen_bridge (MIT).
 */
#include "host.h"

#include <stddef.h>

#include "recomp_trace.h"

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error "the SelfMod bridge is MSVC x86 inline assembly"
#endif

typedef struct {
    uint32_t fn;
    uint32_t in[8]; /* eax ecx edx ebx esp ebp esi edi */
    uint32_t out[8];
    uint32_t st_count;
    double st[8]; /* st[0] is st(0) */
    uint16_t cw_guest;
    uint16_t cw_host;
} selfmod_ctx_t;

/* The assembly below addresses these fields by number. */
C_ASSERT(offsetof(selfmod_ctx_t, fn) == 0);
C_ASSERT(offsetof(selfmod_ctx_t, in) == 4);
C_ASSERT(offsetof(selfmod_ctx_t, out) == 36);
C_ASSERT(offsetof(selfmod_ctx_t, st_count) == 68);
C_ASSERT(offsetof(selfmod_ctx_t, st) == 72);
C_ASSERT(offsetof(selfmod_ctx_t, cw_guest) == 136);
C_ASSERT(offsetof(selfmod_ctx_t, cw_host) == 138);

/* A callback out of native SelfMod code (it registers atexit handlers) builds its lifted
 * frame at g_esp, so g_esp is parked this far below the native code's own frame. */
#define CALLBACK_STACK_GAP 0x40000u

#define ENTRY_ALIGN_SHIFT 4
#define MAX_TRACKED_ENTRIES 4096

static uint32_t s_target;
static volatile LONG s_entries;
static unsigned char s_seen[MAX_TRACKED_ENTRIES / 8];

static __declspec(naked) void selfmod_enter(void) {
    __asm {
        push dword ptr [edi]
        mov  eax, [edi + 4]
        mov  ecx, [edi + 8]
        mov  edx, [edi + 12]
        mov  ebx, [edi + 16]
        mov  ebp, [edi + 24]
        mov  esi, [edi + 28]
        mov  edi, [edi + 32]
        ret
    }
}

/* ecx = the context. Naked, because ebp belongs to the guest while the routine runs and no
 * compiler frame may depend on it. Operands are register-relative once esp moves; the way back
 * is the TEB's ArbitraryUserPointer, as in native32. The x87 stack is rebuilt from the lifted
 * one under the guest's control word and read back the same way. */
static __declspec(naked) void __fastcall selfmod_run(selfmod_ctx_t* c) {
    __asm {
        push edi
        push ebp
        push esi
        push ebx
        push ecx
        push dword ptr fs:[0x14]
        mov  fs:[0x14], esp
        mov  edi, ecx
        fnstcw word ptr [edi + 138]
        fninit
        mov  ecx, [edi + 68]
    load_x87:
        test ecx, ecx
        jz   x87_loaded
        fld  qword ptr [edi + 64 + ecx * 8]
        dec  ecx
        jmp  load_x87
    x87_loaded:
        fldcw word ptr [edi + 136]
        mov  esp, [edi + 20]
        call selfmod_enter
        push eax
        mov  eax, fs:[0x14]
        mov  eax, [eax + 4]
        mov  [eax + 40], ecx
        mov  [eax + 44], edx
        mov  [eax + 48], ebx
        mov  [eax + 56], ebp
        mov  [eax + 60], esi
        mov  [eax + 64], edi
        pop  ecx
        mov  [eax + 36], ecx
        mov  [eax + 52], esp
        mov  esp, fs:[0x14]
        mov  edi, eax
        fnstcw word ptr [edi + 136]
        fnstsw ax
        shr  eax, 11
        neg  eax
        and  eax, 7
        mov  [edi + 68], eax
        xor  ecx, ecx
    store_x87:
        cmp  ecx, eax
        jae  x87_stored
        fstp qword ptr [edi + 72 + ecx * 8]
        inc  ecx
        jmp  store_x87
    x87_stored:
        fninit
        fldcw word ptr [edi + 138]
        pop  dword ptr fs:[0x14]
        add  esp, 4
        pop  ebx
        pop  esi
        pop  ebp
        pop  edi
        ret
    }
}

static void report_first_entry(uint32_t va) {
    uint32_t slot = (va - lift_selfmod_start) >> ENTRY_ALIGN_SHIFT;
    if (slot >= MAX_TRACKED_ENTRIES || (s_seen[slot / 8] & (1u << (slot % 8)))) return;
    s_seen[slot / 8] |= (unsigned char)(1u << (slot % 8));
    host_log("selfmod", "first native entry at 0x%08X from sub_%08X", va, g_cur_func);
}

static void selfmod_bridge(void) {
    selfmod_ctx_t c;
    const char* saved_import = g_cur_import;
    uint32_t guest_esp = g_esp + 4;
    int depth = g_fp_top < 0 ? 0 : g_fp_top > 8 ? 8 : g_fp_top;
    int i;

    report_first_entry(s_target);
    InterlockedIncrement(&s_entries);

    c.fn = s_target;
    c.in[0] = g_eax; c.in[1] = g_ecx; c.in[2] = g_edx; c.in[3] = g_ebx;
    c.in[4] = guest_esp; c.in[5] = g_ebp; c.in[6] = g_esi; c.in[7] = g_edi;
    c.st_count = (uint32_t)depth;
    for (i = 0; i < depth; i++) c.st[i] = RECOMP_X87_ST(i);
    c.cw_guest = g_fpu_cw;
    g_cur_import = "(SelfMod native code)";
    g_esp = guest_esp - CALLBACK_STACK_GAP;

    selfmod_run(&c);

    g_eax = c.out[0]; g_ecx = c.out[1]; g_edx = c.out[2]; g_ebx = c.out[3];
    g_esp = c.out[4]; g_ebp = c.out[5]; g_esi = c.out[6]; g_edi = c.out[7];
#ifdef RECOMP_X87_RING
    for (i = 0; i < depth; i++) (void)RECOMP_X87_POP();
    for (i = (int)c.st_count - 1; i >= 0; i--) RECOMP_X87_PUSH(c.st[i]);
#else
    for (i = 0; i < 8; i++) {
        g_st[i] = i < (int)c.st_count ? c.st[i] : 0.0;
        g_st_i64[i] = 0;
    }
    g_fp_top += (int)c.st_count - depth;
#endif
    g_fpu_cw = c.cw_guest;
    g_cur_import = saved_import;
}

recomp_func_t selfmod_lookup(uint32_t va) {
    if (va < lift_selfmod_start || va >= lift_selfmod_end) return NULL;
    s_target = va;
    return selfmod_bridge;
}

unsigned long selfmod_entries(void) { return (unsigned long)s_entries; }

int selfmod_init(void) {
    DWORD old;
    uint32_t size = lift_selfmod_end - lift_selfmod_start;
    if (!VirtualProtect((void*)(uintptr_t)lift_selfmod_start, size, PAGE_EXECUTE_READWRITE, &old)) {
        host_log("selfmod", "VirtualProtect failed (error %lu)", GetLastError());
        return 0;
    }
    host_log("selfmod", "0x%08X-0x%08X keeps its original code: writable, executable, run natively",
             lift_selfmod_start, lift_selfmod_end);
    return 1;
}
