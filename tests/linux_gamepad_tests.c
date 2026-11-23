/* The Linux gamepad layer over SDL3's own virtual joysticks (no hardware, no window, nothing vibrates): connection and
 * hot-plug, every control of the standard layout reaching the policy as the control it is, the gameplay input the game's
 * CInput read gets (move, look as cursor motion, the actions' command bits, Start as Escape), nothing else becoming a key,
 * the controller in use, rumble through the device's own callback, and a clean shutdown. */
#include <stdio.h>
#include <string.h>

#include "active_profile.h"
#include "gamepad.h"
#include "host_input.h"
#include "jpt_game_version.h"
#include "jpt_sync.h"
#include "linux_card.h"
#include "linux_gamepad.h"
#include "tutorial_prompts.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"

#define SINPUT 0x08u
#define HOTPLUG_WAIT_MS 260u

static uint32_t s_cinput;
enum { STICK_LX, STICK_LY, STICK_RX, STICK_RY, TRIGGER_L, TRIGGER_R };

typedef struct {
    int attached;
    uint32_t id;
    char name[64];
    pad_state_t state;
} fake_pad_t;

static fake_pad_t s_pads[4];
static int s_opened, s_closed;
static unsigned s_rumble_low, s_rumble_high, s_rumble_calls;
static uint32_t s_next_id = 100;

static int fake_init(void) { return 1; }
static void fake_quit(void) {}
static void fake_update(void) {}

static int fake_list(uint32_t* ids, int capacity) {
    int n = 0;
    for (int i = 0; i < 4 && n < capacity; i++)
        if (s_pads[i].attached) ids[n++] = s_pads[i].id;
    return n;
}

static void* fake_open(uint32_t id) {
    for (int i = 0; i < 4; i++)
        if (s_pads[i].attached && s_pads[i].id == id) return s_opened++, &s_pads[i];
    return NULL;
}

static void fake_close(void* h) {
    (void)h;
    s_closed++;
}

static void fake_read(void* h, pad_state_t* out) { *out = ((fake_pad_t*)h)->state; }

static void fake_info(void* h, lx_pad_info_t* out) {
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", ((fake_pad_t*)h)->name);
    out->type = JPT_PAD_TYPE_XBOXONE, out->vendor = 0x045E, out->product = 0x02FF, out->rumble = 1;
}

static int fake_rumble(void* h, uint16_t low, uint16_t high, unsigned ms) {
    (void)h, (void)ms;
    s_rumble_low = low, s_rumble_high = high, s_rumble_calls++;
    return 1;
}

static const lx_pad_backend_t k_fake = { fake_init, fake_quit, fake_update, fake_list, fake_open, fake_close, fake_read, fake_info, fake_rumble };

static fake_pad_t* attach(const char* name) {
    for (int i = 0; i < 4; i++) {
        if (s_pads[i].attached) continue;
        memset(&s_pads[i], 0, sizeof s_pads[i]);
        s_pads[i].attached = 1, s_pads[i].id = s_next_id++;
        snprintf(s_pads[i].name, sizeof s_pads[i].name, "%s", name);
        return &s_pads[i];
    }
    return NULL;
}

static void detach(fake_pad_t* p) { p->attached = 0; }

static void settle(void) {
    jpt_sleep_ms(HOTPLUG_WAIT_MS);
    lx_gamepad_poll();
}

static void press(fake_pad_t* p, int control, int down) {
    if (down) p->state.buttons |= JPT_PAD_BIT(control);
    else p->state.buttons &= ~JPT_PAD_BIT(control);
}

static void axis(fake_pad_t* p, int which, float v) {
    float* slot[6] = { &p->state.lx, &p->state.ly, &p->state.rx, &p->state.ry, &p->state.lt, &p->state.rt };
    *slot[which] = v;
}

static void release_everything(fake_pad_t* p) { memset(&p->state, 0, sizeof p->state); }

static float read_f(uint32_t va) {
    float f;
    memcpy(&f, gm_ptr(va, 4), 4);
    return f;
}

static uint32_t read_u(uint32_t va) {
    uint32_t v;
    memcpy(&v, gm_ptr(va, 4), 4);
    return v;
}

/* The original CInput read: what the keyboard and mouse alone would leave in SInput. */
static void original_read(void) {
    memset(gm_ptr(s_cinput + SINPUT, 0x20), 0, 0x20);
}

/* One gameplay frame: the hook around the original, 30 ms apart. */
static void frame(void) {
    jpt_sleep_ms(30);
    lx_gamepad_read_around(s_cinput, original_read);
}

static double seconds_now(void) { return (double)jpt_counter() / (double)jpt_counter_frequency(); }

static uint32_t commands(void) { return read_u(s_cinput + SINPUT + 0x14); }

static int cursor_x(void) {
    uint32_t point = g_scratch + 0x100;
    api("USER32.dll", "GetCursorPos", 1, point);
    return (int)read_u(point);
}

static int cursor_y(void) {
    uint32_t point = g_scratch + 0x100;
    api("USER32.dll", "GetCursorPos", 1, point);
    return (int)read_u(point + 4);
}

#define INVERT_MOUSE_VA 0x006603D8u

/* The game's own Invert Mouse option (a byte of its state) turns the vertical of the stick-as-mouse round, as it does the mouse's. */
static void test_invert_mouse(fake_pad_t* joystick) {
    int up_off, up_on;
    double off_from, on_from, off_seconds, on_seconds, inverted;
    if (!gm_ptr_checked(INVERT_MOUSE_VA, 1, 1)) gm_alloc_pages(INVERT_MOUSE_VA & ~0xFFFFu, 0x10000u, "game state");
    release_everything(joystick);
    frame();
    off_from = seconds_now();
    lx_user32_cursor_set(200, 200);
    *(uint8_t*)gm_ptr(INVERT_MOUSE_VA, 1) = 0;
    axis(joystick, STICK_RY, 1.0f);
    for (int i = 0; i < 6; i++) frame();
    off_seconds = seconds_now() - off_from;
    up_off = cursor_y() - 200;
    axis(joystick, STICK_RY, 0.0f);
    frame();
    frame();
    on_from = seconds_now();
    lx_user32_cursor_set(200, 200);
    *(uint8_t*)gm_ptr(INVERT_MOUSE_VA, 1) = 1;
    axis(joystick, STICK_RY, 1.0f);
    for (int i = 0; i < 6; i++) frame();
    on_seconds = seconds_now() - on_from;
    up_on = cursor_y() - 200;
    axis(joystick, STICK_RY, 0.0f);
    frame();
    frame();
    *(uint8_t*)gm_ptr(INVERT_MOUSE_VA, 1) = 0;
    /* The two batches do not span the same wall time: compare the rates, not the raw counts. */
    inverted = (double)up_on * off_seconds / on_seconds;
    CHECK(up_off != 0 && inverted < 0.0 && inverted + up_off <= 2.0 && inverted + up_off >= -2.0,
          "Invert Mouse read from the game's own state flips the stick's vertical look (%d without, %d with)", up_off, up_on);
}

static void test_connection_and_mapping(fake_pad_t* joystick) {
    lx_gamepad_stats_t stats;
    const pad_state_t* s;
    CHECK(lx_gamepad_connected() == 1, "the virtual controller is connected");
    lx_gamepad_stats(&stats);
    CHECK(stats.connections == 1 && stats.disconnections == 0, "one connection counted (%u)", stats.connections);

    for (int c = 0; c < 15; c++) {
        release_everything(joystick);
        press(joystick, c, 1);
        lx_gamepad_poll();
        s = lx_gamepad_state();
        CHECK(s->buttons == JPT_PAD_BIT(c), "button %d (%s) reaches the policy as control %d alone (mask 0x%X)", c, jpt_pad_control_key(c), c, s->buttons);
    }
    release_everything(joystick);

    axis(joystick, STICK_LX, 1.0f);
    axis(joystick, STICK_LY, -1.0f);
    axis(joystick, STICK_RX, -1.0f);
    axis(joystick, STICK_RY, 1.0f);
    axis(joystick, TRIGGER_L, 1.0f);
    axis(joystick, TRIGGER_R, 0.6f);
    lx_gamepad_poll();
    s = lx_gamepad_state();
    CHECK(s->lx > 0.99f && s->ly < -0.99f && s->rx < -0.99f && s->ry > 0.99f, "the four stick axes keep SDL's directions (%.2f %.2f %.2f %.2f)", s->lx, s->ly, s->rx, s->ry);
    CHECK(s->lt > 0.99f && s->rt > 0.5f && s->rt < 0.7f, "the triggers read 0..1 (%.2f, %.2f)", s->lt, s->rt);
    release_everything(joystick);
    lx_gamepad_poll();
}

static void test_gameplay(fake_pad_t* joystick) {
    jpt_gamepad_settings_t cfg;
    lx_gamepad_stats_t before, after;
    int x0;
    jpt_gamepad_defaults(&cfg);
    release_everything(joystick);
    frame();

    axis(joystick, STICK_LY, -1.0f);
    frame();
    frame();
    CHECK(read_f(s_cinput + SINPUT + 0x10) > 0.9f, "the left stick forward is a full forward move (%.2f)", read_f(s_cinput + SINPUT + 0x10));
    axis(joystick, STICK_LY, 0.0f);
    frame();
    frame();
    CHECK(read_f(s_cinput + SINPUT + 0x10) == 0.0f, "the centred stick moves nothing");

    for (int a = 0; a < JPT_ACT_COUNT; a++) {
        int control = jpt_pad_actions[a].default_control;
        uint32_t command = jpt_pad_actions[a].command;
        if (control == JPT_PAD_NONE || a == JPT_ACT_CROUCH_TOGGLE || a == JPT_ACT_WRIST || a == JPT_ACT_ARM) continue;
        release_everything(joystick);
        frame();
        if (control < 15) press(joystick, control, 1);
        else axis(joystick, control == JPT_PAD_LEFT_TRIGGER ? TRIGGER_L : TRIGGER_R, 1.0f);
        frame();
        frame();
        CHECK(commands() & command, "%s (%s) holds its command bit 0x%X (state 0x%X)", jpt_pad_actions[a].name, jpt_pad_control_key(control), command, commands());
        release_everything(joystick);
        frame();
        frame();
    }

    release_everything(joystick);
    frame();
    x0 = cursor_x();
    axis(joystick, STICK_RX, 1.0f);
    for (int i = 0; i < 6; i++) frame();
    CHECK(cursor_x() > x0, "the right stick moves the cursor the game reads (%d -> %d)", x0, cursor_x());
    axis(joystick, STICK_RX, 0.0f);
    frame();
    x0 = cursor_x();
    frame();
    frame();
    CHECK(cursor_x() == x0, "a centred stick stops the look at once");

    release_everything(joystick);
    frame();
    lx_gamepad_stats(&before);
    press(joystick, JPT_PAD_LEFT_SHOULDER, 1);
    frame();
    press(joystick, JPT_PAD_LEFT_SHOULDER, 0);
    press(joystick, JPT_PAD_RIGHT_SHOULDER, 1);
    frame();
    press(joystick, JPT_PAD_RIGHT_SHOULDER, 0);
    press(joystick, JPT_PAD_DPAD_LEFT, 1);
    frame();
    press(joystick, JPT_PAD_DPAD_LEFT, 0);
    frame();
    lx_gamepad_stats(&after);
    CHECK(after.keys_posted == before.keys_posted, "LB, RB and the D-pad never become keyboard input (%u keys)", after.keys_posted - before.keys_posted);
    press(joystick, JPT_PAD_START, 1);
    frame();
    press(joystick, JPT_PAD_START, 0);
    frame();
    lx_gamepad_stats(&after);
    CHECK(after.keys_posted == before.keys_posted, "Start is not a key of the input read: the UI loop's shared step posts Escape (%u)", after.keys_posted - before.keys_posted);
    CHECK(lx_gamepad_device() == PAD_DEVICE_GAMEPAD, "the pad is the device in use");
    lx_gamepad_note_keyboard();
    CHECK(lx_gamepad_device() == PAD_DEVICE_KEYBOARD_MOUSE, "a key takes the prompts back");
}


/* ---- the game's menus driven by the pad: a synthetic game with the retail window layout ---- */

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define VT_BUTTON_ 0x0064554Cu
#define VT_MAIN_ 0x006466B8u
#define VT_GAME_ 0x00645C3Cu
#define WM_KEYDOWN_ 0x100u
#define WM_LBUTTONDOWN_ 0x201u
#define WM_LBUTTONUP_ 0x202u
#define CLASS_NAME_ "DWI - Trespasser"

static uint32_t s_arena, s_arena_used, s_hwnd;

static uint32_t fake(uint32_t bytes) {
    uint32_t va = s_arena + s_arena_used;
    s_arena_used += (bytes + 15u) & ~15u;
    return va;
}

static void put(uint32_t va, uint32_t v) { gp_write_u32(va, v); }

static uint32_t make_control(uint32_t vt, int id, int x, int y, int w, int h) {
    uint32_t c = fake(0xA0);
    put(c, vt);
    put(c + 0x08, 6);
    put(c + 0x0C, (uint32_t)id);
    put(c + 0x10, (uint32_t)x);
    put(c + 0x14, (uint32_t)y);
    put(c + 0x18, (uint32_t)(x + w));
    put(c + 0x1C, (uint32_t)(y + h));
    return c;
}

static uint32_t make_window(uint32_t vt, const uint32_t* controls, int n) {
    uint32_t w = fake(0x40), v = fake(4u * (uint32_t)(n > 0 ? n : 1));
    put(w, vt);
    put(w + 0x18, 640);
    put(w + 0x1C, 480);
    for (int i = 0; i < n; i++) put(v + 4u * (uint32_t)i, controls[i]);
    put(w + 0x20, v);
    put(w + 0x24, v + 4u * (uint32_t)n);
    return w;
}

static void set_stack(uint32_t window) {
    uint32_t main = fake(0x10), manager = fake(0x60), v = fake(4);
    put(v, window);
    put(main + 0x08, manager);
    put(manager + 0x20, 1);
    put(manager + 0x38, v);
    put(manager + 0x3C, v + 4);
    put(MAIN_WINDOW_OBJECT, main);
}

static void main_menu(void) {
    uint32_t c[4];
    for (int i = 0; i < 4; i++) c[i] = make_control(VT_BUTTON_, 1000 + i, 205, 70 + 90 * i, 208, 81);
    set_stack(make_window(VT_MAIN_, c, 4));
}

static void gameplay_view(void) { set_stack(make_window(VT_GAME_, NULL, 0)); }

static int button_under_cursor(void) {
    const int x = cursor_x(), y = cursor_y();
    for (int i = 0; i < 4; i++)
        if (x >= 205 && x < 413 && y >= 70 + 90 * i && y < 151 + 90 * i) return i;
    return -1;
}

typedef struct {
    uint32_t message, wparam, lparam;
} posted_t;

static int drain(posted_t* out, int cap) {
    uint32_t msg = g_scratch + 0x300;
    int n = 0;
    while (n < cap && U32(PeekMessageA, 5, msg, 0u, 0u, 0u, 1u)) {
        out[n].message = read_u(msg + 4);
        out[n].wparam = read_u(msg + 8);
        out[n].lparam = read_u(msg + 12);
        n++;
    }
    return n;
}

static int has(const posted_t* p, int n, uint32_t message, uint32_t wparam, int check_wparam) {
    for (int i = 0; i < n; i++)
        if (p[i].message == message && (!check_wparam || p[i].wparam == wparam)) return 1;
    return 0;
}

/* One UI frame of the game's loop, 30 ms after the last. */
static void ui_frame(void) {
    jpt_sleep_ms(30);
    lx_gamepad_ui_loop();
    overlay_ui_loop();
}

static void tap(fake_pad_t* p, int control) {
    press(p, control, 1);
    ui_frame();
    press(p, control, 0);
    ui_frame();
}

static void test_menu_navigation(fake_pad_t* joystick) {
    char error[256];
    const jpt_game_version_t* v = jpt_game_version_by_id("en-116");
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    posted_t posted[32];
    int first, second, n;
    CHECK(v && jpt_profile_activate(v, error, sizeof error), "the en-116 profile is active for the menu tests");
    if (!v) return;
    if (!gm_ptr_checked(MAIN_WINDOW_OBJECT, 4, 1)) gm_alloc_pages(MAIN_WINDOW_OBJECT & ~0xFFFFu, 0x10000u, "game state");
    s_arena = gm_alloc_pages(0, 0x100000, "menu arena");
    s_arena_used = 0;
    wc[9] = put_string(g_scratch + 0x500, CLASS_NAME_);
    gp_copy_out(g_scratch + 0x600, wc, sizeof wc);
    U32(RegisterClassA, 1, g_scratch + 0x600);
    s_hwnd = U32(CreateWindowExA, 12, 0u, wc[9], put_string(g_scratch + 0x700, "Trespasser"), 0x90080000u, 0u, 0u, 640u, 480u, 0u, 0u, 0x00400000u, 0u);
    CHECK(s_hwnd != 0 && lx_user32_main_window() == s_hwnd, "the game's window exists for the pad's clicks");
    drain(posted, 32);

    release_everything(joystick);
    lx_user32_cursor_set(5, 5);
    main_menu();
    tap(joystick, JPT_PAD_DPAD_DOWN);
    first = button_under_cursor();
    CHECK(first >= 0, "the first D-pad press puts the game's pointer on a button (%d,%d)", cursor_x(), cursor_y());
    CHECK(gamepad_menu_focus(), "gamepad focus drives the menu: the host pointer is hidden");
    tap(joystick, JPT_PAD_DPAD_DOWN);
    second = button_under_cursor();
    CHECK(second == first + 1, "D-pad down moves to the next button (%d -> %d)", first, second);
    tap(joystick, JPT_PAD_DPAD_UP);
    CHECK(button_under_cursor() == first, "D-pad up moves back (%d)", button_under_cursor());
    axis(joystick, STICK_LY, 1.0f);
    ui_frame();
    axis(joystick, STICK_LY, 0.0f);
    ui_frame();
    CHECK(button_under_cursor() == first + 1, "the left stick moves focus the same way (%d)", button_under_cursor());
    drain(posted, 32);
    tap(joystick, JPT_PAD_SOUTH);
    n = drain(posted, 32);
    CHECK(has(posted, n, WM_LBUTTONDOWN_, 0, 0) && has(posted, n, WM_LBUTTONUP_, 0, 0), "A is one click at the pointer: button down then up (%d messages)", n);
    CHECK(!has(posted, n, WM_KEYDOWN_, 0, 0), "and no key is posted for it");
    drain(posted, 32);
    tap(joystick, JPT_PAD_EAST);
    n = drain(posted, 32);
    CHECK(has(posted, n, WM_KEYDOWN_, 0x1B, 1), "B is Escape in a menu (%d messages)", n);
    CHECK(lx_gamepad_device() == PAD_DEVICE_GAMEPAD, "the pad's own clicks and keys never switch the device back to the mouse");

    {
        lx_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = LX_EV_MOUSE_MOVE, ev.relative = 1, ev.dx = 40.0f, ev.dy = 25.0f;
        lx_user32_inject_event(&ev);
        CHECK(lx_gamepad_device() == PAD_DEVICE_KEYBOARD_MOUSE && !gamepad_menu_focus(), "the mouse takes the pointer back and the host pointer shows again");
        memset(&ev, 0, sizeof ev);
        ev.kind = LX_EV_KEY, ev.vk = 0x41, ev.down = 1;
        lx_user32_inject_event(&ev);
        CHECK(lx_gamepad_device() == PAD_DEVICE_KEYBOARD_MOUSE, "a key leaves it with the keyboard and mouse");
        tap(joystick, JPT_PAD_DPAD_DOWN);
        CHECK(lx_gamepad_device() == PAD_DEVICE_GAMEPAD && gamepad_menu_focus() && button_under_cursor() >= 0, "the next pad press takes focus again, on a control");
    }

    gameplay_view();
    drain(posted, 32);
    release_everything(joystick);
    ui_frame();
    tap(joystick, JPT_PAD_START);
    n = drain(posted, 32);
    CHECK(has(posted, n, WM_KEYDOWN_, 0x1B, 1), "Start is the Escape key over gameplay (%d messages)", n);
    CHECK(!tutorial_prompts_card_shown(), "no controls card yet");
    tap(joystick, JPT_PAD_BACK);
    CHECK(tutorial_prompts_card_shown(), "View opens the controls card");
    {
        static uint32_t frame[640 * 480];
        lx_card_stats_t st;
        CHECK(lx_card_draw(frame, 640, 640, 480, 0, 0, 640, 480), "the card is drawn for the pad");
        lx_card_stats(&st);
        CHECK(st.glyphs > 8 && st.fits == 1, "with the controller's own glyphs in its rows (%d glyphs, %d rows)", st.glyphs, st.rows);
    }
    tap(joystick, JPT_PAD_EAST);
    CHECK(!tutorial_prompts_card_shown(), "B closes the card");
    drain(posted, 32);
    gm_release(s_arena);
    jpt_profile_reset();
}

static void test_rumble(fake_pad_t* joystick) {
    (void)joystick;
    s_rumble_calls = 0;
    CHECK(lx_gamepad_rumble(0.5f, 1.0f, 50) == 1 && s_rumble_calls == 1, "rumble reaches the device's own callback");
    CHECK(s_rumble_low > 32000 && s_rumble_low < 33500 && s_rumble_high == 65535, "scaled to 16 bits (%u, %u)", s_rumble_low, s_rumble_high);
    CHECK(lx_gamepad_rumble(-3.0f, 9.0f, 50) == 1 && s_rumble_low == 0 && s_rumble_high == 65535, "out-of-range strengths are clamped");
}

static void test_hotplug(fake_pad_t* first) {
    fake_pad_t* second;
    lx_gamepad_stats_t stats;
    second = attach("Second fake pad");
    CHECK(second != NULL, "a second controller attaches");
    settle();
    lx_gamepad_stats(&stats);
    CHECK(stats.connections == 2, "the second is noticed (%u connections)", stats.connections);
    press(second, JPT_PAD_SOUTH, 1);
    lx_gamepad_poll();
    CHECK(lx_gamepad_state()->buttons == JPT_PAD_BIT(JPT_PAD_SOUTH), "the controller with the latest activity is the one in use");
    press(second, JPT_PAD_SOUTH, 0);

    detach(second);
    settle();
    lx_gamepad_stats(&stats);
    CHECK(stats.disconnections == 1 && lx_gamepad_connected() == 1, "removing the one in use leaves the other connected");
    CHECK(lx_gamepad_rumble(0.1f, 0.1f, 10) == 1, "and rumble goes to it");

    detach(first);
    settle();
    lx_gamepad_stats(&stats);
    CHECK(stats.disconnections == 2 && lx_gamepad_connected() == 0, "removing the last controller leaves none");
    CHECK(lx_gamepad_device() == PAD_DEVICE_KEYBOARD_MOUSE, "the keyboard and mouse take over");
    CHECK(lx_gamepad_rumble(1.0f, 1.0f, 10) == 0, "no controller, no rumble");
    frame();
    CHECK(commands() == 0, "gameplay input is neutral without a controller");

    {
        fake_pad_t* again = attach("Reconnected fake pad");
        settle();
        lx_gamepad_stats(&stats);
        CHECK(again != NULL && stats.connections == 3 && lx_gamepad_connected() == 1, "a controller connects again (%u connections)", stats.connections);
        detach(again);
        settle();
        CHECK(lx_gamepad_connected() == 0, "and goes away again");
    }
}

int main(void) {
    jpt_gamepad_settings_t cfg;
    fake_pad_t* joystick;
    if (!harness_start()) return 2;
    s_cinput = g_scratch + 0x1000;
    lx_gamepad_set_backend(&k_fake);
    jpt_gamepad_defaults(&cfg);
    CHECK(lx_gamepad_init(&cfg) == 1, "the gamepad layer starts");
    lx_gamepad_poll();
    CHECK(lx_gamepad_connected() == 0, "no controller before one is attached");
    frame();
    CHECK(commands() == 0, "and gameplay input is untouched");

    joystick = attach("Fake standard pad");
    settle();
    test_connection_and_mapping(joystick);
    test_gameplay(joystick);
    test_rumble(joystick);
    test_invert_mouse(joystick);
    test_menu_navigation(joystick);
    test_hotplug(joystick);

    lx_gamepad_shutdown();
    CHECK(s_opened == s_closed && s_opened == 3, "every controller that was opened was closed (%d opened, %d closed)", s_opened, s_closed);
    lx_gamepad_set_backend(NULL);
    harness_stop();
    printf("linux_gamepad_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
