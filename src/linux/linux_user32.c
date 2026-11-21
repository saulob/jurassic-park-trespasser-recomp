#include "linux_user32.h"

#include <stdio.h>
#include <string.h>

#include "active_profile.h"
#include "debug_menu.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_sync.h"
#include "l10n.h"
#include "linux_features.h"
#include "linux_gamepad.h"
#include "linux_gdi32.h"
#include "linux_guest_quit.h"
#include "linux_hostkeys.h"
#include "linux_shutdown.h"
#include "linux_log.h"
#include "linux_mouse.h"
#include "linux_resource.h"
#include "linux_screen.h"
#include "linux_spine.h"
#include "linux_window.h"

#define MAX_WINDOWS 64
#define MAX_CLASSES 16
#define MAX_QUEUE 256
#define MAX_TIMERS 16
#define MAX_MESSAGE_NAMES 32
#define MAX_TEXT 128
#define MAX_EXTRA 32
#define CLASS_ATOM_BASE 0xC000u
#define MESSAGE_ATOM_BASE 0xC100u
#define ICON_BASE 0x00030000u
#define CURSOR_BASE 0x00040000u

enum {
    WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8,
    WM_SETTEXT = 0xC, WM_GETTEXT = 0xD, WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
    WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_GETMINMAXINFO = 0x24,
    WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83,
    WM_NCACTIVATE = 0x86, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105,
    WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_RBUTTONDBLCLK = 0x206, WM_MBUTTONDOWN = 0x207,
    WM_MBUTTONUP = 0x208, WM_MBUTTONDBLCLK = 0x209, WM_MOUSEWHEEL = 0x20A
};

enum {
    WS_CHILD = 0x40000000u, WS_VISIBLE = 0x10000000u, WS_BORDER = 0x00800000u, WS_DLGFRAME = 0x00400000u,
    WS_THICKFRAME = 0x00040000u, WS_MINIMIZE = 0x20000000u
};
enum { CS_DBLCLKS = 0x8, SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOWMINIMIZED = 2, SW_SHOW = 5, SW_MINIMIZE = 6, SW_RESTORE = 9 };
enum { SWP_NOSIZE = 1, SWP_NOMOVE = 2, SWP_NOACTIVATE = 0x10, SWP_SHOWWINDOW = 0x40, SWP_HIDEWINDOW = 0x80 };
enum { PM_REMOVE = 1 };
enum { SC_CLOSE = 0xF060 };
enum { GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_ID = -12, GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21 };
enum { MB_TYPEMASK = 0xF };

typedef struct {
    int used;
    char name[64];
    uint32_t style, wndproc, cls_extra, wnd_extra, hinstance, hicon, hcursor, hbrush;
} class_t;

typedef struct {
    int used;
    uint32_t generation;
    int cls;
    uint32_t wndproc, style, exstyle, parent, id, hinstance, userdata;
    int32_t x, y, w, h;
    int visible, needs_paint, needs_erase, minimized, in_destroy;
    char text[MAX_TEXT];
    uint8_t extra[MAX_EXTRA];
} window_t;

typedef struct {
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t px, py;
} message_t;

typedef struct {
    int used;
    uint32_t hwnd, id, interval, proc;
    uint64_t due;
} timer_t_;

static class_t s_class[MAX_CLASSES];
static window_t s_win[MAX_WINDOWS];
static uint32_t s_next_generation = 1;
static message_t s_queue[MAX_QUEUE];
static unsigned s_qhead, s_qcount;
static timer_t_ s_timer[MAX_TIMERS];
static char s_message_name[MAX_MESSAGE_NAMES][64];
static int s_message_names;
static uint32_t s_main, s_active, s_focus, s_capture;
static int s_quit_posted;
static uint32_t s_quit_code;
static int s_cursor_x, s_cursor_y, s_cursor_count;
static uint32_t s_cursor;
static uint32_t s_clip[4];
static int s_clipped;
static uint8_t s_keys[256];
static int s_buttons;
static unsigned s_idle_limit, s_empty_peeks;
static uint64_t s_last_click_ms;
static int s_last_click_x, s_last_click_y;
static uint32_t s_last_click_message;

void lx_user32_set_idle_limit(unsigned empty_peeks) { s_idle_limit = empty_peeks; }

void hle_user32_init(void) {
    memset(s_class, 0, sizeof s_class);
    memset(s_win, 0, sizeof s_win);
    memset(s_queue, 0, sizeof s_queue);
    memset(s_timer, 0, sizeof s_timer);
    memset(s_message_name, 0, sizeof s_message_name);
    memset(s_keys, 0, sizeof s_keys);
    memset(s_clip, 0, sizeof s_clip);
    s_next_generation = 1;
    s_qhead = s_qcount = 0;
    s_message_names = 0;
    s_main = s_active = s_focus = s_capture = 0;
    s_quit_posted = 0;
    s_quit_code = 0;
    s_cursor_x = s_cursor_y = 0;
    s_cursor_count = 0;
    s_cursor = 0;
    s_clipped = 0;
    lx_mouse_reset();
    s_buttons = 0;
    s_empty_peeks = 0;
    s_last_click_ms = 0;
    s_last_click_message = 0;
}

void hle_user32_shutdown(void) { lx_screen_window_close(); }

/* ---- tokens ---- */

static uint32_t token_of(int slot) { return (s_win[slot].generation << 8) | (uint32_t)(slot + 1); }

static window_t* window_of(uint32_t hwnd) {
    uint32_t slot = (hwnd & 0xFFu);
    window_t* w;
    if (!slot || slot > MAX_WINDOWS) return NULL;
    w = &s_win[slot - 1];
    return w->used && w->generation == (hwnd >> 8) ? w : NULL;
}

static uint32_t hwnd_of(const window_t* w) { return token_of((int)(w - s_win)); }

int lx_user32_window_exists(uint32_t hwnd) { return window_of(hwnd) != NULL; }
uint32_t lx_user32_main_window(void) { return s_main; }

unsigned lx_user32_window_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) n += (unsigned)s_win[i].used;
    return n;
}

unsigned lx_user32_class_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_CLASSES; i++) n += (unsigned)s_class[i].used;
    return n;
}

uint32_t lx_user32_class_wndproc(const char* name) {
    for (int i = 0; i < MAX_CLASSES; i++)
        if (s_class[i].used && !strcmp(s_class[i].name, name)) return s_class[i].wndproc;
    return 0;
}

uint32_t lx_user32_queue_length(void) { return s_qcount; }

static uint32_t now_ms(void) { return (uint32_t)jpt_time_ms(); }

static int style_border(uint32_t style) {
    if (style & WS_THICKFRAME) return 4;
    if (style & WS_DLGFRAME) return 3;
    return (style & WS_BORDER) ? 1 : 0;
}

static void client_of(const window_t* w, int* cw, int* ch) {
    int b = style_border(w->style), caption = ((w->style & 0x00C00000u) == 0x00C00000u) ? 18 : 0;
    *cw = w->w - 2 * b;
    *ch = w->h - 2 * b - caption;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

int lx_user32_client_size(uint32_t hwnd, int* w, int* h) {
    const window_t* win = window_of(hwnd);
    if (!win) return 0;
    client_of(win, w, h);
    return 1;
}

/* ---- calling the guest ---- */

static uint32_t call_proc(uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    uint32_t args[4];
    args[0] = hwnd, args[1] = msg, args[2] = wparam, args[3] = lparam;
    return lx_call_guest(proc, LX_STDCALL, 4, args, 0, NULL);
}

static uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
static uint64_t u_DestroyWindow(const uint32_t* a, void* u);

uint32_t lx_user32_send(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    window_t* w = window_of(hwnd);
    if (!w) return 0;
    return w->wndproc ? call_proc(w->wndproc, hwnd, msg, wparam, lparam) : def_window_proc(hwnd, msg, wparam, lparam);
}

static uint32_t send_to(const window_t* w, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    return lx_user32_send(hwnd_of(w), msg, wparam, lparam);
}

/* ---- the queue ---- */

int lx_user32_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    message_t* m;
    if (s_qcount == MAX_QUEUE) return 0;
    m = &s_queue[(s_qhead + s_qcount++) % MAX_QUEUE];
    m->hwnd = hwnd, m->message = msg, m->wparam = wparam, m->lparam = lparam;
    m->time = now_ms();
    m->px = s_cursor_x, m->py = s_cursor_y;
    return 1;
}

static int message_matches(const message_t* m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && m->hwnd != hwnd) return 0;
    if (lo || hi) return m->message >= lo && m->message <= hi;
    return 1;
}

static void remove_queue_at(unsigned i) {
    for (unsigned k = i; k + 1 < s_qcount; k++) s_queue[(s_qhead + k) % MAX_QUEUE] = s_queue[(s_qhead + k + 1) % MAX_QUEUE];
    s_qcount--;
}

static uint32_t input_target(void) {
    if (s_capture && window_of(s_capture)) return s_capture;
    if (s_focus && window_of(s_focus)) return s_focus;
    return s_main;
}

static uint32_t key_state_flags(void) {
    return (uint32_t)((s_buttons & 1 ? 1 : 0) | (s_buttons & 2 ? 2 : 0) | (s_keys[0x10] & 0x80 ? 4 : 0) | (s_keys[0x11] & 0x80 ? 8 : 0) |
                      (s_buttons & 4 ? 0x10 : 0));
}

static void translate_event(const lx_event_t* ev);

void lx_user32_inject_event(const lx_event_t* ev) { translate_event(ev); }

int lx_user32_cursor_get(long* x, long* y) {
    *x = s_cursor_x, *y = s_cursor_y;
    return 1;
}

void lx_user32_cursor_set(int x, int y) {
    s_cursor_x = x, s_cursor_y = y;
}

uint32_t lx_user32_capture(void) { return s_capture; }
int lx_user32_clipped(void) { return s_clipped; }

void lx_user32_cursor_nudge(int dx, int dy) {
    int w = 0, h = 0;
    lx_screen_window_client(&w, &h);
    s_cursor_x += dx;
    s_cursor_y += dy;
    if (w > 0 && h > 0) {
        s_cursor_x = s_cursor_x < 0 ? 0 : s_cursor_x >= w ? w - 1 : s_cursor_x;
        s_cursor_y = s_cursor_y < 0 ? 0 : s_cursor_y >= h ? h - 1 : s_cursor_y;
    }
}

static uint32_t mouse_lparam(int x, int y) { return ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF); }

static void translate_event(const lx_event_t* ev) {
    uint32_t target = input_target();
    window_t* w = window_of(target);
    switch (ev->kind) {
    case LX_EV_CLOSE:
        lx_shutdown_request(ev->vk == 1 ? LX_QUIT_ALT_F4 : LX_QUIT_WINDOW);
        break;
    case LX_EV_KEY: {
        int alt = (s_keys[0x12] & 0x80) != 0, was_down = (s_keys[ev->vk & 0xFF] & 0x80) != 0;
        uint32_t lp = 1u | ((ev->scancode & 0xFFu) << 16);
        unsigned msg = ev->down ? (alt || ev->vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt || ev->vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP);
        if (ev->down && !ev->synthetic) lx_gamepad_note_keyboard();
        if (lx_hostkeys_filter(msg, ev->vk, ev->repeat, (s_keys[0x10] & 0x80) != 0, (s_keys[0x11] & 0x80) != 0, alt)) break;
        s_keys[ev->vk & 0xFF] = ev->down ? 0x80 : 0;
        if (ev->down && was_down) lp |= 1u << 30;
        if (!ev->down) lp |= (1u << 30) | (1u << 31);
        if (alt || ev->vk == 0x12) lp |= 1u << 29;
        if (w) lx_user32_post(target, ev->down ? (alt || ev->vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN)
                                               : (alt || ev->vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP),
                              ev->vk, lp);
        break;
    }
    case LX_EV_CHAR:
        if (lx_features_running() && ev->vk < 128u && debug_menu_char((char)ev->vk)) break;
        if (w) lx_user32_post(target, WM_CHAR, ev->vk, 1);
        break;
    case LX_EV_MOUSE_MOVE: {
        int gx = ev->x, gy = ev->y;
        if (!ev->synthetic) lx_gamepad_note_mouse_motion(ev->relative ? (int)ev->dx : ev->x - s_cursor_x, ev->relative ? (int)ev->dy : ev->y - s_cursor_y);
        lx_mouse_note(ev->relative ? LX_MN_RELATIVE_MOTION : LX_MN_ABSOLUTE_MOTION);
        if (ev->relative) {
            long x = s_cursor_x, y = s_cursor_y;
            if (!lx_mouse_motion((long)ev->dx, (long)ev->dy, &x, &y)) break;
            s_cursor_x = (int)x, s_cursor_y = (int)y;
            if (w) lx_user32_post(target, WM_MOUSEMOVE, key_state_flags(), mouse_lparam(s_cursor_x, s_cursor_y));
            break;
        }
        if (lx_mouse_cursor_virtual()) break;
        if (lx_screen_window_is_open() && !lx_screen_client_to_guest(ev->x, ev->y, &gx, &gy)) break;
        if (gx == s_cursor_x && gy == s_cursor_y) break;
        s_cursor_x = gx;
        s_cursor_y = gy;
        if (lx_features_pointer_move(gx, gy)) break;
        if (w) lx_user32_post(target, WM_MOUSEMOVE, key_state_flags(), mouse_lparam(gx, gy));
        break;
    }
    case LX_EV_MOUSE_BUTTON: {
        uint32_t down_msg = ev->vk == 1 ? WM_LBUTTONDOWN : ev->vk == 2 ? WM_RBUTTONDOWN : WM_MBUTTONDOWN, msg = down_msg;
        lx_mouse_note(LX_MN_BUTTON);
        if (ev->down && !ev->synthetic) lx_gamepad_note_mouse_button();
        if (ev->down) s_buttons |= (int)ev->vk;
        else s_buttons &= ~(int)ev->vk, msg = down_msg + 1;
        if (lx_features_pointer_button(ev->vk, ev->down, s_cursor_x, s_cursor_y)) break;
        if (ev->down && w && s_class[w->cls].style & CS_DBLCLKS) {
            uint64_t t = jpt_time_ms();
            if (s_last_click_message == down_msg && t - s_last_click_ms < 500 && s_last_click_x == s_cursor_x && s_last_click_y == s_cursor_y)
                msg = down_msg + 2, s_last_click_message = 0;
            else s_last_click_message = down_msg, s_last_click_ms = t, s_last_click_x = s_cursor_x, s_last_click_y = s_cursor_y;
        }
        if (w) lx_user32_post(target, msg, key_state_flags(), mouse_lparam(s_cursor_x, s_cursor_y));
        break;
    }
    case LX_EV_MOUSE_WHEEL:
        lx_mouse_note(LX_MN_WHEEL);
        if (w) lx_user32_post(target, WM_MOUSEWHEEL, ((uint32_t)(ev->y & 0xFFFF) << 16) | key_state_flags(), mouse_lparam(s_cursor_x, s_cursor_y));
        break;
    case LX_EV_FOCUS:
        lx_mouse_set_focus(ev->down);
        if (w) {
            lx_user32_post(target, WM_ACTIVATEAPP, (uint32_t)ev->down, 0);
            lx_user32_post(target, WM_ACTIVATE, ev->down ? 1u : 0u, 0);
            lx_user32_post(target, ev->down ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        }
        break;
    default: break;
    }
}

static void pump_backend(void) {
    lx_event_t ev;
    if (!lx_screen_window_is_open()) return;
    lx_mouse_update();
    while (lx_window_backend()->next_event(lx_window_backend(), 0, &ev) && ev.kind != LX_EV_NONE) translate_event(&ev);
}

static int timer_message(uint32_t hwnd, uint32_t lo, uint32_t hi, message_t* out) {
    uint64_t t = jpt_time_ms();
    if ((lo || hi) && !(WM_TIMER >= lo && WM_TIMER <= hi)) return 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_* tm = &s_timer[i];
        if (!tm->used || (hwnd && tm->hwnd != hwnd) || t < tm->due) continue;
        tm->due = t + (tm->interval ? tm->interval : 1);
        out->hwnd = tm->hwnd, out->message = WM_TIMER, out->wparam = tm->id, out->lparam = tm->proc;
        out->time = (uint32_t)t, out->px = s_cursor_x, out->py = s_cursor_y;
        return 1;
    }
    return 0;
}

static int paint_message(uint32_t hwnd, uint32_t lo, uint32_t hi, message_t* out) {
    if ((lo || hi) && !(WM_PAINT >= lo && WM_PAINT <= hi)) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_t* w = &s_win[i];
        if (!w->used || !w->needs_paint || !w->visible || (hwnd && token_of(i) != hwnd)) continue;
        out->hwnd = token_of(i), out->message = WM_PAINT, out->wparam = 0, out->lparam = 0;
        out->time = now_ms(), out->px = s_cursor_x, out->py = s_cursor_y;
        return 1;
    }
    return 0;
}

static int fetch_message(uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, message_t* out) {
    pump_backend();
    for (unsigned i = 0; i < s_qcount; i++) {
        message_t* m = &s_queue[(s_qhead + i) % MAX_QUEUE];
        if (!message_matches(m, hwnd, lo, hi)) continue;
        *out = *m;
        if (remove) remove_queue_at(i);
        return 1;
    }
    if (s_quit_posted && (!(lo || hi) || (WM_QUIT >= lo && WM_QUIT <= hi))) {
        memset(out, 0, sizeof *out);
        out->message = WM_QUIT, out->wparam = s_quit_code, out->time = now_ms();
        if (remove) s_quit_posted = 0;
        return 1;
    }
    if (paint_message(hwnd, lo, hi, out)) return 1;
    return timer_message(hwnd, lo, hi, out);
}

/* ---- guest structures ---- */

static int read_rect(uint32_t va, int32_t r[4]) { return gp_copy_in(r, va, 16); }
static int write_rect(uint32_t va, const int32_t r[4]) { return gp_copy_out(va, r, 16); }

/* ---- the default procedure and the window sequences ---- */

static void set_text(window_t* w, const char* text) {
    snprintf(w->text, sizeof w->text, "%s", text ? text : "");
    if (hwnd_of(w) == s_main) lx_screen_set_title(w->text);
}

static void paint_invalidate(window_t* w, int erase) {
    w->needs_paint = 1;
    if (erase) w->needs_erase = 1;
}

static void activate(window_t* w) {
    uint32_t h = hwnd_of(w);
    if (s_active == h) return;
    if (!s_active) send_to(w, WM_ACTIVATEAPP, 1, 0);
    send_to(w, WM_NCACTIVATE, 1, 0);
    send_to(w, WM_ACTIVATE, 1, 0);
    s_active = h;
    if (window_of(h)) {
        s_focus = h;
        send_to(w, WM_SETFOCUS, 0, 0);
    }
}

static void geometry_notify(window_t* w, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags) {
    uint32_t va = gm_heap_alloc(28, 1);
    uint32_t pos[7];
    if (!va) return;
    pos[0] = hwnd_of(w), pos[1] = 0, pos[2] = (uint32_t)x, pos[3] = (uint32_t)y, pos[4] = (uint32_t)cx, pos[5] = (uint32_t)cy, pos[6] = flags;
    gp_copy_out(va, pos, sizeof pos);
    send_to(w, WM_WINDOWPOSCHANGING, 0, va);
    gp_copy_in(pos, va, sizeof pos);
    w->x = (int32_t)pos[2], w->y = (int32_t)pos[3], w->w = (int32_t)pos[4], w->h = (int32_t)pos[5];
    send_to(w, WM_WINDOWPOSCHANGED, 0, va);
    gm_heap_free(va);
}

static void show_window(window_t* w, int cmd) {
    uint32_t h = hwnd_of(w);
    int show = cmd != SW_HIDE && cmd != SW_MINIMIZE && cmd != SW_SHOWMINIMIZED;
    int was = w->visible;
    if (show == was && cmd != SW_SHOWNORMAL) return;
    w->minimized = cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED;
    if (show && h == s_main) {
        int cw, ch;
        client_of(w, &cw, &ch);
        lx_screen_window_open(w->text, cw, ch);
        lx_screen_set_visible(1);
    }
    if (show == was && cmd == SW_SHOWNORMAL && was) return;
    send_to(w, WM_SHOWWINDOW, (uint32_t)show, 0);
    w->visible = show;
    if (show) w->style |= WS_VISIBLE;
    else w->style &= ~(uint32_t)WS_VISIBLE;
    if (show) {
        geometry_notify(w, w->x, w->y, w->w, w->h, SWP_SHOWWINDOW);
        activate(w);
        paint_invalidate(w, 1);
    } else {
        if (h == s_main) lx_screen_set_visible(0);
        if (s_active == h) s_active = 0;
        if (s_focus == h) s_focus = 0;
    }
}

static uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    window_t* w = window_of(hwnd);
    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_NCACTIVATE: return 1;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return 0;
    case WM_CLOSE: {
        uint32_t destroy_args[1];
        destroy_args[0] = hwnd;
        u_DestroyWindow(destroy_args, NULL);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0u) == SC_CLOSE) send_to(w, WM_CLOSE, 0, 0);
        return 0;
    case WM_PAINT:
        w->needs_paint = 0;
        return 0;
    case WM_SETTEXT: {
        const char* s = lparam ? gp_string(lparam, MAX_TEXT) : NULL;
        set_text(w, s);
        return 1;
    }
    case WM_GETTEXTLENGTH: return (uint32_t)strlen(w->text);
    case WM_GETTEXT: {
        uint32_t n = (uint32_t)strlen(w->text);
        char* out = (char*)gm_ptr_checked(lparam, wparam ? wparam : 1, 1);
        if (!out || !wparam) return 0;
        if (n >= wparam) n = wparam - 1;
        memcpy(out, w->text, n);
        out[n] = 0;
        return n;
    }
    case WM_WINDOWPOSCHANGED: {
        int cw, ch;
        client_of(w, &cw, &ch);
        send_to(w, WM_MOVE, 0, mouse_lparam(w->x, w->y));
        send_to(w, WM_SIZE, 0, mouse_lparam(cw, ch));
        return 0;
    }
    default: return 0;
    }
}

/* ---- classes, windows ---- */

static uint64_t u_RegisterClassA(const uint32_t* a, void* u) {
    uint32_t wc[10];
    const char* name;
    (void)u;
    if (!gp_copy_in(wc, a[0], sizeof wc)) return 0;
    name = wc[9] > 0xFFFFu ? gp_string(wc[9], 64) : NULL;
    if (!name) return 0;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (s_class[i].used && !strcmp(s_class[i].name, name)) return 0;
    for (int i = 0; i < MAX_CLASSES; i++) {
        class_t* c = &s_class[i];
        if (c->used) continue;
        c->used = 1;
        snprintf(c->name, sizeof c->name, "%s", name);
        c->style = wc[0], c->wndproc = wc[1], c->cls_extra = wc[2], c->wnd_extra = wc[3], c->hinstance = wc[4];
        c->hicon = wc[5], c->hcursor = wc[6], c->hbrush = wc[7];
        lx_log("Linux USER32", "registered class \"%s\" (wndproc 0x%08X, style 0x%X)", c->name, c->wndproc, c->style);
        return CLASS_ATOM_BASE + (uint32_t)i;
    }
    return 0;
}

static int find_class(uint32_t name_va) {
    const char* name;
    if (name_va <= 0xFFFFu) {
        uint32_t atom = name_va;
        return atom >= CLASS_ATOM_BASE && atom < CLASS_ATOM_BASE + MAX_CLASSES && s_class[atom - CLASS_ATOM_BASE].used ? (int)(atom - CLASS_ATOM_BASE) : -1;
    }
    name = gp_string(name_va, 64);
    if (!name) return -1;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (s_class[i].used && !strcmp(s_class[i].name, name)) return i;
    return -1;
}

static void free_window(window_t* w) {
    uint32_t h = hwnd_of(w);
    for (int i = 0; i < MAX_TIMERS; i++)
        if (s_timer[i].used && s_timer[i].hwnd == h) s_timer[i].used = 0;
    for (unsigned i = 0; i < s_qcount;)
        if (s_queue[(s_qhead + i) % MAX_QUEUE].hwnd == h) remove_queue_at(i);
        else i++;
    if (s_active == h) s_active = 0;
    if (s_focus == h) s_focus = 0;
    if (s_capture == h) s_capture = 0;
    lx_gdi_window_gone(h);
    w->used = 0;
}

static uint64_t u_DestroyWindow(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    uint32_t h = a[0];
    (void)u;
    if (!w || w->in_destroy) return 0;
    w->in_destroy = 1;
    if (w->visible) show_window(w, SW_HIDE);
    send_to(w, WM_DESTROY, 0, 0);
    if (!window_of(h)) return 1;
    send_to(w, WM_NCDESTROY, 0, 0);
    if (h == s_main) {
        s_main = 0;
        lx_screen_window_close();
    }
    free_window(w);
    return 1;
}

static uint64_t u_CreateWindowExA(const uint32_t* a, void* u) {
    int ci = find_class(a[1]), slot = -1;
    window_t* w;
    uint32_t cs_va, hwnd, cs[12], r;
    const char* title = a[2] ? gp_string(a[2], MAX_TEXT) : "";
    (void)u;
    if (ci < 0) {
        hle_set_last_error(1407);
        return 0;
    }
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!s_win[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return 0;
    w = &s_win[slot];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->generation = s_next_generation++ & 0xFFFFFFu;
    if (!w->generation) w->generation = s_next_generation++;
    w->cls = ci;
    w->wndproc = s_class[ci].wndproc;
    w->exstyle = a[0], w->style = a[3];
    w->x = (int32_t)a[4] == (int32_t)0x80000000 ? 0 : (int32_t)a[4];
    w->y = (int32_t)a[5] == (int32_t)0x80000000 ? 0 : (int32_t)a[5];
    w->w = (int32_t)a[6] == (int32_t)0x80000000 ? 640 : (int32_t)a[6];
    w->h = (int32_t)a[7] == (int32_t)0x80000000 ? 480 : (int32_t)a[7];
    w->parent = a[8], w->id = a[9], w->hinstance = a[10];
    set_text(w, title);
    hwnd = token_of(slot);
    if (!(w->style & WS_CHILD) && !s_main) s_main = hwnd;
    lx_log("Linux USER32", "created window 0x%08X class \"%s\" \"%s\" %dx%d at %d,%d style 0x%08X", hwnd, s_class[ci].name, w->text, w->w, w->h, w->x,
           w->y, w->style);

    cs_va = gm_heap_alloc(sizeof cs, 1);
    if (!cs_va) {
        free_window(w);
        if (s_main == hwnd) s_main = 0;
        return 0;
    }
    cs[0] = a[11], cs[1] = a[10], cs[2] = a[9], cs[3] = a[8], cs[4] = (uint32_t)w->h, cs[5] = (uint32_t)w->w, cs[6] = (uint32_t)w->y,
    cs[7] = (uint32_t)w->x, cs[8] = w->style, cs[9] = a[2], cs[10] = a[1], cs[11] = a[0];
    gp_copy_out(cs_va, cs, sizeof cs);
    if (!(w->style & WS_CHILD)) {
        uint32_t mm = gm_heap_alloc(40, 1);
        if (mm) {
            send_to(w, WM_GETMINMAXINFO, 0, mm);
            gm_heap_free(mm);
        }
    }
    r = send_to(w, WM_NCCREATE, 0, cs_va);
    if (!r) {
        gm_heap_free(cs_va);
        free_window(w);
        if (s_main == hwnd) s_main = 0;
        return 0;
    }
    {
        uint32_t rect_va = gm_heap_alloc(16, 1);
        int32_t rc[4];
        if (rect_va) {
            rc[0] = w->x, rc[1] = w->y, rc[2] = w->x + w->w, rc[3] = w->y + w->h;
            gp_copy_out(rect_va, rc, sizeof rc);
            send_to(w, WM_NCCALCSIZE, 0, rect_va);
            gm_heap_free(rect_va);
        }
    }
    r = send_to(w, WM_CREATE, 0, cs_va);
    gm_heap_free(cs_va);
    if ((int32_t)r == -1) {
        if (window_of(hwnd)) free_window(w);
        if (s_main == hwnd) s_main = 0;
        return 0;
    }
    if (window_of(hwnd) && (w->style & WS_VISIBLE)) show_window(w, SW_SHOWNORMAL);
    return hwnd;
}

static uint64_t u_ShowWindow(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    int was;
    (void)u;
    if (!w) return 0;
    was = w->visible;
    show_window(w, (int)a[1]);
    return (uint64_t)was;
}

static uint64_t u_UpdateWindow(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    (void)u;
    if (!w) return 0;
    if (w->needs_paint && w->visible) {
        if (w->needs_erase) {
            w->needs_erase = 0;
            send_to(w, WM_ERASEBKGND, 0, 0);
        }
        send_to(w, WM_PAINT, 0, 0);
    }
    return 1;
}

static uint64_t u_InvalidateRect(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    (void)u;
    if (!w) return 0;
    paint_invalidate(w, a[2] != 0);
    return 1;
}

static uint64_t u_ValidateRect(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    (void)u;
    if (!w) return 0;
    w->needs_paint = 0;
    return 1;
}

static uint64_t u_SetWindowPos(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    uint32_t flags = a[6];
    int32_t x, y, cx, cy;
    (void)u;
    if (!w) return 0;
    x = (flags & SWP_NOMOVE) ? w->x : (int32_t)a[2];
    y = (flags & SWP_NOMOVE) ? w->y : (int32_t)a[3];
    cx = (flags & SWP_NOSIZE) ? w->w : (int32_t)a[4];
    cy = (flags & SWP_NOSIZE) ? w->h : (int32_t)a[5];
    if (flags & SWP_HIDEWINDOW) show_window(w, SW_HIDE);
    geometry_notify(w, x, y, cx, cy, flags);
    if (a[0] == s_main && !(flags & SWP_NOSIZE)) {
        int cw, ch;
        client_of(w, &cw, &ch);
        lx_screen_set_client_size(cw, ch);
    }
    if (flags & SWP_SHOWWINDOW) show_window(w, SW_SHOW);
    return 1;
}

static uint64_t u_MoveWindow(const uint32_t* a, void* u) {
    uint32_t args[7];
    args[0] = a[0], args[1] = 0, args[2] = a[1], args[3] = a[2], args[4] = a[3], args[5] = a[4], args[6] = 4;
    return u_SetWindowPos(args, u);
}

static uint64_t u_GetWindowRect(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    int32_t r[4];
    (void)u;
    if (!w) return 0;
    r[0] = w->x, r[1] = w->y, r[2] = w->x + w->w, r[3] = w->y + w->h;
    return write_rect(a[1], r);
}

static uint64_t u_GetClientRect(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    int32_t r[4] = { 0, 0, 0, 0 };
    int cw, ch;
    (void)u;
    if (!w) return 0;
    client_of(w, &cw, &ch);
    r[2] = cw, r[3] = ch;
    return write_rect(a[1], r);
}

static uint64_t u_ClientToScreen(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    int32_t pt[2];
    (void)u;
    if (!w || !gp_copy_in(pt, a[1], sizeof pt)) return 0;
    pt[0] += w->x + style_border(w->style);
    pt[1] += w->y + style_border(w->style);
    return gp_copy_out(a[1], pt, sizeof pt);
}

static uint64_t u_GetWindowPlacement(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    uint32_t wp[11] = { 44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    (void)u;
    if (!w) return 0;
    wp[1] = 0, wp[2] = w->minimized ? SW_SHOWMINIMIZED : w->visible ? SW_SHOWNORMAL : SW_HIDE;
    wp[7] = (uint32_t)w->x, wp[8] = (uint32_t)w->y, wp[9] = (uint32_t)(w->x + w->w), wp[10] = (uint32_t)(w->y + w->h);
    return gp_copy_out(a[1], wp, sizeof wp);
}

static uint64_t u_GetWindowLongA(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    int32_t index = (int32_t)a[1];
    (void)u;
    if (!w) return 0;
    switch (index) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_STYLE: return w->style;
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
    case GWL_HINSTANCE: return w->hinstance;
    default:
        if (index >= 0 && index + 4 <= MAX_EXTRA) {
            uint32_t v;
            memcpy(&v, w->extra + index, 4);
            return v;
        }
        return 0;
    }
}

static uint64_t u_SetWindowLongA(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    int32_t index = (int32_t)a[1];
    uint32_t old = 0;
    (void)u;
    if (!w) return 0;
    switch (index) {
    case GWL_WNDPROC: old = w->wndproc, w->wndproc = a[2]; break;
    case GWL_STYLE: old = w->style, w->style = a[2]; break;
    case GWL_EXSTYLE: old = w->exstyle, w->exstyle = a[2]; break;
    case GWL_USERDATA: old = w->userdata, w->userdata = a[2]; break;
    case GWL_ID: old = w->id, w->id = a[2]; break;
    default:
        if (index >= 0 && index + 4 <= MAX_EXTRA) {
            memcpy(&old, w->extra + index, 4);
            memcpy(w->extra + index, &a[2], 4);
        }
    }
    return old;
}

static uint64_t u_SetWindowTextA(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    (void)u;
    if (!w) return 0;
    set_text(w, a[1] ? gp_string(a[1], MAX_TEXT) : "");
    return 1;
}

static uint64_t u_GetParent(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    (void)u;
    return w ? w->parent : 0;
}

static uint64_t u_GetActiveWindow(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return s_active;
}

static uint64_t u_SetActiveWindow(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    uint32_t old = s_active;
    (void)u;
    if (w) activate(w);
    return old;
}

static uint64_t u_IsIconic(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    (void)u;
    return w && w->minimized;
}

static uint64_t u_SetCapture(const uint32_t* a, void* u) {
    uint32_t old = s_capture;
    (void)u;
    if (window_of(a[0])) s_capture = a[0];
    return old;
}

static uint64_t u_ReleaseCapture(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    s_capture = 0;
    return 1;
}

static uint64_t u_EnumWindows(const uint32_t* a, void* u) {
    (void)u;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        uint32_t args[2];
        if (!s_win[i].used) continue;
        args[0] = token_of(i), args[1] = a[1];
        if (!lx_call_guest(a[0], LX_STDCALL, 2, args, 0, NULL)) break;
    }
    return 1;
}

static uint64_t u_GetSystemMenu(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t u_EnableMenuItem(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0xFFFFFFFFu;
}

/* ---- messages ---- */

static void store_message(uint32_t va, const message_t* m) {
    uint32_t out[7];
    out[0] = m->hwnd, out[1] = m->message, out[2] = m->wparam, out[3] = m->lparam, out[4] = m->time, out[5] = (uint32_t)m->px,
    out[6] = (uint32_t)m->py;
    gp_copy_out(va, out, sizeof out);
}

static uint64_t u_PeekMessageA(const uint32_t* a, void* u) {
    message_t m;
    (void)u;
    if (lx_shutdown_state() == LX_QUIT_REQUESTED && lx_guest_quit_step(lx_guest_eip()) == LX_QUIT_STEP_SKIP_VIDEO) {
        memset(&m, 0, sizeof m);
        m.hwnd = s_main, m.message = WM_KEYUP, m.wparam = 0x1B, m.lparam = 0xC0010001u, m.time = now_ms();
        store_message(a[0], &m);
        return 1;
    }
    if (!fetch_message(a[1], a[2], a[3], (a[4] & PM_REMOVE) != 0, &m)) {
        if (s_idle_limit && ++s_empty_peeks >= s_idle_limit) lx_abort(LX_STOP, s_empty_peeks, "idle: the guest polls for messages and gets none");
        return 0;
    }
    s_empty_peeks = 0;
    store_message(a[0], &m);
    return 1;
}

static uint64_t u_TranslateMessage(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t u_TranslateAcceleratorA(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t u_DispatchMessageA(const uint32_t* a, void* u) {
    uint32_t m[7];
    (void)u;
    if (!gp_copy_in(m, a[0], sizeof m)) return 0;
    if (m[1] == WM_TIMER && m[3]) {
        uint32_t args[4];
        args[0] = m[0], args[1] = WM_TIMER, args[2] = m[2], args[3] = m[4];
        return lx_call_guest(m[3], LX_STDCALL, 4, args, 0, NULL);
    }
    if (m[1] == WM_PAINT) {
        window_t* w = window_of(m[0]);
        if (w && w->needs_erase) {
            w->needs_erase = 0;
            send_to(w, WM_ERASEBKGND, 0, 0);
        }
    }
    return lx_user32_send(m[0], m[1], m[2], m[3]);
}

static uint64_t u_PostQuitMessage(const uint32_t* a, void* u) {
    (void)u;
    s_quit_posted = 1;
    s_quit_code = a[0];
    return 0;
}

static uint64_t u_SendMessageA(const uint32_t* a, void* u) {
    (void)u;
    return lx_user32_send(a[0], a[1], a[2], a[3]);
}

static uint64_t u_DefWindowProcA(const uint32_t* a, void* u) {
    (void)u;
    return def_window_proc(a[0], a[1], a[2], a[3]);
}

static uint64_t u_RegisterWindowMessageA(const uint32_t* a, void* u) {
    const char* name = gp_string(a[0], 64);
    (void)u;
    if (!name) return 0;
    for (int i = 0; i < s_message_names; i++)
        if (!strcmp(s_message_name[i], name)) return MESSAGE_ATOM_BASE + (uint32_t)i;
    if (s_message_names == MAX_MESSAGE_NAMES) return 0;
    snprintf(s_message_name[s_message_names], sizeof s_message_name[0], "%s", name);
    return MESSAGE_ATOM_BASE + (uint32_t)s_message_names++;
}

static uint64_t u_SetTimer(const uint32_t* a, void* u) {
    (void)u;
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_* t = &s_timer[i];
        if (t->used && t->hwnd == a[0] && a[0] && t->id == a[1]) {
            t->interval = a[2], t->proc = a[3], t->due = jpt_time_ms() + a[2];
            return a[1];
        }
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_* t = &s_timer[i];
        if (t->used) continue;
        t->used = 1;
        t->hwnd = a[0];
        t->id = a[0] ? a[1] : (uint32_t)i + 1;
        t->interval = a[2], t->proc = a[3], t->due = jpt_time_ms() + a[2];
        return t->id;
    }
    return 0;
}

static uint64_t u_KillTimer(const uint32_t* a, void* u) {
    (void)u;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (s_timer[i].used && s_timer[i].hwnd == a[0] && s_timer[i].id == a[1]) {
            s_timer[i].used = 0;
            return 1;
        }
    return 0;
}

/* ---- input state, cursor, metrics ---- */

int lx_user32_key_held(unsigned vk) { return vk < 256 && (s_keys[vk] & 0x80) != 0; }

static void (*s_ui_loop)(void);

void lx_user32_set_ui_loop(void (*fn)(void)) { s_ui_loop = fn; }

static uint64_t u_GetCursorPos(const uint32_t* a, void* u) {
    int32_t pt[2];
    (void)u;
    if (s_ui_loop && lx_guest_eip() == jpt_gva_canonical[GVA_ID(0x00417840)]) s_ui_loop();
    pt[0] = s_cursor_x, pt[1] = s_cursor_y;
    return gp_copy_out(a[0], pt, sizeof pt);
}

static uint64_t u_SetCursorPos(const uint32_t* a, void* u) {
    (void)u;
    s_cursor_x = (int32_t)a[0];
    s_cursor_y = (int32_t)a[1];
    return 1;
}

static uint64_t u_ShowCursor(const uint32_t* a, void* u) {
    (void)u;
    s_cursor_count += a[0] ? 1 : -1;
    lx_mouse_set_cursor_count(s_cursor_count);
    return (uint64_t)(uint32_t)s_cursor_count;
}

static uint64_t u_SetCursor(const uint32_t* a, void* u) {
    uint32_t old = s_cursor;
    (void)u;
    s_cursor = a[0];
    return old;
}

static uint64_t u_InitCommonControls(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t u_ClipCursor(const uint32_t* a, void* u) {
    (void)u;
    s_clipped = a[0] != 0 && gp_copy_in(s_clip, a[0], sizeof s_clip);
    return 1;
}

static uint64_t u_GetForegroundWindow(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return s_active;
}

static uint64_t u_GetAsyncKeyState(const uint32_t* a, void* u) {
    (void)u;
    return a[0] < 256 && (s_keys[a[0]] & 0x80) ? 0x8000u : 0;
}

static uint64_t u_GetKeyState(const uint32_t* a, void* u) {
    (void)u;
    return a[0] < 256 ? (uint32_t)(s_keys[a[0]] & 0x80 ? 0xFF80u : 0) : 0;
}

static uint64_t u_GetSystemMetrics(const uint32_t* a, void* u) {
    lx_display_mode_t m = lx_screen_mode();
    (void)u;
    switch (a[0]) {
    case 0: case 16: case 61: return (uint32_t)m.width;
    case 1: case 17: case 62: return (uint32_t)m.height;
    case 2: case 3: return 16;
    case 4: return 19;
    case 5: case 6: return 1;
    case 7: case 8: return 3;
    case 11: case 12: case 13: case 14: return 32;
    case 15: return 19;
    case 19: return 1;
    case 32: case 33: return 4;
    case 43: return 3;
    default: return 0;
    }
}

static uint64_t u_LoadIconA(const uint32_t* a, void* u) {
    (void)u;
    return a[1] <= 0xFFFFu ? ICON_BASE + a[1] : ICON_BASE + 0x10000u;
}

static uint64_t u_LoadCursorA(const uint32_t* a, void* u) {
    (void)u;
    return a[1] <= 0xFFFFu ? CURSOR_BASE + a[1] : CURSOR_BASE + 0x10000u;
}

/* ---- device contexts ---- */

static uint64_t u_GetDC(const uint32_t* a, void* u) {
    const window_t* w = window_of(a[0]);
    int cw, ch;
    (void)u;
    if (!w) return 0;
    client_of(w, &cw, &ch);
    return lx_gdi_window_dc(a[0], cw, ch);
}

static uint64_t u_ReleaseDC(const uint32_t* a, void* u) {
    (void)u;
    return lx_gdi_release_dc(a[1]);
}

static uint64_t u_BeginPaint(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    uint32_t ps[16];
    int cw, ch;
    (void)u;
    if (!w) return 0;
    client_of(w, &cw, &ch);
    memset(ps, 0, sizeof ps);
    ps[0] = lx_gdi_window_dc(a[0], cw, ch);
    ps[1] = w->needs_erase;
    ps[4] = (uint32_t)cw, ps[5] = (uint32_t)ch;
    if (w->needs_erase) {
        w->needs_erase = 0;
        send_to(w, WM_ERASEBKGND, ps[0], 0);
    }
    gp_copy_out(a[1], ps, sizeof ps);
    return ps[0];
}

static uint64_t u_EndPaint(const uint32_t* a, void* u) {
    window_t* w = window_of(a[0]);
    uint32_t hdc = 0;
    (void)u;
    if (!w) return 0;
    gp_read_u32(a[1], &hdc);
    w->needs_paint = 0;
    lx_gdi_present_window(a[0]);
    lx_gdi_release_dc(hdc);
    return 1;
}

static uint64_t u_FillRect(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    return read_rect(a[1], r) && lx_gdi_fill_rect(a[0], r, a[2]);
}

static uint64_t u_DrawTextExA(const uint32_t* a, void* u) {
    int32_t r[4], margins[2] = { 0, 0 };
    uint32_t params[5];
    int h;
    (void)u;
    if (!read_rect(a[3], r)) return 0;
    if (a[5]) {
        if (!gp_copy_in(params, a[5], sizeof params) || params[0] != sizeof params) return 0;
        margins[0] = (int32_t)params[2], margins[1] = (int32_t)params[3];
    }
    h = lx_gdi_draw_text(a[0], a[1], (int32_t)a[2], r, a[4], a[5] ? margins : NULL);
    if (!h) return 0;
    if (a[4] & 0x400) write_rect(a[3], r);
    if (a[5]) gp_write_u32(a[5] + 16, (uint32_t)((int32_t)a[2] < 0 ? 0 : (int32_t)a[2]));
    return (uint32_t)h;
}

static uint64_t u_DrawEdge(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    if (!read_rect(a[1], r) || !lx_gdi_draw_edge(a[0], r, a[2], a[3])) return 0;
    return (a[3] & 0x2000) ? write_rect(a[1], r) : 1;
}

/* ---- rectangles ---- */

static uint64_t u_SetRect(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    r[0] = (int32_t)a[1], r[1] = (int32_t)a[2], r[2] = (int32_t)a[3], r[3] = (int32_t)a[4];
    return write_rect(a[0], r);
}

static uint64_t u_OffsetRect(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    if (!read_rect(a[0], r)) return 0;
    r[0] += (int32_t)a[1], r[2] += (int32_t)a[1], r[1] += (int32_t)a[2], r[3] += (int32_t)a[2];
    return write_rect(a[0], r);
}

static uint64_t u_InflateRect(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    if (!read_rect(a[0], r)) return 0;
    r[0] -= (int32_t)a[1], r[2] += (int32_t)a[1], r[1] -= (int32_t)a[2], r[3] += (int32_t)a[2];
    return write_rect(a[0], r);
}

static int rect_empty(const int32_t r[4]) { return r[0] >= r[2] || r[1] >= r[3]; }

static uint64_t u_IntersectRect(const uint32_t* a, void* u) {
    int32_t x[4], y[4], d[4] = { 0, 0, 0, 0 };
    int ok;
    (void)u;
    if (!read_rect(a[1], x) || !read_rect(a[2], y)) return 0;
    d[0] = x[0] > y[0] ? x[0] : y[0], d[1] = x[1] > y[1] ? x[1] : y[1];
    d[2] = x[2] < y[2] ? x[2] : y[2], d[3] = x[3] < y[3] ? x[3] : y[3];
    ok = !rect_empty(d);
    if (!ok) memset(d, 0, sizeof d);
    write_rect(a[0], d);
    return (uint64_t)ok;
}

static uint64_t u_UnionRect(const uint32_t* a, void* u) {
    int32_t x[4], y[4], d[4];
    (void)u;
    if (!read_rect(a[1], x) || !read_rect(a[2], y)) return 0;
    if (rect_empty(x) && rect_empty(y)) {
        memset(d, 0, sizeof d);
        write_rect(a[0], d);
        return 0;
    }
    if (rect_empty(x)) memcpy(d, y, sizeof d);
    else if (rect_empty(y)) memcpy(d, x, sizeof d);
    else {
        d[0] = x[0] < y[0] ? x[0] : y[0], d[1] = x[1] < y[1] ? x[1] : y[1];
        d[2] = x[2] > y[2] ? x[2] : y[2], d[3] = x[3] > y[3] ? x[3] : y[3];
    }
    write_rect(a[0], d);
    return 1;
}

static uint64_t u_PtInRect(const uint32_t* a, void* u) {
    int32_t r[4];
    (void)u;
    if (!read_rect(a[0], r)) return 0;
    return (int32_t)a[1] >= r[0] && (int32_t)a[1] < r[2] && (int32_t)a[2] >= r[1] && (int32_t)a[2] < r[3];
}

/* ---- text, strings, message boxes ---- */

static int resource_string(uint32_t hinst, uint32_t id, char* out, size_t cap) {
    uint32_t data, size, p;
    uint16_t len;
    if (!lx_resource_find(hinst, 6, id / 16 + 1, &data, &size)) return 0;
    p = data;
    for (uint32_t i = 0; i < id % 16; i++) {
        if (!gp_copy_in(&len, p, 2)) return 0;
        p += 2u + (uint32_t)len * 2u;
    }
    if (!gp_copy_in(&len, p, 2)) return 0;
    out[0] = 0;
    for (uint32_t i = 0; i < len && i + 1 < cap; i++) {
        uint16_t ch;
        if (!gp_copy_in(&ch, p + 2 + i * 2, 2)) return 0;
        out[i] = (char)(ch < 0x100 ? ch : '?');
        out[i + 1] = 0;
    }
    return 1;
}

static uint64_t u_LoadStringA(const uint32_t* a, void* u) {
    char text[512];
    char* out;
    size_t n;
    (void)u;
    if (a[3] && (out = (char*)gm_ptr_checked(a[2], a[3], 1)) != NULL) {
        int translated = l10n_load_string(a[1], out, (int)a[3]);
        if (translated >= 0) return (uint32_t)translated;
    }
    if (!a[3] || !resource_string(a[0], a[1], text, sizeof text)) return 0;
    n = strlen(text);
    if (n >= a[3]) n = a[3] - 1;
    out = (char*)gm_ptr_checked(a[2], (uint32_t)n + 1, 1);
    if (!out) return 0;
    memcpy(out, text, n);
    out[n] = 0;
    return n;
}

static size_t format_win32(char* out, size_t cap, const char* fmt, const uint32_t* args, int nargs, uint32_t list_va) {
    size_t n = 0;
    int ai = 0;
#define NEXT_ARG(dst) do { \
        if (list_va) { if (!gp_read_u32(list_va + 4u * (uint32_t)ai, &(dst))) (dst) = 0; } \
        else (dst) = ai < nargs ? args[ai] : 0; \
        ai++; } while (0)
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (const char* p = fmt; *p; p++) {
        char spec[16], tmp[64];
        int sl = 0;
        uint32_t v = 0;
        if (*p != '%') {
            PUT(*p);
            continue;
        }
        spec[sl++] = *p++;
        while (*p && strchr("-+ #0123456789.", *p) && sl < 12) spec[sl++] = *p++;
        if (*p == '*') {
            NEXT_ARG(v);
            sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", (int)v);
            p++;
        }
        while (*p == 'l' || *p == 'h') p++;
        if (*p == '%') {
            PUT('%');
            continue;
        }
        if (!*p) break;
        NEXT_ARG(v);
        if (*p == 's') {
            const char* s = v ? gp_string(v, 4096) : "(null)";
            spec[sl++] = 's', spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, s ? s : "(null)");
        } else if (*p == 'c') {
            spec[sl++] = 'c', spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)v);
        } else {
            spec[sl++] = *p, spec[sl] = 0;
            if (*p == 'd' || *p == 'i') snprintf(tmp, sizeof tmp, spec, (int)v);
            else snprintf(tmp, sizeof tmp, spec, (unsigned)v);
        }
        for (const char* t = tmp; *t; t++) PUT(*t);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
#undef NEXT_ARG
    return n;
}

static uint64_t u_wsprintfA(const uint32_t* a, void* u) {
    char text[1024];
    const char* fmt = gp_string(a[1], 1024);
    size_t n;
    char* out;
    (void)u;
    if (!fmt) return 0;
    n = format_win32(text, sizeof text, fmt, a + 2, 14, 0);
    out = (char*)gm_ptr_checked(a[0], (uint32_t)n + 1, 1);
    if (!out) return 0;
    memcpy(out, text, n + 1);
    return n;
}

static uint64_t u_wvsprintfA(const uint32_t* a, void* u) {
    char text[1024];
    const char* fmt = gp_string(a[1], 1024);
    size_t n;
    char* out;
    (void)u;
    if (!fmt) return 0;
    n = format_win32(text, sizeof text, fmt, NULL, 0, a[2]);
    out = (char*)gm_ptr_checked(a[0], (uint32_t)n + 1, 1);
    if (!out) return 0;
    memcpy(out, text, n + 1);
    return n;
}

static uint64_t u_MessageBoxA(const uint32_t* a, void* u) {
    const char* text = a[1] ? gp_string(a[1], 1024) : "";
    const char* caption = a[2] ? gp_string(a[2], 256) : "";
    uint32_t kind = a[3] & MB_TYPEMASK;
    (void)u;
    lx_log("Linux USER32", "MessageBox \"%s\": %s", caption ? caption : "", text ? text : "");
    switch (kind) {
    case 2: return 3;
    case 3: case 4: return 6;
    case 5: return 4;
    default: return 1;
    }
}

#define U32(fn, argc) { "user32.dll", #fn, u_##fn, argc, HT_STDCALL }

static const hle_entry_t k_table[] = {
    U32(RegisterClassA, 1), U32(RegisterWindowMessageA, 1), U32(CreateWindowExA, 12), U32(DestroyWindow, 1), U32(ShowWindow, 2),
    U32(UpdateWindow, 1), U32(InvalidateRect, 3), U32(ValidateRect, 2), U32(SetWindowPos, 7), U32(MoveWindow, 6),
    U32(GetWindowRect, 2), U32(GetClientRect, 2), U32(ClientToScreen, 2), U32(GetWindowPlacement, 2), U32(GetWindowLongA, 2),
    U32(SetWindowLongA, 3), U32(SetWindowTextA, 2), U32(GetParent, 1), U32(GetActiveWindow, 0), U32(SetActiveWindow, 1),
    U32(IsIconic, 1), U32(SetCapture, 1), U32(ReleaseCapture, 0), U32(EnumWindows, 2), U32(GetSystemMenu, 2),
    U32(EnableMenuItem, 3), U32(PeekMessageA, 5), U32(TranslateMessage, 1), U32(TranslateAcceleratorA, 3),
    U32(DispatchMessageA, 1), U32(PostQuitMessage, 1), U32(SendMessageA, 4), U32(DefWindowProcA, 4), U32(SetTimer, 4),
    U32(KillTimer, 2), U32(GetCursorPos, 1), U32(SetCursorPos, 2), U32(ShowCursor, 1), U32(SetCursor, 1), U32(ClipCursor, 1),
    U32(GetForegroundWindow, 0), U32(GetAsyncKeyState, 1), U32(GetKeyState, 1), U32(GetSystemMetrics, 1), U32(LoadIconA, 2),
    U32(LoadCursorA, 2), U32(GetDC, 1), U32(ReleaseDC, 2), U32(BeginPaint, 2), U32(EndPaint, 2), U32(FillRect, 3), U32(DrawTextExA, 6), U32(DrawEdge, 4),
    U32(SetRect, 5), U32(OffsetRect, 3), U32(InflateRect, 3), U32(IntersectRect, 3), U32(UnionRect, 3), U32(PtInRect, 3),
    U32(LoadStringA, 4), U32(wvsprintfA, 3), U32(MessageBoxA, 4),
    { "user32.dll", "wsprintfA", u_wsprintfA, 16, HT_CDECL },
    { "comctl32.dll", "#17", u_InitCommonControls, 0, HT_STDCALL },
};

const hle_entry_t* hle_user32_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
