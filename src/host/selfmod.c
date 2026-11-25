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
#include "terrain_shadows.h"

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
    host_log_perf("selfmod", "first native entry at 0x%08X from sub_%08X", va, g_cur_func);
}

#define TIMED_SLOTS 512u
static struct {
    uint32_t target, caller;
    unsigned long long calls;
    LONG64 ticks;
} s_timed[TIMED_SLOTS];
static LONG64 s_total_ticks;

static void note_time(uint32_t target, uint32_t caller, LONG64 ticks) {
    unsigned i = ((target * 2654435761u) ^ (caller * 40503u)) & (TIMED_SLOTS - 1u);
    for (unsigned n = 0; n < TIMED_SLOTS; n++, i = (i + 1u) & (TIMED_SLOTS - 1u))
        if (!s_timed[i].calls || (s_timed[i].target == target && s_timed[i].caller == caller)) {
            s_timed[i].target = target;
            s_timed[i].caller = caller;
            s_timed[i].calls++;
            s_timed[i].ticks += ticks;
            break;
        }
    s_total_ticks += ticks;
}

static double ticks_to_ms(LONG64 ticks) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)ticks * 1000.0 / (double)f.QuadPart;
}

double selfmod_total_ms(void) { return ticks_to_ms(s_total_ticks); }

void selfmod_report(double frames, double frame_ms) {
    unsigned shown[10], count = 0;
    for (unsigned k = 0; k < 10; k++) {
        unsigned best = TIMED_SLOTS;
        for (unsigned i = 0; i < TIMED_SLOTS; i++) {
            int taken = 0;
            for (unsigned j = 0; j < count; j++) taken |= shown[j] == i;
            if (s_timed[i].calls && !taken && (best == TIMED_SLOTS || s_timed[i].ticks > s_timed[best].ticks)) best = i;
        }
        if (best == TIMED_SLOTS) break;
        shown[count++] = best;
    }
    host_log("perf-detail", "SelfMod native code: %llu entries, %.1f per frame, %.2f ms per frame (%.1f%% of the frame)",
             (unsigned long long)s_entries, s_entries / (frames > 0.0 ? frames : 1.0),
             selfmod_total_ms() / (frames > 0.0 ? frames : 1.0),
             frame_ms > 0.0 ? 100.0 * selfmod_total_ms() / (frames > 0.0 ? frames : 1.0) / frame_ms : 0.0);
    for (unsigned j = 0; j < count; j++) {
        double ms = ticks_to_ms(s_timed[shown[j]].ticks), per_frame = ms / (frames > 0.0 ? frames : 1.0);
        host_log("perf-detail", "SelfMod 0x%08X from sub_%08X: %llu calls, %.1f ms, %.2f us each, %.1f%% of the frame",
                 s_timed[shown[j]].target, s_timed[shown[j]].caller, s_timed[shown[j]].calls, ms,
                 ms * 1000.0 / (double)s_timed[shown[j]].calls, frame_ms > 0.0 ? 100.0 * per_frame / frame_ms : 0.0);
    }
}

static void selfmod_bridge(void) {
    selfmod_ctx_t c;
    LARGE_INTEGER began, ended;
    uint32_t caller = g_cur_func;
    const char* saved_import = g_cur_import;
    uint32_t guest_esp = g_esp + 4;
    int depth = g_fp_top < 0 ? 0 : g_fp_top > 8 ? 8 : g_fp_top;
    int i;

    report_first_entry(s_target);
    InterlockedIncrement(&s_entries);
    terrain_shadows_span(s_target); /* DEBUG_SAULO */

    c.fn = s_target;
    c.in[0] = g_eax; c.in[1] = g_ecx; c.in[2] = g_edx; c.in[3] = g_ebx;
    c.in[4] = guest_esp; c.in[5] = g_ebp; c.in[6] = g_esi; c.in[7] = g_edi;
    c.st_count = (uint32_t)depth;
    for (i = 0; i < depth; i++) c.st[i] = RECOMP_X87_ST(i);
    c.cw_guest = g_fpu_cw;
    g_cur_import = "(SelfMod native code)";
    g_esp = guest_esp - CALLBACK_STACK_GAP;

    QueryPerformanceCounter(&began);
    selfmod_run(&c);
    QueryPerformanceCounter(&ended);
    note_time(c.fn, caller, ended.QuadPart - began.QuadPart);

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

static selfmod_interceptor_t s_interceptor;
static DWORD s_interceptor_thread;

/* Only the installing thread is redirected: the game's worker threads keep reaching SelfMod code meanwhile. */
void selfmod_intercept(selfmod_interceptor_t interceptor) {
    s_interceptor_thread = interceptor ? GetCurrentThreadId() : 0;
    s_interceptor = interceptor;
}

recomp_func_t selfmod_lookup(uint32_t va) {
    if (va < lift_selfmod_start || va >= lift_selfmod_end) return NULL;
    if (s_interceptor && s_interceptor_thread == GetCurrentThreadId()) return s_interceptor(va);
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
