/* The call spine: how lifted guest code and the host call each other on Linux x86_64.
 *
 * Lifted code keeps the guest's registers in the g_* globals (loaded into locals on entry, written back at every call and
 * return), pushes a dummy return address on the guest stack and calls the C function of its target. A target in the thunk
 * range is a host function: the spine reads its arguments from the guest stack, runs it, sets eax (edx) and pops the
 * stack the way the original calling convention does. Host code calls guest functions through lx_call_guest, which
 * builds the same frame. Nothing here executes guest bytes. */
#ifndef JPT_LINUX_SPINE_H
#define JPT_LINUX_SPINE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_thunks.h"

typedef void (*lx_guest_fn)(void);

typedef struct {
    uint32_t va;
    lx_guest_fn fn;
} lx_entry_t;

typedef struct {
    const lx_entry_t* entries; /* sorted by va */
    size_t count;
} lx_table_t;

typedef enum {
    LX_OK = 0,
    LX_EXIT,            /* the guest ended the process (ExitProcess, exit) */
    LX_MISSING_IMPORT,  /* a guest call reached an import with no implementation */
    LX_FATAL_ABI,       /* stack imbalance, stack overflow, unreadable thunk arguments */
    LX_BAD_DISPATCH,    /* a call or jump to an address that is neither lifted code nor a thunk */
    LX_NOT_LIFTED,      /* control reached a function the lift left out */
    LX_STOP             /* the host ended the run on purpose (a frame was presented, the guest went idle): what says why */
} lx_status_t;

typedef struct {
    lx_status_t status;
    uint32_t va;      /* the target, or the exit code */
    uint32_t caller;  /* the lifted function that made the call (guest EIP of the last lifted entry) */
    char what[96];    /* module!function for imports */
} lx_diag_t;

typedef struct lx_thread lx_thread_t;

/* Installs the dispatch of the active lift and points the lifted code's memory base at the guest window. */
void lx_spine_init(lx_table_t main_table, lx_table_t selfmod_table);
lx_guest_fn lx_find_entry(uint32_t va);
/* Another module's lifted functions (a guest DLL), sorted by address; the lookup tries each table whose address range holds va. */
void lx_spine_add_table(lx_table_t table);

/* A guest thread: its own register file, x87 stack, guest stack and TIB. The machine lock (one guest thread runs at a
 * time, as in the original lifted runtime) is held between attach and detach and released around blocking host waits. */
lx_thread_t* lx_thread_new(uint32_t stack_bytes);
void lx_thread_free(lx_thread_t* t);
void lx_thread_attach(lx_thread_t* t);
void lx_thread_detach(void);
lx_thread_t* lx_thread_current(void);
uint32_t lx_thread_stack_top(const lx_thread_t* t);
uint32_t lx_thread_tib(const lx_thread_t* t);

/* Around a host call that blocks: lets another guest thread run, then takes the machine back with this thread's registers. */
typedef struct {
    lx_thread_t* thread;
} lx_blocking_t;
void lx_blocking_begin(lx_blocking_t* b);
void lx_blocking_end(lx_blocking_t* b);

enum { LX_STDCALL = HT_STDCALL, LX_CDECL = HT_CDECL, LX_THISCALL = HT_THISCALL };

/* Calls the guest function at va from host code. Arguments are guest values (pointers are guest addresses). stdcall and
 * thiscall callees pop their arguments; for cdecl the spine does. The guest's ebx, esi, edi, ebp, esp and flags are
 * restored on return, so a host function that calls back into the guest leaves the caller's frame intact. eax is returned,
 * edx stored in *edx_out when given. A stack that does not end where the convention puts it is LX_FATAL_ABI. */
uint32_t lx_call_guest(uint32_t va, int convention, int argc, const uint32_t* args, uint32_t this_ecx, uint32_t* edx_out);

/* Runs body on the current guest thread inside a recovery frame; a fatal condition anywhere below returns here with its
 * status (and lx_diag()). LX_OK when body returns. */
lx_status_t lx_run(void (*body)(void*), void* arg);
void lx_abort(lx_status_t status, uint32_t va, const char* what);
const lx_diag_t* lx_diag(void);
void lx_diag_reset(void);

uint32_t lx_current_import_va(void);
uint32_t lx_guest_eip(void);
/* Thunks the guest has called so far (each once). */
unsigned lx_reached_imports(const uint32_t** vas);

#endif
