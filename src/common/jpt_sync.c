#include "jpt_sync.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

typedef enum { KIND_EVENT_MANUAL, KIND_EVENT_AUTO, KIND_SEMAPHORE, KIND_MUTEX, KIND_THREAD } kind_t;

struct jpt_waitable {
    kind_t kind;
    int signaled;
    int32_t count, maximum;
    uint64_t owner;
    int recursion;
    int abandoned;
    jpt_waitable_t* next_mutex;
};

/* One lock and one condition serve every wait: a state change wakes all sleepers and each re-checks its own objects. */
static SDL_Mutex* s_hub;
static SDL_Condition* s_hub_changed;
static SDL_InitState s_init;
static jpt_waitable_t* s_mutexes;
static Uint64 s_epoch_ns;

static void init_once(void) {
    if (!SDL_ShouldInit(&s_init)) return;
    s_hub = SDL_CreateMutex();
    s_hub_changed = SDL_CreateCondition();
    s_epoch_ns = SDL_GetTicksNS();
    SDL_SetInitialized(&s_init, true);
}

static jpt_waitable_t* new_waitable(kind_t kind) {
    jpt_waitable_t* w;
    init_once();
    w = (jpt_waitable_t*)calloc(1, sizeof *w);
    if (w) w->kind = kind;
    return w;
}

static void changed(void) { SDL_BroadcastCondition(s_hub_changed); }

jpt_waitable_t* jpt_event_new(int manual_reset, int initially_set) {
    jpt_waitable_t* e = new_waitable(manual_reset ? KIND_EVENT_MANUAL : KIND_EVENT_AUTO);
    if (e) e->signaled = initially_set != 0;
    return e;
}

int jpt_event_set(jpt_waitable_t* e) {
    if (!e || (e->kind != KIND_EVENT_MANUAL && e->kind != KIND_EVENT_AUTO)) return 0;
    SDL_LockMutex(s_hub);
    e->signaled = 1;
    changed();
    SDL_UnlockMutex(s_hub);
    return 1;
}

int jpt_event_reset(jpt_waitable_t* e) {
    if (!e || (e->kind != KIND_EVENT_MANUAL && e->kind != KIND_EVENT_AUTO)) return 0;
    SDL_LockMutex(s_hub);
    e->signaled = 0;
    SDL_UnlockMutex(s_hub);
    return 1;
}

jpt_waitable_t* jpt_semaphore_new(int32_t initial, int32_t maximum) {
    jpt_waitable_t* s;
    if (maximum <= 0 || initial < 0 || initial > maximum) return NULL;
    s = new_waitable(KIND_SEMAPHORE);
    if (s) {
        s->count = initial;
        s->maximum = maximum;
    }
    return s;
}

int jpt_semaphore_release(jpt_waitable_t* s, int32_t count, int32_t* previous) {
    int ok;
    if (!s || s->kind != KIND_SEMAPHORE || count <= 0) return 0;
    SDL_LockMutex(s_hub);
    ok = s->count <= s->maximum - count;
    if (previous) *previous = s->count;
    if (ok) {
        s->count += count;
        changed();
    }
    SDL_UnlockMutex(s_hub);
    return ok;
}

jpt_waitable_t* jpt_mutex_new(int initially_owned) {
    jpt_waitable_t* m = new_waitable(KIND_MUTEX);
    if (!m) return NULL;
    SDL_LockMutex(s_hub);
    if (initially_owned) {
        m->owner = jpt_thread_id();
        m->recursion = 1;
    }
    m->next_mutex = s_mutexes;
    s_mutexes = m;
    SDL_UnlockMutex(s_hub);
    return m;
}

int jpt_mutex_release(jpt_waitable_t* m) {
    int ok;
    if (!m || m->kind != KIND_MUTEX) return 0;
    SDL_LockMutex(s_hub);
    ok = m->owner == jpt_thread_id() && m->recursion > 0;
    if (ok && --m->recursion == 0) {
        m->owner = 0;
        changed();
    }
    SDL_UnlockMutex(s_hub);
    return ok;
}

void jpt_waitable_free(jpt_waitable_t* w) {
    if (!w) return;
    if (w->kind == KIND_MUTEX) {
        jpt_waitable_t** link;
        SDL_LockMutex(s_hub);
        for (link = &s_mutexes; *link; link = &(*link)->next_mutex)
            if (*link == w) {
                *link = w->next_mutex;
                break;
            }
        SDL_UnlockMutex(s_hub);
    }
    free(w);
}

static int can_take(const jpt_waitable_t* w, uint64_t me) {
    switch (w->kind) {
    case KIND_EVENT_MANUAL:
    case KIND_EVENT_AUTO:
    case KIND_THREAD: return w->signaled;
    case KIND_SEMAPHORE: return w->count > 0;
    case KIND_MUTEX: return w->owner == 0 || w->owner == me;
    }
    return 0;
}

/* Takes the object (the hub lock is held and can_take was true); returns whether it was an abandoned mutex. */
static int take(jpt_waitable_t* w, uint64_t me) {
    switch (w->kind) {
    case KIND_EVENT_AUTO: w->signaled = 0; break;
    case KIND_SEMAPHORE: w->count--; break;
    case KIND_MUTEX: {
        int abandoned = w->abandoned;
        w->abandoned = 0;
        w->owner = me;
        w->recursion++;
        return abandoned;
    }
    default: break;
    }
    return 0;
}

static uint32_t wait_locked(jpt_waitable_t* const* w, uint32_t count, int wait_all, uint32_t timeout_ms) {
    uint64_t me = jpt_thread_id();
    Uint64 deadline = timeout_ms == JPT_INFINITE ? 0 : SDL_GetTicksNS() + (Uint64)timeout_ms * 1000000ull;
    for (;;) {
        uint32_t i;
        if (wait_all) {
            int all = 1;
            for (i = 0; i < count && all; i++) all = can_take(w[i], me);
            if (all) {
                uint32_t result = JPT_WAIT_OBJECT_0;
                for (i = 0; i < count; i++)
                    if (take(w[i], me) && result == JPT_WAIT_OBJECT_0) result = JPT_WAIT_ABANDONED_0 + i;
                return result;
            }
        } else {
            for (i = 0; i < count; i++)
                if (can_take(w[i], me)) return take(w[i], me) ? JPT_WAIT_ABANDONED_0 + i : JPT_WAIT_OBJECT_0 + i;
        }
        if (timeout_ms == 0) return JPT_WAIT_TIMEOUT;
        if (timeout_ms == JPT_INFINITE) {
            SDL_WaitCondition(s_hub_changed, s_hub);
        } else {
            Uint64 now = SDL_GetTicksNS();
            if (now >= deadline) return JPT_WAIT_TIMEOUT;
            SDL_WaitConditionTimeout(s_hub_changed, s_hub, (Sint32)((deadline - now + 999999ull) / 1000000ull));
        }
    }
}

uint32_t jpt_wait_multiple(jpt_waitable_t* const* waitables, uint32_t count, int wait_all, uint32_t timeout_ms) {
    uint32_t r;
    if (!waitables || !count || count > 64) return JPT_WAIT_FAILED;
    for (uint32_t i = 0; i < count; i++)
        if (!waitables[i]) return JPT_WAIT_FAILED;
    SDL_LockMutex(s_hub);
    r = wait_locked(waitables, count, wait_all, timeout_ms);
    SDL_UnlockMutex(s_hub);
    return r;
}

uint32_t jpt_wait(jpt_waitable_t* w, uint32_t timeout_ms) { return jpt_wait_multiple(&w, 1, 0, timeout_ms); }

/* ---- threads ---- */

struct jpt_thread {
    SDL_Thread* handle;
    jpt_thread_fn fn;
    void* arg;
    uint32_t exit_code;
    int detached, done;
    jpt_waitable_t* waitable;
};

uint64_t jpt_thread_id(void) { return (uint64_t)SDL_GetCurrentThreadID(); }

static int thread_main(void* arg) {
    jpt_thread_t* t = (jpt_thread_t*)arg;
    uint64_t me = jpt_thread_id();
    int detached;
    uint32_t code = t->fn(t->arg);
    SDL_LockMutex(s_hub);
    t->exit_code = code;
    for (jpt_waitable_t* m = s_mutexes; m; m = m->next_mutex)
        if (m->owner == me) {
            m->owner = 0;
            m->recursion = 0;
            m->abandoned = 1;
        }
    t->waitable->signaled = 1;
    t->done = 1;
    detached = t->detached;
    changed();
    SDL_UnlockMutex(s_hub);
    if (detached) {
        free(t->waitable);
        free(t);
    }
    return 0;
}

jpt_thread_t* jpt_thread_create(jpt_thread_fn fn, void* arg) {
    jpt_thread_t* t;
    init_once();
    t = (jpt_thread_t*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->waitable = new_waitable(KIND_THREAD);
    t->fn = fn;
    t->arg = arg;
    if (!t->waitable || !(t->handle = SDL_CreateThread(thread_main, "jpt guest thread", t))) {
        free(t->waitable);
        free(t);
        return NULL;
    }
    return t;
}

jpt_waitable_t* jpt_thread_waitable(jpt_thread_t* t) { return t ? t->waitable : NULL; }

int jpt_thread_exit_code(jpt_thread_t* t, uint32_t* code) {
    int done;
    if (!t) return 0;
    SDL_LockMutex(s_hub);
    done = t->done;
    if (done && code) *code = t->exit_code;
    SDL_UnlockMutex(s_hub);
    return done;
}

void jpt_thread_destroy(jpt_thread_t* t) {
    if (!t) return;
    SDL_WaitThread(t->handle, NULL);
    free(t->waitable);
    free(t);
}

void jpt_thread_detach(jpt_thread_t* t) {
    int already_done;
    if (!t) return;
    SDL_LockMutex(s_hub);
    t->detached = 1;
    already_done = t->done;
    SDL_UnlockMutex(s_hub);
    SDL_DetachThread(t->handle);
    if (already_done) {
        free(t->waitable);
        free(t);
    }
}

/* ---- critical sections ---- */

struct jpt_cs {
    SDL_Mutex* mutex;
};

jpt_cs_t* jpt_cs_new(void) {
    jpt_cs_t* cs = (jpt_cs_t*)calloc(1, sizeof *cs);
    if (cs && !(cs->mutex = SDL_CreateMutex())) {
        free(cs);
        return NULL;
    }
    return cs;
}

void jpt_cs_enter(jpt_cs_t* cs) { SDL_LockMutex(cs->mutex); }
int jpt_cs_try_enter(jpt_cs_t* cs) { return SDL_TryLockMutex(cs->mutex); }
void jpt_cs_leave(jpt_cs_t* cs) { SDL_UnlockMutex(cs->mutex); }

void jpt_cs_free(jpt_cs_t* cs) {
    if (!cs) return;
    SDL_DestroyMutex(cs->mutex);
    free(cs);
}

/* ---- clocks and timers ---- */

uint32_t jpt_time_ms(void) {
    init_once();
    return (uint32_t)((SDL_GetTicksNS() - s_epoch_ns) / 1000000ull);
}

uint64_t jpt_counter(void) { return (uint64_t)SDL_GetPerformanceCounter(); }
uint64_t jpt_counter_frequency(void) { return (uint64_t)SDL_GetPerformanceFrequency(); }
void jpt_sleep_ms(uint32_t ms) { SDL_Delay(ms); }

struct jpt_timer {
    SDL_TimerID id;
    jpt_timer_fn fn;
    void* user;
    uint64_t interval_ns;
    int periodic, killed, running;
};

static Uint64 timer_tick(void* user, SDL_TimerID id, Uint64 interval) {
    jpt_timer_t* t = (jpt_timer_t*)user;
    int killed;
    (void)id;
    SDL_LockMutex(s_hub);
    killed = t->killed;
    if (!killed) t->running = 1;
    SDL_UnlockMutex(s_hub);
    if (killed) return 0;
    t->fn(t->user);
    SDL_LockMutex(s_hub);
    t->running = 0;
    killed = t->killed;
    changed();
    SDL_UnlockMutex(s_hub);
    return t->periodic && !killed ? interval : 0;
}

jpt_timer_t* jpt_timer_start(uint32_t delay_ms, int periodic, jpt_timer_fn fn, void* user) {
    jpt_timer_t* t;
    init_once();
    if (!fn) return NULL;
    t = (jpt_timer_t*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->user = user;
    t->periodic = periodic != 0;
    t->interval_ns = (Uint64)(delay_ms ? delay_ms : 1) * 1000000ull;
    t->id = SDL_AddTimerNS(t->interval_ns, timer_tick, t);
    if (!t->id) {
        free(t);
        return NULL;
    }
    return t;
}

void jpt_timer_kill(jpt_timer_t* t) {
    if (!t) return;
    SDL_LockMutex(s_hub);
    t->killed = 1;
    SDL_UnlockMutex(s_hub);
    SDL_RemoveTimer(t->id);
    SDL_LockMutex(s_hub);
    while (t->running) SDL_WaitCondition(s_hub_changed, s_hub);
    SDL_UnlockMutex(s_hub);
    free(t);
}
