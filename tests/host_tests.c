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

#include "ddraw/d3d11_shaders.h"
#include "ddraw/d3d_translate.h"
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
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_BEGIN_FRAME));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x1, LOCK_CALLER_FLUSH_BATCH));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x00513660));
    CHECK(target_access_reads(CPU_ACCESS_LOCK, 0x1, 0x00504D40));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x21, 0x00435590));
    CHECK(!target_access_reads(CPU_ACCESS_LOCK, 0x2001, 0x00435590));
    CHECK(target_access_reads(CPU_ACCESS_GETDC, 0x20, LOCK_CALLER_BEGIN_FRAME));
    CHECK(target_access_reads(CPU_ACCESS_GETDC, 0, 0x005038E0));
    CHECK(!target_access_reads(CPU_ACCESS_GETDC, 0, GETDC_CALLER_TEXT_OVERLAY));
    CHECK(target_access_reads(CPU_ACCESS_BLT_SOURCE, 0, 0));
    CHECK(!target_access_reads(CPU_ACCESS_BLT_DEST, 0, 0));
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
}

static void test_readback_classes(void) {
    CHECK(readback_classify(CPU_ACCESS_LOCK, 1, 0x500000, 0x513660) == READBACK_LOCK_READ);
    CHECK(readback_classify(CPU_ACCESS_LOCK, 0, 0x500000, 0x513660) == READBACK_LOCK_WRITE);
    CHECK(readback_classify(CPU_ACCESS_LOCK, 0, 0x513660, 0x513660) == READBACK_PIXEL_CALIBRATION);
    CHECK(readback_classify(CPU_ACCESS_GETDC, 0, 1, 0x513660) == READBACK_GETDC);
    CHECK(readback_classify(CPU_ACCESS_BLT_SOURCE, 1, 1, 0x513660) == READBACK_BLT_SOURCE);
    CHECK(readback_classify(CPU_ACCESS_FRONT, 1, 1, 0x513660) == READBACK_FRONT_BUFFER);
    CHECK(readback_classify(CPU_ACCESS_OTHER, 1, 1, 0) == READBACK_OTHER);
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
        CHECK(!d3dt_row_black(out, 3) && d3dt_row_black(out, 1));
        out[0] = 0x00000000u;
        out[1] = 0xFF000000u;
        CHECK(d3dt_row_black(out, 2));
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
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}
