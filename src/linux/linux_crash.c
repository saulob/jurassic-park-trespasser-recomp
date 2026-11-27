#include "linux_crash.h"

#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "engine_mode.h"
#include "guest_memory.h"
#include "guest_thunks.h"
#include "linux_log.h"
#include "recomp_types.h"

#define REPORT_BYTES 4096
#define ALT_STACK_BYTES 65536
#define BACKTRACE_FRAMES 48
#define GREG_RIP 16
#define GREG_RSP 15
#define PAGE 4096
#define STACK_SCAN_BYTES 8192
#define MAX_CANDIDATES 40

static volatile uint32_t s_present_stage, s_audio_stage, s_import_va;
static volatile uintptr_t s_load_base, s_text_lo, s_text_hi;
static __thread uint32_t t_guest_id, t_guest_entry;

static const char* const k_present_names[] = { "none", "PRESENT_BEGIN", "TEXTURE_LOCK", "TEXTURE_UPLOAD", "RENDER_CLEAR", "RENDER_TEXTURE",
                                               "RENDER_PRESENT", "EVENT_PUMP", "FRAME_HOOK", "PRESENT_END" };
static const char* const k_audio_names[] = { "none", "AUDIO_DEVICE_OPEN", "STREAM_THREAD", "GUEST_AUDIO_THREAD", "BUFFER_LOCK", "BUFFER_UNLOCK",
                                             "DSOUND_METHOD" };

void lx_crash_present_stage(lx_present_stage_t stage) { __atomic_store_n(&s_present_stage, (uint32_t)stage, __ATOMIC_RELAXED); }
void lx_crash_audio_stage(lx_audio_stage_t stage) { __atomic_store_n(&s_audio_stage, (uint32_t)stage, __ATOMIC_RELAXED); }
void lx_crash_import(uint32_t thunk_va) { __atomic_store_n(&s_import_va, thunk_va, __ATOMIC_RELAXED); }
void lx_crash_thread_started(uint32_t id, uint32_t entry) { t_guest_id = id, t_guest_entry = entry; }
void lx_crash_thread_ended(void) { t_guest_id = 0, t_guest_entry = 0; }

typedef struct {
    char text[REPORT_BYTES];
    int len;
} out_t;

static void put_str(out_t* o, const char* s) {
    while (*s && o->len < REPORT_BYTES - 1) o->text[o->len++] = *s++;
}

static void put_hex(out_t* o, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = digits[v & 15];
        v >>= 4;
    } while (v);
    put_str(o, "0x");
    while (n && o->len < REPORT_BYTES - 1) o->text[o->len++] = tmp[--n];
}

static void put_dec(out_t* o, int64_t v) {
    char tmp[21];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
    if (v < 0) put_str(o, "-");
    do {
        tmp[n++] = (char)(48 + u % 10);
        u /= 10;
    } while (u);
    while (n && o->len < REPORT_BYTES - 1) o->text[o->len++] = tmp[--n];
}

static const char* name_of(const char* const* names, unsigned count, uint32_t i) { return i < count ? names[i] : "?"; }

static void field(out_t* o, const char* label, uint64_t value) {
    put_str(o, "  ");
    put_str(o, label);
    put_str(o, " ");
    put_hex(o, value);
    put_str(o, "\n");
}

static int build_report(out_t* o, int signo, int code, void* fault_address, void* host_rip) {
    uint32_t import_va = __atomic_load_n(&s_import_va, __ATOMIC_RELAXED);
    const char* import_name = import_va ? ht_name(import_va) : NULL;
    o->len = 0;
    put_str(o, "\n==== trespasser_recomp fatal signal ");
    put_dec(o, signo);
    put_str(o, " si_code ");
    put_dec(o, code);
    put_str(o, "\n");
    field(o, "fault address      ", (uint64_t)(uintptr_t)fault_address);
    field(o, "host rip           ", (uint64_t)(uintptr_t)host_rip);
    field(o, "host rip - base    ", (uint64_t)((uintptr_t)host_rip - s_load_base));
    field(o, "load base          ", (uint64_t)s_load_base);
    if ((uintptr_t)fault_address >= (uintptr_t)gm_base() && (uintptr_t)fault_address - (uintptr_t)gm_base() <= UINT32_MAX &&
        engine_heaps_guard((uint32_t)((uintptr_t)fault_address - (uintptr_t)gm_base())))
        put_str(o, "  GUARD_PAGE_HIT: past an engine heap that was not asked for the room, a bug of the port\n");
    put_str(o, "  host thread        ");
    put_dec(o, syscall(SYS_gettid));
    put_str(o, "\n  guest thread       ");
    put_hex(o, t_guest_id);
    put_str(o, t_guest_id ? " (entry " : " (main, entry ");
    put_hex(o, t_guest_entry);
    put_str(o, ")\n");
    field(o, "guest function     ", g_cur_func);
    field(o, "guest esp          ", g_esp);
    field(o, "guest ebp          ", g_ebp);
    put_str(o, "  last import        ");
    put_hex(o, import_va);
    put_str(o, " ");
    put_str(o, import_name ? import_name : "-");
    put_str(o, "\n  present stage      ");
    put_str(o, name_of(k_present_names, sizeof k_present_names / sizeof k_present_names[0], __atomic_load_n(&s_present_stage, __ATOMIC_RELAXED)));
    put_str(o, "\n  audio stage        ");
    put_str(o, name_of(k_audio_names, sizeof k_audio_names / sizeof k_audio_names[0], __atomic_load_n(&s_audio_stage, __ATOMIC_RELAXED)));
    put_str(o, "\n  code addresses found on the stack, innermost first (tools/linux_symbolize.sh)\n");
    return o->len;
}

static void write_all(int fd, const char* p, int n) {
    while (n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if (w <= 0) return;
        p += w;
        n -= (int)w;
    }
}

static int readable(uintptr_t address, size_t bytes) {
    unsigned char vec[STACK_SCAN_BYTES / PAGE + 2];
    uintptr_t first = address & ~(uintptr_t)(PAGE - 1);
    return mincore((void*)first, address + bytes - first, vec) == 0;
}

/* The words on the stack that point into the executable's code, as addresses a symbolizer can resolve. */
__attribute__((no_sanitize_address)) static int stack_candidates(out_t* o, uintptr_t sp) {
    const uintptr_t* words = (const uintptr_t*)(sp & ~(uintptr_t)7);
    int found = 0;
    o->len = 0;
    if (!readable((uintptr_t)words, STACK_SCAN_BYTES)) return 0;
    for (unsigned i = 0; i < STACK_SCAN_BYTES / sizeof *words && found < MAX_CANDIDATES; i++) {
        if (words[i] < s_text_lo || words[i] >= s_text_hi) continue;
        put_str(o, "  frame [");
        put_hex(o, words[i]);
        put_str(o, "]\n");
        found++;
    }
    return o->len;
}

static void emit(const int* fds, int count, const char* text, int n) {
    for (int i = 0; i < count; i++)
        if (fds[i] >= 0) write_all(fds[i], text, n);
}

static void emit_report(const int* fds, int count, int signo, int code, void* fault_address, void* host_rip, uintptr_t sp) {
    static out_t report, stack;
    void* frames[BACKTRACE_FRAMES];
    int n = build_report(&report, signo, code, fault_address, host_rip);
    emit(fds, count, report.text, n);
    emit(fds, count, stack.text, stack_candidates(&stack, sp));
    if ((uintptr_t)host_rip >= s_text_lo && (uintptr_t)host_rip < s_text_hi) {
        int frames_found = backtrace(frames, BACKTRACE_FRAMES);
        for (int i = 0; i < count; i++)
            if (fds[i] >= 0) backtrace_symbols_fd(frames, frames_found, fds[i]);
    }
}

int lx_crash_report(int fd, int signo, void* fault_address, void* host_rip) {
    int local = 0;
    emit_report(&fd, 1, signo, 0, fault_address, host_rip, (uintptr_t)&local);
    return 1;
}

static void on_signal(int signo, siginfo_t* info, void* context) {
    const ucontext_t* uc = (const ucontext_t*)context;
    int fds[2] = { 2, lx_log_fd() };
    void* rip = uc ? (void*)uc->uc_mcontext.gregs[GREG_RIP] : NULL;
    uintptr_t sp = uc ? (uintptr_t)uc->uc_mcontext.gregs[GREG_RSP] : 0;
    emit_report(fds, 2, signo, info ? info->si_code : 0, info ? info->si_addr : NULL, rip, sp);
    if (signo == SIGABRT) raise(signo);
}

static int load_base_cb(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    (void)data;
    s_load_base = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            s_text_lo = (uintptr_t)info->dlpi_addr + ph->p_vaddr;
            s_text_hi = s_text_lo + ph->p_memsz;
        }
    }
    return 1;
}

void lx_crash_install(void) {
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    void* warm[4];
    stack_t alt;
    struct sigaction sa;
    dl_iterate_phdr(load_base_cb, NULL);
    backtrace(warm, 4);
    alt.ss_sp = malloc(ALT_STACK_BYTES);
    alt.ss_size = ALT_STACK_BYTES;
    alt.ss_flags = 0;
    if (alt.ss_sp) sigaltstack(&alt, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; i++) sigaction(signals[i], &sa, NULL);
    lx_log("crash", "handler installed, load base 0x%lx, code 0x%lx-0x%lx", (unsigned long)s_load_base, (unsigned long)s_text_lo, (unsigned long)s_text_hi);
}
