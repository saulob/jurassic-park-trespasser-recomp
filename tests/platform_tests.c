/*
 * Headless checks of the SDL3 platform layer's decisions: which SDL mouse motion reaches the game's virtual cursor
 * (Quick Save, Quick Load, menus and focus never inject a delta), fractional counts and Mouse sensitivity, the messages
 * the host routes past SDL's window procedure, SDL's lifetime, the hotkeys, controller hotplug, the settings file, and
 * where SDL is started, pumped and stopped in the host's sources. No SDL, no window, no device.
 * Build: CMake target platform_tests.
 */
#include <io.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamepad_policy.h"
#include "host_policy.h"
#include "jpt_config.h"
#include "test_profile.h"

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

enum {
    WM_SIZE_ = 0x0005, WM_ACTIVATE_ = 0x0006, WM_SETFOCUS_ = 0x0007, WM_KILLFOCUS_ = 0x0008, WM_PAINT_ = 0x000F,
    WM_CLOSE_ = 0x0010, WM_ERASEBKGND_ = 0x0014, WM_ACTIVATEAPP_ = 0x001C, WM_MOUSEACTIVATE_ = 0x0021,
    WM_WINDOWPOSCHANGING_ = 0x0046, WM_WINDOWPOSCHANGED_ = 0x0047, WM_NCCALCSIZE_ = 0x0083, WM_NCHITTEST_ = 0x0084, WM_KEYDOWN_ = 0x0100,
    WM_KEYUP_ = 0x0101, WM_CHAR_ = 0x0102, WM_SYSKEYDOWN_ = 0x0104, WM_SYSKEYUP_ = 0x0105, WM_SYSCOMMAND_ = 0x0112,
    WM_MOUSEMOVE_ = 0x0200, WM_LBUTTONDOWN_ = 0x0201, WM_DROPFILES_ = 0x0233, WM_TOUCH_ = 0x0240,
    WM_DPICHANGED_ = 0x02E0, WM_GETDPISCALEDSIZE_ = 0x02E4
};
enum { VK_ESCAPE_ = 0x1B, VK_F4_ = 0x73, VK_F5_ = 0x74, VK_F7_ = 0x76, VK_F8_ = 0x77, VK_F9_ = 0x78, VK_F10_ = 0x79,
       VK_F11_ = 0x7A };

#define SCREEN_W 1920
#define SCREEN_H 1080
#define MS 1000000ull

/* window_host.c's mouse path: the state from what the game shows and focus, the virtual cursor taken back at the centre
 * when the game's own pointer was the real one, SDL's relative mode on exactly while the state captures, its start
 * stamped, and SDL's motion counted, scaled and added only while captured. */
typedef struct {
    input_state_t state;
    long x, y;
    int relative;
    uint64_t since, clock;
    motion_counts_t counts;
    mouse_scale_t scale;
    float sensitivity;
} host_t;

static void host_init(host_t* h, float sensitivity) {
    memset(h, 0, sizeof *h);
    h->state = INPUT_MENU;
    h->sensitivity = sensitivity;
    h->clock = 1000 * MS;
}

static void host_show(host_t* h, guest_ui_t ui, int focused) {
    input_signals_t sig;
    input_state_t next;
    memset(&sig, 0, sizeof sig);
    sig.ui = ui;
    sig.focused = focused;
    next = input_state_from(&sig);
    if (next != h->state) {
        pointer_recapture(h->state, next, &h->x, &h->y, SCREEN_W, SCREEN_H);
        memset(&h->scale, 0, sizeof h->scale);
        memset(&h->counts, 0, sizeof h->counts);
        h->state = next;
    }
    if (input_captured(h->state) && !h->relative) {
        h->relative = 1;
        h->since = h->clock;
    } else if (!input_captured(h->state)) {
        h->relative = 0;
    }
}

static void host_motion(host_t* h, float dx, float dy, uint64_t stamp) {
    long x, y, sx, sy;
    if (!relative_motion_accepted(h->relative, stamp, h->since)) return;
    motion_counts(&h->counts, dx, dy, &x, &y);
    if (!x && !y) return;
    sx = x;
    sy = y;
    if (input_captured(h->state)) mouse_scale_counts(&h->scale, h->sensitivity, x, y, &sx, &sy);
    pointer_add_relative(h->state, &h->x, &h->y, sx, sy);
}

static void tick(host_t* h, uint64_t ns) { h->clock += ns; }

static void in_gameplay(host_t* h) {
    host_show(h, GUEST_UI_MENU, 1);
    tick(h, 16 * MS);
    host_show(h, GUEST_UI_GAMEPLAY, 1);
}

static int centred(const host_t* h) { return h->x == SCREEN_W / 2 && h->y == SCREEN_H / 2; }

static void test_motion_acceptance(void) {
    CHECK(!relative_motion_accepted(0, 5 * MS, 1 * MS));
    CHECK(!relative_motion_accepted(1, 1 * MS - 1, 1 * MS));
    CHECK(relative_motion_accepted(1, 1 * MS, 1 * MS));
    CHECK(relative_motion_accepted(1, 9 * MS, 1 * MS));
}

/* Quick Save and Quick Load stop the game (a dialog state for the host) and resume it: motion SDL queued before the
 * resume, drained on the next frame, must not turn the view. */
static void test_quick_save_and_load_no_delta(void) {
    for (int key = 0; key < 2; key++) {
        host_t h;
        uint64_t queued;
        host_init(&h, 1.0f);
        in_gameplay(&h);
        CHECK(h.state == INPUT_GAMEPLAY_CAPTURED && centred(&h) && h.relative);
        tick(&h, 4 * MS);
        host_motion(&h, 12.0f, -7.0f, h.clock);
        CHECK(h.x == SCREEN_W / 2 + 12 && h.y == SCREEN_H / 2 - 7);
        h.x = SCREEN_W / 2;
        h.y = SCREEN_H / 2;
        host_show(&h, GUEST_UI_DIALOG, 1);
        CHECK(!h.relative);
        tick(&h, key ? 900 * MS : 40 * MS);
        queued = h.clock;
        host_motion(&h, 300.0f, 200.0f, queued);
        tick(&h, 1 * MS);
        host_show(&h, GUEST_UI_GAMEPLAY, 1);
        host_motion(&h, 300.0f, 200.0f, queued);
        CHECK(h.state == INPUT_GAMEPLAY_CAPTURED && centred(&h));
        tick(&h, 2 * MS);
        host_motion(&h, 5.0f, 0.0f, h.clock);
        CHECK(h.x == SCREEN_W / 2 + 5 && h.y == SCREEN_H / 2); /* motion after the resume still counts */
    }
}

/* The same save with no state change at all: the stamp still separates nothing, real motion is real. */
static void test_quick_save_without_state_change(void) {
    host_t h;
    host_init(&h, 1.0f);
    in_gameplay(&h);
    tick(&h, 30 * MS);
    host_motion(&h, 3.0f, 4.0f, h.clock - 10 * MS);
    CHECK(h.x == SCREEN_W / 2 + 3 && h.y == SCREEN_H / 2 + 4);
}

static void test_menu_to_gameplay_no_delta(void) {
    host_t h;
    uint64_t in_menu;
    host_init(&h, 1.0f);
    host_show(&h, GUEST_UI_MENU, 1);
    h.x = 100;
    h.y = 900;
    tick(&h, 5 * MS);
    in_menu = h.clock;
    host_motion(&h, 50.0f, 50.0f, in_menu);
    CHECK(h.x == 100 && h.y == 900);
    tick(&h, 5 * MS);
    host_show(&h, GUEST_UI_GAMEPLAY, 1);
    host_motion(&h, 50.0f, 50.0f, in_menu);
    CHECK(centred(&h));
}

/* Alt+Tab away and back: the cursor stays virtual (released), so the game's centre is kept and nothing stamped before
 * the window came back moves it. */
static void test_focus_regain_no_delta(void) {
    host_t h;
    uint64_t away;
    host_init(&h, 1.0f);
    in_gameplay(&h);
    tick(&h, 10 * MS);
    host_show(&h, GUEST_UI_GAMEPLAY, 0);
    CHECK(h.state == INPUT_GAMEPLAY_RELEASED && !h.relative);
    tick(&h, 2000 * MS);
    away = h.clock;
    host_motion(&h, -400.0f, 120.0f, away);
    CHECK(centred(&h));
    tick(&h, 1 * MS);
    host_show(&h, GUEST_UI_GAMEPLAY, 1);
    host_motion(&h, -400.0f, 120.0f, away);
    CHECK(h.state == INPUT_GAMEPLAY_CAPTURED && h.relative && centred(&h));
}

static void test_counts_and_sensitivity(void) {
    motion_counts_t m;
    long x, y;
    host_t h;
    memset(&m, 0, sizeof m);
    motion_counts(&m, 0.4f, -0.4f, &x, &y);
    CHECK(x == 0 && y == 0);
    motion_counts(&m, 0.7f, -0.7f, &x, &y);
    CHECK(x == 1 && y == -1);
    motion_counts(&m, 3.0f, -2.0f, &x, &y);
    CHECK(x == 3 && y == -2);
    host_init(&h, 1.0f);
    in_gameplay(&h);
    tick(&h, 1 * MS);
    host_motion(&h, 10.0f, -6.0f, h.clock);
    CHECK(h.x == SCREEN_W / 2 + 10 && h.y == SCREEN_H / 2 - 6); /* 1.00 passes counts through */
    host_init(&h, 0.5f);
    in_gameplay(&h);
    tick(&h, 1 * MS);
    for (int i = 0; i < 4; i++) host_motion(&h, 1.0f, 3.0f, h.clock);
    CHECK(h.x == SCREEN_W / 2 + 2 && h.y == SCREEN_H / 2 + 6); /* half counts carry */
    host_init(&h, 2.0f);
    in_gameplay(&h);
    tick(&h, 1 * MS);
    host_motion(&h, 7.0f, -1.0f, h.clock);
    CHECK(h.x == SCREEN_W / 2 + 14 && h.y == SCREEN_H / 2 - 2);
}

static void test_sdl_message_routing(void) {
    static const unsigned skipped[] = { WM_PAINT_,      WM_ERASEBKGND_,         WM_MOUSEACTIVATE_, WM_WINDOWPOSCHANGING_,
                                        WM_WINDOWPOSCHANGED_, WM_NCCALCSIZE_, WM_NCHITTEST_,     WM_SYSCOMMAND_,
                                        WM_DROPFILES_,  WM_TOUCH_,              0x02CC,            WM_DPICHANGED_,
                                        WM_GETDPISCALEDSIZE_, WM_MOUSEMOVE_, WM_LBUTTONDOWN_, 0x020A, 0x020E };
    static const unsigned through[] = { WM_SIZE_,     WM_ACTIVATE_, WM_SETFOCUS_,   WM_KILLFOCUS_, WM_ACTIVATEAPP_,
                                        WM_KEYDOWN_,  WM_KEYUP_,    WM_CHAR_,       WM_SYSKEYDOWN_, WM_SYSKEYUP_,
                                        WM_CLOSE_,    0x01FF,       0x020F };
    for (size_t i = 0; i < sizeof skipped / sizeof skipped[0]; i++) CHECK(window_message_skips_sdl(skipped[i]));
    for (size_t i = 0; i < sizeof through / sizeof through[0]; i++) CHECK(!window_message_skips_sdl(through[i]));
}

static void test_sdl_lifetime(void) {
    sdl_life_t l;
    memset(&l, 0, sizeof l);
    CHECK(sdl_life_may_start(&l) && !sdl_life_may_stop(&l, 7));
    sdl_life_started(&l, 7);
    CHECK(!sdl_life_may_start(&l));
    CHECK(!sdl_life_may_stop(&l, 8));
    CHECK(sdl_life_may_stop(&l, 7));
    sdl_life_stopped(&l);
    CHECK(!sdl_life_may_stop(&l, 7) && !sdl_life_may_start(&l));
}

/* Each host key acts on its key-down only, once: a repeat or the key-up never acts again. */
static void test_hotkeys_once(void) {
    static const struct {
        unsigned vk;
        key_action_t action;
    } keys[] = { { VK_F5_, KEY_HOST_MINIMAP },      { VK_F7_, KEY_HOST_QUICK_SAVE }, { VK_F8_, KEY_HOST_QUICK_LOAD },
                 { VK_F9_, KEY_HOST_SNAPSHOT },     { VK_F10_, KEY_HOST_CHEATS },    { VK_F11_, KEY_HOST_FULLSCREEN } };
    key_policy_set_quick_keys(VK_F7_, VK_F8_);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        CHECK(key_policy(WM_KEYDOWN_, keys[i].vk, 0, 0, 0) == keys[i].action);
        CHECK(key_policy(WM_KEYDOWN_, keys[i].vk, 1, 0, 0) == KEY_DROP);
        CHECK(key_policy(WM_KEYUP_, keys[i].vk, 0, 0, 0) == KEY_DROP);
    }
    CHECK(key_policy(WM_SYSKEYDOWN_, VK_F4_, 0, 0, 1) == KEY_HOST_CLOSE);
    CHECK(key_policy(WM_SYSKEYDOWN_, VK_F4_, 1, 0, 1) == KEY_DROP);
    CHECK(key_policy(WM_KEYDOWN_, VK_F11_, 0, 1, 0) == KEY_DROP && key_policy(WM_KEYUP_, VK_F11_, 0, 1, 0) == KEY_DROP);
    CHECK(key_policy(WM_KEYDOWN_, VK_ESCAPE_, 0, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(WM_KEYDOWN_, 'W', 0, 0, 0) == KEY_TO_GAME && key_policy(WM_KEYUP_, 'W', 0, 0, 0) == KEY_TO_GAME);
}

/* SDL gives a reconnected controller a new instance id: it joins as a new slot and becomes the one in use. */
static void test_gamepad_hotplug(void) {
    pad_roster_t r;
    pad_tracker_t t;
    int slot;
    pad_roster_init(&r);
    pad_tracker_init(&t);
    slot = pad_roster_add(&r, 7, 0);
    CHECK(slot >= 0 && r.active == slot && pad_roster_add(&r, 7, 0) < 0);
    CHECK(pad_tracker_gamepad(&t) && t.device == PAD_DEVICE_GAMEPAD);
    CHECK(pad_roster_remove(&r, 7) && pad_roster_count(&r) == 0 && pad_roster_find(&r, 7) < 0);
    CHECK(pad_tracker_pads_left(&t, 0) && t.device == PAD_DEVICE_KEYBOARD_MOUSE);
    slot = pad_roster_add(&r, 9, 0);
    CHECK(slot >= 0 && r.active == slot && pad_roster_count(&r) == 1);
    CHECK(pad_tracker_gamepad(&t) && t.device == PAD_DEVICE_GAMEPAD);
    CHECK(!pad_roster_remove(&r, 7));
}

/* The settings the platform reads keep their names and values. */
static void test_settings_unchanged(void) {
    char path[512];
    FILE* f;
    jpt_config_t c;
    snprintf(path, sizeof path, "%s\\platform_tests_%u.ini", getenv("TEMP") ? getenv("TEMP") : ".", (unsigned)rand());
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs("display_mode = borderless\nmouse_sensitivity = 1.75\nminimap = 0\n", f);
    fclose(f);
    CHECK(jpt_config_load(path, &c));
    CHECK(c.borderless && fabsf(c.mouse_sensitivity - 1.75f) < 1e-6f && !c.minimap);
    remove(path);
}

static char* read_source(const char* rel) {
    char path[512];
    FILE* f;
    long n;
    char* text;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, rel);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (text) text[n] = 0;
    fclose(f);
    return text;
}

static int before(const char* text, const char* first, const char* second) {
    const char* a = text ? strstr(text, first) : NULL;
    const char* b = a ? strstr(a, second) : NULL;
    return a && b;
}

/* SDL is started, stopped and initialised in platform.c only; no host source reads the mouse through Raw Input or
 * clips the cursor itself, and none uses XInput. */
static void test_sources(void) {
    struct _finddata_t found;
    char pattern[512];
    intptr_t handle;
    int files = 0;
    char *platform = read_source("src/host/platform.c"), *window = read_source("src/host/window_host.c");
    char *gamepad = read_source("src/host/gamepad.c"), *main_c = read_source("src/host/main.c");
    char *exit_c = read_source("src/host/exit_policy.c"), *deps = read_source("config/launcher_deps.txt");
    char* licenses = read_source("packaging/licenses/THIRD-PARTY-LICENSES.txt");
    CHECK(platform && window && gamepad && main_c && exit_c && deps && licenses);
    snprintf(pattern, sizeof pattern, "%s/src/host/*.c*", JPT_SOURCE_DIR);
    handle = _findfirst(pattern, &found);
    CHECK(handle != -1);
    if (handle != -1) {
        do {
            char rel[300], *text;
            snprintf(rel, sizeof rel, "src/host/%s", found.name);
            text = read_source(rel);
            if (!text) continue;
            files++;
            if (strcmp(found.name, "platform.c")) {
                CHECK(!strstr(text, "SDL_Init(") && !strstr(text, "SDL_InitSubSystem(") && !strstr(text, "SDL_Quit"));
                CHECK(!strstr(text, "SDL_SetHint("));
            }
            CHECK(!strstr(text, "RegisterRawInputDevices") && !strstr(text, "GetRawInputData"));
            CHECK(!strstr(text, "XInput") && !strstr(text, "DirectInput8Create"));
            if (strcmp(found.name, "window_host.c")) CHECK(!strstr(text, "ClipCursor("));
            free(text);
        } while (_findnext(handle, &found) == 0);
        _findclose(handle);
    }
    CHECK(files > 50);
    if (window) {
        const char* shim = strstr(window, "void shim_ClipCursor(void)");
        CHECK(shim && !strstr(window, "ClipCursor(NULL)") && !strstr(window, "ClipCursor(&"));
        CHECK(before(window, "s.game_proc = (WNDPROC)GetWindowLongPtrA", "platform_window_attach(window);"));
        CHECK(before(window, "platform_window_attach(window);", "hook(window);"));
        CHECK(before(window, "void shim_GetCursorPos(void)", "platform_pump();"));
        CHECK(before(strstr(window, "void shim_GetCursorPos(void)"), "platform_pump();", "update_input(\"GetCursorPos\");"));
        CHECK(strstr(window, "window_message_skips_sdl(msg) ? s.game_proc : s.next") != NULL);
        CHECK(before(window, "static void set_relative(int on)", "s.last_move_valid = 0;"));
        CHECK(before(window, "static void update_input(const char* why)", "pointer_recapture(s.state, next,"));
    }
    if (platform) {
        CHECK(before(platform, "SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, \"0\"", "SDL_InitSubSystem(SDL_INIT_VIDEO)"));
        CHECK(before(platform, "static void quiet_events(void)", "SDL_EVENT_DROP_FILE"));
        CHECK(!strstr(platform, "SDL_INIT_AUDIO"));
        CHECK(strstr(platform, "sdl_life_may_stop(&P.life, GetCurrentThreadId())") != NULL);
    }
    if (gamepad) CHECK(strstr(gamepad, "SDL_OpenGamepad") && strstr(gamepad, "SDL_GetGamepads") && !strstr(gamepad, "SDL_PeepEvents"));
    if (main_c) CHECK(before(main_c, "static DWORD WINAPI guest_main", "platform_start()") &&
                      before(strstr(main_c, "static DWORD WINAPI guest_main"), "platform_start()", "native32_call_guest"));
    if (exit_c) CHECK(before(exit_c, "present_release();", "window_host_detach_platform();") &&
                      before(exit_c, "window_host_detach_platform();", "platform_stop();") &&
                      before(exit_c, "platform_stop();", "host_log(\"exit\", \"exit code %d\""));
    if (deps) CHECK(strstr(deps, "external/SDL") && strstr(deps, "f00dc919ca35b9756316e86c4777392de442f9b2"));
    if (licenses) CHECK(strstr(licenses, "SDL 3.4.20") && strstr(licenses, "This software is provided 'as-is'"));
    free(platform);
    free(window);
    free(gamepad);
    free(main_c);
    free(exit_c);
    free(deps);
    free(licenses);
}

int main(void) {
    test_profile_activate();
    test_motion_acceptance();
    test_quick_save_and_load_no_delta();
    test_quick_save_without_state_change();
    test_menu_to_gameplay_no_delta();
    test_focus_regain_no_delta();
    test_counts_and_sensitivity();
    test_sdl_message_routing();
    test_sdl_lifetime();
    test_hotkeys_once();
    test_gamepad_hotplug();
    test_settings_unchanged();
    test_sources();
    printf("platform_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
