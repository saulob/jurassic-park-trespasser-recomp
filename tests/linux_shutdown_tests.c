/* One way out: the state machine, the signals, the guest threads' end, the SDL window's close routes (SDL's dummy video driver), and
 * the installed game led out of the menu and out of the level by each request. Nothing opens a window or plays a sound. The game's part
 * needs its guest image; without it that part says DATA UNAVAILABLE. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "linux_boot.h"
#include "linux_crash.h"
#include "linux_ddraw.h"
#include "linux_shutdown.h"
#include "linux_win32_harness.h"

enum { MENU_FRAMES = 300, NEW_GAME_X = 310, NEW_GAME_Y = 105, LEVEL_FRAME = 700, STORM_THREADS = 4, STORM_CYCLES = 1000 };

static void test_state_machine(void) {
    lx_shutdown_reset();
    CHECK(lx_shutdown_state() == LX_RUNNING && lx_shutdown_reason() == LX_QUIT_NONE, "a run starts RUNNING with nothing requested");
    CHECK(lx_shutdown_request(LX_QUIT_WINDOW) == 1 && lx_shutdown_state() == LX_QUIT_REQUESTED && lx_shutdown_reason() == LX_QUIT_WINDOW,
          "the first request moves to QUIT_REQUESTED and keeps its reason");
    CHECK(lx_shutdown_request(LX_QUIT_SIGINT) == 0 && lx_shutdown_reason() == LX_QUIT_WINDOW && lx_shutdown_repeats() == 1,
          "a second request is counted and changes nothing");
    CHECK(lx_shutdown_begin() == 1 && lx_shutdown_state() == LX_SHUTTING_DOWN, "cleanup starts once: SHUTTING_DOWN");
    CHECK(lx_shutdown_begin() == 0 && lx_shutdown_request(LX_QUIT_SIGTERM) == 0 && lx_shutdown_state() == LX_SHUTTING_DOWN,
          "later begins and requests do not restart it");
    lx_shutdown_stopped();
    CHECK(lx_shutdown_state() == LX_STOPPED && lx_shutdown_begin() == 0, "STOPPED stays stopped");
    lx_shutdown_new_run();
    CHECK(lx_shutdown_state() == LX_RUNNING, "the next run starts clean");
    CHECK(lx_shutdown_begin() == 1, "a run that ends without a request still goes through SHUTTING_DOWN once");
    lx_shutdown_reset();
}

typedef struct {
    pthread_barrier_t* barrier;
    int first_wins;
    lx_quit_reason_t reason;
} requester_t;

static void* requester(void* arg) {
    requester_t* r = (requester_t*)arg;
    pthread_barrier_wait(r->barrier);
    r->first_wins = lx_shutdown_request(r->reason);
    return NULL;
}

static void test_simultaneous_requests(void) {
    unsigned bad_winner = 0, bad_state = 0, bad_begin = 0;
    for (int cycle = 0; cycle < STORM_CYCLES; cycle++) {
        pthread_barrier_t barrier;
        pthread_t threads[STORM_THREADS];
        requester_t r[STORM_THREADS];
        int winners = 0, begins = 0;
        lx_shutdown_reset();
        pthread_barrier_init(&barrier, NULL, STORM_THREADS);
        for (int i = 0; i < STORM_THREADS; i++) {
            r[i].barrier = &barrier, r[i].first_wins = 0, r[i].reason = (lx_quit_reason_t)(LX_QUIT_WINDOW + i);
            pthread_create(&threads[i], NULL, requester, &r[i]);
        }
        for (int i = 0; i < STORM_THREADS; i++) {
            pthread_join(threads[i], NULL);
            winners += r[i].first_wins;
        }
        pthread_barrier_destroy(&barrier);
        bad_winner += winners != 1;
        bad_state += lx_shutdown_state() != LX_QUIT_REQUESTED || lx_shutdown_repeats() != STORM_THREADS - 1;
        begins += lx_shutdown_begin();
        begins += lx_shutdown_begin();
        bad_begin += begins != 1;
        lx_shutdown_stopped();
    }
    CHECK(bad_winner == 0, "%d rounds of %d simultaneous requests: exactly one is first every time (%u wrong)", STORM_CYCLES, STORM_THREADS, bad_winner);
    CHECK(bad_state == 0 && bad_begin == 0, "and the state, the repeat count and the single cleanup start hold (%u, %u wrong)", bad_state, bad_begin);
    lx_shutdown_reset();
}

static int disposition_is_siginfo(int signo) {
    struct sigaction sa;
    sigaction(signo, NULL, &sa);
    return (sa.sa_flags & SA_SIGINFO) != 0;
}

static void test_signals(void) {
    lx_shutdown_reset();
    CHECK(lx_shutdown_install_signals() == 1, "the signal handlers install");
    lx_crash_install();
    CHECK(!disposition_is_siginfo(SIGINT) && !disposition_is_siginfo(SIGTERM) && disposition_is_siginfo(SIGSEGV),
          "the crash handler holds the fatal signals and not SIGINT or SIGTERM");
    raise(SIGINT);
    CHECK(lx_shutdown_state() == LX_QUIT_REQUESTED && lx_shutdown_reason() == LX_QUIT_SIGINT, "SIGINT becomes a shutdown request");
    CHECK(!lx_shutdown_force_due(), "one signal waits for the game's own way out");
    lx_shutdown_reset();
    raise(SIGTERM);
    CHECK(lx_shutdown_state() == LX_QUIT_REQUESTED && lx_shutdown_reason() == LX_QUIT_SIGTERM, "SIGTERM becomes the same request");
    raise(SIGINT);
    CHECK(lx_shutdown_force_due() && lx_shutdown_reason() == LX_QUIT_SIGTERM, "a second signal asks for the host to end the guest, and the first reason stays");
    CHECK(lx_shutdown_exit_code() == 0, "a requested exit is code 0");
    lx_shutdown_restore_signals();
    lx_shutdown_reset();
}

static uint32_t s_wait_event;

static uint32_t make_workers(int running, int suspended) {
    uint32_t created = 0;
    for (int i = 0; i < running + suspended; i++) {
        uint32_t thread = api("kernel32.dll", "CreateThread", 6, 0u, 0u, (uint32_t)FAKE_WAITER, s_wait_event, i < running ? 0u : 4u, 0u);
        created += thread != 0;
    }
    return created;
}

static void test_guest_threads(void) {
    unsigned not_joined = 0, slow = 0;
    s_wait_event = api("kernel32.dll", "CreateEventA", 4, 0u, 1u, 0u, 0u);
    CHECK(s_wait_event != 0, "an event the workers wait on forever");
    for (int cycle = 0; cycle < 20; cycle++) {
        lx_blocking_t b;
        struct timespec t0, t1;
        int joined;
        CHECK(make_workers(6, 3) == 9, "nine guest threads, three of them suspended");
        lx_blocking_begin(&b);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        joined = hle_join_guest_threads(3000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        lx_blocking_end(&b);
        not_joined += !joined;
        slow += (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 400;
    }
    CHECK(not_joined == 0, "20 rounds: every guest thread, waiting forever or suspended, ends and is joined (%u rounds failed)", not_joined);
    CHECK(slow == 0, "each round ends within a wait slice or two (%u slow)", slow);
}

/* SDL's poll sentinel hides an event pushed right after a poll until the next one. */
static int poll_event(lx_window_backend_t* b, lx_event_t* ev) {
    for (int i = 0; i < 3; i++)
        if (b->next_event(b, 0, ev) && ev->kind != LX_EV_NONE) return 1;
    return 0;
}

static void test_sdl_routes(void) {
    lx_window_backend_t* b = lx_sdl_backend();
    lx_window_desc_t d = { "shutdown", 320, 240, 0, 0 };
    lx_event_t ev;
    SDL_Event e;
    if (!b->open(b, &d)) {
        printf("  SDL video is not available here: the SDL routes were not checked\n");
        return;
    }
    memset(&ev, 0, sizeof ev);
    while (b->next_event(b, 0, &ev) && ev.kind != LX_EV_NONE) memset(&ev, 0, sizeof ev);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    SDL_PushEvent(&e);
    memset(&ev, 0, sizeof ev);
    CHECK(poll_event(b, &ev) && ev.kind == LX_EV_CLOSE && ev.vk == 0, "the window manager's close request is a close event");
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN, e.key.scancode = SDL_SCANCODE_F4, e.key.mod = SDL_KMOD_LALT, e.key.down = true;
    CHECK(SDL_PushEvent(&e), "the key event is queued: %s", SDL_GetError());
    CHECK(poll_event(b, &ev) && ev.kind == LX_EV_CLOSE && ev.vk == 1, "Alt+F4 as a key is the same close event (kind %d)", (int)ev.kind);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN, e.key.scancode = SDL_SCANCODE_F4, e.key.down = true;
    SDL_PushEvent(&e);
    CHECK(poll_event(b, &ev) && ev.kind == LX_EV_KEY, "F4 without Alt stays a key");
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
    CHECK(poll_event(b, &ev) && ev.kind == LX_EV_CLOSE, "SDL's own quit event is a close event");
    b->destroy(b);
    b->destroy(b);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0, "destroying the backend twice quits video once");
}

typedef enum { VIA_FOCUS_LOSS_AND_GAIN, VIA_CLOSE_EVENT, VIA_ALT_F4_EVENT, VIA_SIGINT, VIA_SIGTERM, VIA_BOTH, VIA_TWO_SIGNALS } route_t;

typedef struct {
    unsigned request_at;
    route_t route;
    int click_new_game;
} script_t;

static void queue(lx_event_kind_t kind, uint32_t vk, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.vk = vk, ev.down = down, ev.x = NEW_GAME_X, ev.y = NEW_GAME_Y;
    lx_virtual_queue_event(&ev);
}

static void hook(unsigned frame, void* user) {
    const script_t* s = (const script_t*)user;
    if (s->click_new_game) {
        if (frame == MENU_FRAMES) queue(LX_EV_MOUSE_MOVE, 0, 0);
        if (frame == MENU_FRAMES + 2) queue(LX_EV_MOUSE_BUTTON, 1, 1);
        if (frame == MENU_FRAMES + 4) queue(LX_EV_MOUSE_BUTTON, 1, 0);
    }
    if (frame != s->request_at) return;
    switch (s->route) {
    case VIA_FOCUS_LOSS_AND_GAIN:
        queue(LX_EV_FOCUS, 0, 0);
        queue(LX_EV_FOCUS, 0, 1);
        break;
    case VIA_CLOSE_EVENT: queue(LX_EV_CLOSE, 0, 0); break;
    case VIA_ALT_F4_EVENT: queue(LX_EV_CLOSE, 1, 0); break;
    case VIA_SIGINT: raise(SIGINT); break;
    case VIA_SIGTERM: raise(SIGTERM); break;
    case VIA_BOTH:
        queue(LX_EV_CLOSE, 0, 0);
        queue(LX_EV_CLOSE, 1, 0);
        raise(SIGINT);
        break;
    case VIA_TWO_SIGNALS:
        raise(SIGTERM);
        raise(SIGINT);
        break;
    }
}

static lx_boot_status_t run(const char* game_dir, script_t* s, lx_boot_result_t* r) {
    lx_boot_options_t o;
    lx_boot_options_init(&o, game_dir, "en-116", "build/linux_shutdown_tests_user", NULL, NULL, 0);
    o.frame_limit = s->request_at + 2500;
    o.frame_hook = hook;
    o.hook_user = s;
    return lx_boot_headless(&o, r);
}

/* Alt-Tabbing to the terminal for Ctrl+C and back made the game restore and set its display again: that is its reaction to losing and
 * regaining the window's focus, not to the signal. */
static void test_focus_resets(const char* game) {
    lx_boot_result_t r;
    script_t level = { LEVEL_FRAME, VIA_FOCUS_LOSS_AND_GAIN, 1 }, quiet = { LEVEL_FRAME, VIA_FOCUS_LOSS_AND_GAIN, 1 };
    lx_boot_options_t o;
    unsigned with_focus, without_focus;
    lx_shutdown_reset();
    lx_boot_options_init(&o, game, "en-116", "build/linux_shutdown_tests_user", NULL, NULL, 0);
    o.frame_limit = LEVEL_FRAME + 200;
    o.frame_hook = hook;
    o.hook_user = &level;
    if (lx_boot_headless(&o, &r) == LX_BOOT_DATA_UNAVAILABLE) return;
    with_focus = lx_ddraw_restore_count();
    quiet.request_at = LEVEL_FRAME + 1000;
    o.hook_user = &quiet;
    lx_boot_headless(&o, &r);
    without_focus = lx_ddraw_restore_count();
    CHECK(with_focus > without_focus, "losing and regaining focus in the level restores the display again (%u restores against %u without)", with_focus, without_focus);
}

static int test_game(const char* game) {
    lx_boot_result_t r;
    const lx_virtual_state_t* v = lx_virtual_state();
    static const struct {
        const char* name;
        script_t script;
        lx_quit_reason_t reason;
        int guest_leads;
    } cases[] = {
        { "the window's close button at the menu", { 150, VIA_CLOSE_EVENT, 0 }, LX_QUIT_WINDOW, 1 },
        { "Alt+F4 at the menu", { 150, VIA_ALT_F4_EVENT, 0 }, LX_QUIT_ALT_F4, 1 },
        { "SIGINT at the menu", { 150, VIA_SIGINT, 0 }, LX_QUIT_SIGINT, 1 },
        { "SIGTERM at the menu", { 150, VIA_SIGTERM, 0 }, LX_QUIT_SIGTERM, 1 },
        { "a close, Alt+F4 and SIGINT at once at the menu", { 150, VIA_BOTH, 0 }, LX_QUIT_SIGINT, 1 },
        { "the close button in the level", { LEVEL_FRAME, VIA_CLOSE_EVENT, 1 }, LX_QUIT_WINDOW, 1 },
        { "SIGINT in the level", { LEVEL_FRAME, VIA_SIGINT, 1 }, LX_QUIT_SIGINT, 1 },
        { "two signals in the level (the host ends the guest)", { LEVEL_FRAME, VIA_TWO_SIGNALS, 1 }, LX_QUIT_SIGTERM, 0 },
    };
    lx_shutdown_install_signals();
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        script_t script = cases[i].script;
        lx_boot_status_t st;
        lx_shutdown_reset();
        st = run(game, &script, &r);
        if (st == LX_BOOT_DATA_UNAVAILABLE) {
            printf("  the game's guest image is not installed: the game's part was skipped (DATA UNAVAILABLE)\n");
            lx_shutdown_restore_signals();
            return 0;
        }
        printf("  %s: %s, reason %s, %s, %u windows ended by the host, %u frames\n", cases[i].name, lx_boot_status_name(st),
               lx_shutdown_reason_name((lx_quit_reason_t)r.quit_reason), r.host_ended_guest ? "the host ended the guest" : "the game left by itself",
               r.windows_ended_by_host, v->presents);
        CHECK(st == LX_BOOT_EXITED && lx_boot_process_exit_code(&r) == 0, "%s ends the guest and the process exit code is 0: %s (%s)", cases[i].name, lx_boot_status_name(st), r.detail);
        CHECK(r.quit_reason == (int)cases[i].reason, "%s: the reason is the first request's", cases[i].name);
        CHECK(cases[i].guest_leads ? r.host_ended_guest == 0 : r.host_ended_guest == 1, "%s: %s", cases[i].name,
              cases[i].guest_leads ? "the game left through its own way out" : "the host ended the guest");
        CHECK(r.cleanup_complete && lx_shutdown_state() == LX_STOPPED && v->opened == 0, "%s: guest threads joined, state STOPPED, window closed", cases[i].name);
    }
    lx_shutdown_restore_signals();
    return 1;
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    const char* game = argc > 1 ? argv[1] : env && *env ? env : "game";
    if (!harness_start()) return 2;
    g_fake_wait_thunk = hle_resolve("kernel32.dll", "WaitForSingleObject", 0, NULL);
    test_state_machine();
    test_simultaneous_requests();
    test_signals();
    test_guest_threads();
    test_sdl_routes();
    harness_stop();
    test_focus_resets(game);
    test_game(game);
    printf("linux_shutdown_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
