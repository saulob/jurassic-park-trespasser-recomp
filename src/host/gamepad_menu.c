/*
 * Gamepad focus in the game's menus. The game's UI has no keyboard focus: its controls answer the pointer it reads
 * each frame and the clicks its window gets. So focus is the game's own pointer placed on a control (the control
 * shows its own hover picture) and A is a click there; sliders are dragged and lists scrolled with their own arrows.
 */
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "frontend_layout.h"
#include "gamepad.h"

#define WND_CONTROLS_BEGIN 0x20u
#define WND_CONTROLS_END 0x24u
#define CTRL_FLAGS 0x08u
#define CTRL_ID 0x0Cu
#define CTRL_RECT 0x10u
#define CTRL_ENABLED 2u
#define CTRL_VISIBLE 4u
#define MAX_CONTROLS 160u

#define VT_BUTTON 0x0064554Cu
#define VT_LISTBOX 0x00645650u
#define VT_CHECKBOX 0x00645758u
#define VT_SLIDER 0x00645864u
#define VT_EDITBOX 0x006458DCu
#define VT_HOTSPOT 0x006459CCu
#define VT_CONTROLS_WND 0x00645F38u
#define VT_SAVE_WND 0x006462A8u

#define SLIDER_UNITS 0x2Cu
#define SLIDER_VALUE 0x30u
#define SLIDER_THUMB 0x40u
#define RASTER_W 0x08u
#define EDIT_TEXT 0x24u
#define LIST_ITEMS_BEGIN 0x24u
#define LIST_ITEMS_END 0x28u
#define LIST_UP 0x30u
#define LIST_DOWN 0x34u
#define LIST_SELECTED 0x3Cu
#define LIST_TOP 0x40u
#define LIST_ROWS 0x44u
#define LIST_BUTTON_W 0x48u
#define LIST_LINE_H 0x4Cu
#define LIST_ITEM_SIZE 12u

/* CControlsWnd's key remapping hotspots: a click there makes the next key or button the binding. */
#define REMAP_FIRST_ID 1010
#define REMAP_LAST_ID 1024
#define SAVE_NAME_ID 1004
#define SAVE_NAME_MAX 21

#define MAX_TARGETS 96
#define MAX_LISTS 4
#define MAX_STEPS 6
#define POINTER_SPEED 420.0f
#define SLIDER_STEPS 10

typedef enum { T_BUTTON, T_CHECK, T_SLIDER, T_ROW, T_EDIT, T_HOTSPOT } target_kind_t;

typedef struct {
    pad_rect_t rect;
    uint8_t kind;
    uint32_t control;
    int row;
} target_t;

typedef struct {
    uint32_t control, up, down;
    pad_rect_t rows; /* the row area, the scroll column left out */
    int top, shown, count, row_h, selected;
} list_t;

typedef struct {
    target_t targets[MAX_TARGETS];
    int count;
    list_t lists[MAX_LISTS];
    int list_count;
    uint32_t window, window_vt;
} scan_t;

enum { MOUSE_NONE = -1 };

typedef struct {
    int x, y, set_pointer, mouse, then_up;
} step_t;

static struct {
    uint32_t window;
    uint32_t focus_control;
    int focus_row, has_focus, auto_focus, fresh;
    pad_nav_dir_t stick_dir;
    pad_repeat_t repeat;
    step_t steps[MAX_STEPS];
    int step_count, step_at;
    int a_down;
    uint32_t named_window;
    float px, py;
    int pointer_free;
} M;

static uint32_t peek32(uint32_t va) {
    uint32_t v = 0;
    return guest_read32(va, &v) ? v : 0;
}

static int read_rect(uint32_t va, pad_rect_t* r) {
    uint32_t v[4];
    for (int i = 0; i < 4; i++)
        if (!guest_read32(va + 4u * (uint32_t)i, &v[i])) return 0;
    r->left = (int32_t)v[0];
    r->top = (int32_t)v[1];
    r->right = (int32_t)v[2];
    r->bottom = (int32_t)v[3];
    return r->right > r->left && r->bottom > r->top;
}

static int centre_x(pad_rect_t r) { return (r.left + r.right) / 2; }
static int centre_y(pad_rect_t r) { return (r.top + r.bottom) / 2; }
static int inside(pad_rect_t r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static void add_target(scan_t* s, int kind, uint32_t control, pad_rect_t rect, int row) {
    target_t* t;
    if (s->count >= MAX_TARGETS) return;
    t = &s->targets[s->count++];
    t->kind = (uint8_t)kind;
    t->control = control;
    t->rect = rect;
    t->row = row;
}

static int read_list(uint32_t c, pad_rect_t rect, list_t* l) {
    uint32_t begin = peek32(c + LIST_ITEMS_BEGIN), end = peek32(c + LIST_ITEMS_END);
    int32_t line_h = (int32_t)peek32(c + LIST_LINE_H), side = (int32_t)peek32(c + LIST_BUTTON_W);
    int rows = (int32_t)peek32(c + LIST_ROWS);
    if (end < begin || (end - begin) % LIST_ITEM_SIZE || line_h <= 0 || line_h > 200 || rows <= 0 || rows > 64) return 0;
    memset(l, 0, sizeof *l);
    l->control = c;
    l->up = peek32(c + LIST_UP);
    l->down = peek32(c + LIST_DOWN);
    l->count = (int)((end - begin) / LIST_ITEM_SIZE);
    l->top = (int32_t)peek32(c + LIST_TOP);
    if (l->top < 0) l->top = 0;
    l->selected = (int32_t)peek32(c + LIST_SELECTED);
    l->row_h = line_h;
    l->shown = l->count - l->top < rows ? l->count - l->top : rows;
    if (l->shown < 0) l->shown = 0;
    if (side <= 0 || side > 64) side = 16;
    l->rows = rect;
    l->rows.right -= side;
    return 1;
}

static pad_rect_t row_rect(const list_t* l, int item) {
    pad_rect_t r = l->rows;
    r.top = l->rows.top + (item - l->top) * l->row_h;
    r.bottom = r.top + l->row_h;
    return r;
}

static const list_t* list_of(const scan_t* s, uint32_t control) {
    for (int i = 0; i < s->list_count; i++)
        if (s->lists[i].control == control) return &s->lists[i];
    return NULL;
}

static int is_arrow(const scan_t* s, uint32_t control) {
    for (int i = 0; i < s->list_count; i++)
        if (control && (s->lists[i].up == control || s->lists[i].down == control)) return 1;
    return 0;
}

static int read_controls(uint32_t window, uint32_t* out, unsigned* count) {
    uint32_t begin = peek32(window + WND_CONTROLS_BEGIN), end = peek32(window + WND_CONTROLS_END);
    if (!begin || end < begin || (end - begin) % 4 || (end - begin) / 4 > MAX_CONTROLS || !guest_readable(begin, end - begin))
        return 0;
    *count = (end - begin) / 4;
    for (unsigned i = 0; i < *count; i++) out[i] = ((const uint32_t*)(uintptr_t)begin)[i];
    return 1;
}

static int covers_checkbox(const uint32_t* controls, unsigned count, pad_rect_t hotspot) {
    for (unsigned i = 0; i < count; i++) {
        pad_rect_t r;
        uint32_t flags = peek32(controls[i] + CTRL_FLAGS);
        if (peek32(controls[i]) != VT_CHECKBOX || (flags & (CTRL_VISIBLE | CTRL_ENABLED)) != (CTRL_VISIBLE | CTRL_ENABLED) ||
            !read_rect(controls[i] + CTRL_RECT, &r))
            continue;
        if (inside(hotspot, centre_x(r), centre_y(r))) return 1;
    }
    return 0;
}

static int scan_window(uint32_t window, scan_t* s) {
    uint32_t controls[MAX_CONTROLS];
    unsigned count;
    memset(s, 0, sizeof *s);
    s->window = window;
    s->window_vt = peek32(window);
    if (!window || !read_controls(window, controls, &count)) return 0;
    for (unsigned i = 0; i < count && s->list_count < MAX_LISTS; i++) {
        pad_rect_t r;
        if (peek32(controls[i]) == VT_LISTBOX && (peek32(controls[i] + CTRL_FLAGS) & CTRL_VISIBLE) &&
            read_rect(controls[i] + CTRL_RECT, &r) && read_list(controls[i], r, &s->lists[s->list_count]))
            s->list_count++;
    }
    for (unsigned i = 0; i < count; i++) {
        uint32_t c = controls[i], vt = peek32(c), flags = peek32(c + CTRL_FLAGS);
        int32_t id = (int32_t)peek32(c + CTRL_ID);
        pad_rect_t r;
        if (!(flags & CTRL_VISIBLE) || !read_rect(c + CTRL_RECT, &r)) continue;
        switch (vt) {
        case VT_BUTTON:
            if ((flags & CTRL_ENABLED) && !is_arrow(s, c)) add_target(s, T_BUTTON, c, r, 0);
            break;
        case VT_CHECKBOX:
            if (flags & CTRL_ENABLED) add_target(s, T_CHECK, c, r, 0);
            break;
        case VT_SLIDER:
            if (flags & CTRL_ENABLED) add_target(s, T_SLIDER, c, r, 0);
            break;
        case VT_EDITBOX: add_target(s, T_EDIT, c, r, 0); break;
        case VT_HOTSPOT:
            if (!(flags & CTRL_ENABLED) || covers_checkbox(controls, count, r)) break;
            if (s->window_vt == VT_CONTROLS_WND && id >= REMAP_FIRST_ID && id <= REMAP_LAST_ID) break;
            add_target(s, T_HOTSPOT, c, r, 0);
            break;
        case VT_LISTBOX: {
            const list_t* l = list_of(s, c);
            for (int k = 0; l && k < l->shown; k++) add_target(s, T_ROW, c, row_rect(l, l->top + k), l->top + k);
            break;
        }
        default: break;
        }
    }
    return 1;
}

static int find_focus(const scan_t* s) {
    if (!M.has_focus) return -1;
    for (int i = 0; i < s->count; i++)
        if (s->targets[i].control == M.focus_control && (s->targets[i].kind != T_ROW || s->targets[i].row == M.focus_row))
            return i;
    return -1;
}

static void set_focus(const target_t* t) {
    M.has_focus = 1;
    M.focus_control = t->control;
    M.focus_row = t->row;
    M.pointer_free = 0;
}

static void point_at(int x, int y) {
    M.px = (float)x;
    M.py = (float)y;
    window_host_set_pad_cursor(x, y);
}

static void queue(int x, int y, int set_pointer, int mouse, int then_up) {
    step_t* st;
    if (M.step_count >= MAX_STEPS) return;
    st = &M.steps[M.step_count++];
    st->x = x;
    st->y = y;
    st->set_pointer = set_pointer;
    st->mouse = mouse;
    st->then_up = then_up;
}

static void run_step(void) {
    const step_t* st = &M.steps[M.step_at++];
    if (st->set_pointer) point_at(st->x, st->y);
    if (st->mouse != MOUSE_NONE) window_host_post_mouse(st->mouse);
    if (st->then_up) window_host_post_mouse(PAD_MOUSE_UP);
    if (M.step_at >= M.step_count) M.step_count = M.step_at = 0;
}

static void click_at(pad_rect_t r) { queue(centre_x(r), centre_y(r), 1, PAD_MOUSE_DOWN, 1); }

static int initial_target(const scan_t* s) {
    int x, y, best = -1;
    if (window_host_game_cursor(&x, &y))
        for (int i = 0; i < s->count; i++)
            if (inside(s->targets[i].rect, x, y)) return i;
    for (int l = 0; l < s->list_count; l++)
        for (int i = 0; i < s->count; i++)
            if (s->targets[i].kind == T_ROW && s->targets[i].control == s->lists[l].control &&
                (s->targets[i].row == s->lists[l].selected || best < 0))
                best = i;
    if (best >= 0) return best;
    for (int i = 0; i < s->count; i++) {
        const pad_rect_t a = s->targets[i].rect;
        if (best < 0 || a.top < s->targets[best].rect.top ||
            (a.top == s->targets[best].rect.top && a.left < s->targets[best].rect.left))
            best = i;
    }
    return best;
}

static void focus_initial(const scan_t* s) {
    int i = initial_target(s);
    if (i < 0) return;
    set_focus(&s->targets[i]);
    point_at(centre_x(s->targets[i].rect), centre_y(s->targets[i].rect));
}

/* Into a list from outside, focus lands on its selected row (or the nearest one): the selection does not change. */
static int entry_row(const scan_t* s, int picked) {
    const list_t* l = list_of(s, s->targets[picked].control);
    if (!l || l->selected < l->top || l->selected >= l->top + l->shown) return picked;
    for (int i = 0; i < s->count; i++)
        if (s->targets[i].kind == T_ROW && s->targets[i].control == l->control && s->targets[i].row == l->selected) return i;
    return picked;
}

static int list_step(const scan_t* s, const target_t* t, pad_nav_dir_t dir) {
    const list_t* l = list_of(s, t->control);
    int next = t->row + (dir == PAD_NAV_DOWN ? 1 : -1);
    uint32_t arrow;
    pad_rect_t ar;
    if (!l || next < 0 || next >= l->count) return 0;
    M.focus_row = next;
    if (next >= l->top && next < l->top + l->shown) {
        click_at(row_rect(l, next));
        return 1;
    }
    arrow = dir == PAD_NAV_DOWN ? l->down : l->up;
    if (!arrow || !read_rect(arrow + CTRL_RECT, &ar)) {
        M.focus_row = t->row;
        return 0;
    }
    queue(centre_x(ar), centre_y(ar), 1, PAD_MOUSE_DOWN, 1);
    click_at(t->rect);
    return 1;
}

static void slider_step(const target_t* t, pad_nav_dir_t dir) {
    uint32_t thumb = peek32(t->control + SLIDER_THUMB);
    int thumb_w = thumb ? (int)peek32(thumb + RASTER_W) : 10, y = centre_y(t->rect), from, to, step;
    fe_rect_t track = fe_rect(t->rect.left, t->rect.top, t->rect.right, t->rect.bottom);
    if (thumb_w <= 0 || thumb_w > 64) thumb_w = 10;
    from = fe_slider_thumb_x(track, thumb_w, (int32_t)peek32(t->control + SLIDER_VALUE), (int)peek32(t->control + SLIDER_UNITS));
    step = (t->rect.right - t->rect.left - thumb_w) / SLIDER_STEPS;
    if (step < 2) step = 2;
    to = from + (dir == PAD_NAV_RIGHT ? step : -step);
    if (to < t->rect.left + thumb_w / 2) to = t->rect.left + thumb_w / 2;
    if (to > t->rect.right - (thumb_w + 1) / 2) to = t->rect.right - (thumb_w + 1) / 2;
    queue(from, y, 1, PAD_MOUSE_DOWN, 0);
    queue(to, y, 1, PAD_MOUSE_MOVE, 0);
    queue(to, y, 1, PAD_MOUSE_UP, 0);
}

static void navigate(const scan_t* s, pad_nav_dir_t dir) {
    pad_rect_t rects[MAX_TARGETS];
    int index[MAX_TARGETS], n = 0, cur = find_focus(s), next;
    const target_t* t;
    if (cur < 0) {
        focus_initial(s);
        return;
    }
    t = &s->targets[cur];
    if (t->kind == T_SLIDER && (dir == PAD_NAV_LEFT || dir == PAD_NAV_RIGHT)) {
        slider_step(t, dir);
        return;
    }
    if (t->kind == T_ROW && (dir == PAD_NAV_UP || dir == PAD_NAV_DOWN) && list_step(s, t, dir)) return;
    for (int i = 0; i < s->count; i++) {
        if (t->kind == T_ROW && s->targets[i].kind == T_ROW && s->targets[i].control == t->control) continue;
        rects[n] = s->targets[i].rect;
        index[n++] = i;
    }
    next = pad_nav_pick(rects, n, t->rect, dir);
    if (next < 0) return;
    next = index[next];
    if (s->targets[next].kind == T_ROW) next = entry_row(s, next);
    set_focus(&s->targets[next]);
    point_at(centre_x(s->targets[next].rect), centre_y(s->targets[next].rect));
}

static const char* edit_text(uint32_t edit, char* buf, int cap) {
    if (!guest_read_string(peek32(edit + EDIT_TEXT), buf, cap)) buf[0] = 0;
    return buf;
}

/* CSaveGameWnd saves only under a typed name: the gamepad types one, through the window's own WM_CHAR handling. */
static void type_save_name(void) {
    SYSTEMTIME t;
    char name[SAVE_NAME_MAX + 1];
    GetLocalTime(&t);
    _snprintf(name, sizeof name - 1, "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    name[sizeof name - 1] = 0;
    for (const char* p = name; *p; p++) window_host_post_char(*p);
}

static uint32_t save_name_box(const scan_t* s) {
    for (int i = 0; i < s->count; i++)
        if (s->targets[i].kind == T_EDIT && (int32_t)peek32(s->targets[i].control + CTRL_ID) == SAVE_NAME_ID)
            return s->targets[i].control;
    return 0;
}

static void name_new_save(const scan_t* s, int device_gamepad) {
    char text[64];
    uint32_t box;
    if (!device_gamepad || s->window_vt != VT_SAVE_WND || M.named_window == s->window) return;
    box = save_name_box(s);
    if (!box || s->list_count < 1 || s->lists[0].selected != 0 || edit_text(box, text, sizeof text)[0]) return;
    M.named_window = s->window;
    type_save_name();
}

static void activate(const scan_t* s) {
    int cur = find_focus(s);
    const target_t* t;
    char text[64];
    if (cur < 0) {
        if (!M.pointer_free) return;
        window_host_post_mouse(PAD_MOUSE_DOWN);
        M.a_down = 1;
        return;
    }
    t = &s->targets[cur];
    point_at(centre_x(t->rect), centre_y(t->rect));
    if (t->kind == T_ROW) {
        const list_t* l = list_of(s, t->control);
        window_host_post_mouse(l && l->selected == t->row ? PAD_MOUSE_DOUBLE : PAD_MOUSE_DOWN);
        window_host_post_mouse(PAD_MOUSE_UP);
        return;
    }
    if (t->kind == T_EDIT && s->window_vt == VT_SAVE_WND && !edit_text(t->control, text, sizeof text)[0]) {
        type_save_name();
        return;
    }
    window_host_post_mouse(PAD_MOUSE_DOWN);
    M.a_down = 1;
}

static void move_pointer(const pad_state_t* pad, float dt, float deadzone) {
    float x, y, m;
    pad_stick_shape(pad->rx, pad->ry, deadzone + 0.05f, &x, &y);
    m = sqrtf(x * x + y * y);
    if (m <= 0.0f || dt <= 0.0f) return;
    if (!M.pointer_free) {
        int gx, gy;
        if (window_host_game_cursor(&gx, &gy)) {
            M.px = (float)gx;
            M.py = (float)gy;
        }
    }
    M.has_focus = 0;
    M.pointer_free = 1;
    M.px += x / m * pad_look_curve(m) * POINTER_SPEED * dt;
    M.py += y / m * pad_look_curve(m) * POINTER_SPEED * dt;
    window_host_set_pad_cursor((int)floorf(M.px + 0.5f), (int)floorf(M.py + 0.5f));
}

void gamepad_menu_reset(void) {
    M.has_focus = 0;
    M.auto_focus = 1;
    M.fresh = 1;
    M.step_count = M.step_at = 0;
    M.pointer_free = 0;
    M.a_down = 0;
}

void gamepad_menu_forget_focus(void) {
    M.has_focus = 0;
    M.auto_focus = 0;
    M.pointer_free = 0;
}

void gamepad_menu_frame(const pad_state_t* pad, uint32_t held, uint32_t pressed, uint32_t released, int device_gamepad,
                        uint32_t now_ms, float dt, float stick_deadzone) {
    guest_ui_view_t v;
    scan_t s;
    pad_nav_dir_t dir, fire;
    int woke = 0;
    if (!guest_ui_probe(&v) || !scan_window(v.top, &s)) return;
    if (M.window != v.top) {
        M.window = v.top;
        gamepad_menu_reset();
    }
    if (M.step_count) {
        run_step();
        return;
    }
    if (M.a_down && (released & JPT_PAD_BIT(JPT_PAD_SOUTH))) {
        window_host_post_mouse(PAD_MOUSE_UP);
        M.a_down = 0;
    }
    if (pressed & (JPT_PAD_BIT(JPT_PAD_EAST) | JPT_PAD_BIT(JPT_PAD_START))) {
        window_host_post_key(VK_ESCAPE);
        return;
    }
    if (device_gamepad && M.auto_focus && s.count) {
        M.auto_focus = 0;
        focus_initial(&s);
        woke = 1;
    }
    name_new_save(&s, device_gamepad);
    dir = pad_nav_direction(held, pad->lx, pad->ly, M.stick_dir);
    M.stick_dir = dir;
    if (M.fresh) {
        M.fresh = 0;
        pad_repeat_reset(&M.repeat);
        M.repeat.dir = dir;
        M.repeat.next = now_ms + PAD_REPEAT_DELAY_MS;
    }
    fire = pad_repeat_update(&M.repeat, dir, now_ms);
    if (woke || M.a_down) return;
    move_pointer(pad, dt, stick_deadzone);
    if (fire != PAD_NAV_NONE) navigate(&s, fire);
    if (pressed & JPT_PAD_BIT(JPT_PAD_SOUTH)) {
        if (find_focus(&s) < 0 && !M.pointer_free) focus_initial(&s);
        else activate(&s);
    }
}
