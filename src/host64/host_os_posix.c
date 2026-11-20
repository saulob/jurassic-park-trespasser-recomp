#ifndef _WIN32
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "host_os.h"

#define PAGE 0x1000u

/* One byte per 4 KB page of the single reservation: what the guest was given, since POSIX has no VirtualQuery. */
static uint8_t* s_base;
static size_t s_size;
static uint8_t* s_state;

void hos_rdlock(hos_rwlock_t* lock) { pthread_rwlock_rdlock(lock); }
void hos_wrlock(hos_rwlock_t* lock) { pthread_rwlock_wrlock(lock); }
void hos_unlock_rd(hos_rwlock_t* lock) { pthread_rwlock_unlock(lock); }
void hos_unlock_wr(hos_rwlock_t* lock) { pthread_rwlock_unlock(lock); }
void hos_atomic_inc(volatile int32_t* value) { __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }

static int posix_prot(int prot) {
    return prot == HOS_PROT_RW ? (PROT_READ | PROT_WRITE) : prot == HOS_PROT_R ? PROT_READ : PROT_NONE;
}

static void set_state(const uint8_t* at, size_t size, uint8_t value) {
    size_t first = (size_t)(at - s_base) / PAGE, count = (size + PAGE - 1) / PAGE;
    if (s_state) memset(s_state + first, value, count);
}

#define GRANULARITY 0x10000u

/* A reservation on a 64 KB boundary, like VirtualAlloc's: the kernel only promises page alignment. */
static void* map_aligned(size_t size, int flags) {
    uint8_t* raw = (uint8_t*)mmap(NULL, size + GRANULARITY, PROT_NONE, flags, -1, 0);
    uintptr_t aligned;
    if (raw == MAP_FAILED) return MAP_FAILED;
    aligned = ((uintptr_t)raw + GRANULARITY - 1) & ~(uintptr_t)(GRANULARITY - 1);
    if (aligned > (uintptr_t)raw) munmap(raw, aligned - (uintptr_t)raw);
    if ((uintptr_t)raw + GRANULARITY > aligned) munmap((void*)(aligned + size), (uintptr_t)raw + GRANULARITY - aligned);
    return (void*)aligned;
}

uint8_t* hos_reserve(uint64_t preferred_base, size_t size, int require) {
    void* p = MAP_FAILED;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (s_base) return NULL;
    if (preferred_base) {
#ifdef MAP_FIXED_NOREPLACE
        p = mmap((void*)(uintptr_t)preferred_base, size, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uint64_t)(uintptr_t)p != preferred_base) {
            munmap(p, size);
            p = MAP_FAILED;
        }
#endif
    }
    if (p == MAP_FAILED && !(preferred_base && require)) p = map_aligned(size, flags);
    if (p == MAP_FAILED) return NULL;
    s_state = (uint8_t*)mmap(NULL, size / PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s_state == (uint8_t*)MAP_FAILED) {
        s_state = NULL;
        munmap(p, size);
        return NULL;
    }
    s_base = (uint8_t*)p;
    s_size = size;
    return s_base;
}

void hos_release(uint8_t* base, size_t size) {
    munmap(base, size);
    if (s_state) munmap(s_state, s_size / PAGE);
    s_state = NULL;
    s_base = NULL;
    s_size = 0;
}

int hos_commit(uint8_t* at, size_t size, int prot) {
    if (mprotect(at, size, posix_prot(prot)) != 0) return 0;
    set_state(at, size, (uint8_t)prot);
    return 1;
}

int hos_decommit(uint8_t* at, size_t size) {
    if (madvise(at, size, MADV_DONTNEED) != 0) return 0;
    if (mprotect(at, size, PROT_NONE) != 0) return 0;
    set_state(at, size, HOS_PROT_NONE);
    return 1;
}

int hos_protect(uint8_t* at, size_t size, int prot) { return hos_commit(at, size, prot); }

int hos_accessible(const uint8_t* at, size_t len, int writable) {
    size_t first, last;
    if (!s_base || at < s_base || (size_t)(at - s_base) + (len ? len : 1) > s_size) return 0;
    first = (size_t)(at - s_base) / PAGE;
    last = ((size_t)(at - s_base) + (len ? len : 1) - 1) / PAGE;
    for (; first <= last; first++)
        if (s_state[first] < (writable ? HOS_PROT_RW : HOS_PROT_R)) return 0;
    return 1;
}
#endif
