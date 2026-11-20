#ifndef TESTS_FAULT_GUARD_H
#define TESTS_FAULT_GUARD_H

/* faults_on_read / faults_on_write: whether touching a byte raises an access violation (SEH on Windows, SIGSEGV and
 * SIGBUS on POSIX). */
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int faults_on_read(const volatile uint8_t* p) {
    __try {
        volatile uint8_t v = *p;
        (void)v;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
    return 0;
}

static int faults_on_write(volatile uint8_t* p) {
    __try {
        *p = 1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
    return 0;
}
#else
#include <setjmp.h>
#include <signal.h>

static sigjmp_buf s_fault_jump;

static void fault_handler(int sig) {
    (void)sig;
    siglongjmp(s_fault_jump, 1);
}

static int touch_faults(volatile uint8_t* p, int write) {
    struct sigaction sa, old_segv, old_bus;
    int faulted;
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    faulted = sigsetjmp(s_fault_jump, 1);
    if (!faulted) {
        if (write) *p = 1;
        else {
            volatile uint8_t v = *p;
            (void)v;
        }
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return faulted;
}

static int faults_on_read(const volatile uint8_t* p) { return touch_faults((volatile uint8_t*)p, 0); }
static int faults_on_write(volatile uint8_t* p) __attribute__((unused));
static int faults_on_write(volatile uint8_t* p) { return touch_faults(p, 1); }
#endif

#endif
