/*
 * How the guest's ending becomes the host's exit code, and the last phases of a shutdown. The CRT
 * startup passes WinMain's return value to exit(), and WinMain returns 1 when the game ran and 0
 * when start-up did not complete. That 1 is the game's success, so it is the only guest code the
 * host turns into 0; exit(1) called from anywhere else (the STL's length_error handler, for one)
 * stays a failure.
 */
#include "host.h"
#include "gamepad.h"

#include <stdio.h>
#include <string.h>

#define MAX_GUEST_THREADS 64
#define THREAD_LOG_LIMIT 16
/* As pcrecomp's built-in CreateThread: lifted frames need far more stack than the original asked for. */
#define GUEST_THREAD_STACK (16u << 20)

typedef void(__cdecl* crt_cexit_t)(void);

static struct {
    HANDLE handle;
    DWORD id;
    uint32_t entry, creator;
} s_threads[MAX_GUEST_THREADS];
static LONG s_thread_count;

static const char* quit_route(void) {
    const char* origin = window_host_close_origin();
    return origin ? origin : "the game's own menu";
}

int host_exit_code_for_guest_return(int code) {
    if (code == 1) {
        host_log("exit", "WinMain returned 1, the game's success value: normal exit (quit through %s)", quit_route());
        return 0;
    }
    if (code == 0) {
        host_log("exit", "WinMain returned 0: the game did not finish starting up, or start-up was cancelled "
                 "(see the lines above)");
        return HOST_EXIT_GAME_FAILED;
    }
    host_log("exit", "WinMain returned %d, which the game never uses", code);
    return HOST_EXIT_GUEST_ERROR;
}

void shim_CreateThread(void) {
    uint32_t* a = (uint32_t*)(uintptr_t)(g_esp + 4);
    SIZE_T size = a[1] > GUEST_THREAD_STACK ? a[1] : GUEST_THREAD_STACK;
    HANDLE h = CreateThread((LPSECURITY_ATTRIBUTES)(uintptr_t)a[0], size, (LPTHREAD_START_ROUTINE)(uintptr_t)a[2],
                            (LPVOID)(uintptr_t)a[3], a[4] | STACK_SIZE_PARAM_IS_A_RESERVATION, (LPDWORD)(uintptr_t)a[5]);
    LONG n = s_thread_count;
    if (h && n < MAX_GUEST_THREADS &&
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &s_threads[n].handle, SYNCHRONIZE, FALSE, 0)) {
        s_threads[n].id = GetThreadId(h);
        s_threads[n].entry = a[2];
        s_threads[n].creator = g_cur_func;
        s_thread_count = n + 1;
        guest_profile_add_worker(h, a[2]);
        if (n < THREAD_LOG_LIMIT)
            host_log("thread", "guest thread %lu created by sub_%08X, entry sub_%08X", GetThreadId(h), g_cur_func, a[2]);
    }
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 6 * 4;
}

static int worker_running(LONG i) { return WaitForSingleObject(s_threads[i].handle, 0) == WAIT_TIMEOUT; }

unsigned guest_threads_alive(void) {
    unsigned alive = 1;
    for (LONG i = 0; i < s_thread_count; i++)
        if (worker_running(i)) alive++;
    return alive;
}

void guest_threads_log(const char* when) {
    unsigned alive = guest_threads_alive();
    host_log("shutdown", "guest threads %s: %u (the main thread and %u worker%s)", when, alive, alive - 1,
             alive == 2 ? "" : "s");
    for (LONG i = 0; i < s_thread_count; i++)
        if (worker_running(i))
            host_log("shutdown", "  worker %lu: entry sub_%08X, created by sub_%08X", s_threads[i].id, s_threads[i].entry,
                     s_threads[i].creator);
}

static void finish_logging(void) {
    window_host_release_cursor();
    window_host_input_summary();
    gamepad_summary();
    d3dr_summary();
    guest_profile_summary();
    io_trace_summary();
}

/* Runs on the guest thread inside a shim, so it holds the machine lock: no lifted code can run on
 * any thread from here on, and nothing here returns to the guest. */
static __declspec(noreturn) void host_finish(int host_code) {
    guest_threads_log("after quit");
    window_host_begin_cleanup();
    host_log("shutdown", "no lifted code runs after this point: the exiting thread keeps the machine lock until the "
             "process ends, so a remaining worker cannot re-enter the game");
    finish_logging();
    io_trace_release_swap_files();
    present_release();
    window_host_exited();
    diagnostics_process_ending();
    host_log("exit", "exit code %d", host_code);
    ExitProcess((UINT)host_code);
}

/* The CRT's own end without its ExitProcess. exit() runs the game's atexit handlers (static
 * destructors) here, on its thread, as its menu Exit does; _exit and ExitProcess skip them
 * (_c_exit). Either way msvcrt is marked terminated, so its unload runs no game code. */
static void end_guest_crt(int run_atexit) {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    crt_cexit_t end = crt ? (crt_cexit_t)GetProcAddress(crt, run_atexit ? "_cexit" : "_c_exit") : NULL;
    if (run_atexit) host_log("shutdown", "running the game's atexit handlers (msvcrt _cexit)");
    if (end) end();
    if (run_atexit) host_log("shutdown", "the game's atexit handlers finished");
}

static void guest_exit(int code, const char* api) {
    int host_code;
    if (g_cur_func == lift_entry_va) {
        host_code = host_exit_code_for_guest_return(code);
    } else {
        host_log("exit", "%s(%d) called by the game from sub_%08X: an error exit, not WinMain's return", api, code,
                 g_cur_func);
        host_code = HOST_EXIT_GUEST_ERROR;
    }
    window_host_crt_exiting();
    end_guest_crt(!strcmp(api, "exit"));
    window_host_guest_stopped();
    host_finish(host_code);
}

void shim_exit(void) { guest_exit((int)GUEST_ARG(0), "exit"); }

void shim__exit(void) { guest_exit((int)GUEST_ARG(0), "_exit"); }

void shim_ExitProcess(void) {
    UINT code = GUEST_ARG(0);
    host_log("exit", "ExitProcess(%u) called by the game from sub_%08X", code, g_cur_func);
    window_host_crt_exiting();
    end_guest_crt(0);
    window_host_guest_stopped();
    host_finish((int)code);
}
