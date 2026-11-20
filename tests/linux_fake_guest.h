#ifndef LINUX_FAKE_GUEST_H
#define LINUX_FAKE_GUEST_H

#include <stdint.h>

#define FAKE_STDCALL0 0x00500000u
#define FAKE_STDCALL1_PTR 0x00500010u
#define FAKE_STDCALL3 0x00500020u
#define FAKE_CDECL3 0x00500030u
#define FAKE_THISCALL 0x00500040u
#define FAKE_CLOBBER 0x00500050u
#define FAKE_LEAF 0x00500060u
#define FAKE_OUTER 0x00500070u
#define FAKE_FORGETS_TO_POP 0x00500080u
#define FAKE_CALLS_UNMAPPED 0x00500090u
#define FAKE_BLOCKING 0x005000A0u
#define FAKE_CALLS_THUNK_CDECL 0x005000B0u
#define FAKE_RECURSE 0x005000C0u
#define FAKE_WNDPROC 0x005000D0u
#define FAKE_ENUMCB 0x005000E0u
#define FAKE_WAITER 0x005000F0u

typedef struct {
    uint32_t va;
    void (*fn)(void);
} fake_entry_t;

extern const fake_entry_t g_fake_table[];
extern const unsigned g_fake_table_count;

/* A window procedure: records every message it gets; WM_NCCREATE answers 1, WM_USER (0x400) answers lparam + 1, the rest 0. */
#define FAKE_WNDPROC_LOG 256
extern uint32_t g_fake_wp_hwnd[FAKE_WNDPROC_LOG], g_fake_wp_msg[FAKE_WNDPROC_LOG], g_fake_wp_wparam[FAKE_WNDPROC_LOG], g_fake_wp_lparam[FAKE_WNDPROC_LOG];
extern unsigned g_fake_wp_count;
/* A (pointer, context) stdcall callback that counts its calls and answers 1; the first pointer argument's first dword is saved. */
extern unsigned g_fake_enum_count;
extern uint32_t g_fake_enum_first_dword[64];

/* The thunks the fake guest code calls, set by the test. */
extern uint32_t g_fake_callback_thunk, g_fake_add_thunk, g_fake_block_thunk, g_fake_cdecl_thunk, g_fake_wait_thunk;

#endif
