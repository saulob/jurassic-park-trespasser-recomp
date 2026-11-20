#ifdef _WIN32
#include "host_os.h"

void hos_rdlock(hos_rwlock_t* lock) { AcquireSRWLockShared(lock); }
void hos_wrlock(hos_rwlock_t* lock) { AcquireSRWLockExclusive(lock); }
void hos_unlock_rd(hos_rwlock_t* lock) { ReleaseSRWLockShared(lock); }
void hos_unlock_wr(hos_rwlock_t* lock) { ReleaseSRWLockExclusive(lock); }
void hos_atomic_inc(volatile int32_t* value) { InterlockedIncrement((volatile LONG*)value); }

static DWORD win_prot(int prot) {
    return prot == HOS_PROT_RW ? PAGE_READWRITE : prot == HOS_PROT_R ? PAGE_READONLY : PAGE_NOACCESS;
}

uint8_t* hos_reserve(uint64_t preferred_base, size_t size, int require) {
    uint8_t* p = NULL;
    if (preferred_base) p = (uint8_t*)VirtualAlloc((void*)(uintptr_t)preferred_base, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p && !(preferred_base && require)) p = (uint8_t*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    return p;
}

void hos_release(uint8_t* base, size_t size) {
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
}

int hos_commit(uint8_t* at, size_t size, int prot) { return VirtualAlloc(at, size, MEM_COMMIT, win_prot(prot)) != NULL; }
int hos_decommit(uint8_t* at, size_t size) { return VirtualFree(at, size, MEM_DECOMMIT) != 0; }

int hos_protect(uint8_t* at, size_t size, int prot) {
    DWORD old;
    return VirtualProtect(at, size, win_prot(prot), &old) != 0;
}

int hos_accessible(const uint8_t* at, size_t len, int writable) {
    uint64_t a = (uint64_t)(uintptr_t)at, end = a + (len ? len : 1);
    DWORD ok = writable ? (PAGE_READWRITE | PAGE_WRITECOPY) : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & ok) ||
            (mbi.Protect & PAGE_GUARD))
            return 0;
        a = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
#endif
