/*
 * Headless checks of the host's pure logic: no window, no device, no game, no audio.
 * Build: CMake target host_tests (docs/BUILD.md, "Headless tests").
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <ddraw.h>
#include <d3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bump_spans.h"
#include "ddraw/d3d11_shaders.h"
#include "ddraw/d3d_translate.h"
#include "ddraw/page_arena.h"
#include "ddraw/target_sync.h"
#include "host_policy.h"
#include "jpt_display.h"

static int s_failures, s_checks;

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_fps_counter(void) {
    fps_counter_t c;
    const int64_t f = 1000000;
    int updates = 0;
    fps_counter_init(&c, f, 0);
    for (int i = 1; i <= 60; i++) updates += fps_counter_present(&c, (int64_t)i * f / 60);
    CHECK(updates == 1);
    CHECK(c.fps > 59.9 && c.fps < 60.1);
    CHECK(c.total_frames == 60);
    CHECK(!fps_counter_idle(&c, f + f / 2));
    CHECK(fps_counter_idle(&c, 3 * f + 1));
    CHECK(c.fps == 0.0);
    CHECK(!fps_counter_idle(&c, 5 * f));
    fps_counter_init(&c, f, 0);
    CHECK(!fps_counter_present(&c, f / 2));
    CHECK(fps_counter_present(&c, f + f / 4));
    CHECK(c.fps > 1.59 && c.fps < 1.61);
}

static void test_input_state(void) {
    input_signals_t sig = { 0, 0, 0, 1, 0, GUEST_UI_GAMEPLAY };
    CHECK(input_state_from(&sig) == INPUT_GAMEPLAY_CAPTURED);
    sig.focused = 0;
    CHECK(input_state_from(&sig) == INPUT_GAMEPLAY_RELEASED);
    sig.focused = 1;
    sig.minimized = 1;
    CHECK(input_state_from(&sig) == INPUT_GAMEPLAY_RELEASED);
    sig.minimized = 0;
    sig.ui = GUEST_UI_DIALOG;
    CHECK(input_state_from(&sig) == INPUT_DIALOG);
    sig.ui = GUEST_UI_MENU;
    CHECK(input_state_from(&sig) == INPUT_MENU);
    sig.ui = GUEST_UI_NONE;
    CHECK(input_state_from(&sig) == INPUT_MENU);
    sig.ui = GUEST_UI_GAMEPLAY;
    sig.size_move = 1;
    CHECK(input_state_from(&sig) == INPUT_MOVING_WINDOW);
    sig.transition = 1;
    CHECK(input_state_from(&sig) == INPUT_FULLSCREEN_TRANSITION);
    sig.shutdown = 1;
    CHECK(input_state_from(&sig) == INPUT_SHUTDOWN);
    CHECK(!strcmp(input_state_name(INPUT_GAMEPLAY_CAPTURED), "INPUT_GAMEPLAY_CAPTURED"));
    CHECK(!strcmp(input_state_name(INPUT_GAMEPLAY_RELEASED), "INPUT_GAMEPLAY_RELEASED"));
}

/* The cursor is confined (and Raw Input drives the view) exactly when the window host would clip. */
static int clips(guest_ui_t ui, int focused, int minimized, int transition, int size_move, int shutdown) {
    input_signals_t sig;
    sig.shutdown = shutdown;
    sig.transition = transition;
    sig.size_move = size_move;
    sig.focused = focused;
    sig.minimized = minimized;
    sig.ui = ui;
    return input_should_clip(input_state_from(&sig), 1, 0);
}

static void test_capture_policy(void) {
    CHECK(clips(GUEST_UI_GAMEPLAY, 1, 0, 0, 0, 0));
    CHECK(input_captured(INPUT_GAMEPLAY_CAPTURED) && input_hide_pointer(INPUT_GAMEPLAY_CAPTURED, 0));
    /* Esc, F1 and F12 open CInGameOptionsWnd, CHintWnd and CControlsWnd over CGameWnd: a dialog. */
    CHECK(!clips(GUEST_UI_DIALOG, 1, 0, 0, 0, 0));
    CHECK(key_opens_game_window(0x100, 0x1B, 0) && key_opens_game_window(0x100, 0x70, 0) &&
          key_opens_game_window(0x100, 0x7B, 0));
    CHECK(!key_opens_game_window(0x100, 0x1B, 1) && !key_opens_game_window(0x101, 0x1B, 0) &&
          !key_opens_game_window(0x100, 0x74, 0));
    /* Back in CGameWnd with focus: captured again. */
    CHECK(clips(GUEST_UI_GAMEPLAY, 1, 0, 0, 0, 0));
    /* Alt+Tab and focus loss, minimising, F11, moving the window, menus: released. */
    CHECK(!clips(GUEST_UI_GAMEPLAY, 0, 0, 0, 0, 0));
    CHECK(!clips(GUEST_UI_GAMEPLAY, 1, 1, 0, 0, 0));
    CHECK(!clips(GUEST_UI_GAMEPLAY, 1, 0, 1, 0, 0));
    CHECK(!clips(GUEST_UI_GAMEPLAY, 1, 0, 0, 1, 0));
    CHECK(!clips(GUEST_UI_MENU, 1, 0, 0, 0, 0));
    /* Alt+F4 / X: never captured again, whatever the game shows next. */
    CHECK(!clips(GUEST_UI_GAMEPLAY, 1, 0, 0, 0, 1));
    CHECK(!input_hide_pointer(INPUT_SHUTDOWN, 1) && !input_captured(INPUT_SHUTDOWN));
    /* The click that activated the window on its title bar is not dragged into the picture. */
    CHECK(!input_should_clip(INPUT_GAMEPLAY_CAPTURED, 0, 1));
    CHECK(input_should_clip(INPUT_GAMEPLAY_CAPTURED, 1, 1) && input_should_clip(INPUT_GAMEPLAY_CAPTURED, 0, 0));
    /* Menus follow the real cursor; gameplay reads the game's own re-centred one. */
    CHECK(!input_cursor_virtual(INPUT_MENU) && !input_cursor_virtual(INPUT_DIALOG));
    CHECK(input_cursor_virtual(INPUT_GAMEPLAY_CAPTURED) && input_cursor_virtual(INPUT_GAMEPLAY_RELEASED));
    CHECK(input_hide_pointer(INPUT_MENU, 1) && !input_hide_pointer(INPUT_MENU, 0) && !input_hide_pointer(INPUT_GAMEPLAY_RELEASED, 1));
}

static void test_keys(void) {
    CHECK(key_policy(0x100, 0x7A, 0, 0, 0) == KEY_HOST_FULLSCREEN);
    CHECK(key_policy(0x100, 0x7A, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x101, 0x7A, 0, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x7A, 0, 1, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x101, 0x7A, 0, 1, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x100, 0x74, 0, 0, 0) == KEY_HOST_RENDERER);
    CHECK(key_policy(0x100, 0x74, 1, 0, 0) == KEY_DROP && key_policy(0x101, 0x74, 0, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x104, 0x73, 0, 0, 1) == KEY_HOST_CLOSE);
    CHECK(key_policy(0x104, 0x73, 1, 0, 1) == KEY_DROP);
    CHECK(key_policy(0x100, 0x73, 0, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x100, 0x1B, 0, 0, 0) == KEY_TO_GAME && key_policy(0x101, 0x1B, 0, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x100, 0x1B, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x70, 0, 0, 0) == KEY_TO_GAME && key_policy(0x100, 0x70, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x7B, 0, 0, 0) == KEY_TO_GAME && key_policy(0x100, 0x7B, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x57, 1, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x0102, 0x1B, 0, 0, 0) == KEY_TO_GAME);
}

static void test_shutdown(void) {
    shutdown_state_t s;
    static const shutdown_phase_t gameplay_path[] = {
        SHUTDOWN_GUEST_QUIT_REQUESTED, SHUTDOWN_GAMEWND_CLOSING, SHUTDOWN_WINMAIN_RETURNING, SHUTDOWN_CRT_EXITING,
        SHUTDOWN_GUEST_STOPPED, SHUTDOWN_HOST_CLEANUP, SHUTDOWN_EXITED,
    };
    unsigned progress;
    shutdown_init(&s);
    CHECK(!shutdown_active(&s) && shutdown_guest_may_run(&s) && shutdown_renderer_allowed(&s));
    CHECK(shutdown_request(&s, "Alt+F4") && shutdown_active(&s) && s.phase == SHUTDOWN_CLOSE_REQUESTED);
    CHECK(!shutdown_request(&s, "WM_CLOSE") && !strcmp(s.source, "Alt+F4"));
    progress = s.progress;
    for (unsigned i = 0; i < sizeof gameplay_path / sizeof gameplay_path[0]; i++) {
        CHECK(shutdown_advance(&s, gameplay_path[i], "ignored") && s.phase == gameplay_path[i]);
        CHECK(s.progress == ++progress && !strcmp(s.source, "Alt+F4"));
        if (gameplay_path[i] < SHUTDOWN_CRT_EXITING) CHECK(shutdown_host_driving(&s));
        else CHECK(!shutdown_host_driving(&s));
        if (gameplay_path[i] < SHUTDOWN_GUEST_STOPPED) CHECK(shutdown_guest_may_run(&s));
        else CHECK(!shutdown_guest_may_run(&s));
        if (gameplay_path[i] < SHUTDOWN_HOST_CLEANUP) CHECK(shutdown_renderer_allowed(&s));
        else CHECK(!shutdown_renderer_allowed(&s));
    }
    CHECK(!shutdown_advance(&s, SHUTDOWN_MODAL_CLOSING, NULL) && s.phase == SHUTDOWN_EXITED);
    CHECK(!shutdown_request(&s, "again"));
    /* A phase met twice is not a new transition, but each quit step still counts as progress. */
    shutdown_init(&s);
    shutdown_request(&s, "X");
    shutdown_advance(&s, SHUTDOWN_MODAL_CLOSING, NULL);
    progress = s.progress;
    CHECK(!shutdown_advance(&s, SHUTDOWN_MODAL_CLOSING, NULL) && s.progress == progress);
    shutdown_note_progress(&s);
    CHECK(s.progress == progress + 1);
    /* The game's own menu Exit: no host request, the game unwinds by itself. */
    shutdown_init(&s);
    CHECK(shutdown_advance(&s, SHUTDOWN_WINMAIN_RETURNING, "the game's own menu"));
    CHECK(!strcmp(s.source, "the game's own menu") && !shutdown_host_driving(&s) && shutdown_active(&s));
    CHECK(!shutdown_request(&s, "Alt+F4"));
    CHECK(shutdown_advance(&s, SHUTDOWN_CRT_EXITING, NULL) && shutdown_advance(&s, SHUTDOWN_GUEST_STOPPED, NULL));
    CHECK(!strcmp(shutdown_phase_name(SHUTDOWN_GAMEWND_CLOSING), "GameWndClosing"));
    CHECK(!strcmp(shutdown_phase_name(SHUTDOWN_CRT_EXITING), "CRTExiting"));
    CHECK(shutdown_stall_threshold(0) == 250 && shutdown_stall_threshold(1) == 1000 && shutdown_stall_threshold(2) == 2000);
    CHECK(shutdown_stall_threshold(3) == 4000 && shutdown_stall_threshold(40) == 128000);
}

/* The game's window stack as its handlers change it on EndModal: CInGameOptionsWnd -1 opens CQuitWnd,
 * CQuitWnd 1 makes CGameWnd end with -1, CGameWnd or CMainScreenWnd -1 leaves the screen loop. */
static int simulate_quit(guest_wnd_t* stack, int depth, int max_pumps) {
    for (int pump = 0; pump < max_pumps; pump++) {
        quit_action_t a;
        if (!depth) return pump;
        a = quit_action_for(stack[depth - 1], 0);
        if (a.step != QUIT_END_WINDOW) return -1;
        switch (stack[depth - 1]) {
        case GUEST_WND_INGAME_OPTIONS:
            if (a.code == -1) stack[depth - 1] = GUEST_WND_QUIT;
            else depth--;
            break;
        case GUEST_WND_QUIT:
            depth--;
            if (a.code == 1 && depth && stack[depth - 1] == GUEST_WND_GAME) depth--;
            break;
        default: depth--; break;
        }
    }
    return depth ? -1 : max_pumps;
}

static void test_quit_steps(void) {
    quit_action_t a = quit_action_for(GUEST_WND_GAME, 0);
    CHECK(a.step == QUIT_END_WINDOW && a.code == -1 && a.quit_to_windows);
    a = quit_action_for(GUEST_WND_INGAME_OPTIONS, 0);
    CHECK(a.step == QUIT_END_WINDOW && a.code == -1 && !a.quit_to_windows);
    a = quit_action_for(GUEST_WND_QUIT, 0);
    CHECK(a.step == QUIT_END_WINDOW && a.code == 1);
    a = quit_action_for(GUEST_WND_MAIN_MENU, 0);
    CHECK(a.step == QUIT_END_WINDOW && a.code == -1);
    a = quit_action_for(GUEST_WND_OTHER, 0);
    CHECK(a.step == QUIT_END_WINDOW && a.code == 0);
    CHECK(quit_action_for(GUEST_WND_LOADER, 0).step == QUIT_WAIT && quit_action_for(GUEST_WND_NONE, 0).step == QUIT_WAIT);
    CHECK(quit_action_for(GUEST_WND_VIDEO, 0).step == QUIT_WAIT);
    CHECK(quit_action_for(GUEST_WND_VIDEO, 1).step == QUIT_SKIP_VIDEO);
    CHECK(quit_action_for(GUEST_WND_GAME, 1).step == QUIT_WAIT);
    CHECK(quit_action_for(GUEST_WND_GAME, 0).phase == SHUTDOWN_GAMEWND_CLOSING);
    CHECK(quit_action_for(GUEST_WND_MAIN_MENU, 0).phase == SHUTDOWN_MAINSCREEN_CLOSING);
    CHECK(quit_action_for(GUEST_WND_QUIT, 0).phase == SHUTDOWN_MODAL_CLOSING);
    {
        guest_wnd_t gameplay[] = { GUEST_WND_GAME };
        guest_wnd_t options[] = { GUEST_WND_GAME, GUEST_WND_INGAME_OPTIONS };
        guest_wnd_t hint[] = { GUEST_WND_GAME, GUEST_WND_OTHER };
        guest_wnd_t menu_dialog[] = { GUEST_WND_MAIN_MENU, GUEST_WND_OTHER };
        CHECK(simulate_quit(gameplay, 1, 8) == 1);
        CHECK(simulate_quit(options, 2, 8) == 2);
        CHECK(simulate_quit(hint, 2, 8) == 2);
        CHECK(simulate_quit(menu_dialog, 2, 8) == 2);
    }
}

static void test_open_press(void) {
    open_press_t p;
    open_press_reset(&p);
    /* Esc key-down in gameplay opens CInGameOptionsWnd (stack 1 -> 2); its key-up stays out of it. */
    open_press_keydown(&p, 0x1B, 0, 1, 1);
    CHECK(open_press_keyup(&p, 0x1B, 2));
    /* The second Esc, inside the menu, closes it normally. */
    open_press_keydown(&p, 0x1B, 0, 0, 2);
    CHECK(!open_press_keyup(&p, 0x1B, 2));
    /* Held Esc: repeats neither re-arm nor disarm; one key-up is consumed. */
    open_press_keydown(&p, 0x1B, 0, 1, 1);
    open_press_keydown(&p, 0x1B, 1, 0, 2);
    open_press_keydown(&p, 0x1B, 1, 0, 2);
    CHECK(open_press_keyup(&p, 0x1B, 2) && !open_press_keyup(&p, 0x1B, 2));
    /* A key-down that opened nothing lets its key-up through. */
    open_press_keydown(&p, 0x1B, 0, 1, 1);
    CHECK(!open_press_keyup(&p, 0x1B, 1));
    /* The game flushed the opening key-up: the next press re-decides, nothing stale is swallowed. */
    open_press_keydown(&p, 0x1B, 0, 1, 1);
    open_press_keydown(&p, 0x1B, 0, 0, 2);
    CHECK(!open_press_keyup(&p, 0x1B, 2));
    /* F1 and F12 behave the same; other keys and other key-ups are untouched. */
    open_press_keydown(&p, 0x70, 0, 1, 1);
    CHECK(!open_press_keyup(&p, 0x1B, 2) && open_press_keyup(&p, 0x70, 2));
    open_press_keydown(&p, 0x7B, 0, 1, 1);
    open_press_keydown(&p, 0x57, 0, 1, 1);
    CHECK(open_press_keyup(&p, 0x7B, 2));
    open_press_keydown(&p, 0x1B, 0, 1, 1);
    open_press_reset(&p);
    CHECK(!open_press_keyup(&p, 0x1B, 2));
}

static void test_safemode(void) {
    CHECK(safemode_value_virtual(0) && !safemode_value_virtual(1));
    CHECK(safemode_prompt_args(0x0040E4D0, 0x23, 1, 11));
    CHECK(!safemode_prompt_args(0x0040E4D0, 0x23, 1, 0x13));
    CHECK(!safemode_prompt_args(0x0040E4D0, 0x10001, 0x16, 2));
    CHECK(!safemode_prompt_args(0x0040E590, 0x23, 1, 11));
}

/* A render target whose GPU picture is kept at the legacy size: performs target_sync's actions the way
 * d3d11_renderer.c does, so whole sequences of the game's calls can be checked headless. */
#define SIM_W 32
#define SIM_H 24
#define SIM_PITCH (SIM_W * 2 + 8)

typedef struct {
    tsync_t sync;
    uint16_t gpu[SIM_W * SIM_H];
    uint8_t copy[SIM_PITCH * SIM_H];
    uint8_t front[SIM_PITCH * SIM_H];
    uint16_t reference[SIM_W * SIM_H];
    d3dt_rect_t dirty;
    unsigned readbacks, uploads, front_copies;
    tsync_counts_t written;
} sim_t;

static const d3dt_rect_t k_sim_all = { 0, 0, SIM_W, SIM_H };

static uint16_t* sim_px(uint8_t* buffer, long x, long y) { return (uint16_t*)(buffer + y * SIM_PITCH) + x; }

static void sim_load(sim_t* s, const uint16_t* gpu, uint8_t* src) {
    for (long y = 0; y < SIM_H; y++)
        for (long x = 0; x < SIM_W; x++) {
            uint16_t v = gpu ? gpu[y * SIM_W + x] : *sim_px(src, x, y);
            *sim_px(s->copy, x, y) = v;
            s->reference[y * SIM_W + x] = v;
        }
}

static void sim_apply(sim_t* s, unsigned acts, const d3dt_rect_t* region) {
    if (acts & TSYNC_COMPOSITE) {
        d3dt_rect_t box, area = d3dt_rect_empty(&s->dirty) ? k_sim_all : s->dirty;
        memset(&s->dirty, 0, sizeof s->dirty);
        if (tsync_changed_box(s->copy, SIM_PITCH, s->reference, SIM_W, &area, &box)) {
            static uint32_t texels[SIM_W * SIM_H];
            long bw = box.right - box.left;
            tsync_overlay_box(s->copy, SIM_PITCH, s->reference, SIM_W, &box, texels, &s->written);
            for (long y = box.top; y < box.bottom; y++)
                for (long x = box.left; x < box.right; x++) {
                    uint32_t texel = texels[(y - box.top) * bw + (x - box.left)];
                    if (texel >> 24) s->gpu[y * SIM_W + x] = d3dt_bgra_to_rgb565(texel);
                }
        }
    }
    if (acts & TSYNC_UPLOAD) {
        for (long y = 0; y < SIM_H; y++)
            for (long x = 0; x < SIM_W; x++) s->gpu[y * SIM_W + x] = s->reference[y * SIM_W + x] = *sim_px(s->copy, x, y);
        s->uploads++;
    }
    if (acts & TSYNC_READBACK) {
        sim_load(s, s->gpu, NULL);
        s->readbacks++;
    }
    if (acts & TSYNC_COPY_FRONT) {
        sim_load(s, NULL, s->front);
        s->front_copies++;
    }
    if (acts & TSYNC_SYNTHESIZE) tsync_fill(s->copy, SIM_PITCH, s->reference, SIM_W, &k_sim_all, s->sync.color);
    if (acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL))
        tsync_fill(s->copy, SIM_PITCH, s->reference, SIM_W, (acts & TSYNC_CAPTURE_ALL) || !region ? &k_sim_all : region,
                   TSYNC_UNWRITTEN);
}

static void sim_init(sim_t* s) {
    memset(s, 0, sizeof *s);
    tsync_attach(&s->sync);
}

static void sim_lock(sim_t* s, int reads) {
    sim_apply(s, tsync_cpu_access(&s->sync, reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ), &k_sim_all);
}

static void sim_write(sim_t* s, long left, long top, long right, long bottom, uint16_t value) {
    for (long y = top; y < bottom; y++)
        for (long x = left; x < right; x++) *sim_px(s->copy, x, y) = value;
}

static void sim_unlock(sim_t* s) {
    tsync_cpu_wrote(&s->sync);
    d3dt_rect_union(&s->dirty, &k_sim_all);
}

static void sim_draw(sim_t* s, long left, long top, long right, long bottom, uint16_t value) {
    sim_apply(s, tsync_gpu_draw(&s->sync), NULL);
    for (long y = top; y < bottom; y++)
        for (long x = left; x < right; x++) s->gpu[y * SIM_W + x] = value;
}

static void sim_clear(sim_t* s, uint16_t value) {
    tsync_gpu_clear(&s->sync, value);
    for (int i = 0; i < SIM_W * SIM_H; i++) s->gpu[i] = value;
}

static void sim_exchange(sim_t* s) {
    static uint8_t keep[SIM_PITCH * SIM_H];
    memcpy(keep, s->front, sizeof keep);
    memcpy(s->front, s->copy, sizeof keep);
    memcpy(s->copy, keep, sizeof keep);
}

/* 1 when the GPU picture was presented, 0 when DirectDraw showed the CPU copy. */
static int sim_flip(sim_t* s) {
    unsigned acts = tsync_present(&s->sync);
    if (!tsync_presents_gpu(&s->sync)) {
        sim_exchange(s);
        return 0;
    }
    sim_apply(s, acts, NULL);
    sim_exchange(s);
    tsync_flipped(&s->sync);
    return 1;
}

static uint16_t sim_pattern(long x, long y) { return (uint16_t)(0x0841u * (1 + (x + y) % 7) | 0x0020u); }

static void sim_draw_pattern(sim_t* s) {
    sim_apply(s, tsync_gpu_draw(&s->sync), NULL);
    for (long y = 0; y < SIM_H; y++)
        for (long x = 0; x < SIM_W; x++) s->gpu[y * SIM_W + x] = sim_pattern(x, y);
}

static int sim_gpu_is(const sim_t* s, long left, long top, long right, long bottom, uint16_t inside) {
    for (long y = 0; y < SIM_H; y++)
        for (long x = 0; x < SIM_W; x++) {
            int in = x >= left && x < right && y >= top && y < bottom;
            if (s->gpu[y * SIM_W + x] != (in ? inside : sim_pattern(x, y))) return 0;
        }
    return 1;
}

static void test_target_access_policy(void) {
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_BEGIN_FRAME, 0));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_FLUSH_BATCH, 0));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x00513660, 0));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x00504D40, 0));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_CLEAR, 0));
    /* Polygon walkers are no longer named one by one: their Locks go through the CPU polygon path. */
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x005D66F0, 0));
    {
        static const float x[] = { 100.4f, 140.0f, 120.2f }, y[] = { 50.0f, 52.6f, 90.1f };
        static const float off[] = { -40.0f, 3500.0f, 10.0f }, nan_y[] = { 0.0f, 1e30f, 1.0f };
        long box[4];
        CHECK(cpu_polygon_box(x, y, 3, 3440, 1440, 2, box) && box[0] == 98 && box[1] == 48 && box[2] == 142 &&
              box[3] == 93);
        CHECK(cpu_polygon_box(off, y, 3, 3440, 1440, 2, box) && box[0] == 0 && box[2] == 3440);
        CHECK(!cpu_polygon_box(x, nan_y, 3, 3440, 1440, 2, box));
        CHECK(!cpu_polygon_box(x, y, 0, 3440, 1440, 2, box));
        CHECK(!cpu_polygon_box(off, y, 1, 3440, 1440, 2, box));
        /* A polygon box is a few hundred pixels of a 4.95 M-pixel screen. */
        CHECK(cpu_polygon_box(x, y, 3, 3440, 1440, 2, box) && (box[2] - box[0]) * (box[3] - box[1]) < 3440 * 1440 / 1000);
    }
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x21, 0x00435590, 0));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x2001, 0x00435590, 0));
    CHECK(target_access_reads(CPU_ACCESS_GETDC, 0x20, LOCK_CALLER_BEGIN_FRAME, 0));
    CHECK(target_access_reads(CPU_ACCESS_GETDC, 0, 0x005038E0, 0));
    CHECK(!target_access_reads(CPU_ACCESS_GETDC, 0, GETDC_CALLER_TEXT_OVERLAY, 0));
    CHECK(target_access_reads(CPU_ACCESS_BLT_SOURCE, 0, 0, 0));
    CHECK(!target_access_reads(CPU_ACCESS_BLT_DEST, 0, 0, 0));
    CHECK(!strcmp(tsync_location_name(TSYNC_CPU | TSYNC_GPU), "CPU+GPU"));
    CHECK(!strcmp(tsync_location_name(0), "none"));
}

static void test_target_sync_buffers(void) {
    static uint8_t copy[SIM_PITCH * SIM_H];
    static uint16_t reference[SIM_W * SIM_H];
    static uint32_t texels[SIM_W * SIM_H];
    d3dt_rect_t box, area = { 2, 3, 20, 9 };
    tsync_counts_t counts = { 0, 0 };
    tsync_fill(copy, SIM_PITCH, reference, SIM_W, &k_sim_all, 0x1234);
    CHECK(!tsync_changed_box(copy, SIM_PITCH, reference, SIM_W, &k_sim_all, &box));
    *sim_px(copy, 5, 4) = 0;
    *sim_px(copy, 12, 7) = 0x07E0;
    *sim_px(copy, 25, 20) = 0x001F;
    CHECK(tsync_changed_box(copy, SIM_PITCH, reference, SIM_W, &area, &box));
    CHECK(box.left == 5 && box.top == 4 && box.right == 13 && box.bottom == 8);
    tsync_overlay_box(copy, SIM_PITCH, reference, SIM_W, &box, texels, &counts);
    CHECK(counts.changed == 2 && counts.black == 1);
    CHECK(texels[0] == 0xFF000000u && texels[1] == 0);
    CHECK(reference[4 * SIM_W + 5] == 0 && reference[7 * SIM_W + 12] == 0x07E0);
    CHECK(!tsync_changed_box(copy, SIM_PITCH, reference, SIM_W, &area, &box));
    CHECK(tsync_changed_box(copy, SIM_PITCH, reference, SIM_W, &k_sim_all, &box) && box.left == 25 && box.top == 20);
}

/* GPU picture not black, CPU copy left black (stale), the CPU writes a 10x10 square: only the square
 * changes on the GPU, black pixels written inside it included. */
static void test_stale_black_cannot_replace_gpu(void) {
    sim_t s;
    for (int reads = 0; reads < 2; reads++) {
        sim_init(&s);
        sim_draw_pattern(&s);
        CHECK(s.uploads == 1);
        CHECK(s.sync.valid == TSYNC_GPU);
        sim_lock(&s, reads);
        CHECK(s.readbacks == (unsigned)reads);
        sim_write(&s, 4, 5, 14, 15, 0x0000);
        sim_unlock(&s);
        CHECK(sim_flip(&s));
        CHECK(sim_gpu_is(&s, 4, 5, 14, 15, 0x0000));
        CHECK(s.written.changed == 100 && s.written.black == 100);
        CHECK(s.uploads == 1);
    }
}

static void test_no_read_lock_on_gpu_picture(void) {
    sim_t s;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 0);
    sim_unlock(&s);
    sim_draw(&s, 0, 0, 1, 1, sim_pattern(0, 0));
    CHECK(s.readbacks == 0 && s.written.changed == 0);
    CHECK(sim_gpu_is(&s, 0, 0, 0, 0, 0));
    CHECK(s.sync.valid == TSYNC_GPU);
}

static void test_reading_lock_reads_back(void) {
    sim_t s;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 1);
    CHECK(s.readbacks == 1 && s.sync.valid == (TSYNC_CPU | TSYNC_GPU) && s.sync.exact);
    CHECK(*sim_px(s.copy, 9, 9) == sim_pattern(9, 9));
    sim_unlock(&s);
    sim_lock(&s, 1);
    CHECK(s.readbacks == 1);
}

/* Writes the GPU has not received are composited before a readback could overwrite them. */
static void test_pending_writes_survive_readback(void) {
    sim_t s;
    unsigned acts;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 0);
    sim_write(&s, 3, 3, 4, 4, 0x0000);
    sim_unlock(&s);
    acts = tsync_cpu_access(&s.sync, TSYNC_ACCESS_READ);
    CHECK(acts == (TSYNC_COMPOSITE | TSYNC_READBACK));
    sim_apply(&s, acts, &k_sim_all);
    CHECK(s.gpu[3 * SIM_W + 3] == 0 && *sim_px(s.copy, 3, 3) == 0 && s.readbacks == 1);
}

/* A Lock as the renderer runs it on a target whose pages it can protect: what cannot wait now, the
 * rest at the first touch; an Unlock without a touch restores the state of before. */
static void sim_lock_deferred(sim_t* s, int reads, tsync_deferred_t* d) {
    sim_apply(s, tsync_lock_deferred(&s->sync, reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ, d), &k_sim_all);
}

static void sim_touch(sim_t* s, tsync_deferred_t* d) { sim_apply(s, tsync_lock_touched(d), &k_sim_all); }

static void sim_unlock_deferred(sim_t* s, tsync_deferred_t* d) {
    if (d->acts) tsync_lock_untouched(&s->sync, d);
    else sim_unlock(s);
}

/* READ: the pixels are the GPU picture when first read. */
static void test_deferred_read_keeps_content(void) {
    sim_t s;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock_deferred(&s, 1, &d);
    CHECK(d.acts == TSYNC_READBACK && s.readbacks == 0);
    sim_touch(&s, &d);
    CHECK(s.readbacks == 1 && d.acts == 0);
    CHECK(*sim_px(s.copy, 7, 11) == sim_pattern(7, 11) && *sim_px(s.copy, 0, 0) == sim_pattern(0, 0));
    sim_unlock_deferred(&s, &d);
    CHECK(sim_flip(&s) && sim_gpu_is(&s, 0, 0, 0, 0, 0) && s.written.changed == 0);
}

/* WRITE: a Lock that never reads captures instead and never reads back. */
static void test_deferred_write_never_reads(void) {
    sim_t s;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock_deferred(&s, 0, &d);
    CHECK((d.acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL)) && !(d.acts & TSYNC_READBACK));
    sim_touch(&s, &d);
    sim_write(&s, 2, 2, 6, 6, 0x0000);
    sim_unlock_deferred(&s, &d);
    CHECK(sim_flip(&s) && sim_gpu_is(&s, 2, 2, 6, 6, 0x0000) && s.readbacks == 0 && s.written.changed == 16);
}

/* READ_WRITE: read back at the touch, then the writes reach the GPU and nothing else changes. */
static void test_deferred_read_write(void) {
    sim_t s;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock_deferred(&s, 1, &d);
    sim_touch(&s, &d);
    CHECK(*sim_px(s.copy, 4, 4) == sim_pattern(4, 4));
    sim_write(&s, 4, 4, 9, 6, 0x001F);
    sim_unlock_deferred(&s, &d);
    sim_draw(&s, 30, 30, 31, 31, sim_pattern(30, 30));
    CHECK(s.readbacks == 1 && sim_gpu_is(&s, 4, 4, 9, 6, 0x001F) && s.written.changed == 10);
}

/* SYNC_ONLY: a Lock/Unlock that touches nothing (the game waiting for the GPU) reads nothing back, writes
 * nothing and leaves the state as it was, so later accesses decide as if it never happened. */
static void test_deferred_sync_only(void) {
    sim_t s;
    tsync_t before;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    before = s.sync;
    for (int i = 0; i < 3; i++) {
        sim_lock_deferred(&s, i != 1, &d);
        CHECK(d.acts != 0);
        sim_unlock_deferred(&s, &d);
        CHECK(!memcmp(&s.sync, &before, sizeof before));
    }
    CHECK(s.readbacks == 0);
    sim_draw(&s, 0, 0, 1, 1, sim_pattern(0, 0));
    CHECK(s.written.changed == 0 && sim_gpu_is(&s, 0, 0, 0, 0, 0));
    sim_lock(&s, 1);
    CHECK(s.readbacks == 1 && *sim_px(s.copy, 12, 3) == sim_pattern(12, 3));
}

/* Pending CPU writes reach the GPU at the Lock itself, so an untouched Lock cannot lose them. */
static void test_deferred_keeps_pending_writes(void) {
    sim_t s;
    tsync_deferred_t d;
    unsigned now;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 0);
    sim_write(&s, 3, 3, 4, 4, 0x0000);
    sim_unlock(&s);
    now = tsync_lock_deferred(&s.sync, TSYNC_ACCESS_READ, &d);
    CHECK(now == TSYNC_COMPOSITE && d.acts == TSYNC_READBACK);
    sim_apply(&s, now, &k_sim_all);
    CHECK(s.gpu[3 * SIM_W + 3] == 0);
    sim_unlock_deferred(&s, &d);
    CHECK(!s.sync.pending && s.readbacks == 0);
    CHECK(sim_flip(&s) && sim_gpu_is(&s, 3, 3, 4, 4, 0x0000));
}

/* Nothing waits when the CPU copy is already the picture; after a full clear the colour waits. */
static void test_deferred_current_copy_and_clear(void) {
    sim_t s;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 1);
    sim_unlock(&s);
    sim_lock_deferred(&s, 1, &d);
    CHECK(d.acts == 0 && s.readbacks == 1);
    sim_unlock_deferred(&s, &d);
    sim_clear(&s, 0x1234);
    sim_lock_deferred(&s, 1, &d);
    CHECK(d.acts == TSYNC_SYNTHESIZE);
    sim_unlock_deferred(&s, &d);
    CHECK(s.sync.uniform && s.sync.color == 0x1234 && !(s.sync.valid & TSYNC_CPU));
    sim_lock_deferred(&s, 1, &d);
    sim_touch(&s, &d);
    CHECK(*sim_px(s.copy, 5, 5) == 0x1234 && s.readbacks == 1);
}

/* The hardware water frame: FlushBatch and the water upload wait for the GPU with Lock/Unlock and
 * BeginFrame only hands out the address; none touches a pixel, so the frame reads nothing back. */
static void test_deferred_water_frame(void) {
    sim_t s;
    tsync_deferred_t d;
    sim_init(&s);
    sim_draw_pattern(&s);
    CHECK(sim_flip(&s));
    for (int frame = 0; frame < 3; frame++) {
        sim_clear(&s, 0);
        sim_draw_pattern(&s);
        sim_lock_deferred(&s, 0, &d);
        sim_unlock_deferred(&s, &d);
        sim_lock_deferred(&s, 1, &d);
        sim_unlock_deferred(&s, &d);
        sim_draw(&s, 0, 0, 2, 2, 0x07E0);
        sim_lock_deferred(&s, 0, &d);
        sim_unlock_deferred(&s, &d);
        CHECK(sim_flip(&s) && sim_gpu_is(&s, 0, 0, 2, 2, 0x07E0));
    }
    CHECK(s.readbacks == 0 && s.written.changed == 0);
}

/* Each format's definition, independent of the converter. */
static uint32_t oracle_texel(int format, uint32_t v) {
    switch (format) {
    case D3DT_FMT_RGB565: {
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case D3DT_FMT_XRGB1555:
    case D3DT_FMT_ARGB1555: {
        uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        uint32_t a = format == D3DT_FMT_XRGB1555 || (v & 0x8000) ? 255 : 0;
        return a << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    case D3DT_FMT_ARGB4444:
    case D3DT_FMT_XRGB4444: {
        uint32_t a = format == D3DT_FMT_XRGB4444 ? 15 : (v >> 12) & 15;
        return (a * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | (v & 15) * 17;
    }
    default: return 0;
    }
}

static void test_convert_rows(void) {
    static const int formats[] = { D3DT_FMT_RGB565, D3DT_FMT_XRGB1555, D3DT_FMT_ARGB1555, D3DT_FMT_ARGB4444,
                                   D3DT_FMT_XRGB4444 };
    static uint16_t in[65536];
    static uint32_t out[65536];
    static const uint32_t pixels32[] = { 0x00000000u, 0x12345678u, 0xFFFFFFFFu, 0x80FF0001u };
    uint32_t out32[4];
    int bad = 0;
    for (uint32_t v = 0; v < 65536; v++) in[v] = (uint16_t)v;
    for (int f = 0; f < 5; f++)
        for (int keyed = 0; keyed < 2; keyed++) {
            uint32_t low = 0x1200, high = 0x9FFF;
            d3dt_convert_row(formats[f], in, out, 65536, keyed, low, high);
            for (uint32_t v = 0; v < 65536; v++) {
                uint32_t want = oracle_texel(formats[f], v);
                if (keyed && v >= low && v <= high) want &= 0x00FFFFFFu;
                bad += out[v] != want;
            }
        }
    CHECK(bad == 0);
    d3dt_convert_row(D3DT_FMT_XRGB8888, pixels32, out32, 4, 1, 0x12345678u, 0x12345678u);
    CHECK(out32[0] == 0xFF000000u && out32[1] == 0x00345678u && out32[2] == 0xFFFFFFFFu && out32[3] == 0xFFFF0001u);
    d3dt_convert_row(D3DT_FMT_ARGB8888, pixels32, out32, 4, 0, 0, 0);
    CHECK(!memcmp(out32, pixels32, sizeof out32));
    out32[0] = 7;
    d3dt_convert_row(D3DT_FMT_UNKNOWN, pixels32, out32, 1, 1, 0, 0xFFFFFFFFu);
    CHECK(out32[0] == 0);
}

/* The game's hardware frame: Clear(0), FlushBatch, D3D sky, terrain, BeginFrame, a software polygon,
 * a D3D polygon over part of it, FlushBatch, Flip. One readback, for the polygon that may read. */
static void test_hybrid_frame(void) {
    sim_t s;
    sim_init(&s);
    for (int frame = 0; frame < 3; frame++) {
        unsigned readbacks = s.readbacks;
        sim_clear(&s, 0);
        sim_lock(&s, 0);
        sim_unlock(&s);
        sim_draw(&s, 0, 0, SIM_W, 8, 0x5DDF);
        sim_draw(&s, 0, 8, SIM_W, SIM_H, 0xC618);
        sim_lock(&s, 0);
        sim_unlock(&s);
        sim_lock(&s, 1);
        sim_write(&s, 10, 10, 18, 18, 0x0000);
        sim_write(&s, 10, 10, 14, 18, 0x07E0);
        sim_unlock(&s);
        sim_draw(&s, 16, 16, 20, 20, 0xF800);
        sim_lock(&s, 0);
        sim_unlock(&s);
        CHECK(sim_flip(&s));
        CHECK(s.readbacks == readbacks + 1);
        CHECK(s.gpu[2 * SIM_W + 2] == 0x5DDF && s.gpu[20 * SIM_W + 2] == 0xC618);
        CHECK(s.gpu[12 * SIM_W + 11] == 0x07E0 && s.gpu[12 * SIM_W + 15] == 0x0000);
        CHECK(s.gpu[17 * SIM_W + 17] == 0xF800 && s.gpu[19 * SIM_W + 19] == 0xF800);
        CHECK(s.gpu[10 * SIM_W + 20] == 0xC618);
    }
    CHECK(s.uploads == 0);
}

/* Menus and videos drawn by the CPU while a Direct3D device exists. */
static void test_cpu_made_frames(void) {
    sim_t s;
    sim_init(&s);
    sim_lock(&s, 1);
    sim_write(&s, 0, 0, SIM_W, SIM_H, 0x1111);
    sim_unlock(&s);
    CHECK(!sim_flip(&s));
    CHECK(s.sync.valid == TSYNC_CPU && s.readbacks == 0 && s.uploads == 0);

    sim_init(&s);
    sim_clear(&s, 0);
    sim_lock(&s, 1);
    CHECK(s.readbacks == 0 && s.sync.exact);
    sim_write(&s, 2, 2, 30, 20, 0x2222);
    sim_write(&s, 4, 4, 6, 6, 0x0000);
    sim_unlock(&s);
    CHECK(sim_flip(&s));
    CHECK(s.gpu[3 * SIM_W + 3] == 0x2222 && s.gpu[5 * SIM_W + 5] == 0 && s.gpu[0] == 0);
    CHECK(s.sync.front_mirror);
    for (int frame = 0; frame < 4; frame++) {
        sim_lock(&s, 1);
        CHECK(*sim_px(s.copy, 3, 3) == 0x2222);
        sim_write(&s, 8, 8, 16, 12, (uint16_t)(0x3000 + frame));
        sim_unlock(&s);
        CHECK(sim_flip(&s));
        CHECK(s.gpu[9 * SIM_W + 9] == 0x3000 + frame && s.gpu[3 * SIM_W + 3] == 0x2222);
    }
    CHECK(s.readbacks == 0 && s.front_copies == 4);
    tsync_front_changed(&s.sync);
    sim_lock(&s, 1);
    CHECK(s.readbacks == 1);
}

static void test_gpu_lost_and_resized(void) {
    sim_t s;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_apply(&s, tsync_gpu_release(&s.sync), NULL);
    CHECK(s.readbacks == 1 && s.sync.valid == TSYNC_CPU);
    memset(s.gpu, 0, sizeof s.gpu);
    sim_draw(&s, 0, 0, 1, 1, sim_pattern(0, 0));
    CHECK(s.uploads == 2 && sim_gpu_is(&s, 0, 0, 0, 0, 0));

    sim_init(&s);
    sim_draw_pattern(&s);
    tsync_gpu_gone(&s.sync);
    CHECK(s.sync.valid == TSYNC_GPU && s.sync.uniform && s.sync.color == 0);
    sim_lock(&s, 1);
    CHECK(s.readbacks == 0 && *sim_px(s.copy, 1, 1) == 0);
}

/* A full clear covers writes the GPU had not received. */
static void test_clear_drops_pending(void) {
    sim_t s;
    sim_init(&s);
    sim_draw_pattern(&s);
    sim_lock(&s, 0);
    sim_write(&s, 1, 1, 5, 5, 0x00FF);
    sim_unlock(&s);
    sim_clear(&s, 0x0841);
    CHECK(sim_flip(&s));
    CHECK(s.written.changed == 0 && s.gpu[2 * SIM_W + 2] == 0x0841);
}

static void test_icall_ring(void) {
    uint32_t targets[8] = { 0 }, from[8] = { 0 };
    static const uint32_t wrappers[] = { 0x00503400, 0x00504D20 };
    uint32_t next = 0;
    CHECK(icall_ring_caller(targets, from, next, 8, wrappers, 2) == 0);
    targets[next & 7] = 0x00504D20, from[next & 7] = 0x0041A000, next++;
    targets[next & 7] = 0x0D2E0000, from[next & 7] = 0x00503400, next++;
    CHECK(icall_ring_caller(targets, from, next, 8, wrappers, 2) == 0x0041A000);
    targets[next & 7] = 0x00503400, from[next & 7] = 0x005FB070, next++;
    targets[next & 7] = 0x0D2E0000, from[next & 7] = 0x00503400, next++;
    CHECK(icall_ring_caller(targets, from, next, 8, wrappers, 2) == 0x005FB070);
    for (int i = 0; i < 9; i++) targets[next & 7] = 0x00600000, from[next & 7] = 0x00400000, next++;
    CHECK(icall_ring_caller(targets, from, next, 8, wrappers, 2) == 0);
    /* RasterBlt -> CRasterWin::LockAll (0x00504D40) -> tail jump to CRasterVid::Lock -> DirectDraw (log 027). */
    {
        static const uint32_t lock_wrappers[] = { 0x00503400, 0x00504D20, 0x00504D40 };
        targets[next & 7] = 0x00504D40, from[next & 7] = LOCK_CALLER_RASTER_BLT, next++;
        targets[next & 7] = 0x00503400, from[next & 7] = 0x00504D40, next++;
        targets[next & 7] = 0x0D2E0000, from[next & 7] = 0x00503400, next++;
        CHECK(icall_ring_caller(targets, from, next, 8, lock_wrappers, 3) == LOCK_CALLER_RASTER_BLT);
        targets[next & 7] = 0x00504D40, from[next & 7] = LOCK_CALLER_CLEAR, next++;
        targets[next & 7] = 0x00503400, from[next & 7] = 0x00504D40, next++;
        CHECK(icall_ring_caller(targets, from, next, 8, lock_wrappers, 3) == LOCK_CALLER_CLEAR);
    }
}

static void test_readback_classes(void) {
    CHECK(readback_classify(CPU_ACCESS_LOCK, 1, 0x500000) == READBACK_LOCK_READ);
    CHECK(readback_classify(CPU_ACCESS_LOCK, 0, 0x500000) == READBACK_LOCK_WRITE);
    CHECK(readback_classify(CPU_ACCESS_LOCK, 0, 0x513660) == READBACK_PIXEL_CALIBRATION);
    CHECK(readback_classify(CPU_ACCESS_GETDC, 0, 1) == READBACK_GETDC);
    CHECK(readback_classify(CPU_ACCESS_BLT_SOURCE, 1, 1) == READBACK_BLT_SOURCE);
    CHECK(readback_classify(CPU_ACCESS_FRONT, 1, 1) == READBACK_FRONT_BUFFER);
    CHECK(readback_classify(CPU_ACCESS_OTHER, 1, 1) == READBACK_OTHER);
    CHECK(!strcmp(readback_reason_name(READBACK_GETDC), "READBACK_GETDC"));
}

static void test_primitives(void) {
    uint32_t idx[64];
    uint16_t in[6] = { 5, 6, 7, 8, 9, 10 };
    CHECK(d3dt_primitive_count(D3DT_PT_TRIANGLEFAN, 5) == 3);
    CHECK(d3dt_primitive_count(D3DT_PT_TRIANGLESTRIP, 2) == 0);
    CHECK(d3dt_primitive_count(D3DT_PT_LINESTRIP, 4) == 3);
    CHECK(d3dt_primitive_count(9, 4) == 0);
    CHECK(d3dt_fan_indices(10, 5, idx) == 9);
    CHECK(idx[0] == 11 && idx[1] == 12 && idx[2] == 10);
    CHECK(idx[6] == 13 && idx[7] == 14 && idx[8] == 10);
    CHECK(d3dt_fan_indices(0, 2, idx) == 0);
    CHECK(d3dt_strip_indices(0, 4, idx) == 6);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
    CHECK(idx[3] == 1 && idx[4] == 3 && idx[5] == 2);
    CHECK(d3dt_list_indices(4, 7, idx) == 6 && idx[5] == 9);
    CHECK(d3dt_indexed_triangles(D3DT_PT_TRIANGLEFAN, in, 4, 100, idx) == 6);
    CHECK(idx[0] == 106 && idx[1] == 107 && idx[2] == 105 && idx[5] == 105);
    CHECK(d3dt_indexed_triangles(D3DT_PT_TRIANGLELIST, in, 6, 0, idx) == 6 && idx[4] == 9);
    CHECK(d3dt_indexed_triangles(D3DT_PT_TRIANGLESTRIP, in, 4, 0, idx) == 6 && idx[4] == 8 && idx[5] == 7);
}

static void test_states(void) {
    int s, d, sa, da;
    CHECK(d3dt_blend(5, 0) == 5 && d3dt_blend(6, 0) == 6);
    CHECK(d3dt_blend(3, 1) == 5 && d3dt_blend(10, 1) == 8 && d3dt_blend(3, 0) == 3);
    CHECK(d3dt_blend(0, 0) == 0 && d3dt_blend(12, 0) == 0);
    CHECK(d3dt_blend_pair(5, 6, &s, &d, &sa, &da) && s == 5 && d == 6 && sa == 5 && da == 6);
    CHECK(d3dt_blend_pair(12, 1, &s, &d, &sa, &da) && s == 5 && d == 6);
    CHECK(d3dt_blend_pair(13, 1, &s, &d, &sa, &da) && s == 6 && d == 5);
    CHECK(!d3dt_blend_pair(0, 2, &s, &d, &sa, &da));
    CHECK(d3dt_compare(7) == 7 && d3dt_compare(0) == 0 && d3dt_compare(9) == 0);
    CHECK(d3dt_address(3) == 3 && d3dt_address(5) == 0);
    CHECK(d3dt_cull(1) == 1 && d3dt_cull(2) == 2 && d3dt_cull(3) == 3 && d3dt_cull(4) == 0);
    CHECK(d3dt_fill(3) == 3 && d3dt_fill(2) == 2 && d3dt_fill(1) == 0);
    CHECK(d3dt_mag_linear(1) == 0 && d3dt_mag_linear(2) == 1 && d3dt_mag_linear(5) == 1 && d3dt_mag_linear(9) == -1);
    CHECK(d3dt_min_linear(1) == 0 && d3dt_min_linear(3) == 1 && d3dt_min_linear(4) == -1);
    CHECK(d3dt_mip_mode(1) == 0 && d3dt_mip_mode(3) == 2 && d3dt_mip_mode(0) == -1);
    CHECK(d3dt_alpha_ref(0x1000) == 0 && d3dt_alpha_ref(0x80) == 0x80);
    CHECK(!strcmp(d3dt_render_state_name(47), "ZBIAS") && !strcmp(d3dt_render_state_name(48), "RANGEFOGENABLE"));
    CHECK(!strcmp(d3dt_render_state_name(21), "TEXTUREMAPBLEND") && !strcmp(d3dt_render_state_name(27), "ALPHABLENDENABLE"));
    CHECK(!strcmp(d3dt_stage_state_name(12), "ADDRESS") && !strcmp(d3dt_stage_state_name(16), "MAGFILTER"));
    CHECK(!strcmp(d3dt_stage_state_name(17), "MINFILTER") && !strcmp(d3dt_stage_state_name(99), "unknown"));
    CHECK(d3dt_depth_bias(0, 7) == 0 && d3dt_depth_bias(2, 7) > 0 && d3dt_depth_bias(2, 4) < 0);
    CHECK(d3dt_depth_bias(40, 7) == d3dt_depth_bias(16, 7));
    /* 5e-6 of depth per step, in 2^-24 units, towards greater z for GREATER/GREATEREQUAL. */
    CHECK(d3dt_depth_bias(1, 7) == 84 && d3dt_depth_bias(1, 5) == 84 && d3dt_depth_bias(1, 4) == -84);
    CHECK(d3dt_depth_bias(16, 7) == 16 * 84 && d3dt_depth_bias(3, 8) == -3 * 84);
    CHECK(d3dt_depth_bias(1, 7) * (1.0 / 16777216.0) > 4.9e-6 && d3dt_depth_bias(1, 7) * (1.0 / 16777216.0) < 5.1e-6);
}

static void test_texture_blend(void) {
    uint32_t tex = 0x80FF8000u, diffuse = 0x40808080u;
    CHECK(d3dt_texture_blend(1, tex, diffuse, 1) == tex);
    CHECK(d3dt_texture_blend(1, tex, diffuse, 0) == 0xFFFF8000u);
    CHECK(d3dt_texture_blend(7, tex, diffuse, 0) == 0xFFFF8000u);
    CHECK(d3dt_texture_blend(2, tex, diffuse, 1) == 0x80804000u);
    CHECK(d3dt_texture_blend(2, tex, diffuse, 0) == 0x40804000u);
    CHECK(d3dt_texture_blend(6, tex, diffuse, 0) == 0x40804000u);
    CHECK(d3dt_texture_blend(4, tex, diffuse, 1) == 0x20804000u);
    CHECK(d3dt_texture_blend(8, tex, diffuse, 1) == 0x40FFFF80u);
    CHECK((d3dt_texture_blend(3, 0x00FFFFFFu, 0x40102030u, 1) & 0xFFFFFFu) == 0x102030u);
    CHECK(d3dt_texture_blend(3, 0xFFFFFFFFu, 0x40102030u, 1) == 0x40FFFFFFu);
    CHECK(d3dt_texture_blend_known(8) && !d3dt_texture_blend_known(9) && !d3dt_texture_blend_known(0));
}

static void test_formats(void) {
    uint16_t px[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    uint16_t argb1555[2] = { 0x7C00, 0x8000 | 0x03E0 };
    uint16_t argb4444[1] = { 0x8F00 };
    uint32_t out[4];
    CHECK(d3dt_format_from_masks(16, 0xF800, 0x07E0, 0x1F, 0) == D3DT_FMT_RGB565);
    CHECK(d3dt_format_from_masks(16, 0x7C00, 0x03E0, 0x1F, 0x8000) == D3DT_FMT_ARGB1555);
    CHECK(d3dt_format_from_masks(16, 0x7C00, 0x03E0, 0x1F, 0) == D3DT_FMT_XRGB1555);
    CHECK(d3dt_format_from_masks(16, 0x0F00, 0x00F0, 0x0F, 0xF000) == D3DT_FMT_ARGB4444);
    CHECK(d3dt_format_from_masks(32, 0xFF0000, 0xFF00, 0xFF, 0) == D3DT_FMT_XRGB8888);
    CHECK(d3dt_format_from_masks(8, 0, 0, 0, 0) == D3DT_FMT_UNKNOWN);
    CHECK(d3dt_format_has_alpha(D3DT_FMT_ARGB4444) && !d3dt_format_has_alpha(D3DT_FMT_RGB565));
    d3dt_convert_row(D3DT_FMT_RGB565, px, out, 4, 0, 0, 0);
    CHECK(out[0] == 0xFFFF0000u && out[1] == 0xFF00FF00u && out[2] == 0xFF0000FFu && out[3] == 0xFFFFFFFFu);
    d3dt_convert_row(D3DT_FMT_RGB565, px, out, 4, 1, 0x07E0, 0x07E0);
    CHECK(out[1] == 0x0000FF00u && out[0] == 0xFFFF0000u);
    d3dt_convert_row(D3DT_FMT_ARGB1555, argb1555, out, 2, 0, 0, 0);
    CHECK(out[0] == 0x00FF0000u && out[1] == 0xFF00FF00u);
    d3dt_convert_row(D3DT_FMT_ARGB4444, argb4444, out, 1, 0, 0, 0);
    CHECK(out[0] == 0x88FF0000u);
    for (uint32_t v = 0; v < 65536; v += 97) CHECK(d3dt_bgra_to_rgb565(d3dt_rgb565_to_bgra((uint16_t)v)) == v);
    CHECK(d3dt_format_from_masks(16, 0x0F00, 0x00F0, 0x0F, 0) == D3DT_FMT_XRGB4444);
    CHECK(d3dt_format_from_pixelformat(0x40 | 0x1, 16, 0x0F00, 0x00F0, 0x0F, 0xF000) == D3DT_FMT_ARGB4444);
    CHECK(d3dt_format_from_pixelformat(0x40, 16, 0x0F00, 0x00F0, 0x0F, 0xF000) == D3DT_FMT_XRGB4444);
    CHECK(d3dt_format_from_pixelformat(0x40, 16, 0x7C00, 0x03E0, 0x1F, 0x8000) == D3DT_FMT_XRGB1555);
    CHECK(d3dt_format_from_pixelformat(0x20, 8, 0, 0, 0, 0) == D3DT_FMT_PAL8);
    CHECK(d3dt_format_from_pixelformat(0x20 | 0x40, 16, 0xF800, 0x7E0, 0x1F, 0) == D3DT_FMT_UNKNOWN);
    CHECK(d3dt_format_from_pixelformat(0, 16, 0xF800, 0x7E0, 0x1F, 0) == D3DT_FMT_UNKNOWN);
    CHECK(d3dt_format_from_pixelformat(0x40, 16, 0xF800, 0x7E0, 0x1F, 0) == D3DT_FMT_RGB565);
    CHECK(d3dt_format_bytes(D3DT_FMT_PAL8) == 1 && d3dt_format_bytes(D3DT_FMT_XRGB4444) == 2 && !d3dt_format_bytes(0));
    CHECK(!strcmp(d3dt_format_name(D3DT_FMT_PAL8), "PAL8") && !strcmp(d3dt_format_name(D3DT_FMT_XRGB4444), "XRGB4444"));
    CHECK(!d3dt_format_has_alpha(D3DT_FMT_XRGB4444) && !d3dt_format_has_alpha(D3DT_FMT_PAL8));
    {
        uint16_t x4444[1] = { 0x8F00 };
        uint8_t idx[3] = { 0, 1, 2 };
        uint32_t pal[256];
        memset(pal, 0, sizeof pal);
        pal[0] = 0xFF000000u;
        pal[1] = 0xFF102030u;
        pal[2] = 0xFFFFFFFFu;
        d3dt_convert_row(D3DT_FMT_XRGB4444, x4444, out, 1, 0, 0, 0);
        CHECK(out[0] == 0xFFFF0000u);
        d3dt_convert_row_pal8(idx, out, 3, pal, 1, 2, 2);
        CHECK(out[0] == 0xFF000000u && out[1] == 0xFF102030u && out[2] == 0x00FFFFFFu);
    }
}

static void test_overlay_and_rects(void) {
    uint16_t now[4] = { 1, 2, 3, 4 }, before[4] = { 1, 9, 3, 9 };
    uint32_t out[4];
    d3dt_rect_t r, acc = { 0, 0, 0, 0 }, add = { 5, 5, 10, 10 }, more = { 0, 8, 6, 20 };
    CHECK(d3dt_overlay_row(now, before, 4, out) == 2);
    CHECK(out[0] == 0 && (out[1] >> 24) == 0xFF && out[2] == 0 && (out[3] >> 24) == 0xFF);
    r = d3dt_scale_rect(0, 0, 800, 600, 800, 600, 1600, 1200);
    CHECK(r.left == 0 && r.top == 0 && r.right == 1600 && r.bottom == 1200);
    r = d3dt_scale_rect(10, 20, 30, 40, 640, 480, 1024, 768);
    CHECK(r.left == 16 && r.top == 32 && r.right == 64 && r.bottom == 96);
    r = d3dt_scale_rect(-5, -5, 2000, 2000, 640, 480, 1280, 960);
    CHECK(r.left == 0 && r.top == 0 && r.right == 1280 && r.bottom == 960);
    r = d3dt_scale_rect(700, 0, 10, 10, 640, 480, 1280, 960);
    CHECK(d3dt_rect_empty(&r));
    d3dt_rect_union(&acc, &add);
    d3dt_rect_union(&acc, &more);
    CHECK(acc.left == 0 && acc.top == 5 && acc.right == 10 && acc.bottom == 20);
    {
        uint16_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8] = { 1, 2, 9, 4, 5, 9, 7, 8 };
        unsigned first = 0, end = 0;
        CHECK(!d3dt_row_span(a, a, 8, &first, &end));
        CHECK(d3dt_row_span(a, b, 8, &first, &end) && first == 2 && end == 6);
        b[0] = 0;
        CHECK(d3dt_row_span(a, b, 8, &first, &end) && first == 0 && end == 6);
    }
}

static void test_resolution_and_msaa(void) {
    unsigned w = 1, h = 1;
    float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 }, b[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 },
          m[16];
    CHECK(d3dt_parse_resolution("auto", &w, &h) && w == 0 && h == 0);
    CHECK(d3dt_parse_resolution("1600x1200", &w, &h) && w == 1600 && h == 1200);
    CHECK(!d3dt_parse_resolution("1600x", &w, &h) && !d3dt_parse_resolution("100x100", &w, &h));
    CHECK(!d3dt_parse_resolution("1600x1200x", &w, &h));
    d3dt_auto_resolution(960, 720, 800, 600, &w, &h);
    CHECK(w == 1024 && h == 768);
    d3dt_auto_resolution(1440, 1080, 800, 600, &w, &h);
    CHECK(w == 1600 && h == 1200);
    d3dt_auto_resolution(400, 300, 800, 600, &w, &h);
    CHECK(w == 800 && h == 600);
    d3dt_auto_resolution(5000, 4000, 640, 480, &w, &h);
    CHECK(w == 2560 && h == 1920);
    CHECK(d3dt_pick_msaa(4, (1u << 2) | (1u << 4) | (1u << 8)) == 4);
    CHECK(d3dt_pick_msaa(8, (1u << 2) | (1u << 4)) == 4);
    CHECK(d3dt_pick_msaa(4, 0) == 1);
    CHECK(d3dt_pick_msaa(1, 1u << 4) == 1);
    d3dt_matrix_multiply(a, b, m);
    CHECK(m[0] == 2 && m[12] == 10 && m[13] == 12 && m[14] == 14 && m[15] == 1);
}

static void test_renderer_switch(void) {
    pending_switch_t p = { 0 };
    CHECK(renderer_switch_decide(0, 1, 0, 0, 0, 0) == SWITCH_DEFER);
    CHECK(renderer_switch_decide(0, 1, 2, 1, 1, 0) == SWITCH_DEFER);
    CHECK(renderer_switch_decide(0, 0, 0, 0, 0, 0) == SWITCH_NOW);
    CHECK(renderer_switch_decide(1, 1, 0, 0, 0, 0) == SWITCH_NOW);
    CHECK(renderer_switch_decide(1, 1, 1, 0, 0, 0) == SWITCH_WAIT);
    CHECK(renderer_switch_decide(1, 0, 0, 1, 0, 3) == SWITCH_WAIT);
    CHECK(renderer_switch_decide(1, 0, 0, 0, 1, RENDERER_SWITCH_ATTEMPTS - 1) == SWITCH_GIVE_UP);
    CHECK(!strcmp(renderer_switch_name(RENDERER_SWITCH_DEFERRED), "RENDERER_SWITCH_DEFERRED"));
    CHECK(!strcmp(renderer_switch_name(RENDERER_DRAINING), "RENDERER_DRAINING"));
    CHECK(pending_switch_peek(&p) == -1 && pending_switch_take(&p) == -1);
    pending_switch_set(&p, 0);
    CHECK(pending_switch_peek(&p) == 0 && pending_switch_take(&p) == 0 && pending_switch_peek(&p) == -1);
    pending_switch_set(&p, 1);
    pending_switch_cancel(&p);
    CHECK(pending_switch_take(&p) == -1);
}

/* The F5 lifecycle as the host drives it, on a model of the objects the census counts: a live switch
 * only at a boundary with nothing open, the old device gone before the new display, and Direct3D to
 * software inside a level waiting for the next DirectDraw creation. */
typedef struct {
    int hardware, scene, pending_applied;
    long locks, dcs, scenes, directdraws, devices, surfaces;
    pending_switch_t pending;
} f5_model_t;

static void model_rebuild(f5_model_t* m, int hardware) {
    m->hardware = hardware;
    m->surfaces = 3;
    m->devices = hardware ? 1 : 0;
}

static renderer_switch_t model_f5(f5_model_t* m, unsigned* boundaries) {
    unsigned attempts = 0;
    for (;;) {
        switch_decision_t d = renderer_switch_decide(!m->hardware, m->scene, m->locks, m->dcs, m->scenes, attempts);
        (*boundaries)++;
        if (d == SWITCH_DEFER) {
            pending_switch_set(&m->pending, !m->hardware);
            return RENDERER_SWITCH_DEFERRED;
        }
        if (d == SWITCH_GIVE_UP) return RENDERER_SWITCH_FAILED;
        if (d == SWITCH_NOW) {
            model_rebuild(m, !m->hardware);
            return RENDERER_ACTIVE_NEW;
        }
        attempts++;
        if (attempts == 2) m->locks = m->dcs = m->scenes = 0;
    }
}

static void model_directdraw_created(f5_model_t* m) {
    int to = pending_switch_take(&m->pending);
    m->directdraws++;
    if (to >= 0) {
        model_rebuild(m, to);
        m->pending_applied = 1;
    }
}

static void test_f5_lifecycle(void) {
    f5_model_t m;
    unsigned boundaries = 0;
    memset(&m, 0, sizeof m);
    m.hardware = 1, m.devices = 1, m.surfaces = 3, m.directdraws = 1;
    /* Menu: D3D11 -> Software drains an open lock first, then rebuilds with no device left. */
    m.locks = 1;
    CHECK(model_f5(&m, &boundaries) == RENDERER_ACTIVE_NEW && boundaries == 3);
    CHECK(!m.hardware && m.devices == 0 && m.locks == 0);
    /* Software -> D3D11 in a level is live and leaves exactly one device. */
    m.scene = 1;
    boundaries = 0;
    CHECK(model_f5(&m, &boundaries) == RENDERER_ACTIVE_NEW && m.hardware && m.devices == 1);
    /* D3D11 -> Software in a level waits; nothing changes until the game creates DirectDraw again. */
    boundaries = 0;
    CHECK(model_f5(&m, &boundaries) == RENDERER_SWITCH_DEFERRED && m.hardware && m.devices == 1);
    CHECK(pending_switch_peek(&m.pending) == 0);
    model_directdraw_created(&m);
    CHECK(m.pending_applied && !m.hardware && m.devices == 0 && pending_switch_peek(&m.pending) == -1);
    /* A later DirectDraw creation with nothing pending changes nothing. */
    m.pending_applied = 0;
    model_directdraw_created(&m);
    CHECK(!m.pending_applied && !m.hardware);
}

static void test_address_and_cull(void) {
    CHECK(d3dt_address(D3DTADDRESS_WRAP) == D3D11_TEXTURE_ADDRESS_WRAP);
    CHECK(d3dt_address(D3DTADDRESS_MIRROR) == D3D11_TEXTURE_ADDRESS_MIRROR);
    CHECK(d3dt_address(D3DTADDRESS_CLAMP) == D3D11_TEXTURE_ADDRESS_CLAMP);
    CHECK(d3dt_address(D3DTADDRESS_BORDER) == D3D11_TEXTURE_ADDRESS_BORDER);
    CHECK(d3dt_address(0) == 0 && d3dt_address(5) == 0);
    /* D3D11's default front face is clockwise in screen space, the space D3D6 TL vertices are in. */
    CHECK(d3dt_cull(D3DCULL_NONE) == D3D11_CULL_NONE);
    CHECK(d3dt_cull(D3DCULL_CW) == D3D11_CULL_FRONT);
    CHECK(d3dt_cull(D3DCULL_CCW) == D3D11_CULL_BACK);
    CHECK(d3dt_compare(D3DCMP_GREATEREQUAL) == D3D11_COMPARISON_GREATER_EQUAL);
    CHECK(d3dt_compare(D3DCMP_ALWAYS) == D3D11_COMPARISON_ALWAYS && d3dt_compare(D3DCMP_LESSEQUAL) == D3D11_COMPARISON_LESS_EQUAL);
    {
        uint16_t now[5] = { 0, 0, 0x1234, 0, 0 }, before[5] = { 0x07E0, 0, 0x07E0, 0x001F, 0 };
        CHECK(d3dt_black_over_content(now, before, 5) == 2);
    }
}

static void test_hardware_water(void) {
    CHECK(hardware_water_policy(1, 0, 0) == 1);
    CHECK(hardware_water_policy(0, 0, 0) == -1);
    CHECK(hardware_water_policy(1, 1, 0) == -1);
    CHECK(hardware_water_policy(1, 0, 1) == -1);
}

static void test_terrain_blend(void) {
    uint32_t sand = d3dt_rgb565_to_bgra(0x8410);
    CHECK(sand == 0xFF848284u);
    /* The terrain batch: RGB565 texture, TEXTUREMAPBLEND DECAL, white diffuse; DECAL ignores the vertex colour. */
    CHECK(d3dt_texture_blend(1, sand, 0xFFFFFFFFu, 0) == sand);
    CHECK(d3dt_texture_blend(1, sand, 0x00000000u, 0) == sand);
    CHECK(d3dt_texture_blend(7, sand, 0x00000000u, 0) == sand);
    CHECK(d3dt_texture_blend(2, sand, 0xFFFFFFFFu, 0) == sand);
    CHECK(d3dt_texture_blend(2, sand, 0xFF808080u, 0) == 0xFF424142u);
    CHECK(d3dt_texture_blend(2, sand, 0x00808080u, 0) == 0x00424142u);
    /* DECALMASK/MODULATEMASK: the low alpha bit picks the texture; alpha always from the vertex. */
    CHECK(d3dt_texture_blend(5, 0x01FF0000u, 0x80102030u, 1) == 0x80FF0000u);
    CHECK(d3dt_texture_blend(5, 0x02FF0000u, 0x80102030u, 1) == 0x80102030u);
    CHECK(d3dt_texture_blend(5, 0x00FF0000u, 0x80102030u, 0) == 0x80FF0000u);
    CHECK(d3dt_texture_blend(6, 0xFFFF8000u, 0x40808080u, 1) == 0x40804000u);
    CHECK(d3dt_texture_blend(6, 0xFEFF8000u, 0x40808080u, 1) == 0x40808080u);
    CHECK(d3dt_texture_blend(8, 0xFFF0F0F0u, 0x20404040u, 0) == 0x20FFFFFFu);
}

static void test_fog_and_layout(void) {
    unsigned stride, diffuse, specular, uv;
    CHECK(d3dt_apply_fog(0xFF102030u, 0xC0C0C0u, 255) == 0xFF102030u);
    CHECK(d3dt_apply_fog(0xFF102030u, 0xC0C0C0u, 0) == 0xFFC0C0C0u);
    CHECK(((d3dt_apply_fog(0xFF102030u, 0xC0C0C0u, 128) >> 16) & 0xFF) == 0x68);
    CHECK(d3dt_apply_fog(0x80000000u, 0x000000u, 0) == 0x80000000u);
    CHECK(D3DFVF_TLVERTEX == 0x1C4);
    CHECK(d3dt_fvf_layout(D3DFVF_TLVERTEX, &stride, &diffuse, &specular, &uv));
    CHECK(stride == sizeof(D3DTLVERTEX) && stride == 32);
    CHECK(diffuse == offsetof(D3DTLVERTEX, color) && specular == offsetof(D3DTLVERTEX, specular));
    CHECK(uv == offsetof(D3DTLVERTEX, tu) && uv + 4 == offsetof(D3DTLVERTEX, tv));
    CHECK(d3dt_fvf_layout(D3DFVF_XYZRHW | D3DFVF_TEX1, &stride, &diffuse, &specular, &uv) && stride == 24 && !diffuse &&
          uv == 16);
    CHECK(!d3dt_fvf_layout(D3DFVF_XYZ | D3DFVF_DIFFUSE, &stride, &diffuse, &specular, &uv));
    CHECK(!d3dt_fvf_layout(D3DFVF_XYZRHW | D3DFVF_NORMAL, &stride, &diffuse, &specular, &uv));
}

static void test_depth_and_cull(void) {
    float cw_x[3] = { 0, 10, 0 }, cw_y[3] = { 0, 0, 10 }, ccw_x[3] = { 0, 0, 10 }, ccw_y[3] = { 0, 10, 0 };
    float quad_x[4] = { 0, 10, 10, 0 }, quad_y[4] = { 0, 0, 10, 10 };
    float strip_x[4] = { 0, 0, 10, 10 }, strip_y[4] = { 0, 10, 0, 10 };
    uint32_t idx[6];
    /* z cleared to 0, GREATEREQUAL: larger sz (nearer) wins, the clear value never hides a draw. */
    CHECK(d3dt_depth_test(D3DCMP_GREATEREQUAL, 0.3f, 0.0f) && d3dt_depth_test(D3DCMP_GREATEREQUAL, 0.3f, 0.3f));
    CHECK(!d3dt_depth_test(D3DCMP_GREATEREQUAL, 0.2f, 0.3f));
    CHECK(d3dt_depth_test(D3DCMP_LESSEQUAL, 0.2f, 0.3f) && !d3dt_depth_test(D3DCMP_NEVER, 0.2f, 0.3f));
    CHECK(d3dt_depth_test(D3DCMP_ALWAYS, 0.0f, 1.0f));
    CHECK(d3dt_signed_area(cw_x, cw_y) > 0.0f && d3dt_signed_area(ccw_x, ccw_y) < 0.0f);
    CHECK(d3dt_triangle_culled(D3DCULL_CW, cw_x, cw_y) && !d3dt_triangle_culled(D3DCULL_CCW, cw_x, cw_y));
    CHECK(d3dt_triangle_culled(D3DCULL_CCW, ccw_x, ccw_y) && !d3dt_triangle_culled(D3DCULL_NONE, ccw_x, ccw_y));
    /* Fans keep the polygon's winding in every triangle; strips flip every other triangle back. */
    CHECK(d3dt_fan_indices(0, 4, idx) == 6);
    for (int t = 0; t < 2; t++) {
        float x[3] = { quad_x[idx[3 * t]], quad_x[idx[3 * t + 1]], quad_x[idx[3 * t + 2]] };
        float y[3] = { quad_y[idx[3 * t]], quad_y[idx[3 * t + 1]], quad_y[idx[3 * t + 2]] };
        CHECK(d3dt_signed_area(x, y) > 0.0f);
    }
    CHECK(d3dt_strip_indices(0, 4, idx) == 6);
    {
        float x0[3] = { strip_x[idx[0]], strip_x[idx[1]], strip_x[idx[2]] }, y0[3] = { strip_y[idx[0]], strip_y[idx[1]], strip_y[idx[2]] };
        float x1[3] = { strip_x[idx[3]], strip_x[idx[4]], strip_x[idx[5]] }, y1[3] = { strip_y[idx[3]], strip_y[idx[4]], strip_y[idx[5]] };
        CHECK((d3dt_signed_area(x0, y0) > 0.0f) == (d3dt_signed_area(x1, y1) > 0.0f));
    }
}

static void test_coverage_and_uv(void) {
    float x[3] = { 0, 100, 0 }, y[3] = { 0, 0, 100 }, w[3], u, v;
    float tu[3] = { 0, 1, 0 }, tv[3] = { 0, 0, 1 }, rhw[3] = { 1.0f, 0.25f, 1.0f }, flat[3] = { 1, 1, 1 };
    CHECK(d3dt_barycentric(x, y, 10, 10, w) && w[0] > 0.79f && w[0] < 0.81f);
    CHECK(!d3dt_barycentric(x, y, 90, 90, w));
    CHECK(d3dt_barycentric(x, y, 0, 0, w) && w[0] > 0.99f);
    d3dt_perspective_uv(tu, tv, rhw, w, &u, &v);
    CHECK(u < 0.01f && v < 0.01f);
    CHECK(d3dt_barycentric(x, y, 50, 0, w));
    d3dt_perspective_uv(tu, tv, flat, w, &u, &v);
    CHECK(u > 0.49f && u < 0.51f);
    /* A far vertex (small rhw) pulls less of the screen-space midpoint towards its texture coordinate. */
    d3dt_perspective_uv(tu, tv, rhw, w, &u, &v);
    CHECK(u > 0.19f && u < 0.21f);
    {
        volatile float zero = 0.0f, big = 3.0e38f;
        CHECK(d3dt_float_bad(zero / zero) && d3dt_float_bad(big * 10.0f) && !d3dt_float_bad(1.0f));
    }
}

static void test_terrain_state_leak(void) {
    d3dt_draw_state_t terrain, water, leaked;
    memset(&terrain, 0, sizeof terrain);
    terrain.textured = 1;
    terrain.tmb = D3DTBLEND_DECAL;
    terrain.fog = 1;
    terrain.fog_color = 0x8090A0;
    terrain.shade = D3DSHADE_GOURAUD;
    terrain.zenable = terrain.zwrite = 1;
    terrain.zfunc = D3DCMP_GREATEREQUAL;
    terrain.cull = D3DCULL_NONE;
    terrain.address_u = terrain.address_v = D3DTADDRESS_CLAMP;
    terrain.mag = terrain.min = 2;
    water = terrain;
    water.tmb = D3DTBLEND_MODULATE;
    water.blend_enable = 1;
    water.src_blend = D3DBLEND_SRCALPHA;
    water.dst_blend = D3DBLEND_INVSRCALPHA;
    leaked = terrain;
    leaked.blend_enable = water.blend_enable;
    CHECK(d3dt_state_fingerprint(&terrain) == d3dt_state_fingerprint(&terrain));
    CHECK(d3dt_state_fingerprint(&terrain) != d3dt_state_fingerprint(&water));
    CHECK(d3dt_state_fingerprint(&terrain) != d3dt_state_fingerprint(&leaked));
    leaked = terrain;
    leaked.alpha_test = 1;
    CHECK(d3dt_state_fingerprint(&terrain) != d3dt_state_fingerprint(&leaked));
    {
        char text[384];
        d3dt_describe_state(&terrain, text, sizeof text);
        CHECK(strstr(text, "tmb 1") && strstr(text, "blend off") && strstr(text, "fog on 0x8090A0"));
    }
}

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);

static void test_shaders(void) {
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dll ? (compile_fn)GetProcAddress(dll, "D3DCompile") : NULL;
    CHECK(compile != NULL);
    if (!compile) return;
    for (int i = 0; i < D3D11_SHADER_COUNT; i++) {
        ID3DBlob *code = NULL, *errors = NULL;
        HRESULT hr = compile(d3d11_shader_source, strlen(d3d11_shader_source), "trespasser_d3d6", NULL, NULL,
                             d3d11_shader_entries[i].entry, d3d11_shader_entries[i].profile, 0, 0, &code, &errors);
        if (FAILED(hr))
            printf("shader %s: %s\n", d3d11_shader_entries[i].entry,
                   errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors) : "?");
        CHECK(SUCCEEDED(hr));
        if (code) code->lpVtbl->Release(code);
        if (errors) errors->lpVtbl->Release(errors);
    }
}

/* The bump walk's closed form (what ps_bump_span computes) against the guest's pixel-by-pixel loop, the shader's
 * pixel against the reference, and the shader's 565 expansion against the composite's. The native routine itself
 * is compared in water_tests. */
static uint32_t s_bump_rng = 0x2545F491u;
static uint32_t bump_rnd(void) {
    s_bump_rng ^= s_bump_rng << 13;
    s_bump_rng ^= s_bump_rng >> 17;
    s_bump_rng ^= s_bump_rng << 5;
    return s_bump_rng;
}

static void test_bump_math(void) {
    static uint16_t clut[0x6000], texels[512 * 256];
    static uint8_t table[1024];
    unsigned long walk_differ = 0, pixel_differ = 0, expand_differ = 0, steps = 0;
    bump_pixel_t c;
    for (unsigned i = 0; i < sizeof clut / sizeof clut[0]; i++) clut[i] = (uint16_t)bump_rnd();
    for (unsigned i = 0; i < sizeof texels / sizeof texels[0]; i++) texels[i] = (uint16_t)bump_rnd();
    for (unsigned i = 0; i < sizeof table; i++) table[i] = (uint8_t)(bump_rnd() % 64u);
    c.clut = clut;
    c.bump = table;
    for (int n = 0; n < 4000; n++) {
        bump_walk_t d;
        bump_walk_state_t w, start;
        bump_span_t span;
        uint32_t wbits = 3 + bump_rnd() % 6, hbits = 3 + bump_rnd() % 6, tiled = bump_rnd() % 3 != 0;
        int32_t du = (int32_t)(bump_rnd() % 7) - 3, dv = (int32_t)(bump_rnd() % 5) - 2;
        uint32_t k_max = 1 + bump_rnd() % 2048;
        d.du_frac = bump_rnd() & 0xFFFF0000u;
        d.dv_frac = bump_rnd() & 0xFFFF0000u;
        d.line_pixels = tiled ? 512u : 512u;
        d.tile_mask = tiled ? ((1u << hbits) - 1) << 9 | ((1u << wbits) - 1) : 0xFFFFFFFFu;
        d.step = (tiled ? (du & (int32_t)((1u << wbits) - 1)) : du) + dv * 512;
        d.step_carry = d.step + 512;
        start.index = tiled ? bump_rnd() & d.tile_mask : bump_rnd();
        start.ufrac = bump_rnd();
        start.vfrac = bump_rnd();
        w = start;
        for (uint32_t k = 0; k < k_max; k++, steps++) {
            if (bump_walk_index(&start, &d, k) != w.index) walk_differ++;
            bump_walk_step(&w, &d);
        }
        if (!tiled) continue;
        c.clut_base = (bump_rnd() % 8u) << 11;
        c.light_theta = bump_rnd() & 0x3F0u;
        span.start = start;
        span.x0 = (int32_t)(bump_rnd() % 600);
        span.n = 64;
        w = start;
        for (int32_t x = span.x0; x < span.x0 + span.n; x++) {
            uint16_t ref;
            int transparent = (int)(bump_rnd() & 1u);
            int written = bump_texel_rgb565(&c, texels[w.index], transparent, &ref);
            uint32_t shader = bump_shader_pixel(&span, x, &d, texels, &c, transparent);
            if (written ? shader != ref : shader != 0x10000u) pixel_differ++;
            bump_walk_step(&w, &d);
        }
    }
    for (uint32_t v = 0; v < 0x10000u; v++) {
        uint32_t r = v >> 11, g = (v >> 5) & 63u, b = v & 31u;
        uint32_t shader = 0xFF000000u | ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
        if (shader != d3dt_rgb565_to_bgra((uint16_t)v)) expand_differ++;
    }
    printf("bump: %lu walk steps, closed form differs %lu; shader pixels differ %lu; 565 expansions differ %lu\n", steps,
           walk_differ, pixel_differ, expand_differ);
    CHECK(walk_differ == 0 && pixel_differ == 0 && expand_differ == 0 && steps > 1000000);
}

/* The batch's texture arrays: one slot per distinct array, the ninth refused; the layer as the HLSL decodes it. */
static void test_batch_slots(void) {
    const void* bound[D3D11_WORLD_ARRAYS];
    int items[D3D11_WORLD_ARRAYS + 1];
    unsigned count = 0;
    for (int i = 0; i < D3D11_WORLD_ARRAYS; i++) CHECK(d3dt_batch_slot(bound, &count, D3D11_WORLD_ARRAYS, &items[i]) == i);
    for (int i = D3D11_WORLD_ARRAYS - 1; i >= 0; i--)
        CHECK(d3dt_batch_slot(bound, &count, D3D11_WORLD_ARRAYS, &items[i]) == i);
    CHECK(d3dt_batch_slot(bound, &count, D3D11_WORLD_ARRAYS, &items[D3D11_WORLD_ARRAYS]) == -1);
    CHECK(count == D3D11_WORLD_ARRAYS);
    count = 0;
    CHECK(d3dt_batch_slot(bound, &count, D3D11_WORLD_ARRAYS, &items[D3D11_WORLD_ARRAYS]) == 0);
    for (uint32_t slice = 0; slice < 64; slice += 7)
        for (uint32_t slot = 0; slot < D3D11_WORLD_ARRAYS; slot++)
            for (uint32_t flags = 0; flags < 8; flags++)
                for (uint32_t sampler = 0; sampler <= D3D11_DEVICE_SAMPLER; sampler++) {
                    uint32_t layer = d3dt_world_layer(slice, slot, flags, sampler);
                    CHECK((layer & 0xFFFFu) == slice && ((layer >> 16) & 15u) == slot && ((layer >> 20) & 7u) == flags &&
                          ((layer >> 24) & 7u) == sampler);
                }
    CHECK(D3D11_PS_TEXTURED < 8 && D3D11_PS_TEX_ALPHA < 8 && D3D11_PS_KEY_TEST < 8);
    /* Wrap (1, or 0 unset) and clamp (3) per axis pick s0-s3 as samplers_for builds them; mirror (2) the device's. */
    CHECK(d3dt_vertex_sampler(0, 0) == 0 && d3dt_vertex_sampler(1, 1) == 0 && d3dt_vertex_sampler(3, 1) == 1 &&
          d3dt_vertex_sampler(1, 3) == 2 && d3dt_vertex_sampler(3, 3) == 3);
    CHECK(d3dt_vertex_sampler(2, 1) == D3D11_DEVICE_SAMPLER && d3dt_vertex_sampler(3, 2) == D3D11_DEVICE_SAMPLER);
    CHECK(d3dt_address_joins(1, 3) && d3dt_address_joins(3, 1) && d3dt_address_joins(0, 3));
    CHECK(!d3dt_address_joins(1, 2) && !d3dt_address_joins(2, 3));
}

/* A Load copies only the rows that differ and reports their box; the result equals a full copy. */
static void test_copy_changed(void) {
    static uint8_t dst[256 * 1040], src[256 * 1040], full[256 * 1040];
    unsigned long differ = 0, boxes_wrong = 0;
    for (int n = 0; n < 400; n++) {
        unsigned bytes = n % 3 == 0 ? 1 : n % 3 == 1 ? 2 : 4, w = 8u << (bump_rnd() % 6), h = 8u << (bump_rnd() % 6);
        long pitch = (long)(w * bytes + (bump_rnd() % 4) * 4);
        d3dt_rect_t box, want = { (long)w, -1, 0, 0 };
        int changed;
        for (size_t i = 0; i < sizeof dst; i++) dst[i] = src[i] = (uint8_t)bump_rnd();
        for (unsigned y = 0; y < h; y++) memcpy(src + (size_t)y * pitch, dst + (size_t)y * pitch, (size_t)w * bytes);
        for (int k = (int)(bump_rnd() % 6); k > 0; k--) {
            unsigned x = bump_rnd() % w, y = bump_rnd() % h, run = 1 + bump_rnd() % 4;
            for (unsigned i = 0; i < run && x + i < w; i++) {
                src[(size_t)y * pitch + (x + i) * bytes] = (uint8_t)(dst[(size_t)y * pitch + (x + i) * bytes] + 1);
                if ((long)(x + i) < want.left) want.left = (long)(x + i);
                if ((long)(x + i) + 1 > want.right) want.right = (long)(x + i) + 1;
            }
            if (want.top < 0 || (long)y < want.top) want.top = (long)y;
            if ((long)y + 1 > want.bottom) want.bottom = (long)y + 1;
        }
        memcpy(full, dst, sizeof full);
        for (unsigned y = 0; y < h; y++) memcpy(full + (size_t)y * pitch, src + (size_t)y * pitch, (size_t)w * bytes);
        changed = d3dt_copy_changed(dst, pitch, src, pitch, w, h, bytes, &box);
        if (memcmp(dst, full, sizeof full)) differ++;
        if (changed != (want.top >= 0) ||
            (changed && (box.left != want.left || box.top != want.top || box.right != want.right || box.bottom != want.bottom)))
            boxes_wrong++;
    }
    CHECK(differ == 0 && boxes_wrong == 0);
}

static void test_bump_shaders(void) {
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dll ? (compile_fn)GetProcAddress(dll, "D3DCompile") : NULL;
    CHECK(compile != NULL);
    if (!compile) return;
    for (int i = 0; i < 3; i++) {
        ID3DBlob *code = NULL, *errors = NULL;
        HRESULT hr = compile(d3d11_bump_shader_source, strlen(d3d11_bump_shader_source), "trespasser_bump", NULL, NULL,
                             d3d11_bump_shader_entries[i].entry, d3d11_bump_shader_entries[i].profile, 0, 0, &code,
                             &errors);
        if (FAILED(hr))
            printf("shader %s: %s\n", d3d11_bump_shader_entries[i].entry,
                   errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors) : "?");
        CHECK(SUCCEEDED(hr));
        if (code) code->lpVtbl->Release(code);
        if (errors) errors->lpVtbl->Release(errors);
    }
}

static jpt_size_t box_of(const char* preset, int aspect) {
    return jpt_render_box(jpt_render_preset_index(preset), aspect, 0, 0);
}

static int size_is(jpt_size_t s, int w, int h) { return s.width == w && s.height == h; }

#define ASPECT_32_9 4

static void test_native_gameplay_resolution(void) {
    static const struct {
        const char* preset;
        int aspect, w, h;
    } k[] = {
        { "1080p", JPT_ASPECT_16_9, 1920, 1080 }, { "1440p", JPT_ASPECT_16_9, 2560, 1440 },
        { "4k", JPT_ASPECT_16_9, 3840, 2160 },    { "1440p", JPT_ASPECT_21_9, 3440, 1440 },
        { "1440p", ASPECT_32_9, 5120, 1440 },     { "1440p", JPT_ASPECT_4_3, 2560, 1920 },
        { "1080p", JPT_ASPECT_4_3, 1920, 1440 },  { "720p", JPT_ASPECT_16_9, 1280, 720 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        jpt_size_t box = box_of(k[i].preset, k[i].aspect);
        jpt_size_t game = jpt_gameplay_mode(box, k[i].aspect, 1);
        jpt_size_t target = jpt_render_target(box, game.width, game.height);
        d3dt_rect_t vp = d3dt_scale_rect(0, 0, game.width, game.height, (unsigned)game.width, (unsigned)game.height,
                                         (unsigned)target.width, (unsigned)target.height);
        d3dt_rect_t mid = d3dt_scale_rect(game.width / 2, game.height / 2, 1, 1, (unsigned)game.width,
                                          (unsigned)game.height, (unsigned)target.width, (unsigned)target.height);
        CHECK(size_is(box, k[i].w, k[i].h));
        CHECK(size_is(game, k[i].w, k[i].h));   /* the game's own screen is the chosen resolution */
        CHECK(size_is(target, k[i].w, k[i].h)); /* and D3D11 rasterises it 1:1 */
        CHECK(vp.left == 0 && vp.top == 0 && vp.right == game.width && vp.bottom == game.height);
        CHECK(mid.left == game.width / 2 && mid.right == mid.left + 1 && mid.top == game.height / 2);
        CHECK(size_is(jpt_title_size(box, game, 1), k[i].w, k[i].h));
        CHECK(game.width < 32768 && game.height < 32768); /* 16.16 span x in the software rasteriser */
        CHECK(game.width % 2 == 0 && (long long)game.width * game.height * 4 < 0x7FFFFFFFll);
        CHECK(jpt_gameplay_host_bytes(game) <= 120ull * 1048576);
        CHECK(fabs(jpt_hor_plus_scale(game.width, game.height) - (double)game.width / game.height / (4.0 / 3.0)) < 1e-9);
    }
    CHECK(fabs(jpt_hor_plus_scale(2560, 1920) - 1.0) < 1e-9); /* 4:3 keeps the original view */
    {
        /* Above the budget: the largest mode of the shape within it, rasterised and shown at the box. */
        jpt_size_t box = box_of("4k", JPT_ASPECT_21_9), game = jpt_gameplay_mode(box, JPT_ASPECT_21_9, 1);
        jpt_size_t big = box_of("4k", ASPECT_32_9), wide = jpt_gameplay_mode(big, ASPECT_32_9, 1);
        CHECK(size_is(box, 5120, 2160));
        CHECK((long long)game.width * game.height <= JPT_GAMEPLAY_MAX_PIXELS);
        CHECK(game.width % 8 == 0 && game.height % 8 == 0);
        CHECK(game.width > 4000 && fabs((double)game.width / game.height - 5120.0 / 2160.0) < 0.02);
        CHECK(size_is(jpt_render_target(box, game.width, game.height), 5120, 2160));
        CHECK(size_is(jpt_title_size(box, game, 1), 5120, 2160));
        CHECK(size_is(big, 7680, 2160) && (long long)wide.width * wide.height <= JPT_GAMEPLAY_MAX_PIXELS);
        CHECK(jpt_gameplay_host_bytes(wide) <= 120ull * 1048576);
    }
    {
        /* Software and Original keep the compatibility modes, and the title names them. */
        jpt_size_t box = box_of("1440p", JPT_ASPECT_16_9), none = { 0, 0 };
        CHECK(size_is(jpt_gameplay_mode(box, JPT_ASPECT_16_9, 0), 1024, 576));
        CHECK(size_is(jpt_gameplay_mode(box, JPT_ASPECT_4_3, 0), 800, 600));
        CHECK(size_is(jpt_gameplay_mode(none, JPT_ASPECT_16_9, 1), 1024, 576));
        CHECK(size_is(jpt_title_size(box, jpt_gameplay_mode(box, JPT_ASPECT_16_9, 0), 0), 1024, 576));
        CHECK(size_is(jpt_title_size(none, jpt_gameplay_mode(none, JPT_ASPECT_16_9, 1), 1), 1024, 576));
    }
}

static void test_menus_videos_and_text(void) {
    jpt_size_t box = box_of("1440p", JPT_ASPECT_16_9);
    jpt_size_t menus = jpt_render_target(box, 640, 480);
    jpt_rect_t frame = jpt_present_rect(2560, 1440, 640, 480, 0);
    jpt_rect_t ultra = jpt_present_rect(3440, 1440, 640, 480, 0);
    jpt_rect_t shrunk = jpt_present_rect(960, 540, 2560, 1440, 0);
    CHECK(size_is(menus, 1920, 1440)); /* menus and 640x480 videos: 4:3 at an integer 3x */
    CHECK(frame.left == 320 && frame.right == 2240 && frame.top == 0 && frame.bottom == 1440);
    CHECK(ultra.left == 760 && ultra.right == 2680 && ultra.top == 0 && ultra.bottom == 1440);
    CHECK(shrunk.left == 0 && shrunk.right == 960 && shrunk.top == 0 && shrunk.bottom == 540);
    for (unsigned k = 0; k < 1440; k++) CHECK(!d3dt_row_on_texel_edge(k, 480, 1440));
    CHECK(jpt_ui_text_height(32, 576) == 32); /* compatibility modes keep the game's own size */
    CHECK(jpt_ui_text_height(32, 600) == 32);
    CHECK(jpt_ui_text_height(32, 1080) == 58);
    CHECK(jpt_ui_text_height(32, 1440) == 77);
    CHECK(jpt_ui_text_height(32, 2160) == 115);
    CHECK(jpt_ui_text_height(24, 1440) == 58);
    for (int h = 600; h <= 2160; h += 120) CHECK(fabs(jpt_ui_text_height(32, h) / (double)h - 32.0 / 600.0) < 0.001);
}

/* The texel a point sample of target row k reads, taking the lower neighbour when the pixel centre is exactly on a
 * texel edge (rounding the GPU may apply). */
static long sampled_row(unsigned k, unsigned legacy, unsigned target) {
    long texel = (long)(((2ull * k + 1) * legacy) / (2ull * target));
    return d3dt_row_on_texel_edge(k, legacy, target) ? texel - 1 : texel;
}

/* Target rows whose centres lie in [top, bottom) scaled: the rows a box's quad covers (top-left rule). */
static void covered_rows(long top, long bottom, unsigned legacy, unsigned target, unsigned* first, unsigned* end) {
    unsigned k = 0;
    while ((2ull * k + 1) * legacy < 2ull * (unsigned long long)top * target) k++;
    *first = k;
    while ((2ull * k + 1) * legacy < 2ull * (unsigned long long)bottom * target) k++;
    *end = k;
}

static void test_overlay_texel_edge(void) {
    /* The tutorial text's line: 1024x576 composited onto 2560x1440 (2.5x). A box starting on an odd row has its first
     * target row exactly on a texel edge, so the quad's top row may read the row above the box. */
    const unsigned legacy = 576, target = 1440;
    d3dt_rect_t box = { 100, 11, 400, 43 }, up;
    unsigned first, end, above = 0, outside_upload = 0;
    covered_rows(box.top, box.bottom, legacy, target, &first, &end);
    CHECK(first == 27 && d3dt_row_on_texel_edge(first, legacy, target));
    for (unsigned k = first; k < end; k++) above += sampled_row(k, legacy, target) < box.top;
    CHECK(above == 1); /* one target row from texel top-1, which a box-only upload leaves stale */
    up = d3dt_overlay_upload_rect(&box, 1024, 576);
    CHECK(up.left == 99 && up.top == 10 && up.right == 401 && up.bottom == 44);
    for (unsigned k = first; k < end; k++) {
        long t = sampled_row(k, legacy, target);
        outside_upload += t < up.top || t >= up.bottom;
    }
    CHECK(outside_upload == 0);
    {
        static uint8_t copy[SIM_PITCH * SIM_H];
        static uint16_t reference[SIM_W * SIM_H];
        static uint32_t texels[SIM_W * SIM_H];
        d3dt_rect_t changed, apron;
        tsync_counts_t counts = { 0, 0 };
        tsync_fill(copy, SIM_PITCH, reference, SIM_W, &k_sim_all, 0x4208);
        *sim_px(copy, 6, 5) = 0xFFFF;
        *sim_px(copy, 9, 7) = 0;
        CHECK(tsync_changed_box(copy, SIM_PITCH, reference, SIM_W, &k_sim_all, &changed));
        apron = d3dt_overlay_upload_rect(&changed, SIM_W, SIM_H);
        CHECK(apron.top == changed.top - 1 && apron.left == changed.left - 1);
        tsync_overlay_box(copy, SIM_PITCH, reference, SIM_W, &apron, texels, &counts);
        CHECK(counts.changed == 2);
        for (long x = 0; x < apron.right - apron.left; x++) CHECK(texels[x] == 0); /* the apron row is transparent */
    }
    {
        d3dt_rect_t whole = { 0, 0, 1024, 576 }, all = d3dt_overlay_upload_rect(&whole, 1024, 576);
        CHECK(all.left == 0 && all.top == 0 && all.right == 1024 && all.bottom == 576);
    }
    for (unsigned k = 0; k < 1440; k++) CHECK(!d3dt_row_on_texel_edge(k, 1440, 1440)); /* native: 1:1 */
}

static int rect_is(jpt_rect_t r, int l, int t, int rt, int b) { return r.left == l && r.top == t && r.right == rt && r.bottom == b; }

static void test_ui_canvas(void) {
    static const struct {
        int frame_w, frame_h, left, top, right, bottom;
        double scale;
    } k[] = {
        { 2560, 1440, 320, 0, 2240, 1440, 3.0 },   { 1920, 1080, 240, 0, 1680, 1080, 2.25 },
        { 3840, 2160, 480, 0, 3360, 2160, 4.5 },   { 3440, 1440, 760, 0, 2680, 1440, 3.0 },
        { 5120, 1440, 1600, 0, 3520, 1440, 3.0 },  { 2560, 1920, 0, 0, 2560, 1920, 4.0 },
        { 1280, 720, 160, 0, 1120, 720, 1.5 },     { 640, 480, 0, 0, 640, 480, 1.0 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        jpt_ui_canvas_t c = jpt_ui_canvas(k[i].frame_w, k[i].frame_h);
        int x, y, fx, fy, inside;
        CHECK(rect_is(c.rect, k[i].left, k[i].top, k[i].right, k[i].bottom));
        CHECK(fabs(c.scale - k[i].scale) < 1e-9);
        CHECK((c.rect.right - c.rect.left) * 3 == (c.rect.bottom - c.rect.top) * 4); /* 4:3, never stretched */
        CHECK(c.rect.left - 0 == k[i].frame_w - c.rect.right && c.rect.top == k[i].frame_h - c.rect.bottom);
        /* Mouse: corners and centre of the canvas rectangle. */
        inside = jpt_ui_to_canvas(&c, c.rect.left, c.rect.top, &x, &y);
        CHECK(inside && x == 0 && y == 0);
        inside = jpt_ui_to_canvas(&c, (c.rect.left + c.rect.right) / 2, (c.rect.top + c.rect.bottom) / 2, &x, &y);
        CHECK(inside && x == 320 && y == 240);
        inside = jpt_ui_to_canvas(&c, c.rect.right - 1, c.rect.bottom - 1, &x, &y);
        CHECK(inside && x == 639 && y == 479);
        inside = jpt_ui_to_canvas(&c, c.rect.right, c.rect.bottom, &x, &y);
        CHECK(!inside && x == 639 && y == 479);
        inside = jpt_ui_to_canvas(&c, c.rect.left - 1, c.rect.top - 1, &x, &y);
        CHECK(!inside && x == 0 && y == 0);
        /* Every canvas pixel maps to the frame and back to itself. */
        for (int u = 0; u < JPT_UI_CANVAS_W; u += 7) {
            for (int v = 0; v < JPT_UI_CANVAS_H; v += 5) {
                jpt_ui_to_frame(&c, u, v, &fx, &fy);
                CHECK(jpt_ui_to_canvas(&c, fx, fy, &x, &y) && x == u && y == v);
            }
        }
    }
    {
        /* 2560x1440 at 3x: pixel art keeps whole pixels, point sampling reads texel floor((x - left) / 3). */
        jpt_ui_canvas_t c = jpt_ui_canvas(2560, 1440);
        int x, y;
        for (int px = 0; px < 12; px++) {
            jpt_ui_to_canvas(&c, c.rect.left + px, 0, &x, &y);
            CHECK(x == px / 3);
        }
        jpt_ui_to_canvas(&c, c.rect.left + 959, 719, &x, &y);
        CHECK(x == 319 && y == 239);
    }
    {
        /* A 320x200 dialog centred in the canvas (160,140 - 480,340), and its clip. */
        jpt_ui_canvas_t c = jpt_ui_canvas(2560, 1440), s = jpt_ui_canvas(1920, 1080);
        jpt_rect_t dialog = { 160, 140, 480, 340 }, clip = { 100, 50, 201, 101 };
        CHECK(rect_is(jpt_ui_rect_to_frame(&c, dialog), 800, 420, 1760, 1020));
        CHECK(rect_is(jpt_ui_rect_to_frame(&c, clip), 620, 150, 923, 303));
        /* 2.25x: a clip never loses a pixel (floor left/top, ceil right/bottom). */
        clip.left = 3; clip.top = 3; clip.right = 6; clip.bottom = 6;
        CHECK(rect_is(jpt_ui_rect_to_frame(&s, clip), 240 + 6, 6, 240 + 14, 14));
        CHECK(rect_is(jpt_ui_rect_to_frame(&c, (jpt_rect_t){ 0, 0, 640, 480 }), 320, 0, 2240, 1440));
    }
    {
        float ndc[4];
        jpt_ui_canvas_t c = jpt_ui_canvas(2560, 1440), w = jpt_ui_canvas(3440, 1440);
        jpt_ui_canvas_ndc(&c, 2560, 1440, ndc);
        CHECK(fabs(ndc[0] + 0.75f) < 1e-6 && fabs(ndc[1] - 1.0f) < 1e-6 && fabs(ndc[2] - 0.75f) < 1e-6 &&
              fabs(ndc[3] + 1.0f) < 1e-6);
        jpt_ui_canvas_ndc(&w, 3440, 1440, ndc);
        CHECK(fabs(ndc[0] + 1920.0f / 3440.0f) < 1e-5 && fabs(ndc[2] - 1920.0f / 3440.0f) < 1e-5);
    }
    {
        /* Windowed: the canvas is composed in the render frame (2560x1440) and the whole frame is shown reduced; a
         * click in a 1753x986 client lands where the same point of the frame is. */
        jpt_ui_canvas_t c = jpt_ui_canvas(2560, 1440);
        jpt_rect_t view = jpt_present_rect(1920, 986, 2560, 1440, 0);
        int fx = (int)(((long long)(960 - view.left) * 2560) / (view.right - view.left));
        int fy = (int)(((long long)(493 - view.top) * 1440) / (view.bottom - view.top)), x, y;
        CHECK(view.right - view.left == 1753 && view.bottom - view.top == 986);
        CHECK(jpt_ui_to_canvas(&c, fx, fy, &x, &y) && abs(x - 320) <= 1 && abs(y - 240) <= 1);
        CHECK(rect_is(jpt_ui_canvas(2560, 1440).rect, 320, 0, 2240, 1440)); /* independent of the window */
    }
    {
        jpt_ui_canvas_t none = jpt_ui_canvas(0, 0), tiny = jpt_ui_canvas(320, 240);
        int x = 7, y = 7;
        CHECK(rect_is(none.rect, 0, 0, 0, 0) && !jpt_ui_to_canvas(&none, 5, 5, &x, &y));
        CHECK(rect_is(tiny.rect, 0, 0, 320, 240) && fabs(tiny.scale - 0.5) < 1e-9);
    }
}

static uint32_t texel(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

static double coverage(const uint32_t* t, size_t n) {
    size_t on = 0;
    for (size_t i = 0; i < n; i++) on += (t[i] >> 24) >= 128;
    return (double)on / (double)n;
}

static void test_mip_chain(void) {
    unsigned w, h;
    CHECK(d3dt_mip_count(256, 256) == 9);
    CHECK(d3dt_mip_count(256, 64) == 9);
    CHECK(d3dt_mip_count(1, 1) == 1);
    CHECK(d3dt_mip_count(0, 4) == 0);
    d3dt_mip_size(256, 64, 7, &w, &h);
    CHECK(w == 2 && h == 1);
    d3dt_mip_size(256, 64, 8, &w, &h);
    CHECK(w == 1 && h == 1);
    CHECK(d3dt_mip_chain_texels(4, 4, 3) == 16 + 4 + 1);
    CHECK(d3dt_mip_chain_texels(8, 2, 4) == 16 + 4 + 2 + 1);
}

static void test_mip_alpha(void) {
    uint32_t opaque[4] = { texel(255, 0, 0, 0), texel(255, 100, 0, 0), texel(255, 0, 200, 0), texel(255, 0, 0, 255) };
    uint32_t out[16], edge[16], half[16], foliage[64 + 16 + 4 + 1], blended[4];
    CHECK(d3dt_alpha_kind(opaque, 4) == D3DT_ALPHA_OPAQUE);
    d3dt_mip_downsample(opaque, 2, 2, D3DT_ALPHA_OPAQUE, out);
    CHECK(out[0] == texel(255, 25, 50, 64));

    /* A colour-keyed or 1555 edge: transparent black texels never darken the opaque colour. */
    edge[0] = texel(0, 0, 0, 0);
    edge[1] = texel(255, 200, 40, 20);
    edge[2] = texel(0, 0, 0, 0);
    edge[3] = texel(255, 200, 40, 20);
    CHECK(d3dt_alpha_kind(edge, 4) == D3DT_ALPHA_BINARY);
    d3dt_mip_downsample(edge, 2, 2, D3DT_ALPHA_BINARY, out);
    CHECK((out[0] & 0xFFFFFF) == 0xC82814 && ((out[0] >> 24) == 0 || (out[0] >> 24) == 255));

    /* Binary alpha stays binary and keeps its coverage. */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) half[y * 4 + x] = x < 2 ? texel(255, 10, 120, 10) : texel(0, 0, 0, 0);
    d3dt_mip_downsample(half, 4, 4, D3DT_ALPHA_BINARY, out);
    CHECK(out[0] >> 24 == 255 && out[2] >> 24 == 255 && out[1] >> 24 == 0 && out[3] >> 24 == 0);

    /* Sparse foliage (a quarter covered, scattered) keeps its density down the chain instead of vanishing. */
    for (int i = 0; i < 64; i++) foliage[i] = (i * 37 + (i / 8) * 11) % 4 == 0 ? texel(255, 30, 90, 20) : texel(0, 0, 0, 0);
    CHECK(d3dt_alpha_kind(foliage, 64) == D3DT_ALPHA_BINARY);
    d3dt_build_mips(foliage, 8, 8, 4, D3DT_ALPHA_BINARY);
    CHECK(fabs(coverage(foliage + 64, 16) - coverage(foliage, 64)) <= 0.125);
    CHECK(coverage(foliage + 80, 4) > 0.0);
    for (int i = 64; i < 85; i++) CHECK((foliage[i] >> 24) == 0 || (foliage[i] >> 24) == 255);
    /* One opaque texel in every footprint: a threshold would keep all or none; a quarter stays. */
    for (int i = 0; i < 64; i++) foliage[i] = (i % 2 == 0 && (i / 8) % 2 == 0) ? texel(255, 30, 90, 20) : texel(0, 0, 0, 0);
    d3dt_build_mips(foliage, 8, 8, 4, D3DT_ALPHA_BINARY);
    CHECK(coverage(foliage, 64) == 0.25 && coverage(foliage + 64, 16) == 0.25);

    /* 4444 translucency: alpha averaged, colour weighted by it. */
    blended[0] = texel(0x88, 0xFF, 0, 0);
    blended[1] = texel(0x88, 0xFF, 0, 0);
    blended[2] = texel(0, 0, 0, 0xFF);
    blended[3] = texel(0, 0, 0, 0xFF);
    CHECK(d3dt_alpha_kind(blended, 4) == D3DT_ALPHA_BLENDED);
    d3dt_mip_downsample(blended, 2, 2, D3DT_ALPHA_BLENDED, out);
    CHECK(out[0] == texel(0x44, 0xFF, 0, 0));

    /* A one-texel-wide level halves along the other side only. */
    d3dt_mip_downsample(opaque, 1, 4, D3DT_ALPHA_OPAQUE, out);
    CHECK(out[0] == texel(255, 50, 0, 0) && out[1] == texel(255, 0, 100, 128));
}

/* The coverage-keeping selection as it was first written (one scan per dither rank): the faster one must pick the same
 * texels on every binary-alpha level. */
static void reference_keep_coverage(uint32_t* dst, unsigned dw, unsigned dh, unsigned n, const unsigned long* hist,
                                    unsigned long target) {
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    unsigned long remaining = target;
    for (unsigned k = n; k >= 1; k--) {
        unsigned long take = hist[k] < remaining ? hist[k] : remaining;
        int all = take == hist[k];
        remaining -= take;
        for (unsigned rank = 0; rank < (all ? 1u : 16u) && take; rank++)
            for (unsigned y = 0; y < dh && take; y++)
                for (unsigned x = 0; x < dw && take; x++) {
                    uint32_t* t = &dst[(size_t)y * dw + x];
                    if ((*t >> 24) != k || (!all && bayer[(y & 3) * 4 + (x & 3)] != rank)) continue;
                    *t |= 0xFF000000u;
                    take--;
                }
        if (!remaining) break;
    }
    for (size_t i = 0; i < (size_t)dw * dh; i++)
        if ((dst[i] >> 24) != 0xFF) dst[i] &= 0x00FFFFFFu;
}

static void reference_binary_downsample(const uint32_t* src, unsigned w, unsigned h, uint32_t* dst) {
    unsigned dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, fx = w > 1 ? 2 : 1, fy = h > 1 ? 2 : 1, n = fx * fy;
    unsigned long hist[5] = { 0, 0, 0, 0, 0 }, covered = 0;
    for (unsigned y = 0; y < dh; y++)
        for (unsigned x = 0; x < dw; x++) {
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0, ur = 0, ug = 0, ub = 0, opaque = 0, r, g, b;
            for (unsigned j = 0; j < fy; j++)
                for (unsigned i = 0; i < fx; i++) {
                    uint32_t t = src[(size_t)(y * fy + j) * w + x * fx + i], a = t >> 24;
                    sa += a, sr += ((t >> 16) & 0xFF) * a, sg += ((t >> 8) & 0xFF) * a, sb += (t & 0xFF) * a;
                    ur += (t >> 16) & 0xFF, ug += (t >> 8) & 0xFF, ub += t & 0xFF;
                    opaque += a >= 128;
                }
            if (sa) r = (sr + sa / 2) / sa, g = (sg + sa / 2) / sa, b = (sb + sa / 2) / sa;
            else r = (ur + n / 2) / n, g = (ug + n / 2) / n, b = (ub + n / 2) / n;
            dst[(size_t)y * dw + x] = opaque << 24 | r << 16 | g << 8 | b;
            hist[opaque]++;
        }
    for (unsigned y = 0; y < dh * fy; y++)
        for (unsigned x = 0; x < dw * fx; x++) covered += (src[(size_t)y * w + x] >> 24) >= 128;
    reference_keep_coverage(dst, dw, dh, n, hist, (unsigned long)(((double)covered / n) + 0.5));
}

static void test_coverage_selection(void) {
    static uint32_t src[256 * 256], fast[128 * 128], slow[128 * 128];
    static const unsigned sizes[][2] = { { 256, 256 }, { 128, 64 }, { 64, 128 }, { 8, 8 }, { 1, 32 }, { 32, 1 }, { 2, 2 } };
    uint32_t seed = 0x1234567u;
    unsigned long mismatches = 0, levels = 0;
    for (int round = 0; round < 40; round++)
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            unsigned w = sizes[s][0], h = sizes[s][1], density = 1 + round % 7;
            unsigned dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
            for (unsigned i = 0; i < w * h; i++) {
                seed = seed * 1664525u + 1013904223u;
                src[i] = ((seed >> 28) < density ? 0xFF000000u : 0u) | ((seed >> 4) & 0xFFFFFFu);
            }
            d3dt_mip_downsample(src, w, h, D3DT_ALPHA_BINARY, fast);
            reference_binary_downsample(src, w, h, slow);
            mismatches += memcmp(fast, slow, (size_t)dw * dh * 4) != 0;
            levels++;
        }
    CHECK(levels == 280 && mismatches == 0);
}

static void test_dynamic_textures(void) {
    d3dt_upload_history_t water, terrain, burst;
    memset(&water, 0, sizeof water);
    memset(&terrain, 0, sizeof terrain);
    memset(&burst, 0, sizeof burst);
    CHECK(!d3dt_note_upload(&water, 10));
    CHECK(!d3dt_note_upload(&water, 11) && !d3dt_note_upload(&water, 12));
    CHECK(d3dt_note_upload(&water, 13) && water.dynamic);
    CHECK(!d3dt_note_upload(&water, 14));
    for (unsigned long long f = 0; f < 1000; f += 90) CHECK(!d3dt_note_upload(&terrain, f));
    CHECK(!terrain.dynamic);
    for (int i = 0; i < 20; i++) CHECK(!d3dt_note_upload(&burst, 5));
    CHECK(!burst.dynamic);
}

static void test_sampler_choice(void) {
    d3dt_sampler_choice_t c = d3dt_choose_sampler(D3DT_FILTER_ORIGINAL, 1, 1, 16);
    CHECK(c.filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT && !c.mips && c.anisotropy == 1);
    c = d3dt_choose_sampler(D3DT_FILTER_ORIGINAL, 0, 0, 16);
    CHECK(c.filter == D3D11_FILTER_MIN_MAG_MIP_POINT && !c.mips);
    c = d3dt_choose_sampler(D3DT_FILTER_ANISOTROPIC, 1, 1, 16);
    CHECK(c.filter == D3D11_FILTER_ANISOTROPIC && c.anisotropy == 16 && c.mips);
    c = d3dt_choose_sampler(D3DT_FILTER_ANISOTROPIC, 0, 0, 16);
    CHECK(c.filter == D3D11_FILTER_MIN_MAG_MIP_POINT && !c.mips);
    c = d3dt_choose_sampler(D3DT_FILTER_ANISOTROPIC, 1, 0, 16);
    CHECK(c.filter == D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT && !c.mips);
    c = d3dt_choose_sampler(D3DT_FILTER_BILINEAR, 0, 0, 1);
    CHECK(c.filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR && c.mips);
    c = d3dt_choose_sampler(D3DT_FILTER_POINT, 1, 1, 16);
    CHECK(c.filter == D3D11_FILTER_MIN_MAG_MIP_POINT && !c.mips);
    CHECK(!d3dt_filter_uses_mips(D3DT_FILTER_ORIGINAL) && !d3dt_filter_uses_mips(D3DT_FILTER_POINT));
    CHECK(d3dt_filter_uses_mips(D3DT_FILTER_BILINEAR) && d3dt_filter_uses_mips(D3DT_FILTER_ANISOTROPIC));
    CHECK(d3dt_pick_anisotropy(16, 16) == 16 && d3dt_pick_anisotropy(16, 8) == 8 && d3dt_pick_anisotropy(16, 5) == 4);
    CHECK(d3dt_pick_anisotropy(16, 2) == 2 && d3dt_pick_anisotropy(16, 1) == 1 && d3dt_pick_anisotropy(3, 16) == 2);
    CHECK(d3dt_pick_msaa(4, (1u << 2) | (1u << 8)) == 2);
    CHECK(d3dt_pick_msaa(4, 1u << 3) == 1);
    CHECK(d3dt_pick_msaa(4, (1u << 2) | (1u << 4)) == 4);
}

static void test_readback_regions(void) {
    float quad[4][8] = { { 16.0f, 16.0f }, { 144.0f, 16.0f }, { 144.0f, 144.0f }, { 16.0f, 144.0f } };
    float bad[2][8] = { { 10.0f, 10.0f }, { NAN, 4.0f } };
    d3dt_rect_t drawn = d3dt_vertex_box(&quad[0][0], sizeof quad[0], 4, 640, 480), r;
    d3dt_rect_t none = { 0, 0, 0, 0 };
    CHECK(drawn.left == 15 && drawn.top == 15 && drawn.right == 145 && drawn.bottom == 145);
    r = d3dt_vertex_box(&bad[0][0], sizeof bad[0], 2, 640, 480);
    CHECK(r.left == 0 && r.top == 0 && r.right == 640 && r.bottom == 480);
    r = d3dt_readback_box(1, &drawn, 640, 480);
    CHECK(r.left == 13 && r.top == 13 && r.right == 147 && r.bottom == 147);
    r = d3dt_readback_box(0, &drawn, 640, 480);
    CHECK(r.left == 0 && r.right == 640 && r.bottom == 480);
    r = d3dt_readback_box(1, &none, 640, 480);
    CHECK(d3dt_rect_empty(&r));
    /* The calibration quad at a 2560x1440 game screen: 135x135 pixels instead of 3.7 million. */
    r = d3dt_readback_box(1, &drawn, 2560, 1440);
    CHECK((r.right - r.left) * (r.bottom - r.top) < 20000);
}

static void test_write_only_locks(void) {
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_FILL_RECT, 0));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_RASTER_BLT, 1));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_RASTER_BLT, 0));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x00412680, 1));
    CHECK(is_pixel_calibration_caller(0x00513660) && is_pixel_calibration_caller(0x00514150) &&
          is_pixel_calibration_caller(0x00514070) && !is_pixel_calibration_caller(0x00412680));
    CHECK(readback_classify(CPU_ACCESS_LOCK, 0, 0x00514150) == READBACK_PIXEL_CALIBRATION);
}

/* RasterBlt of the frozen picture onto the screen in a paused frame: the screen is written, never read, so the
 * GPU picture is not read back and only the blitted pixels reach it. */
static void test_ui_frame_without_readback(void) {
    tsync_t t;
    tsync_deferred_t d;
    unsigned acts;
    tsync_attach(&t);
    tsync_gpu_draw(&t);
    tsync_present(&t);
    tsync_flipped(&t);
    acts = tsync_lock_deferred(&t, target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_RASTER_BLT, 1)
                                       ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ, &d);
    CHECK(!(acts & TSYNC_READBACK));
    acts = tsync_lock_touched(&d);
    CHECK(!(acts & TSYNC_READBACK));
    tsync_cpu_wrote(&t);
    CHECK(tsync_present(&t) & TSYNC_COMPOSITE);
}

static void test_projected_size_policy(void) {
    float ppm1440 = engine_view_pixels_per_metre(1440.0f, 0.7673f * 0.75f);
    float ppm1080 = engine_view_pixels_per_metre(1080.0f, 0.7673f * 0.75f);
    float ppm2160 = engine_view_pixels_per_metre(2160.0f, 0.7673f * 0.75f);
    CHECK(fabs(ppm1440 - ENGINE_PIXELS_PER_METRE_DEFAULT) < 1.0f);
    CHECK(engine_view_pixels_per_metre(0.0f, 0.5f) == ENGINE_PIXELS_PER_METRE_DEFAULT);
    /* The same object spans more pixels at a higher resolution, so it stays 3D farther; 21:9 Hor+ keeps the vertical
     * view, so only the line count matters. */
    CHECK(engine_full_3d_distance(4.0f, ppm1080, 1.0f) < engine_full_3d_distance(4.0f, ppm1440, 1.0f));
    CHECK(engine_full_3d_distance(4.0f, ppm1440, 1.0f) < engine_full_3d_distance(4.0f, ppm2160, 1.0f));
    /* The impostor (256 px) at 1:1 when it takes over. Log 030's meshes are 4 m and up in radius: at 3440x1440,
     * 2 m 25 (the minimum), 4 m ~39, 6 m ~59, 10 m ~98, 16 m 150 (the maximum). */
    CHECK(ENGINE_IMPOSTOR_SWITCH_PX == 256.0f && ENGINE_FULL_3D_MIN == 25.0f && ENGINE_FULL_3D_MAX == 150.0f);
    CHECK(engine_full_3d_distance(0.5f, ppm1440, 1.0f) == ENGINE_FULL_3D_MIN);
    CHECK(engine_full_3d_distance(2.0f, ppm1440, 1.0f) == ENGINE_FULL_3D_MIN);
    CHECK(fabs(engine_full_3d_distance(4.0f, ppm1440, 1.0f) - 39.1f) < 0.5f);
    CHECK(fabs(engine_full_3d_distance(6.0f, ppm1440, 1.0f) - 58.6f) < 0.5f);
    CHECK(fabs(engine_full_3d_distance(10.0f, ppm1440, 1.0f) - 97.7f) < 0.5f);
    CHECK(engine_full_3d_distance(16.0f, ppm1440, 1.0f) == ENGINE_FULL_3D_MAX);
    CHECK(engine_full_3d_distance(500.0f, ppm2160, 9.0f) == ENGINE_FULL_3D_MAX);
    CHECK(engine_size_class(0.5f) == ENGINE_SIZE_SMALL && engine_size_class(3.0f) == ENGINE_SIZE_MEDIUM &&
          engine_size_class(5.0f) == ENGINE_SIZE_LARGE && engine_size_class(10.0f) == ENGINE_SIZE_LANDMARK);
    /* Never nearer than 1998's 15 m x CacheMul, even past the max. */
    CHECK(engine_full_3d_distance(0.2f, ppm1440, 4.0f) == 60.0f);
    CHECK(engine_full_3d_distance(0.2f, ppm1440, 12.0f) == 180.0f);
    /* The guest multiplies the stored distance by CacheMul again. */
    CHECK(fabs(engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, 6.0f, ppm1440, 1.25f) * 1.25f -
               engine_full_3d_distance(6.0f, ppm1440, 1.25f)) < 0.01f);
    CHECK(engine_cache_min_distance(1, 0, ENGINE_PART_SPATIAL, 6.0f, ppm1440, 0.0f) ==
          engine_full_3d_distance(6.0f, ppm1440, 1.0f));
    /* Meshes the CPU rasterises into the Direct3D frame, entities (animals, the player, triggers, water...) and the
     * software renderer keep 1998's distance. */
    CHECK(engine_cache_min_distance(1, 1, ENGINE_PART_STATIC, 6.0f, ppm1440, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance(1, 0, ENGINE_PART_ENTITY, 6.0f, ppm1440, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance(0, 0, ENGINE_PART_STATIC, 6.0f, ppm1440, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    /* The distance an object becomes an impostor is never beyond where it stays visible with its level's 50 m
     * Culling at 1.4x only when it is small: large objects keep 3D until their own, longer, cull distance. */
    CHECK(engine_full_3d_distance(0.5f, ppm1440, 1.0f) < 50.0f * ENGINE_OBJECT_DISTANCE_SCALE);
}

static void test_partition_kinds(void) {
    CHECK(engine_partition_kind(VTABLE_PARTITION) == ENGINE_PART_SPATIAL);
    CHECK(engine_partition_kind(VTABLE_PARTITION_SPACE) == ENGINE_PART_SPATIAL);
    CHECK(engine_partition_kind(VTABLE_PARTITION_SPACE_QUERY) == ENGINE_PART_SPATIAL);
    CHECK(engine_partition_kind(VTABLE_INSTANCE) == ENGINE_PART_STATIC);
    CHECK(engine_partition_kind(VTABLE_TERRAIN_OBJECT) == ENGINE_PART_STATIC);
    /* CAnimal, CAnimate, CPlayer, CGun, CEntity, CEntityAttached, CEntityWater, CBackdrop, CTrigger. */
    CHECK(engine_partition_kind(0x0064F9A8u) == ENGINE_PART_ENTITY && engine_partition_kind(0x0064ED18u) == ENGINE_PART_ENTITY);
    CHECK(engine_partition_kind(0x0064AB48u) == ENGINE_PART_ENTITY && engine_partition_kind(0x0064A274u) == ENGINE_PART_ENTITY);
    CHECK(engine_partition_kind(0x0064A46Cu) == ENGINE_PART_ENTITY && engine_partition_kind(0x0064F7B0u) == ENGINE_PART_ENTITY);
    CHECK(engine_partition_kind(0x0064F3F4u) == ENGINE_PART_ENTITY && engine_partition_kind(0x0064F614u) == ENGINE_PART_ENTITY);
    CHECK(engine_partition_kind(0x0064B534u) == ENGINE_PART_ENTITY);
}

static void test_projected_cull(void) {
    float ppm = ENGINE_PIXELS_PER_METRE_DEFAULT;
    /* A 4 m rock at the 1000 m far clip still spans ~10 px: nothing the level shows today is culled. */
    CHECK(engine_projected_pixels(4.0f, 1000.0f * 1000.0f, ppm) > 9.0f);
    CHECK(!engine_projected_cull(ENGINE_PART_STATIC, 1, 4.0f, 1000.0f * 1000.0f, ppm));
    /* A 5 cm detail beyond 85 m is under 1.5 px. */
    CHECK(engine_projected_cull(ENGINE_PART_STATIC, 1, 0.05f, 90.0f * 90.0f, ppm));
    CHECK(!engine_projected_cull(ENGINE_PART_STATIC, 1, 0.05f, 80.0f * 80.0f, ppm));
    /* Never groups, entities or partitions without a mesh. */
    CHECK(!engine_projected_cull(ENGINE_PART_SPATIAL, 0, 0.05f, 90.0f * 90.0f, ppm));
    CHECK(!engine_projected_cull(ENGINE_PART_ENTITY, 1, 0.05f, 90.0f * 90.0f, ppm));
    CHECK(!engine_projected_cull(ENGINE_PART_STATIC, 0, 0.05f, 90.0f * 90.0f, ppm));
    CHECK(!engine_projected_cull(ENGINE_PART_STATIC, 1, 0.05f, 0.0f, ppm));
}

/* An object visited first and kept 3D holds its parts 3D in the same frame; the next frame starts empty. */
static void test_object_atomicity(void) {
    static engine_frame_slot_t set[64];
    const uint32_t object = 0x01234560u, other = 0x0BADF00Du;
    CHECK(!engine_frame_set_has(set, 64, object, 7));
    engine_frame_set_add(set, 64, object, 7);
    CHECK(engine_frame_set_has(set, 64, object, 7));
    CHECK(!engine_frame_set_has(set, 64, other, 7));
    CHECK(!engine_frame_set_has(set, 64, object, 8));
    for (uint32_t k = 1; k <= 64; k++) engine_frame_set_add(set, 64, k * 0x40u, 8);
    CHECK(engine_frame_set_has(set, 64, 0x40u * 64, 8));
    CHECK(!engine_frame_set_has(set, 64, object, 8));
    engine_frame_set_add(set, 64, object, 9);
    CHECK(engine_frame_set_has(set, 64, object, 9) && !engine_frame_set_has(set, 64, 0x40u, 9));
    /* Only an object that will become one impostor later holds its parts (log 031: holding them for every refusal
     * kept 885 partitions a frame 3D and no impostor at all). */
    CHECK(engine_refusal_holds_parts(CACHE_REFUSED_3D_DISTANCE) && engine_refusal_holds_parts(CACHE_REFUSED_PARALLAX));
    CHECK(engine_refusal_holds_parts(CACHE_REFUSED_TOO_CLOSE) && engine_refusal_holds_parts(CACHE_REFUSED_TOO_BIG));
    CHECK(!engine_refusal_holds_parts(CACHE_REFUSED_NOT_CACHEABLE) && !engine_refusal_holds_parts(CACHE_REFUSED_VOLUME));
    CHECK(!engine_refusal_holds_parts(CACHE_REFUSED_MOVING) && !engine_refusal_holds_parts(CACHE_REFUSED_GROUP));
    CHECK(!engine_refusal_holds_parts(CACHE_REFUSED_SIMPLE_OBJECT) &&
          !engine_refusal_holds_parts(CACHE_REFUSED_FACES_CAMERA));
    /* The stored distance for a part of an object kept 3D refuses at any distance and CacheMul. */
    CHECK(sqrtf(1e12f) + 10.0f < ENGINE_CACHE_DISTANCE_NEVER * 0.01f);
}

/* The game's view sits inside the surface (3212x1345 centred in 3440x1440): iOffsetPointer moves a polygon's box. */
static void test_cpu_polygon_origin(void) {
    long box[4] = { 100, 50, 140, 90 };
    const long pitch = 3440 * 2;
    cpu_polygon_to_surface(box, 47u * (uint32_t)pitch + 114u * 2u, pitch);
    CHECK(box[0] == 214 && box[1] == 97 && box[2] == 254 && box[3] == 137);
    cpu_polygon_to_surface(box, 0, pitch);
    CHECK(box[0] == 214 && box[1] == 97);
    cpu_polygon_to_surface(box, 1000, 0);
    CHECK(box[0] == 214);
}

/* The retail scene heap: CDArray<CRenderPolygon> (0x7C bytes, 20000) reserved by pHeapAlloc and committed by
 * CommitToOffset (0x0051D820) through pvCommitMem; bCommit checks the capacity only after committing, and
 * ptFastAlloc never commits. Pages are tracked to see whether an allocated polygon lies in committed memory. */
#define HEAP_TEST_BASE 0x10000000u
#define HEAP_TEST_ELEMENT 0x7Cu
#define HEAP_TEST_PAGE 0x1000u
typedef struct {
    uint32_t len, max, commit, end;
    unsigned char committed[4096];
    int clamp;
} heap_test_t;

static uint32_t heap_test_commit(heap_test_t* h, uint32_t pv, uint32_t bytes) {
    uint32_t size;
    if (h->clamp) bytes = engine_heap_commit_bytes(pv, bytes, h->end, HEAP_TEST_PAGE);
    size = (bytes + HEAP_TEST_PAGE - 1u) & ~(HEAP_TEST_PAGE - 1u);
    /* VirtualAlloc(MEM_COMMIT) fails as a whole when the range leaves the reservation; pvCommitMem ignores that. */
    if (size && pv + size <= h->end)
        for (uint32_t a = pv & ~(HEAP_TEST_PAGE - 1u); a < pv + size; a += HEAP_TEST_PAGE)
            h->committed[(a - HEAP_TEST_BASE) / HEAP_TEST_PAGE] = 1;
    return pv + size;
}

static int heap_test_bcommit(heap_test_t* h, uint32_t n) {
    uint32_t offset = h->len + n;
    if (offset > h->commit) {
        uint32_t pv = HEAP_TEST_BASE + h->commit * HEAP_TEST_ELEMENT;
        uint32_t end = heap_test_commit(h, pv, (offset - h->commit) * HEAP_TEST_ELEMENT);
        h->commit = (end - HEAP_TEST_BASE) / HEAP_TEST_ELEMENT;
    }
    return offset <= h->max;
}

/* Shapes of these polygon counts; returns how many allocated polygons lay outside committed pages. */
static unsigned heap_test_scene(heap_test_t* h, const unsigned* shapes, unsigned count) {
    unsigned bad = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!heap_test_bcommit(h, shapes[i] * 2u + 32u)) continue;
        for (unsigned k = 0; k < shapes[i]; k++) {
            uint32_t at = HEAP_TEST_BASE + h->len++ * HEAP_TEST_ELEMENT;
            if (!h->committed[(at - HEAP_TEST_BASE) / HEAP_TEST_PAGE] ||
                !h->committed[(at + HEAP_TEST_ELEMENT - 1u - HEAP_TEST_BASE) / HEAP_TEST_PAGE])
                bad++;
        }
    }
    return bad;
}

static void heap_test_init(heap_test_t* h, uint32_t max, int clamp) {
    memset(h, 0, sizeof *h);
    h->max = max;
    h->end = HEAP_TEST_BASE + ((max * HEAP_TEST_ELEMENT + HEAP_TEST_PAGE - 1u) & ~(HEAP_TEST_PAGE - 1u));
    h->clamp = clamp;
}

/* Scenes up to the scaled heap's 80,000 polygons, random shapes over many frames (the heap is emptied each frame and
 * keeps its commits), and requests that end exactly at the reservation or just past it: with the clamp no polygon ever
 * lands outside committed pages; without it the same frames do. */
static void test_scene_heap_stress(void) {
    static heap_test_t h;
    static unsigned shapes[2048];
    uint32_t seed = 0x2468ACE1u, max = 20000u * ENGINE_PIPELINE_HEAP_SCALE;
    unsigned long bad = 0, unguarded_bad = 0, frames = 0;
    unsigned n = 0, accepted;
    heap_test_init(&h, max, 1);
    while (n < 900) shapes[n++] = 100;
    CHECK(heap_test_scene(&h, shapes, n) == 0);
    CHECK(h.len <= max && h.len >= max - (2u * 100u + 32u));
    accepted = h.len;
    shapes[0] = 1;
    CHECK(heap_test_scene(&h, shapes, 1) == 0 && h.len == accepted + (accepted + 34u <= max));
    for (int clamp = 1; clamp >= 0; clamp--) {
        for (int capacity = 0; capacity < 2; capacity++) {
            heap_test_init(&h, capacity ? max : 20000u, clamp);
            for (int frame = 0; frame < 150; frame++) {
                unsigned count = 0;
                while (count < sizeof shapes / sizeof shapes[0] - 1) {
                    seed = seed * 1664525u + 1013904223u;
                    shapes[count++] = 1u + (seed >> 8) % ((seed & 3u) ? 300u : 4000u);
                    if (count > 40 + (seed >> 24)) break;
                }
                h.len = 0;
                if (clamp) bad += heap_test_scene(&h, shapes, count);
                else unguarded_bad += heap_test_scene(&h, shapes, count);
                frames++;
            }
        }
    }
    CHECK(bad == 0);
    CHECK(unguarded_bad > 0);
    CHECK(frames == 600);
    /* A request ending exactly at the reservation commits it all; one element more commits nothing past it. */
    heap_test_init(&h, 20000u, 1);
    {
        uint32_t whole = h.end - HEAP_TEST_BASE;
        CHECK(engine_heap_commit_bytes(HEAP_TEST_BASE, whole, h.end, HEAP_TEST_PAGE) == whole);
        CHECK(engine_heap_commit_bytes(HEAP_TEST_BASE, whole + HEAP_TEST_ELEMENT, h.end, HEAP_TEST_PAGE) == whole);
        CHECK(engine_heap_commit_bytes(h.end - HEAP_TEST_ELEMENT, 2u * HEAP_TEST_ELEMENT, h.end, HEAP_TEST_PAGE) == 0u);
    }
}

/* Log 031: a dense frame filled the 20000-polygon heap; a large shape asked past the reservation, the failed commit
 * still advanced uCommit, the shape was skipped, and the next small shape wrote into uncommitted pages (0x0051B90D,
 * [esi+0x74]). */
static void test_scene_heap_overflow(void) {
    static heap_test_t h;
    static unsigned shapes[64];
    unsigned n = 0;
    for (; n < 47; n++) shapes[n] = 400; /* 18800 polygons */
    shapes[n++] = 1000;                  /* asks for 2032 more: past the 20000 */
    for (unsigned k = 0; k < 12; k++) shapes[n++] = 100; /* each asks for 232: fits the capacity */
    memset(&h, 0, sizeof h);
    h.max = 20000;
    h.end = HEAP_TEST_BASE + ((h.max * HEAP_TEST_ELEMENT + HEAP_TEST_PAGE - 1u) & ~(HEAP_TEST_PAGE - 1u));
    CHECK(heap_test_scene(&h, shapes, n) > 0);
    memset(&h, 0, sizeof h);
    h.max = 20000;
    h.end = HEAP_TEST_BASE + ((h.max * HEAP_TEST_ELEMENT + HEAP_TEST_PAGE - 1u) & ~(HEAP_TEST_PAGE - 1u));
    h.clamp = 1;
    CHECK(heap_test_scene(&h, shapes, n) == 0);
    CHECK(h.len == 19800);
    /* With the scaled heap the same scene keeps every shape. */
    memset(&h, 0, sizeof h);
    h.max = 20000 * ENGINE_PIPELINE_HEAP_SCALE;
    h.end = HEAP_TEST_BASE + ((h.max * HEAP_TEST_ELEMENT + HEAP_TEST_PAGE - 1u) & ~(HEAP_TEST_PAGE - 1u));
    h.clamp = 1;
    CHECK(heap_test_scene(&h, shapes, n) == 0);
    CHECK(h.len == 18800 + 1000 + 1200);
    /* The clamp keeps whole pages inside the reservation and leaves fitting requests untouched. */
    CHECK(engine_heap_commit_bytes(0x10000F00u, 0x2000u, 0x10004000u, 0x1000u) == 0x2000u);
    CHECK(engine_heap_commit_bytes(0x10002F00u, 0x3000u, 0x10004000u, 0x1000u) == 0x1000u);
    CHECK(engine_heap_commit_bytes(0x10003F00u, 0x3000u, 0x10004000u, 0x1000u) == 0u);
    CHECK(engine_heap_commit_bytes(0x10004000u, 0x10u, 0x10004000u, 0x1000u) == 0u);
    /* The four scene heaps scale capacity and reservation together; the terrain's only their reservation. */
    {
        engine_heap_t polys = engine_heap_for(0x0025D780u), verts = engine_heap_for(0x30D400u);
        engine_heap_t tin = engine_heap_for(1758600u), other = engine_heap_for(12345u);
        CHECK(polys.capacity_va == 0x00732F34u && polys.capacity == 20000u && polys.bytes == 20000u * 0x7Cu);
        CHECK(verts.capacity_va == 0x00732EFCu && verts.capacity == 80000u && verts.bytes == 80000u * 0x28u);
        CHECK(engine_heap_for(0x00013880u).capacity == 20000u && engine_heap_for(0x0004E200u).capacity == 80000u);
        CHECK(tin.scale == TERRAIN_HEAP_SCALE && !tin.capacity_va);
        CHECK(other.scale == 1u);
        /* Depth sort's alloca arrays (8 bytes per polygon) stay far below the 4 MB guest stack. */
        CHECK(polys.capacity * polys.scale * 8u < (4u << 20) / 4u);
    }
}

static void test_cache_refusals(void) {
    engine_cache_settings_t c = { 1, 8.55f, 0.085f * 0.085f, 40.0f };
    engine_partition_t mesh = { PARTITION_USES_MESH | PARTITION_CACHEABLE | PARTITION_CACHEABLE_VOLUME |
                                    PARTITION_HARDWARE_ABLE, 10, 100, 4.0f, 4.0f, 0.0f, 1.0f };
    engine_partition_t p = mesh;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_PARALLAX);
    CHECK(engine_cache_refusal(&p, &c, 30.0f * 30.0f) == CACHE_REFUSED_3D_DISTANCE);
    CHECK(engine_cache_refusal(&p, &c, 3.0f * 3.0f) == CACHE_REFUSED_TOO_CLOSE);
    CHECK(engine_cache_refusal(&p, &c, 5.0f * 5.0f) == CACHE_REFUSED_TOO_BIG);
    p.flags &= ~PARTITION_CACHEABLE;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_NOT_CACHEABLE);
    p = mesh;
    p.flags |= PARTITION_SIMPLE_OBJECT;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_SIMPLE_OBJECT);
    p = mesh;
    p.flags |= PARTITION_ALWAYS_FACE;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_FACES_CAMERA);
    p = mesh;
    p.no_cache_until = 100;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_MOVING);
    p = mesh;
    p.flags &= ~PARTITION_CACHEABLE_VOLUME;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_VOLUME);
    /* A group: below 25 m, or less efficient than 0.085 x distance, never one impostor. */
    p = mesh;
    p.flags &= ~PARTITION_USES_MESH;
    p.area_efficiency_sqr = 400.0f;
    CHECK(engine_cache_refusal(&p, &c, 20.0f * 20.0f) == CACHE_REFUSED_GROUP);
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_GROUP);
    CHECK(engine_cache_refusal(&p, &c, 300.0f * 300.0f) == CACHE_REFUSED_PARALLAX);
    for (int r = 0; r < CACHE_REFUSED_COUNT; r++) CHECK(strcmp(engine_cache_refusal_name((cache_refusal_t)r), "?") != 0);
    /* The level's CacheNever is told apart from what the image cache cannot draw. */
    p = mesh;
    p.flags = (p.flags & ~PARTITION_CACHEABLE) | PARTITION_CACHE_NEVER;
    CHECK(engine_cache_refusal(&p, &c, 200.0f * 200.0f) == CACHE_REFUSED_CACHE_NEVER);
    CHECK(!engine_refusal_holds_parts(CACHE_REFUSED_CACHE_NEVER));
}

/* Only a plain static instance marked CacheNever is judged by the rest of bFlagCacheable, and only with Direct3D 11's
 * limits; groups may hold up to 40 objects, never fewer than the game asked. */
static void test_cache_never_and_groups(void) {
    uint32_t never = PARTITION_USES_MESH | PARTITION_CACHE_NEVER;
    CHECK(engine_cache_never_lifted(1, VTABLE_INSTANCE, never));
    CHECK(!engine_cache_never_lifted(0, VTABLE_INSTANCE, never));
    CHECK(!engine_cache_never_lifted(1, VTABLE_INSTANCE, PARTITION_USES_MESH));
    CHECK(!engine_cache_never_lifted(1, VTABLE_TERRAIN_OBJECT, never));
    CHECK(!engine_cache_never_lifted(1, VTABLE_PARTITION, never));
    CHECK(!engine_cache_never_lifted(1, 0x0064D000u, never)); /* an entity */
    CHECK(engine_cache_group_objects(1, 20) == ENGINE_CACHE_GROUP_OBJECTS_MODERN);
    CHECK(engine_cache_group_objects(1, 10) == ENGINE_CACHE_GROUP_OBJECTS_MODERN);
    CHECK(engine_cache_group_objects(0, 20) == 20 && engine_cache_group_objects(0, 10) == 10);
    CHECK(engine_cache_group_objects(1, 64) == 64);
}

typedef struct {
    int calls;
    long area;
} tile_calls_t;

static void count_tile_rect(void* context, const d3dt_rect_t* r) {
    tile_calls_t* c = context;
    c->calls++;
    c->area += (r->right - r->left) * (r->bottom - r->top);
}

/* One Lock spanning several CPU polygons: each captures only tiles no earlier polygon of the Lock may have written. */
static void test_cpu_polygon_tiles(void) {
    static tsync_tiles_t m;
    tile_calls_t c = { 0, 0 };
    d3dt_rect_t a = { 10, 10, 50, 40 }, b = { 40, 20, 100, 60 }, off = { 3430, 1430, 3600, 1600 };
    CHECK(tsync_tiles_reset(&m, 3440, 1440));
    CHECK(m.tiles_x == 108 && m.tiles_y == 45);
    tsync_tiles_claim(&m, &a, count_tile_rect, &c);
    CHECK(c.calls == 2 && c.area == 64 * 64);
    c.calls = 0;
    c.area = 0;
    tsync_tiles_claim(&m, &a, count_tile_rect, &c);
    CHECK(c.calls == 0);
    tsync_tiles_claim(&m, &b, count_tile_rect, &c);
    CHECK(c.area == 128 * 64 - 64 * 64);
    c.calls = 0;
    c.area = 0;
    tsync_tiles_claim(&m, &off, count_tile_rect, &c);
    CHECK(c.calls == 1 && c.area == 16 * 32);
    /* A whole-surface capture leaves nothing to claim. */
    tsync_tiles_claim_all(&m);
    c.calls = 0;
    tsync_tiles_claim(&m, &b, count_tile_rect, &c);
    CHECK(c.calls == 0);
    CHECK(!tsync_tiles_reset(&m, 9000, 100));
}

/* After a Flip the reference matches the other CPU buffer: in a lazy capture only tiles captured since are compared;
 * a stale tile's old pixels are never uploaded and its reference is left alone. */
static void test_lazy_capture(void) {
    static uint16_t copy[64 * 64], reference[64 * 64];
    static uint32_t out[64 * 64];
    static tsync_tiles_t fresh;
    d3dt_rect_t all = { 0, 0, 64, 64 }, tile0 = { 0, 0, 32, 32 }, box;
    tsync_counts_t counts = { 0, 0 };
    for (int i = 0; i < 64 * 64; i++) {
        copy[i] = (uint16_t)(i * 7);
        reference[i] = (uint16_t)(i * 13);
    }
    CHECK(tsync_tiles_reset(&fresh, 64, 64));
    tsync_fill((uint8_t*)copy, 128, reference, 64, &tile0, TSYNC_UNWRITTEN);
    tsync_tiles_claim(&fresh, &tile0, count_tile_rect, &(tile_calls_t){ 0, 0 });
    copy[5 * 64 + 6] = 0x1234;
    CHECK(tsync_changed_box_in((const uint8_t*)copy, 128, reference, 64, &all, &fresh, &box));
    CHECK(box.left == 6 && box.right == 7 && box.top == 5 && box.bottom == 6);
    /* Unmasked, the stale tiles would all count as changed. */
    CHECK(tsync_changed_box((const uint8_t*)copy, 128, reference, 64, &all, &box) && box.right == 64 && box.bottom == 64);
    tsync_overlay_box_in((const uint8_t*)copy, 128, reference, 64, &all, &fresh, out, &counts);
    CHECK(counts.changed == 1 && out[5 * 64 + 6] != 0);
    CHECK(out[40 * 64 + 40] == 0 && out[0] == 0);
    CHECK(reference[5 * 64 + 6] == 0x1234 && reference[40 * 64 + 40] == (uint16_t)((40 * 64 + 40) * 13));
    CHECK(tsync_tile_claimed(&fresh, 0, 0) && !tsync_tile_claimed(&fresh, 1, 0) && !tsync_tile_claimed(&fresh, 1, 1));
    /* Without a tile map both behave as before. */
    CHECK(!tsync_changed_box_in((const uint8_t*)copy, 128, reference, 64, &tile0, NULL, &box) ||
          tsync_changed_box((const uint8_t*)copy, 128, reference, 64, &tile0, &box));
}

/* The text overlay's capture: only the strings' boxes are claimed, and a composite compares nothing outside the bounds
 * of the claimed tiles, which find every changed pixel the full compare would inside them. */
static void test_text_box_capture(void) {
    static uint16_t copy[3440 * 64], reference[3440 * 64];
    static tsync_tiles_t fresh;
    d3dt_rect_t all = { 0, 0, 3440, 64 }, line = { 100, 10, 260, 40 }, line2 = { 3000, 33, 3100, 50 }, region, box;
    tile_calls_t c = { 0, 0 };
    CHECK(tsync_tiles_reset(&fresh, 3440, 64));
    region = tsync_tiles_clip(&fresh, &all);
    CHECK(d3dt_rect_empty(&region));
    tsync_tiles_claim(&fresh, &line, count_tile_rect, &c);
    CHECK(fresh.bounds.left == 96 && fresh.bounds.top == 0 && fresh.bounds.right == 288 && fresh.bounds.bottom == 64);
    tsync_tiles_claim(&fresh, &line2, count_tile_rect, &c);
    CHECK(fresh.bounds.left == 96 && fresh.bounds.right == 3104 && fresh.bounds.bottom == 64);
    region = tsync_tiles_clip(&fresh, &all);
    CHECK(region.left == 96 && region.right == 3104 && region.top == 0 && region.bottom == 64);
    for (int i = 0; i < 3440 * 64; i++) copy[i] = reference[i] = TSYNC_UNWRITTEN;
    copy[20 * 3440 + 150] = 0x07E0;
    copy[40 * 3440 + 3050] = 0xF800;
    copy[5 * 3440 + 2000] = 0x001F; /* outside every claimed tile: not this capture's */
    CHECK(tsync_changed_box_in((const uint8_t*)copy, 3440 * 2, reference, 3440, &region, &fresh, &box));
    CHECK(box.left == 150 && box.right == 3051 && box.top == 20 && box.bottom == 41);
    /* An off-surface box is clipped; a whole claim covers everything. */
    tsync_tiles_claim_all(&fresh);
    region = tsync_tiles_clip(&fresh, &all);
    CHECK(region.left == 0 && region.right == 3440 && region.bottom == 64);
    CHECK(tsync_tiles_reset(&fresh, 3440, 64) && d3dt_rect_empty(&fresh.bounds));
}

/* A box readback for a polygon whose primitive may read: only pixels the CPU did not write take the GPU picture, and
 * nothing outside the box is read. */
static void test_cpu_polygon_box_readback(void) {
    uint16_t copy[8 * 4], reference[8 * 4], gpu[3 * 2];
    d3dt_rect_t all = { 0, 0, 8, 4 }, box = { 2, 1, 5, 3 };
    for (int i = 0; i < 6; i++) gpu[i] = (uint16_t)(0x1000 + i);
    tsync_fill((uint8_t*)copy, 16, reference, 8, &all, TSYNC_UNWRITTEN);
    copy[1 * 8 + 3] = 0x0777;
    tsync_fill_unwritten((uint8_t*)copy, 16, reference, 8, &box, gpu, 3);
    CHECK(copy[1 * 8 + 2] == 0x1000 && reference[1 * 8 + 2] == 0x1000);
    CHECK(copy[1 * 8 + 3] == 0x0777 && reference[1 * 8 + 3] == TSYNC_UNWRITTEN);
    CHECK(copy[2 * 8 + 4] == 0x1005 && reference[2 * 8 + 4] == 0x1005);
    CHECK(copy[0] == TSYNC_UNWRITTEN && copy[3 * 8 + 7] == TSYNC_UNWRITTEN && copy[1 * 8 + 5] == TSYNC_UNWRITTEN);
    /* The written pixel still differs from the reference, so the composite uploads it; the rest stay transparent. */
    {
        d3dt_rect_t changed;
        CHECK(tsync_changed_box((const uint8_t*)copy, 16, reference, 8, &all, &changed));
        CHECK(changed.left == 3 && changed.right == 4 && changed.top == 1 && changed.bottom == 2);
    }
}

static void test_engine_limits(void) {
    float cull = engine_cull_distance(1.0f, 800.0f, ENGINE_CULL_SCALE_ORIGINAL, ENGINE_CULL_MAX_ORIGINAL);
    float combined = ENGINE_OBJECT_DISTANCE_SCALE * ENGINE_OBJECT_DISTANCE_SCALE, level_culling = 50.0f;
    CHECK(fabs(cull - 126.0f) < 0.01);
    CHECK(fabs(engine_cull_distance(4.0f, 800.0f, ENGINE_CULL_SCALE_ORIGINAL, ENGINE_CULL_MAX_ORIGINAL) - 504.0f) < 0.01);
    CHECK(fabs(engine_cull_distance(7.2f, 800.0f, ENGINE_CULL_SCALE_ORIGINAL, ENGINE_CULL_MAX_ORIGINAL) - 907.2f) < 0.01);
    CHECK(fabs(engine_cull_distance(0.05f, 800.0f, ENGINE_CULL_SCALE_ORIGINAL, ENGINE_CULL_MAX_ORIGINAL) - 20.05f) < 0.01);
    /* bInRange: d * d < cull * cull * combined, so the combined factor is the square of the distance scale. */
    CHECK(cull * (ENGINE_OBJECT_DISTANCE_SCALE - 0.01f) * cull * (ENGINE_OBJECT_DISTANCE_SCALE - 0.01f) <
              cull * cull * combined &&
          cull * (ENGINE_OBJECT_DISTANCE_SCALE + 0.01f) * cull * (ENGINE_OBJECT_DISTANCE_SCALE + 0.01f) >
              cull * cull * combined);
    /* Log 029: 1.6x considered 1100-1569 partitions in dense areas; 1.4x keeps (1.4/1.6)^2 = 77% of the area. */
    CHECK(ENGINE_OBJECT_DISTANCE_SCALE >= 1.35f && ENGINE_OBJECT_DISTANCE_SCALE <= 1.5f);
    CHECK(69.9f * 69.9f < level_culling * level_culling * combined && 70.1f * 70.1f > level_culling * level_culling * combined);
    test_projected_size_policy();
    test_cache_refusals();
    test_partition_kinds();
    test_projected_cull();
    test_object_atomicity();
    CHECK(engine_texture_budget_mb(24, 1, 1) == ENGINE_TEXTURE_MB_MODERN && engine_texture_budget_mb(24, 0, 1) == 24);
    CHECK(engine_texture_budget_mb(24, 1, 0) == ENGINE_TEXTURE_MB_SAFE);
    CHECK(engine_texture_budget_mb(16, 1, 1) == 16 && engine_texture_budget_mb(18, 1, 1) == 18);
    CHECK(engine_impostor_pages(9113, 128) == 284);
    CHECK(engine_impostor_pages(15667, 256) == 122);
    CHECK(engine_texture_dim_policy(1) == 256 && engine_texture_dim_policy(0) == 0);
    CHECK(engine_render_quality_policy(1, 0) == 4 && !engine_render_quality_policy(1, 1) &&
          !engine_render_quality_policy(0, 0));
    /* x86: the texture memory the game may hold, twice over for its pages and ours, stays far below 2 GB. */
    CHECK((unsigned long long)ENGINE_TEXTURE_MB_MODERN * 2 * 1048576 < 256ull * 1048576);
    /* Terrain pages: 256 px, 16-bit, and the regular textures keep more than the old 40 MB budget gave them. */
    CHECK(ENGINE_TERRAIN_TEXTURE_KB_MODERN >= 1920u && ENGINE_TERRAIN_TEXTURE_KB_MODERN % 128u == 0);
    CHECK(engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0x7FFFFFFFu, 1).regular -
              (int32_t)(ENGINE_TERRAIN_TEXTURE_KB_MODERN * 1024u - 0x1E0000u) >
          engine_texture_layout(ENGINE_TEXTURE_MB_SAFE, 0x7FFFFFFFu, 0).regular);
}

/* CQuadRootTIN::Update's inputs: levels set 230 m, TrrPixelTol 10 (7 near the beach) and TrrPixelTolFar 50; quality 4
 * scales distances by 1.5 and tolerances by 0.5. */
static void test_terrain_settings(void) {
    terrain_settings_t level = { 230.0f, 10.0f, 50.0f, 1.5f, 0.5f }, used;
    used = terrain_modern_settings(level, 1000.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - 1000.0f) < 0.01);
    /* 1440 lines: the level's 10 is already finer than the community's 4 at 480 lines (12); far 50 becomes 36. */
    CHECK(used.pixel_tolerance == 10.0f && fabs(used.pixel_tolerance_far - 36.0f) < 0.001);
    CHECK(used.distance_scale == level.distance_scale && used.pixel_tolerance_scale == level.pixel_tolerance_scale);
    used = terrain_modern_settings(level, 1000.0f, 1.0f);
    CHECK(used.pixel_tolerance == TERRAIN_PIXEL_TOL_480 && used.pixel_tolerance_far == TERRAIN_PIXEL_TOL_FAR_480);
    used = terrain_modern_settings(level, 1000.0f, 0.0f);
    CHECK(used.pixel_tolerance == 10.0f && used.pixel_tolerance_far == 50.0f);
    used = terrain_modern_settings(level, 600.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - 600.0f) < 0.01);
    used = terrain_modern_settings(level, 0.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - TERRAIN_TEXTURE_DISTANCE_MAX) < 0.01);
    used = terrain_modern_settings(level, 50000.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - TERRAIN_TEXTURE_DISTANCE_MAX) < 0.01);
    /* The rvar's own range is 10-1000: Update reads the data directly, so a value past 1000 / 1.5 still works. */
    CHECK(used.dynamic_texture_distance > 230.0f && used.dynamic_texture_distance <= 1000.0f);
    level.dynamic_texture_distance = 900.0f;
    level.pixel_tolerance = 7.0f;
    level.pixel_tolerance_far = 5.0f;
    used = terrain_modern_settings(level, 1000.0f, 3.0f);
    CHECK(used.dynamic_texture_distance == 900.0f && used.pixel_tolerance == 7.0f && used.pixel_tolerance_far == 7.0f);
    /* The finer far tolerance only with the node heaps enlarged: 1998's 15000 nodes have no overflow check. */
    CHECK(TERRAIN_HEAP_SCALE >= 2u && 1758600u * TERRAIN_HEAP_SCALE < 64u << 20);
}

/* The 1998 split of the texture budget, in the engine's 32-bit arithmetic (test 024: 64 MB made the game drop
 * Direct3D right after creating its gameplay device). */
/* The game ends its upload loop when GetTickCount() - start > 7: inside the budget the clock stands still, after it
 * the game's own test stops the loop. */
static void test_engine_upload_clock(void) {
    const uint32_t start = 0xFFFFFFF0u;
    CHECK(engine_upload_clock(start, 0.0, ENGINE_UPLOAD_BUDGET_MS) == start);
    CHECK(engine_upload_clock(start, ENGINE_UPLOAD_BUDGET_MS, ENGINE_UPLOAD_BUDGET_MS) == start);
    CHECK(engine_upload_clock(start, ENGINE_UPLOAD_BUDGET_MS + 0.01, ENGINE_UPLOAD_BUDGET_MS) - start >
          ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(engine_upload_clock(100u, 7.5, 7.0) - 100u > ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(engine_upload_clock(100u, 6.9, 7.0) - 100u <= ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(ENGINE_UPLOAD_BUDGET_MS > ENGINE_UPLOAD_MS_ORIGINAL && ENGINE_UPLOAD_BUDGET_MS < 16.7);
}

static void test_engine_texture_layout(void) {
    static const unsigned budgets[] = { 64, 96, 128 };
    engine_texture_layout_t l = engine_texture_layout(24, 0x7FFFFFFFu, 0);
    CHECK(l.total == 23330816 && l.image_cache == 9332326 && l.regular == 9483674 && l.accepted && !l.overflow);
    CHECK(l.water == 569 * 1024 && l.terrain == 0x1E0000);
    /* The engine's own arithmetic: 64 MB wraps and turns Direct3D off (test 024), 46 MB wraps the 95% mark. */
    l = engine_texture_layout(64, 0x7FFFFFFFu, 0);
    CHECK(!l.accepted && l.overflow && l.image_cache < 0 && l.low_water_85 < 0);
    l = engine_texture_layout(48, 0x7FFFFFFFu, 0);
    CHECK(l.accepted && l.overflow && l.high_water_95 < 0);
    CHECK(engine_texture_layout(45, 0x7FFFFFFFu, 0).accepted && !engine_texture_layout(45, 0x7FFFFFFFu, 0).overflow);
    CHECK(engine_texture_layout(46, 0x7FFFFFFFu, 0).overflow);
    for (unsigned mb = 8; mb <= ENGINE_TEXTURE_MB_SAFE; mb++) {
        l = engine_texture_layout(mb, 0x7FFFFFFFu, 0);
        CHECK(l.accepted && !l.overflow && l.image_cache > 0 && l.high_water_95 > l.low_water_85);
    }
    /* Below the overflow the host's split is the engine's, value for value. */
    for (unsigned mb = 8; mb <= 45; mb++) {
        engine_texture_layout_t engine = engine_texture_layout(mb, 0x7FFFFFFFu, 0);
        l = engine_texture_layout(mb, 0x7FFFFFFFu, 1);
        CHECK(l.image_cache == engine.image_cache && l.regular == engine.regular &&
              l.low_water_85 == engine.low_water_85 && l.high_water_95 == engine.high_water_95);
    }
    for (unsigned i = 0; i < sizeof budgets / sizeof budgets[0]; i++) {
        engine_texture_layout_t engine = engine_texture_layout(budgets[i], 0x7FFFFFFFu, 0);
        int32_t allocated, low, high, rest, engine_run, corrected;
        l = engine_texture_layout(budgets[i], 0x7FFFFFFFu, 1);
        CHECK(l.accepted && !l.overflow && l.image_cache == (int32_t)(ENGINE_IMAGE_CACHE_KB_MAX * 1024u));
        CHECK(l.low_water_85 == (int32_t)((int64_t)l.regular * 85 / 100) &&
              l.high_water_95 == (int32_t)((int64_t)l.regular * 95 / 100) && l.high_water_95 > l.low_water_85);
        CHECK(l.regular > engine_texture_layout(ENGINE_TEXTURE_MB_SAFE, 0x7FFFFFFFu, 0).regular);
        /* The split subtracts its own (wrapped) cache share; the host moves the difference to the regular pool. */
        rest = l.total - 0x20000 - 0x1C0000 - l.water - l.terrain;
        engine_run = (int32_t)(uint32_t)((uint32_t)rest - (uint32_t)engine.image_cache);
        CHECK(engine_run > 0x280000);
        corrected = (int32_t)((int64_t)engine_run + engine_image_cache_bytes(l.total, 0) - engine_image_cache_bytes(l.total, 1));
        CHECK(corrected == l.regular);
        engine_agp_marks(corrected, &allocated, &low, &high);
        CHECK(allocated == l.regular && low == l.low_water_85 && high == l.high_water_95);
    }
    engine_agp_marks(0x40000, &l.regular, &l.low_water_85, &l.high_water_95);
    CHECK(l.regular == 0 && l.low_water_85 == 0 && l.high_water_95 == 0);
    l = engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0x7FFFFFFFu, 1);
    CHECK(l.regular > 3 * engine_texture_layout(ENGINE_TEXTURE_MB_ORIGINAL, 0x7FFFFFFFu, 0).regular);
    /* Less video memory than the budget: the engine takes the smaller. */
    CHECK(engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 16u << 20, 1).total == (16 << 20) - 0x120000);
    CHECK(engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0xFFFFFFFFu, 1).total ==
          engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0x7FFFFFFFu, 1).total);
}

static void test_page_arena(void) {
    uint32_t bitmap[8] = { 0 };
    ULONG_PTR count = 16;
    ULONG granularity = 0;
    PVOID pages[16];
    uint8_t *a, *b;
    size_t reserved = 0, used = 0;
    CHECK(page_run_find(bitmap, 256, 4) == 0);
    page_run_mark(bitmap, 0, 3, 1);
    page_run_mark(bitmap, 5, 1, 1);
    CHECK(page_run_find(bitmap, 256, 2) == 3);
    CHECK(page_run_find(bitmap, 256, 3) == 6);
    page_run_mark(bitmap, 0, 256, 1);
    CHECK(page_run_find(bitmap, 256, 1) == -1);
    CHECK(page_run_find(bitmap, 256, 0) == -1);
    a = (uint8_t*)page_arena_alloc(32 * 32 * 2);
    b = (uint8_t*)page_arena_alloc(64 * 64 * 2);
    CHECK(a && b && a != b);
    CHECK(!page_arena_alloc(PAGE_ARENA_MAX_BYTES + 1));
    if (!a || !b) return;
    CHECK(((uintptr_t)a % PAGE_ARENA_PAGE) == 0 && ((uintptr_t)b % PAGE_ARENA_PAGE) == 0);
    CHECK(a[0] == 0 && b[8191] == 0);
    ResetWriteWatch(a, PAGE_ARENA_PAGE);
    ResetWriteWatch(b, 2 * PAGE_ARENA_PAGE);
    b[100] = 7;
    CHECK(!GetWriteWatch(0, a, PAGE_ARENA_PAGE, pages, &count, &granularity) && count == 0);
    count = 16;
    CHECK(!GetWriteWatch(0, b, 2 * PAGE_ARENA_PAGE, pages, &count, &granularity) && count == 1);
    page_arena_usage(&reserved, &used);
    CHECK(reserved >= 1048576 && used == 3 * PAGE_ARENA_PAGE);
    page_arena_free(b, 64 * 64 * 2);
    b = (uint8_t*)page_arena_alloc(64 * 64 * 2);
    CHECK(b && b[100] == 0);
    count = 16;
    CHECK(b && !GetWriteWatch(0, b, 2 * PAGE_ARENA_PAGE, pages, &count, &granularity) && count == 0);
    page_arena_free(a, 32 * 32 * 2);
    if (b) page_arena_free(b, 64 * 64 * 2);
    page_arena_usage(&reserved, &used);
    CHECK(used == 0);
}

int main(void) {
    test_fps_counter();
    test_input_state();
    test_capture_policy();
    test_keys();
    test_shutdown();
    test_quit_steps();
    test_safemode();
    test_open_press();
    test_target_access_policy();
    test_scene_heap_overflow();
    test_cpu_polygon_tiles();
    test_cpu_polygon_origin();
    test_lazy_capture();
    test_cache_never_and_groups();
    test_scene_heap_stress();
    test_text_box_capture();
    test_cpu_polygon_box_readback();
    test_target_sync_buffers();
    test_stale_black_cannot_replace_gpu();
    test_no_read_lock_on_gpu_picture();
    test_reading_lock_reads_back();
    test_pending_writes_survive_readback();
    test_deferred_read_keeps_content();
    test_deferred_write_never_reads();
    test_deferred_read_write();
    test_deferred_sync_only();
    test_deferred_keeps_pending_writes();
    test_deferred_current_copy_and_clear();
    test_deferred_water_frame();
    test_hybrid_frame();
    test_cpu_made_frames();
    test_gpu_lost_and_resized();
    test_clear_drops_pending();
    test_icall_ring();
    test_renderer_switch();
    test_f5_lifecycle();
    test_address_and_cull();
    test_readback_classes();
    test_primitives();
    test_states();
    test_texture_blend();
    test_terrain_blend();
    test_hardware_water();
    test_fog_and_layout();
    test_depth_and_cull();
    test_coverage_and_uv();
    test_terrain_state_leak();
    test_formats();
    test_convert_rows();
    test_overlay_and_rects();
    test_resolution_and_msaa();
    test_native_gameplay_resolution();
    test_menus_videos_and_text();
    test_overlay_texel_edge();
    test_ui_canvas();
    test_shaders();
    test_bump_shaders();
    test_bump_math();
    test_batch_slots();
    test_copy_changed();
    test_mip_chain();
    test_mip_alpha();
    test_coverage_selection();
    test_dynamic_textures();
    test_sampler_choice();
    test_readback_regions();
    test_write_only_locks();
    test_ui_frame_without_readback();
    test_engine_limits();
    test_engine_texture_layout();
    test_terrain_settings();
    test_engine_upload_clock();
    test_page_arena();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}
