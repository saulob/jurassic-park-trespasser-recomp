/*
 * Which of the game's own windows is in front, read from its window stack: the gameplay view
 * (CGameWnd), a modal window over it (options, hint, controls, quit...) or a menu of its own.
 * Every pointer is checked first, so a half-built stack reads as "none". The only writes are the
 * game's own EndModal and quit flags, used by the host's close path.
 */
#include "host.h"

#include <ddraw.h>
#include <string.h>

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define MANAGER_OFFSET 0x08u
#define MANAGER_ACTIVE_OFFSET 0x04u
#define MANAGER_MENU_FLAG_OFFSET 0x20u
#define STACK_BEGIN_OFFSET 0x38u
#define STACK_END_OFFSET 0x3Cu
#define GAME_WND_VTABLE 0x00645C3Cu
#define GAME_WND_STOPPED_OFFSET 0x30u
#define MAX_STACK 32u
#define DISPLAY_OBJECT 0x0073DC40u
#define DISPLAY_RELEASED 0x00669B90u
/* CUIWnd::EndModal(code) sets the done byte at +8 and the result at +0xC; every window class uses it. */
#define END_MODAL_SLOT 0x4Cu
#define END_MODAL_FN 0x004176F0u
#define WND_DONE_OFFSET 0x08u
#define WND_RESULT_OFFSET 0x0Cu
/* Set by the in-game Quit's "to Windows" answer: CGameWnd's close then skips the menu's display mode. */
#define QUITTING_TO_WINDOWS 0x0066DE4Cu

static const struct {
    uint32_t vtable;
    const char* name;
    guest_wnd_t kind;
} k_classes[] = {
    { 0x00645C3Cu, "CGameWnd", GUEST_WND_GAME },
    { 0x00646B40u, "CInGameOptionsWnd", GUEST_WND_INGAME_OPTIONS },
    { 0x00646D10u, "CHintWnd", GUEST_WND_OTHER },
    { 0x00645F38u, "CControlsWnd", GUEST_WND_OTHER },
    { 0x00646970u, "CQuitWnd", GUEST_WND_QUIT },
    { 0x00645E24u, "CYesNoMsgDlg", GUEST_WND_OTHER },
    { 0x00645D34u, "CYesNo", GUEST_WND_OTHER },
    { 0x006461B8u, "CYesNoTextDlg", GUEST_WND_OTHER },
    { 0x00646390u, "COKDlg", GUEST_WND_OTHER },
    { 0x00646480u, "CMsgOkDlg", GUEST_WND_OTHER },
    { 0x006466B8u, "CMainScreenWnd", GUEST_WND_MAIN_MENU },
    { 0x006467A0u, "CLoaderWnd", GUEST_WND_LOADER },
    { 0x00646FC8u, "CVideoWnd", GUEST_WND_VIDEO },
    { 0x00646A58u, "COptionsWnd", GUEST_WND_OTHER },
    { 0x006462A8u, "CSaveGameWnd", GUEST_WND_OTHER },
    { 0x006460D0u, "CLoadGameWnd", GUEST_WND_OTHER },
    { 0x00646C28u, "CAudioWnd", GUEST_WND_OTHER },
    { 0x00645B4Cu, "CRenderWnd", GUEST_WND_OTHER },
    { 0x00646888u, "CDirectLoadWnd", GUEST_WND_LOADER },
};

const char* guest_ui_class_name(uint32_t vtable) {
    for (size_t i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++)
        if (k_classes[i].vtable == vtable) return k_classes[i].name;
    return "a window of the game";
}

static guest_wnd_t class_kind(uint32_t vtable) {
    for (size_t i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++)
        if (k_classes[i].vtable == vtable) return k_classes[i].kind;
    return GUEST_WND_OTHER;
}

static int committed(uint32_t va, uint32_t bytes, DWORD wanted) {
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t lo = (uintptr_t)va, hi = lo + bytes;
    if (va < 0x10000u || hi < lo) return 0;
    if (!VirtualQuery((const void*)lo, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(mi.Protect & wanted))
        return 0;
    return hi <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static int readable(uint32_t va, uint32_t bytes) {
    static uintptr_t s_lo, s_hi;
    uintptr_t lo = (uintptr_t)va, hi = lo + bytes;
    MEMORY_BASIC_INFORMATION mi;
    if (va >= 0x10000u && hi >= lo && lo >= s_lo && hi <= s_hi) return 1;
    if (!committed(va, bytes, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
        return 0;
    VirtualQuery((const void*)lo, &mi, sizeof mi);
    s_lo = (uintptr_t)mi.BaseAddress;
    s_hi = s_lo + mi.RegionSize;
    return 1;
}

static int writable(uint32_t va, uint32_t bytes) {
    return committed(va, bytes, PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE);
}

static int read32(uint32_t va, uint32_t* out) {
    if (!readable(va, 4)) return 0;
    *out = *(const volatile uint32_t*)(uintptr_t)va;
    return 1;
}

int guest_read32(uint32_t va, uint32_t* out) { return read32(va, out); }

guest_wnd_t guest_ui_window_kind(uint32_t window) {
    uint32_t vtable;
    return window && read32(window, &vtable) ? class_kind(vtable) : GUEST_WND_NONE;
}

/* The game swaps its display object for an empty one while its window is inactive. */
int guest_display_built(void) {
    uint32_t display;
    return read32(DISPLAY_OBJECT, &display) && display && display != DISPLAY_RELEASED;
}

int guest_ui_probe(guest_ui_view_t* out) {
    uint32_t main_wnd, manager, begin, end, top, stopped;
    memset(out, 0, sizeof *out);
    out->ui = GUEST_UI_NONE;
    out->kind = GUEST_WND_NONE;
    if (!read32(MAIN_WINDOW_OBJECT, &main_wnd) || !main_wnd || !read32(main_wnd + MANAGER_OFFSET, &manager) || !manager)
        return 0;
    out->manager = manager;
    if (!read32(manager + STACK_BEGIN_OFFSET, &begin) || !read32(manager + STACK_END_OFFSET, &end) || end <= begin ||
        (end - begin) % 4 || (end - begin) / 4 > MAX_STACK)
        return 0;
    out->depth = (end - begin) / 4;
    if (!read32(end - 4, &top) || !top || !read32(top, &out->top_vtable)) return 0;
    out->top = top;
    out->kind = class_kind(out->top_vtable);
    if (out->top_vtable == GAME_WND_VTABLE) {
        if (!read32(top + GAME_WND_STOPPED_OFFSET, &stopped)) return 0;
        out->stopped = stopped != 0;
        out->ui = out->stopped ? GUEST_UI_DIALOG : GUEST_UI_GAMEPLAY;
        return 1;
    }
    out->ui = GUEST_UI_MENU;
    for (uint32_t i = 0; i + 1 < out->depth; i++) {
        uint32_t w, v;
        if (read32(begin + 4 * i, &w) && w && read32(w, &v) && v == GAME_WND_VTABLE) out->ui = GUEST_UI_DIALOG;
    }
    return 1;
}

int guest_ui_active(const guest_ui_view_t* v) {
    if (!v->manager || !readable(v->manager + MANAGER_ACTIVE_OFFSET, 1)) return 0;
    return *(const volatile uint8_t*)(uintptr_t)(v->manager + MANAGER_ACTIVE_OFFSET) != 0;
}

int guest_ui_end_window(const guest_ui_view_t* v, int32_t code, int quit_to_windows) {
    uint32_t end_modal;
    if (!v->top || !read32(v->top_vtable + END_MODAL_SLOT, &end_modal) || end_modal != END_MODAL_FN ||
        !writable(v->top + WND_DONE_OFFSET, 8))
        return 0;
    if (*(const volatile uint8_t*)(uintptr_t)(v->top + WND_DONE_OFFSET)) return -1;
    if (quit_to_windows) {
        if (!writable(v->manager + MANAGER_MENU_FLAG_OFFSET, 4) || !writable(QUITTING_TO_WINDOWS, 4)) return 0;
        *(volatile uint32_t*)(uintptr_t)(v->manager + MANAGER_MENU_FLAG_OFFSET) = 1;
        *(volatile uint32_t*)(uintptr_t)QUITTING_TO_WINDOWS = 1;
    }
    *(volatile int32_t*)(uintptr_t)(v->top + WND_RESULT_OFFSET) = code;
    *(volatile uint8_t*)(uintptr_t)(v->top + WND_DONE_OFFSET) = 1;
    return 1;
}

/* The game's IDirectDraw2 and IDirectDraw4 (0x006D0E48 / 0x006D0E4C) and its Direct3D driver object
 * (0x006D0E98). Its software fallback (0x00503D8D) releases the IDirectDraw4 and shuts the driver
 * down, and only DirectDraw start-up (sub_00505480) sets them up again. */
#define GAME_DDRAW2 0x006D0E48u
#define GAME_DDRAW4 0x006D0E4Cu
#define GAME_D3D_DRIVER 0x006D0E98u
#define D3D_DRIVER_INIT 0x00505AF0u

/* A lifted thiscall entered as Windows enters a window procedure: the execute fault on the guest
 * address takes it through pcrecomp's callback trampoline, which takes the machine lock. */
static uint32_t call_guest_method(uint32_t va, uint32_t self) {
    uint32_t result;
    __asm {
        mov ecx, self
        mov eax, va
        call eax
        mov result, eax
    }
    return result;
}

int guest_restore_direct3d(void) {
    uint32_t dd2, dd4;
    if (!read32(GAME_DDRAW2, &dd2) || !dd2 || !read32(GAME_DDRAW4, &dd4) || !writable(GAME_DDRAW4, 4)) return 0;
    if (!dd4) {
        IDirectDraw2* dd = (IDirectDraw2*)(uintptr_t)dd2;
        void* out = NULL;
        if (FAILED(dd->lpVtbl->QueryInterface(dd, &IID_IDirectDraw4, &out)) || !out) return 0;
        *(volatile uint32_t*)(uintptr_t)GAME_DDRAW4 = (uint32_t)(uintptr_t)out;
    }
    return (call_guest_method(D3D_DRIVER_INIT, GAME_D3D_DRIVER) & 0xFF) != 0;
}

/* Set by CGameWnd's constructor and read by the level loader (sub_004142C0 -> sub_00414200): the next
 * level load restarts DirectDraw, which reads "Use D3D" again. CGameWnd's stop clears it. */
#define DISPLAY_RESTART_ON_LOAD 0x0066E341u

int guest_request_display_restart(void) {
    if (!writable(DISPLAY_RESTART_ON_LOAD, 1)) return 0;
    if (*(volatile uint8_t*)(uintptr_t)DISPLAY_RESTART_ON_LOAD) return 1;
    *(volatile uint8_t*)(uintptr_t)DISPLAY_RESTART_ON_LOAD = 1;
    return 1;
}
