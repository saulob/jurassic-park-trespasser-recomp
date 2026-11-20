/* The Win32 structures the Linux HLE reads and writes in guest memory, at their 32-bit Windows layouts: 4-byte pointers
 * and handles, i386 alignment, no dependence on the host's long or pointer width. Offsets are asserted at compile time. */
#ifndef JPT_LINUX_ABI_H
#define JPT_LINUX_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "abi/static_assert.h"

typedef uint32_t guest_dword_t;
typedef uint32_t guest_handle_t;
typedef uint32_t guest_pointer_t;

typedef struct {
    guest_dword_t dwLowDateTime, dwHighDateTime;
} guest_filetime_t;

typedef struct {
    guest_dword_t dwFileAttributes;
    guest_filetime_t ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    guest_dword_t nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
    char reserved[2];
} guest_find_data_t;

typedef struct {
    guest_dword_t cb;
    guest_pointer_t lpReserved, lpDesktop, lpTitle;
    guest_dword_t dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    uint16_t wShowWindow, cbReserved2;
    guest_pointer_t lpReserved2;
    guest_handle_t hStdInput, hStdOutput, hStdError;
} guest_startup_info_t;

typedef struct {
    guest_dword_t dwOemId;
    guest_dword_t dwPageSize;
    guest_pointer_t lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    guest_dword_t dwActiveProcessorMask, dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    uint16_t wProcessorLevel, wProcessorRevision;
} guest_system_info_t;

typedef struct {
    guest_dword_t dwLength, dwMemoryLoad, dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual,
        dwAvailVirtual;
} guest_memory_status_t;

typedef struct {
    guest_dword_t nLength;
    guest_pointer_t lpSecurityDescriptor;
    guest_dword_t bInheritHandle;
} guest_security_attributes_t;

typedef struct {
    guest_pointer_t DebugInfo;
    guest_dword_t LockCount, RecursionCount;
    guest_handle_t OwningThread, LockSemaphore;
    guest_dword_t SpinCount;
} guest_critical_section_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t wFormatTag, nChannels;
    guest_dword_t nSamplesPerSec, nAvgBytesPerSec;
    uint16_t nBlockAlign, wBitsPerSample, cbSize;
} guest_waveformatex_t;
#pragma pack(pop)

typedef struct {
    guest_dword_t hwnd, message, wParam, lParam, time;
    int32_t x, y;
} guest_msg_t;

JPT_STATIC_ASSERT(sizeof(guest_filetime_t) == 8, filetime);
JPT_STATIC_ASSERT(sizeof(guest_find_data_t) == 320, find_data);
JPT_STATIC_ASSERT(offsetof(guest_find_data_t, cFileName) == 44, find_data_name);
JPT_STATIC_ASSERT(sizeof(guest_startup_info_t) == 68, startup_info);
JPT_STATIC_ASSERT(sizeof(guest_system_info_t) == 36, system_info);
JPT_STATIC_ASSERT(sizeof(guest_memory_status_t) == 32, memory_status);
JPT_STATIC_ASSERT(sizeof(guest_security_attributes_t) == 12, security_attributes);
JPT_STATIC_ASSERT(sizeof(guest_waveformatex_t) == 18, waveformatex);
JPT_STATIC_ASSERT(offsetof(guest_waveformatex_t, cbSize) == 16, waveformatex_cbsize);
JPT_STATIC_ASSERT(sizeof(guest_critical_section_t) == 24, critical_section);
JPT_STATIC_ASSERT(sizeof(guest_msg_t) == 28, msg);
JPT_STATIC_ASSERT(sizeof(guest_dword_t) == 4 && sizeof(guest_handle_t) == 4 && sizeof(guest_pointer_t) == 4, widths);

#endif
