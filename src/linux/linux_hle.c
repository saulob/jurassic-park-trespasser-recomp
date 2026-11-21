#include "l10n.h"
#include "linux_hle.h"
#include "stream_vfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "host_os.h"
#include "jpt_fs.h"
#include "jpt_sync.h"
#include "linux_crash.h"
#include "linux_frontend.h"
#include "linux_log.h"
#include "linux_modules.h"
#include "linux_msvcrt.h"
#include "linux_advapi32.h"
#include "linux_ddraw.h"
#include "linux_dsound.h"
#include "linux_lz32.h"
#include "linux_shutdown.h"
#include "linux_smack.h"
#include "linux_gdi32.h"
#include "linux_processor.h"
#include "linux_resource.h"
#include "linux_screen.h"
#include "linux_user32.h"
#include "linux_spine.h"

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_READ_FAULT 30u
#define ERROR_NEGATIVE_SEEK 131u

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u

typedef enum { H_NONE, H_WAITABLE, H_FILE, H_FIND, H_THREAD } handle_kind_t;

typedef struct gthread gthread_t;

/* After a host call that blocked: a guest thread that was terminated meanwhile ends here. */
static void guest_thread_checkpoint(void);
static int guest_wait_should_end(void);

#define WAIT_SLICE_MS 50u

/* Waits are taken in slices so a thread asked to end (the run is over, or the host is shutting down) is not stranded in a long one. */
static uint32_t wait_sliced(jpt_waitable_t* const* list, uint32_t count, int wait_all, uint32_t timeout_ms) {
    uint32_t waited = 0, result;
    for (;;) {
        uint32_t slice = timeout_ms == JPT_INFINITE ? WAIT_SLICE_MS : timeout_ms - waited < WAIT_SLICE_MS ? timeout_ms - waited : WAIT_SLICE_MS;
        result = jpt_wait_multiple(list, count, wait_all, slice);
        if (result != JPT_WAIT_TIMEOUT || guest_wait_should_end()) return result;
        if (timeout_ms != JPT_INFINITE && (waited += slice) >= timeout_ms) return result;
    }
}

static void sleep_sliced(uint32_t ms) {
    uint32_t waited = 0;
    while (waited < ms && !guest_wait_should_end()) {
        uint32_t slice = ms - waited < WAIT_SLICE_MS ? ms - waited : WAIT_SLICE_MS;
        jpt_sleep_ms(slice);
        waited += slice;
    }
}

typedef struct {
    handle_kind_t kind;
    jpt_waitable_t* waitable;
    jpt_thread_t* thread;
    gthread_t* guest_thread;
    int fd;
    stream_vfile_t* vfile;
    char* guest_name;
    char** names;
    int count, next;
    char dir[JPT_PATH_MAX];
    char pattern[260];
} handle_t;

#define MAX_HANDLES 4096
#define HANDLE_BASE 0x100u

static jpt_vpath_t* s_vpath;
static uint32_t s_image_base;
static char s_exe_path[260];
static handle_t s_handles[MAX_HANDLES];
static hos_rwlock_t s_lock = HOS_RWLOCK_INIT;
static HOS_THREAD_LOCAL uint32_t t_last_error;
static void (*s_exit_handler)(uint32_t);
static unsigned s_missing, s_stubbed;
static int s_permissive;

typedef struct {
    uint32_t va;
    jpt_cs_t* cs;
} cs_slot_t;
#define MAX_CS 1024
static cs_slot_t s_cs[MAX_CS];

#define MAX_MODULES 16
static char s_module[MAX_MODULES][64];
static int s_nmodules;

static void set_error(uint32_t e) { t_last_error = e; }
void hle_set_last_error(uint32_t error) { t_last_error = error; }

static uint32_t new_handle(const handle_t* h) {
    uint32_t result = 0;
    hos_wrlock(&s_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++)
        if (s_handles[i].kind == H_NONE) {
            s_handles[i] = *h;
            result = HANDLE_BASE + 4 * i;
            break;
        }
    hos_unlock_wr(&s_lock);
    if (!result) set_error(ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

static handle_t* handle_at(uint32_t value) {
    if (value < HANDLE_BASE || (value - HANDLE_BASE) % 4 || (value - HANDLE_BASE) / 4 >= MAX_HANDLES) return NULL;
    return s_handles[(value - HANDLE_BASE) / 4].kind == H_NONE ? NULL : &s_handles[(value - HANDLE_BASE) / 4];
}

static const char* cstr(uint32_t va) { return va ? gp_string(va, 1024) : NULL; }

/* ---- time, sleep, errors ---- */

static uint64_t k_GetTickCount(const uint32_t* a, void* u) { (void)a; (void)u; return jpt_time_ms(); }
static uint64_t k_GetLastError(const uint32_t* a, void* u) { (void)a; (void)u; return t_last_error; }

static uint64_t k_Sleep(const uint32_t* a, void* u) {
    lx_blocking_t b;
    (void)u;
    lx_blocking_begin(&b);
    sleep_sliced(a[0] == JPT_INFINITE ? 0x7FFFFFFFu : a[0]);
    lx_blocking_end(&b);
    guest_thread_checkpoint();
    lx_shutdown_poll();
    return 0;
}

static uint64_t k_SleepEx(const uint32_t* a, void* u) { return k_Sleep(a, u); }

void hle_exit_process(uint32_t code) {
    lx_log("hle", "ExitProcess(%u)", code);
    if (s_exit_handler) s_exit_handler(code);
    else exit((int)code);
}

static uint64_t k_ExitProcess(const uint32_t* a, void* u) {
    (void)u;
    hle_exit_process(a[0]);
    return 0;
}

static uint64_t k_OutputDebugStringA(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    (void)u;
    if (s) lx_log("guest", "%s", s);
    return 0;
}

/* ---- strings ---- */

static uint64_t k_lstrlenA(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    (void)u;
    return s ? strlen(s) : 0;
}

static uint64_t k_lstrcpyA(const uint32_t* a, void* u) {
    const char* src = cstr(a[1]);
    char* dst;
    size_t n;
    (void)u;
    if (!src) return 0;
    n = strlen(src) + 1;
    dst = (char*)gm_ptr_checked(a[0], (uint32_t)n, 1);
    if (!dst) return 0;
    memcpy(dst, src, n);
    return a[0];
}

/* ---- synchronisation ---- */

static uint32_t make_waitable(jpt_waitable_t* w) {
    handle_t h;
    uint32_t v;
    memset(&h, 0, sizeof h);
    if (!w) return set_error(ERROR_NOT_ENOUGH_MEMORY), 0;
    h.kind = H_WAITABLE;
    h.waitable = w;
    v = new_handle(&h);
    if (!v) jpt_waitable_free(w);
    return v;
}

static uint64_t k_CreateEventA(const uint32_t* a, void* u) { (void)u; return make_waitable(jpt_event_new(a[1] != 0, a[2] != 0)); }
static uint64_t k_CreateMutexA(const uint32_t* a, void* u) { (void)u; return make_waitable(jpt_mutex_new(a[1] != 0)); }

static uint64_t k_CreateSemaphoreA(const uint32_t* a, void* u) {
    (void)u;
    if ((int32_t)a[2] <= 0 || (int32_t)a[1] < 0 || (int32_t)a[1] > (int32_t)a[2]) return set_error(ERROR_INVALID_PARAMETER), 0;
    return make_waitable(jpt_semaphore_new((int32_t)a[1], (int32_t)a[2]));
}

static jpt_waitable_t* waitable_of(uint32_t value) {
    handle_t* h = handle_at(value);
    if (h && h->kind == H_THREAD) return jpt_thread_waitable(h->thread);
    return h && h->kind == H_WAITABLE ? h->waitable : NULL;
}

/* ---- guest threads ----
 * A guest thread is a host thread with its own register file and guest stack; it runs only while it holds the machine lock,
 * which the running guest thread gives up at its blocking calls. A thread created in an earlier run never runs in a later one. */

#define CREATE_SUSPENDED 4u
#define GUEST_THREAD_STACK (1024u * 1024u)
#define STILL_ACTIVE 259u
#define AUDIO_THREAD_ENTRY 0x00435580u

struct gthread {
    lx_thread_t* machine;
    uint32_t start, param, id, exit_code;
    unsigned run;
    int suspend_count;
    volatile int terminated;
    jpt_waitable_t* gate;
};

#define MAX_GUEST_THREADS 64

static unsigned s_run;
static HOS_THREAD_LOCAL gthread_t* t_gthread;
static gthread_t* s_gthreads[MAX_GUEST_THREADS];
static unsigned s_gthread_count;
static int s_gthreads_live;

static int guest_wait_should_end(void) {
    return t_gthread ? t_gthread->terminated || t_gthread->run != s_run : lx_shutdown_force_due();
}

static void guest_thread_checkpoint(void) {
    if (t_gthread && (t_gthread->terminated || t_gthread->run != s_run)) lx_abort(LX_EXIT, t_gthread->exit_code, "guest thread ended");
}
static uint32_t s_next_thread_id = 0x1000;

static void guest_thread_body(void* arg) {
    gthread_t* g = (gthread_t*)arg;
    g->exit_code = lx_call_guest(g->start, LX_STDCALL, 1, &g->param, 0, NULL);
}

static uint32_t guest_thread_main(void* arg) {
    gthread_t* g = (gthread_t*)arg;
    jpt_wait(g->gate, JPT_INFINITE);
    lx_thread_attach(g->machine);
    t_gthread = g;
    lx_crash_thread_started(g->id, g->start);
    if (g->start == AUDIO_THREAD_ENTRY) lx_crash_audio_stage(LX_AUDIO_GUEST_THREAD);
    if (g->run == s_run && !g->terminated) lx_run(guest_thread_body, g);
    t_gthread = NULL;
    lx_crash_thread_ended();
    lx_thread_detach();
    __atomic_sub_fetch(&s_gthreads_live, 1, __ATOMIC_SEQ_CST);
    return g->exit_code;
}

static uint64_t k_CreateThread(const uint32_t* a, void* u) {
    gthread_t* g = (gthread_t*)calloc(1, sizeof *g);
    handle_t h;
    uint32_t v;
    (void)u;
    if (!g) return set_error(ERROR_NOT_ENOUGH_MEMORY), 0;
    g->machine = lx_thread_new(a[1] ? (a[1] < 65536u ? 65536u : a[1]) : GUEST_THREAD_STACK);
    g->gate = jpt_event_new(1, (a[4] & CREATE_SUSPENDED) == 0);
    g->start = a[2], g->param = a[3], g->run = s_run, g->id = s_next_thread_id++;
    g->suspend_count = (a[4] & CREATE_SUSPENDED) != 0;
    memset(&h, 0, sizeof h);
    h.kind = H_THREAD;
    h.guest_thread = g;
    if (g->machine && g->gate && s_gthread_count < MAX_GUEST_THREADS) {
        s_gthreads[s_gthread_count++] = g;
        __atomic_add_fetch(&s_gthreads_live, 1, __ATOMIC_SEQ_CST);
        h.thread = jpt_thread_create(guest_thread_main, g);
        if (!h.thread) {
            s_gthread_count--;
            __atomic_sub_fetch(&s_gthreads_live, 1, __ATOMIC_SEQ_CST);
        }
    }
    v = h.thread ? new_handle(&h) : 0;
    if (!v) {
        if (h.thread) jpt_thread_detach(h.thread);
        else {
            lx_thread_free(g->machine);
            jpt_waitable_free(g->gate);
            free(g);
        }
        return set_error(ERROR_NOT_ENOUGH_MEMORY), 0;
    }
    if (a[5]) gp_write_u32(a[5], g->id);
    lx_log("Linux HLE", "CreateThread start 0x%08X id %u%s", g->start, g->id, g->suspend_count ? " (suspended)" : "");
    return v;
}

static uint64_t k_ResumeThread(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    uint32_t previous;
    (void)u;
    if (!h || h->kind != H_THREAD) return set_error(ERROR_INVALID_HANDLE), 0xFFFFFFFFu;
    previous = (uint32_t)h->guest_thread->suspend_count;
    if (h->guest_thread->suspend_count > 0 && --h->guest_thread->suspend_count == 0) jpt_event_set(h->guest_thread->gate);
    return previous;
}

static uint64_t k_SuspendThread(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    if (!h || h->kind != H_THREAD) return set_error(ERROR_INVALID_HANDLE), 0xFFFFFFFFu;
    lx_log("Linux HLE", "SuspendThread on thread %u is counted only: a running guest thread yields at its blocking calls", h->guest_thread->id);
    return (uint32_t)h->guest_thread->suspend_count++;
}

static uint64_t k_TerminateThread(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    if (!h || h->kind != H_THREAD) return set_error(ERROR_INVALID_HANDLE), 0;
    h->guest_thread->exit_code = a[1];
    h->guest_thread->terminated = 1;
    jpt_event_set(h->guest_thread->gate);
    lx_log("Linux HLE", "TerminateThread: thread %u ends at its next blocking call or never starts", h->guest_thread->id);
    return 1;
}

static uint64_t k_SetEvent(const uint32_t* a, void* u) {
    (void)u;
    if (jpt_event_set(waitable_of(a[0]))) return 1;
    return set_error(ERROR_INVALID_HANDLE), 0;
}

static uint64_t k_ReleaseSemaphore(const uint32_t* a, void* u) {
    int32_t previous = 0;
    (void)u;
    if (!waitable_of(a[0])) return set_error(ERROR_INVALID_HANDLE), 0;
    if (!jpt_semaphore_release(waitable_of(a[0]), (int32_t)a[1], &previous)) return set_error(ERROR_INVALID_PARAMETER), 0;
    if (a[2]) gp_write_u32(a[2], (uint32_t)previous);
    return 1;
}

static uint64_t k_WaitForSingleObject(const uint32_t* a, void* u) {
    jpt_waitable_t* w = waitable_of(a[0]);
    (void)u;
    lx_blocking_t b;
    uint32_t result;
    if (!w) return set_error(ERROR_INVALID_HANDLE), JPT_WAIT_FAILED;
    lx_blocking_begin(&b);
    result = wait_sliced(&w, 1, 0, a[1]);
    lx_blocking_end(&b);
    guest_thread_checkpoint();
    lx_shutdown_poll();
    return result;
}

static uint64_t k_WaitForSingleObjectEx(const uint32_t* a, void* u) { return k_WaitForSingleObject(a, u); }

static uint64_t k_WaitForMultipleObjects(const uint32_t* a, void* u) {
    jpt_waitable_t* list[64];
    const uint32_t* handles = (const uint32_t*)gm_ptr_checked(a[1], a[0] * 4, 0);
    (void)u;
    if (!handles || a[0] == 0 || a[0] > 64) return set_error(ERROR_INVALID_PARAMETER), JPT_WAIT_FAILED;
    for (uint32_t i = 0; i < a[0]; i++) {
        list[i] = waitable_of(handles[i]);
        if (!list[i]) return set_error(ERROR_INVALID_HANDLE), JPT_WAIT_FAILED;
    }
    {
        lx_blocking_t b;
        uint32_t result;
        lx_blocking_begin(&b);
        result = wait_sliced(list, a[0], a[2] != 0, a[3]);
        lx_blocking_end(&b);
        guest_thread_checkpoint();
        lx_shutdown_poll();
        return result;
    }
}

static void close_handle_object(handle_t* h) {
    if (h->kind == H_THREAD) jpt_thread_detach(h->thread);
    if (h->kind == H_WAITABLE) jpt_waitable_free(h->waitable);
    if (h->kind == H_FILE) {
        stream_vfile_close(h->vfile);
        free(h->guest_name);
        close(h->fd);
    }
    if (h->kind == H_FIND) {
        for (int i = 0; i < h->count; i++) free(h->names[i]);
        free(h->names);
    }
}

static uint64_t k_CloseHandle(const uint32_t* a, void* u) {
    handle_t* h;
    (void)u;
    hos_wrlock(&s_lock);
    h = handle_at(a[0]);
    if (h) {
        close_handle_object(h);
        memset(h, 0, sizeof *h);
    }
    hos_unlock_wr(&s_lock);
    if (!h) return set_error(ERROR_INVALID_HANDLE), 0;
    return 1;
}

static jpt_cs_t* cs_for(uint32_t va, int create) {
    jpt_cs_t* found = NULL;
    hos_wrlock(&s_lock);
    for (int i = 0; i < MAX_CS && !found; i++)
        if (s_cs[i].va == va && s_cs[i].cs) found = s_cs[i].cs;
    if (!found && create)
        for (int i = 0; i < MAX_CS; i++)
            if (!s_cs[i].cs) {
                s_cs[i].va = va;
                s_cs[i].cs = found = jpt_cs_new();
                break;
            }
    hos_unlock_wr(&s_lock);
    return found;
}

static uint64_t k_InitializeCriticalSection(const uint32_t* a, void* u) {
    (void)u;
    cs_for(a[0], 1);
    return 0;
}

static uint64_t k_DeleteCriticalSection(const uint32_t* a, void* u) {
    (void)u;
    hos_wrlock(&s_lock);
    for (int i = 0; i < MAX_CS; i++)
        if (s_cs[i].va == a[0] && s_cs[i].cs) {
            jpt_cs_free(s_cs[i].cs);
            s_cs[i].cs = NULL;
            s_cs[i].va = 0;
        }
    hos_unlock_wr(&s_lock);
    return 0;
}

static uint64_t k_EnterCriticalSection(const uint32_t* a, void* u) {
    jpt_cs_t* cs = cs_for(a[0], 1);
    (void)u;
    if (cs && !jpt_cs_try_enter(cs)) {
        /* The owner may have let go of the machine (a blocking call, a yield) and need it back to leave the section. */
        lx_blocking_t b;
        lx_blocking_begin(&b);
        jpt_cs_enter(cs);
        lx_blocking_end(&b);
    }
    return 0;
}

static uint64_t k_LeaveCriticalSection(const uint32_t* a, void* u) {
    jpt_cs_t* cs = cs_for(a[0], 0);
    (void)u;
    if (cs) jpt_cs_leave(cs);
    return 0;
}

/* ---- memory ---- */

static uint32_t protect_to_gm(uint32_t protect) {
    switch (protect & 0xFF) {
    case 0x01: return GM_PROT_NONE;
    case 0x02: return GM_PROT_R;
    default: return GM_PROT_RW;
    }
}

static uint64_t k_VirtualAlloc(const uint32_t* a, void* u) {
    uint32_t address = a[0], size = a[1], type = a[2], base = address;
    (void)u;
    if (!size) return set_error(ERROR_INVALID_PARAMETER), 0;
    size = (size + GM_PAGE - 1) & ~(GM_PAGE - 1);
    if (type & MEM_RESERVE) {
        base = gm_reserve(address, size, "VirtualAlloc");
        if (!base) return set_error(address ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY), 0;
    } else if (!(type & MEM_COMMIT) || !address) {
        return set_error(ERROR_INVALID_PARAMETER), 0;
    }
    if (type & MEM_COMMIT) {
        uint32_t page = base & ~(GM_PAGE - 1);
        uint32_t end = (base + size + GM_PAGE - 1) & ~(GM_PAGE - 1);
        if (!gm_commit(page, end - page, protect_to_gm(a[3]))) {
            if (type & MEM_RESERVE) gm_release(base);
            return set_error(ERROR_NOT_ENOUGH_MEMORY), 0;
        }
    }
    return base;
}

static uint64_t k_VirtualFree(const uint32_t* a, void* u) {
    (void)u;
    if (a[2] & MEM_RELEASE) return a[1] == 0 && gm_release(a[0]) ? 1 : (set_error(ERROR_INVALID_PARAMETER), 0);
    if (a[2] & MEM_DECOMMIT) {
        uint32_t page = a[0] & ~(GM_PAGE - 1);
        uint32_t size = (a[1] + (a[0] - page) + GM_PAGE - 1) & ~(GM_PAGE - 1);
        return gm_decommit(page, size) ? 1 : (set_error(ERROR_INVALID_PARAMETER), 0);
    }
    return set_error(ERROR_INVALID_PARAMETER), 0;
}

static uint64_t k_GetSystemInfo(const uint32_t* a, void* u) {
    uint32_t info[9] = { 0 };
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    (void)u;
    if (cpus < 1) cpus = 1;
    if (cpus > 32) cpus = 32;
    info[0] = 0;                                  /* PROCESSOR_ARCHITECTURE_INTEL, reserved */
    info[1] = GM_PAGE;
    info[2] = GM_NULL_GUARD_END;
    info[3] = 0x7FFEFFFFu;
    info[4] = cpus == 32 ? 0xFFFFFFFFu : (1u << cpus) - 1u;
    info[5] = (uint32_t)cpus;
    info[6] = 586;                                /* PROCESSOR_INTEL_PENTIUM */
    info[7] = GM_GRANULARITY;
    info[8] = 6;                                  /* wProcessorLevel, wProcessorRevision */
    (void)gp_copy_out(a[0], info, sizeof info);
    return 0;
}

static uint64_t k_GlobalMemoryStatus(const uint32_t* a, void* u) {
    uint32_t m[8] = { 32 };
    long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    uint64_t total = pages > 0 && size > 0 ? (uint64_t)pages * (uint64_t)size : 0x40000000ull;
    uint64_t free_bytes = avail > 0 && size > 0 ? (uint64_t)avail * (uint64_t)size : total / 2;
    (void)u;
    if (total > 0x7FFF0000ull) total = 0x7FFF0000ull;
    if (free_bytes > total) free_bytes = total;
    m[1] = (uint32_t)(100 - free_bytes * 100 / total);
    m[2] = (uint32_t)total;
    m[3] = (uint32_t)free_bytes;
    m[4] = (uint32_t)total;
    m[5] = (uint32_t)free_bytes;
    m[6] = 0x7FFE0000u;
    m[7] = 0x7FFE0000u - 0x00800000u;
    (void)gp_copy_out(a[0], m, sizeof m);
    return 0;
}

static uint64_t k_GlobalHandle(const uint32_t* a, void* u) { (void)u; return a[0]; }
static uint64_t k_GlobalUnlock(const uint32_t* a, void* u) { (void)a; (void)u; return 0; }

static uint64_t k_GlobalFree(const uint32_t* a, void* u) {
    (void)u;
    if (gm_heap_ok(a[0])) {
        gm_heap_free(a[0]);
        return 0;
    }
    return a[0];
}

static uint64_t k_GetStartupInfoA(const uint32_t* a, void* u) {
    uint32_t info[17] = { 68 };
    (void)u;
    (void)gp_copy_out(a[0], info, sizeof info);
    return 0;
}

/* ---- modules ---- */

static int module_index(const char* name) {
    char base[64];
    const char* slash = name;
    size_t n = 0;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/') slash = p + 1;
    for (const char* p = slash; *p && n < sizeof base - 1; p++) base[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    base[n] = 0;
    if (!strchr(base, '.') && n + 4 < sizeof base) strcat(base, ".dll");
    for (int i = 0; i < s_nmodules; i++)
        if (!strcmp(s_module[i], base)) return i;
    return -1;
}

static const char* const k_known_modules[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ddraw.dll",
                                               "dsound.dll", "winmm.dll", "msvcrt.dll", "smackw32.dll", "processor.dll",
                                               "lz32.dll" };

static uint32_t module_handle(int index) { return 0x7FF00000u + 0x1000u * (uint32_t)index; }

static uint64_t k_GetModuleHandleA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    int i;
    (void)u;
    if (!a[0]) return s_image_base;
    if (name && lx_module_find(name)) return lx_module_find(name)->image.base;
    i = name ? module_index(name) : -1;
    if (i < 0) return set_error(ERROR_MOD_NOT_FOUND), 0;
    return module_handle(i);
}

static uint64_t k_LoadLibraryA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    lx_module_t* guest = name ? lx_module_find(name) : NULL;
    int i = name && !guest ? module_index(name) : -1;
    (void)u;
    if (guest) {
        uint32_t base = lx_module_load(guest);
        if (!base) set_error(ERROR_MOD_NOT_FOUND);
        return base;
    }
    if (i < 0) {
        lx_log("hle", "LoadLibraryA(\"%s\") -> not found", name ? name : "?");
        return set_error(ERROR_MOD_NOT_FOUND), 0;
    }
    return module_handle(i);
}

static uint64_t k_FindResourceA(const uint32_t* a, void* u) {
    uint32_t base = a[0] ? a[0] : s_image_base, data, size;
    (void)u;
    if (!lx_resource_find(base, a[2], a[1], &data, &size)) return set_error(1814), 0;
    return data;
}

static uint64_t k_LoadResource(const uint32_t* a, void* u) {
    (void)u;
    return a[1];
}

static uint64_t k_LockResource(const uint32_t* a, void* u) {
    (void)u;
    return a[0];
}

static uint64_t k_FreeResource(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t k_DisableThreadLibraryCalls(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 1;
}

static uint64_t k_FreeLibrary(const uint32_t* a, void* u) {
    lx_module_t* guest = lx_module_by_base(a[0]);
    (void)u;
    if (guest) return lx_module_free(guest) ? 1 : (set_error(ERROR_INVALID_HANDLE), 0);
    return a[0] >= 0x7FF00000u && (a[0] - 0x7FF00000u) / 0x1000u < (uint32_t)s_nmodules;
}

static uint64_t k_GetProcAddress(const uint32_t* a, void* u) {
    const char* name = a[1] > 0xFFFFu ? cstr(a[1]) : NULL;
    uint32_t index = a[0] - 0x7FF00000u;
    lx_module_t* guest = lx_module_by_base(a[0]);
    (void)u;
    if (guest) {
        uint32_t va = a[1] > 0xFFFFu ? (name ? lx_module_export(guest, name, 0) : 0) : lx_module_export(guest, NULL, (uint16_t)a[1]);
        if (!va) set_error(127);
        return va;
    }
    if (!name || a[0] < 0x7FF00000u || index % 0x1000u || index / 0x1000u >= (uint32_t)s_nmodules)
        return set_error(ERROR_INVALID_PARAMETER), 0;
    return hle_resolve(s_module[index / 0x1000u], name, 0, NULL);
}

static uint64_t k_GetModuleFileNameA(const uint32_t* a, void* u) {
    size_t n = strlen(s_exe_path);
    char* out;
    (void)u;
    if (a[0] && a[0] != s_image_base) return set_error(ERROR_MOD_NOT_FOUND), 0;
    if (!a[2]) return 0;
    if (n >= a[2]) n = a[2] - 1;
    out = (char*)gm_ptr_checked(a[1], (uint32_t)n + 1, 1);
    if (!out) return set_error(ERROR_INVALID_PARAMETER), 0;
    memcpy(out, s_exe_path, n);
    out[n] = 0;
    return n;
}

/* ---- files ---- */

static uint32_t error_of_errno(void) {
    switch (errno) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

static int resolve(const char* guest, int create, char* host, size_t cap) {
    int r = jpt_vpath_resolve(s_vpath, guest, create, host, cap);
    if (r == JPT_VP_OK) return 1;
    set_error(r == JPT_VP_NOT_FOUND ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
    return 0;
}

static int pread_exact(void* context, uint64_t offset, void* buffer, size_t bytes) {
    int fd = (int)(intptr_t)context;
    uint8_t* out = (uint8_t*)buffer;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, out + done, bytes - done, (off_t)(offset + done));
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static stream_vfile_t* open_stream_view(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return NULL;
    return stream_vfile_open(pread_exact, (void*)(intptr_t)fd, (uint64_t)st.st_size);
}

static uint64_t k_CreateFileA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    uint32_t access = a[1], disposition = a[4];
    char host[JPT_PATH_MAX], translated[JPT_PATH_MAX];
    int flags = 0, fd, served;
    handle_t h;
    (void)u;
    if (!name || !s_vpath) return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    served = disposition == OPEN_EXISTING && stream_pack_serves_open(name, access, a[5]);
    if (served) flags = O_RDONLY;
    if (!resolve(name, disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING, host, sizeof host)) return INVALID_HANDLE;
    if (disposition == OPEN_EXISTING && l10n_menu_file(name, translated, sizeof translated)) memcpy(host, translated, strlen(translated) + 1), served = 0;
    switch (disposition) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    case OPEN_EXISTING: break;
    default: return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    }
    fd = open(host, flags | O_CLOEXEC, 0666);
    if (fd < 0 && disposition == OPEN_EXISTING && (flags & (O_WRONLY | O_RDWR)) && (errno == EACCES || errno == EROFS)) {
        /* A game folder on a read-only medium: the game opens its data files read-write, and only ever reads them. */
        if (!(flags & O_WRONLY)) fd = open(host, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) return set_error(error_of_errno()), INVALID_HANDLE;
    if (flags & O_CREAT) jpt_vpath_invalidate(s_vpath, host);
    memset(&h, 0, sizeof h);
    h.kind = H_FILE;
    h.fd = fd;
    if (served) h.vfile = open_stream_view(fd);
    h.guest_name = strdup(name);
    {
        uint32_t v = new_handle(&h);
        if (!v) {
            stream_vfile_close(h.vfile);
            free(h.guest_name);
            close(fd);
            return INVALID_HANDLE;
        }
        return v;
    }
}

static handle_t* file_of(uint32_t value) {
    handle_t* h = handle_at(value);
    return h && h->kind == H_FILE ? h : NULL;
}

int hle_file_name(uint32_t handle, char* out, size_t capacity) {
    const handle_t* h = file_of(handle);
    if (!h || !h->guest_name || strlen(h->guest_name) >= capacity) return 0;
    strcpy(out, h->guest_name);
    return 1;
}

int hle_file_fd(uint32_t handle) {
    const handle_t* h = file_of(handle);
    return h ? h->fd : -1;
}

static uint64_t k_ReadFile(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    void* buf = a[2] ? gm_ptr_checked(a[1], a[2], 1) : NULL;
    ssize_t n;
    (void)u;
    if (!h) return set_error(ERROR_INVALID_HANDLE), 0;
    if (a[2] && !buf) return set_error(ERROR_INVALID_PARAMETER), 0;
    if (h->vfile) {
        uint32_t got = 0;
        if (!stream_vfile_read(h->vfile, buf, a[2], &got)) return set_error(ERROR_READ_FAULT), 0;
        if (a[3]) gp_write_u32(a[3], got);
        return 1;
    }
    n = a[2] ? read(h->fd, buf, a[2]) : 0;
    if (n < 0) return set_error(error_of_errno()), 0;
    if (a[3]) gp_write_u32(a[3], (uint32_t)n);
    return 1;
}

static uint64_t k_WriteFile(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    const void* buf = a[2] ? gm_ptr_checked(a[1], a[2], 0) : NULL;
    ssize_t n;
    (void)u;
    if (!h) return set_error(ERROR_INVALID_HANDLE), 0;
    if (a[2] && !buf) return set_error(ERROR_INVALID_PARAMETER), 0;
    if (h->vfile) return set_error(ERROR_ACCESS_DENIED), 0;
    n = a[2] ? write(h->fd, buf, a[2]) : 0;
    if (n < 0) return set_error(error_of_errno()), 0;
    if (a[3]) gp_write_u32(a[3], (uint32_t)n);
    return 1;
}

static uint64_t k_SetFilePointer(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    int64_t distance = (int32_t)a[1];
    uint32_t high = 0;
    off_t at;
    (void)u;
    if (!h) return set_error(ERROR_INVALID_HANDLE), INVALID_HANDLE;
    if (a[2] && gp_read_u32(a[2], &high)) distance = (int64_t)((uint64_t)high << 32 | a[1]);
    if (h->vfile) {
        uint64_t position = 0;
        if (!stream_vfile_seek(h->vfile, distance, a[3], &position)) return set_error(ERROR_NEGATIVE_SEEK), INVALID_HANDLE;
        if (a[2]) gp_write_u32(a[2], (uint32_t)(position >> 32));
        set_error(0);
        return (uint32_t)position;
    }
    at = lseek(h->fd, (off_t)distance, a[3] == 1 ? SEEK_CUR : a[3] == 2 ? SEEK_END : SEEK_SET);
    if (at < 0) return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    if (a[2]) gp_write_u32(a[2], (uint32_t)((uint64_t)at >> 32));
    return (uint32_t)at;
}

static uint64_t k_GetFileSize(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    struct stat st;
    (void)u;
    if (!h || fstat(h->fd, &st) != 0) return set_error(ERROR_INVALID_HANDLE), INVALID_HANDLE;
    if (h->vfile) st.st_size = (off_t)stream_vfile_size(h->vfile);
    if (a[1]) gp_write_u32(a[1], (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}

static uint32_t attributes_of(const struct stat* st) {
    uint32_t attributes = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!(st->st_mode & S_IWUSR)) attributes = (attributes & FILE_ATTRIBUTE_DIRECTORY) | FILE_ATTRIBUTE_READONLY;
    return attributes;
}

static uint64_t k_GetDiskFreeSpaceA(const uint32_t* a, void* u) {
    const char* name = a[0] ? cstr(a[0]) : "C:\\";
    char host[JPT_PATH_MAX];
    struct statvfs sv;
    const uint32_t sectors_per_cluster = 8, bytes_per_sector = 512, cluster = sectors_per_cluster * bytes_per_sector;
    uint64_t free_clusters, total_clusters;
    (void)u;
    if (!name || !s_vpath || !resolve(name, 0, host, sizeof host) || statvfs(host, &sv) != 0) return set_error(ERROR_PATH_NOT_FOUND), 0;
    free_clusters = (uint64_t)sv.f_bavail * sv.f_frsize / cluster;
    total_clusters = (uint64_t)sv.f_blocks * sv.f_frsize / cluster;
    /* The game multiplies the three values in 32 bits: sizes are capped at 2 GB, as Windows 95 reported them. */
    if (free_clusters > 0x7FFFFFFFu / cluster) free_clusters = 0x7FFFFFFFu / cluster;
    if (total_clusters > 0x7FFFFFFFu / cluster) total_clusters = 0x7FFFFFFFu / cluster;
    if (a[1]) gp_write_u32(a[1], sectors_per_cluster);
    if (a[2]) gp_write_u32(a[2], bytes_per_sector);
    if (a[3]) gp_write_u32(a[3], (uint32_t)free_clusters);
    if (a[4]) gp_write_u32(a[4], (uint32_t)total_clusters);
    return 1;
}

static uint64_t k_GetFileAttributesA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    struct stat st;
    (void)u;
    if (!name || !s_vpath) return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    if (!resolve(name, 0, host, sizeof host) || stat(host, &st) != 0) return set_error(ERROR_FILE_NOT_FOUND), INVALID_HANDLE;
    return attributes_of(&st);
}

static uint64_t k_SetFileAttributesA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    struct stat st;
    (void)u;
    if (!name || !s_vpath || !resolve(name, 0, host, sizeof host) || stat(host, &st) != 0) return set_error(ERROR_FILE_NOT_FOUND), 0;
    if (a[1] & FILE_ATTRIBUTE_READONLY) chmod(host, st.st_mode & ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH));
    else chmod(host, st.st_mode | S_IWUSR);
    return 1;
}

static uint64_t k_DeleteFileA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    (void)u;
    if (!name || !s_vpath || !resolve(name, 0, host, sizeof host)) return 0;
    if (unlink(host) != 0) return set_error(error_of_errno()), 0;
    jpt_vpath_invalidate(s_vpath, host);
    lx_frontend_file_deleted(name);
    return 1;
}

static uint64_t k_SetCurrentDirectoryA(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    (void)u;
    if (!name || !s_vpath || !resolve(name, 0, host, sizeof host) || !jpt_fs_is_dir(host)) return set_error(ERROR_PATH_NOT_FOUND), 0;
    return jpt_vpath_chdir(s_vpath, name) ? 1 : 0;
}

static int fold_char(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int wildcard_match(const char* pattern, const char* name) {
    if (!*pattern) return *name == 0;
    if (*pattern == '*') {
        for (;; name++) {
            if (wildcard_match(pattern + 1, name)) return 1;
            if (!*name) return 0;
        }
    }
    if (!*name) return 0;
    if (*pattern != '?' && fold_char((unsigned char)*pattern) != fold_char((unsigned char)*name)) return 0;
    return wildcard_match(pattern + 1, name + 1);
}

typedef struct {
    char** names;
    int count, cap;
    const char* pattern;
} find_collect_t;

static void find_visit(const char* name, int is_dir, void* user) {
    find_collect_t* c = (find_collect_t*)user;
    (void)is_dir;
    if (!wildcard_match(c->pattern, name)) return;
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        char** grown = (char**)realloc(c->names, (size_t)cap * sizeof *grown);
        if (!grown) return;
        c->names = grown;
        c->cap = cap;
    }
    c->names[c->count] = strdup(name);
    if (c->names[c->count]) c->count++;
}

static int by_name(const void* x, const void* y) { return strcasecmp(*(char* const*)x, *(char* const*)y); }

static void filetime_of(time_t t, uint8_t* out) {
    uint64_t ticks = ((uint64_t)t + 11644473600ull) * 10000000ull;
    memcpy(out, &ticks, 8);
}

static uint64_t k_GetFileTime(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    struct stat st;
    uint8_t t[8];
    (void)u;
    if (!h || fstat(h->fd, &st) != 0) return set_error(ERROR_INVALID_HANDLE), 0;
    if (a[1]) filetime_of(st.st_ctime, t), gp_copy_out(a[1], t, 8);
    if (a[2]) filetime_of(st.st_atime, t), gp_copy_out(a[2], t, 8);
    if (a[3]) filetime_of(st.st_mtime, t), gp_copy_out(a[3], t, 8);
    return 1;
}

static uint64_t k_SetFileTime(const uint32_t* a, void* u) {
    handle_t* h = file_of(a[0]);
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    (void)u;
    if (!h) return set_error(ERROR_INVALID_HANDLE), 0;
    for (int i = 0; i < 2; i++) {
        uint64_t ticks;
        if (a[2 + i] && gp_copy_in(&ticks, a[2 + i], 8) && ticks)
            ts[i].tv_sec = (time_t)(ticks / 10000000ull - 11644473600ull), ts[i].tv_nsec = (long)(ticks % 10000000ull) * 100;
    }
    if (futimens(h->fd, ts) != 0) return set_error(error_of_errno()), 0;
    return 1;
}

static uint64_t k_CompareFileTime(const uint32_t* a, void* u) {
    uint64_t x, y;
    (void)u;
    if (!gp_copy_in(&x, a[0], 8) || !gp_copy_in(&y, a[1], 8)) return 0;
    return x < y ? 0xFFFFFFFFu : x > y ? 1u : 0u;
}

static int next_find(handle_t* h, struct stat* st, const char** name) {
    char host[JPT_PATH_MAX + 300];
    if (h->next >= h->count) return 0;
    *name = h->names[h->next++];
    snprintf(host, sizeof host, "%s/%s", h->dir, *name);
    if (stat(host, st) != 0) memset(st, 0, sizeof *st);
    return 1;
}

static int fill_find_data(handle_t* h, uint32_t data_va) {
    uint8_t data[320];
    struct stat st;
    const char* name;
    memset(data, 0, sizeof data);
    if (!next_find(h, &st, &name)) return 0;
    {
        uint32_t attributes = attributes_of(&st), high = (uint32_t)((uint64_t)st.st_size >> 32), low = (uint32_t)st.st_size;
        memcpy(data, &attributes, 4);
        filetime_of(st.st_ctime, data + 4);
        filetime_of(st.st_atime, data + 12);
        filetime_of(st.st_mtime, data + 20);
        memcpy(data + 28, &high, 4);
        memcpy(data + 32, &low, 4);
    }
    strncpy((char*)data + 44, name, 259);
    return gp_copy_out(data_va, data, sizeof data);
}

/* The collected listing of a pattern as a find handle (kernel32 and the C runtime share it); INVALID_HANDLE with the error set. */
static uint32_t find_open(const char* pattern) {
    char dir[JPT_PATH_MAX], host[JPT_PATH_MAX];
    const char* last = pattern;
    handle_t h;
    find_collect_t c = { NULL, 0, 0, NULL };
    size_t n;
    uint32_t v;
    if (!pattern || !s_vpath) return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    for (const char* p = pattern; *p; p++)
        if (*p == '\\' || *p == '/') last = p + 1;
    n = (size_t)(last - pattern);
    if (n >= sizeof dir) return set_error(ERROR_INVALID_PARAMETER), INVALID_HANDLE;
    memcpy(dir, pattern, n);
    dir[n] = 0;
    if (!n) strcpy(dir, ".");
    if (!resolve(dir, 0, host, sizeof host)) return set_error(ERROR_PATH_NOT_FOUND), INVALID_HANDLE;
    c.pattern = last;
    if (!jpt_fs_list(host, find_visit, &c)) return set_error(ERROR_PATH_NOT_FOUND), INVALID_HANDLE;
    if (!c.count) {
        free(c.names);
        return set_error(ERROR_FILE_NOT_FOUND), INVALID_HANDLE;
    }
    qsort(c.names, (size_t)c.count, sizeof *c.names, by_name);
    memset(&h, 0, sizeof h);
    h.kind = H_FIND;
    h.names = c.names;
    h.count = c.count;
    snprintf(h.dir, sizeof h.dir, "%s", host);
    v = new_handle(&h);
    if (!v) {
        for (int i = 0; i < c.count; i++) free(c.names[i]);
        free(c.names);
        return INVALID_HANDLE;
    }
    return v;
}

static uint64_t k_FindFirstFileA(const uint32_t* a, void* u) {
    uint32_t v = find_open(cstr(a[0]));
    (void)u;
    if (v != INVALID_HANDLE) fill_find_data(handle_at(v), a[1]);
    return v;
}

static uint64_t k_FindNextFileA(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    if (!h || h->kind != H_FIND) return set_error(ERROR_INVALID_HANDLE), 0;
    if (!fill_find_data(h, a[1])) return set_error(ERROR_NO_MORE_FILES), 0;
    return 1;
}

static uint64_t k_FindClose(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    if (!h || h->kind != H_FIND) return set_error(ERROR_INVALID_HANDLE), 0;
    return k_CloseHandle(a, NULL);
}

/* struct _finddata_t of the 32-bit C runtime: attrib, create, access and write time (time_t), size, name[260]. */
static int fill_finddata(handle_t* h, uint32_t va) {
    uint8_t data[280];
    struct stat st;
    const char* name;
    uint32_t v[5];
    memset(data, 0, sizeof data);
    if (!next_find(h, &st, &name)) return 0;
    v[0] = attributes_of(&st), v[1] = (uint32_t)st.st_ctime, v[2] = (uint32_t)st.st_atime, v[3] = (uint32_t)st.st_mtime, v[4] = (uint32_t)st.st_size;
    memcpy(data, v, sizeof v);
    strncpy((char*)data + 20, name, 259);
    return gp_copy_out(va, data, sizeof data);
}

static uint64_t c_findfirst(const uint32_t* a, void* u) {
    uint32_t v = find_open(cstr(a[0]));
    (void)u;
    if (v == INVALID_HANDLE) return 0xFFFFFFFFu;
    fill_finddata(handle_at(v), a[1]);
    return v;
}

static uint64_t c_findnext(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    return h && h->kind == H_FIND && fill_finddata(h, a[1]) ? 0u : 0xFFFFFFFFu;
}

static uint64_t c_findclose(const uint32_t* a, void* u) {
    handle_t* h = handle_at(a[0]);
    (void)u;
    return h && h->kind == H_FIND && k_CloseHandle(a, NULL) ? 0u : 0xFFFFFFFFu;
}

/* ---- the table ---- */

#define K32(fn, argc) { "kernel32.dll", #fn, k_##fn, argc, HT_STDCALL }

static const hle_entry_t k_table[] = {
    K32(Sleep, 1), K32(SleepEx, 2), K32(GetTickCount, 0), K32(GetLastError, 0), K32(ExitProcess, 1),
    K32(OutputDebugStringA, 1), K32(lstrlenA, 1), K32(lstrcpyA, 2),
    K32(CreateEventA, 4), K32(CreateMutexA, 3), K32(CreateSemaphoreA, 4), K32(SetEvent, 1), K32(ReleaseSemaphore, 3),
    K32(WaitForSingleObject, 2), K32(WaitForSingleObjectEx, 3), K32(WaitForMultipleObjects, 4), K32(CloseHandle, 1),
    K32(InitializeCriticalSection, 1), K32(DeleteCriticalSection, 1), K32(EnterCriticalSection, 1),
    K32(LeaveCriticalSection, 1),
    K32(VirtualAlloc, 4), K32(VirtualFree, 3), K32(GetSystemInfo, 1), K32(GlobalMemoryStatus, 1), K32(GlobalHandle, 1),
    K32(GlobalUnlock, 1), K32(GlobalFree, 1), K32(GetStartupInfoA, 1),
    K32(GetModuleHandleA, 1), K32(DisableThreadLibraryCalls, 1), K32(LoadLibraryA, 1), K32(FreeLibrary, 1), K32(GetProcAddress, 2), K32(GetModuleFileNameA, 3),
    K32(CreateFileA, 7), K32(ReadFile, 5), K32(WriteFile, 5), K32(SetFilePointer, 4), K32(GetFileSize, 2),
    K32(FindResourceA, 3), K32(LoadResource, 2), K32(LockResource, 1), K32(FreeResource, 1), K32(CreateThread, 6), K32(ResumeThread, 1), K32(SuspendThread, 1), K32(TerminateThread, 2), K32(GetDiskFreeSpaceA, 5), K32(GetFileAttributesA, 1), K32(SetFileAttributesA, 2), K32(DeleteFileA, 1), K32(SetCurrentDirectoryA, 1),
    K32(GetFileTime, 4), K32(SetFileTime, 4), K32(CompareFileTime, 2), K32(FindFirstFileA, 2), K32(FindNextFileA, 2), K32(FindClose, 1),
    { "msvcrt.dll", "_findfirst", c_findfirst, 2, HT_CDECL }, { "msvcrt.dll", "_findnext", c_findnext, 2, HT_CDECL },
    { "msvcrt.dll", "_findclose", c_findclose, 1, HT_CDECL },
};

static const hle_entry_t* kernel32_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}

typedef const hle_entry_t* (*table_fn)(size_t* count);

static const table_fn k_providers[] = { kernel32_table, hle_msvcrt_table, hle_msvcrt_io_table, hle_user32_table,
                                         hle_advapi32_table, hle_processor_table, hle_gdi32_table, hle_dsound_table, hle_ddraw_table, hle_lz32_table, hle_smack_table };

size_t hle_entry_count(void) {
    size_t total = 0, n;
    for (size_t i = 0; i < sizeof k_providers / sizeof k_providers[0]; i++) {
        k_providers[i](&n);
        total += n;
    }
    return total;
}

jpt_vpath_t* hle_vpath(void) { return s_vpath; }

const hle_entry_t* hle_entry_at(size_t index) {
    for (size_t i = 0; i < sizeof k_providers / sizeof k_providers[0]; i++) {
        size_t n;
        const hle_entry_t* table = k_providers[i](&n);
        if (index < n) return &table[index];
        index -= n;
    }
    return NULL;
}

static void lower(char* out, size_t cap, const char* in) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = (char)((in[i] >= 'A' && in[i] <= 'Z') ? in[i] + 32 : in[i]);
    out[i] = 0;
}

const hle_entry_t* hle_find(const char* dll, const char* name) {
    char d[64];
    if (!dll || !name) return NULL;
    lower(d, sizeof d, dll);
    for (size_t i = 0; i < hle_entry_count(); i++) {
        const hle_entry_t* e = hle_entry_at(i);
        if (!strcmp(e->dll, d) && !strcmp(e->name, name)) return e;
    }
    return NULL;
}

static void stream_log(const char* line) { lx_log("Linux STREAM", "%s", line); }

void hle_init(jpt_vpath_t* vpath, uint32_t image_base, const char* exe_guest_path) {
    s_vpath = vpath;
    s_image_base = image_base;
    s_run++;
    hle_msvcrt_init("C:\\trespass.exe");
    lx_screen_reset();
    hle_user32_init();
    hle_gdi32_init();
    hle_ddraw_init();
    hle_smack_init();
    stream_catalog_init(stream_log);
    snprintf(s_exe_path, sizeof s_exe_path, "%s", exe_guest_path ? exe_guest_path : "");
    s_nmodules = 0;
    for (size_t i = 0; i < sizeof k_known_modules / sizeof k_known_modules[0] && s_nmodules < MAX_MODULES; i++)
        snprintf(s_module[s_nmodules++], sizeof s_module[0], "%s", k_known_modules[i]);
}

void hle_shutdown(void) {
    hos_wrlock(&s_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        close_handle_object(&s_handles[i]);
        memset(&s_handles[i], 0, sizeof s_handles[i]);
    }
    for (int i = 0; i < MAX_CS; i++) {
        jpt_cs_free(s_cs[i].cs);
        s_cs[i].cs = NULL;
    }
    hos_unlock_wr(&s_lock);
    hle_user32_shutdown();
    hle_gdi32_shutdown();
    hle_smack_shutdown();
    hle_dsound_shutdown();
    hle_ddraw_shutdown();
    hle_lz32_shutdown();
    s_vpath = NULL;
}

unsigned hle_guest_threads_live(void) { return (unsigned)__atomic_load_n(&s_gthreads_live, __ATOMIC_SEQ_CST); }

void hle_handle_stats(hle_handle_stats_t* out) {
    memset(out, 0, sizeof *out);
    hos_wrlock(&s_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        switch (s_handles[i].kind) {
        case H_FILE: out->files++; if (s_handles[i].vfile) out->stream_views++; break;
        case H_FIND: out->finds++; break;
        case H_THREAD: out->threads++; break;
        case H_WAITABLE: out->waitables++; break;
        default: break;
        }
    }
    hos_unlock_wr(&s_lock);
}

void hle_retire_guest_threads(void) { s_run++; }

int hle_join_guest_threads(unsigned timeout_ms) {
    s_run++;
    for (unsigned i = 0; i < s_gthread_count; i++) {
        s_gthreads[i]->terminated = 1;
        jpt_event_set(s_gthreads[i]->gate);
    }
    for (unsigned waited = 0; __atomic_load_n(&s_gthreads_live, __ATOMIC_SEQ_CST) > 0; waited++) {
        if (waited >= timeout_ms) {
            lx_log("hle", "%d guest threads did not end within %u ms", __atomic_load_n(&s_gthreads_live, __ATOMIC_SEQ_CST), timeout_ms);
            return 0;
        }
        jpt_sleep_ms(1);
    }
    s_gthread_count = 0;
    return 1;
}

void hle_set_exit_handler(void (*handler)(uint32_t code)) { s_exit_handler = handler; }
unsigned hle_missing_count(void) { return s_missing; }

void hle_set_permissive(int on) { s_permissive = on; }
unsigned hle_stubbed_count(void) { return s_stubbed; }

static uint64_t diagnostic_stub(const uint32_t* args, void* user) {
    (void)args;
    lx_log("hle", "Linux HLE diagnostic stub returned 0: %s (guest EIP 0x%08X)", (const char*)user, lx_guest_eip());
    return 0;
}

static uint64_t missing_import(const uint32_t* args, void* user) {
    (void)args;
    lx_log("hle", "Linux HLE missing: %s (guest EIP 0x%08X)", (const char*)user, lx_guest_eip());
    if (s_exit_handler) s_exit_handler(HLE_EXIT_MISSING);
    else exit(HLE_EXIT_MISSING);
    return 0;
}

#define MAX_SEEN 512
static char* s_seen[MAX_SEEN];
static int s_nseen;

const hle_entry_t* hle_find_import(const char* dll, const char* name, uint16_t ordinal) {
    char by_ordinal[16];
    snprintf(by_ordinal, sizeof by_ordinal, "#%u", (unsigned)ordinal);
    return hle_find(dll, name ? name : by_ordinal);
}

uint32_t hle_resolve(const char* dll, const char* name, uint16_t ordinal, void* user) {
    char label[160], d[64], by_ordinal[16];
    const hle_entry_t* e;
    if (!name) snprintf(by_ordinal, sizeof by_ordinal, "#%u", (unsigned)ordinal);
    e = hle_find(dll, name ? name : by_ordinal);
    uint32_t guest_va = 0;
    (void)user;
    lower(d, sizeof d, dll);
    if (lx_module_resolve(dll, name, ordinal, &guest_va) && guest_va) return guest_va;
    if (e) return ht_register(e->name, e->fn, e->argc, e->conv, NULL);
    if (name && !strcmp(d, "msvcrt.dll") && hle_msvcrt_data(name)) return hle_msvcrt_data(name);
    if (name) snprintf(label, sizeof label, "%s!%s", d, name);
    else snprintf(label, sizeof label, "%s!#%u", d, ordinal);
    if (s_permissive && name) {
        int argc = 0, conv = HT_STDCALL;
        if (hle_signature(d, name, &argc, &conv)) {
            s_stubbed++;
            for (int i = 0; i < s_nseen; i++)
                if (!strcmp(s_seen[i], label)) return ht_register(s_seen[i], diagnostic_stub, argc, conv, s_seen[i]);
            if (s_nseen < MAX_SEEN && (s_seen[s_nseen] = strdup(label)) != NULL) {
                s_nseen++;
                return ht_register(s_seen[s_nseen - 1], diagnostic_stub, argc, conv, s_seen[s_nseen - 1]);
            }
        }
    }
    for (int i = 0; i < s_nseen; i++)
        if (!strcmp(s_seen[i], label)) return ht_register(s_seen[i], missing_import, 0, HT_STDCALL, s_seen[i]);
    if (s_nseen < MAX_SEEN && (s_seen[s_nseen] = strdup(label)) != NULL) {
        s_missing++;
        s_nseen++;
        return ht_register(s_seen[s_nseen - 1], missing_import, 0, HT_STDCALL, s_seen[s_nseen - 1]);
    }
    return 0;
}
