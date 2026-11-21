/* The game's own EndModal and app-active flag, written through the guest memory of either host. Only the game's own
 * EndModal and quit flags are written. */
#include "guest_abi.h"

#define MANAGER_ACTIVE_OFFSET 0x04u
#define MANAGER_MENU_FLAG_OFFSET 0x20u
/* CUIWnd::EndModal(code) sets the done byte at +8 and the result at +0xC; every window class uses it. */
#define END_MODAL_SLOT 0x4Cu
#define END_MODAL_FN GVA(0x004176F0)
#define WND_DONE_OFFSET 0x08u
#define WND_RESULT_OFFSET 0x0Cu
/* Set by the in-game Quit's "to Windows" answer: CGameWnd's close then skips the menu's display mode. */
#define QUITTING_TO_WINDOWS GVA(0x0066DE4C)

int guest_ui_active(const guest_ui_view_t* v) {
    if (!v->manager || !guest_readable(v->manager + MANAGER_ACTIVE_OFFSET, 1)) return 0;
    return MEM8(v->manager + MANAGER_ACTIVE_OFFSET) != 0;
}

int guest_ui_end_window(const guest_ui_view_t* v, int32_t code, int quit_to_windows) {
    uint32_t end_modal;
    if (!v->top || !guest_read32(v->top_vtable + END_MODAL_SLOT, &end_modal) || end_modal != END_MODAL_FN ||
        !guest_writable(v->top + WND_DONE_OFFSET, 8))
        return 0;
    if (MEM8(v->top + WND_DONE_OFFSET)) return -1;
    if (quit_to_windows) {
        if (!guest_writable(v->manager + MANAGER_MENU_FLAG_OFFSET, 4) || !guest_writable(QUITTING_TO_WINDOWS, 4)) return 0;
        MEM32(v->manager + MANAGER_MENU_FLAG_OFFSET) = 1;
        MEM32(QUITTING_TO_WINDOWS) = 1;
    }
    MEM32(v->top + WND_RESULT_OFFSET) = (uint32_t)code;
    MEM8(v->top + WND_DONE_OFFSET) = 1;
    return 1;
}
