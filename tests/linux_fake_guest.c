/* Guest functions written the way the lifter writes them (the same macros: local registers, a dummy return address pushed
 * for every call, ret N as an adjustment of esp), so the call spine is tested against the real calling protocol. */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"

#include "linux_fake_guest.h"

uint32_t g_fake_callback_thunk, g_fake_add_thunk, g_fake_block_thunk, g_fake_cdecl_thunk, g_fake_wait_thunk;

#define GUEST_BEGIN(va)                                                                                               \
    int _fpu_cmp = 0;                                                                                                 \
    uint32_t _cf = 0;                                                                                                 \
    int _df = 1;                                                                                                      \
    uint32_t _flag_k = FK_NONE;                                                                                       \
    uint32_t _flag_a = 0, _flag_b = 0;                                                                                \
    uint32_t _itail_tgt = 0;                                                                                          \
    RECOMP_REGS_LOCALS                                                                                                \
    RECOMP_ENTER(va);
#define GUEST_RET(popped)                                                                                             \
    esp += 4 + (popped);                                                                                              \
    RECOMP_FLAGS_OUT();                                                                                               \
    {                                                                                                                 \
        RECOMP_REGS_OUT();                                                                                            \
        return;                                                                                                       \
    }

void fake_stdcall0(void) {
    GUEST_BEGIN(FAKE_STDCALL0)
    eax = 0x1234;
    GUEST_RET(0)
}

void fake_stdcall1_ptr(void) {
    GUEST_BEGIN(FAKE_STDCALL1_PTR)
    eax = MEM32(esp + 4) + 0x10;
    GUEST_RET(4)
}

void fake_stdcall3(void) {
    GUEST_BEGIN(FAKE_STDCALL3)
    eax = MEM32(esp + 4) * 100 + MEM32(esp + 8) * 10 + MEM32(esp + 12);
    GUEST_RET(12)
}

void fake_cdecl3(void) {
    GUEST_BEGIN(FAKE_CDECL3)
    eax = MEM32(esp + 4) + MEM32(esp + 8) + MEM32(esp + 12);
    GUEST_RET(0)
}

void fake_thiscall(void) {
    GUEST_BEGIN(FAKE_THISCALL)
    eax = MEM32(ecx) + MEM32(esp + 4) + MEM32(esp + 8);
    ecx = 0xC0C0C0C0u;
    edx = 0xD0D0D0D0u;
    GUEST_RET(8)
}

void fake_clobber(void) {
    GUEST_BEGIN(FAKE_CLOBBER)
    ebx = 0xB0B0B0B0u;
    esi = 0x51515151u;
    edi = 0xD1D1D1D1u;
    ebp = 0xBEBEBEBEu;
    eax = 7;
    GUEST_RET(0)
}

void fake_leaf(void) {
    GUEST_BEGIN(FAKE_LEAF)
    PUSH32(esp, MEM32(esp + 4));
    RECOMP_ICALL(g_fake_add_thunk);
    GUEST_RET(4)
}

void fake_outer(void) {
    GUEST_BEGIN(FAKE_OUTER)
    ebx = 0x0B0B0B0Bu;
    PUSH32(esp, MEM32(esp + 4));
    PUSH32(esp, FAKE_LEAF);
    RECOMP_ICALL(g_fake_callback_thunk);
    eax += ebx;
    GUEST_RET(4)
}

void fake_forgets_to_pop(void) {
    GUEST_BEGIN(FAKE_FORGETS_TO_POP)
    eax = 1;
    GUEST_RET(0)
}

void fake_calls_unmapped(void) {
    GUEST_BEGIN(FAKE_CALLS_UNMAPPED)
    RECOMP_ICALL(MEM32(esp + 4));
    GUEST_RET(4)
}

void fake_blocking(void) {
    GUEST_BEGIN(FAKE_BLOCKING)
    ebx = MEM32(esp + 4);
    esi = ebx + 1;
    RECOMP_ICALL(g_fake_block_thunk);
    eax = (ebx == MEM32(esp + 4) && esi == ebx + 1) ? ebx : 0xDEADu;
    GUEST_RET(4)
}

void fake_calls_thunk_cdecl(void) {
    GUEST_BEGIN(FAKE_CALLS_THUNK_CDECL)
    PUSH32(esp, 6);
    PUSH32(esp, 5);
    RECOMP_ICALL(g_fake_cdecl_thunk);
    esp += 8;
    GUEST_RET(0)
}

void fake_recurse(void) {
    GUEST_BEGIN(FAKE_RECURSE)
    if (MEM32(esp + 4) == 0) {
        eax = 0;
        GUEST_RET(4)
    }
    PUSH32(esp, MEM32(esp + 4) - 1);
    RECOMP_ICALL(FAKE_RECURSE);
    eax += 1;
    GUEST_RET(4)
}

uint32_t g_fake_wp_hwnd[FAKE_WNDPROC_LOG], g_fake_wp_msg[FAKE_WNDPROC_LOG], g_fake_wp_wparam[FAKE_WNDPROC_LOG], g_fake_wp_lparam[FAKE_WNDPROC_LOG];
unsigned g_fake_wp_count;
unsigned g_fake_enum_count;
uint32_t g_fake_enum_first_dword[64];
uint32_t g_fake_enum_device[5];

void fake_wndproc(void) {
    GUEST_BEGIN(FAKE_WNDPROC)
    if (g_fake_wp_count < FAKE_WNDPROC_LOG) {
        g_fake_wp_hwnd[g_fake_wp_count] = MEM32(esp + 4);
        g_fake_wp_msg[g_fake_wp_count] = MEM32(esp + 8);
        g_fake_wp_wparam[g_fake_wp_count] = MEM32(esp + 12);
        g_fake_wp_lparam[g_fake_wp_count] = MEM32(esp + 16);
    }
    g_fake_wp_count++;
    eax = MEM32(esp + 8) == 0x81 ? 1 : MEM32(esp + 8) == 0x400 ? MEM32(esp + 16) + 1 : 0;
    GUEST_RET(16)
}

void fake_enumcb(void) {
    GUEST_BEGIN(FAKE_ENUMCB)
    if (g_fake_enum_count < 64) g_fake_enum_first_dword[g_fake_enum_count] = MEM32(MEM32(esp + 4) + 12);
    g_fake_enum_count++;
    eax = 1;
    GUEST_RET(8)
}

void fake_enumdevice(void) {
    GUEST_BEGIN(FAKE_ENUMDEVICE)
    g_fake_enum_device[0]++;
    g_fake_enum_device[1] = MEM32(MEM32(esp + 16));
    g_fake_enum_device[2] = MEM32(MEM32(esp + 16) + 12);
    g_fake_enum_device[3] = MEM32(esp + 24);
    g_fake_enum_device[4] = MEM32(MEM32(esp + 20));
    eax = 1;
    GUEST_RET(24)
}

/* A thread procedure that waits forever on the event handle it is given. */
void fake_waiter(void) {
    GUEST_BEGIN(FAKE_WAITER)
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, MEM32(esp + 8));
    RECOMP_ICALL(g_fake_wait_thunk);
    GUEST_RET(4)
}

const fake_entry_t g_fake_table[] = {
    { FAKE_STDCALL0, fake_stdcall0 },       { FAKE_STDCALL1_PTR, fake_stdcall1_ptr },
    { FAKE_STDCALL3, fake_stdcall3 },       { FAKE_CDECL3, fake_cdecl3 },
    { FAKE_THISCALL, fake_thiscall },       { FAKE_CLOBBER, fake_clobber },
    { FAKE_LEAF, fake_leaf },               { FAKE_OUTER, fake_outer },
    { FAKE_FORGETS_TO_POP, fake_forgets_to_pop }, { FAKE_CALLS_UNMAPPED, fake_calls_unmapped },
    { FAKE_BLOCKING, fake_blocking },       { FAKE_CALLS_THUNK_CDECL, fake_calls_thunk_cdecl },
    { FAKE_RECURSE, fake_recurse },          { FAKE_WNDPROC, fake_wndproc },
    { FAKE_ENUMCB, fake_enumcb },
    { FAKE_WAITER, fake_waiter },
    { FAKE_ENUMDEVICE, fake_enumdevice },
};
const unsigned g_fake_table_count = sizeof g_fake_table / sizeof g_fake_table[0];
