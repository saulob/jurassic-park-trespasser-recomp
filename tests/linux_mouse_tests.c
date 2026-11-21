/* The mouse policy on Linux, with the virtual window backend recording what the host was asked to do (relative mode, the
 * pointer, a grab) and the game's window stack answered by a fake: the input context is what the game shows (a video, its
 * main menu, a dialog over the level, the level), never a guess from how the game moves its cursor. Menus, dialogs and videos
 * get the free pointer and absolute SDL positions; only the level takes relative mode and its motion; the game's ClipCursor and
 * SetCapture stay logical; a lost focus gives the pointer back and a regained one follows what the game shows now. */
#include <stdio.h>
#include <string.h>

#include "jpt_sync.h"
#include "linux_guest_ui.h"
#include "linux_mouse.h"
#include "linux_screen.h"
#include "linux_shutdown.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"

#define WS_STYLE 0x90080000u
#define WM_MOUSEMOVE_ 0x0200u
#define WM_LBUTTONDOWN_ 0x0201u

static uint32_t s_msg, s_rect;
static guest_ui_view_t s_view;
static int s_probe_answers = 1;

static int fake_probe(guest_ui_view_t* out) {
    *out = s_view;
    return s_probe_answers;
}

static void show(guest_ui_t ui, guest_wnd_t kind) {
    memset(&s_view, 0, sizeof s_view);
    s_view.ui = ui, s_view.kind = kind;
    s_view.top_vtable = 0x00645C3Cu;
}

static void create_main_window(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    uint32_t name = put_string(g_scratch, "DWI - Trespasser");
    wc[9] = name;
    gp_copy_out(g_scratch + 0x100, wc, sizeof wc);
    U32(RegisterClassA, 1, g_scratch + 0x100);
    U32(CreateWindowExA, 12, 0u, name, put_string(g_scratch + 0x200, "Trespasser"), WS_STYLE, 0u, 0u, 640u, 480u, 0u, 0u, 0x00400000u, 0u);
    s_msg = g_scratch + 0x800;
    s_rect = g_scratch + 0xA00;
    {
        uint32_t r[4] = { 0, 0, 640, 480 };
        gp_copy_out(s_rect, r, sizeof r);
    }
}

typedef struct {
    uint32_t message, lparam;
} seen_t;

static int pump(seen_t* seen, int capacity) {
    int n = 0;
    for (int i = 0; i < 64 && U32(PeekMessageA, 5, s_msg, 0u, 0u, 0u, 1u) == 1; i++) {
        if (seen && n < capacity) seen[n].message = rd(s_msg + 4), seen[n].lparam = rd(s_msg + 12), n++;
    }
    return n;
}

static void cursor(int* x, int* y) {
    uint32_t point = g_scratch + 0xB00;
    U32(GetCursorPos, 1, point);
    *x = (int)rd(point), *y = (int)rd(point + 4);
}

static void send_motion(float dx, float dy) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_MOUSE_MOVE, ev.relative = 1, ev.dx = dx, ev.dy = dy;
    lx_virtual_queue_event(&ev);
}

static void send_absolute(int x, int y) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_MOUSE_MOVE, ev.x = x, ev.y = y;
    lx_virtual_queue_event(&ev);
}

static void send_click(int x, int y) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_MOUSE_BUTTON, ev.vk = 1, ev.down = 1, ev.x = x, ev.y = y;
    lx_virtual_queue_event(&ev);
}

static void send_focus(int gained) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_FOCUS, ev.down = gained;
    lx_virtual_queue_event(&ev);
}

static lx_mouse_snapshot_t snap(void) {
    lx_mouse_snapshot_t s;
    lx_mouse_snapshot(&s);
    return s;
}

static void expect_free(const char* where, input_state_t state) {
    const lx_virtual_state_t* v = lx_virtual_state();
    lx_mouse_snapshot_t s = snap();
    CHECK(v->relative == 0 && v->grabbed == 0 && v->grab_calls == 0, "%s: the host pointer is free (relative %d, grabbed %d)", where, v->relative, v->grabbed);
    CHECK(s.state == state && !s.relative, "%s: the input state is %s (%s)", where, input_state_name(state), input_state_name(s.state));
}

static void test_video_and_menu(void) {
    int x, y;
    seen_t seen[16];
    lx_mouse_snapshot_t s;
    const lx_virtual_state_t* v = lx_virtual_state();
    U32(ClipCursor, 1, s_rect);
    U32(SetCapture, 1, lx_user32_main_window());
    U32(SetCursor, 1, 0x47F00u);
    U32(ShowCursor, 1, 0u);
    show(GUEST_UI_MENU, GUEST_WND_VIDEO);
    pump(NULL, 0);
    s = snap();
    expect_free("the intro video", INPUT_MENU);
    CHECK(s.guest_clip == 1 && s.guest_capture != 0, "the guest's clip and capture are kept as its own state");
    CHECK(s.game_cursor_hidden && s.cursor_hidden && v->cursor_visible == 0, "its hidden cursor hides the host pointer over the picture (it draws its own)");
    CHECK(s.ui == GUEST_UI_MENU && s.top_kind == GUEST_WND_VIDEO, "and the context is the video window the game shows");

    send_absolute(100, 80);
    send_motion(30.0f, 30.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 100 && y == 80, "during the video the pointer is absolute: the position follows it, relative motion is ignored (%d, %d)", x, y);

    send_focus(0);
    pump(NULL, 0);
    expect_free("focus lost during the video", INPUT_MENU);
    send_focus(1);
    pump(NULL, 0);
    expect_free("focus regained during the video", INPUT_MENU);

    show(GUEST_UI_MENU, GUEST_WND_MAIN_MENU);
    send_absolute(300, 200);
    pump(seen, 16);
    cursor(&x, &y);
    expect_free("the main menu after the video", INPUT_MENU);
    CHECK(x == 300 && y == 200, "the menu pointer follows an absolute SDL position (%d, %d)", x, y);
    CHECK(seen[0].message == WM_MOUSEMOVE_ && seen[0].lparam == ((200u << 16) | 300u), "and the game is told with WM_MOUSEMOVE at that position (0x%X)", seen[0].lparam);

    send_absolute(410, 150);
    send_click(410, 150);
    pump(seen, 16);
    cursor(&x, &y);
    CHECK(x == 410 && y == 150 && seen[1].message == WM_LBUTTONDOWN_ && seen[1].lparam == ((150u << 16) | 410u),
          "a click lands where the pointer moved to, not where it was (message 0x%X, 0x%X)", seen[1].message, seen[1].lparam);
    CHECK(v->relative_changes == 0, "relative mode was never switched on by any of it (%u changes)", v->relative_changes);

    send_absolute(0, 0);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 0 && y == 0, "the pointer reaches the corner of the picture (%d, %d)", x, y);
}

static void test_level_and_pause(void) {
    const lx_virtual_state_t* v = lx_virtual_state();
    lx_mouse_snapshot_t s;
    seen_t seen[16];
    int x, y, n;
    lx_screen_set_mode(512, 384, 16);
    U32(ClipCursor, 1, 0u);

    show(GUEST_UI_GAMEPLAY, GUEST_WND_GAME);
    pump(NULL, 0);
    s = snap();
    CHECK(s.state == INPUT_GAMEPLAY_CAPTURED && s.relative && v->relative == 1 && v->grabbed == 0,
          "the level in front: relative mode on, with no call from the game to its cursor at all (%s)", input_state_name(s.state));
    CHECK(s.cursor_hidden && v->cursor_visible == 0, "and the pointer is hidden");
    cursor(&x, &y);
    CHECK(x == 256 && y == 192, "the virtual cursor starts at the centre the game re-centres to (%d, %d)", x, y);

    send_motion(12.0f, -5.0f);
    n = pump(seen, 16);
    cursor(&x, &y);
    CHECK(x == 268 && y == 187, "SDL's relative motion moves it: mouse-look (%d, %d)", x, y);
    {
        int moves = 0;
        for (int i = 0; i < n; i++) moves += seen[i].message == WM_MOUSEMOVE_ && seen[i].lparam == ((187u << 16) | 268u);
        CHECK(moves == 1, "and the game reads it as one WM_MOUSEMOVE at that position (%d of %d messages)", moves, n);
    }
    send_motion(-3.0f, 9.0f);
    send_motion(-3.0f, 9.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 262 && y == 205, "motion adds up the way the counts arrive (%d, %d)", x, y);

    send_absolute(10, 10);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 262 && y == 205 && v->grab_calls == 0, "an absolute position is not a look, and the pointer is never grabbed");

    show(GUEST_UI_DIALOG, GUEST_WND_INGAME_OPTIONS);
    pump(NULL, 0);
    expect_free("the game's options over the level (pause)", INPUT_DIALOG);
    send_absolute(222, 111);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 222 && y == 111, "and its buttons follow the mouse (%d, %d)", x, y);
    send_motion(50.0f, 50.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 222 && y == 111, "relative motion does not move a dialog's pointer");

    show(GUEST_UI_GAMEPLAY, GUEST_WND_GAME);
    pump(NULL, 0);
    CHECK(snap().state == INPUT_GAMEPLAY_CAPTURED && v->relative == 1, "closing the dialog (resume): relative mode comes back");
    cursor(&x, &y);
    CHECK(x == 256 && y == 192, "with the cursor at the centre again (%d, %d)", x, y);
}

static void test_sensitivity(void) {
    int x, y;
    lx_mouse_set_sensitivity(2.0f);
    send_motion(3.0f, 1.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 262 && y == 194, "mouse sensitivity 2.00 doubles the look (%d, %d)", x, y);
    lx_mouse_set_sensitivity(0.5f);
    send_motion(1.0f, 1.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 262 && y == 194, "sensitivity 0.50 keeps the half count for the next motion (%d, %d)", x, y);
    send_motion(1.0f, 1.0f);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 263 && y == 195, "which makes a whole count with the next (%d, %d)", x, y);
    lx_mouse_set_sensitivity(1.0f);
    send_absolute(5, 5);
    pump(NULL, 0);
    cursor(&x, &y);
    CHECK(x == 263 && y == 195, "an absolute position stays no look at any sensitivity");
}

static void test_focus(void) {
    const lx_virtual_state_t* v = lx_virtual_state();
    lx_mouse_snapshot_t s;
    int x, y;
    send_focus(0);
    pump(NULL, 0);
    s = snap();
    CHECK(s.state == INPUT_GAMEPLAY_RELEASED && !s.relative && v->relative == 0, "focus lost in the level gives the pointer back (%s)", input_state_name(s.state));
    cursor(&x, &y);
    send_motion(50.0f, 50.0f);
    pump(NULL, 0);
    {
        int x2, y2;
        cursor(&x2, &y2);
        CHECK(x2 == x && y2 == y, "and no motion moves the view while the window is not focused");
    }
    send_focus(1);
    pump(NULL, 0);
    s = snap();
    CHECK(s.state == INPUT_GAMEPLAY_CAPTURED && s.relative, "focus regained: the level takes relative mode again");

    send_focus(0);
    pump(NULL, 0);
    show(GUEST_UI_MENU, GUEST_WND_MAIN_MENU);
    send_focus(1);
    pump(NULL, 0);
    expect_free("focus regained after the game went back to its menu meanwhile", INPUT_MENU);

    show(GUEST_UI_GAMEPLAY, GUEST_WND_GAME);
    pump(NULL, 0);
    CHECK(snap().state == INPUT_GAMEPLAY_CAPTURED, "back in the level: captured");
    lx_shutdown_request(LX_QUIT_WINDOW);
    pump(NULL, 0);
    CHECK(snap().state == INPUT_SHUTDOWN && !snap().relative && v->relative == 0, "a quit request frees the pointer for the game's way out");
}

static void test_no_stack_yet(void) {
    lx_mouse_reset();
    lx_shutdown_reset();
    show(GUEST_UI_NONE, GUEST_WND_NONE);
    s_probe_answers = 0;
    pump(NULL, 0);
    expect_free("before the game has built a window stack", INPUT_MENU);
    s_probe_answers = 1;
}

static void test_changes_are_few(void) {
    const lx_virtual_state_t* v = lx_virtual_state();
    CHECK(v->relative_changes <= 12, "the host is told of a change only when the state changes (%u relative changes in the whole run)", v->relative_changes);
}

int main(void) {
    if (!harness_start()) return 2;
    create_main_window();
    lx_guest_ui_set_probe(fake_probe);
    CHECK(lx_screen_window_is_open(), "the host window is open");
    test_video_and_menu();
    test_level_and_pause();
    test_sensitivity();
    test_focus();
    test_no_stack_yet();
    test_changes_are_few();
    lx_guest_ui_set_probe(NULL);
    harness_stop();
    printf("linux_mouse_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
