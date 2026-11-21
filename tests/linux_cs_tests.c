/* Guest critical sections and the machine lock. A thread that owns a guest section may give the machine up (a blocking call, the
 * import yield) and needs it back to leave the section; a second thread that enters the section must therefore wait without
 * holding the machine. Held, that was a deadlock of three threads that showed up once in a loaded run. Headless. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jpt_sync.h"
#include "linux_win32_harness.h"

enum { SECTION = 0x2000, ROUNDS = 40 };

static uint32_t s_section, s_other_stack;
static volatile int s_owner_has_section, s_owner_done, s_rounds_done;

static uint32_t call_on(uint32_t stack, uint32_t thunk, int argc, const uint32_t* args) {
    uint32_t esp = stack + 0x4000 - 4 * (uint32_t)(argc + 1), eax = 0, edx = 0;
    gp_write_u32(esp, 0x00401000u);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    if (!ht_invoke(thunk, &esp, &eax, &edx)) return 0xBAD0BAD0u;
    return eax;
}

static uint32_t k32_thunk(const char* name) { return hle_resolve("KERNEL32.dll", name, 0, NULL); }

/* The owner: takes the section, gives the machine up for a while as a blocked or yielding thread does, and leaves the section. */
static uint32_t owner_main(void* arg) {
    lx_thread_t* t = lx_thread_new(1024 * 1024);
    (void)arg;
    lx_thread_attach(t);
    for (int i = 0; i < ROUNDS; i++) {
        lx_blocking_t b;
        uint32_t section = s_section;
        call_on(s_other_stack, k32_thunk("EnterCriticalSection"), 1, &section);
        s_owner_has_section = 1;
        lx_blocking_begin(&b);
        jpt_sleep_ms(3);
        lx_blocking_end(&b);
        call_on(s_other_stack, k32_thunk("LeaveCriticalSection"), 1, &section);
        s_owner_has_section = 0;
        s_rounds_done++;
    }
    s_owner_done = 1;
    lx_thread_detach();
    lx_thread_free(t);
    return 0;
}

static uint32_t watchdog_main(void* arg) {
    (void)arg;
    for (int i = 0; i < 200 && !s_owner_done; i++) jpt_sleep_ms(50);
    if (!s_owner_done) {
        printf("FAIL: deadlock: the owner finished %d of %d rounds in 10 s\n", s_rounds_done, ROUNDS);
        fflush(stdout);
        _exit(1);
    }
    return 0;
}

int main(void) {
    jpt_thread_t *owner, *watchdog;
    unsigned entered = 0;
    if (!harness_start()) return 2;
    s_section = g_scratch + SECTION;
    s_other_stack = gm_alloc_pages(0, 0x10000, "other stack");
    api("KERNEL32.dll", "InitializeCriticalSection", 1, s_section);
    watchdog = jpt_thread_create(watchdog_main, NULL);
    owner = jpt_thread_create(owner_main, NULL);

    while (!s_owner_done) {
        if (s_owner_has_section) {
            api("KERNEL32.dll", "EnterCriticalSection", 1, s_section);
            entered++;
            api("KERNEL32.dll", "LeaveCriticalSection", 1, s_section);
        }
        {
            lx_blocking_t idle;
            lx_blocking_begin(&idle);
            jpt_sleep_ms(1);
            lx_blocking_end(&idle);
        }
    }
    CHECK(s_rounds_done == ROUNDS, "the owner went round %d times (%d)", ROUNDS, s_rounds_done);
    CHECK(entered > 0, "the other thread entered the section while the owner held it and had left the machine (%u times)", entered);
    jpt_thread_destroy(owner);
    jpt_thread_destroy(watchdog);
    api("KERNEL32.dll", "DeleteCriticalSection", 1, s_section);
    harness_stop();
    printf("linux_cs_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
