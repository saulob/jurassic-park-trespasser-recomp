/* Two guest threads and SDL's dummy audio callback thread at once: each guest thread creates, fills, plays, duplicates and releases
 * DirectSound buffers through the guest's vtables, taking turns on the machine lock the way the real guest threads do, while the
 * mixer pulls from the same buffers on its own host thread. Nothing reaches a sound device. */
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "dsound_compat.h"
#include "linux_dsound.h"
#include "linux_win32_harness.h"

enum {
    DEV_CREATE_BUFFER = 3, DEV_DUPLICATE = 5, DEV_SETCOOP = 6,
    B_RELEASE = 2, B_GETPOS = 4, B_GETSTATUS = 9, B_LOCK = 11, B_PLAY = 12, B_SETVOLUME = 15, B_SETFREQ = 17, B_STOP = 18, B_UNLOCK = 19,
    ITERATIONS = 1200, BUFFER_BYTES = 8192,
    CAPS = 0x80u | 0x20u | 0x40u | 0x10000u
};

static uint32_t s_device;
static volatile unsigned s_errors[2];
static unsigned s_ops[2];

static void yield_machine(void) {
    lx_blocking_t b;
    struct timespec pause = { 0, 20000 };
    lx_blocking_begin(&b);
    nanosleep(&pause, NULL);
    lx_blocking_end(&b);
}

typedef struct {
    int id;
    uint32_t scratch;
} worker_t;

static uint32_t create_buffer(worker_t* w) {
    uint32_t desc = w->scratch, fmt = w->scratch + 0x40, out = w->scratch + 0x80;
    uint32_t d[5] = { 20, CAPS, BUFFER_BYTES, 0, fmt };
    uint8_t wf[18] = { 1, 0, 1, 0, 0x22, 0x56, 0, 0, 0x44, 0xAC, 0, 0, 2, 0, 16, 0, 0, 0 };
    gp_copy_out(desc, d, sizeof d);
    gp_copy_out(fmt, wf, sizeof wf);
    if (com(s_device, DEV_CREATE_BUFFER, 4, desc, out, 0u) != 0) return 0;
    return rd(out);
}

static void check(worker_t* w, uint32_t hr) {
    if (hr != 0) s_errors[w->id]++;
    s_ops[w->id]++;
    yield_machine();
}

static void run_iterations(worker_t* w) {
    uint32_t p1 = w->scratch + 0x100, s1 = w->scratch + 0x104, p2 = w->scratch + 0x108, s2 = w->scratch + 0x10C, pos = w->scratch + 0x110;
    for (int i = 0; i < ITERATIONS; i++) {
        uint32_t buffer = create_buffer(w), copy = 0;
        uint8_t* mem;
        if (!buffer) {
            s_errors[w->id]++;
            continue;
        }
        check(w, com(buffer, B_LOCK, 8, 0u, 0u, p1, s1, p2, s2, 2u));
        mem = (uint8_t*)gm_ptr_checked(rd(p1), rd(s1), 1);
        if (mem) memset(mem, i & 0xFF, rd(s1));
        else s_errors[w->id]++;
        check(w, com(buffer, B_UNLOCK, 5, rd(p1), rd(s1), rd(p2), rd(s2)));
        check(w, com(buffer, B_SETVOLUME, 2, (uint32_t)(-(int)(i % 3000))));
        check(w, com(buffer, B_SETFREQ, 2, 11025u + (uint32_t)(i % 4) * 5512u));
        check(w, com(buffer, B_PLAY, 4, 0u, 0u, 1u));
        check(w, com(buffer, B_GETPOS, 3, pos, 0u));
        check(w, com(buffer, B_GETSTATUS, 2, pos));
        if (i % 7 == 0) {
            uint32_t out = w->scratch + 0x120;
            check(w, com(s_device, DEV_DUPLICATE, 3, buffer, out));
            copy = rd(out);
        }
        check(w, com(buffer, B_STOP, 1));
        if (copy) com(copy, B_RELEASE, 1);
        com(buffer, B_RELEASE, 1);
        yield_machine();
    }
}

static void* second_guest_thread(void* arg) {
    worker_t* w = (worker_t*)arg;
    lx_thread_t* t = lx_thread_new(256 * 1024);
    lx_thread_attach(t);
    run_iterations(w);
    lx_thread_detach();
    lx_thread_free(t);
    return NULL;
}

/* The output is stopped before the buffers go: after each shutdown no mixer and no SDL audio remain, and the next device opens again. */
static void test_shutdown_cycles(void) {
    enum { CYCLES = 60 };
    unsigned bad_mixer = 0, bad_sdl = 0, bad_open = 0;
    for (int cycle = 0; cycle < CYCLES; cycle++) {
        uint32_t out = g_scratch + 0x9000;
        worker_t w = { 0, g_scratch + 0x12000 };
        uint32_t buffers[4];
        if (api("dsound.dll", "DirectSoundCreate", 3, 0u, out, 0u) != 0) {
            bad_open++;
            continue;
        }
        s_device = rd(out);
        com(s_device, DEV_SETCOOP, 3, 0x101u, 2u);
        for (int i = 0; i < 4; i++) {
            buffers[i] = create_buffer(&w);
            if (buffers[i]) com(buffers[i], B_PLAY, 4, 0u, 0u, 1u);
        }
        hle_dsound_shutdown();
        bad_mixer += dsound_compat_mixer() != NULL;
        bad_sdl += SDL_WasInit(SDL_INIT_AUDIO) != 0;
        hle_dsound_shutdown();
    }
    CHECK(bad_open == 0, "%d device sessions open after a shutdown (%u failed)", CYCLES, bad_open);
    CHECK(bad_mixer == 0 && bad_sdl == 0, "each shutdown, buffers still playing, leaves no mixer and no SDL audio (%u, %u wrong), and a second one is harmless", bad_mixer, bad_sdl);
}

int main(void) {
    worker_t first = { 0, 0 }, second = { 1, 0 };
    pthread_t thread;
    dsound_compat_counts_t counts;
    uint32_t out;
    if (!harness_start()) return 2;
    lx_dsound_set_headless(1);
    first.scratch = g_scratch + 0x10000;
    second.scratch = g_scratch + 0x11000;
    out = g_scratch + 0x9000;
    CHECK(api("dsound.dll", "DirectSoundCreate", 3, 0u, out, 0u) == 0, "DirectSoundCreate opens the dummy output");
    s_device = rd(out);
    CHECK(com(s_device, DEV_SETCOOP, 3, 0x101u, 2u) == 0, "SetCooperativeLevel");
    CHECK(pthread_create(&thread, NULL, second_guest_thread, &second) == 0, "the second guest thread starts");
    run_iterations(&first);
    {
        lx_blocking_t b;
        lx_blocking_begin(&b);
        pthread_join(thread, NULL);
        lx_blocking_end(&b);
    }
    CHECK(s_errors[0] == 0 && s_errors[1] == 0, "every call of both threads succeeded (%u and %u failures in %u and %u calls)", s_errors[0], s_errors[1],
          s_ops[0], s_ops[1]);
    dsound_compat_counts(&counts);
    CHECK(counts.buffers_created >= 2 * ITERATIONS && counts.buffers_live == 0, "every buffer was released (%u created, %u live)", counts.buffers_created, counts.buffers_live);
    test_shutdown_cycles();
    harness_stop();
    printf("linux_audio_stress_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
