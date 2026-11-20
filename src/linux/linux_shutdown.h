/* One way out for the Linux host. Every source of "the user wants to quit" (the window's close button, Alt+F4, SIGINT, SIGTERM) only
 * asks, with an atomic store that is safe in a signal handler; the guest's main thread notices the request at its next import call or
 * wait slice and leads the game out through the game's own quit path (the game ignores WM_CLOSE), and the host cleans up once, after the
 * guest has stopped. If the game does not leave in time, or a second request comes, the guest is ended from the host. */
#ifndef JPT_LINUX_SHUTDOWN_H
#define JPT_LINUX_SHUTDOWN_H

#include <stdint.h>

typedef enum { LX_RUNNING, LX_QUIT_REQUESTED, LX_SHUTTING_DOWN, LX_STOPPED } lx_shutdown_state_t;

typedef enum { LX_QUIT_NONE, LX_QUIT_WINDOW, LX_QUIT_ALT_F4, LX_QUIT_SIGINT, LX_QUIT_SIGTERM, LX_QUIT_GUEST } lx_quit_reason_t;

/* Back to RUNNING with nothing requested. */
void lx_shutdown_reset(void);
/* A new run starts: reset if the last one finished; a request made before the guest started stays. */
void lx_shutdown_new_run(void);
/* Asks for the run to end; async-signal-safe. 1 for the first request, 0 for any later one (which only counts). */
int lx_shutdown_request(lx_quit_reason_t reason);
lx_shutdown_state_t lx_shutdown_state(void);
lx_quit_reason_t lx_shutdown_reason(void);
const char* lx_shutdown_reason_name(lx_quit_reason_t reason);
/* Requests after the first. */
unsigned lx_shutdown_repeats(void);

/* The guest has stopped and cleanup starts: 1 the first time only. */
int lx_shutdown_begin(void);
void lx_shutdown_stopped(void);

/* SIGINT and SIGTERM both become lx_shutdown_request (and nothing else); the third signal of a stuck run ends the process. */
int lx_shutdown_install_signals(void);
void lx_shutdown_restore_signals(void);

/* The calling host thread is the guest's main thread: the only one lx_shutdown_poll acts on. */
void lx_shutdown_mark_guest_main(void);
/* From the guest's main thread, at the points it reaches often: delivers WM_CLOSE once, and ends the guest from the host when the game
 * has not left within the grace period or a second request came. Returns normally unless it ends the guest. */
void lx_shutdown_poll(void);
/* 1 when poll would end the guest now: lets a wait stop early so the thread can take the machine and poll. */
int lx_shutdown_force_due(void);
/* Times the host ended the guest because the game did not leave by itself. */
unsigned lx_shutdown_forced_ends(void);

/* The exit code of a run the user ended: 0 (SIGTERM too: the user asked for it, the exit is orderly). */
int lx_shutdown_exit_code(void);

#define LX_SHUTDOWN_GRACE_MS 5000u

#endif
