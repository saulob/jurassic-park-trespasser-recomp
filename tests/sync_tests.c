/* The host side of the guest's synchronisation and time: events (manual and auto reset), semaphores, mutexes with
 * ownership and abandonment, waits on one or several objects with timeouts, threads, critical sections, the monotonic
 * clock and timers. Headless: SDL3's threads and timers only; runs on Windows and Linux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_sync.h"

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

static void test_events(void) {
    jpt_waitable_t* manual = jpt_event_new(1, 0);
    jpt_waitable_t* autom = jpt_event_new(0, 1);
    CHECK(jpt_wait(manual, 0) == JPT_WAIT_TIMEOUT, "a clear event times out at once");
    CHECK(jpt_event_set(manual), "SetEvent");
    CHECK(jpt_wait(manual, 0) == JPT_WAIT_OBJECT_0 && jpt_wait(manual, 0) == JPT_WAIT_OBJECT_0,
          "a manual-reset event stays signalled");
    CHECK(jpt_event_reset(manual) && jpt_wait(manual, 0) == JPT_WAIT_TIMEOUT, "ResetEvent");
    CHECK(jpt_wait(autom, 0) == JPT_WAIT_OBJECT_0, "an auto-reset event created set releases a wait");
    CHECK(jpt_wait(autom, 0) == JPT_WAIT_TIMEOUT, "and clears itself");
    jpt_event_set(autom);
    CHECK(jpt_wait(autom, JPT_INFINITE) == JPT_WAIT_OBJECT_0, "an infinite wait on a set event returns");
    CHECK(!jpt_event_set(NULL) && jpt_wait(NULL, 0) == JPT_WAIT_FAILED, "NULL is refused");
    jpt_waitable_free(manual);
    jpt_waitable_free(autom);
}

static void test_semaphore(void) {
    jpt_waitable_t* s = jpt_semaphore_new(1, 3);
    int32_t previous = -1;
    CHECK(jpt_semaphore_new(0, 0) == NULL && jpt_semaphore_new(4, 3) == NULL, "bad counts are refused");
    CHECK(jpt_wait(s, 0) == JPT_WAIT_OBJECT_0 && jpt_wait(s, 0) == JPT_WAIT_TIMEOUT, "one unit, then none");
    CHECK(jpt_semaphore_release(s, 2, &previous) && previous == 0, "ReleaseSemaphore reports the previous count");
    CHECK(!jpt_semaphore_release(s, 2, &previous) && previous == 2, "past the maximum fails and changes nothing");
    CHECK(jpt_wait(s, 0) == JPT_WAIT_OBJECT_0 && jpt_wait(s, 0) == JPT_WAIT_OBJECT_0 && jpt_wait(s, 0) == JPT_WAIT_TIMEOUT,
          "the two units are taken one wait at a time");
    jpt_waitable_free(s);
}

typedef struct {
    jpt_waitable_t* mutex;
    jpt_waitable_t* ready;
    jpt_waitable_t* go;
    uint32_t wait_result, release_result;
} mutex_probe_t;

static uint32_t mutex_probe_thread(void* arg) {
    mutex_probe_t* p = (mutex_probe_t*)arg;
    p->release_result = (uint32_t)jpt_mutex_release(p->mutex);
    p->wait_result = jpt_wait(p->mutex, 0);
    jpt_event_set(p->ready);
    jpt_wait(p->go, JPT_INFINITE);
    return 7;
}

static void test_mutex(void) {
    mutex_probe_t p;
    jpt_thread_t* t;
    uint32_t code = 0;
    p.mutex = jpt_mutex_new(1);
    p.ready = jpt_event_new(1, 0);
    p.go = jpt_event_new(1, 0);
    CHECK(jpt_wait(p.mutex, 0) == JPT_WAIT_OBJECT_0, "the owner waits again: recursive");
    CHECK(jpt_mutex_release(p.mutex) && jpt_mutex_release(p.mutex) && !jpt_mutex_release(p.mutex),
          "released once per acquisition, then it is no longer owned");
    CHECK(jpt_wait(p.mutex, 0) == JPT_WAIT_OBJECT_0, "free again");
    t = jpt_thread_create(mutex_probe_thread, &p);
    CHECK(t != NULL, "thread");
    CHECK(jpt_wait(p.ready, 5000) == JPT_WAIT_OBJECT_0, "the other thread ran");
    CHECK(p.release_result == 0, "a thread that does not own a mutex cannot release it");
    CHECK(p.wait_result == JPT_WAIT_TIMEOUT, "nor take it while another thread owns it");
    jpt_event_set(p.go);
    CHECK(jpt_wait(jpt_thread_waitable(t), 5000) == JPT_WAIT_OBJECT_0 && jpt_thread_exit_code(t, &code) && code == 7,
          "the thread object is signalled with its exit code");
    jpt_thread_destroy(t);
    jpt_mutex_release(p.mutex);
    jpt_waitable_free(p.mutex);
    jpt_waitable_free(p.ready);
    jpt_waitable_free(p.go);
}

typedef struct {
    jpt_waitable_t* mutex;
} owner_exit_t;

static uint32_t take_and_exit(void* arg) {
    owner_exit_t* o = (owner_exit_t*)arg;
    jpt_wait(o->mutex, JPT_INFINITE);
    return 0;
}

static void test_abandoned_mutex(void) {
    owner_exit_t o;
    jpt_thread_t* t;
    o.mutex = jpt_mutex_new(0);
    t = jpt_thread_create(take_and_exit, &o);
    jpt_wait(jpt_thread_waitable(t), 5000);
    jpt_thread_destroy(t);
    CHECK(jpt_wait(o.mutex, 0) == JPT_WAIT_ABANDONED_0, "a mutex whose owner exited is reported abandoned");
    CHECK(jpt_wait(o.mutex, 0) == JPT_WAIT_OBJECT_0, "and then owned by the waiter, recursively");
    jpt_mutex_release(o.mutex);
    jpt_mutex_release(o.mutex);
    jpt_waitable_free(o.mutex);
}

typedef struct {
    jpt_waitable_t* event;
    uint32_t delay_ms;
} delayed_set_t;

static uint32_t delayed_set(void* arg) {
    delayed_set_t* d = (delayed_set_t*)arg;
    jpt_sleep_ms(d->delay_ms);
    jpt_event_set(d->event);
    return 0;
}

static void test_waits(void) {
    jpt_waitable_t* a = jpt_event_new(1, 0);
    jpt_waitable_t* b = jpt_event_new(1, 0);
    jpt_waitable_t* both[2];
    delayed_set_t d;
    jpt_thread_t* t;
    uint32_t t0, t1;
    both[0] = a;
    both[1] = b;

    t0 = jpt_time_ms();
    CHECK(jpt_wait(a, 60) == JPT_WAIT_TIMEOUT, "a timed wait times out");
    t1 = jpt_time_ms();
    CHECK(t1 - t0 >= 55 && t1 - t0 < 1000, "after about the timeout, on the monotonic clock (%u ms)", t1 - t0);

    jpt_event_set(b);
    CHECK(jpt_wait_multiple(both, 2, 0, 0) == JPT_WAIT_OBJECT_0 + 1, "wait-any returns the index of the signalled object");
    CHECK(jpt_wait_multiple(both, 2, 1, 0) == JPT_WAIT_TIMEOUT, "wait-all needs every object");
    jpt_event_set(a);
    CHECK(jpt_wait_multiple(both, 2, 1, 0) == JPT_WAIT_OBJECT_0, "and succeeds when all are set");
    CHECK(jpt_wait_multiple(both, 0, 0, 0) == JPT_WAIT_FAILED, "an empty list fails");

    jpt_event_reset(a);
    {
        jpt_waitable_t* autos[2];
        autos[0] = jpt_event_new(0, 1);
        autos[1] = jpt_event_new(0, 0);
        CHECK(jpt_wait_multiple(autos, 2, 1, 0) == JPT_WAIT_TIMEOUT, "wait-all with a clear auto-reset event times out");
        CHECK(jpt_wait(autos[0], 0) == JPT_WAIT_OBJECT_0, "and took nothing: the set auto-reset event was not consumed");
        jpt_waitable_free(autos[0]);
        jpt_waitable_free(autos[1]);
    }

    d.event = a;
    d.delay_ms = 80;
    t0 = jpt_time_ms();
    t = jpt_thread_create(delayed_set, &d);
    CHECK(jpt_wait(a, JPT_INFINITE) == JPT_WAIT_OBJECT_0, "a sleeping wait is woken by another thread");
    t1 = jpt_time_ms();
    CHECK(t1 - t0 >= 70 && t1 - t0 < 2000, "after the other thread's delay (%u ms)", t1 - t0);
    jpt_wait(jpt_thread_waitable(t), 5000);
    jpt_thread_destroy(t);
    jpt_waitable_free(a);
    jpt_waitable_free(b);
}

#define CS_THREADS 4
#define CS_ROUNDS 2000

typedef struct {
    jpt_cs_t* cs;
    long* counter;
} cs_job_t;

static uint32_t cs_worker(void* arg) {
    cs_job_t* j = (cs_job_t*)arg;
    for (int i = 0; i < CS_ROUNDS; i++) {
        jpt_cs_enter(j->cs);
        jpt_cs_enter(j->cs);
        (*j->counter)++;
        jpt_cs_leave(j->cs);
        jpt_cs_leave(j->cs);
    }
    return 0;
}

static void test_critical_section(void) {
    jpt_cs_t* cs = jpt_cs_new();
    long counter = 0;
    cs_job_t job;
    jpt_thread_t* threads[CS_THREADS];
    job.cs = cs;
    job.counter = &counter;
    CHECK(jpt_cs_try_enter(cs) && jpt_cs_try_enter(cs), "recursive entry by the owner");
    jpt_cs_leave(cs);
    jpt_cs_leave(cs);
    for (int i = 0; i < CS_THREADS; i++) threads[i] = jpt_thread_create(cs_worker, &job);
    for (int i = 0; i < CS_THREADS; i++) {
        jpt_wait(jpt_thread_waitable(threads[i]), 20000);
        jpt_thread_destroy(threads[i]);
    }
    CHECK(counter == (long)CS_THREADS * CS_ROUNDS, "no lost update under contention (%ld)", counter);
    jpt_cs_free(cs);
}

typedef struct {
    jpt_waitable_t* fired;
    volatile int count;
} timer_probe_t;

static void timer_callback(void* user) {
    timer_probe_t* p = (timer_probe_t*)user;
    p->count++;
    jpt_event_set(p->fired);
}

static void slow_callback(void* user) {
    timer_probe_t* p = (timer_probe_t*)user;
    jpt_sleep_ms(120);
    p->count++;
}

static void test_clocks_and_timers(void) {
    timer_probe_t p;
    jpt_timer_t* timer;
    uint64_t c0, c1;
    uint32_t last = jpt_time_ms();
    int monotonic = 1;
    p.fired = jpt_event_new(0, 0);
    p.count = 0;

    for (int i = 0; i < 100; i++) {
        uint32_t now = jpt_time_ms();
        if ((int32_t)(now - last) < 0) monotonic = 0;
        last = now;
    }
    CHECK(monotonic, "the millisecond clock never goes back");
    c0 = jpt_counter();
    jpt_sleep_ms(20);
    c1 = jpt_counter();
    CHECK(jpt_counter_frequency() > 0 && c1 > c0, "the performance counter advances");
    CHECK((c1 - c0) * 1000 / jpt_counter_frequency() >= 15, "at its frequency (%llu ms for a 20 ms sleep)",
          (unsigned long long)((c1 - c0) * 1000 / jpt_counter_frequency()));

    timer = jpt_timer_start(30, 0, timer_callback, &p);
    CHECK(timer != NULL, "a one-shot timer");
    CHECK(jpt_wait(p.fired, 5000) == JPT_WAIT_OBJECT_0, "it fires");
    jpt_sleep_ms(120);
    CHECK(p.count == 1, "exactly once (%d)", p.count);
    jpt_timer_kill(timer);

    p.count = 0;
    timer = jpt_timer_start(20, 1, timer_callback, &p);
    for (int i = 0; i < 4; i++) CHECK(jpt_wait(p.fired, 5000) == JPT_WAIT_OBJECT_0, "periodic tick %d", i);
    jpt_timer_kill(timer);
    {
        int at_kill = p.count;
        jpt_sleep_ms(100);
        CHECK(p.count == at_kill, "no tick after kill (%d, then %d)", at_kill, p.count);
    }

    p.count = 0;
    timer = jpt_timer_start(10, 0, slow_callback, &p);
    jpt_sleep_ms(50);
    jpt_timer_kill(timer);
    CHECK(p.count == 1, "kill waits for a callback that is running (%d)", p.count);
    jpt_waitable_free(p.fired);
}

int main(void) {
    test_events();
    test_semaphore();
    test_mutex();
    test_abandoned_mutex();
    test_waits();
    test_critical_section();
    test_clocks_and_timers();
    printf("sync_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
