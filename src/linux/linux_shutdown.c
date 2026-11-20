#include "linux_shutdown.h"

#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "linux_log.h"
#include "linux_spine.h"
#include "linux_user32.h"

#define WM_CLOSE_MESSAGE 0x10u
#define EMERGENCY_SIGNALS 3u

static int s_state, s_reason;
static unsigned s_requests, s_signals, s_forced, s_close_posted;
static uint64_t s_request_ms;
static __thread int t_guest_main;
static struct sigaction s_old_int, s_old_term;
static int s_signals_installed;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void lx_shutdown_reset(void) {
    __atomic_store_n(&s_state, LX_RUNNING, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_reason, LX_QUIT_NONE, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_requests, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_signals, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_close_posted, 0, __ATOMIC_SEQ_CST);
    s_forced = 0;
}

void lx_shutdown_new_run(void) {
    if (lx_shutdown_state() >= LX_SHUTTING_DOWN) lx_shutdown_reset();
}

int lx_shutdown_request(lx_quit_reason_t reason) {
    int running = LX_RUNNING;
    if (__atomic_fetch_add(&s_requests, 1, __ATOMIC_SEQ_CST) != 0) return 0;
    __atomic_store_n(&s_reason, (int)reason, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_request_ms, now_ms(), __ATOMIC_SEQ_CST);
    __atomic_compare_exchange_n(&s_state, &running, LX_QUIT_REQUESTED, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return 1;
}

lx_shutdown_state_t lx_shutdown_state(void) { return (lx_shutdown_state_t)__atomic_load_n(&s_state, __ATOMIC_SEQ_CST); }
lx_quit_reason_t lx_shutdown_reason(void) { return (lx_quit_reason_t)__atomic_load_n(&s_reason, __ATOMIC_SEQ_CST); }
unsigned lx_shutdown_repeats(void) {
    unsigned n = __atomic_load_n(&s_requests, __ATOMIC_SEQ_CST);
    return n ? n - 1 : 0;
}
unsigned lx_shutdown_forced_ends(void) { return __atomic_load_n(&s_forced, __ATOMIC_SEQ_CST); }

const char* lx_shutdown_reason_name(lx_quit_reason_t reason) {
    switch (reason) {
    case LX_QUIT_WINDOW: return "the window's close button";
    case LX_QUIT_ALT_F4: return "Alt+F4";
    case LX_QUIT_SIGINT: return "SIGINT";
    case LX_QUIT_SIGTERM: return "SIGTERM";
    case LX_QUIT_GUEST: return "the game";
    default: return "nothing";
    }
}

int lx_shutdown_begin(void) {
    int state = __atomic_load_n(&s_state, __ATOMIC_SEQ_CST);
    while (state == LX_RUNNING || state == LX_QUIT_REQUESTED)
        if (__atomic_compare_exchange_n(&s_state, &state, LX_SHUTTING_DOWN, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
    return 0;
}

void lx_shutdown_stopped(void) { __atomic_store_n(&s_state, LX_STOPPED, __ATOMIC_SEQ_CST); }

static void on_signal(int signo) {
    unsigned n = __atomic_add_fetch(&s_signals, 1, __ATOMIC_SEQ_CST);
    lx_shutdown_request(signo == SIGINT ? LX_QUIT_SIGINT : LX_QUIT_SIGTERM);
    if (n >= EMERGENCY_SIGNALS) _exit(128 + signo);
}

int lx_shutdown_install_signals(void) {
    struct sigaction sa;
    if (s_signals_installed) return 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, &s_old_int) != 0 || sigaction(SIGTERM, &sa, &s_old_term) != 0) return 0;
    s_signals_installed = 1;
    return 1;
}

void lx_shutdown_restore_signals(void) {
    if (!s_signals_installed) return;
    sigaction(SIGINT, &s_old_int, NULL);
    sigaction(SIGTERM, &s_old_term, NULL);
    s_signals_installed = 0;
}

void lx_shutdown_mark_guest_main(void) { t_guest_main = 1; }

int lx_shutdown_force_due(void) {
    return lx_shutdown_state() == LX_QUIT_REQUESTED &&
           (__atomic_load_n(&s_signals, __ATOMIC_SEQ_CST) >= 2 || now_ms() - __atomic_load_n(&s_request_ms, __ATOMIC_SEQ_CST) > LX_SHUTDOWN_GRACE_MS);
}

void lx_shutdown_poll(void) {
    if (!t_guest_main || lx_shutdown_state() != LX_QUIT_REQUESTED) return;
    if (!__atomic_exchange_n(&s_close_posted, 1, __ATOMIC_SEQ_CST)) {
        lx_log("quit", "requested by %s: WM_CLOSE to the game, then its own way out", lx_shutdown_reason_name(lx_shutdown_reason()));
        if (lx_user32_main_window()) lx_user32_post(lx_user32_main_window(), WM_CLOSE_MESSAGE, 0, 0);
    }
    if (lx_shutdown_force_due()) {
        __atomic_add_fetch(&s_forced, 1, __ATOMIC_SEQ_CST);
        lx_log("quit", "the game did not leave by itself (%s): the guest is ended from the host",
               __atomic_load_n(&s_signals, __ATOMIC_SEQ_CST) >= 2 ? "second signal" : "grace period over");
        lx_abort(LX_EXIT, 0, "quit requested");
    }
}

int lx_shutdown_exit_code(void) { return 0; }
