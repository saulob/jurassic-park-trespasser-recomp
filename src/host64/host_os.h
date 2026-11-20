#ifndef HOST64_HOST_OS_H
#define HOST64_HOST_OS_H

/* The operating-system services the 64-bit guest memory and thunk layers need: one address-space reservation with
 * commit/decommit/protect, a reader/writer lock, an atomic counter and thread-local storage. Implemented for Windows
 * (host_os_win.c) and POSIX (host_os_posix.c). */

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK hos_rwlock_t;
#define HOS_RWLOCK_INIT SRWLOCK_INIT
#define HOS_THREAD_LOCAL __declspec(thread)
#else
#include <pthread.h>
typedef pthread_rwlock_t hos_rwlock_t;
#define HOS_RWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define HOS_THREAD_LOCAL _Thread_local
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { HOS_PROT_NONE = 0, HOS_PROT_R = 1, HOS_PROT_RW = 2 };

void hos_rdlock(hos_rwlock_t* lock);
void hos_wrlock(hos_rwlock_t* lock);
void hos_unlock_rd(hos_rwlock_t* lock);
void hos_unlock_wr(hos_rwlock_t* lock);
void hos_atomic_inc(volatile int32_t* value);

/* Reserve address space, inaccessible until committed. A preferred base (0: none) is tried first; require makes it
 * the only choice. Returns NULL on failure. The size of the reservation is remembered for hos_query. */
uint8_t* hos_reserve(uint64_t preferred_base, size_t size, int require);
void hos_release(uint8_t* base, size_t size);
/* Pages inside a reservation. A committed page reads as zero until written; decommit returns it to zero and
 * inaccessible. Sizes are multiples of the 4 KB page. */
int hos_commit(uint8_t* at, size_t size, int prot);
int hos_decommit(uint8_t* at, size_t size);
int hos_protect(uint8_t* at, size_t size, int prot);
/* Whether every page of [at, at+len) inside the reservation is committed and readable (writable if set). */
int hos_accessible(const uint8_t* at, size_t len, int writable);

#ifdef __cplusplus
}
#endif

#endif
