/* The call spine against guest functions written the way the lifter writes them: stdcall, cdecl and thiscall frames, results
 * in eax, the stack where each convention leaves it, host functions that call back into the guest (nested to any depth),
 * register preservation, faults reported as results instead of crashes, and one register file per guest thread. Headless. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_sync.h"
#include "linux_fake_guest.h"
#include "linux_hle.h"
#include "linux_spine.h"
#include "recomp_types.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static lx_thread_t* s_thread;
static uint32_t s_nested_depth;

static uint64_t host_callback(const uint32_t* args, void* user) {
    uint32_t value = args[1];
    (void)user;
    return lx_call_guest(args[0], LX_STDCALL, 1, &value, 0, NULL) + 1;
}

static uint64_t host_add(const uint32_t* args, void* user) {
    (void)user;
    return args[0] + 100u;
}

static uint64_t host_mul_cdecl(const uint32_t* args, void* user) {
    (void)user;
    return (uint64_t)args[0] * args[1];
}

static uint64_t host_block(const uint32_t* args, void* user) {
    lx_blocking_t b;
    (void)args;
    (void)user;
    lx_blocking_begin(&b);
    jpt_sleep_ms(25);
    lx_blocking_end(&b);
    return 0;
}

static uint64_t host_reenter(const uint32_t* args, void* user) {
    uint32_t depth = args[0];
    (void)user;
    s_nested_depth++;
    if (depth == 0) return 0;
    {
        uint32_t next = depth - 1;
        return lx_call_guest(FAKE_RECURSE, LX_STDCALL, 1, &next, 0, NULL);
    }
}

static uint32_t guest_stack_pointer(void) { return g_esp; }

static void exit_handler(uint32_t code) {
    if (code == HLE_EXIT_MISSING) {
        const char* name = ht_name(lx_current_import_va());
        lx_abort(LX_MISSING_IMPORT, lx_current_import_va(), name ? name : "?");
    }
    lx_abort(LX_EXIT, code, "ExitProcess");
}

typedef struct {
    uint32_t va;
    int convention, argc;
    uint32_t args[8], ecx, result;
} call_t;

static void do_call(void* arg) {
    call_t* c = (call_t*)arg;
    c->result = lx_call_guest(c->va, c->convention, c->argc, c->args, c->ecx, NULL);
}

static lx_status_t run_call(call_t* c) { return lx_run(do_call, c); }

static void test_stdcall(void) {
    call_t c;
    uint32_t esp0 = guest_stack_pointer();
    memset(&c, 0, sizeof c);
    c.va = FAKE_STDCALL0;
    CHECK(run_call(&c) == LX_OK && c.result == 0x1234 && guest_stack_pointer() == esp0, "stdcall, no arguments: result and esp");
    c.va = FAKE_STDCALL1_PTR;
    c.argc = 1;
    c.args[0] = 0x00A00000u;
    CHECK(run_call(&c) == LX_OK && c.result == 0x00A00010u && guest_stack_pointer() == esp0, "stdcall, one argument: a guest pointer comes back");
    c.va = FAKE_STDCALL3;
    c.argc = 3;
    c.args[0] = 1;
    c.args[1] = 2;
    c.args[2] = 3;
    CHECK(run_call(&c) == LX_OK && c.result == 123 && guest_stack_pointer() == esp0,
          "stdcall, several arguments in push order (%u), the callee pops them", c.result);
}

static void test_cdecl(void) {
    call_t c;
    uint32_t esp0 = guest_stack_pointer();
    memset(&c, 0, sizeof c);
    c.va = FAKE_CDECL3;
    c.convention = LX_CDECL;
    c.argc = 3;
    c.args[0] = 10;
    c.args[1] = 20;
    c.args[2] = 30;
    CHECK(run_call(&c) == LX_OK && c.result == 60 && guest_stack_pointer() == esp0, "cdecl: the callee leaves the arguments, the spine pops them");
    c.va = FAKE_STDCALL3;
    c.convention = LX_CDECL;
    CHECK(run_call(&c) == LX_FATAL_ABI && strstr(lx_diag()->what, "stack imbalance"),
          "a stdcall function called as cdecl is a stack imbalance, not a silent shift (%s)", lx_diag()->what);
    g_esp = esp0;
    c.va = FAKE_CDECL3;
    c.convention = LX_STDCALL;
    CHECK(run_call(&c) == LX_FATAL_ABI, "and a cdecl function called as stdcall is too");
    g_esp = esp0;
}

static void test_thiscall(void) {
    call_t c;
    uint32_t object = gm_heap_alloc(16, 1), esp0 = guest_stack_pointer();
    gp_write_u32(object, 1000);
    memset(&c, 0, sizeof c);
    c.va = FAKE_THISCALL;
    c.convention = LX_THISCALL;
    c.argc = 2;
    c.args[0] = 20;
    c.args[1] = 3;
    c.ecx = object;
    CHECK(run_call(&c) == LX_OK && c.result == 1023 && guest_stack_pointer() == esp0,
          "thiscall: `this` arrives in ecx, the two stack arguments are popped by the callee (%u)", c.result);
    gm_heap_free(object);
}

static void test_register_preservation(void) {
    call_t c;
    memset(&c, 0, sizeof c);
    c.va = FAKE_CLOBBER;
    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
    CHECK(run_call(&c) == LX_OK && c.result == 7, "a guest function that clobbers the callee-saved registers");
    CHECK(g_ebx == 0x11111111u && g_esi == 0x22222222u && g_edi == 0x33333333u && g_ebp == 0x44444444u,
          "ebx, esi, edi and ebp are the host caller's again after the call");
}

static void test_nested_calls(void) {
    call_t c;
    uint32_t esp0 = guest_stack_pointer();
    memset(&c, 0, sizeof c);
    c.va = FAKE_OUTER;
    c.argc = 1;
    c.args[0] = 7;
    CHECK(run_call(&c) == LX_OK && c.result == 108 + 0x0B0B0B0Bu, "guest -> host -> guest -> host -> guest and back: 7 + 100, +1, + the caller's ebx (%u)", c.result);
    CHECK(guest_stack_pointer() == esp0, "the stack is balanced after the nesting");

    c.va = FAKE_CALLS_THUNK_CDECL;
    c.argc = 0;
    CHECK(run_call(&c) == LX_OK && c.result == 30 && guest_stack_pointer() == esp0, "a guest call to a cdecl host function: the guest pops the arguments");

    s_nested_depth = 0;
    {
        uint32_t depth_thunk = ht_register("test!reenter", host_reenter, 1, HT_STDCALL, NULL);
        c.va = depth_thunk;
        c.argc = 1;
        c.args[0] = 200;
        CHECK(run_call(&c) == LX_OK && c.result == 199 && s_nested_depth == 1 && guest_stack_pointer() == esp0,
              "a host function entered with a host call, calling guest recursion 200 deep");
    }
}

static void test_faults_are_results(void) {
    call_t c;
    uint32_t esp0 = guest_stack_pointer();
    memset(&c, 0, sizeof c);
    c.va = FAKE_FORGETS_TO_POP;
    c.argc = 1;
    c.args[0] = 5;
    CHECK(run_call(&c) == LX_FATAL_ABI && lx_diag()->va == FAKE_FORGETS_TO_POP, "a stdcall function that returns without popping: LX_FATAL_ABI naming it");
    g_esp = esp0;

    c.va = 0x00501234u;
    c.argc = 0;
    CHECK(run_call(&c) == LX_BAD_DISPATCH && lx_diag()->va == 0x00501234u, "a host call to an address with no lifted function: LX_BAD_DISPATCH");
    g_esp = esp0;

    c.va = FAKE_CALLS_UNMAPPED;
    c.argc = 1;
    c.args[0] = 0x00501234u;
    CHECK(run_call(&c) == LX_BAD_DISPATCH && lx_diag()->caller == FAKE_CALLS_UNMAPPED, "a guest ICALL to such an address: LX_BAD_DISPATCH, with the calling function");
    g_esp = esp0;
    c.args[0] = 0x7F00FFF0u;
    CHECK(run_call(&c) == LX_BAD_DISPATCH, "and to an unregistered thunk address");
    g_esp = esp0;

    {
        uint32_t missing = hle_resolve("USER32.dll", "DialogBoxParamA", 0, NULL);
        c.va = FAKE_CALLS_UNMAPPED;
        c.args[0] = missing;
        CHECK(run_call(&c) == LX_MISSING_IMPORT && !strcmp(lx_diag()->what, "user32.dll!DialogBoxParamA") &&
                  lx_diag()->caller == FAKE_CALLS_UNMAPPED,
              "an import with no implementation: LX_MISSING_IMPORT with module!function and the caller (%s)", lx_diag()->what);
        g_esp = esp0;
    }
    {
        uint32_t exit_thunk = hle_resolve("kernel32.dll", "ExitProcess", 0, NULL);
        c.va = exit_thunk;
        c.argc = 1;
        c.args[0] = 9;
        CHECK(run_call(&c) == LX_EXIT && lx_diag()->va == 9, "ExitProcess ends the run with its code");
        g_esp = esp0;
    }
    {
        uint32_t saved = g_esp;
        g_esp = lx_thread_stack_top(s_thread) - 2u * 1024u * 1024u + 0x20;
        c.va = FAKE_STDCALL0;
        c.argc = 0;
        CHECK(run_call(&c) == LX_FATAL_ABI && strstr(lx_diag()->what, "overflow"), "a guest stack that is used up: LX_FATAL_ABI (%s)", lx_diag()->what);
        g_esp = saved;
    }
}

typedef struct {
    uint32_t tag, result, stack_top, tib;
} worker_t;

static void worker_body(void* arg) {
    worker_t* w = (worker_t*)arg;
    uint32_t tag = w->tag;
    w->result = lx_call_guest(FAKE_BLOCKING, LX_STDCALL, 1, &tag, 0, NULL);
}

static uint32_t worker_main(void* arg) {
    worker_t* w = (worker_t*)arg;
    lx_thread_t* t = lx_thread_new(256 * 1024);
    lx_thread_attach(t);
    w->stack_top = lx_thread_stack_top(t);
    w->tib = lx_thread_tib(t);
    for (int i = 0; i < 4; i++) lx_run(worker_body, w);
    lx_thread_detach();
    lx_thread_free(t);
    return 0;
}

static void test_guest_threads(void) {
    worker_t a = { 0xAAAA0001u, 0, 0, 0 }, b = { 0xBBBB0002u, 0, 0, 0 };
    jpt_thread_t* ta;
    jpt_thread_t* tb;
    lx_thread_detach();
    ta = jpt_thread_create(worker_main, &a);
    tb = jpt_thread_create(worker_main, &b);
    jpt_wait(jpt_thread_waitable(ta), 20000);
    jpt_wait(jpt_thread_waitable(tb), 20000);
    jpt_thread_destroy(ta);
    jpt_thread_destroy(tb);
    CHECK(a.result == a.tag && b.result == b.tag,
          "two guest threads blocking in host waits keep their own registers (%08X, %08X)", a.result, b.result);
    CHECK(a.stack_top != b.stack_top && a.tib != b.tib, "each has its own guest stack and TIB");
    lx_thread_attach(s_thread);
}

int main(void) {
    lx_table_t table;
    static lx_entry_t entries[32];
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    for (unsigned i = 0; i < g_fake_table_count; i++) {
        entries[i].va = g_fake_table[i].va;
        entries[i].fn = g_fake_table[i].fn;
    }
    table.entries = entries;
    table.count = g_fake_table_count;
    lx_spine_init(table, (lx_table_t){ NULL, 0 });
    hle_init(NULL, 0x00400000u, "C:\\trespass.exe");
    hle_set_exit_handler(exit_handler);
    g_fake_callback_thunk = ht_register("test!callback", host_callback, 2, HT_STDCALL, NULL);
    g_fake_add_thunk = ht_register("test!add", host_add, 1, HT_STDCALL, NULL);
    g_fake_cdecl_thunk = ht_register("test!mul", host_mul_cdecl, 2, HT_CDECL, NULL);
    g_fake_block_thunk = ht_register("test!block", host_block, 0, HT_STDCALL, NULL);

    s_thread = lx_thread_new(2 * 1024 * 1024);
    lx_thread_attach(s_thread);
    test_stdcall();
    test_cdecl();
    test_thiscall();
    test_register_preservation();
    test_nested_calls();
    test_faults_are_results();
    test_guest_threads();
    lx_thread_detach();
    lx_thread_free(s_thread);
    hle_shutdown();
    gm_shutdown();
    printf("linux_call_spine_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
