/* The fatal-signal report: what it names, and that a real fault in a forked child ends with the report on stderr and the signal's
 * default action (the core dump) still taken. Nothing is started that opens a window or a sound. */
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linux_crash.h"
#include "linux_win32_harness.h"

static int read_all(int fd, char* out, int cap) {
    int n = 0, r;
    while (n < cap - 1 && (r = (int)read(fd, out + n, (size_t)(cap - 1 - n))) > 0) n += r;
    out[n] = 0;
    return n;
}

static void test_report(void) {
    char text[8192];
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    lx_crash_present_stage(LX_PRESENT_RENDER_PRESENT);
    lx_crash_audio_stage(LX_AUDIO_BUFFER_LOCK);
    lx_crash_import(hle_resolve("kernel32.dll", "Sleep", 0, NULL));
    lx_crash_thread_started(0x1000, 0x00435580);
    lx_crash_report(fds[1], SIGSEGV, (void*)0x10, (void*)0x1234);
    close(fds[1]);
    read_all(fds[0], text, sizeof text);
    close(fds[0]);
    CHECK(strstr(text, "fatal signal 11"), "the report names the signal");
    CHECK(strstr(text, "fault address       0x10"), "the fault address");
    CHECK(strstr(text, "RENDER_PRESENT") && strstr(text, "BUFFER_LOCK"), "the last present and audio stages");
    CHECK(strstr(text, "guest thread       0x1000 (entry 0x435580)"), "the guest thread and its entry");
    CHECK(strstr(text, "Sleep"), "the last import");
    CHECK(strstr(text, "host thread"), "the host thread");
    lx_crash_thread_ended();
    lx_crash_present_stage(LX_PRESENT_NONE);
    lx_crash_audio_stage(LX_AUDIO_NONE);
}

static void test_real_fault(void) {
    int fds[2], status = 0;
    char text[8192];
    pid_t pid;
    CHECK(pipe(fds) == 0, "pipe");
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        void (*bad)(void) = (void (*)(void))(uintptr_t)0x10;
        dup2(fds[1], 2);
        close(fds[0]);
        lx_crash_install();
        lx_crash_present_stage(LX_PRESENT_FRAME_HOOK);
        bad();
        _exit(0);
    }
    close(fds[1]);
    read_all(fds[0], text, sizeof text);
    close(fds[0]);
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the child still dies of SIGSEGV after the report (default action restored)");
    CHECK(strstr(text, "fatal signal 11") && strstr(text, "fault address       0x10") && strstr(text, "host rip            0x10"),
          "a call through address 0x10 reports the fault address and the host rip");
    CHECK(strstr(text, "FRAME_HOOK"), "and the present stage the child was in");
    if (g_failures) printf("child output: [%s]\n", text);
}

int main(void) {
    if (!harness_start()) return 2;
    test_report();
    test_real_fault();
    harness_stop();
    printf("linux_crash_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
