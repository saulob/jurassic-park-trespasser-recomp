/*
 * The game treats its window as the whole screen. Here that window is an ordinary resizable one,
 * and the game keeps seeing the virtual display as a screen of its own: client == screen ==
 * (0,0)-(width,height). Pictures are shown by a mouse-transparent child window with the display's
 * aspect ratio; the rest of the client is black. A change of the game's screen size takes effect when the first
 * frame of the new screen is presented (window_host_frame_commit), never before.
 *
 * The game's cursor is virtual: GetCursorPos/SetCursorPos read and write a position in display
 * pixels. In menus it follows the real cursor; in focused gameplay it moves by SDL's relative mouse
 * motion while SDL hides and confines the real cursor, and any other state frees it. The game's own
 * capture, ClipCursor and disabled Close are not applied.
 *
 * The window is also an SDL window (platform.c). The host's procedure stays in front of SDL's, so it
 * sees every message first; what it passes on goes through SDL to the game.
 *
 * Closing (Alt+F4, the X button, WM_CLOSE) leads the game out through its own quit path from inside
 * its message pump while the window stays up; WinMain then returns and exit_policy.c finishes.
 */
#include "guest_va.h"
#include "host.h"
#include "ddraw/renderer_backend.h"
#include "perf_sections.h"
#include "gamepad.h"
#include "host_policy.h"
#include "loading_hd.h"
#include "minimap.h"
#include "overlay_policy.h"
#include "physics_watch.h"
#include "platform.h"
#include "tutorial_prompts.h"
#include "window_title.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_monitors.h"
#include "jpt_tr.h"
#include "recomp_trace.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#define WM_MOUSEHWHEEL_ 0x020E
#define DPI_CONTEXT_PER_MONITOR_V2 ((HANDLE)(INT_PTR)-4)

#if defined(_M_X64) || defined(_M_AMD64)
#define HOST_ARCH "x64"
#else
#define HOST_ARCH "x86"
#endif

#define PRODUCT_TITLE "Jurassic Park: Trespasser Recompiled"
#define SURFACE_CLASS "TrespasserPresentation"
#define POLICY_TIMER_ID 0x5450
#define POLICY_INTERVAL_MS 250
#define INPUT_LOG_LIMIT 120
#define KEY_LOG_LIMIT 60
#define QUIT_LOG_LIMIT 24
#define MIN_CONTENT_WIDTH 320
#define BASE_DPI 96
#define CLIP_HOLD_MS 400
#define QUIT_REACTIVATIONS 3
#define MODAL_LOG_LIMIT 12
#define SHUTDOWN_POLL_MS 50
#define SHUTDOWN_TIMEOUT_MS 60000
/* The game functions whose PeekMessageA calls take a quit step: CUIWnd's modal pump, the only one
 * that returns to a loop checking EndModal, and the Smacker player's frame loop. Lifted calls push
 * a dummy return address, so the caller is the current lifted function. */
#define MODAL_PUMP GVA(0x00417760)
#define VIDEO_PLAYER GVA(0x00418AE0)
/* CUIWnd::DoModal's loop reads the pointer into its manager here before taking its messages, every frame. */
#define UI_LOOP GVA(0x00417840)
#define WM_HOST_PAD_MOUSE (WM_APP + 0x47)
/* Gamepad keys are posted with a scan code no keyboard sends, so they never count as keyboard use. */
#define PAD_SCAN_CODE 0xFEu
#define PAD_KEY_DOWN ((LPARAM)(1u | (PAD_SCAN_CODE << 16)))
#define PAD_KEY_UP ((LPARAM)(0xC0000001u | (PAD_SCAN_CODE << 16)))
#define WND_RESULT_OFFSET 0x0Cu

typedef BOOL (WINAPI* set_dpi_context_fn)(HANDLE);
typedef UINT (WINAPI* window_dpi_fn)(HWND);
typedef BOOL (WINAPI* adjust_for_dpi_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT (WINAPI* monitor_dpi_fn)(HMONITOR, int, UINT*, UINT*);

static struct {
    window_dpi_fn window_dpi;
    adjust_for_dpi_fn adjust;
    monitor_dpi_fn monitor_dpi;
} s_api;

static struct {
    HWND game, surface;
    WNDPROC next, game_proc;
    int fbw, fbh;
    int ui_canvas;
    int title_w[2], title_h[2];
    int virtual_on;
    RECT view;
    jpt_presentation_t pres;
    jpt_presentation_t logged;
    int fullscreen, active, focused, size_move, transition;
    input_state_t state;
    guest_ui_view_t guest;
    shutdown_state_t shutdown;
    int cursor_count;
    int relative, relative_refused;
    uint64_t relative_since;
    motion_counts_t motion;
    DWORD clip_hold_until;
    POINT last_move;
    int last_move_valid;
    POINT cursor;
    POINT pad_at;
    int pad_cursor, pad_focus;
    POINT menu_mouse;
    int menu_mouse_valid;
    float mouse_sensitivity;
    mouse_scale_t mouse_scale;
    HWND capture;
    unsigned long relative_moves, captures, dropped_repeats;
    int input_lines, key_lines, quit_lines, ignored_logged;
    int quit_reactivations, quit_video_logged;
    uint32_t quit_waiting_for;
    volatile uint32_t quit_front_vtable, quit_pump_func;
    volatile unsigned long quit_pump_calls;
    open_press_t open_press;
    activation_defer_t activation;
    int activation_logs;
    int modal_lines;
    WINDOWPLACEMENT saved_placement;
    LONG_PTR saved_style, saved_exstyle;
    LARGE_INTEGER qpc_frequency;
    fps_counter_t fps;
    int fps_ready, frame_counter;
    char title[160];
    struct {
        int borderless, monitor, window_w, window_h, stretch, show_fps, show_resolution;
        char settings_path[MAX_PATH];
    } cfg;
} s;

enum {
    IGNORED_STYLE = 1, IGNORED_POSITION = 2, IGNORED_TITLE = 4, IGNORED_CLIP = 8, IGNORED_CAPTURE = 16,
    IGNORED_MENU = 32, IGNORED_RESTORE = 64, IGNORED_CURSOR_POS = 128
};

static int64_t qpc_now(void);
static void update_input(const char* why);

void window_host_init(void) {
    HMODULE user = GetModuleHandleA("user32.dll");
    HMODULE shcore = LoadLibraryExA("shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    set_dpi_context_fn set_context = NULL;
    if (user) {
        set_context = (set_dpi_context_fn)GetProcAddress(user, "SetProcessDpiAwarenessContext");
        s_api.window_dpi = (window_dpi_fn)GetProcAddress(user, "GetDpiForWindow");
        s_api.adjust = (adjust_for_dpi_fn)GetProcAddress(user, "AdjustWindowRectExForDpi");
    }
    if (shcore) s_api.monitor_dpi = (monitor_dpi_fn)GetProcAddress(shcore, "GetDpiForMonitor");
    QueryPerformanceFrequency(&s.qpc_frequency);
    fps_counter_init(&s.fps, s.qpc_frequency.QuadPart, qpc_now());
    shutdown_init(&s.shutdown);
    s.state = INPUT_MENU;
    jpt_presentation_init(&s.pres, 0);
    s.cfg.monitor = 1;
    s.cfg.show_fps = 1;
    if (set_context && set_context(DPI_CONTEXT_PER_MONITOR_V2))
        host_log("window", "DPI awareness: per-monitor v2; window and cursor coordinates are physical pixels");
    else
        host_log("window", "per-monitor v2 DPI awareness is unavailable; Windows will scale the window");
}

void window_host_configure(int borderless, int monitor, int window_w, int window_h, int stretch, int show_fps,
                           int show_resolution, const char* settings_path) {
    s.cfg.borderless = borderless;
    s.cfg.monitor = monitor;
    s.cfg.window_w = window_w;
    s.cfg.window_h = window_h;
    s.cfg.stretch = stretch;
    s.pres.stretch = stretch != 0;
    s.cfg.show_fps = show_fps;
    s.cfg.show_resolution = show_resolution;
    strncpy(s.cfg.settings_path, settings_path ? settings_path : "", sizeof s.cfg.settings_path - 1);
    if (window_w > 0)
        host_log("window", "display: %s, monitor %d, window %dx%d, presentation %s, FPS in title %s, resolution in title %s",
                 borderless ? "borderless full screen" : "windowed", monitor, window_w, window_h,
                 stretch ? "stretch" : "letterbox", show_fps ? "on" : "off", show_resolution ? "on" : "off");
    else
        host_log("window", "display: %s, monitor %d, window size auto, presentation %s, FPS in title %s, resolution in title %s",
                 borderless ? "borderless full screen" : "windowed", monitor, stretch ? "stretch" : "letterbox",
                 show_fps ? "on" : "off", show_resolution ? "on" : "off");
}

int window_host_monitor_size(int monitor, int* width, int* height) {
    jpt_monitor_t m;
    if (!jpt_find_monitor(monitor, &m)) return 0;
    *width = m.bounds.right - m.bounds.left;
    *height = m.bounds.bottom - m.bounds.top;
    return 1;
}

static int fixed_window_size(void) { return s.cfg.window_w > 0 && s.cfg.window_h > 0; }

static UINT window_dpi(HWND window) {
    UINT dpi = s_api.window_dpi && window ? s_api.window_dpi(window) : 0;
    return dpi ? dpi : BASE_DPI;
}

static UINT monitor_dpi(HMONITOR monitor) {
    UINT x = 0, y = 0;
    if (s_api.monitor_dpi && SUCCEEDED(s_api.monitor_dpi(monitor, 0, &x, &y)) && x) return x;
    return BASE_DPI;
}

static void frame_extents(DWORD style, DWORD exstyle, UINT dpi, int* width, int* height) {
    RECT r = { 0, 0, 0, 0 };
    if (s_api.adjust) s_api.adjust(&r, style, FALSE, exstyle, dpi);
    else AdjustWindowRectEx(&r, style, FALSE, exstyle);
    *width = r.right - r.left;
    *height = r.bottom - r.top;
}

static long round_div(long a, long b) { return (a + b / 2) / b; }

static HMONITOR active_monitor(void) {
    HWND foreground = GetForegroundWindow();
    POINT cursor;
    if (foreground) return MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    GetCursorPos(&cursor);
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
}

static HMONITOR configured_monitor(void) {
    jpt_monitor_t m;
    if (jpt_find_monitor(s.cfg.monitor, &m)) return m.handle;
    return active_monitor();
}

/* Auto shows the content at the monitor's scale (640x480 at 100%); a chosen size is in pixels. Either is reduced,
 * keeping its shape, to fit the work area. */
static void initial_placement(int content_w, int content_h, DWORD style, DWORD exstyle, int* x, int* y, int* w, int* h) {
    MONITORINFO mi;
    HMONITOR monitor = configured_monitor();
    UINT dpi = monitor_dpi(monitor);
    int frame_w, frame_h;
    jpt_size_t client;
    long work_w, work_h;
    if (fixed_window_size()) {
        client.width = s.cfg.window_w;
        client.height = s.cfg.window_h;
    } else {
        client.width = MulDiv(content_w, (int)dpi, BASE_DPI);
        client.height = MulDiv(content_h, (int)dpi, BASE_DPI);
    }
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(monitor, &mi);
    frame_extents(style, exstyle, dpi, &frame_w, &frame_h);
    work_w = mi.rcWork.right - mi.rcWork.left;
    work_h = mi.rcWork.bottom - mi.rcWork.top;
    client = jpt_fit_window(client, frame_w, frame_h, (int)work_w, (int)work_h);
    *w = client.width + frame_w;
    *h = client.height + frame_h;
    *x = mi.rcWork.left + (int)((work_w - *w) / 2);
    *y = mi.rcWork.top + (int)((work_h - *h) / 2);
    host_log("window", "created at %d,%d, %dx%d client %dx%d, monitor %u dpi (%u%%)", *x, *y, *w, *h, client.width,
             client.height, dpi, dpi * 100 / BASE_DPI);
}

static int display_known(void) { return s.fbw > 0 && s.fbh > 0; }

static int active_w(void) { return s.pres.guest.width; }

static int active_h(void) { return s.pres.guest.height; }

static int virtual_window(HWND window) { return s.game && window == s.game && s.virtual_on && display_known(); }

static int view_valid(void) { return s.view.right > s.view.left && s.view.bottom > s.view.top; }

static int virtual_input_active(void) { return virtual_window(s.game) && view_valid() && !IsIconic(s.game); }

static int ui_canvas_active(void) { return s.ui_canvas && (s.state == INPUT_MENU || s.state == INPUT_DIALOG); }

/* The gamepad's pointer is in the game's own coordinates: the UI canvas while it is used, else the display. */
static POINT pad_cursor_on_display(void) {
    POINT d = s.pad_at;
    if (ui_canvas_active()) {
        jpt_ui_canvas_t canvas = jpt_ui_canvas(active_w(), active_h());
        int x, y;
        jpt_ui_to_frame(&canvas, (int)s.pad_at.x, (int)s.pad_at.y, &x, &y);
        d.x = x;
        d.y = y;
    }
    return d;
}

static POINT client_to_display(POINT c) {
    POINT v = { 0, 0 };
    int x, y;
    jpt_presentation_to_guest(&s.pres, (int)c.x, (int)c.y, &x, &y);
    v.x = x;
    v.y = y;
    if (ui_canvas_active()) {
        jpt_ui_canvas_t canvas = jpt_ui_canvas(active_w(), active_h());
        int x, y;
        jpt_ui_to_canvas(&canvas, (int)v.x, (int)v.y, &x, &y);
        v.x = x;
        v.y = y;
    }
    return v;
}

static POINT screen_to_client_point(POINT p) {
    POINT origin = { 0, 0 };
    ClientToScreen(s.game, &origin);
    p.x -= origin.x;
    p.y -= origin.y;
    return p;
}

static POINT screen_to_display(POINT p) { return client_to_display(screen_to_client_point(p)); }

static void paint_background(void) {
    HDC dc = GetDC(s.game);
    RECT r;
    if (!dc) return;
    GetClientRect(s.game, &r);
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ReleaseDC(s.game, dc);
}

static void log_presentation(void) {
    const jpt_presentation_t* p = &s.pres;
    if (p->host.width <= 0 || p->guest.width <= 0) return;
    if (s.logged.host.width == p->host.width && s.logged.host.height == p->host.height &&
        s.logged.guest.width == p->guest.width && s.logged.guest.height == p->guest.height &&
        !memcmp(&s.logged.view, &p->view, sizeof p->view))
        return;
    s.logged = *p;
    host_log("window", "Presentation: host %dx%d, guest %dx%d generation %u, viewport %dx%d at %d,%d (%s)", p->host.width,
             p->host.height, p->guest.width, p->guest.height, p->generation, p->view.right - p->view.left,
             p->view.bottom - p->view.top, p->view.left, p->view.top, p->stretch ? "Stretch" : "Letterbox");
}

static void layout_surface(void) {
    RECT client, view;
    if (!s.game || !GetClientRect(s.game, &client) || client.right <= 0 || client.bottom <= 0) return;
    jpt_presentation_set_host(&s.pres, client.right, client.bottom);
    SetRect(&view, s.pres.view.left, s.pres.view.top, s.pres.view.right, s.pres.view.bottom);
    if (EqualRect(&view, &s.view)) return;
    s.view = view;
    if (s.surface)
        SetWindowPos(s.surface, NULL, view.left, view.top, view.right - view.left, view.bottom - view.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    paint_background();
}

static int64_t qpc_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void view_rect_on_screen(RECT* r) {
    POINT origin = { 0, 0 };
    ClientToScreen(s.game, &origin);
    *r = s.view;
    OffsetRect(r, origin.x, origin.y);
}

static void input_log(const char* fmt, ...) {
    char line[256];
    va_list ap;
    if (s.input_lines >= INPUT_LOG_LIMIT) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    host_log("input", "%s", line);
    if (++s.input_lines == INPUT_LOG_LIMIT) host_log("input", "input line limit reached; later changes are not logged");
}

static void note_ignored(int what, const char* text) {
    if (s.ignored_logged & what) return;
    s.ignored_logged |= what;
    host_log("window", "%s (later identical calls are not logged)", text);
}

/* WindowFromPoint is a system call: its answer for an unmoved pointer is kept for POINTER_WINDOW_MS. */
#define POINTER_WINDOW_MS 50u

static int pointer_over_view(void) {
    static POINT s_at;
    static HWND s_window;
    static DWORD s_when;
    POINT p;
    RECT view;
    DWORD now;
    if (!s.game || !view_valid() || !GetCursorPos(&p)) return 0;
    view_rect_on_screen(&view);
    if (!PtInRect(&view, p)) return 0;
    now = GetTickCount();
    if (p.x != s_at.x || p.y != s_at.y || now - s_when >= POINTER_WINDOW_MS || !s_window) {
        s_window = WindowFromPoint(p);
        s_at = p;
        s_when = now;
    }
    return s_window == s.game || s_window == s.surface;
}

static int game_cursor_hidden(void) { return s.cursor_count < 0; }

static void show_client_cursor(void) {
    HCURSOR cursor = s.game ? (HCURSOR)GetClassLongPtrA(s.game, GCLP_HCURSOR) : NULL;
    SetCursor(cursor ? cursor : LoadCursorA(NULL, (LPCSTR)IDC_ARROW));
}

static int pointer_hidden(void) {
    if (debug_menu_visible()) return 0;
    return input_hide_pointer(s.state, game_cursor_hidden()) || input_pad_hides_pointer(s.state, s.pad_focus);
}

static void refresh_pointer_shape(void) {
    if (!pointer_over_view()) return;
    if (pointer_hidden()) SetCursor(NULL);
    else show_client_cursor();
}

/* Every change of mouse mode starts a new motion base: SDL moves the real cursor back when relative mode ends, and that
 * move must not become mouse-look. */
static void set_relative(int on) {
    s.last_move_valid = 0;
    memset(&s.motion, 0, sizeof s.motion);
    if (on == s.relative) return;
    if (on && !platform_relative_mouse(1)) {
        s.relative_refused = 1;
        host_log("input", "SDL relative mouse mode unavailable: mouse-look uses cursor motion confined to the picture");
        return;
    }
    if (!on) platform_relative_mouse(0);
    s.relative = on;
    if (on) s.relative_since = platform_ticks_ns();
}

static void release_mouse(void) {
    if (s.relative) set_relative(0);
    platform_confine_mouse(NULL);
}

/* Mouse-look exists only in INPUT_GAMEPLAY_CAPTURED: SDL's relative mode, or, where SDL refuses it, cursor motion with
 * the cursor confined to the picture. */
static void apply_mouse(void) {
    int hold = s.clip_hold_until && (LONG)(GetTickCount() - s.clip_hold_until) < 0;
    if (!hold) s.clip_hold_until = 0;
    if (!hold && view_valid() && GetForegroundWindow() == s.game &&
        input_should_clip(s.state, pointer_over_view(), platform_mouse_buttons_held())) {
        if (!s.relative && !s.relative_refused) set_relative(1);
        if (s.relative_refused) platform_confine_mouse(&s.view);
    } else {
        release_mouse();
    }
}

static void release_everything(void) {
    release_mouse();
    if (GetCapture() == s.game && s.game) ReleaseCapture();
}

void window_host_input_changed(void) { update_input("host menu"); }

void window_host_level_load_done(void) {
    int active;
    if (!s.game || !activation_after_load(&s.activation, &active)) return;
    host_log("focus", "deferred %s delivered after the level load", active ? "display restore" : "deactivation");
    CallWindowProcA(s.game_proc, s.game, WM_ACTIVATEAPP, active, 0);
}

void window_host_release_cursor(void) { release_everything(); }

static const char* ui_text(const guest_ui_view_t* g) {
    return g->top_vtable ? guest_ui_class_name(g->top_vtable) : "no window yet";
}

/* The result each window over the gameplay view closed with, for the first few. */
static void trace_modal(const guest_ui_view_t* before, const guest_ui_view_t* after) {
    uint32_t result;
    if (s.modal_lines >= MODAL_LOG_LIMIT) return;
    if (after->depth > before->depth && before->kind == GUEST_WND_GAME) {
        s.modal_lines++;
        input_log("%s modal begin", ui_text(after));
    } else if (after->depth < before->depth && before->top && after->kind == GUEST_WND_GAME &&
               guest_read32(before->top + WND_RESULT_OFFSET, &result)) {
        s.modal_lines++;
        input_log("%s modal end, EndModal value %d", ui_text(before), (int)result);
    }
}

static void update_input(const char* why) {
    input_signals_t signals;
    guest_ui_view_t guest;
    input_state_t next;
    int changed = 0;
    if (!s.game) return;
    guest_ui_probe(&guest);
    if (guest.top != s.guest.top) trace_modal(&s.guest, &guest);
    if (guest.top_vtable != s.guest.top_vtable || guest.stopped != s.guest.stopped || guest.ui != s.guest.ui) {
        if (guest.top_vtable == s.guest.top_vtable && guest.stopped != s.guest.stopped)
            input_log("game %s (%s)", guest.stopped ? "stopped its gameplay (SetupGameStoppage)"
                                                    : "resumed its gameplay (ClearGameStoppage)",
                      ui_text(&guest));
        else if (guest.depth > s.guest.depth && s.guest.top_vtable)
            input_log("game opened %s over %s (StartUIWnd), stack depth %u", ui_text(&guest), ui_text(&s.guest), guest.depth);
        else
            input_log("game window in front: %s, stack depth %u", ui_text(&guest), guest.depth);
        s.guest = guest;
    }
    signals.shutdown = shutdown_active(&s.shutdown);
    signals.transition = s.transition;
    signals.size_move = s.size_move;
    signals.focused = s.active && s.focused && GetForegroundWindow() == s.game;
    signals.minimized = IsIconic(s.game) != 0;
    signals.ui = guest.ui == GUEST_UI_GAMEPLAY && debug_menu_visible() ? GUEST_UI_MENU : guest.ui;
    next = input_state_from(&signals);
    if (next != s.state) {
        input_log("state %s -> %s (%s)", input_state_name(s.state), input_state_name(next), why);
        if (input_captured(next)) {
            s.captures++;
            s.clip_hold_until = 0;
        }
        pointer_recapture(s.state, next, &s.cursor.x, &s.cursor.y, active_w(), active_h());
        memset(&s.mouse_scale, 0, sizeof s.mouse_scale);
        memset(&s.motion, 0, sizeof s.motion);
        s.last_move_valid = 0;
        s.menu_mouse_valid = 0;
        if (input_cursor_virtual(next)) s.pad_cursor = 0;
        s.state = next;
        changed = 1;
    }
    if (gamepad_menu_focus() != s.pad_focus) {
        s.pad_focus = !s.pad_focus;
        changed = 1;
    }
    apply_mouse();
    /* After the mouse mode: SDL sets its own cursor when relative mode changes. */
    if (changed) refresh_pointer_shape();
}

static const char* renderer_text(void) { return d3dr_name(); }

/* The selected resolution of the current renderer; the game's screen only when none was given. */
static int title_size(int* width, int* height) {
    int hw = d3dr_hardware() ? 1 : 0;
    *width = s.title_w[hw] > 0 ? s.title_w[hw] : s.fbw;
    *height = s.title_w[hw] > 0 ? s.title_h[hw] : s.fbh;
    return *width > 0 && *height > 0;
}

static void refresh_title(void) {
    char text[sizeof s.title], base[sizeof s.title];
    title_content_t content;
    memset(&content, 0, sizeof content);
    _snprintf(base, sizeof base - 1, PRODUCT_TITLE " | " HOST_ARCH " | %s", renderer_text());
    base[sizeof base - 1] = 0;
    content.base = base;
    content.show_resolution = s.cfg.show_resolution;
    title_size(&content.width, &content.height);
    content.show_fps = s.cfg.show_fps;
    content.fps_ready = s.fps_ready;
    content.fps = s.fps.fps;
    title_compose(&content, text, sizeof text);
    if (!s.game || !strcmp(text, s.title)) return;
    strcpy(s.title, text);
    DefWindowProcA(s.game, WM_SETTEXT, 0, (LPARAM)text);
}

/* The only frame source: a successful IDXGISwapChain::Present, whatever the renderer. */
void window_host_frame_presented(void) {
    if (fps_counter_present(&s.fps, qpc_now())) {
        s.fps_ready = 1;
        refresh_title();
    }
    update_input("frame");
}

void window_host_set_ui_canvas(int on) { s.ui_canvas = on != 0; }

int window_host_pointer(int* x, int* y) {
    POINT p, c;
    if (!virtual_input_active() || input_cursor_virtual(s.state)) return 0;
    if (s.pad_cursor) {
        p = pad_cursor_on_display();
        *x = (int)p.x;
        *y = (int)p.y;
        return *x >= 0 && *y >= 0 && *x < active_w() && *y < active_h();
    }
    if (!GetCursorPos(&p)) return 0;
    c = screen_to_client_point(p);
    return jpt_presentation_to_guest(&s.pres, (int)c.x, (int)c.y, x, y);
}

void window_host_display_size(int* width, int* height) {
    *width = s.virtual_on ? s.fbw : 0;
    *height = s.virtual_on ? s.fbh : 0;
}

void window_host_set_title_sizes(int hardware_w, int hardware_h, int software_w, int software_h) {
    s.title_w[1] = hardware_w;
    s.title_h[1] = hardware_h;
    s.title_w[0] = software_w;
    s.title_h[0] = software_h;
    refresh_title();
}

int window_host_view_rect(RECT* view) {
    if (!view_valid()) return 0;
    *view = s.view;
    return 1;
}

void window_host_view_size(int* width, int* height) {
    jpt_rect_t next = jpt_presentation_next_view(&s.pres);
    *width = next.right > next.left ? next.right - next.left : 0;
    *height = next.bottom > next.top ? next.bottom - next.top : 0;
}

static void cover_monitor(void) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(s.game, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(s.game, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void enter_fullscreen(void) {
    s.saved_placement.length = sizeof s.saved_placement;
    GetWindowPlacement(s.game, &s.saved_placement);
    s.saved_style = GetWindowLongPtrA(s.game, GWL_STYLE);
    s.saved_exstyle = GetWindowLongPtrA(s.game, GWL_EXSTYLE);
    if (IsZoomed(s.game)) ShowWindow(s.game, SW_SHOWNORMAL);
    s.fullscreen = 1;
    SetWindowLongPtrA(s.game, GWL_STYLE, (s.saved_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
    cover_monitor();
    host_log("window", "borderless full screen (F11), desktop mode unchanged");
}

static void leave_fullscreen(void) {
    s.fullscreen = 0;
    SetWindowLongPtrA(s.game, GWL_STYLE, s.saved_style);
    SetWindowLongPtrA(s.game, GWL_EXSTYLE, s.saved_exstyle);
    SetWindowPlacement(s.game, &s.saved_placement);
    SetWindowPos(s.game, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    host_log("window", "windowed (F11), previous placement restored");
}

static void save_display_mode(void) {
    const char* key = JPT_KEY_DISPLAY_MODE;
    const char* value = s.fullscreen ? "borderless" : "windowed";
    if (!s.cfg.settings_path[0]) return;
    if (jpt_ini_set(s.cfg.settings_path, &key, &value, 1))
        host_log("window", "display_mode = %s saved to %s", value, s.cfg.settings_path);
    else
        host_log("window", "could not save display_mode to %s", s.cfg.settings_path);
}

void window_host_toggle_minimap(const char* source) { overlay_toggle_minimap(source, s.cfg.settings_path); }

void window_host_set_frame_counter(int on) { s.frame_counter = on != 0; }

void window_host_toggle_frame_counter(void) {
    s.frame_counter = !s.frame_counter;
    host_log("fps", "F6: frame counter %s", s.frame_counter ? "on" : "off");
}

int window_host_frame_counter_on(void) { return s.frame_counter; }

int window_host_frame_counter_text(char* out, size_t cap) {
    if (!s.frame_counter || !s.fps_ready || !cap) return 0;
    title_fps_text(s.fps.fps, out, cap);
    return 1;
}

static void toggle_fullscreen(void) {
    if (shutdown_active(&s.shutdown)) return;
    s.transition = 1;
    update_input("F11 transition");
    release_mouse();
    if (s.fullscreen) leave_fullscreen();
    else enter_fullscreen();
    layout_surface();
    present_set_fullscreen(s.fullscreen);
    s.transition = 0;
    update_input("F11 done");
    save_display_mode();
}

/* Keeps the client area at the display's aspect ratio while the user drags an edge. */
static void constrain_sizing(HWND window, WPARAM edge, RECT* r) {
    MONITORINFO mi;
    UINT dpi = window_dpi(window);
    int frame_w, frame_h;
    long cw, ch, max_w, max_h, min_w;
    if (active_w() <= 0 || fixed_window_size() || s.cfg.stretch) return;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
    frame_extents((DWORD)GetWindowLongPtrA(window, GWL_STYLE), (DWORD)GetWindowLongPtrA(window, GWL_EXSTYLE), dpi,
                  &frame_w, &frame_h);
    max_w = (mi.rcWork.right - mi.rcWork.left) - frame_w;
    max_h = (mi.rcWork.bottom - mi.rcWork.top) - frame_h;
    min_w = MulDiv(MIN_CONTENT_WIDTH, (int)dpi, BASE_DPI);
    cw = (r->right - r->left) - frame_w;
    ch = (r->bottom - r->top) - frame_h;
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = round_div(ch * active_w(), active_h());
    if (cw < min_w) cw = min_w;
    if (cw > max_w) cw = max_w;
    ch = round_div(cw * active_h(), active_w());
    if (ch > max_h) {
        ch = max_h;
        cw = round_div(ch * active_w(), active_h());
    }
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - (LONG)(cw + frame_w);
    else r->right = r->left + (LONG)(cw + frame_w);
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - (LONG)(ch + frame_h);
    else r->bottom = r->top + (LONG)(ch + frame_h);
}

static void cover_limits(HWND window, MINMAXINFO* info) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
    info->ptMaxPosition.x = mi.rcMonitor.left;
    info->ptMaxPosition.y = mi.rcMonitor.top;
    info->ptMaxSize.x = info->ptMaxTrackSize.x = mi.rcMonitor.right - mi.rcMonitor.left;
    info->ptMaxSize.y = info->ptMaxTrackSize.y = mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static void limit_minimum(HWND window, MINMAXINFO* info) {
    UINT dpi = window_dpi(window);
    int frame_w, frame_h;
    long min_w = MulDiv(MIN_CONTENT_WIDTH, (int)dpi, BASE_DPI);
    long min_h = active_w() > 0 ? round_div(min_w * active_h(), active_w()) : round_div(min_w * 3, 4);
    frame_extents((DWORD)GetWindowLongPtrA(window, GWL_STYLE), (DWORD)GetWindowLongPtrA(window, GWL_EXSTYLE), dpi,
                  &frame_w, &frame_h);
    info->ptMinTrackSize.x = (LONG)(min_w + frame_w);
    info->ptMinTrackSize.y = (LONG)(min_h + frame_h);
}

static LPARAM pack_point(POINT p) {
    if (p.x < -32768) p.x = -32768;
    if (p.x > 32767) p.x = 32767;
    if (p.y < -32768) p.y = -32768;
    if (p.y > 32767) p.y = 32767;
    return MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y);
}

/* The original ran full screen, so its cursor never left the display. */
static POINT game_cursor(void) {
    POINT real;
    if (!input_cursor_virtual(s.state) && s.pad_cursor) {
        s.cursor = s.pad_at;
        return s.cursor;
    }
    if (!input_cursor_virtual(s.state) && GetCursorPos(&real)) {
        s.cursor = screen_to_display(real);
        if (s.cursor.x < 0) s.cursor.x = 0;
        if (s.cursor.y < 0) s.cursor.y = 0;
        if (s.cursor.x >= active_w()) s.cursor.x = active_w() - 1;
        if (s.cursor.y >= active_h()) s.cursor.y = active_h() - 1;
    }
    return s.cursor;
}

static void add_relative(LONG dx, LONG dy) {
    if (pointer_add_relative(s.state, &s.cursor.x, &s.cursor.y, dx, dy)) s.relative_moves++;
}

/* The mouse's own motion; the gamepad's look has its own sensitivity and goes to add_relative directly. */
static void mouse_relative(LONG dx, LONG dy) {
    long x = dx, y = dy;
    if (input_captured(s.state)) mouse_scale_counts(&s.mouse_scale, s.mouse_sensitivity, dx, dy, &x, &y);
    add_relative((LONG)x, (LONG)y);
}

void window_host_set_mouse_sensitivity(float value) { s.mouse_sensitivity = jpt_mouse_sensitivity_clamp(value); }

void window_host_mouse_motion(float dx, float dy, uint64_t when_ns) {
    long x, y;
    if (!relative_motion_accepted(s.relative, when_ns, s.relative_since)) return;
    motion_counts(&s.motion, dx, dy, &x, &y);
    if (!x && !y) return;
    gamepad_note_mouse_motion((int)x, (int)y);
    mouse_relative(x, y);
}

int window_host_input_focused(void) {
    return s.game && s.active && s.focused && GetForegroundWindow() == s.game && !shutdown_active(&s.shutdown) &&
           !IsIconic(s.game);
}

void window_host_set_pad_cursor(int x, int y) {
    int w = ui_canvas_active() ? JPT_UI_CANVAS_W : active_w(), h = ui_canvas_active() ? JPT_UI_CANVAS_H : active_h();
    if (!display_known() || input_cursor_virtual(s.state)) return;
    s.pad_at.x = x < 0 ? 0 : x >= w ? w - 1 : x;
    s.pad_at.y = y < 0 ? 0 : y >= h ? h - 1 : y;
    s.pad_cursor = 1;
}

void window_host_release_pad_cursor(void) { s.pad_cursor = 0; }

void window_host_pad_relative(int dx, int dy) { add_relative(dx, dy); }

int window_host_game_cursor(int* x, int* y) {
    POINT p;
    if (!virtual_input_active()) return 0;
    p = game_cursor();
    *x = (int)p.x;
    *y = (int)p.y;
    return 1;
}

void window_host_post_key(unsigned vk) {
    if (!s.game) return;
    PostMessageA(s.game, WM_KEYDOWN, vk, PAD_KEY_DOWN);
    PostMessageA(s.game, WM_KEYUP, vk, PAD_KEY_UP);
}

void window_host_post_key_up(unsigned vk) {
    if (s.game) PostMessageA(s.game, WM_KEYUP, vk, PAD_KEY_UP);
}

void window_host_post_char(char c) {
    if (s.game) PostMessageA(s.game, WM_CHAR, (WPARAM)(unsigned char)c, PAD_KEY_DOWN);
}

void window_host_post_mouse(int kind) {
    if (s.game) PostMessageA(s.game, WM_HOST_PAD_MOUSE, (WPARAM)kind, 0);
}

/* A gamepad click or drag at the game's pointer, delivered as the mouse message its window expects. */
static LRESULT pad_mouse(HWND window, WPARAM kind) {
    static const UINT msgs[] = { WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_MOUSEMOVE };
    WPARAM keys = kind == PAD_MOUSE_UP ? 0 : MK_LBUTTON;
    if (kind > PAD_MOUSE_MOVE || !virtual_input_active()) return 0;
    update_input("gamepad");
    return CallWindowProcA(s.game_proc, window, msgs[kind], keys, pack_point(game_cursor()));
}

static int is_press(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ||
           msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK || msg == 0x020B || msg == 0x020D;
}

/* 1 when the message goes on to the game with lparam in display coordinates. */
static int route_mouse(UINT msg, LPARAM* lparam) {
    POINT client;
    client.x = (short)LOWORD(*lparam);
    client.y = (short)HIWORD(*lparam);
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL_) {
        *lparam = pack_point(input_cursor_virtual(s.state) ? s.cursor : screen_to_display(client));
        return 1;
    }
    update_input("mouse");
    if (msg == WM_MOUSEMOVE && !input_captured(s.state)) {
        if (s.menu_mouse_valid) gamepad_note_mouse_motion(client.x - s.menu_mouse.x, client.y - s.menu_mouse.y);
        s.menu_mouse = client;
        s.menu_mouse_valid = 1;
    }
    if (!s.relative && msg == WM_MOUSEMOVE && input_captured(s.state)) {
        if (s.last_move_valid) mouse_relative(client.x - s.last_move.x, client.y - s.last_move.y);
        s.last_move = client;
        s.last_move_valid = 1;
    }
    if (is_press(msg) && !PtInRect(&s.view, client)) return 0;
    if (is_press(msg)) gamepad_note_mouse_button();
    *lparam = pack_point(input_cursor_virtual(s.state) ? s.cursor : client_to_display(client));
    return 1;
}

static void log_key(UINT msg, WPARAM vk, LPARAM lparam, const char* fate) {
    const char* name = vk == VK_ESCAPE ? "Esc"
                       : vk == VK_F1   ? "F1"
                       : vk == VK_F2   ? "F2"
                       : vk == VK_F12  ? "F12"
                       : vk == VK_F11  ? "F11"
                                       : NULL;
    if (!name || s.key_lines >= KEY_LOG_LIMIT) return;
    if (++s.key_lines == KEY_LOG_LIMIT) {
        host_log("input", "key line limit reached");
        return;
    }
    host_log("input", "%s %s%s -> %s", name, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ? "keydown" : "keyup",
             (lparam & 0x40000000) && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? " (repeat)" : "", fate);
}

static void quit_log(const char* fmt, ...) {
    char line[256];
    va_list ap;
    if (s.quit_lines >= QUIT_LOG_LIMIT) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    host_log("shutdown", "%s", line);
    s.quit_lines++;
}

static void advance(shutdown_phase_t phase, const char* source) {
    if (shutdown_advance(&s.shutdown, phase, source))
        host_log("shutdown", "%s (close source: %s)", shutdown_phase_name(phase),
                 s.shutdown.source ? s.shutdown.source : "the game");
}

static void start_shutdown_watchdog(void);

/* The game greys out Close and ignores WM_CLOSE (its handler is a bare return). Nothing is torn
 * down here and the window stays up: input to the game stops, and the next pass through its modal
 * pump takes the first step of its own way out. */
static void request_close(HWND window, const char* source) {
    if (!shutdown_request(&s.shutdown, source)) return;
    host_log("window", "close requested: %s", source);
    release_everything();
    update_input("closing");
    guest_threads_log("before quit");
    CallWindowProcA(s.game_proc, window, WM_CLOSE, 0, 0);
    advance(SHUTDOWN_GUEST_QUIT_REQUESTED, source);
    start_shutdown_watchdog();
}

static void reactivate_for_quit(const guest_ui_view_t* v) {
    if (s.quit_reactivations >= QUIT_REACTIVATIONS || !s.game) return;
    s.quit_reactivations++;
    quit_log("the game is inactive (%s): reactivated so its pump returns to its loops", ui_text(v));
    activation_delivered(&s.activation, 1);
    CallWindowProcA(s.game_proc, s.game, WM_ACTIVATEAPP, TRUE, 0);
}

static void synthesize_skip(MSG* msg) {
    memset(msg, 0, sizeof *msg);
    msg->hwnd = s.game;
    msg->message = WM_KEYUP;
    msg->wParam = VK_ESCAPE;
    msg->lParam = (LPARAM)0xC0010001;
    msg->time = GetTickCount();
}

/* One step of the game's way out, taken where its loops read messages. 1 when msg holds a message. */
static int drive_quit(MSG* msg, uint32_t func) {
    guest_ui_view_t v;
    quit_action_t a;
    int at_video = func == VIDEO_PLAYER, done;
    if (!shutdown_host_driving(&s.shutdown) || (func != MODAL_PUMP && !at_video)) return 0;
    guest_ui_probe(&v);
    s.quit_front_vtable = v.top_vtable;
    s.quit_pump_func = func;
    s.quit_pump_calls++;
    if (!at_video && v.manager && !guest_ui_active(&v)) reactivate_for_quit(&v);
    a = quit_action_for(v.kind, at_video);
    switch (a.step) {
    case QUIT_SKIP_VIDEO:
        if (!s.quit_video_logged) quit_log("video playing (CVideoWnd): skipped with the key-up its player waits for");
        s.quit_video_logged = 1;
        advance(a.phase, NULL);
        synthesize_skip(msg);
        return 1;
    case QUIT_END_WINDOW:
        done = guest_ui_end_window(&v, a.code, a.quit_to_windows);
        if (done > 0) {
            s.quit_waiting_for = 0;
            s.quit_pump_calls = 0;
            shutdown_note_progress(&s.shutdown);
            quit_log("%s ended with EndModal(%d)%s", ui_text(&v), a.code,
                     a.quit_to_windows ? " and the in-game Quit's \"to Windows\" flags" : "");
            advance(a.phase, NULL);
        } else if (!done && s.quit_waiting_for != v.top_vtable) {
            s.quit_waiting_for = v.top_vtable;
            quit_log("%s cannot be ended through its EndModal; waiting", ui_text(&v));
        }
        return 0;
    default:
        if (s.quit_waiting_for != v.top_vtable + 1) {
            s.quit_waiting_for = v.top_vtable + 1;
            quit_log("waiting for %s to finish before the next quit step", ui_text(&v));
        }
        return 0;
    }
}

/* Progress of a host-led quit, checked from its own thread: a stall is reported within 250 ms with
 * where the game is, and a game that never leaves its loops ends the run with code 8, never 0. */
static DWORD WINAPI shutdown_watchdog(LPVOID unused) {
    unsigned last_progress = s.shutdown.progress, reports = 0;
    DWORD since = GetTickCount(), start = since;
    (void)unused;
    while (shutdown_guest_may_run(&s.shutdown)) {
        DWORD now;
        Sleep(SHUTDOWN_POLL_MS);
        now = GetTickCount();
        if (s.shutdown.progress != last_progress) {
            last_progress = s.shutdown.progress;
            since = now;
            reports = 0;
            continue;
        }
        if (now - start >= SHUTDOWN_TIMEOUT_MS) {
            diagnostics_shutdown_timeout("the game did not leave its loops within 60 s of the close request");
            return 0;
        }
        if (now - since < shutdown_stall_threshold(reports)) continue;
        reports++;
        host_log("shutdown", "no progress for %lu ms in %s: front window %s, lifted function sub_%08X, last native call "
                 "%s, quit pump %s%08X called %lu times since the last step", now - since,
                 shutdown_phase_name(s.shutdown.phase),
                 s.quit_front_vtable ? guest_ui_class_name(s.quit_front_vtable) : "not seen yet", g_cur_func,
                 g_cur_import ? g_cur_import : "none", s.quit_pump_func ? "sub_" : "none ", s.quit_pump_func,
                 s.quit_pump_calls);
    }
    return 0;
}

static void start_shutdown_watchdog(void) {
    HANDLE t = CreateThread(NULL, 0, shutdown_watchdog, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static LRESULT handle_key(HWND window, UINT msg, WPARAM wparam, LPARAM lparam, int* handled) {
    int repeat = (lparam & 0x40000000) != 0;
    int alt = (lparam & 0x20000000) != 0;
    int ctrl = GetKeyState(VK_CONTROL) < 0;
    key_action_t action = key_policy(msg, (unsigned)wparam, repeat, ctrl, alt);
    guest_ui_view_t g;
    *handled = 1;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !repeat &&
        (action == KEY_TO_GAME || action == KEY_HOST_CONTROLS || action == KEY_HOST_SHOW_OBJECTIVE) &&
        ((lparam >> 16) & 0xFF) != PAD_SCAN_CODE)
        gamepad_note_keyboard();
    if (overlay_key(msg, wparam, lparam)) {
        if (msg == WM_KEYDOWN && !repeat) log_key(msg, wparam, lparam, "host overlay");
        return 0;
    }
    if (action != KEY_HOST_MINIMAP && action != KEY_HOST_CHEATS && action != KEY_HOST_OBJECTIVES &&
        action != KEY_HOST_CONTROLS && action != KEY_HOST_FULLSCREEN &&
        action != KEY_HOST_CLOSE && action != KEY_HOST_SHOW_OBJECTIVE && action != KEY_HOST_FRAME_COUNTER &&
        debug_menu_key(msg, wparam, lparam)) {
        if (msg == WM_KEYDOWN && !repeat) log_key(msg, wparam, lparam, "host menu");
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP) guest_ui_probe(&g);
    if (msg == WM_KEYDOWN) open_press_keydown(&s.open_press, (unsigned)wparam, repeat, g.ui == GUEST_UI_GAMEPLAY, g.depth);
    if (msg == WM_KEYUP && open_press_keyup(&s.open_press, (unsigned)wparam, g.depth)) {
        log_key(msg, wparam, lparam, "kept from the window its own key-down opened");
        input_log("%s key-up of the press that opened %s consumed", wparam == VK_ESCAPE ? "Esc" : "F1", ui_text(&g));
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && screen_size_key_dropped((unsigned)wparam, g.ui == GUEST_UI_GAMEPLAY)) {
        if (msg == WM_KEYDOWN && !repeat) log_key(msg, wparam, lparam, "dropped (legacy Screen Size)");
        return 0;
    }
    if (input_captured(s.state) && key_opens_game_window(msg, (unsigned)wparam, repeat)) {
        s.clip_hold_until = GetTickCount() + CLIP_HOLD_MS;
        release_mouse();
    }
    switch (action) {
    case KEY_HOST_FULLSCREEN:
        log_key(msg, wparam, lparam, "host full screen toggle");
        toggle_fullscreen();
        return 0;
    case KEY_HOST_MINIMAP:
        window_host_toggle_minimap("F5");
        return 0;
    case KEY_HOST_FRAME_COUNTER:
        window_host_toggle_frame_counter();
        return 0;
    case KEY_HOST_CHEATS:
        overlay_request(OV_CHEATS, "F10");
        return 0;
    case KEY_HOST_CLOSE:
        request_close(window, "Alt+F4");
        return 0;
    case KEY_HOST_CONTROLS:
        log_key(msg, wparam, lparam, "host controls card (in a level)");
        overlay_request(OV_CONTROLS, "F2");
        return 0;
    case KEY_HOST_OBJECTIVES:
        overlay_request(OV_OBJECTIVES, "F3");
        return 0;
    case KEY_HOST_SNAPSHOT:
        physics_watch_request();
        return 0;
    case KEY_HOST_QUICK_SAVE:
        log_key(msg, wparam, lparam, "Quick Save");
        quick_save_request();
        return 0;
    case KEY_HOST_QUICK_LOAD:
        log_key(msg, wparam, lparam, "Quick Load");
        quick_load_request();
        return 0;
    case KEY_HOST_SHOW_OBJECTIVE:
        if (g.ui == GUEST_UI_GAMEPLAY && !debug_menu_open_menu() && !tutorial_prompts_card_shown())
            debug_menu_show_objective();
        else
            log_key(msg, wparam, lparam, "Show Objective outside gameplay: consumed");
        return 0;
    case KEY_DROP:
        if (repeat) s.dropped_repeats++;
        if (wparam == VK_F12) {
            if (msg == WM_KEYDOWN && !repeat) log_key(msg, wparam, lparam, "dropped (Controls is F2)");
        } else if (repeat && wparam != VK_F3 && wparam != VK_F4 && wparam != VK_F5 && wparam != VK_F9 &&
                   wparam != VK_F10 && wparam != VK_F11 && wparam != VK_F2 && wparam != VK_F6) {
            log_key(msg, wparam, lparam, "auto-repeat dropped");
        }
        return 0;
    default:
        log_key(msg, wparam, lparam, "game");
        *handled = 0;
        return 0;
    }
}

static int is_user_input(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

static LRESULT CALLBACK host_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result;
    if (shutdown_host_driving(&s.shutdown) && is_user_input(msg))
        return msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR ? 0 : DefWindowProcA(window, msg, wparam, lparam);
    switch (msg) {
    case WM_HOST_PAD_MOUSE:
        return shutdown_host_driving(&s.shutdown) ? 0 : pad_mouse(window, wparam);
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        int handled;
        LRESULT r = handle_key(window, msg, wparam, lparam, &handled);
        if (handled) return r;
        if (video_test_visible()) return 0;
        break;
    }
    case WM_CHAR:
    case WM_DEADCHAR:
        if (video_test_visible()) return 0;
        if (debug_menu_char((char)wparam)) return 0;
        break;
    case WM_GETMINMAXINFO:
        if (s.fullscreen) cover_limits(window, (MINMAXINFO*)lparam);
        else limit_minimum(window, (MINMAXINFO*)lparam);
        return 0;
    case WM_SIZING:
        constrain_sizing(window, wparam, (RECT*)lparam);
        return TRUE;
    case WM_SIZE:
        layout_surface();
        if (s.surface) {
            RECT r;
            if (GetClientRect(s.surface, &r) && r.right > 0 && r.bottom > 0)
                present_on_resize((uint32_t)r.right, (uint32_t)r.bottom);
        }
        if (!s.size_move) log_presentation();
        update_input("resize");
        break;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(window, &r);
        FillRect((HDC)wparam, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_ENTERSIZEMOVE:
        s.size_move = 1;
        update_input("move/resize");
        break;
    case WM_EXITSIZEMOVE:
        s.size_move = 0;
        log_presentation();
        update_input("move/resize done");
        break;
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (msg == WM_KILLFOCUS) open_press_reset(&s.open_press);
        if (msg == WM_ACTIVATEAPP) s.active = wparam != 0;
        if (msg == WM_SETFOCUS) s.focused = 1;
        if (msg == WM_KILLFOCUS) s.focused = 0;
        if ((msg == WM_ACTIVATEAPP && !s.active) || msg == WM_KILLFOCUS) release_mouse();
        update_input(msg == WM_ACTIVATEAPP ? (s.active ? "activated" : "deactivated")
                     : msg == WM_SETFOCUS  ? "focus"
                     : msg == WM_KILLFOCUS ? "focus lost"
                                           : "activation");
        /* While the host leads the game out, losing the foreground must not pause the game. */
        if (shutdown_host_driving(&s.shutdown)) return DefWindowProcA(window, msg, wparam, lparam);
        if (msg == WM_ACTIVATEAPP && !activation_message(&s.activation, loading_hd_level_loading(), wparam != 0)) {
            if (s.activation_logs < 16 && ++s.activation_logs)
                host_log("focus", "%s deferred while the level loads", wparam ? "display restore" : "deactivation");
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && ((HWND)wparam == window || (HWND)wparam == s.surface)) {
            if (pointer_over_view() && pointer_hidden()) SetCursor(NULL);
            else show_client_cursor();
            return TRUE;
        }
        return DefWindowProcA(window, msg, wparam, lparam);
    case WM_NCHITTEST:
        return s.fullscreen ? HTCLIENT : DefWindowProcA(window, msg, wparam, lparam);
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONUP:
        return DefWindowProcA(window, msg, wparam, lparam);
    case WM_DPICHANGED: {
        const RECT* suggested = (const RECT*)lparam;
        host_log("window", "dpi changed to %u (%u%%)", (unsigned)LOWORD(wparam), (unsigned)LOWORD(wparam) * 100 / BASE_DPI);
        if (s.fullscreen)
            cover_monitor();
        else
            SetWindowPos(window, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (s.fullscreen) cover_monitor();
        break;
    case WM_TIMER:
        if (wparam == POLICY_TIMER_ID) {
            if (fps_counter_idle(&s.fps, qpc_now())) refresh_title();
            update_input("timer");
            gamepad_idle();
            return 0;
        }
        break;
    case WM_SYSCOMMAND: {
        UINT command = (UINT)(wparam & 0xFFF0);
        if (command == SC_CLOSE) {
            request_close(window, (GetKeyState(VK_MENU) & 0x8000) ? "Alt+F4" : "title bar close button or system menu");
            return 0;
        }
        if (command == SC_MINIMIZE) release_mouse();
        if (command == SC_SIZE || command == SC_MOVE || command == SC_MINIMIZE || command == SC_MAXIMIZE ||
            command == SC_RESTORE)
            return DefWindowProcA(window, msg, wparam, lparam);
        break;
    }
    case WM_CLOSE:
        request_close(window, "WM_CLOSE (taskbar or another program)");
        return 0;
    case WM_DESTROY:
        host_log("window", "WM_DESTROY (%s)", shutdown_phase_name(s.shutdown.phase));
        KillTimer(window, POLICY_TIMER_ID);
        release_everything();
        present_release();
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case 0x020B:
    case 0x020C:
    case 0x020D:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL_:
        if (video_test_mouse(msg, wparam, lparam)) return 0;
        if (debug_menu_visible()) {
            if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL_)
                debug_menu_mouse(msg, (short)LOWORD(lparam) - (int)s.view.left, (short)HIWORD(lparam) - (int)s.view.top);
            return 0;
        }
        if (virtual_input_active() && !route_mouse(msg, &lparam)) return 0;
        break;
    }
    result = CallWindowProcA(window_message_skips_sdl(msg) ? s.game_proc : s.next, window, msg, wparam, lparam);
    /* SDL shows its own cursor when the window gains focus. */
    if (msg == WM_ACTIVATE || msg == WM_SETFOCUS) refresh_pointer_shape();
    if (msg == WM_NCDESTROY) {
        platform_window_detach();
        s.game = NULL;
        s.surface = NULL;
    }
    return result;
}

static LRESULT CALLBACK surface_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(window, msg, wparam, lparam);
}

static HWND create_surface(HWND parent) {
    WNDCLASSEXA wc;
    HINSTANCE instance = GetModuleHandleA(NULL);
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = surface_proc;
    wc.hInstance = instance;
    wc.lpszClassName = SURFACE_CLASS;
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    return CreateWindowExA(0, SURFACE_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL,
                           instance, NULL);
}

static void hook(HWND window) {
    s.next = (WNDPROC)GetWindowLongPtrA(window, GWLP_WNDPROC);
    SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)host_proc);
}

/* Letting SDL go puts the game's procedure back over the host's, so the host hooks the window again. */
static void detach_platform(HWND window) {
    if (!platform_window_attached()) return;
    release_mouse();
    platform_window_detach();
    if (window && IsWindow(window)) hook(window);
}

void window_host_detach_platform(void) { detach_platform(s.game); }

/* The application icon (resource 1 of the executable: src/trespasser_recomp.rc) on the game window, its title bar and taskbar button. */
static void set_window_icon(HWND window) {
    HINSTANCE instance = GetModuleHandleA(NULL);
    const struct { WPARAM kind; int cx; int cy; } sizes[] = {
        { ICON_BIG, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON) },
        { ICON_SMALL, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON) },
    };
    for (int i = 0; i < 2; i++) {
        HANDLE icon = LoadImageA(instance, MAKEINTRESOURCEA(1), IMAGE_ICON, sizes[i].cx, sizes[i].cy, LR_DEFAULTCOLOR);
        if (icon) SendMessageA(window, WM_SETICON, sizes[i].kind, (LPARAM)icon);
    }
}

static void attach(HWND window) {
    s.game = window;
    set_window_icon(window);
    activation_defer_init(&s.activation);
    s.active = 1;
    s.game_proc = (WNDPROC)GetWindowLongPtrA(window, GWLP_WNDPROC);
    platform_window_attach(window);
    hook(window);
    s.focused = GetFocus() == window;
    s.surface = create_surface(window);
    SetTimer(window, POLICY_TIMER_ID, POLICY_INTERVAL_MS, NULL);
    fps_counter_init(&s.fps, s.qpc_frequency.QuadPart, qpc_now());
    layout_surface();
    host_log("window", "game window 0x%08X: resizable, centered, F11 toggles borderless full screen; "
             "presentation surface %s", (uint32_t)(uintptr_t)window, s.surface ? "created" : "NOT created");
    host_log("hotkeys", "F1 Hint | F2 Controls | F3 Objectives | F4 Show Objective | F5 Minimap | F6 FPS | F10 Cheats | "
             "F11 Fullscreen | Esc Menu");
    host_log("input", "SDL3 mouse: relative mode for gameplay mouse-look, the cursor hidden and confined in "
             "INPUT_GAMEPLAY_CAPTURED only; keyboard: the game's own key messages, host keys taken from them first");
    if (s.cfg.borderless) {
        enter_fullscreen();
        layout_surface();
    }
}

HWND window_host_game_window(void) { return s.game; }

HWND window_host_surface(HWND window) { return s.surface && window == s.game ? s.surface : window; }

static int window_follows_guest(void) {
    return s.game && !fixed_window_size() && !s.cfg.stretch && !s.fullscreen && !IsZoomed(s.game) && !IsIconic(s.game);
}

static void window_frame_and_work(int* frame_w, int* frame_h, int* work_w, int* work_h) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(s.game, MONITOR_DEFAULTTONEAREST), &mi);
    frame_extents((DWORD)GetWindowLongPtrA(s.game, GWL_STYLE), (DWORD)GetWindowLongPtrA(s.game, GWL_EXSTYLE),
                  window_dpi(s.game), frame_w, frame_h);
    *work_w = mi.rcWork.right - mi.rcWork.left;
    *work_h = mi.rcWork.bottom - mi.rcWork.top;
}

static void log_pending_presentation(unsigned generation) {
    jpt_size_t target = s.pres.host;
    if (window_follows_guest()) {
        int frame_w, frame_h, work_w, work_h;
        window_frame_and_work(&frame_w, &frame_h, &work_w, &work_h);
        target = jpt_auto_client(s.pres.host, s.pres.pending_guest, frame_w, frame_h, work_w, work_h);
    }
    host_log("window", "Presentation pending: guest %dx%d generation %u, host target %dx%d", s.pres.pending_guest.width,
             s.pres.pending_guest.height, generation, target.width, target.height);
}

void window_host_set_display(HWND window, int width, int height, unsigned generation) {
    int active, known;
    if (window != s.game || !window || width <= 0 || height <= 0) return;
    known = s.pres.has_pending && s.pres.pending_generation == generation;
    s.fbw = width;
    s.fbh = height;
    s.virtual_on = 1;
    s.cursor.x = width / 2;
    s.cursor.y = height / 2;
    s.pad_cursor = 0;
    active = jpt_presentation_request_guest(&s.pres, width, height, generation);
    if (s.pres.has_pending) {
        if (!known) log_pending_presentation(generation);
        return;
    }
    if (active) s.view.right = s.view.left;
    layout_surface();
    update_input("display mode");
    refresh_title();
    log_presentation();
}

/* Called just before the frame is presented: the window, the view and the mouse mapping change in this one step. */
void window_host_frame_commit(unsigned generation) {
    jpt_commit_t commit;
    int frame_w = 0, frame_h = 0, work_w = 0, work_h = 0, follow;
    if (!s.game || !s.pres.has_pending || s.pres.pending_generation != generation) return;
    follow = window_follows_guest();
    if (follow) window_frame_and_work(&frame_w, &frame_h, &work_w, &work_h);
    commit = jpt_presentation_commit_frame(&s.pres, generation, follow, frame_w, frame_h, work_w, work_h);
    if (!commit.committed) return;
    if (commit.resize)
        SetWindowPos(s.game, NULL, 0, 0, commit.client.width + frame_w, commit.client.height + frame_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    layout_surface();
    update_input("presentation");
    refresh_title();
    log_presentation();
}

void window_host_clear_display(HWND window) {
    if (window != s.game || !s.virtual_on) return;
    s.virtual_on = 0;
    update_input("display released");
}

const char* window_host_close_origin(void) { return s.shutdown.source; }

int window_host_frames_allowed(void) { return shutdown_renderer_allowed(&s.shutdown); }

void window_host_crt_exiting(void) { advance(SHUTDOWN_CRT_EXITING, "the game's own menu"); }

void window_host_guest_stopped(void) { advance(SHUTDOWN_GUEST_STOPPED, "the game's own menu"); }

void window_host_begin_cleanup(void) { advance(SHUTDOWN_HOST_CLEANUP, NULL); }

void window_host_exited(void) { shutdown_exited(&s.shutdown); }

void shim_CreateWindowExA(void) {
    DWORD exstyle = GUEST_ARG(0), style = GUEST_ARG(3);
    const char* title = (const char*)GUEST_PTR(2);
    HWND parent = (HWND)GUEST_PTR(8);
    int x = (int)GUEST_ARG(4), y = (int)GUEST_ARG(5), w = (int)GUEST_ARG(6), h = (int)GUEST_ARG(7);
    int main_window = !s.game && !parent && (style & WS_POPUP);
    HWND window;
    if (main_window) {
        style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE;
        title = PRODUCT_TITLE;
        initial_placement(w, h, style, exstyle, &x, &y, &w, &h);
    }
    window = CreateWindowExA(exstyle, (LPCSTR)(uintptr_t)GUEST_ARG(1), title, style, x, y, w, h, parent,
                             (HMENU)GUEST_PTR(9), (HINSTANCE)GUEST_PTR(10), GUEST_PTR(11));
    if (main_window && window) attach(window);
    g_eax = (uint32_t)(uintptr_t)window;
    GUEST_RETURN_STDCALL(12);
}

void shim_DestroyWindow(void) {
    HWND window = (HWND)GUEST_PTR(0);
    if (window && window == s.game) {
        advance(SHUTDOWN_WINMAIN_RETURNING, "the game's own menu");
        host_log("window", "DestroyWindow by the game from sub_%08X (close source: %s)", g_cur_func,
                 s.shutdown.source ? s.shutdown.source : "the game");
        release_everything();
        update_input("closing");
        detach_platform(window);
    }
    g_eax = (uint32_t)DestroyWindow(window);
    GUEST_RETURN_STDCALL(1);
}

void shim_PostQuitMessage(void) {
    advance(SHUTDOWN_WINMAIN_RETURNING, "the game's own menu");
    host_log("window", "PostQuitMessage(%d) by the game from sub_%08X", (int)GUEST_ARG(0), g_cur_func);
    PostQuitMessage((int)GUEST_ARG(0));
    GUEST_RETURN_STDCALL(1);
}

void shim_SetWindowLongA(void) {
    HWND window = (HWND)GUEST_PTR(0);
    int index = (int)GUEST_ARG(1);
    if (window == s.game && (index == GWL_STYLE || index == GWL_EXSTYLE)) {
        note_ignored(IGNORED_STYLE, "game SetWindowLong of the window's style ignored: the host owns the window");
        g_eax = (uint32_t)GetWindowLongA(window, index);
    } else {
        g_eax = (uint32_t)SetWindowLongA(window, index, (LONG)GUEST_ARG(2));
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_SetWindowPos(void) {
    HWND window = (HWND)GUEST_PTR(0);
    if (window == s.game) {
        note_ignored(IGNORED_POSITION, "game SetWindowPos/MoveWindow on the window ignored: the host owns the window");
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)SetWindowPos(window, (HWND)GUEST_PTR(1), (int)GUEST_ARG(2), (int)GUEST_ARG(3),
                                       (int)GUEST_ARG(4), (int)GUEST_ARG(5), GUEST_ARG(6));
    }
    GUEST_RETURN_STDCALL(7);
}

void shim_MoveWindow(void) {
    HWND window = (HWND)GUEST_PTR(0);
    if (window == s.game) {
        note_ignored(IGNORED_POSITION, "game SetWindowPos/MoveWindow on the window ignored: the host owns the window");
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)MoveWindow(window, (int)GUEST_ARG(1), (int)GUEST_ARG(2), (int)GUEST_ARG(3),
                                     (int)GUEST_ARG(4), (BOOL)GUEST_ARG(5));
    }
    GUEST_RETURN_STDCALL(6);
}

void shim_SetWindowTextA(void) {
    HWND window = (HWND)GUEST_PTR(0);
    if (window == s.game) {
        note_ignored(IGNORED_TITLE, "game SetWindowText of the window ignored: the title shows the host's status");
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)SetWindowTextA(window, (LPCSTR)GUEST_PTR(1));
    }
    GUEST_RETURN_STDCALL(2);
}

/* On reactivation the game restores its window, which would undo a maximised window. */
void shim_ShowWindow(void) {
    HWND window = (HWND)GUEST_PTR(0);
    int command = (int)GUEST_ARG(1);
    if (window == s.game && shutdown_host_driving(&s.shutdown)) {
        g_eax = IsWindowVisible(window) ? TRUE : FALSE;
    } else if (window == s.game && command == SW_RESTORE && !IsIconic(window)) {
        note_ignored(IGNORED_RESTORE, "game ShowWindow(SW_RESTORE) on a window that is not minimised ignored");
        g_eax = IsWindowVisible(window) ? TRUE : FALSE;
    } else {
        g_eax = (uint32_t)ShowWindow(window, command);
    }
    GUEST_RETURN_STDCALL(2);
}

void shim_GetWindowRect(void) {
    HWND window = (HWND)GUEST_PTR(0);
    RECT* out = (RECT*)GUEST_PTR(1);
    if (virtual_window(window)) {
        SetRect(out, 0, 0, s.fbw, s.fbh);
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)GetWindowRect(window, out);
    }
    GUEST_RETURN_STDCALL(2);
}

void shim_GetClientRect(void) {
    HWND window = (HWND)GUEST_PTR(0);
    RECT* out = (RECT*)GUEST_PTR(1);
    if (virtual_window(window)) {
        SetRect(out, 0, 0, s.fbw, s.fbh);
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)GetClientRect(window, out);
    }
    GUEST_RETURN_STDCALL(2);
}

void shim_ClientToScreen(void) {
    HWND window = (HWND)GUEST_PTR(0);
    g_eax = virtual_window(window) ? TRUE : (uint32_t)ClientToScreen(window, (POINT*)GUEST_PTR(1));
    GUEST_RETURN_STDCALL(2);
}

void shim_GetCursorPos(void) {
    POINT* out = (POINT*)GUEST_PTR(0);
    BOOL ok = TRUE;
    if (virtual_input_active()) {
        platform_pump();
        update_input("GetCursorPos");
        if (g_cur_func == UI_LOOP) {
            int64_t section = perf_section_begin();
            gamepad_ui_loop();
            overlay_ui_loop();
            debug_menu_ui_loop();
            widescreen_apply_fov();
            ce_gameplay_ui_loop();
            update_input("GetCursorPos");
            perf_section_end(PS_UI_LOOP, section);
        }
        *out = game_cursor();
    } else {
        ok = GetCursorPos(out);
    }
    g_eax = (uint32_t)ok;
    GUEST_RETURN_STDCALL(1);
}

void shim_SetCursorPos(void) {
    s.cursor.x = (LONG)GUEST_ARG(0);
    s.cursor.y = (LONG)GUEST_ARG(1);
    note_ignored(IGNORED_CURSOR_POS, "game SetCursorPos sets its virtual cursor only; the real cursor is never moved");
    g_eax = TRUE;
    GUEST_RETURN_STDCALL(2);
}

void shim_ClipCursor(void) {
    const RECT* rect = (const RECT*)GUEST_PTR(0);
    if (rect) {
        char text[160];
        _snprintf(text, sizeof text - 1, "game ClipCursor(%ld,%ld,%ld,%ld) from sub_%08X not applied: the host confines "
                  "the cursor itself, in INPUT_GAMEPLAY_CAPTURED only", rect->left, rect->top, rect->right, rect->bottom,
                  g_cur_func);
        text[sizeof text - 1] = 0;
        note_ignored(IGNORED_CLIP, text);
    }
    g_eax = TRUE;
    GUEST_RETURN_STDCALL(1);
}

/* The display counter is the game's own; the system cursor is hidden only over the picture. */
void shim_ShowCursor(void) {
    int was_hidden = game_cursor_hidden();
    s.cursor_count += GUEST_ARG(0) ? 1 : -1;
    if (was_hidden != game_cursor_hidden()) refresh_pointer_shape();
    g_eax = (uint32_t)s.cursor_count;
    GUEST_RETURN_STDCALL(1);
}

/* The game captures the mouse for its whole run, which would send title-bar clicks to the client. */
void shim_SetCapture(void) {
    HWND window = (HWND)GUEST_PTR(0), previous = s.capture;
    if (window == s.game) {
        note_ignored(IGNORED_CAPTURE, "game SetCapture on its window kept virtual: the title bar and Close stay reachable");
        s.capture = window;
        g_eax = (uint32_t)(uintptr_t)previous;
    } else {
        g_eax = (uint32_t)(uintptr_t)SetCapture(window);
    }
    GUEST_RETURN_STDCALL(1);
}

void shim_ReleaseCapture(void) {
    if (s.capture) {
        s.capture = NULL;
        g_eax = TRUE;
    } else {
        g_eax = (uint32_t)ReleaseCapture();
    }
    GUEST_RETURN_STDCALL(0);
}

void shim_EnableMenuItem(void) {
    HMENU menu = (HMENU)GUEST_PTR(0);
    UINT item = GUEST_ARG(1), flags = GUEST_ARG(2);
    if (s.game && menu == GetSystemMenu(s.game, FALSE) && !(flags & MF_BYPOSITION) && (item == SC_CLOSE || item == SC_MOVE) &&
        (flags & (MF_GRAYED | MF_DISABLED))) {
        note_ignored(IGNORED_MENU, "game disabling Close/Move in the system menu ignored: Alt+F4 and the X button work");
        g_eax = MF_ENABLED;
    } else {
        g_eax = (uint32_t)EnableMenuItem(menu, item, flags);
    }
    GUEST_RETURN_STDCALL(3);
}

/* The game's pump drops system keys without dispatching them while some of its loops run, so
 * Alt+F4 is taken here, where every message it removes passes. */
void shim_PeekMessageA(void) {
    MSG* msg = (MSG*)GUEST_PTR(0);
    UINT remove = GUEST_ARG(4);
    BOOL got;
    if (g_cur_func == VIDEO_PLAYER && !shutdown_active(&s.shutdown)) gamepad_video_loop();
    if (drive_quit(msg, g_cur_func)) {
        got = TRUE;
    } else {
        got = PeekMessageA(msg, (HWND)GUEST_PTR(1), GUEST_ARG(2), GUEST_ARG(3), remove);
        if (got && (remove & PM_REMOVE) && s.game && msg->hwnd == s.game && msg->message == WM_SYSKEYDOWN &&
            msg->wParam == VK_F4 && (msg->lParam & 0x20000000) && !(msg->lParam & 0x40000000))
            request_close(s.game, "Alt+F4");
    }
    g_eax = (uint32_t)got;
    GUEST_RETURN_STDCALL(5);
}

/* While the host leads the game out its pump must return even when the window is minimised or
 * in the background: the game reactivates only for the foreground window. */
void shim_IsIconic(void) {
    HWND window = (HWND)GUEST_PTR(0);
    g_eax = window == s.game && shutdown_host_driving(&s.shutdown) ? FALSE : (uint32_t)IsIconic(window);
    GUEST_RETURN_STDCALL(1);
}

void shim_GetForegroundWindow(void) {
    g_eax = s.game && shutdown_host_driving(&s.shutdown) ? (uint32_t)(uintptr_t)s.game
                                                          : (uint32_t)(uintptr_t)GetForegroundWindow();
    GUEST_RETURN_STDCALL(0);
}

void window_host_input_summary(void) {
    host_log("input", "summary: final state %s, %lu gameplay captures, %lu relative moves, %lu auto-repeated "
             "Esc/F1/F2/F5/F11 key-downs dropped", input_state_name(s.state), s.captures, s.relative_moves,
             s.dropped_repeats);
}
