#include "linux_guest_quit.h"

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_va.h"
#include "linux_log.h"
#include "linux_user32.h"

#define MODAL_PUMP jpt_gva_or_canonical(GVAI_0x00417760)
#define VIDEO_PLAYER jpt_gva_or_canonical(GVAI_0x00418AE0)
#define MAIN_WINDOW_OBJECT jpt_gva_or_canonical(GVAI_0x0066E010)
#define QUITTING_TO_WINDOWS jpt_gva_or_canonical(GVAI_0x0066DE4C)
#define END_MODAL_FN jpt_gva_or_canonical(GVAI_0x004176F0)
#define END_MODAL_SLOT 0x4Cu
#define MANAGER_OFFSET 0x08u
#define MANAGER_ACTIVE_OFFSET 0x04u
#define MANAGER_MENU_FLAG_OFFSET 0x20u
#define STACK_BEGIN_OFFSET 0x38u
#define STACK_END_OFFSET 0x3Cu
#define WND_DONE_OFFSET 0x08u
#define WND_RESULT_OFFSET 0x0Cu
#define MAX_STACK 32u
#define QUIT_REACTIVATIONS 3u
#define WM_ACTIVATEAPP_MESSAGE 0x1Cu

typedef enum { WND_OTHER, WND_GAME, WND_INGAME_OPTIONS, WND_QUIT, WND_MAIN_MENU, WND_VIDEO, WND_LOADER } wnd_kind_t;

static const struct {
    unsigned id;
    wnd_kind_t kind;
} k_classes[] = {
    { GVAI_0x00645C3C, WND_GAME }, { GVAI_0x00646B40, WND_INGAME_OPTIONS }, { GVAI_0x00646970, WND_QUIT },
    { GVAI_0x006466B8, WND_MAIN_MENU }, { GVAI_0x00646FC8, WND_VIDEO }, { GVAI_0x006467A0, WND_LOADER },
};

static unsigned s_ended, s_reactivations;

void lx_guest_quit_reset(void) { s_ended = s_reactivations = 0; }
unsigned lx_guest_quit_windows_ended(void) { return s_ended; }

static int read32(uint32_t va, uint32_t* out) { return gp_read_u32(va, out); }

static wnd_kind_t kind_of(uint32_t vtable) {
    for (unsigned i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++)
        if (jpt_gva_or_canonical(k_classes[i].id) == vtable) return k_classes[i].kind;
    return WND_OTHER;
}

typedef struct {
    uint32_t top, top_vtable, manager;
} view_t;

static int probe(view_t* v) {
    uint32_t main_wnd, begin, end;
    if (!read32(MAIN_WINDOW_OBJECT, &main_wnd) || !main_wnd || !read32(main_wnd + MANAGER_OFFSET, &v->manager) || !v->manager) return 0;
    if (!read32(v->manager + STACK_BEGIN_OFFSET, &begin) || !read32(v->manager + STACK_END_OFFSET, &end) || end <= begin || (end - begin) % 4 ||
        (end - begin) / 4 > MAX_STACK)
        return 0;
    return read32(end - 4, &v->top) && v->top && read32(v->top, &v->top_vtable);
}

static int manager_active(const view_t* v) {
    const uint8_t* flag = (const uint8_t*)gm_ptr_checked(v->manager + MANAGER_ACTIVE_OFFSET, 1, 0);
    return flag && *flag;
}

/* 1 done, -1 already ending, 0 not possible. */
static int end_window(const view_t* v, int32_t code, int to_windows) {
    uint32_t end_modal;
    uint8_t* done = (uint8_t*)gm_ptr_checked(v->top + WND_DONE_OFFSET, 8, 1);
    if (!read32(v->top_vtable + END_MODAL_SLOT, &end_modal) || end_modal != END_MODAL_FN || !done) return 0;
    if (*done) return -1;
    if (to_windows) {
        uint32_t* menu_flag = (uint32_t*)gm_ptr_checked(v->manager + MANAGER_MENU_FLAG_OFFSET, 4, 1);
        uint32_t* quitting = (uint32_t*)gm_ptr_checked(QUITTING_TO_WINDOWS, 4, 1);
        if (!menu_flag || !quitting) return 0;
        *menu_flag = 1;
        *quitting = 1;
    }
    *(int32_t*)(done + WND_RESULT_OFFSET - WND_DONE_OFFSET) = code;
    *done = 1;
    return 1;
}

lx_quit_step_t lx_guest_quit_step(uint32_t pumping_function) {
    view_t v;
    int at_video = pumping_function == VIDEO_PLAYER, ended = 0;
    int32_t code = 0;
    int to_windows = 0;
    if ((pumping_function != MODAL_PUMP && !at_video) || !probe(&v)) return LX_QUIT_STEP_NONE;
    if (!at_video && !manager_active(&v) && s_reactivations < QUIT_REACTIVATIONS) {
        s_reactivations++;
        lx_user32_send(lx_user32_main_window(), WM_ACTIVATEAPP_MESSAGE, 1, 0);
    }
    switch (at_video ? WND_VIDEO : kind_of(v.top_vtable)) {
    case WND_VIDEO:
        return at_video && kind_of(v.top_vtable) == WND_VIDEO ? LX_QUIT_STEP_SKIP_VIDEO : LX_QUIT_STEP_NONE;
    case WND_LOADER: return LX_QUIT_STEP_NONE;
    case WND_GAME: code = -1, to_windows = 1; break;
    case WND_MAIN_MENU:
    case WND_INGAME_OPTIONS: code = -1; break;
    case WND_QUIT: code = 1; break;
    default: code = 0; break;
    }
    ended = end_window(&v, code, to_windows);
    if (ended > 0) {
        s_ended++;
        lx_log("quit", "the game's window in front ended with EndModal(%d)%s", (int)code, to_windows ? " and the in-game Quit's flags" : "");
        return LX_QUIT_STEP_ENDED_WINDOW;
    }
    return LX_QUIT_STEP_NONE;
}
