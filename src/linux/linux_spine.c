#include "linux_spine.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "host_os.h"
#include "jpt_sync.h"
#include "linux_crash.h"
#include "linux_log.h"
#include "linux_shutdown.h"
#include "recomp_types.h"

#define GUEST_STACK_GUARD 0x1000u
#define FPU_CONTROL_DEFAULT 0x027F

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;
uint16_t g_fpu_cw = FPU_CONTROL_DEFAULT;
uint32_t g_fs_base, g_gs_base, g_es_base;
ptrdiff_t g_mem_base;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;
uint32_t g_recomp_site_lo, g_recomp_site_hi, g_recomp_fixed_lo, g_recomp_fixed_span;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, esi, edi, ebp;
    uint32_t fs_base, cur_func, flag_k, flag_a, flag_b, flag_cf;
    int fp_top;
    uint16_t fpu_cw;
    double st[8];
    int64_t sti[8];
} regs_t;

typedef struct run_frame {
    jmp_buf jump;
    struct run_frame* prev;
} run_frame_t;

struct lx_thread {
    regs_t regs;
    uint32_t stack_lo, stack_hi, tib;
    run_frame_t* frame;
    lx_diag_t diag;
};

#define MAX_TABLES 8

typedef struct {
    lx_table_t table;
    uint32_t lo, hi;
} range_table_t;

static range_table_t s_tables[MAX_TABLES];
static unsigned s_ntables;
static jpt_cs_t* s_machine;
static HOS_THREAD_LOCAL lx_thread_t* t_current;
static HOS_THREAD_LOCAL uint32_t t_import_va;
static lx_diag_t s_orphan_diag;

void recomp_x87_recentre(void) {
    double v[8];
    int64_t w[8];
    memcpy(v, g_stp, sizeof v);
    memcpy(w, g_sti64p, sizeof w);
    g_stp = g_stbuf + X87_RING / 2;
    g_sti64p = g_sti64buf + X87_RING / 2;
    memcpy(g_stp, v, sizeof v);
    memcpy(g_sti64p, w, sizeof w);
}

static void save_regs(regs_t* r) {
    r->eax = g_eax, r->ecx = g_ecx, r->edx = g_edx, r->ebx = g_ebx, r->esp = g_esp, r->esi = g_esi, r->edi = g_edi,
    r->ebp = g_ebp;
    r->fs_base = g_fs_base, r->cur_func = g_cur_func;
    r->flag_k = g_flag_k, r->flag_a = g_flag_a, r->flag_b = g_flag_b, r->flag_cf = g_flag_cf;
    r->fp_top = g_fp_top, r->fpu_cw = g_fpu_cw;
    memcpy(r->st, g_stp, sizeof r->st);
    memcpy(r->sti, g_sti64p, sizeof r->sti);
}

static void load_regs(const regs_t* r) {
    g_eax = r->eax, g_ecx = r->ecx, g_edx = r->edx, g_ebx = r->ebx, g_esp = r->esp, g_esi = r->esi, g_edi = r->edi,
    g_ebp = r->ebp;
    g_fs_base = r->fs_base, g_cur_func = r->cur_func;
    g_flag_k = r->flag_k, g_flag_a = r->flag_a, g_flag_b = r->flag_b, g_flag_cf = r->flag_cf;
    g_fp_top = r->fp_top, g_fpu_cw = r->fpu_cw;
    g_stp = g_stbuf + X87_RING / 2;
    g_sti64p = g_sti64buf + X87_RING / 2;
    memcpy(g_stp, r->st, sizeof r->st);
    memcpy(g_sti64p, r->sti, sizeof r->sti);
}

static lx_guest_fn search(lx_table_t table, uint32_t va) {
    size_t lo = 0, hi = table.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table.entries[mid].va == va) return table.entries[mid].fn;
        if (table.entries[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

lx_guest_fn lx_find_entry(uint32_t va) {
    for (unsigned i = 0; i < s_ntables; i++)
        if (va >= s_tables[i].lo && va <= s_tables[i].hi) {
            lx_guest_fn fn = search(s_tables[i].table, va);
            if (fn) return fn;
        }
    return NULL;
}

void lx_spine_add_table(lx_table_t table) {
    if (!table.count || s_ntables == MAX_TABLES) return;
    s_tables[s_ntables].table = table;
    s_tables[s_ntables].lo = table.entries[0].va;
    s_tables[s_ntables].hi = table.entries[table.count - 1].va;
    s_ntables++;
}

static lx_diag_t* diag_slot(void) { return t_current ? &t_current->diag : &s_orphan_diag; }
const lx_diag_t* lx_diag(void) { return diag_slot(); }
void lx_diag_reset(void) { memset(diag_slot(), 0, sizeof(lx_diag_t)); }
uint32_t lx_current_import_va(void) { return t_import_va; }

void lx_abort(lx_status_t status, uint32_t va, const char* what) {
    lx_diag_t* d = diag_slot();
    d->status = status;
    d->va = va;
    d->caller = g_cur_func;
    snprintf(d->what, sizeof d->what, "%s", what ? what : "");
    if (t_current && t_current->frame) longjmp(t_current->frame->jump, 1);
    lx_log("spine", "fatal outside a run frame: status %d va 0x%08X", (int)status, va);
    abort();
}

void recomp_not_lifted(uint32_t va) { lx_abort(LX_NOT_LIFTED, va, "function not lifted"); }

#define MAX_REACHED 512
static uint32_t s_reached[MAX_REACHED];
static unsigned s_nreached;

static void note_reached(uint32_t va) {
    for (unsigned i = 0; i < s_nreached; i++)
        if (s_reached[i] == va) return;
    if (s_nreached < MAX_REACHED) s_reached[s_nreached++] = va;
}

unsigned lx_reached_imports(const uint32_t** vas) {
    if (vas) *vas = s_reached;
    return s_nreached;
}

uint32_t lx_guest_eip(void) { return g_cur_func; }

/* Guest threads waiting for the machine lock. The lock is not fair, so a thread that only polls (never sleeps or waits) would keep
 * the others from ever running; the import path hands the machine over now and then while any thread is waiting. */
static int s_waiting;
static unsigned s_import_calls;

#define YIELD_EVERY 64
#define YIELD_NS 100000

static void import_trampoline(void) {
    uint32_t va = t_import_va, esp = g_esp, eax = 0, edx = 0;
    note_reached(va);
    lx_crash_import(va);
    if (lx_shutdown_state() == LX_QUIT_REQUESTED) lx_shutdown_poll();
    if (ht_is_raw(va)) {
        ht_call_raw(va);
        return;
    }
    if (__atomic_load_n(&s_waiting, __ATOMIC_RELAXED) && (++s_import_calls & (YIELD_EVERY - 1)) == 0) {
        lx_blocking_t b;
        struct timespec pause = { 0, YIELD_NS };
        lx_blocking_begin(&b);
        nanosleep(&pause, NULL);
        lx_blocking_end(&b);
    }
    if (!ht_invoke_ex(va, &esp, g_ecx, &eax, &edx)) lx_abort(LX_FATAL_ABI, va, ht_name(va));
    g_esp = esp;
    g_eax = eax;
    g_edx = edx;
}

static void bad_dispatch_trampoline(void) {
    lx_abort(LX_BAD_DISPATCH, t_import_va, "call to an address that is not lifted code or a thunk");
}

static int is_import(uint32_t va) { return ht_is_thunk(va); }

recomp_func_t recomp_lookup(uint32_t va) { return lx_find_entry(va); }
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    t_import_va = va;
    return ht_is_thunk(va) ? import_trampoline : bad_dispatch_trampoline;
}

void lx_spine_init(lx_table_t main_table, lx_table_t selfmod_table) {
    s_nreached = 0;
    s_ntables = 0;
    lx_spine_add_table(main_table);
    lx_spine_add_table(selfmod_table);
    g_mem_base = (ptrdiff_t)(uintptr_t)gm_base();
    g_recomp_site_lo = main_table.count ? main_table.entries[0].va : 0;
    g_recomp_site_hi = main_table.count ? main_table.entries[main_table.count - 1].va + 1 : 0;
    if (selfmod_table.count && selfmod_table.entries[selfmod_table.count - 1].va >= g_recomp_site_hi)
        g_recomp_site_hi = selfmod_table.entries[selfmod_table.count - 1].va + 1;
    g_recomp_fixed_lo = g_recomp_fixed_span = 0;
    recomp_is_import = is_import;
    if (!s_machine) s_machine = jpt_cs_new();
}

lx_thread_t* lx_thread_new(uint32_t stack_bytes) {
    lx_thread_t* t = (lx_thread_t*)calloc(1, sizeof *t);
    uint32_t tib_fields[16] = { 0 };
    if (!t) return NULL;
    t->stack_lo = gm_alloc_pages(0, stack_bytes, "guest stack");
    t->tib = gm_heap_alloc(0x1000, 1);
    if (!t->stack_lo || !t->tib) {
        lx_thread_free(t);
        return NULL;
    }
    t->stack_hi = t->stack_lo + ((stack_bytes + GM_PAGE - 1) & ~(GM_PAGE - 1));
    tib_fields[0] = 0xFFFFFFFFu;
    tib_fields[1] = t->stack_hi;
    tib_fields[2] = t->stack_lo;
    tib_fields[6] = t->tib;
    gp_copy_out(t->tib, tib_fields, sizeof tib_fields);
    t->regs.esp = t->stack_hi - 0x10;
    t->regs.fs_base = t->tib;
    t->regs.fpu_cw = FPU_CONTROL_DEFAULT;
    return t;
}

void lx_thread_free(lx_thread_t* t) {
    if (!t) return;
    if (t->stack_lo) gm_release(t->stack_lo);
    if (t->tib) gm_heap_free(t->tib);
    free(t);
}

uint32_t lx_thread_stack_top(const lx_thread_t* t) { return t->stack_hi; }
uint32_t lx_thread_tib(const lx_thread_t* t) { return t->tib; }
lx_thread_t* lx_thread_current(void) { return t_current; }

static void enter_machine(void) {
    __atomic_add_fetch(&s_waiting, 1, __ATOMIC_RELAXED);
    jpt_cs_enter(s_machine);
    __atomic_sub_fetch(&s_waiting, 1, __ATOMIC_RELAXED);
}

void lx_thread_attach(lx_thread_t* t) {
    enter_machine();
    load_regs(&t->regs);
    t_current = t;
}

void lx_thread_detach(void) {
    if (!t_current) return;
    save_regs(&t_current->regs);
    t_current = NULL;
    jpt_cs_leave(s_machine);
}

void lx_blocking_begin(lx_blocking_t* b) {
    b->thread = t_current;
    if (!b->thread) return;
    save_regs(&b->thread->regs);
    t_current = NULL;
    jpt_cs_leave(s_machine);
}

void lx_blocking_end(lx_blocking_t* b) {
    if (!b->thread) return;
    enter_machine();
    load_regs(&b->thread->regs);
    t_current = b->thread;
}

static void push32(uint32_t value) {
    g_esp -= 4;
    if (!gp_write_u32(g_esp, value)) lx_abort(LX_FATAL_ABI, g_esp, "guest stack is not writable");
}

uint32_t lx_call_guest(uint32_t va, int convention, int argc, const uint32_t* args, uint32_t this_ecx, uint32_t* edx_out) {
    lx_guest_fn fn = lx_find_entry(va);
    uint32_t saved_esp = g_esp, saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi, saved_ebp = g_ebp;
    uint32_t saved_k = g_flag_k, saved_a = g_flag_a, saved_b = g_flag_b, saved_cf = g_flag_cf, saved_func = g_cur_func;
    uint32_t after_call, result, result_high;
    int stdcall_like = convention != LX_CDECL;

    if (!fn && !ht_is_thunk(va)) {
        char what[96];
        snprintf(what, sizeof what, "host call to 0x%08X, which is not lifted code or a thunk", va);
        lx_abort(LX_BAD_DISPATCH, va, what);
    }
    if (argc < 0 || argc > 64) lx_abort(LX_FATAL_ABI, va, "argument count");
    if (t_current && saved_esp - 4u * (uint32_t)(argc + 2) < t_current->stack_lo + GUEST_STACK_GUARD)
        lx_abort(LX_FATAL_ABI, va, "guest stack overflow");
    for (int i = argc - 1; i >= 0; i--) push32(args[i]);
    push32(RECOMP_RETADDR);
    g_ecx = convention == LX_THISCALL ? this_ecx : g_ecx;
    after_call = stdcall_like ? saved_esp : saved_esp - 4u * (uint32_t)argc;
    if (fn) {
        fn();
    } else {
        t_import_va = va;
        import_trampoline();
    }
    if (g_esp != after_call) {
        char what[96];
        snprintf(what, sizeof what, "stack imbalance after 0x%08X: esp 0x%08X, expected 0x%08X", va, g_esp, after_call);
        lx_abort(LX_FATAL_ABI, va, what);
    }
    result = g_eax;
    result_high = g_edx;
    g_esp = saved_esp;
    g_ebx = saved_ebx, g_esi = saved_esi, g_edi = saved_edi, g_ebp = saved_ebp;
    g_flag_k = saved_k, g_flag_a = saved_a, g_flag_b = saved_b, g_flag_cf = saved_cf, g_cur_func = saved_func;
    if (edx_out) *edx_out = result_high;
    return result;
}

lx_status_t lx_run(void (*body)(void*), void* arg) {
    run_frame_t frame;
    lx_thread_t* t = t_current;
    if (!t) return LX_FATAL_ABI;
    frame.prev = t->frame;
    t->frame = &frame;
    memset(&t->diag, 0, sizeof t->diag);
    if (setjmp(frame.jump) == 0) {
        body(arg);
        t->frame = frame.prev;
        return LX_OK;
    }
    t->frame = frame.prev;
    return t->diag.status;
}
