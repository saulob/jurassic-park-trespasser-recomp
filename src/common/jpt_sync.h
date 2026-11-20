/* Win32-style events, semaphores, mutexes, threads, waits, critical sections, clocks and timers over SDL3, for the host side
 * of the guest's kernel32. Waits sleep on a condition variable; time is monotonic, never the wall clock. */
#ifndef JPT_SYNC_H
#define JPT_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_INFINITE 0xFFFFFFFFu

#define JPT_WAIT_OBJECT_0 0u
#define JPT_WAIT_ABANDONED_0 0x80u
#define JPT_WAIT_TIMEOUT 0x102u
#define JPT_WAIT_FAILED 0xFFFFFFFFu

typedef struct jpt_waitable jpt_waitable_t;

/* Events. A manual-reset event stays set until reset; an auto-reset event releases one waiter and clears itself. */
jpt_waitable_t* jpt_event_new(int manual_reset, int initially_set);
int jpt_event_set(jpt_waitable_t* e);
int jpt_event_reset(jpt_waitable_t* e);

jpt_waitable_t* jpt_semaphore_new(int32_t initial, int32_t maximum);
/* Adds count; fails (0) when the maximum would be passed. previous receives the count before. */
int jpt_semaphore_release(jpt_waitable_t* s, int32_t count, int32_t* previous);

/* A mutex is owned by the thread that waited on it or created it owned, recursively. */
jpt_waitable_t* jpt_mutex_new(int initially_owned);
int jpt_mutex_release(jpt_waitable_t* m);

void jpt_waitable_free(jpt_waitable_t* w);

/* The wait functions return JPT_WAIT_OBJECT_0 + index, JPT_WAIT_ABANDONED_0 + index (a mutex whose owner exited),
 * JPT_WAIT_TIMEOUT or JPT_WAIT_FAILED. wait_all takes every object atomically or none. A timeout of 0 only tests. */
uint32_t jpt_wait(jpt_waitable_t* w, uint32_t timeout_ms);
uint32_t jpt_wait_multiple(jpt_waitable_t* const* waitables, uint32_t count, int wait_all, uint32_t timeout_ms);

/* Threads. The thread object (a waitable owned by the thread record) is signalled when the function returns. A mutex
 * the thread still owns then becomes abandoned. */
typedef uint32_t (*jpt_thread_fn)(void* arg);
typedef struct jpt_thread jpt_thread_t;
jpt_thread_t* jpt_thread_create(jpt_thread_fn fn, void* arg);
jpt_waitable_t* jpt_thread_waitable(jpt_thread_t* t);
int jpt_thread_exit_code(jpt_thread_t* t, uint32_t* code);
uint64_t jpt_thread_id(void);
/* Waits for the thread to end and frees its record and its waitable. */
void jpt_thread_destroy(jpt_thread_t* t);
/* Lets the thread free its own record when it ends; the waitable then must not be used again. */
void jpt_thread_detach(jpt_thread_t* t);

/* Critical sections: recursive. */
typedef struct jpt_cs jpt_cs_t;
jpt_cs_t* jpt_cs_new(void);
void jpt_cs_enter(jpt_cs_t* cs);
int jpt_cs_try_enter(jpt_cs_t* cs);
void jpt_cs_leave(jpt_cs_t* cs);
void jpt_cs_free(jpt_cs_t* cs);

/* Clocks: milliseconds since the first call to any clock function (timeGetTime and GetTickCount; wraps at 2^32 as the
 * originals do), and a 64-bit counter with its frequency (QueryPerformanceCounter and Frequency). */
uint32_t jpt_time_ms(void);
uint64_t jpt_counter(void);
uint64_t jpt_counter_frequency(void);
void jpt_sleep_ms(uint32_t ms);

/* Timers: timeSetEvent and timeKillEvent. The callback runs on a timer thread; kill returns after a callback that is
 * already running has finished. */
typedef void (*jpt_timer_fn)(void* user);
typedef struct jpt_timer jpt_timer_t;
jpt_timer_t* jpt_timer_start(uint32_t delay_ms, int periodic, jpt_timer_fn fn, void* user);
void jpt_timer_kill(jpt_timer_t* t);

#ifdef __cplusplus
}
#endif

#endif
