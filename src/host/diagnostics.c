/* Fault, dead-end and watchdog reports. The fault report follows sp00nznet/themovies host.c (MIT). */
#include "host.h"

#include <stdarg.h>
#include <stdio.h>

#include "recomp_trace.h"

#define FOREIGN_FAULT_LOG_LIMIT 20

static char s_report[4096];
static int s_report_len;
static uint32_t s_host_lo, s_host_hi;
static uint32_t s_guest_lo, s_guest_hi;
static DWORD s_watchdog_seconds;
static LONG s_foreign_faults;
static volatile LONG s_process_ending;

static void report_begin(void) { s_report_len = 0; }

static void report_add(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(s_report + s_report_len, sizeof s_report - 1 - s_report_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_report_len += n;
}

static void report_machine_state(void) {
    char ddraw[160];
    report_add("  lifted function sub_%08X, last native call (any thread) %s\n", g_cur_func, g_cur_import);
    if (ddraw_compat_last_call(ddraw, sizeof ddraw) || ddraw_trace_last_call(ddraw, sizeof ddraw))
        report_add("  last DirectDraw call %s\n", ddraw);
    report_add("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    report_add("  x87 depth %d, control word %04X; SelfMod entries %lu; indirect calls %u\n",
               g_fp_top, g_fpu_cw, selfmod_entries(), g_icall_count);
    report_add("  last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* name = native32_name(g_icall_trace[k]);
        if (!name && ddraw_trace_name(g_icall_trace[k], ddraw, sizeof ddraw)) name = ddraw;
        report_add("    0x%08X  from sub_%08X  %s\n", g_icall_trace[k], g_icall_from[k], name ? name : "");
    }
}

static void report_emit(void) { host_log_raw(s_report, (unsigned)s_report_len); }

static void stop(int code) {
    window_host_release_cursor();
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

void recomp_not_lifted(uint32_t va) {
    report_begin();
    report_add("\n=== reached sub_%08X, which is not lifted ===\n", va);
    report_add("  lift it: py -3 tools\\run_lift.py (without --closure), then rebuild\n");
    report_machine_state();
    report_emit();
    stop(HOST_EXIT_NOT_LIFTED);
}

static uint32_t s_unresolved_va;

static void unresolved_text_target(void) {
    report_begin();
    report_add("\n=== indirect transfer to 0x%08X, inside .text but not a lifted entry ===\n", s_unresolved_va);
    report_add("  add \"0x%08X\" to config\\extra_entries.json, run tools\\run_lift.py, rebuild\n",
               s_unresolved_va);
    report_machine_state();
    report_emit();
    stop(HOST_EXIT_UNRESOLVED);
}

#define DISPATCH_CACHE_BITS 13

static struct {
    uint32_t va;
    recomp_func_t fn;
} s_dispatch_cache[1u << DISPATCH_CACHE_BITS];
static unsigned long long s_dispatch_hits, s_dispatch_misses;
static int outside_dispatch(uint32_t va) {
    const lift_image_t* image = lift_active_image();
    return !image || !image->meta->function_count || va < image->dispatch[0].address ||
           va > image->dispatch[image->meta->function_count - 1].address;
}

/* Calls out of the lifted code: what answers a target is decided at its first call (and again when the DirectDraw
 * observer watches more methods). Bridged targets still go through recomp_lookup_import every time, which names the
 * native function the bridge calls. */
enum { CALL_BRIDGE, CALL_FIXED, CALL_IN_PLACE, CALL_COMPAT, CALL_OBSERVED, CALL_DISPATCH };
#define CALL_CACHE_BITS 10
#define IN_PLACE_SLOTS 8

typedef struct {
    uint32_t va;
    unsigned observed;
    int kind;
    recomp_func_t fn;
} call_slot_t;

static call_slot_t s_calls[1u << CALL_CACHE_BITS];
static uint32_t s_in_place_target;

/* A compatibility-layer method on the guest's arguments, without leaving the machine: the stdcall callee pops its
 * own arguments, and esp before and after says how many (as native32's bridge measures it). The callee is host C,
 * which keeps ebp, so the frame the locals are addressed through stays valid. */
static void in_place_call(void) {
    uint32_t fn = s_in_place_target;
    uint32_t* src = (uint32_t*)(uintptr_t)(g_esp + 4);
    uint32_t r_eax, purge;
    const char* saved_import = g_cur_import;
    g_cur_import = "(compatibility layer)";
    __asm {
        mov  edx, src
        mov  eax, fn
        push esi
        push ebx
        mov  esi, esp              ; the callee keeps esi (C ABI)
        mov  ecx, IN_PLACE_SLOTS
    copy_args:
        push dword ptr [edx + ecx * 4 - 4]
        dec  ecx
        jnz  copy_args
        call eax
        mov  edx, esp
        sub  edx, esi
        add  edx, IN_PLACE_SLOTS * 4 ; bytes the callee popped
        mov  esp, esi
        pop  ebx
        pop  esi
        mov  r_eax, eax
        mov  purge, edx
    }
    g_cur_import = saved_import;
    g_eax = r_eax;
    g_esp += 4 + purge;
}

static void classify_call(call_slot_t* c, uint32_t va, unsigned observed) {
    milestones_observe(va);
    c->va = va;
    c->observed = observed;
    c->fn = NULL;
    if (ddraw_compat_lookup(va)) c->kind = CALL_COMPAT;
    else if (ddraw_trace_lookup(va)) c->kind = CALL_OBSERVED;
    else if (!outside_dispatch(va)) c->kind = CALL_DISPATCH;
    else if (d3d_method_in_place(va)) c->kind = CALL_IN_PLACE;
    else if (native32_is_shim(va) && (c->fn = recomp_lookup_import(va)) != NULL) c->kind = CALL_FIXED;
    else c->kind = CALL_BRIDGE;
}

static recomp_func_t outside_text(uint32_t va) {
    call_slot_t* c = &s_calls[(va * 2654435761u) >> (32 - CALL_CACHE_BITS)];
    unsigned observed = ddraw_trace_entry_count();
    if (c->va != va || c->observed != observed) classify_call(c, va, observed);
    switch (c->kind) {
    case CALL_IN_PLACE:
        s_in_place_target = va;
        return in_place_call;
    case CALL_FIXED: return c->fn;
    case CALL_COMPAT: return ddraw_compat_lookup(va);
    case CALL_OBSERVED: return ddraw_trace_lookup(va);
    case CALL_DISPATCH: return NULL;
    default: return recomp_lookup_import(va);
    }
}

void dispatch_cache_counts(unsigned long long* hits, unsigned long long* misses) {
    *hits = s_dispatch_hits;
    *misses = s_dispatch_misses;
}

/* pcrecomp answers an unknown target by logging and continuing with eax = 0, which moves the
 * failure far from its cause. A target inside .text with no lifted entry stops the run here.
 * A lifted entry's function never changes (overrides switch inside its stable entry), so its answer is kept;
 * only the thread that holds the machine lock runs lifted code and resolves its calls. */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    recomp_func_t fn = selfmod_lookup(va);
    unsigned slot;
    if (fn) return fn;
    if (va < lift_text_start || va >= lift_text_end) return outside_text(va);
    slot = (va * 2654435761u) >> (32 - DISPATCH_CACHE_BITS);
    if (s_dispatch_cache[slot].va == va) {
        s_dispatch_hits++;
        return s_dispatch_cache[slot].fn;
    }
    s_dispatch_misses++;
    fn = recomp_lookup(va);
    if (!fn) {
        s_unresolved_va = va;
        return unresolved_text_target;
    }
    s_dispatch_cache[slot].fn = fn;
    s_dispatch_cache[slot].va = va;
    return fn;
}

static int in_range(uint32_t va, uint32_t lo, uint32_t hi) { return va >= lo && va < hi; }

/* Runs after native32's handler, which has already turned Windows-to-guest calls into
 * lifted calls. A fault inside a system DLL may still be handled by that DLL's own SEH,
 * so only faults in lifted code or in the guest image end the run here. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    static volatile LONG reporting;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    uint32_t pc = (uint32_t)(uintptr_t)r->ExceptionAddress;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (!in_range(pc, s_host_lo, s_host_hi) && !in_range(pc, s_guest_lo, s_guest_hi)) {
        if (!s_process_ending && InterlockedIncrement(&s_foreign_faults) <= FOREIGN_FAULT_LOG_LIMIT) {
            char line[256];
            int n = _snprintf(line, sizeof line, "[fault] first-chance 0x%08lX at 0x%08X outside lifted "
                              "code (last native call %s); left to its own handler\n",
                              r->ExceptionCode, pc, g_cur_import);
            host_log_raw(line, n > 0 ? (unsigned)n : (unsigned)sizeof line);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedExchange(&reporting, 1)) stop(HOST_EXIT_FAULT);
    report_begin();
    report_add("\n=== fault 0x%08lX at 0x%08X (%s), thread %lu ===\n", r->ExceptionCode, pc,
               in_range(pc, s_guest_lo, s_guest_hi) ? "guest image, native code" : "lifted code",
               GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) report_add("  host stack overflow\n");
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        report_add("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    report_machine_state();
    report_emit();
    stop(HOST_EXIT_FAULT);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    report_begin();
    report_add("\n=== unhandled exception 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    report_machine_state();
    report_emit();
    stop(HOST_EXIT_FAULT);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(s_watchdog_seconds * 1000);
    report_begin();
    report_add("\n=== watchdog: %lu s elapsed ===\n", s_watchdog_seconds);
    report_machine_state();
    report_emit();
    stop(HOST_EXIT_WATCHDOG);
    return 0;
}

void diagnostics_shutdown_timeout(const char* detail) {
    report_begin();
    report_add("\n=== shutdown watchdog: %s ===\n", detail);
    report_machine_state();
    report_emit();
    host_log("exit", "exit code %d (shutdown timeout, not a normal exit)", HOST_EXIT_SHUTDOWN_TIMEOUT);
    stop(HOST_EXIT_SHUTDOWN_TIMEOUT);
}

void diagnostics_process_ending(void) { InterlockedExchange(&s_process_ending, 1); }

void diagnostics_install(unsigned watchdog_seconds) {
    HMODULE self = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((char*)self + ((IMAGE_DOS_HEADER*)self)->e_lfanew);
    s_host_lo = (uint32_t)(uintptr_t)self;
    s_host_hi = s_host_lo + nt->OptionalHeader.SizeOfImage;
    s_guest_lo = JPT_GUEST_IMAGE_BASE;
    s_guest_hi = lift_selfmod_end;
    AddVectoredExceptionHandler(0, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    s_watchdog_seconds = watchdog_seconds;
    if (watchdog_seconds) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
}
