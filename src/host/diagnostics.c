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
    report_add("  lifted function sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
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

/* pcrecomp answers an unknown target by logging and continuing with eax = 0, which moves the
 * failure far from its cause. A target inside .text with no lifted entry stops the run here. */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    recomp_func_t fn = selfmod_lookup(va);
    if (fn) return fn;
    if (va < trespasser_text_start || va >= trespasser_text_end) {
        milestones_observe(va);
        fn = ddraw_compat_lookup(va);
        return fn ? fn : ddraw_trace_lookup(va);
    } else if (!recomp_lookup(va)) {
        s_unresolved_va = va;
        return unresolved_text_target;
    }
    return NULL;
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
    s_guest_lo = trespasser_image_base;
    s_guest_hi = trespasser_selfmod_end;
    AddVectoredExceptionHandler(0, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    s_watchdog_seconds = watchdog_seconds;
    if (watchdog_seconds) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
}
