/* The Linux user32 layer, called the way the guest calls it. Windows are guest objects behind 32-bit tokens (slot and generation),
 * their procedures run through lx_call_guest (the fake window procedure of linux_fake_guest.c records every message), and the
 * host window is the virtual backend, so nothing here opens a window. */
#include <stdlib.h>
#include <unistd.h>

#include "jpt_sync.h"
#include "linux_screen.h"
#include "linux_shutdown.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"

enum {
    WM_CREATE = 1, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
    WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47,
    WM_NCCREATE = 0x81, WM_NCCALCSIZE = 0x83, WM_NCACTIVATE = 0x86, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_TIMER = 0x113,
    WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_USER = 0x400
};

#define CLASS_NAME "DWI - Trespasser"
#define WS_STYLE 0x90080000u

static uint32_t s_class_name, s_wndclass;

static void register_class(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    s_class_name = put_string(g_scratch, CLASS_NAME);
    wc[9] = s_class_name;
    s_wndclass = g_scratch + 0x100;
    gp_copy_out(s_wndclass, wc, sizeof wc);
}

static uint32_t create_window(void) {
    return U32(CreateWindowExA, 12, 0u, s_class_name, put_string(g_scratch + 0x200, "Trespasser"), WS_STYLE, 0u, 0u, 640u, 480u, 0u, 0u,
               0x00400000u, 0u);
}

static int logged(uint32_t msg) {
    for (unsigned i = 0; i < g_fake_wp_count && i < FAKE_WNDPROC_LOG; i++)
        if (g_fake_wp_msg[i] == msg) return 1;
    return 0;
}

static int logged_before(uint32_t a, uint32_t b) {
    int ia = -1, ib = -1;
    for (unsigned i = 0; i < g_fake_wp_count && i < FAKE_WNDPROC_LOG; i++) {
        if (ia < 0 && g_fake_wp_msg[i] == a) ia = (int)i;
        if (ib < 0 && g_fake_wp_msg[i] == b) ib = (int)i;
    }
    return ia >= 0 && ib >= 0 && ia < ib;
}

static void test_classes_and_messages(void) {
    uint32_t atom, again, a, b;
    register_class();
    CHECK(lx_user32_class_count() == 0, "no class before RegisterClassA");
    atom = U32(RegisterClassA, 1, s_wndclass);
    CHECK(atom >= 0xC000u, "RegisterClassA returns an atom (0x%X)", atom);
    CHECK(lx_user32_class_count() == 1 && lx_user32_class_wndproc(CLASS_NAME) == FAKE_WNDPROC, "the class keeps its window procedure");
    again = U32(RegisterClassA, 1, s_wndclass);
    CHECK(again == 0, "registering the same name again fails");
    a = U32(RegisterWindowMessageA, 1, put_string(g_scratch + 0x400, "commctrl_DragListMsg"));
    b = U32(RegisterWindowMessageA, 1, g_scratch + 0x400);
    CHECK(a >= 0xC000u && a == b, "RegisterWindowMessageA gives one id per name (0x%X, 0x%X)", a, b);
    CHECK(U32(RegisterWindowMessageA, 1, put_string(g_scratch + 0x440, "another")) != a, "different names, different ids");
}

static void test_window_creation(void) {
    uint32_t hwnd, rect = g_scratch + 0x600, h2;
    const lx_virtual_state_t* v;
    g_fake_wp_count = 0;
    hwnd = create_window();
    v = lx_virtual_state();
    CHECK(hwnd != 0 && hwnd < 0x01000000u, "CreateWindowExA returns a 32-bit token, not a host pointer (0x%08X)", hwnd);
    CHECK(lx_user32_window_exists(hwnd) && lx_user32_main_window() == hwnd && lx_user32_window_count() == 1, "the window exists and is the main one");
    CHECK(logged(WM_GETMINMAXINFO) && logged(WM_NCCREATE) && logged(WM_NCCALCSIZE) && logged(WM_CREATE) && logged(WM_SHOWWINDOW) &&
              logged(WM_WINDOWPOSCHANGING) && logged(WM_ACTIVATEAPP) && logged(WM_NCACTIVATE) && logged(WM_ACTIVATE) &&
              logged(WM_SETFOCUS) && logged(WM_WINDOWPOSCHANGED),
          "the creation and show sequence reaches the window procedure through lx_call_guest (%u messages)", g_fake_wp_count);
    CHECK(logged_before(WM_NCCREATE, WM_CREATE) && logged_before(WM_CREATE, WM_SHOWWINDOW) && logged_before(WM_ACTIVATE, WM_SETFOCUS),
          "in the order Windows sends them");
    CHECK(g_fake_wp_msg[0] == WM_GETMINMAXINFO && g_fake_wp_hwnd[2] == hwnd, "every message carries the window's token");
    CHECK(v->opened && v->visible && v->width == 640 && v->height == 480 && !strcmp(v->title, "Trespasser"),
          "the host window opened 640x480 titled Trespasser (%d, %dx%d, \"%s\")", v->opened, v->width, v->height, v->title);
    CHECK(U32(GetClientRect, 2, hwnd, rect) == 1 && rd(rect) == 0 && rd(rect + 4) == 0 && rd(rect + 8) == 640 && rd(rect + 12) == 480,
          "GetClientRect of the popup window is 640x480");
    CHECK(U32(GetWindowRect, 2, hwnd, rect) == 1 && rd(rect + 8) == 640 && rd(rect + 12) == 480, "GetWindowRect");
    CHECK(U32(GetWindowLongA, 2, hwnd, 0xFFFFFFF0u) == (WS_STYLE | 0u), "GWL_STYLE (0x%X)", U32(GetWindowLongA, 2, hwnd, 0xFFFFFFF0u));
    CHECK(U32(GetSystemMetrics, 1, 0u) == 640 && U32(GetSystemMetrics, 1, 1u) == 480, "SM_CXSCREEN/SM_CYSCREEN follow the guest's display mode");
    CHECK(U32(SendMessageA, 4, hwnd, (uint32_t)WM_USER, 0u, 41u) == 42, "SendMessageA runs the window procedure and returns its result");
    CHECK(U32(IsIconic, 1, hwnd) == 0 && U32(GetActiveWindow, 0) == hwnd && U32(GetForegroundWindow, 0) == hwnd, "the shown window is active");
    CHECK(U32(SetWindowTextA, 2, hwnd, put_string(g_scratch + 0x700, "Renamed")) == 1 && !strcmp(lx_virtual_state()->title, "Renamed"),
          "SetWindowTextA reaches the host window title");

    CHECK(U32(DestroyWindow, 1, hwnd) == 1 && !lx_user32_window_exists(hwnd) && lx_user32_window_count() == 0, "DestroyWindow frees the slot");
    CHECK(U32(GetClientRect, 2, hwnd, rect) == 0, "the stale token fails (GetClientRect)");
    CHECK(U32(SendMessageA, 4, hwnd, (uint32_t)WM_USER, 0u, 1u) == 0, "the stale token fails (SendMessageA)");
    CHECK(!lx_virtual_state()->opened, "the host window closed with the main window");
    h2 = create_window();
    CHECK(h2 != 0 && h2 != hwnd && (h2 & 0xFFu) == (hwnd & 0xFFu), "a new window reuses the slot with a new generation (0x%X after 0x%X)", h2, hwnd);
    CHECK(!lx_user32_window_exists(hwnd) && lx_user32_window_exists(h2), "the old token stays stale");
}

static void test_message_queue(void) {
    uint32_t hwnd = lx_user32_main_window(), msg = g_scratch + 0x800;
    unsigned before;
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_PAINT, "a shown window with an invalid client has a WM_PAINT waiting");
    CHECK(U32(DispatchMessageA, 1, msg) == 0, "DispatchMessageA of WM_PAINT reaches the procedure");
    CHECK(U32(UpdateWindow, 1, hwnd) == 1, "UpdateWindow");
    U32(ValidateRect, 2, hwnd, 0u);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 0, "nothing left once the client is valid");

    CHECK(lx_user32_post(hwnd, WM_USER, 7, 8) == 1 && lx_user32_queue_length() == 1, "a posted message is queued");
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 0u) == 1 && lx_user32_queue_length() == 1, "PeekMessageA without PM_REMOVE keeps it");
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg) == hwnd && rd(msg + 4) == WM_USER && rd(msg + 8) == 7 && rd(msg + 12) == 8,
          "PeekMessageA with PM_REMOVE returns hwnd, message, wParam, lParam");
    CHECK(lx_user32_queue_length() == 0, "and removes it");

    before = g_fake_wp_count;
    lx_user32_post(hwnd, WM_USER, 0, 99);
    U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u);
    CHECK(U32(DispatchMessageA, 1, msg) == 100 && g_fake_wp_count == before + 1, "DispatchMessageA calls the window procedure and returns its result");

    lx_user32_post(hwnd, WM_USER + 1, 0, 0);
    CHECK(U32(PeekMessageA, 5, msg, 0u, WM_USER, WM_USER, 1u) == 0 && lx_user32_queue_length() == 1, "the message filter applies");
    U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u);

    U32(PostQuitMessage, 1, 5u);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_QUIT && rd(msg + 8) == 5, "PostQuitMessage becomes WM_QUIT with the exit code");
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 0, "WM_QUIT is delivered once");
}

static void test_timers(void) {
    uint32_t hwnd = lx_user32_main_window(), msg = g_scratch + 0x800, id;
    int got = 0;
    id = U32(SetTimer, 4, hwnd, 5u, 10u, 0u);
    CHECK(id == 5, "SetTimer with a window keeps the id given");
    CHECK(U32(PeekMessageA, 5, msg, 0u, WM_TIMER, WM_TIMER, 1u) == 0, "a timer is not due at once");
    for (int i = 0; i < 100 && !got; i++) {
        jpt_sleep_ms(5);
        got = (int)U32(PeekMessageA, 5, msg, 0u, WM_TIMER, WM_TIMER, 1u);
    }
    CHECK(got && rd(msg) == hwnd && rd(msg + 8) == 5, "the timer elapsed and posted WM_TIMER with its id");
    CHECK(U32(KillTimer, 2, hwnd, 5u) == 1 && U32(KillTimer, 2, hwnd, 5u) == 0, "KillTimer stops it, once");
    jpt_sleep_ms(30);
    CHECK(U32(PeekMessageA, 5, msg, 0u, WM_TIMER, WM_TIMER, 1u) == 0, "no more WM_TIMER");
    CHECK(U32(SetTimer, 4, 0u, 0u, 50u, 0u) != 0, "a thread timer gets an id");
}

static void send_event(lx_event_kind_t kind, int x, int y, uint32_t vk, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.x = x, ev.y = y, ev.vk = vk, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static void test_input(void) {
    uint32_t hwnd = lx_user32_main_window(), msg = g_scratch + 0x800;
    while (U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u)) {
    }
    send_event(LX_EV_KEY, 0, 0, 'A', 1);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_KEYDOWN && rd(msg + 8) == 'A', "a backend key event becomes WM_KEYDOWN with the virtual key");
    CHECK((U32(GetAsyncKeyState, 1, (uint32_t)'A') & 0x8000u) != 0 && (U32(GetKeyState, 1, (uint32_t)'A') & 0x80u) != 0, "the key is down");
    send_event(LX_EV_KEY, 0, 0, 'A', 0);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_KEYUP && (rd(msg + 12) & 0xC0000000u) == 0xC0000000u,
          "WM_KEYUP carries the previous-state and transition bits");
    CHECK(U32(GetAsyncKeyState, 1, (uint32_t)'A') == 0, "the key is up");

    send_event(LX_EV_MOUSE_MOVE, 320, 240, 0, 0);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_MOUSEMOVE && rd(msg + 12) == ((240u << 16) | 320u),
          "mouse motion is delivered in the guest's client coordinates (lParam 0x%08X)", rd(msg + 12));
    send_event(LX_EV_MOUSE_BUTTON, 0, 0, 1, 1);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_LBUTTONDOWN && (rd(msg + 8) & 1) == 1, "left button down with MK_LBUTTON");
    send_event(LX_EV_MOUSE_BUTTON, 0, 0, 1, 0);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_LBUTTONUP, "left button up");
    jpt_sleep_ms(600);
    send_event(LX_EV_MOUSE_BUTTON, 0, 0, 1, 1);
    send_event(LX_EV_MOUSE_BUTTON, 0, 0, 1, 0);
    send_event(LX_EV_MOUSE_BUTTON, 0, 0, 1, 1);
    U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u);
    U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u);
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_LBUTTONDBLCLK, "a second click of a CS_DBLCLKS class is WM_LBUTTONDBLCLK");
    {
        uint32_t pt = g_scratch + 0x900;
        CHECK(U32(GetCursorPos, 1, pt) == 1 && rd(pt) == 320 && rd(pt + 4) == 240, "GetCursorPos follows the pointer");
    }
    lx_shutdown_reset();
    send_event(LX_EV_CLOSE, 0, 0, 0, 0);
    {
        int close_seen = 0;
        for (int i = 0; i < 16 && U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1; i++) close_seen |= rd(msg + 4) == WM_CLOSE;
        CHECK(!close_seen && lx_shutdown_state() == LX_QUIT_REQUESTED && lx_shutdown_reason() == LX_QUIT_WINDOW,
              "closing the host window is a shutdown request, not a guest message");
    }
    lx_shutdown_mark_guest_main();
    lx_shutdown_poll();
    CHECK(U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u) == 1 && rd(msg + 4) == WM_CLOSE && rd(msg) == hwnd, "the guest's main thread then delivers WM_CLOSE to the main window");
    lx_shutdown_reset();
}

static void test_cursor_and_clip(void) {
    uint32_t rect = g_scratch + 0xA00;
    uint32_t r[4] = { 0, 0, 640, 480 };
    gp_copy_out(rect, r, sizeof r);
    CHECK(U32(ClipCursor, 1, rect) == 1 && lx_virtual_state()->grabbed == 1, "ClipCursor grabs the host pointer");
    CHECK(U32(ClipCursor, 1, 0u) == 1 && lx_virtual_state()->grabbed == 0, "ClipCursor(NULL) releases it");
    CHECK(U32(ShowCursor, 1, 0u) == 0xFFFFFFFFu && lx_virtual_state()->cursor_visible == 0, "ShowCursor(FALSE) hides the host pointer");
    CHECK(U32(ShowCursor, 1, 1u) == 0 && lx_virtual_state()->cursor_visible == 1, "ShowCursor(TRUE) shows it");
    CHECK(U32(SetCapture, 1, lx_user32_main_window()) == 0 && U32(ReleaseCapture, 0) == 1, "SetCapture / ReleaseCapture");
    CHECK(U32(LoadIconA, 2, 0x400000u, 0x65u) != 0 && U32(LoadCursorA, 2, 0u, 32512u) != 0, "icons and cursors are tokens");
}

static void test_helpers(void) {
    uint32_t a = g_scratch + 0xB00, b = g_scratch + 0xB10, d = g_scratch + 0xB20;
    uint32_t text = g_scratch + 0xC00;
    U32(SetRect, 5, a, 0u, 0u, 100u, 100u);
    U32(SetRect, 5, b, 50u, 50u, 200u, 200u);
    CHECK(U32(IntersectRect, 3, d, a, b) == 1 && rd(d) == 50 && rd(d + 8) == 100, "IntersectRect");
    U32(SetRect, 5, b, 150u, 150u, 200u, 200u);
    CHECK(U32(IntersectRect, 3, d, a, b) == 0 && rd(d + 8) == 0, "IntersectRect of disjoint rectangles is empty");
    CHECK(U32(UnionRect, 3, d, a, b) == 1 && rd(d + 8) == 200, "UnionRect");
    CHECK(U32(PtInRect, 3, a, 10u, 10u) == 1 && U32(PtInRect, 3, a, 100u, 10u) == 0, "PtInRect excludes the right edge");
    U32(OffsetRect, 3, a, 5u, 7u);
    CHECK(rd(a) == 5 && rd(a + 4) == 7 && rd(a + 8) == 105, "OffsetRect");
    U32(InflateRect, 3, a, 1u, 2u);
    CHECK(rd(a) == 4 && rd(a + 4) == 5, "InflateRect");

    CHECK(U32(wsprintfA, 7, text, put_string(g_scratch + 0xD00, "%s=%d %04X %c|%-3d|"), put_string(g_scratch + 0xD80, "n"), 42u, 0xBEEu,
              (uint32_t)'x', 7u) == strlen("n=42 0BEE x|7  |") &&
              !strcmp((const char*)gm_ptr(text, 24), "n=42 0BEE x|7  |"),
          "wsprintfA formats %%s %%d %%04X %%c (\"%s\")", (const char*)gm_ptr(text, 24));
    CHECK(U32(LoadStringA, 4, 0x400000u, 1u, text, 64u) == 0, "LoadStringA without a resource directory gives 0");
    CHECK(U32(DrawTextExA, 6, 0x1234u, put_string(g_scratch + 0xE80, "x"), 1u, a, 0u, 0u) == 0, "DrawTextExA on a stale context fails");
    CHECK(U32(DrawTextExA, 6, 0x1234u, put_string(g_scratch + 0xE80, "x"), 1u, 0u, 0u, 0u) == 0, "DrawTextExA without a rectangle fails");
    CHECK(U32(DrawEdge, 4, 0x1234u, a, 0xAu, 0x200Fu) == 0 && rd(a) == 4, "DrawEdge on a stale context fails and leaves the rectangle");
    CHECK(U32(MessageBoxA, 4, 0u, put_string(g_scratch + 0xE00, "text"), put_string(g_scratch + 0xE40, "caption"), 4u) == 6, "MessageBoxA answers the default button of Yes/No");
}

int main(void) {
    if (!harness_start()) return 2;
    test_classes_and_messages();
    test_window_creation();
    test_message_queue();
    test_timers();
    test_input();
    test_cursor_and_clip();
    test_helpers();
    harness_stop();
    printf("linux_user32_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
