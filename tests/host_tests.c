/*
 * Headless checks of the host's pure logic: no window, no device, no game, no audio.
 * Build: CMake target host_tests (docs/BUILD.md, "Headless tests").
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "test_profile.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <ddraw.h>
#include <d3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bump_spans.h"
#include "texture_gpu_policy.h"
#include "ddraw/d3d11_shaders.h"
#include "ddraw/d3d_translate.h"
#include "ddraw/page_arena.h"
#include "ddraw/perf_stat.h"
#include "ddraw/target_sync.h"
#include "host_policy.h"
#include "jpt_display.h"
#include "window_title.h"

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

/* The cursor is confined (and relative motion drives the view) exactly when the window host would clip. */
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
    /* Esc and F1 open CInGameOptionsWnd and CHintWnd over CGameWnd: a dialog. F12 (CControlsWnd) is no longer passed
     * on and F2 is the host's controls card, so neither opens a window of the game. */
    CHECK(!clips(GUEST_UI_DIALOG, 1, 0, 0, 0, 0));
    CHECK(key_opens_game_window(0x100, 0x1B, 0) && key_opens_game_window(0x100, 0x70, 0));
    CHECK(!key_opens_game_window(0x100, 0x7B, 0) && !key_opens_game_window(0x100, 0x71, 0));
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
    {
        /* log 055: the Controls background (318x198) blitted through its 1.5x control rect after Alt+Tab redrew the
         * whole window; the source rect stays inside the picture, an empty one draws nothing */
        int32_t r[4] = { 0, 0, 480, 300 }, inside[4] = { 10, 20, 100, 120 }, outside[4] = { 400, 0, 480, 300 };
        int32_t negative[4] = { -5, -5, 20, 20 };
        CHECK(raster_source_clip(r, 318, 198) && r[0] == 0 && r[1] == 0 && r[2] == 318 && r[3] == 198);
        CHECK(raster_source_clip(inside, 318, 198) && inside[2] == 100 && inside[3] == 120);
        CHECK(!raster_source_clip(outside, 318, 198));
        CHECK(raster_source_clip(negative, 318, 198) && negative[0] == 0 && negative[1] == 0 && negative[2] == 20);
    }
    {
        /* log 103: Controls' Cancel (cancel_0.tga, 118x42) in its 1.75x rect; the hover at (515,392) sampled column
         * 128, row 50 of the picture */
        const int32_t cancel[4] = { 387, 342, 593, 415 }, same[4] = { 10, 20, 128, 62 }, narrow[4] = { 0, 0, 60, 20 };
        int32_t p[2] = { 515, 392 }, out[2] = { 600, 392 }, q[2] = { 50, 30 }, s[2] = { 59, 19 }, edge[2];
        int all_inside = 1;
        CHECK(button_picture_point(cancel, 118, 42, p) && p[0] == 387 + 128 * 118 / 206 && p[1] == 342 + 50 * 42 / 73);
        CHECK(!button_picture_point(cancel, 118, 42, out) && out[0] == 600 && out[1] == 392);
        CHECK(!button_picture_point(same, 118, 42, q) && q[0] == 50 && q[1] == 30);
        CHECK(!button_picture_point(narrow, 118, 42, s) && s[0] == 59 && s[1] == 19);
        CHECK(!button_picture_point(cancel, 0, 42, p));
        for (int32_t y = cancel[1]; y < cancel[3]; y++)
            for (int32_t x = cancel[0]; x < cancel[2]; x++) {
                edge[0] = x;
                edge[1] = y;
                button_picture_point(cancel, 118, 42, edge);
                if (edge[0] - cancel[0] >= 118 || edge[1] - cancel[1] >= 42 || edge[0] < cancel[0] || edge[1] < cancel[1])
                    all_inside = 0;
            }
        CHECK(all_inside);
    }
    /* Gamepad focus hides the pointer in menus and dialogs only; the mouse (pad_focus 0) brings it back. */
    CHECK(input_pad_hides_pointer(INPUT_MENU, 1) && input_pad_hides_pointer(INPUT_DIALOG, 1));
    CHECK(!input_pad_hides_pointer(INPUT_MENU, 0) && !input_pad_hides_pointer(INPUT_DIALOG, 0));
    CHECK(!input_pad_hides_pointer(INPUT_GAMEPLAY_RELEASED, 1) && !input_pad_hides_pointer(INPUT_SHUTDOWN, 1) &&
          !input_pad_hides_pointer(INPUT_MOVING_WINDOW, 1));
}

static void test_keys(void) {
    CHECK(key_policy(0x100, 0x7A, 0, 0, 0) == KEY_HOST_FULLSCREEN);
    CHECK(key_policy(0x100, 0x7A, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x101, 0x7A, 0, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x7A, 0, 1, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x7A, 1, 1, 0) == KEY_DROP);
    CHECK(key_policy(0x101, 0x7A, 0, 1, 0) == KEY_DROP);
    /* F5 is the host's in every state (gameplay, menus, dialogs): the minimap toggle, which acts in gameplay only. */
    for (int mods = 0; mods < 4; mods++) {
        CHECK(key_policy(0x100, 0x74, 0, mods & 1, mods >> 1) == KEY_HOST_MINIMAP);
        CHECK(key_policy(0x104, 0x74, 0, mods & 1, mods >> 1) == KEY_HOST_MINIMAP);
        CHECK(key_policy(0x100, 0x74, 1, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x74, 0, mods & 1, mods >> 1) == KEY_DROP);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x74 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_MINIMAP;
        CHECK(others == 0);
    }
    /* F10 is Cheats; Windows sends it as a system key and opens the menu bar on its key-up unless that is kept. */
    for (int mods = 0; mods < 4; mods++) {
        CHECK(key_policy(0x104, 0x79, 0, mods & 1, mods >> 1) == KEY_HOST_CHEATS);
        CHECK(key_policy(0x100, 0x79, 0, mods & 1, mods >> 1) == KEY_HOST_CHEATS);
        CHECK(key_policy(0x104, 0x79, 1, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x105, 0x79, 0, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x79, 0, mods & 1, mods >> 1) == KEY_DROP);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x79 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_CHEATS;
        CHECK(others == 0);
    }
    /* F3 opens and closes Objectives, once per press; the game's key table gives it no action. */
    for (int mods = 0; mods < 4; mods++) {
        CHECK(key_policy(0x100, 0x72, 0, mods & 1, mods >> 1) == KEY_HOST_OBJECTIVES);
        CHECK(key_policy(0x104, 0x72, 0, mods & 1, mods >> 1) == KEY_HOST_OBJECTIVES);
        CHECK(key_policy(0x100, 0x72, 1, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x72, 0, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x100, 0x41, 0, mods & 1, mods >> 1) == KEY_TO_GAME);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x72 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_OBJECTIVES;
        CHECK(others == 0);
    }
    CHECK(key_policy(0x100, 0x78, 0, 0, 0) == KEY_HOST_SNAPSHOT && key_policy(0x100, 0x78, 1, 0, 0) == KEY_DROP &&
          key_policy(0x101, 0x78, 0, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x7A, 0, 1, 0) == KEY_DROP);
    CHECK(key_policy(0x104, 0x73, 0, 0, 1) == KEY_HOST_CLOSE);
    CHECK(key_policy(0x104, 0x73, 1, 0, 1) == KEY_DROP);
    CHECK(key_policy(0x105, 0x73, 0, 0, 1) == KEY_DROP);
    /* F4 alone is Show Objective, once per press; nothing of it reaches the game (its key table gives F4 no action). */
    for (int ctrl = 0; ctrl < 2; ctrl++) {
        CHECK(key_policy(0x100, 0x73, 0, ctrl, 0) == KEY_HOST_SHOW_OBJECTIVE);
        CHECK(key_policy(0x100, 0x73, 1, ctrl, 0) == KEY_DROP);
        CHECK(key_policy(0x101, 0x73, 0, ctrl, 0) == KEY_DROP);
        CHECK(key_policy(0x104, 0x73, 0, ctrl, 0) == KEY_DROP);
        CHECK(key_policy(0x104, 0x73, 0, ctrl, 1) == KEY_HOST_CLOSE);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x73 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_SHOW_OBJECTIVE;
        CHECK(others == 0);
    }
    CHECK(key_policy(0x100, 0x1B, 0, 0, 0) == KEY_TO_GAME && key_policy(0x101, 0x1B, 0, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x100, 0x1B, 1, 0, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x70, 0, 0, 0) == KEY_TO_GAME && key_policy(0x100, 0x70, 1, 0, 0) == KEY_DROP);
    /* F2 is the host's controls card: one toggle per press, nothing reaches the game (its key table gives F2 no
     * action); F12 no longer reaches the game's controls window. */
    for (int mods = 0; mods < 4; mods++) {
        CHECK(key_policy(0x100, 0x71, 0, mods & 1, mods >> 1) == KEY_HOST_CONTROLS);
        CHECK(key_policy(0x104, 0x71, 0, mods & 1, mods >> 1) == KEY_HOST_CONTROLS);
        CHECK(key_policy(0x100, 0x71, 1, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x71, 0, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x100, 0x7B, 0, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x7B, 0, mods & 1, mods >> 1) == KEY_DROP);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x71 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_CONTROLS;
        CHECK(others == 0);
    }
    /* F6 is the frame counter: one toggle per press, nothing reaches the game, no other key does it; F2 keeps the card */
    for (int mods = 0; mods < 4; mods++) {
        CHECK(key_policy(0x100, 0x75, 0, mods & 1, mods >> 1) == KEY_HOST_FRAME_COUNTER);
        CHECK(key_policy(0x104, 0x75, 0, mods & 1, mods >> 1) == KEY_HOST_FRAME_COUNTER);
        CHECK(key_policy(0x100, 0x75, 1, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x101, 0x75, 0, mods & 1, mods >> 1) == KEY_DROP);
        CHECK(key_policy(0x100, 0x71, 0, mods & 1, mods >> 1) == KEY_HOST_CONTROLS);
    }
    {
        int others = 0;
        for (unsigned vk = 0; vk < 256; vk++)
            for (unsigned msg = 0x100; msg <= 0x105; msg++)
                others += vk != 0x75 && key_policy(msg, vk, 0, 0, 0) == KEY_HOST_FRAME_COUNTER;
        CHECK(others == 0);
    }
    /* a quick key the launcher bound to F6 keeps it: the choice made in the launcher wins over the port's own use */
    key_policy_set_quick_keys(0x75, 0x77);
    CHECK(key_policy(0x100, 0x75, 0, 0, 0) == KEY_HOST_QUICK_SAVE && key_policy(0x101, 0x75, 0, 0, 0) == KEY_DROP);
    key_policy_set_quick_keys(0x76, 0x77);
    CHECK(key_policy(0x100, 0x75, 0, 0, 0) == KEY_HOST_FRAME_COUNTER);
    /* F1, F5, F11, Esc and Alt+F4 keep their actions; Ctrl+F11 is dropped */
    CHECK(key_policy(0x100, 0x70, 0, 0, 0) == KEY_TO_GAME && key_policy(0x100, 0x74, 0, 0, 0) == KEY_HOST_MINIMAP);
    CHECK(key_policy(0x100, 0x7A, 0, 0, 0) == KEY_HOST_FULLSCREEN && key_policy(0x100, 0x7A, 0, 1, 0) == KEY_DROP &&
          key_policy(0x101, 0x7A, 0, 1, 0) == KEY_DROP);
    CHECK(key_policy(0x100, 0x1B, 0, 0, 0) == KEY_TO_GAME && key_policy(0x104, 0x73, 0, 0, 1) == KEY_HOST_CLOSE);
    CHECK(key_policy(0x100, 0x57, 1, 0, 0) == KEY_TO_GAME);
    CHECK(key_policy(0x0102, 0x1B, 0, 0, 0) == KEY_TO_GAME);
}

/* The mouse's relative motion and the right stick's counts land in the same cursor, only while gameplay holds it. */
static void test_pointer_relative(void) {
    long x = 400, y = 300;
    CHECK(pointer_add_relative(INPUT_GAMEPLAY_CAPTURED, &x, &y, 20, -3) && x == 420 && y == 297);
    CHECK(!pointer_add_relative(INPUT_GAMEPLAY_CAPTURED, &x, &y, 0, 0) && x == 420 && y == 297);
    for (int st = 0; st < INPUT_STATE_COUNT; st++)
        if (st != INPUT_GAMEPLAY_CAPTURED)
            CHECK(!pointer_add_relative((input_state_t)st, &x, &y, 5, 5) && x == 420 && y == 297);
}

/* The game takes its pointer back from a dialog or menu: the virtual cursor is the centre CInput recentres to. */
static void test_pointer_recapture(void) {
    static const input_state_t real[] = { INPUT_MENU, INPUT_DIALOG };
    static const input_state_t virtual_states[] = { INPUT_GAMEPLAY_CAPTURED, INPUT_GAMEPLAY_RELEASED, INPUT_MOVING_WINDOW,
                                                     INPUT_FULLSCREEN_TRANSITION, INPUT_SHUTDOWN };
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) {
        long x = 11, y = 22;
        CHECK(pointer_recapture(real[i], INPUT_GAMEPLAY_CAPTURED, &x, &y, 2560, 1920) && x == 1280 && y == 960);
        x = 11;
        y = 22;
        for (size_t j = 0; j < sizeof virtual_states / sizeof virtual_states[0]; j++)
            if (virtual_states[j] != INPUT_GAMEPLAY_CAPTURED)
                CHECK(!pointer_recapture(real[i], virtual_states[j], &x, &y, 2560, 1920) && x == 11 && y == 22);
        CHECK(!pointer_recapture(real[i], INPUT_DIALOG, &x, &y, 2560, 1920) && !pointer_recapture(real[i], INPUT_MENU, &x, &y, 2560, 1920));
    }
    for (size_t j = 0; j < sizeof virtual_states / sizeof virtual_states[0]; j++) {
        long x = 11, y = 22;
        CHECK(!pointer_recapture(virtual_states[j], INPUT_GAMEPLAY_CAPTURED, &x, &y, 2560, 1920) && x == 11 && y == 22);
    }
    {
        long x = 11, y = 22;
        CHECK(!pointer_recapture(INPUT_DIALOG, INPUT_GAMEPLAY_CAPTURED, &x, &y, 0, 1920) && !pointer_recapture(INPUT_DIALOG, INPUT_GAMEPLAY_CAPTURED, &x, &y, 2560, -1));
        CHECK(pointer_recapture(INPUT_DIALOG, INPUT_GAMEPLAY_CAPTURED, &x, &y, 1, 1) && x == 0 && y == 0);
    }
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
    /* F1 behaves the same; other keys (F2 and F12 among them) and other key-ups are untouched. */
    open_press_keydown(&p, 0x70, 0, 1, 1);
    CHECK(!open_press_keyup(&p, 0x1B, 2) && open_press_keyup(&p, 0x70, 2));
    open_press_keydown(&p, 0x70, 0, 1, 1);
    open_press_keydown(&p, 0x57, 0, 1, 1);
    open_press_keydown(&p, 0x71, 0, 1, 1);
    open_press_keydown(&p, 0x7B, 0, 1, 1);
    CHECK(!open_press_keyup(&p, 0x71, 2) && !open_press_keyup(&p, 0x7B, 2) && open_press_keyup(&p, 0x70, 2));
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

static void test_skip_intro(void) {
    CHECK(intro_video_of("menu\\tpassintro") == INTRO_STARTUP);
    CHECK(intro_video_of("menu\\newgame") == INTRO_NEW_GAME);
    CHECK(intro_video_of("data/menu/NEWGAME.SMK") == INTRO_NEW_GAME);
    CHECK(intro_video_of("tpassintro.smk") == INTRO_STARTUP);
    CHECK(intro_video_of("menu\\credits") == INTRO_NONE);
    CHECK(intro_video_of("menu\\win") == INTRO_NONE);
    CHECK(intro_video_of("menu\\newgame2") == INTRO_NONE);
    CHECK(intro_video_of("menu\\tpassintro.bak") == INTRO_NONE);
    CHECK(intro_video_of("") == INTRO_NONE && intro_video_of(NULL) == INTRO_NONE);
    /* Off changes nothing. */
    CHECK(!skip_intro_applies(0, INTRO_STARTUP, 0) && !skip_intro_applies(0, INTRO_NEW_GAME, 0));
    CHECK(skip_intro_applies(1, INTRO_STARTUP, 0) && skip_intro_applies(1, INTRO_NEW_GAME, 0));
    /* The credits, the ending and the Video Test's requests still play. */
    CHECK(!skip_intro_applies(1, intro_video_of("menu\\credits"), 0));
    CHECK(!skip_intro_applies(1, intro_video_of("menu\\win"), 0));
    CHECK(!skip_intro_applies(1, INTRO_STARTUP, 1) && !skip_intro_applies(1, INTRO_NEW_GAME, 1));
    CHECK(!strcmp(intro_video_name(INTRO_STARTUP), "tpassintro.smk"));
    CHECK(!strcmp(intro_video_name(INTRO_NEW_GAME), "newgame.smk"));
}

/* The crouch held at 0.072 (log 057): the lower clamp, 0.09 * (h - 0.2), taken in double leaves the height stored as a
 * float below its own bound, so it fires every 10 ms step and zeroes the speed that would lift the player. */
static float crouch_then_stand(int at_24_bits) {
    const float h = 1.0f, dt = 0.01f;
    float pos = h - 0.2f, vel = 0.0f;
    for (int step = 0; step < 350; step++) {
        float knees = step < 150 ? 0.1f * (h - 0.2f) : h - 0.2f, acc = 50.0f * (knees - pos) - 10.0f * vel;
        if (at_24_bits) {
            pelvis_mouth_t m = pelvis_mouth_clamps(pos, vel, h, 0.0f, 0.2, 0.09, 1.1);
            pos = m.pos;
            vel = m.vel;
        } else {
            double lowest = ((double)h - 0.2) * 0.09, highest = ((double)h - 0.2) * 1.1;
            if (!((double)pos >= lowest)) pos = (float)lowest, vel = 0.0f;
            if ((double)pos > highest) pos = (float)highest, vel = 0.0f;
        }
        pos += dt * vel;
        vel += dt * acc;
    }
    return pos;
}

static void test_pelvis_clamps(void) {
    pelvis_mouth_t m;
    float lowest = (float)((double)(float)(1.0 - 0.2) * 0.09);
    CHECK(crouch_then_stand(0) == (float)(0.8 * 0.09)); /* the lifted double: held at the bound */
    CHECK(fabsf(crouch_then_stand(1) - 0.8f) < 0.01f);  /* the game's single precision: back up */
    /* a height already held there is clamped once more, to the single bound, and then rises */
    m = pelvis_mouth_clamps((float)(0.8 * 0.09), 0.33f, 1.0f, 0.0f, 0.2, 0.09, 1.1);
    CHECK(m.pos == lowest && m.vel == 0.0f && lowest > (float)(0.8 * 0.09));
    m = pelvis_mouth_clamps(lowest, 0.33f, 1.0f, 0.0f, 0.2, 0.09, 1.1);
    CHECK(m.pos == lowest && m.vel == 0.33f);
    /* the top bound, NaN, and the ceiling check after them */
    m = pelvis_mouth_clamps(0.95f, 0.5f, 1.0f, 0.0f, 0.2, 0.09, 1.1);
    CHECK(m.pos == (float)(0.8f * 1.1) && m.vel == 0.0f);
    m = pelvis_mouth_clamps(m.pos, 0.2f, 1.0f, 0.0f, 0.2, 0.09, 1.1);
    CHECK(m.vel == 0.2f);
    m = pelvis_mouth_clamps(sqrtf(-1.0f), 0.5f, 1.0f, 0.0f, 0.2, 0.09, 1.1);
    CHECK(m.pos == lowest && m.vel == 0.0f);
    m = pelvis_mouth_clamps(0.5f, 0.3f, 1.0f, -1.0f, 0.2, 0.09, 1.1);
    CHECK(m.pos == 0.5f && m.vel == 0.0f);
    m = pelvis_mouth_clamps(0.5f, -0.3f, 1.0f, -1.0f, 0.2, 0.09, 1.1);
    CHECK(m.vel == -0.3f);
    /* every human height in 0.8..3 m: a clamped height never clamps again at single precision */
    for (int cm = 80; cm <= 300; cm++) {
        float h = cm / 100.0f;
        pelvis_mouth_t low = pelvis_mouth_clamps(-1.0f, 0.0f, h, 0.0f, 0.2, 0.09, 1.1);
        pelvis_mouth_t high = pelvis_mouth_clamps(10.0f, 0.0f, h, 0.0f, 0.2, 0.09, 1.1);
        CHECK(pelvis_mouth_clamps(low.pos, 1.0f, h, 0.0f, 0.2, 0.09, 1.1).vel == 1.0f);
        CHECK(pelvis_mouth_clamps(high.pos, -1.0f, h, 0.0f, 0.2, 0.09, 1.1).vel == -1.0f);
    }
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

/* What a batch is drawn with follows the device's states, and a change of any state in it draws the open batch first. */
static void test_draw_constants(void) {
    enum { SHADE = 9, ALPHATEST = 15, MAPBLEND = 21, ALPHAREF = 24, ALPHAFUNC = 25, FOG = 28, SPECULAR = 29, FOGCOLOR = 34 };
    static const unsigned flushes[] = { SHADE, ALPHATEST, MAPBLEND, ALPHAREF, ALPHAFUNC, FOG, SPECULAR, FOGCOLOR };
    uint32_t render[256] = { 0 };
    d3dt_draw_constants_t off, on, c;
    render[SHADE] = 2;
    d3dt_draw_constants(render, &off);
    CHECK(off.flags == 0 && off.fog[3] == 1.0f && sizeof off == 32);
    render[FOG] = 1;
    render[FOGCOLOR] = 0xFF336699u;
    d3dt_draw_constants(render, &on);
    CHECK(on.flags == D3D11_PS_FOG && on.fog[0] == 0x33 / 255.0f && on.fog[1] == 0x66 / 255.0f && on.fog[2] == 0x99 / 255.0f);
    render[FOGCOLOR] = 0x00010203u;
    d3dt_draw_constants(render, &c);
    CHECK(c.flags == on.flags && c.fog[0] == 1 / 255.0f && memcmp(&c, &on, sizeof c));
    render[FOG] = 0;
    d3dt_draw_constants(render, &c);
    CHECK(c.flags == 0);
    render[SPECULAR] = 1, render[ALPHATEST] = 1, render[SHADE] = 1, render[MAPBLEND] = 2, render[ALPHAFUNC] = 5, render[ALPHAREF] = 0x10FF;
    d3dt_draw_constants(render, &c);
    CHECK(c.flags == (D3D11_PS_SPECULAR | D3D11_PS_ALPHA_TEST | D3D11_PS_FLAT) && c.blend_mode == 2 && c.alpha_func == 5 && c.alpha_ref == 1.0f);
    for (size_t i = 0; i < sizeof flushes / sizeof flushes[0]; i++)
        CHECK(d3dt_render_change_flushes(flushes[i], 0, 1) && !d3dt_render_state_keeps_batch(flushes[i]));
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

/* The alpha-weighted downsample as it was first written, with divisions and a loop over the footprint. */
static void reference_downsample(const uint32_t* src, unsigned w, unsigned h, int kind, uint32_t* dst) {
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
            if (kind == D3DT_ALPHA_OPAQUE) dst[(size_t)y * dw + x] = 255u << 24 | r << 16 | g << 8 | b;
            else if (kind == D3DT_ALPHA_BLENDED) dst[(size_t)y * dw + x] = ((sa + n / 2) / n) << 24 | r << 16 | g << 8 | b;
            else dst[(size_t)y * dw + x] = opaque << 24 | r << 16 | g << 8 | b, hist[opaque]++;
        }
    if (kind != D3DT_ALPHA_BINARY) return;
    for (unsigned y = 0; y < dh * fy; y++)
        for (unsigned x = 0; x < dw * fx; x++) covered += (src[(size_t)y * w + x] >> 24) >= 128;
    reference_keep_coverage(dst, dw, dh, n, hist, (unsigned long)(((double)covered / n) + 0.5));
}

/* The GPU path of the perspective-textured primitive: the span's row loop, the planes it is replaced by and the
 * texel index its pixel shader computes. */
static void test_texture_gpu(void) {
    tgpu_steps_t d = { 0x18000, 0x6000, { 1.0f, 2.0f, 0.5f }, { 0.25f, -0.5f, 0.125f }, 640 };
    tgpu_scan_t s = { 10 << 16, 0xF000, 3.0f, 4.0f, 2.0f };
    tgpu_rows_t r = { 5, 9, 3200, 20 << 16, 1 << 16 };
    int32_t x0, x1;
    CHECK(tgpu_row_span(&s, &r, &x0, &x1) && x0 == 10 && x1 == 30);
    tgpu_next_row(&s, &r, &d);
    /* 0xF000 + 0x6000 passes 0x10000: the remainder wraps and the carry extra is added */
    CHECK(s.rem == 0x5000 && s.fx == (10 << 16) + 0x18000 && r.y == 6 && r.line == 3840 && r.len == 21 << 16);
    CHECK(s.uz == 4.25f && s.vz == 5.5f && s.wz == 2.625f);
    tgpu_next_row(&s, &r, &d);
    CHECK(s.rem == 0xB000 && s.uz == 5.25f && s.vz == 7.5f && s.wz == 3.125f);
    s.rem = 0xA000;
    tgpu_next_row(&s, &r, &d);
    CHECK(s.rem == 0x10000 && s.uz == 6.25f);
    d.rem_inc = -0x2000;
    s.rem = 0x1000;
    tgpu_next_row(&s, &r, &d);
    CHECK(s.rem == 0xF000 && s.uz == 7.5f && s.vz == 11.0f);
    r.len = 0;
    CHECK(!tgpu_row_span(&s, &r, &x0, &x1));

    {
        uint32_t seed = 99u;
        int ok = 1;
        for (int k = 0; k < 500; k++) {
            float a[3], gx[3], gy[3], s0[3], s1[3];
            float xa, ya, xb, yb;
            tgpu_plane_t pl;
            for (int i = 0; i < 3; i++) {
                seed = seed * 1664525u + 1013904223u;
                a[i] = (float)(seed >> 8) / 65536.0f;
                seed = seed * 1664525u + 1013904223u;
                gx[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.1f;
                seed = seed * 1664525u + 1013904223u;
                gy[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.1f;
            }
            xa = (float)(k % 300);
            ya = (float)(k % 200);
            xb = xa + (float)(k % 7) - 3.0f;
            yb = ya + 1.0f + (float)(k % 3);
            for (int i = 0; i < 3; i++) {
                s0[i] = a[i] + gx[i] * xa + gy[i] * ya;
                s1[i] = a[i] + gx[i] * xb + gy[i] * yb;
            }
            tgpu_plane(&pl, xa, ya, s0, xb, yb, s1, gx);
            for (int i = 0; i < 3; i++) {
                const float* q = i == 0 ? pl.u : i == 1 ? pl.v : pl.w;
                float px = xa + 37.0f, py = ya + 11.0f;
                float want = a[i] + gx[i] * px + gy[i] * py, got = q[0] + q[1] * (px - pl.origin_x) + q[2] * (py - pl.origin_y);
                ok &= fabsf(want - got) < 1e-3f * (1.0f + fabsf(want));
            }
        }
        CHECK(ok);
    }
    {
        tgpu_plane_t pl;
        float a[3] = { 1, 2, 3 }, gx[3] = { 0.5f, 0.25f, 0.0f };
        tgpu_plane(&pl, 4, 7, a, 9, 7, a, gx);
        CHECK(pl.u[2] == 0.0f && pl.u[1] == 0.5f && pl.origin_x == 4.0f && pl.origin_y == 7.0f);
    }
    {
        tgpu_texture_t c = { 1, 0.2f, 63.8f, 31.8f, 64, 0xFFFFFFFFu, 0xFFFFFFFFu };
        tgpu_texture_t w = { 0, 0.2f, 0.0f, 0.0f, 512, 63, (31u << 9) | 63u };
        CHECK(tgpu_texel_index(&c, 5.7f, 2.3f) == 2u * 64u + 5u);
        CHECK(tgpu_texel_index(&c, -4.0f, -9.0f) == 0u);
        CHECK(tgpu_texel_index(&c, 900.0f, 900.0f) == 31u * 64u + 63u);
        CHECK(tgpu_texture_bytes(&c) == 31u * 64u + 63u + 1u);
        CHECK(tgpu_texel_index(&c, 900.0f, 900.0f) < tgpu_texture_bytes(&c));
        CHECK(tgpu_texel_index(&w, 65.5f, 2.0f) == (2u << 9) + 1u);
        CHECK(tgpu_texel_index(&w, -1.0f, -1.0f) == ((31u << 9) | 63u));
        CHECK(tgpu_texel_index(&w, 63.0f, 32.0f) == 63u);
        CHECK(tgpu_texture_bytes(&w) == ((31u << 9) | 63u) + 1u);
        w.tile_mask = 0xFFFFFFFFu;
        CHECK(tgpu_texture_bytes(&w) == 0u);
        c.vmax = 1e6f;
        CHECK(tgpu_texture_bytes(&c) == 0u);
        c.vmax = 31.8f;
        c.tile_mask = 0x7FFu;
        CHECK(tgpu_texture_bytes(&c) == 0x800u);
        c.tile_mask = 0xFFFFFFu;
        CHECK(tgpu_texture_bytes(&c) == 31u * 64u + 63u + 1u);
        c.clamped = 0;
        CHECK(tgpu_texture_bytes(&c) == 0u);
        w.tile_mask = (3u << 9) | 63u;
        CHECK(tgpu_texture_bytes(&w) == ((3u << 9) | 63u) + 1u);
    }
}

/* The span's row epilogue at the flag level: add eax, fx_inc; add ebx, rem_inc; jge (SF == OF); the negative branch
 * adds 0x10000, the other compares with 0x10000; a carry adds the extras before the three fstp. */
static void reference_next_row(tgpu_scan_t* s, tgpu_rows_t* r, const tgpu_steps_t* d) {
    uint32_t ebx = (uint32_t)s->rem + (uint32_t)d->rem_inc;
    int sf = (int)(ebx >> 31), of = (int)((((uint32_t)s->rem ^ ebx) & ((uint32_t)d->rem_inc ^ ebx)) >> 31), carry = 0;
    double st[3] = { s->uz, s->vz, s->wz };
    if (sf != of) {
        ebx += 0x10000u;
        carry = 1;
    } else if ((int32_t)ebx > 0x10000) {
        ebx -= 0x10000u;
        carry = 1;
    }
    for (int i = 0; i < 3; i++) {
        st[i] += d->step[i];
        if (carry) st[i] += d->carry[i];
    }
    s->fx = (int32_t)((uint32_t)s->fx + (uint32_t)d->fx_inc);
    s->rem = (int32_t)ebx;
    s->uz = (float)st[0];
    s->vz = (float)st[1];
    s->wz = (float)st[2];
    r->len = (int32_t)((uint32_t)r->len + (uint32_t)r->dlen);
    r->line = (int32_t)((uint32_t)r->line + (uint32_t)d->line_pixels);
    r->y++;
}

/* Only what the native span writes back changes: pscan's edge and u/z, v/z, 1/z, pdtri's y, line and length. */
static void test_texture_gpu_rows(void) {
    uint32_t seed = 7u;
    int same = 1, untouched = 1;
    for (int k = 0; k < 20000; k++) {
        tgpu_scan_t a, b;
        tgpu_rows_t ra, rb;
        tgpu_steps_t d, d0;
        float f[9];
        for (int i = 0; i < 9; i++) {
            seed = seed * 1664525u + 1013904223u;
            f[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * (i < 3 ? 4096.0f : 8.0f);
        }
        seed = seed * 1664525u + 1013904223u;
        a.fx = (int32_t)(seed & 0x0FFFFFFFu);
        seed = seed * 1664525u + 1013904223u;
        a.rem = k % 7 == 0 ? (int32_t)seed : (int32_t)(seed % 0x10001u);
        a.uz = f[0], a.vz = f[1], a.wz = f[2];
        seed = seed * 1664525u + 1013904223u;
        d.fx_inc = (int32_t)(seed % 0x40000u) - 0x20000;
        seed = seed * 1664525u + 1013904223u;
        d.rem_inc = k % 5 == 0 ? (int32_t)seed : (int32_t)(seed % 0x20000u) - 0x10000;
        d.step[0] = f[3], d.step[1] = f[4], d.step[2] = f[5];
        d.carry[0] = f[6], d.carry[1] = f[7], d.carry[2] = f[8];
        d.line_pixels = 3840 + k % 3;
        ra.y = k % 2160;
        ra.y_end = ra.y + 1 + k % 9;
        ra.line = ra.y * 3840;
        seed = seed * 1664525u + 1013904223u;
        ra.len = (int32_t)(seed % 0x2000000u);
        ra.dlen = (int32_t)(seed % 0x40000u) - 0x20000;
        b = a;
        rb = ra;
        d0 = d;
        do {
            tgpu_next_row(&a, &ra, &d);
            reference_next_row(&b, &rb, &d);
        } while (ra.y < ra.y_end);
        same &= a.fx == b.fx && a.rem == b.rem && !memcmp(&a.uz, &b.uz, sizeof(float)) &&
                !memcmp(&a.vz, &b.vz, sizeof(float)) && !memcmp(&a.wz, &b.wz, sizeof(float)) && ra.y == rb.y &&
                ra.line == rb.line && ra.len == rb.len;
        untouched &= ra.y_end == rb.y_end && ra.dlen == rb.dlen && !memcmp(&d, &d0, sizeof d);
    }
    CHECK(same);
    CHECK(untouched);
}

/* A polygon is GPU or CPU from its first Lock on; an unforeseen state after the GPU choice turns the path off once. */
static void test_texture_gpu_session(void) {
    tgpu_session_t s;
    tgpu_session_init(&s, 0);
    CHECK(tgpu_polygon_begin(&s) == TGPU_OFF);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_NONE) == TGPU_OFF);
    CHECK(!tgpu_polygon_end(&s, 1) && s.gpu == 0 && s.cpu == 0);

    tgpu_session_init(&s, 1);
    CHECK(tgpu_polygon_begin(&s) == TGPU_PENDING);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_TEXTURE_UNBOUNDED) == TGPU_CPU);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_NONE) == TGPU_CPU);
    tgpu_polygon_unsafe(&s);
    CHECK(!s.unsafe);
    CHECK(!tgpu_polygon_end(&s, 0) && s.cpu == 1 && s.enabled);
    CHECK(s.reason[TGPU_REASON_TEXTURE_UNBOUNDED] == 1 && s.reason[TGPU_REASON_MEMORY_RASTER] == 0);

    /* never Locked: a memory raster */
    CHECK(tgpu_polygon_begin(&s) == TGPU_PENDING);
    CHECK(!tgpu_polygon_end(&s, 0) && s.cpu == 2 && s.mode == TGPU_OFF);
    CHECK(s.reason[TGPU_REASON_MEMORY_RASTER] == 1);

    CHECK(tgpu_polygon_begin(&s) == TGPU_PENDING);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_NONE) == TGPU_GPU);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_RASTER_LOCKED) == TGPU_GPU);
    CHECK(!tgpu_polygon_end(&s, 1) && s.gpu == 1 && s.enabled && s.reason[TGPU_REASON_RASTER_LOCKED] == 0);

    CHECK(tgpu_polygon_begin(&s) == TGPU_PENDING);
    CHECK(tgpu_polygon_lock(&s, TGPU_REASON_NONE) == TGPU_GPU);
    tgpu_polygon_unsafe(&s);
    CHECK(tgpu_polygon_end(&s, 1) && s.dropped == 1 && s.gpu == 1 && !s.enabled && s.disabled_after_failure);
    CHECK(tgpu_polygon_begin(&s) == TGPU_OFF);
    CHECK(!tgpu_polygon_end(&s, 0) && s.dropped == 1);

    tgpu_session_init(&s, 1);
    tgpu_polygon_begin(&s);
    tgpu_polygon_lock(&s, TGPU_REASON_NONE);
    CHECK(tgpu_polygon_end(&s, 0) && s.dropped == 1 && !s.enabled);

    tgpu_session_init(&s, 1);
    for (unsigned r = 1; r < TGPU_REASON_COUNT; r++) {
        tgpu_polygon_begin(&s);
        tgpu_polygon_lock(&s, (tgpu_reason_t)r);
        tgpu_polygon_end(&s, 0);
    }
    tgpu_polygon_begin(&s);
    tgpu_polygon_lock(&s, (tgpu_reason_t)(TGPU_REASON_COUNT + 5));
    tgpu_polygon_end(&s, 0);
    CHECK(s.cpu == TGPU_REASON_COUNT && s.reason[TGPU_REASON_OTHER] == 2 && s.reason[TGPU_REASON_RASTER_LOCKED] == 1);
    CHECK(!strcmp(tgpu_reason_name(TGPU_REASON_RASTER_LOCKED), "raster_locked") &&
          !strcmp(tgpu_reason_name(TGPU_REASON_MEMORY_RASTER), "memory_raster") &&
          !strcmp(tgpu_reason_name((tgpu_reason_t)99), "other"));
}

/* A screen the game holds locked no longer keeps its polygons on the CPU. */
static void test_texture_gpu_run(void) {
    /* locked, run_active, lock_untouched, run_captured, overlaps */
    CHECK(tgpu_run_action(0, 0, 0, 0, 0) == TGPU_RUN_FRESH);
    CHECK(tgpu_run_action(0, 1, 1, 1, 1) == TGPU_RUN_FRESH);
    CHECK(tgpu_run_action(1, 1, 0, 1, 0) == TGPU_RUN_KEEP);
    CHECK(tgpu_run_action(1, 1, 0, 1, 1) == TGPU_RUN_SPLIT);
    CHECK(tgpu_run_action(1, 1, 0, 0, 0) == TGPU_RUN_SPLIT);
    CHECK(tgpu_run_action(1, 1, 1, 0, 1) == TGPU_RUN_SPLIT);
    CHECK(tgpu_run_action(1, 0, 1, 0, 0) == TGPU_RUN_ADOPT);
    CHECK(tgpu_run_action(1, 0, 1, 1, 1) == TGPU_RUN_ADOPT);
    CHECK(tgpu_run_action(1, 0, 0, 1, 0) == TGPU_RUN_BUSY);
    CHECK(tgpu_run_action(1, 0, 0, 0, 1) == TGPU_RUN_BUSY);
}

static tgpu_texture_cache_t s_tex_cache;
static tgpu_clut_cache_t s_clut_cache;

/* Device and level changes empty the caches; one address may hold another texture or CLUT afterwards. */
static void test_texture_gpu_cache(void) {
    static uint8_t tex[2][4096];
    static uint16_t clut[3][TGPU_CLUT_ENTRIES];
    int up;
    int64_t a, b, c;
    uint32_t s0, s1;
    for (int i = 0; i < 4096; i++) tex[0][i] = (uint8_t)i, tex[1][i] = (uint8_t)(i * 7);
    for (unsigned i = 0; i < TGPU_CLUT_ENTRIES; i++) clut[0][i] = (uint16_t)i, clut[1][i] = (uint16_t)(i ^ 0xF81F),
        clut[2][i] = (uint16_t)(i * 3);
    tgpu_texture_cache_reset(&s_tex_cache, 1u << 16);
    tgpu_clut_cache_reset(&s_clut_cache);

    a = tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 1, &up);
    CHECK(a == 0 && up);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 1, &up) == a && !up);
    tex[0][100] ^= 0xFF;
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 1, &up) == a && !up);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 2, &up) == a && up);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 3, &up) == a && !up);
    b = tgpu_texture_cache_find(&s_tex_cache, tex[0], 2048, 3, &up);
    CHECK(b >= 0 && b != a && up);
    c = tgpu_texture_cache_find(&s_tex_cache, tex[1], 4096, 3, &up);
    CHECK(c >= 0 && c != a && c != b && up && (c & 15) == 0);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[1], 0, 3, &up) == -1 && !up);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[1], (1u << 16) / 4u + 1u, 3, &up) == -1 && !up);
    for (int i = 0; i < 20; i++) tgpu_texture_cache_find(&s_tex_cache, tex[0] + 16 * (i + 1), 4096, 3, &up);
    CHECK(s_tex_cache.top <= s_tex_cache.pool);

    tgpu_texture_cache_reset(&s_tex_cache, 1u << 16);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 3, &up) == 0 && up);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[1], 4096, 3, &up) == 4096 && up);
    memcpy(tex[0], tex[1], sizeof tex[0]);
    tgpu_texture_cache_reset(&s_tex_cache, 1u << 16);
    CHECK(tgpu_texture_cache_find(&s_tex_cache, tex[0], 4096, 3, &up) == 0 && up);

    s0 = tgpu_clut_cache_find(&s_clut_cache, clut[0], &up);
    CHECK(up && s0 < TGPU_CLUT_SLOTS);
    CHECK(tgpu_clut_cache_find(&s_clut_cache, clut[0], &up) == s0 && !up);
    s1 = tgpu_clut_cache_find(&s_clut_cache, clut[1], &up);
    CHECK(up && s1 != s0);
    memcpy(clut[0], clut[2], sizeof clut[0]);
    CHECK(tgpu_clut_cache_find(&s_clut_cache, clut[0], &up) == s0 && up);
    CHECK(s_clut_cache.slot[s0].copy[5] == 15);
    CHECK(tgpu_clut_cache_find(&s_clut_cache, clut[1], &up) == s1 && !up);
    tgpu_clut_cache_reset(&s_clut_cache);
    CHECK(tgpu_clut_cache_find(&s_clut_cache, clut[1], &up) == s1 && up);
}

/* The bump perspective span's pixel as its inner loop computes it, against the model the linear bump span is proven by,
 * and what the GPU path derives from the table. */
static void test_texture_gpu_bump(void) {
    static uint8_t table[TGPU_BUMP_TABLE_BYTES];
    static uint16_t clut[64 * 256];
    static uint8_t cache_a[2][4096];
    uint32_t seed = 31u;
    int same = 1, bounded = 1, differs = 0;
    int up;
    bump_pixel_t c;
    tgpu_texture_cache_t cache;
    for (unsigned i = 0; i < TGPU_BUMP_TABLE_BYTES; i++) table[i] = (uint8_t)((i * 7u) % 23u);
    for (unsigned i = 0; i < sizeof clut / sizeof clut[0]; i++) clut[i] = (uint16_t)(i * 2654435761u >> 11);
    CHECK(tgpu_bump_clut_entries(table) == 23u * 64u);
    table[5] = 255;
    CHECK(tgpu_bump_clut_entries(table) == 256u * 64u);
    table[5] = 0;
    CHECK(tgpu_bump_clut_entries(table) == 23u * 64u);
    c.clut = clut;
    c.clut_base = 0;
    c.bump = table;
    for (int k = 0; k < 100000; k++) {
        uint16_t out;
        uint32_t texel, theta, index;
        seed = seed * 1664525u + 1013904223u;
        texel = seed >> 16;
        theta = (seed >> 4) & 0x3F0u;
        c.light_theta = theta;
        index = tgpu_bump_clut_index(texel, theta, table);
        same &= bump_texel_rgb565(&c, (uint16_t)texel, 0, &out) && out == clut[index];
        bounded &= index < tgpu_bump_clut_entries(table);
        differs |= (tgpu_bump_clut_index(texel, 0, table) != index);
    }
    CHECK(same);
    CHECK(bounded);
    CHECK(differs);
    CHECK(tgpu_bump_clut_index(0xFFFFu, 0x3F0u, table) == (uint32_t)table[(0xFFFFu - 0x3F0u) & 0x3FFu] * 64u + 63u);
    CHECK(tgpu_bump_clut_index(0u, 0x10u, table) == (uint32_t)table[0x3F0u] * 64u);

    /* a table rewritten in place inside a frame is seen at the next use, a texture only at the next frame */
    tgpu_texture_cache_reset(&cache, 1u << 16);
    for (int i = 0; i < 4096; i++) cache_a[0][i] = (uint8_t)i;
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[0], 1024, &up) == 0 && up);
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[0], 1024, &up) == 0 && !up);
    cache_a[0][700] ^= 1;
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[0], 1024, &up) == 0 && up);
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[0], 1024, &up) == 0 && !up);
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[0], 512, &up) > 0 && up);
    CHECK(tgpu_texture_cache_find_checked(&cache, NULL, 512, &up) == -1);
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[1], (1u << 16) / 4u + 1u, &up) == -1);
    tgpu_texture_cache_reset(&cache, 1u << 16);
    CHECK(tgpu_texture_cache_find_checked(&cache, cache_a[1], 1024, &up) == 0 && up);
}

/* The vectorised texture conversion, alpha scan and mip downsample against the per-texel reference, byte for byte. */
static uint32_t s_simd_seed = 12345u;

static uint32_t simd_rand(void) {
    s_simd_seed = s_simd_seed * 1664525u + 1013904223u;
    return s_simd_seed >> 8 ^ s_simd_seed << 16;
}

static void test_texture_simd_convert(void) {
    static const int formats[] = { D3DT_FMT_RGB565, D3DT_FMT_XRGB1555, D3DT_FMT_ARGB1555, D3DT_FMT_ARGB4444,
                                   D3DT_FMT_XRGB4444, D3DT_FMT_XRGB8888, D3DT_FMT_ARGB8888 };
    static uint32_t src[96 + 8], out_a[96 + 8], out_b[96 + 8];
    int bad = 0, edge_bad = 0;
    for (unsigned f = 0; f < sizeof formats / sizeof formats[0]; f++)
        for (int k = 0; k < 100000; k++) {
            unsigned count = simd_rand() % 80, offset = simd_rand() % 4, at = simd_rand() % 3;
            uint32_t low = simd_rand() & 0xFFFFu, high = low + (simd_rand() % 4 == 0 ? 0xFFFFu : simd_rand() % 4096u);
            int keyed = (int)(simd_rand() % 3 != 0), bytes = formats[f] >= D3DT_FMT_XRGB8888 && formats[f] != D3DT_FMT_XRGB4444 ? 4 : 2;
            const uint8_t* in = (const uint8_t*)src + offset * (unsigned)bytes;
            for (unsigned i = 0; i < 100; i++) src[i] = simd_rand() ^ (simd_rand() << 16);
            if (k % 7 == 0)
                for (unsigned i = 0; i < 100; i++) src[i] = (i % 2) ? 0xFFFFFFFFu : 0u;
            if (k % 11 == 0 && keyed) low = 0, high = 0xFFFFFFFFu;
            if (k % 13 == 0) high = low;
            memset(out_a, 0xA5, sizeof out_a);
            memset(out_b, 0xA5, sizeof out_b);
            d3dt_convert_row_reference(formats[f], in, out_a + at, count, keyed, low, high);
            d3dt_convert_row(formats[f], in, out_b + at, count, keyed, low, high);
            bad |= memcmp(out_a, out_b, sizeof out_a) != 0;
        }
    for (unsigned f = 0; f < 5; f++) {
        uint16_t extremes[8] = { 0x0000, 0xFFFF, 0x8000, 0x7FFF, 0xF800, 0x07E0, 0x001F, 0x0F0F };
        for (unsigned n = 1; n <= 8; n++) {
            uint32_t a[8], b[8];
            d3dt_convert_row_reference(formats[f], extremes, a, n, 1, 0x7FFF, 0xF800);
            d3dt_convert_row(formats[f], extremes, b, n, 1, 0x7FFF, 0xF800);
            edge_bad |= memcmp(a, b, n * 4) != 0;
        }
    }
    CHECK(!bad);
    CHECK(!edge_bad);
}

static void test_texture_simd_alpha(void) {
    static uint32_t px[70];
    int bad = 0;
    for (int k = 0; k < 100000; k++) {
        unsigned count = simd_rand() % 66, at = simd_rand() % 3, mode = (unsigned)k % 4;
        for (unsigned i = 0; i < 70; i++) px[i] = 0xFF000000u | simd_rand();
        if (mode && count) px[simd_rand() % count] = (mode == 1 ? 0u : mode == 2 ? 0x80000000u : 0x01000000u) | (simd_rand() & 0xFFFFFFu);
        if (mode == 3 && count > 4) px[count - 1] = simd_rand() & 0xFFFFFFu;
        bad |= d3dt_alpha_kind(px + at, count) != d3dt_alpha_kind_reference(px + at, count);
    }
    CHECK(!bad);
}

static void test_texture_simd_mips(void) {
    static const unsigned sizes[][2] = { { 256, 256 }, { 128, 128 }, { 64, 64 }, { 32, 32 }, { 16, 16 }, { 8, 8 },
                                         { 256, 128 }, { 128, 256 }, { 64, 128 }, { 32, 8 }, { 8, 16 }, { 1, 1 },
                                         { 1, 8 }, { 8, 1 }, { 2, 2 }, { 3, 5 }, { 5, 3 }, { 7, 9 }, { 12, 6 }, { 4, 4 } };
    static uint32_t src[256 * 256], a[256 * 256 / 3 + 64], b[256 * 256 / 3 + 64];
    int bad = 0, chains_bad = 0;
    for (int k = 0; k < 10000; k++) {
        unsigned s = (unsigned)k % (sizeof sizes / sizeof sizes[0]), w = sizes[s][0], h = sizes[s][1], kind = (unsigned)k / 20 % 3;
        unsigned dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        unsigned long texels = (unsigned long)w * h;
        for (unsigned long i = 0; i < texels; i++) {
            uint32_t c = simd_rand() & 0xFFFFFFu, alpha = kind == 0 ? 255u : kind == 1 ? ((simd_rand() & 3u) ? 255u : 0u) : (simd_rand() & 0xFFu);
            src[i] = (alpha << 24) | c;
        }
        if (k % 5 == 0)
            for (unsigned long i = 0; i < texels; i++) src[i] = kind == 0 ? 0xFF000000u : src[i] | 0x00FFFFFFu;
        memset(a, 0x5A, sizeof a);
        memset(b, 0x5A, sizeof b);
        {
            int ak = d3dt_alpha_kind(src, texels);
            d3dt_mip_downsample_reference(src, w, h, ak, a);
            d3dt_mip_downsample(src, w, h, ak, b);
            bad |= memcmp(a, b, ((size_t)dw * dh + 8) * 4) != 0;
        }
    }
    for (int k = 0; k < 200; k++) {
        unsigned s = (unsigned)k % 12, w = sizes[s][0], h = sizes[s][1], levels = d3dt_mip_count(w, h);
        size_t total = d3dt_mip_chain_texels(w, h, levels), i;
        static uint32_t chain_a[256 * 256 * 2], chain_b[256 * 256 * 2];
        for (i = 0; i < (size_t)w * h; i++) chain_a[i] = chain_b[i] = 0xFF000000u | simd_rand();
        d3dt_build_mips(chain_b, w, h, levels, D3DT_ALPHA_OPAQUE);
        for (unsigned l = 1, lw, lh, off = 0; l < levels; l++) {
            uint32_t* level = chain_a + off;
            d3dt_mip_size(w, h, l - 1, &lw, &lh);
            d3dt_mip_downsample_reference(level, lw, lh, D3DT_ALPHA_OPAQUE, level + (size_t)lw * lh);
            off += lw * lh;
        }
        chains_bad |= memcmp(chain_a, chain_b, total * 4) != 0;
    }
    CHECK(!bad);
    CHECK(!chains_bad);
}

/* Reference against vectorised on a 128x128 texture, printed only. */
static void bench_texture_simd(void) {
    static uint16_t src[128 * 128];
    static uint32_t dst[128 * 128], mip[128 * 128];
    LARGE_INTEGER f, t0, t1, t2, t3, t4;
    double ref_row, simd_row, ref_mip, simd_mip;
    QueryPerformanceFrequency(&f);
    for (unsigned i = 0; i < 128 * 128; i++) src[i] = (uint16_t)simd_rand();
    QueryPerformanceCounter(&t0);
    for (int r = 0; r < 200; r++)
        for (unsigned y = 0; y < 128; y++) d3dt_convert_row_reference(D3DT_FMT_RGB565, src + y * 128, dst + y * 128, 128, 0, 0, 0);
    QueryPerformanceCounter(&t1);
    for (int r = 0; r < 200; r++)
        for (unsigned y = 0; y < 128; y++) d3dt_convert_row(D3DT_FMT_RGB565, src + y * 128, dst + y * 128, 128, 0, 0, 0);
    QueryPerformanceCounter(&t2);
    ref_row = (double)(t1.QuadPart - t0.QuadPart), simd_row = (double)(t2.QuadPart - t1.QuadPart);
    for (unsigned i = 0; i < 128 * 128; i++) dst[i] |= 0xFF000000u;
    QueryPerformanceCounter(&t0);
    for (int r = 0; r < 400; r++) d3dt_mip_downsample_reference(dst, 128, 128, D3DT_ALPHA_OPAQUE, mip);
    QueryPerformanceCounter(&t3);
    for (int r = 0; r < 400; r++) d3dt_mip_downsample(dst, 128, 128, D3DT_ALPHA_OPAQUE, mip);
    QueryPerformanceCounter(&t4);
    ref_mip = (double)(t3.QuadPart - t0.QuadPart), simd_mip = (double)(t4.QuadPart - t3.QuadPart);
    printf("texture simd: RGB565 row conversion x%.1f (%.2f -> %.2f us per 128x128), opaque mip downsample x%.1f "
           "(%.1f -> %.1f us per 128x128)\n", ref_row / simd_row, ref_row * 1e6 / (double)f.QuadPart / 200.0,
           simd_row * 1e6 / (double)f.QuadPart / 200.0, ref_mip / simd_mip, ref_mip * 1e6 / (double)f.QuadPart / 400.0,
           simd_mip * 1e6 / (double)f.QuadPart / 400.0);
}

static void test_fill_runs(void) {
    static uint16_t copy[4 * 96], reference[4 * 96];
    for (long left = 0; left < 12; left++)
        for (long width = 0; width < 70; width++) {
            d3dt_rect_t r = { left, 1, left + width, 3 };
            int ok = 1;
            for (unsigned i = 0; i < 4 * 96; i++) copy[i] = reference[i] = 0x1111;
            tsync_fill((uint8_t*)copy, 96 * 2, reference, 96, &r, 0xF81F);
            for (long y = 0; y < 4; y++)
                for (long x = 0; x < 96; x++) {
                    uint16_t want = y >= 1 && y < 3 && x >= left && x < left + width ? 0xF81F : 0x1111;
                    ok &= copy[y * 96 + x] == want && reference[y * 96 + x] == want;
                }
            CHECK(ok);
        }
}

static void test_overlay_row_sync(void) {
    uint32_t seed = 12345u;
    for (unsigned density = 0; density < 5; density++)
        for (unsigned count = 0; count < 70; count++) {
            uint16_t cur[80], old[80], keep[80], keep_ref[80];
            uint32_t out[80], out_ref[80];
            unsigned long black = 7, black_ref = 7;
            unsigned changed, changed_ref;
            for (unsigned i = 0; i < 80; i++) {
                seed = seed * 1664525u + 1013904223u;
                cur[i] = (uint16_t)(seed >> 8);
                seed = seed * 1664525u + 1013904223u;
                old[i] = ((seed >> 16) % 5u) < density ? (uint16_t)(seed >> 4) : cur[i];
                if ((seed & 7u) == 0) cur[i] = 0;
                keep[i] = keep_ref[i] = old[i];
                out[i] = out_ref[i] = 0xDEADBEEFu;
            }
            changed = d3dt_overlay_row_sync(cur, keep, count, out, &black);
            changed_ref = d3dt_overlay_row(cur, keep_ref, count, out_ref);
            black_ref += d3dt_black_over_content(cur, keep_ref, count);
            memcpy(keep_ref, cur, count * 2u);
            CHECK(changed == changed_ref);
            CHECK(black == black_ref);
            CHECK(!memcmp(out, out_ref, sizeof out));
            CHECK(!memcmp(keep, keep_ref, sizeof keep));
        }
}

static void test_mip_downsample_exact(void) {
    static uint32_t src[256 * 256], fast[128 * 128], slow[128 * 128];
    static const unsigned sizes[][2] = { { 256, 256 }, { 64, 128 }, { 8, 8 }, { 1, 32 }, { 32, 1 }, { 2, 2 }, { 2, 1 } };
    uint32_t seed = 0xC0FFEEu;
    unsigned long mismatches = 0, cases = 0;
    for (int kind = D3DT_ALPHA_OPAQUE; kind <= D3DT_ALPHA_BLENDED; kind++)
        for (int round = 0; round < 24; round++)
            for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
                unsigned w = sizes[s][0], h = sizes[s][1], dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
                for (unsigned i = 0; i < w * h; i++) {
                    uint32_t a;
                    seed = seed * 1664525u + 1013904223u;
                    a = (seed >> 24) & 0xFFu;
                    if (round % 4 == 0) a = 0xFFu;                         /* all opaque */
                    else if (round % 4 == 1) a = (seed >> 20) & 1u ? 0xFFu : 0u; /* binary */
                    else if (round % 4 == 2) a = (seed >> 22) % 5u ? 0xFFu : a;  /* mostly opaque */
                    src[i] = a << 24 | ((seed * 2654435761u) >> 8 & 0xFFFFFFu);
                }
                d3dt_mip_downsample(src, w, h, kind, fast);
                reference_downsample(src, w, h, kind, slow);
                mismatches += memcmp(fast, slow, (size_t)dw * dh * 4) != 0;
                cases++;
            }
    CHECK(cases == 3 * 24 * 7 && mismatches == 0);
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
    /* The impostor (256 px) takes over at 256 px, never magnified. At 1440 lines: 2 m 25 (the minimum), 4 m 39.1,
     * 6 m 58.6, 10 m 97.7, 16 m 150 (the maximum); at 2160 lines 29.3, 58.6, 88.0, 146.6, 150. */
    CHECK(ENGINE_IMPOSTOR_SWITCH_PX == 256.0f && ENGINE_IMPOSTOR_RETURN_PX == 288.0f && ENGINE_FULL_3D_MAX == 150.0f);
    CHECK(ENGINE_FULL_3D_MIN == 25.0f);
    CHECK(engine_full_3d_distance(0.5f, ppm1440, 1.0f) == ENGINE_FULL_3D_MIN);
    CHECK(engine_full_3d_distance(2.0f, ppm1440, 1.0f) == ENGINE_FULL_3D_MIN);
    CHECK(fabs(engine_full_3d_distance(4.0f, ppm1440, 1.0f) - 39.1f) < 0.1f);
    CHECK(fabs(engine_full_3d_distance(6.0f, ppm1440, 1.0f) - 58.6f) < 0.1f);
    CHECK(fabs(engine_full_3d_distance(10.0f, ppm1440, 1.0f) - 97.7f) < 0.1f);
    CHECK(engine_full_3d_distance(16.0f, ppm1440, 1.0f) == ENGINE_FULL_3D_MAX);
    CHECK(fabs(engine_full_3d_distance(2.0f, ppm2160, 1.0f) - 29.3f) < 0.1f);
    CHECK(fabs(engine_full_3d_distance(4.0f, ppm2160, 1.0f) - 58.6f) < 0.1f);
    CHECK(fabs(engine_full_3d_distance(6.0f, ppm2160, 1.0f) - 88.0f) < 0.1f);
    CHECK(fabs(engine_full_3d_distance(10.0f, ppm2160, 1.0f) - 146.6f) < 0.1f);
    CHECK(engine_full_3d_distance(16.0f, ppm2160, 1.0f) == ENGINE_FULL_3D_MAX);
    CHECK(engine_full_3d_distance(500.0f, ppm2160, 1.0f) == ENGINE_FULL_3D_MAX);
    /* Past a CacheMul of 10 the maximum is below 1998's own distance, which stays the floor. */
    CHECK(engine_full_3d_distance(500.0f, ppm2160, 10.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL * 10.0f);
    CHECK(engine_full_3d_distance(500.0f, ppm2160, 9.0f) == ENGINE_FULL_3D_MAX);
    CHECK(engine_full_3d_distance(500.0f, ppm2160, 11.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL * 11.0f);
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
     * Culling at 1.1x only when it is small: large objects keep 3D until their own, longer, cull distance. */
    CHECK(engine_full_3d_distance(0.5f, ppm1440, 1.0f) < 50.0f * ENGINE_OBJECT_DISTANCE_SCALE);
}

/* The rule the 320 px threshold replaced: 256 px with 25 and 150 m. */
static float legacy_full_3d_distance(float radius, float pixels_per_metre, float cache_mul) {
    float d = (radius > 0.0f ? 2.0f * radius * pixels_per_metre / 256.0f : 0.0f) * cache_mul;
    if (d < 25.0f) d = 25.0f;
    if (d > 150.0f) d = 150.0f;
    if (d < ENGINE_CACHE_DISTANCE_ORIGINAL * cache_mul) d = ENGINE_CACHE_DISTANCE_ORIGINAL * cache_mul;
    return d;
}

static engine_partition_t cacheable_mesh(float radius, float cache_mul) {
    engine_partition_t p = { PARTITION_USES_MESH | PARTITION_CACHEABLE | PARTITION_CACHEABLE_VOLUME |
                                 PARTITION_HARDWARE_ABLE, 10, 100, radius * radius, radius * radius, 0.0f, cache_mul };
    return p;
}

/* The 256 px rule with 25-150 m is back: every distance is that rule's, never below 1998's 15 m x CacheMul. */
static void test_impostor_sooner(void) {
    const float radii[] = { 0.5f, 1.0f, 2.0f, 4.0f, 6.0f, 10.0f, 16.0f, 24.0f }, muls[] = { 1.0f, 1.25f, 2.0f, 4.0f };
    const float ppms[] = { 938.4f, ENGINE_PIXELS_PER_METRE_DEFAULT, 1876.7f };
    for (unsigned a = 0; a < sizeof radii / sizeof radii[0]; a++)
        for (unsigned m = 0; m < sizeof muls / sizeof muls[0]; m++)
            for (unsigned q = 0; q < sizeof ppms / sizeof ppms[0]; q++) {
                float old_d = legacy_full_3d_distance(radii[a], ppms[q], muls[m]);
                float new_d = engine_full_3d_distance(radii[a], ppms[q], muls[m]);
                float floor_d = ENGINE_CACHE_DISTANCE_ORIGINAL * muls[m];
                CHECK(isfinite(new_d) && new_d > 0.0f);
                CHECK(new_d <= old_d);
                CHECK(new_d >= floor_d && new_d >= ENGINE_FULL_3D_MIN);
                CHECK(fabsf(new_d - old_d) < 0.01f);
            }
    /* Radius 2/4/6/10/16 m at 1440 lines, CacheMul 1: 25/39.1/58.6/97.7/150 m (280 px had 22.9/35.7/53.6/89.4/120). */
    {
        const float radii5[] = { 2.0f, 4.0f, 6.0f, 10.0f, 16.0f }, was[] = { 25.0f, 39.1f, 58.6f, 97.7f, 150.0f },
                    now[] = { 25.0f, 39.1f, 58.6f, 97.7f, 150.0f };
        for (unsigned a = 0; a < 5; a++) {
            CHECK(fabsf(legacy_full_3d_distance(radii5[a], ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f) - was[a]) < 0.1f);
            CHECK(fabsf(engine_full_3d_distance(radii5[a], ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f) - now[a]) < 0.1f);
        }
    }
}

/* An impostor turns back into 3D nearer than the 3D object turns into one: walking at the switch distance keeps it. */
static void test_impostor_hysteresis(void) {
    const float radii[] = { 0.5f, 2.0f, 4.0f, 6.0f, 10.0f, 16.0f, 40.0f }, muls[] = { 1.0f, 1.25f, 2.0f, 8.0f, 12.0f };
    const float ppm = 1876.7f;
    for (unsigned a = 0; a < sizeof radii / sizeof radii[0]; a++)
        for (unsigned m = 0; m < sizeof muls / sizeof muls[0]; m++) {
            float out = engine_full_3d_distance(radii[a], ppm, muls[m]), back = engine_return_3d_distance(radii[a], ppm, muls[m]);
            float floor_d = ENGINE_CACHE_DISTANCE_ORIGINAL * muls[m];
            CHECK(back <= out && back >= floor_d);
            if (out > floor_d) CHECK(fabsf(back - fmaxf(out * 256.0f / 288.0f, floor_d)) < 0.01f);
            CHECK(engine_cache_min_distance_held(1, 0, ENGINE_PART_STATIC, radii[a], ppm, muls[m], 1) * muls[m] - back < 0.01f);
            CHECK(engine_cache_min_distance_held(1, 0, ENGINE_PART_STATIC, radii[a], ppm, muls[m], 0) ==
                  engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, radii[a], ppm, muls[m]));
        }
    /* 4 m at 2160 lines: an impostor beyond 58.6 m, 3D again within 52.1 m; 1998's own rules ignore the band. */
    CHECK(fabsf(engine_full_3d_distance(4.0f, ppm, 1.0f) - 58.6f) < 0.1f);
    CHECK(fabsf(engine_return_3d_distance(4.0f, ppm, 1.0f) - 52.1f) < 0.1f);
    CHECK(fabsf(engine_return_3d_distance(500.0f, ppm, 1.0f) - 150.0f * 256.0f / 288.0f) < 0.01f);
    CHECK(engine_cache_min_distance_held(1, 1, ENGINE_PART_STATIC, 4.0f, ppm, 1.0f, 1) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance_held(1, 0, ENGINE_PART_ENTITY, 4.0f, ppm, 1.0f, 1) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance_held(0, 0, ENGINE_PART_STATIC, 4.0f, ppm, 1.0f, 1) == ENGINE_CACHE_DISTANCE_ORIGINAL);
}

/* = and - only resized the 3D view inside gameplay (CGameWnd::OnKey -> ChangeViewportSize); the view margin the
 * registry stores is answered 0, so the view is always the whole launcher resolution. */
static void test_screen_size_removed(void) {
    CHECK(screen_size_key_dropped(0xBBu, 1) && screen_size_key_dropped(0xBDu, 1));
    CHECK(!screen_size_key_dropped(0xBBu, 0) && !screen_size_key_dropped(0xBDu, 0));
    CHECK(!screen_size_key_dropped(0x6Bu, 1) && !screen_size_key_dropped(0x6Du, 1));
    CHECK(!screen_size_key_dropped(0x7Au, 1) && !screen_size_key_dropped('W', 1) && !screen_size_key_dropped(0x1Bu, 1));
    CHECK(registry_view_margin_name("XView") && registry_view_margin_name("YView") && !registry_view_margin_name("Width"));
    CHECK(REGISTRY_VIEW_MARGIN == 0u);
}

static void test_impostor_state(void) {
    static engine_impostor_state_t t[64];
    uint32_t since = 99;
    memset(t, 0, sizeof t);
    CHECK(!engine_impostor_held(t, 64, 0x1000, 10) && !engine_impostor_known(t, 64, 0x1000, 10));
    CHECK(engine_impostor_note(t, 64, 0x1000, 10, 0, &since) == ENGINE_IMPOSTOR_SAME && since == 0);
    CHECK(!engine_impostor_held(t, 64, 0x1000, 11));
    CHECK(engine_impostor_known(t, 64, 0x1000, 11) && !engine_impostor_known(t, 64, 0x1000, 13));
    CHECK(engine_impostor_note(t, 64, 0x1000, 11, 1, &since) == ENGINE_IMPOSTOR_BECAME && since == 0);
    CHECK(engine_impostor_held(t, 64, 0x1000, 11) && engine_impostor_held(t, 64, 0x1000, 12));
    CHECK(engine_impostor_held(t, 64, 0x1000, 11 + ENGINE_IMPOSTOR_STATE_FRAMES));
    CHECK(!engine_impostor_held(t, 64, 0x1000, 12 + ENGINE_IMPOSTOR_STATE_FRAMES));
    CHECK(engine_impostor_note(t, 64, 0x1000, 12, 1, &since) == ENGINE_IMPOSTOR_SAME);
    CHECK(engine_impostor_note(t, 64, 0x1000, 12, 1, &since) == ENGINE_IMPOSTOR_SAME);
    CHECK(engine_impostor_note(t, 64, 0x1000, 40, 0, &since) == ENGINE_IMPOSTOR_SAME);
    CHECK(engine_impostor_note(t, 64, 0x1000, 41, 1, &since) == ENGINE_IMPOSTOR_BECAME && since == 30);
    CHECK(engine_impostor_note(t, 64, 0x1000, 42, 0, &since) == ENGINE_IMPOSTOR_DROPPED && since == 1);
    CHECK(!engine_impostor_held(t, 64, 0x1000, 42));
    CHECK(engine_impostor_note(t, 64, 0x1000, 43, 1, &since) == ENGINE_IMPOSTOR_BECAME && since == 1);
    /* Other keys in a full neighbourhood: stale entries are reused, a live key is never lost. */
    for (uint32_t k = 1; k <= 200; k++) engine_impostor_note(t, 64, k * 64u, 50, 1, &since);
    CHECK(engine_impostor_held(t, 64, 64u, 50) || !engine_impostor_held(t, 64, 64u * 200u, 50));
    CHECK(!engine_impostor_held(t, 64, 0, 50));
    CHECK(engine_impostor_note(t, 64, 0, 50, 1, &since) == ENGINE_IMPOSTOR_SAME);
}

/* Non-finite and out-of-range inputs: a finite distance within the limits, never NaN or infinity. */
static void test_impostor_distance_inputs(void) {
    const float values[] = { NAN, -INFINITY, -1.0f, 0.0f, 1e-30f, 0.5f, 1.0f, 100.0f, 1e6f, 1e30f, INFINITY };
    for (unsigned r = 0; r < sizeof values / sizeof values[0]; r++)
        for (unsigned p = 0; p < sizeof values / sizeof values[0]; p++)
            for (unsigned m = 0; m < sizeof values / sizeof values[0]; m++) {
                float d = engine_full_3d_distance(values[r], values[p], values[m]);
                float stored = engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, values[r], values[p], values[m]);
                CHECK(isfinite(d) && d >= ENGINE_FULL_3D_MIN && d <= ENGINE_CACHE_DISTANCE_ORIGINAL * 1e6f + 1.0f);
                CHECK(isfinite(stored) && stored > 0.0f);
            }
    /* Larger objects stay 3D at least as far; CacheMul scales the distance until a limit holds it. */
    for (float r = 0.0f; r < 40.0f; r += 0.25f)
        CHECK(engine_full_3d_distance(r + 0.25f, ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f) >=
              engine_full_3d_distance(r, ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f));
    CHECK(fabsf(engine_full_3d_distance(4.0f, ENGINE_PIXELS_PER_METRE_DEFAULT, 2.0f) -
                2.0f * engine_full_3d_distance(4.0f, ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f)) < 0.01f);
    CHECK(engine_full_3d_distance(4.0f, ENGINE_PIXELS_PER_METRE_DEFAULT, 1.25f) >
          engine_full_3d_distance(4.0f, ENGINE_PIXELS_PER_METRE_DEFAULT, 1.0f));
}

/* Through bShouldCache's own refusals with the distance the host stores: refused inside it, an impostor candidate past
 * it, and between the new and the old distance the object is now one. */
static void test_impostor_decision(void) {
    const float radii[] = { 2.0f, 4.0f, 6.0f, 10.0f, 16.0f };
    const float ppm = ENGINE_PIXELS_PER_METRE_DEFAULT;
    for (unsigned a = 0; a < sizeof radii / sizeof radii[0]; a++) {
        float r = radii[a], new_d = engine_full_3d_distance(r, ppm, 1.0f), old_d = legacy_full_3d_distance(r, ppm, 1.0f);
        engine_partition_t p = cacheable_mesh(r, 1.0f);
        engine_cache_settings_t now = { 1, 8.55f, 0.085f * 0.085f, engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, r, ppm, 1.0f) };
        engine_cache_settings_t was = now;
        float between = (new_d + old_d) * 0.5f - r;
        float inside = 0.95f * new_d - r, beyond = new_d + 1.0f;
        was.min_distance = legacy_full_3d_distance(r, ppm, 1.0f);
        CHECK(engine_cache_refusal(&p, &now, inside * inside) != CACHE_REFUSED_PARALLAX);
        CHECK(engine_cache_refusal(&p, &now, beyond * beyond) == CACHE_REFUSED_PARALLAX);
        if (new_d < old_d) {
            CHECK(engine_cache_refusal(&p, &was, between * between) == CACHE_REFUSED_3D_DISTANCE);
            CHECK(engine_cache_refusal(&p, &now, between * between) == CACHE_REFUSED_PARALLAX);
        }
        /* Close up (under the 1998 distance), an object is refused whatever its size. */
        CHECK(engine_cache_refusal(&p, &now, 10.0f * 10.0f) != CACHE_REFUSED_PARALLAX);
        CHECK(engine_cache_refusal(&p, &now, 1.0f) != CACHE_REFUSED_PARALLAX);
    }
    /* What the image cache cannot draw stays out at any distance, as does anything the level or its movement forbids. */
    for (float far_m = 25.0f; far_m <= 5000.0f; far_m *= 2.0f) {
        engine_cache_settings_t c = { 1, 8.55f, 0.085f * 0.085f, engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, 4.0f, ppm, 1.0f) };
        engine_partition_t p = cacheable_mesh(4.0f, 1.0f);
        p.flags &= ~PARTITION_CACHEABLE;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_NOT_CACHEABLE);
        p.flags |= PARTITION_CACHE_NEVER;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_CACHE_NEVER);
        p = cacheable_mesh(4.0f, 1.0f);
        p.flags |= PARTITION_SIMPLE_OBJECT;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_SIMPLE_OBJECT);
        p = cacheable_mesh(4.0f, 1.0f);
        p.flags |= PARTITION_ALWAYS_FACE;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_FACES_CAMERA);
        p = cacheable_mesh(4.0f, 1.0f);
        p.no_cache_until = 100;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_MOVING);
        p = cacheable_mesh(4.0f, 1.0f);
        p.flags &= ~PARTITION_CACHEABLE_VOLUME;
        CHECK(engine_cache_refusal(&p, &c, far_m * far_m) == CACHE_REFUSED_VOLUME);
    }
    /* Entities, meshes the CPU draws and the software renderer keep 1998's distance, however small the object. */
    CHECK(engine_cache_min_distance(1, 0, ENGINE_PART_ENTITY, 16.0f, ppm, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance(1, 1, ENGINE_PART_SPATIAL, 16.0f, ppm, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    CHECK(engine_cache_min_distance(0, 0, ENGINE_PART_STATIC, 16.0f, ppm, 1.0f) == ENGINE_CACHE_DISTANCE_ORIGINAL);
    /* The guest multiplies the stored distance by CacheMul again: the product is the distance of the policy. */
    for (float mul = 0.5f; mul <= 8.0f; mul *= 2.0f)
        CHECK(fabsf(engine_cache_min_distance(1, 0, ENGINE_PART_STATIC, 6.0f, ppm, mul) * mul -
                    engine_full_3d_distance(6.0f, ppm, mul)) < 0.01f);
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
static int ramp_row(float scale, float offset, float light, float start) {
    return (int)((light < start ? start : light) * scale + offset);
}

/* CClu's ramp: 32 rows from the darkest light to full light, inset by the engine's tolerance. */
static void test_terrain_shadow_row(void) {
    static const uint8_t rgb555[6] = { 10, 3, 5, 3, 0, 3 }, rgb565[6] = { 11, 3, 5, 2, 0, 3 }, bad[6] = { 16, 3, 5, 3, 0, 3 };
    static const float starts[] = { 0.38f, 0.627f, 0.8685f };
    const float tolerance = 0.2f, rows = 32.0f;
    for (int i = 0; i < 3; i++) {
        float start = starts[i], scale = (rows - 2.0f * tolerance) / (1.0f - start), offset = tolerance - start * scale;
        const float was_scale = scale, was_offset = offset;
        CHECK(ramp_row(scale, offset, 0.2f, start) == 0 && !engine_ramp_reserved(scale, offset, start));
        CHECK(engine_ramp_reserve(&scale, &offset, start, 32) && engine_ramp_reserved(scale, offset, start));
        CHECK(!engine_ramp_reserve(&scale, &offset, start, 32));
        for (float light = 0.0f; light <= 1.0f; light += 0.001f) {
            int row = ramp_row(scale, offset, light, start), was = ramp_row(was_scale, was_offset, light, start);
            CHECK(row >= 1 && row < 32 && row >= was && row <= was + 1);
        }
        CHECK(ramp_row(scale, offset, 1.0f, start) == 31 && ramp_row(scale, offset, 0.2f, start) == 1);
    }
    CHECK(engine_ramp_brightness(1, 32, 0.8685f) == 0.8685f && engine_ramp_brightness(0, 32, 0.8685f) == 0.8685f);
    CHECK(engine_ramp_brightness(1, 32, 0.8685f) > ENGINE_SHADOW_BRIGHTNESS);
    CHECK(engine_ramp_brightness(26, 32, 0.38f) > ENGINE_SHADOW_BRIGHTNESS);
    CHECK(engine_ramp_brightness(1, 32, (float)pow(0.2, 0.406)) < ENGINE_SHADOW_BRIGHTNESS);
    CHECK(engine_ramp_brightness(40, 32, 0.5f) == 1.0f && engine_ramp_brightness(5, 1, 0.5f) == 1.0f);
    CHECK(fabs(pow(0.2, 0.29) - ENGINE_SHADOW_BRIGHTNESS) < 0.001);
    {
        float scale = INFINITY, offset = 0.0f, start = 1.0f;
        CHECK(!engine_ramp_reserve(&scale, &offset, start, 32) && !engine_ramp_reserve(&scale, &offset, 0.5f, 1));
        scale = NAN;
        CHECK(!engine_ramp_reserve(&scale, &offset, 0.5f, 32) && !engine_ramp_reserved(scale, offset, 0.5f));
    }
    CHECK(engine_scale_pixel(0x7FFF, rgb555, 1.0f) == 0x7FFF && engine_scale_pixel(0xFFFF, rgb565, 1.0f) == 0xFFFF);
    CHECK(engine_scale_pixel(0xFFFF, rgb555, 0.0f) == 0x8000 && engine_scale_pixel(0xFFFF, rgb565, 0.0f) == 0);
    CHECK(engine_scale_pixel(0x7FFF, rgb555, ENGINE_SHADOW_BRIGHTNESS) == (19 << 10 | 19 << 5 | 19));
    CHECK(engine_scale_pixel(0xFFFF, rgb565, ENGINE_SHADOW_BRIGHTNESS) == (19 << 11 | 40 << 5 | 19));
    CHECK(engine_scale_pixel(0x1234, bad, 0.5f) == 0x1234);
    for (uint32_t p = 0; p < 0x10000u; p++) CHECK(engine_scale_pixel((uint16_t)p, rgb565, ENGINE_SHADOW_BRIGHTNESS) <= p);
}

/* g_afGamma (retail .rdata): the exponent of level i is gamma[i] * (3.0 - 0.1) * 0.1. */
static void test_gamma_level(void) {
    static const double gamma[10] = { 3.0, 2.6, 2.2, 1.8, 1.4, 1.0, 0.6, 0.3, 0.0, 0.0 };
    const double ambient = 0.2, lit = 0.909;
    for (uint32_t level = 0; level < 10; level++) {
        uint32_t used = engine_gamma_level(level);
        double power = gamma[used] * 0.29, shadow = pow(ambient, power) / pow(lit, power);
        CHECK(used == (level < ENGINE_GAMMA_TOP ? level : ENGINE_GAMMA_TOP));
        CHECK(power > 0.0 && shadow < 0.9);
    }
    CHECK(pow(ambient, gamma[9] * 0.29) == pow(lit, gamma[9] * 0.29));
    CHECK(engine_gamma_level(10) == ENGINE_GAMMA_TOP && engine_gamma_level(0xFFFFFFFFu) == ENGINE_GAMMA_TOP);
}

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

/* The callers named by the readbacks of log 034 (sub_0057C980) and the functions that only it and its twin reach. */
static void test_store_only_polygon_callers(void) {
    const uint32_t store_only[CPU_POLYGON_STORE_ONLY_CALLERS] = {
        0x0057C980u, 0x005D4150u, 0x005D5060u, 0x0057E680u, 0x005D6D40u, 0x005D75C0u,
        0x0056F400u, 0x005BF090u, 0x005BFFD0u, 0x00571A30u, 0x005C2F10u, 0x005C37B0u,
        0x00567A90u, 0x005B5010u, 0x005B3E10u, 0x005C1FE0u,
        0x00569A20u, 0x005B6E30u, 0x005B7960u,
    };
    const uint32_t others[] = { 0u, 0x0057B7B0u, 0x0057DB50u, 0x00560666u, 0x005D3340u, 0x00641A40u, 0x0055F540u,
                                0x0057C981u, 0x0057C97Fu, 0x00503400u, 0x005668C0u, 0x00568EF0u, 0x005B67E0u, 0x00567A91u, 0x005B5011u,
                                0x005D66F0u, 0x005D5E70u, 0x00571A31u, 0x0056F3FFu, 0x0063C5F0u, 0x0063DA50u };
    for (int i = 0; i < CPU_POLYGON_STORE_ONLY_CALLERS; i++) CHECK(cpu_polygon_store_only_caller(store_only[i]) == i);
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; i++) CHECK(cpu_polygon_store_only_caller(others[i]) == -1);
}

static void test_perf_stat(void) {
    perf_stat_t s;
    memset(&s, 0, sizeof s);
    CHECK(perf_stat_percentile(&s, 0.5) == 0.0 && perf_stat_mean(&s) == 0.0);
    for (int i = 1; i <= 100; i++) perf_stat_add(&s, i * 0.1);
    CHECK(s.n == 100 && fabs(perf_stat_mean(&s) - 5.05) < 1e-9 && fabs(s.max - 10.0) < 1e-9);
    CHECK(fabs(perf_stat_percentile(&s, 0.5) - 5.25) < 0.01 && fabs(perf_stat_percentile(&s, 0.95) - 9.75) < 0.26);
    perf_stat_add(&s, 5000.0);
    CHECK(s.n == 101 && s.max == 5000.0 && perf_stat_percentile(&s, 1.0) == PERF_STAT_BINS * PERF_STAT_BIN_MS);
    perf_stat_add(&s, -1.0);
    perf_stat_add(&s, NAN);
    perf_stat_add(&s, INFINITY);
    CHECK(s.n == 101);
}

/* A transparent-skip polygon drawn into a captured box: with the box read back from the GPU (the old path) or left
 * unwritten (store-only), the GPU picture that results from compositing the changed pixels is the same. */
#define SKIP_W 96
#define SKIP_H 64

static uint16_t skip_gpu_pixel(int x, int y) { return (uint16_t)(0x2000u + (unsigned)(x * 7 + y * 131) % 0x4000u); }

static int skip_covers(int x, int y) { return x >= 20 && x < 80 && y >= 8 && y < 50 && (x - 20) * 42 > (y - 8) * 20; }

static int skip_transparent(int x, int y) { return ((x / 3) + (y / 2)) % 3 == 0 || (x + y) % 11 == 0; }

/* SelfMod 0x0063C5F0 and 0x0063DA50 per pixel: texel = byte texture[uv]; texel 0 is skipped, otherwise
 * [ebp+edi*2] = clut[texel]. Nothing reads the screen. */
static int s_skip_clut_span;

static uint16_t skip_clut(unsigned texel) { return (uint16_t)(0x0841u * (texel % 29u) + (texel >> 5)); }

static uint8_t skip_texel(int x, int y) { return (uint8_t)((x / 2) % 5 == 0 || (y % 7) == 3 ? 0u : (unsigned)(x * 29 + y * 17) % 255u + 1u); }

static void skip_polygon(uint8_t* copy, long pitch, const d3dt_rect_t* box) {
    if (s_skip_clut_span) {
        for (int y = box->top; y < box->bottom; y++)
            for (int x = box->left; x < box->right; x++) {
                unsigned texel = skip_covers(x, y) ? skip_texel(x, y) : 0u;
                if (texel) ((uint16_t*)(copy + (size_t)y * (size_t)pitch))[x] = skip_clut(texel);
            }
        return;
    }
    for (int y = box->top; y < box->bottom; y++)
        for (int x = box->left; x < box->right; x++)
            if (skip_covers(x, y) && !skip_transparent(x, y))
                ((uint16_t*)(copy + (size_t)y * (size_t)pitch))[x] = (uint16_t)(0x6000u + (unsigned)(x * 5 + y * 3) % 0x1000u);
}

static void skip_run(int read_back, uint16_t* picture, unsigned* composited) {
    static uint16_t gpu[SKIP_W * SKIP_H], reference[SKIP_W * SKIP_H], copy[SKIP_W * SKIP_H];
    static uint32_t overlay[SKIP_W * SKIP_H];
    const long pitch = SKIP_W * 2;
    d3dt_rect_t box = { 18, 6, 82, 52 }, changed, whole = { 0, 0, SKIP_W, SKIP_H };
    tsync_counts_t counts = { 0, 0 };
    for (int y = 0; y < SKIP_H; y++)
        for (int x = 0; x < SKIP_W; x++) gpu[y * SKIP_W + x] = skip_gpu_pixel(x, y);
    memset(copy, 0x5A, sizeof copy);
    memset(reference, 0x5A, sizeof reference);
    tsync_fill((uint8_t*)copy, pitch, reference, SKIP_W, &whole, TSYNC_UNWRITTEN);
    if (read_back) tsync_fill_unwritten((uint8_t*)copy, pitch, reference, SKIP_W, &box, gpu + box.top * SKIP_W + box.left, SKIP_W);
    skip_polygon((uint8_t*)copy, pitch, &box);
    memcpy(picture, gpu, sizeof gpu);
    *composited = 0;
    if (tsync_changed_box((const uint8_t*)copy, pitch, reference, SKIP_W, &whole, &changed)) {
        memset(overlay, 0, sizeof overlay);
        tsync_overlay_box((const uint8_t*)copy, pitch, reference, SKIP_W, &changed, overlay, &counts);
        for (int y = changed.top; y < changed.bottom; y++)
            for (int x = changed.left; x < changed.right; x++)
                if (overlay[(y - changed.top) * (changed.right - changed.left) + (x - changed.left)]) {
                    picture[y * SKIP_W + x] = copy[y * SKIP_W + x];
                    (*composited)++;
                }
    }
}

static void test_transparent_skip_store_only(void) {
    static uint16_t with_readback[SKIP_W * SKIP_H], without[SKIP_W * SKIP_H];
    unsigned written = 0, a = 0, b = 0, expected = 0;
    skip_run(1, with_readback, &a);
    skip_run(0, without, &b);
    CHECK(memcmp(with_readback, without, sizeof without) == 0);
    for (int y = 0; y < SKIP_H; y++)
        for (int x = 0; x < SKIP_W; x++) {
            int drawn = skip_covers(x, y) && !skip_transparent(x, y);
            uint16_t want = drawn ? (uint16_t)(0x6000u + (unsigned)(x * 5 + y * 3) % 0x1000u) : skip_gpu_pixel(x, y);
            written += drawn;
            expected += drawn;
            CHECK(without[y * SKIP_W + x] == want);
        }
    CHECK(written > 500 && a == written && b == written && expected == written);
}

static void test_colour_keyed_span_store_only(void) {
    static uint16_t with_readback[SKIP_W * SKIP_H], without[SKIP_W * SKIP_H];
    unsigned a = 0, b = 0, drawn = 0, keyed = 0;
    for (unsigned t = 1; t < 256; t++) CHECK(skip_clut(t) != TSYNC_UNWRITTEN);
    s_skip_clut_span = 1;
    skip_run(1, with_readback, &a);
    skip_run(0, without, &b);
    s_skip_clut_span = 0;
    CHECK(memcmp(with_readback, without, sizeof without) == 0);
    for (int y = 0; y < SKIP_H; y++)
        for (int x = 0; x < SKIP_W; x++) {
            unsigned texel = skip_covers(x, y) ? skip_texel(x, y) : 0u;
            keyed += skip_covers(x, y) && !texel;
            drawn += texel != 0;
            CHECK(without[y * SKIP_W + x] == (texel ? skip_clut(texel) : skip_gpu_pixel(x, y)));
        }
    CHECK(drawn > 400 && keyed > 100 && a == drawn && b == drawn);
}

/* A span that reads the screen (half the old pixel plus the new colour): the box must hold the GPU picture first. */
static uint16_t blend_half(uint16_t old_pixel, uint16_t colour) {
    return (uint16_t)((((old_pixel >> 1) & 0x7BEFu) + ((colour >> 1) & 0x7BEFu)) & 0xFFFFu);
}

static void blend_run(int read_back, uint16_t* picture) {
    static uint16_t gpu[SKIP_W * SKIP_H], reference[SKIP_W * SKIP_H], copy[SKIP_W * SKIP_H];
    static uint32_t overlay[SKIP_W * SKIP_H];
    const long pitch = SKIP_W * 2;
    d3dt_rect_t box = { 18, 6, 82, 52 }, changed, whole = { 0, 0, SKIP_W, SKIP_H };
    tsync_counts_t counts = { 0, 0 };
    for (int y = 0; y < SKIP_H; y++)
        for (int x = 0; x < SKIP_W; x++) gpu[y * SKIP_W + x] = skip_gpu_pixel(x, y);
    memset(copy, 0x5A, sizeof copy);
    memset(reference, 0x5A, sizeof reference);
    tsync_fill((uint8_t*)copy, pitch, reference, SKIP_W, &whole, TSYNC_UNWRITTEN);
    if (read_back) tsync_fill_unwritten((uint8_t*)copy, pitch, reference, SKIP_W, &box, gpu + box.top * SKIP_W + box.left, SKIP_W);
    for (int y = box.top; y < box.bottom; y++)
        for (int x = box.left; x < box.right; x++)
            if (skip_covers(x, y)) copy[y * SKIP_W + x] = blend_half(copy[y * SKIP_W + x], 0x3333u);
    memcpy(picture, gpu, sizeof gpu);
    if (tsync_changed_box((const uint8_t*)copy, pitch, reference, SKIP_W, &whole, &changed)) {
        memset(overlay, 0, sizeof overlay);
        tsync_overlay_box((const uint8_t*)copy, pitch, reference, SKIP_W, &changed, overlay, &counts);
        for (int y = changed.top; y < changed.bottom; y++)
            for (int x = changed.left; x < changed.right; x++)
                if (overlay[(y - changed.top) * (changed.right - changed.left) + (x - changed.left)])
                    picture[y * SKIP_W + x] = copy[y * SKIP_W + x];
    }
}

static void test_read_modify_write_needs_the_box(void) {
    static uint16_t with_readback[SKIP_W * SKIP_H], without[SKIP_W * SKIP_H];
    unsigned wrong = 0;
    blend_run(1, with_readback);
    blend_run(0, without);
    for (int y = 0; y < SKIP_H; y++)
        for (int x = 0; x < SKIP_W; x++) {
            uint16_t want = skip_covers(x, y) && x >= 18 && x < 82 && y >= 6 && y < 52
                                ? blend_half(skip_gpu_pixel(x, y), 0x3333u) : skip_gpu_pixel(x, y);
            CHECK(with_readback[y * SKIP_W + x] == want);
            wrong += without[y * SKIP_W + x] != want;
        }
    CHECK(wrong > 500);
}

static void test_texture_anomalies(void) {
    CHECK(texture_anomaly(1, 3, 3, 256, 256, 0) == TEXTURE_OK);
    CHECK(texture_anomaly(0, 3, 3, 256, 256, 0) == TEXTURE_NULL_RESOURCE);
    CHECK(texture_anomaly(1, 2, 3, 256, 256, 0) == TEXTURE_STALE_GENERATION);
    CHECK(texture_anomaly(1, 3, 3, 0, 256, 0) == TEXTURE_BAD_DIMENSIONS && texture_anomaly(1, 3, 3, 256, 9000, 0) == TEXTURE_BAD_DIMENSIONS);
    CHECK(texture_anomaly(1, 3, 3, 256, 256, 1) == TEXTURE_DIRTY_NOT_UPLOADED);
    for (int k = TEXTURE_OK; k <= TEXTURE_DIRTY_NOT_UPLOADED; k++) CHECK(strcmp(texture_anomaly_name((texture_anomaly_t)k), "?") != 0);
}

static int dirty_covers(const d3dt_dirty_t* d, long x, long y) {
    for (unsigned i = 0; i < d->n; i++)
        if (x >= d->r[i].left && x < d->r[i].right && y >= d->r[i].top && y < d->r[i].bottom) return 1;
    return 0;
}

static void check_dirty_list(const d3dt_dirty_t* d, const d3dt_rect_t* added, unsigned count) {
    CHECK(d->n <= D3DT_DIRTY_MAX);
    for (unsigned i = 0; i < d->n; i++)
        for (unsigned k = i + 1; k < d->n; k++) CHECK(!d3dt_rects_near(&d->r[i], &d->r[k]));
    for (unsigned i = 0; i < count; i++)
        for (long y = added[i].top; y < added[i].bottom; y++)
            for (long x = added[i].left; x < added[i].right; x++)
                if (!dirty_covers(d, x, y)) {
                    CHECK(!"a written pixel left the dirty boxes");
                    return;
                }
}

static void test_dirty_boxes(void) {
    d3dt_dirty_t d;
    const d3dt_rect_t far_apart[2] = { { 10, 10, 40, 30 }, { 600, 400, 630, 430 } };
    const d3dt_rect_t touching[2] = { { 10, 10, 40, 30 }, { 41, 10, 70, 30 } };
    const d3dt_rect_t overlapping[3] = { { 100, 100, 200, 200 }, { 150, 150, 260, 230 }, { 90, 210, 120, 240 } };
    const d3dt_rect_t empty = { 5, 5, 5, 9 };
    d3dt_rect_t many[40];
    unsigned long long separate = 0, merged = 0;
    memset(&d, 0, sizeof d);
    for (unsigned i = 0; i < 2; i++) d3dt_dirty_add(&d, &far_apart[i]);
    CHECK(d.n == 2);
    check_dirty_list(&d, far_apart, 2);
    for (unsigned i = 0; i < d.n; i++) separate += (unsigned long long)(d.r[i].right - d.r[i].left) * (d.r[i].bottom - d.r[i].top);
    CHECK(separate == 30ull * 20ull + 30ull * 30ull); /* a single union would be 620 x 420 */
    d3dt_dirty_add(&d, &empty);
    CHECK(d.n == 2);
    memset(&d, 0, sizeof d);
    for (unsigned i = 0; i < 2; i++) d3dt_dirty_add(&d, &touching[i]);
    CHECK(d.n == 1 && d.r[0].left == 10 && d.r[0].right == 70);
    memset(&d, 0, sizeof d);
    for (unsigned i = 0; i < 3; i++) d3dt_dirty_add(&d, &overlapping[i]);
    check_dirty_list(&d, overlapping, 3);
    CHECK(d.n == 1);
    /* A full list merges instead of growing, and every box written stays covered with no apron reaching another box. */
    memset(&d, 0, sizeof d);
    for (unsigned i = 0; i < 40; i++) {
        long x = (long)((i * 151u) % 3700u), y = (long)((i * 97u) % 2100u);
        many[i].left = x;
        many[i].top = y;
        many[i].right = x + 3 + (long)(i % 7u) * 9;
        many[i].bottom = y + 2 + (long)(i % 5u) * 11;
        d3dt_dirty_add(&d, &many[i]);
        check_dirty_list(&d, many, i + 1);
    }
    CHECK(d.n >= 1 && d.n <= D3DT_DIRTY_MAX);
    /* Eight small boxes in a grid stay eight; a ninth joins the box it grows least. */
    memset(&d, 0, sizeof d);
    for (unsigned i = 0; i < 9; i++) {
        many[i].left = (long)(i % 3u) * 1200 + (i == 8 ? 60 : 0);
        many[i].top = (long)(i / 3u) * 700;
        many[i].right = many[i].left + 40;
        many[i].bottom = many[i].top + 30;
        d3dt_dirty_add(&d, &many[i]);
        check_dirty_list(&d, many, i + 1);
        CHECK(d.n == (i < 8 ? i + 1 : 8u));
    }
    for (unsigned i = 0; i < d.n; i++) merged += (unsigned long long)(d.r[i].right - d.r[i].left) * (d.r[i].bottom - d.r[i].top);
    CHECK(merged == 7ull * 40ull * 30ull + 1300ull * 30ull);
}

static void test_render_state_batches(void) {
    const DWORD drawn[] = { D3DRENDERSTATE_ZENABLE, D3DRENDERSTATE_FILLMODE, D3DRENDERSTATE_SHADEMODE,
                            D3DRENDERSTATE_ZWRITEENABLE, D3DRENDERSTATE_ALPHATESTENABLE, D3DRENDERSTATE_TEXTUREMAG,
                            D3DRENDERSTATE_TEXTUREMIN, D3DRENDERSTATE_SRCBLEND, D3DRENDERSTATE_DESTBLEND,
                            D3DRENDERSTATE_TEXTUREMAPBLEND, D3DRENDERSTATE_CULLMODE, D3DRENDERSTATE_ZFUNC,
                            D3DRENDERSTATE_ALPHAREF, D3DRENDERSTATE_ALPHAFUNC, D3DRENDERSTATE_ALPHABLENDENABLE,
                            D3DRENDERSTATE_FOGENABLE, D3DRENDERSTATE_SPECULARENABLE, D3DRENDERSTATE_FOGCOLOR,
                            D3DRENDERSTATE_TEXTUREADDRESS, D3DRENDERSTATE_TEXTUREADDRESSU,
                            D3DRENDERSTATE_TEXTUREADDRESSV, D3DRENDERSTATE_ZBIAS };
    const DWORD ignored[] = { D3DRENDERSTATE_TEXTUREHANDLE, D3DRENDERSTATE_TEXTUREPERSPECTIVE, D3DRENDERSTATE_LASTPIXEL,
                              D3DRENDERSTATE_DITHERENABLE, D3DRENDERSTATE_SUBPIXEL, D3DRENDERSTATE_SUBPIXELX,
                              D3DRENDERSTATE_STIPPLEDALPHA, D3DRENDERSTATE_FOGTABLEMODE,
                              D3DRENDERSTATE_COLORKEYENABLE, D3DRENDERSTATE_RANGEFOGENABLE };
    for (unsigned i = 0; i < sizeof drawn / sizeof drawn[0]; i++) {
        CHECK(!d3dt_render_state_keeps_batch(drawn[i]));
        CHECK(d3dt_render_change_flushes(drawn[i], 0, 1) && !d3dt_render_change_flushes(drawn[i], 5, 5));
    }
    for (unsigned i = 0; i < sizeof ignored / sizeof ignored[0]; i++) {
        CHECK(d3dt_render_state_keeps_batch(ignored[i]));
        CHECK(!d3dt_render_change_flushes(ignored[i], 0, 1));
    }
}

static void test_polygon_spans(void) {
    CHECK(cpu_polygon_span(0x005D66F0u) == 0x006402F0u && cpu_polygon_span(0x0057DB50u) == 0x006402F0u);
    CHECK(cpu_polygon_span(0x00567A90u) == 0x0063F5E0u && cpu_polygon_span(0x005B5010u) == 0x0063F5E0u &&
          cpu_polygon_span(0x005B3E10u) == 0x0063F5E0u);
    CHECK(cpu_polygon_span(0x0057C980u) == 0x00640D60u && cpu_polygon_span(0x0057E680u) == 0x0063F850u);
    CHECK(cpu_polygon_span(0x00571A30u) == 0x0063C5F0u && cpu_polygon_span(0x005C2F10u) == 0x0063C5F0u &&
          cpu_polygon_span(0x005C37B0u) == 0x0063C5F0u);
    CHECK(cpu_polygon_span(0x0056F400u) == 0x0063DA50u && cpu_polygon_span(0x005BF090u) == 0x0063DA50u &&
          cpu_polygon_span(0x005BFFD0u) == 0x0063DA50u);
    CHECK(cpu_polygon_span(0x005C1FE0u) == 0x0063F360u);
    CHECK(cpu_polygon_span(0x00569A20u) == 0x0063E900u && cpu_polygon_span(0x005B7960u) == 0x0063E900u);
    CHECK(cpu_polygon_span(0x00401000u) == 0u);
    CHECK(cpu_polygon_store_only_caller(0x005D66F0u) == -1 && cpu_polygon_store_only_caller(0x0057DB50u) == -1);
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

/* After a Flip or a device attach the reference belongs to another buffer: a mark left in the CPU copy must not reach
 * the overlay from a tile the run did not capture. */
static void test_lazy_marks_not_published(void) {
    static uint16_t copy[64 * 64], reference[64 * 64];
    static uint32_t out[64 * 64];
    static tsync_tiles_t fresh;
    d3dt_rect_t all = { 0, 0, 64, 64 }, tile0 = { 0, 0, 32, 32 }, box;
    tsync_counts_t counts = { 0, 0 };
    for (int i = 0; i < 64 * 64; i++) copy[i] = reference[i] = 0x1111;
    copy[40 * 64 + 40] = TSYNC_UNWRITTEN;
    reference[40 * 64 + 40] = 0x2222;
    CHECK(tsync_tiles_reset(&fresh, 64, 64));
    tsync_fill((uint8_t*)copy, 128, reference, 64, &tile0, TSYNC_UNWRITTEN);
    tsync_tiles_claim(&fresh, &tile0, count_tile_rect, &(tile_calls_t){ 0, 0 });
    copy[5 * 64 + 6] = 0x0777;
    CHECK(tsync_changed_box((const uint8_t*)copy, 128, reference, 64, &all, &box));
    CHECK(box.left <= 40 && box.right > 40 && box.top <= 40 && box.bottom > 40);
    CHECK(tsync_changed_box_in((const uint8_t*)copy, 128, reference, 64, &all, &fresh, &box));
    CHECK(box.left == 6 && box.right == 7 && box.top == 5 && box.bottom == 6);
    tsync_overlay_box_in((const uint8_t*)copy, 128, reference, 64, &all, &fresh, out, &counts);
    CHECK(counts.changed == 1 && out[5 * 64 + 6] != 0);
    CHECK(out[40 * 64 + 40] == 0);
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

/* A primitive that reads needs this frame's pixels, so its box is read back before it runs: the capture's marks become
 * the GPU picture, and the CPU's own pixel is published even when its value is the mark's. */
static void test_readback_makes_the_box_current(void) {
    uint16_t copy[4 * 2], reference[4 * 2], gpu[4 * 2];
    uint32_t out[4 * 2];
    d3dt_rect_t all = { 0, 0, 4, 2 };
    tsync_counts_t counts = { 0, 0 };
    for (int i = 0; i < 8; i++) gpu[i] = (uint16_t)(0x1000 + i);
    tsync_fill((uint8_t*)copy, 8, reference, 4, &all, TSYNC_UNWRITTEN);
    tsync_fill_unwritten((uint8_t*)copy, 8, reference, 4, &all, gpu, 4);
    for (int i = 0; i < 8; i++) CHECK(copy[i] == gpu[i] && reference[i] == gpu[i]);
    copy[1 * 4 + 2] = TSYNC_UNWRITTEN;
    tsync_overlay_box((const uint8_t*)copy, 8, reference, 4, &all, out, &counts);
    CHECK(counts.changed == 1 && out[1 * 4 + 2] == 0xFFFF00FFu);
    CHECK(out[0] == 0 && out[7] == 0);
    CHECK(reference[1 * 4 + 2] == TSYNC_UNWRITTEN);
    {
        static tsync_tiles_t fresh;
        CHECK(tsync_tiles_reset(&fresh, 4, 2));
        tsync_tiles_claim_all(&fresh);
        copy[1 * 4 + 2] = TSYNC_UNWRITTEN;
        reference[1 * 4 + 2] = gpu[1 * 4 + 2];
        memset(out, 0, sizeof out);
        counts.changed = 0;
        tsync_overlay_box_in((const uint8_t*)copy, 8, reference, 4, &all, &fresh, out, &counts);
        CHECK(counts.changed == 1 && out[1 * 4 + 2] == 0xFFFF00FFu);
    }
}

static void test_planar_gradient_limit(void) {
    CHECK(planar_gradient_limit(480) == PLANAR_GRADIENT_LIMIT_1998);
    CHECK(planar_gradient_limit(256) == PLANAR_GRADIENT_LIMIT_1998);
    CHECK(planar_gradient_limit(0) == PLANAR_GRADIENT_LIMIT_1998);
    CHECK(planar_gradient_limit(-5) == PLANAR_GRADIENT_LIMIT_1998);
    CHECK(fabsf(planar_gradient_limit(1080) - (-253.125f)) < 1e-3f);
    CHECK(fabsf(planar_gradient_limit(1440) - (-450.0f)) < 1e-3f);
    CHECK(fabsf(planar_gradient_limit(2160) - (-1012.5f)) < 1e-3f);
    CHECK(planar_gradient_limit(2160) < planar_gradient_limit(1440));
}

static void test_terrain_shadow_pool(void) {
    /* 1.0.116 at 0x00492298 and PT-BR at 0x00470F41: SetTextureMemSize's last two calls. */
    static const uint8_t en116[] = { 0xC1, 0xE0, 0x0A, 0x50, 0xE8, 0x3F, 0x67, 0x01, 0x00, 0x8B, 0x15, 0x4C, 0x42, 0x66,
                                     0x00, 0x8B, 0x0D, 0x9C, 0xF9, 0x66, 0x00, 0x6A, 0x00, 0x52, 0x68, 0x00, 0x00, 0x80,
                                     0x3F, 0x6A, 0x00, 0x68, 0x00, 0x00, 0x08, 0x00, 0xE8, 0x4F, 0x67, 0x01, 0x00, 0xE9,
                                     0xBA, 0x33, 0x00, 0x00 };
    static const uint8_t ptbr[] = { 0x8B, 0x15, 0xFC, 0x6F, 0x66, 0x00, 0x8B, 0x0D, 0x34, 0x34, 0x67, 0x00, 0x6A, 0x00,
                                    0x52, 0x68, 0x00, 0x00, 0x80, 0x3F, 0x6A, 0x00, 0x68, 0x00, 0x00, 0x08, 0x00, 0xE8,
                                    0x4F, 0xC1, 0x01, 0x00, 0xE9, 0xFA, 0x33, 0x00, 0x00, 0xC3 };
    uint8_t other[sizeof en116];
    uint32_t pool = 0, alloc = 0;
    CHECK(engine_terrain_shadow_kb(256) == 4096u);
    CHECK(engine_terrain_shadow_kb(128) == 1024u);
    CHECK(engine_terrain_shadow_kb(64) == ENGINE_TERRAIN_SHADOW_KB_ORIGINAL);
    CHECK(engine_terrain_shadow_kb(0) == ENGINE_TERRAIN_SHADOW_KB_ORIGINAL);
    CHECK(engine_terrain_shadow_kb(0x10000) == ENGINE_TERRAIN_SHADOW_KB_ORIGINAL);
    CHECK(engine_terrain_shadow_kb(256) * 1024u / (256u * 256u * 2u) == ENGINE_TERRAIN_SHADOW_PAGES);
    CHECK(ENGINE_TERRAIN_SHADOW_KB_ORIGINAL * 1024u / (256u * 256u * 2u) == 4u);
    CHECK(terrain_shadow_pool_call(en116, 0x00492298u, sizeof en116, &pool, &alloc));
    CHECK(pool == 0x0066F99Cu && alloc == 0x004A8A10u);
    CHECK(terrain_shadow_pool_call(ptbr, 0x00470F41u, sizeof ptbr, &pool, &alloc));
    CHECK(pool == 0x00673434u && alloc == 0x00470F61u + 0x0001C14Fu);
    memcpy(other, en116, sizeof other);
    other[31] = 0x68;
    other[34] = 0x10;
    CHECK(!terrain_shadow_pool_call(other, 0x00492298u, sizeof other, &pool, &alloc));
    CHECK(!terrain_shadow_pool_call(en116, 0x00492298u, 30, &pool, &alloc));
}

/* fCullDistanceCombined multiplies every squared cull distance: a level's own Culling stays shorter or longer than
 * another's, and the original limits keep 1. */
static void test_draw_distance_policy(void) {
    const float culls[] = { 20.0f, 50.0f, 126.0f, 504.0f, 900.0f }, combined = engine_object_cull_combined(1);
    float previous = 0.0f;
    CHECK(fabsf(combined - 1.44f) < 1e-5f && engine_object_cull_combined(0) == 1.0f);
    CHECK(ENGINE_OBJECT_DISTANCE_SCALE >= 1.0f && ENGINE_OBJECT_DISTANCE_SCALE < 1.4f);
    for (unsigned i = 0; i < sizeof culls / sizeof culls[0]; i++) {
        float drawn = sqrtf(culls[i] * culls[i] * combined);
        CHECK(fabsf(drawn - 1.2f * culls[i]) < 0.01f);
        CHECK(drawn >= culls[i] && drawn > previous);
        CHECK(sqrtf(culls[i] * culls[i] * engine_object_cull_combined(0)) == culls[i]);
        previous = drawn;
    }
}

/* The ceiling of the host's own far-clip reach: a level that clips nearer keeps its own far clip. */
static void test_far_clip_limit(void) {
    CHECK(ENGINE_FAR_CLIP_MAX == 750.0f);
    CHECK(engine_far_clip_limit(500.0f) == 500.0f && engine_far_clip_limit(600.0f) == 600.0f);
    CHECK(engine_far_clip_limit(749.0f) == 749.0f && engine_far_clip_limit(750.0f) == 750.0f);
    CHECK(engine_far_clip_limit(900.0f) == 750.0f && engine_far_clip_limit(1000.0f) == 750.0f);
    CHECK(engine_far_clip_limit(50000.0f) == 750.0f);
    CHECK(engine_far_clip_limit(0.0f) == 750.0f && engine_far_clip_limit(-5.0f) == 750.0f);
    CHECK(engine_far_clip_limit(NAN) == 750.0f && engine_far_clip_limit(INFINITY) == 750.0f);
    for (float far_clip = 10.0f; far_clip <= 2000.0f; far_clip += 10.0f)
        CHECK(engine_far_clip_limit(far_clip) == (far_clip < 750.0f ? far_clip : 750.0f));
}

static void test_engine_limits(void) {
    float cull =engine_cull_distance(1.0f, 800.0f, ENGINE_CULL_SCALE_ORIGINAL, ENGINE_CULL_MAX_ORIGINAL);
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
    CHECK(ENGINE_OBJECT_DISTANCE_SCALE == 1.2f);
    CHECK(59.9f * 59.9f < level_culling * level_culling * combined && 60.1f * 60.1f > level_culling * level_culling * combined);
    test_draw_distance_policy();
    test_far_clip_limit();
    test_projected_size_policy();
    test_impostor_sooner();
    test_impostor_hysteresis();
    test_impostor_state();
    test_screen_size_removed();
    test_impostor_distance_inputs();
    test_impostor_decision();
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
    test_gamma_level();
    test_terrain_shadow_row();
    /* x86: the texture memory the game may hold, twice over for its pages and ours, stays far below 2 GB. */
    CHECK((unsigned long long)ENGINE_TEXTURE_MB_MODERN * 2 * 1048576 < 256ull * 1048576);
    /* Terrain pages: 256 px, 16-bit, and the regular textures keep more than the old 40 MB budget gave them. */
    CHECK(ENGINE_TERRAIN_TEXTURE_KB_MODERN >= 1920u && ENGINE_TERRAIN_TEXTURE_KB_MODERN % 128u == 0);
    CHECK(engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0x7FFFFFFFu, 1).regular -
              (int32_t)(ENGINE_TERRAIN_TEXTURE_KB_MODERN * 1024u - 0x1E0000u) >
          engine_texture_layout(ENGINE_TEXTURE_MB_SAFE, 0x7FFFFFFFu, 0).regular);
    {
        /* Log 209 at 64 MB: 63744 KB divided, 23936 KB left to the regular textures once 16 MB of terrain pages and the
         * 20 MB image cache are paid. */
        engine_texture_layout_t at64 = engine_texture_layout(64, 0x7FFFFFFFu, 1);
        engine_texture_layout_t now = engine_texture_layout(ENGINE_TEXTURE_MB_MODERN, 0x7FFFFFFFu, 1);
        CHECK(at64.total / 1024 == 63744 && engine_regular_texture_bytes(&at64, 1) / 1024 == 23936);
        CHECK(engine_regular_texture_bytes(&at64, 0) == at64.regular);
        CHECK(ENGINE_TEXTURE_MB_MODERN == 96u && now.total / 1024 == 96512 && now.image_cache / 1024 == 20480 &&
              now.water / 1024 == 1024 && !now.overflow && now.accepted);
        CHECK(engine_regular_texture_bytes(&now, 1) / 1024 == 56704);
        CHECK(engine_regular_texture_bytes(&now, 1) / 1024 >= 48 * 1024);
        /* Each regular texture has a system-memory surface besides its Direct3D 11 copy: 32 MB more budget is at most
         * 64 MB more of the x86 address space, log 209 peaked at 1658 MB of 2048. */
        CHECK(1658u + 2u * (ENGINE_TEXTURE_MB_MODERN - 64u) <= 1800u);
    }
}

/* CQuadRootTIN::Update's inputs: levels set 230 m, TrrPixelTol 10 (7 near the beach) and TrrPixelTolFar 50; quality 4
 * scales distances by 1.5 and tolerances by 0.5. */
static void test_terrain_settings(void) {
    terrain_settings_t level = { 230.0f, 10.0f, 50.0f, 1.5f, 0.5f }, used;
    used = terrain_modern_settings(level, 1000.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - ENGINE_FAR_CLIP_MAX) < 0.01);
    CHECK(ENGINE_FAR_CLIP_MAX == 750.0f);
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
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - ENGINE_FAR_CLIP_MAX) < 0.01);
    used = terrain_modern_settings(level, 50000.0f, 3.0f);
    CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - ENGINE_FAR_CLIP_MAX) < 0.01);
    /* The rvar's own range is 10-1000: Update reads the data directly, so a value past 1000 / 1.5 still works. */
    CHECK(used.dynamic_texture_distance > 230.0f && used.dynamic_texture_distance <= 1000.0f);
    /* Own textures follow the far clip up to the ceiling; the tolerances do not depend on it. */
    {
        const float fars[] = { 400.0f, 500.0f, 600.0f, 749.0f, 750.0f, 900.0f, 1000.0f, 1500.0f };
        terrain_settings_t reference = terrain_modern_settings(level, 1000.0f, 3.0f);
        for (unsigned i = 0; i < sizeof fars / sizeof fars[0]; i++) {
            float reach = fars[i] < ENGINE_FAR_CLIP_MAX ? fars[i] : ENGINE_FAR_CLIP_MAX;
            used = terrain_modern_settings(level, fars[i], 3.0f);
            CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - reach) < 0.01);
            CHECK(used.pixel_tolerance == reference.pixel_tolerance &&
                  used.pixel_tolerance_far == reference.pixel_tolerance_far);
            CHECK(used.distance_scale == level.distance_scale && used.pixel_tolerance_scale == level.pixel_tolerance_scale);
        }
        /* Quality 0 scales the distance by 0.5: the range variable is larger, the metres are the same. */
        level.distance_scale = 0.5f;
        used = terrain_modern_settings(level, 1000.0f, 3.0f);
        CHECK(fabs(used.dynamic_texture_distance * level.distance_scale - ENGINE_FAR_CLIP_MAX) < 0.01);
        level.distance_scale = 1.5f;
    }
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
/* UseSubTitles: absent reads as On; the game's own Off written before the port had a default becomes On once, and an
 * Off stored after that stays Off. */
static void test_subtitles_default_migration(void) {
    uint32_t replacement = 7;
    CHECK(registry_subtitles_migration(0, 1, 0, &replacement) && replacement == REGISTRY_SUBTITLES_ON);
    replacement = 7;
    CHECK(!registry_subtitles_migration(1, 1, 0, &replacement) && replacement == 7);
    CHECK(!registry_subtitles_migration(0, 0, 0, &replacement));
    CHECK(!registry_subtitles_migration(1, 0, 0, &replacement));
    CHECK(!registry_subtitles_migration(0, 1, 1, &replacement));
    CHECK(!registry_subtitles_migration(0, 1, REGISTRY_SUBTITLES_ON, &replacement));
    CHECK(!registry_subtitles_migration(1, 1, 1, &replacement));
}

static void test_engine_upload_clock(void) {
    const uint32_t start = 0xFFFFFFF0u;
    CHECK(engine_upload_clock(start, 0.0, ENGINE_UPLOAD_BUDGET_MS) == start);
    CHECK(engine_upload_clock(start, ENGINE_UPLOAD_BUDGET_MS, ENGINE_UPLOAD_BUDGET_MS) == start);
    CHECK(engine_upload_clock(start, ENGINE_UPLOAD_BUDGET_MS + 0.01, ENGINE_UPLOAD_BUDGET_MS) - start >
          ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(engine_upload_clock(100u, 7.5, 7.0) - 100u > ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(engine_upload_clock(100u, 6.9, 7.0) - 100u <= ENGINE_UPLOAD_MS_ORIGINAL);
    CHECK(engine_upload_budget_ms(0) == 3.0 && engine_upload_budget_ms(1) == 6.0);
    CHECK(engine_upload_clock(start, 2.9, engine_upload_budget_ms(0)) == start);
    CHECK(engine_upload_clock(start, 3.1, engine_upload_budget_ms(0)) != start);
    CHECK(engine_upload_clock(start, 5.9, engine_upload_budget_ms(1)) == start);
    CHECK(engine_upload_clock(start, 6.1, engine_upload_budget_ms(1)) != start);
    CHECK(ENGINE_UPLOAD_BACKLOG_MS < 16.7 / 2.0);
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

/* Anne's right hand at Movement Speed m, modelled on the retail step (MAX_TIMESTEP 0.01, Pelvis_integrate's forward
 * Euler): Update_Right_Arm (0x005001D0) springs the hand towards body + rest pose through Mass_Solution (0x004F94E0:
 * 450, damping sqrt(1350) / intensity along the target and 2.5 sqrt(1350) across it, capped at 150 m/s^2) with damping
 * input hand velocity - 1.25 body velocity, then gravity. The body is the foot plus the crouch height: the walk drive
 * 36.75 m - 10 v (stop -50 v) acts only in contact, and the foot's contact (Foot_IDOF, 0x004E2E6B) and the jump
 * (0x004E7903) change its velocity directly, which on bumpy ground holds the 7.5x body near the 6-13 m/s of log 073.
 * Past the drop distance the game lets the hand go: it falls and drags on the ground. */
#define HAND_DT 0.01f
#define HAND_DROP_M 1.644f /* pw_hand_limit for Anne */
#define HAND_JUMP_SPEED (5.0f * 1.7320508f) /* Jump Height 3x */

enum { HAND_RETAIL, HAND_PREVIOUS, HAND_FOLLOW };
enum { BAND_10, BAND_50, BAND_100, BAND_500, BANDS };

typedef struct {
    float ox, oy, oz, gx, gy, bump;
} terrain_t;

typedef struct {
    float worst, band[BANDS], travelled, body_speed, hand_speed, final_distance;
    int dropped, finite;
} hand_run_t;

typedef struct {
    float m, intensity, seconds, metres, heading;
    int fix, course_073, move_hand, snag;
    terrain_t ground;
} hand_course_t;

static float terrain_z(const terrain_t* g, float x, float y) {
    float dx = x - g->ox, dy = y - g->oy;
    return g->oz + g->gx * dx + g->gy * dy + 2.0f * sinf(dx / 15.0f) + 1.5f * cosf(dy / 20.0f) - 1.5f +
           g->bump * sinf(3.1f * dx) * sinf(2.7f * dy);
}

static void terrain_normal(const terrain_t* g, float x, float y, float n[3]) {
    float dx = x - g->ox, dy = y - g->oy;
    float sx = g->gx + 2.0f / 15.0f * cosf(dx / 15.0f) + g->bump * 3.1f * cosf(3.1f * dx) * sinf(2.7f * dy);
    float sy = g->gy - 1.5f / 20.0f * sinf(dy / 20.0f) + g->bump * 2.7f * sinf(3.1f * dx) * cosf(2.7f * dy);
    float l = sqrtf(sx * sx + sy * sy + 1.0f);
    n[0] = -sx / l;
    n[1] = -sy / l;
    n[2] = 1.0f / l;
}

static void mass_solution(const float t[3], const float vel[3], float intensity, float r[3]) {
    const float kappa = 450.0f, root = sqrtf(3.0f * 450.0f), along = root / intensity, across = 2.5f * root;
    float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]), th[3] = { 0, 0, 0 }, vmag = 0.0f, mag = 0.0f;
    if (len > 0.001f)
        for (int i = 0; i < 3; i++) th[i] = t[i] / len;
    for (int i = 0; i < 3; i++) vmag += vel[i] * th[i];
    for (int i = 0; i < 3; i++) {
        float v = vmag * th[i];
        r[i] = kappa * t[i] - along * v - across * (vel[i] - v);
        mag += r[i] * r[i];
    }
    if (mag > 150.0f * 150.0f)
        for (int i = 0; i < 3; i++) r[i] *= sqrtf(150.0f * 150.0f / mag);
}

static float length3(const float a[3], const float b[3]) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* Runs, stops, curves, turns sharply, crouches and jumps (every 7 s) for at least c->seconds and c->metres. The 073
 * course instead runs from log 073's last HAND FAR point towards where the pelvis was lost, weaving. */
static hand_run_t hand_run(const hand_course_t* c) {
    float foot[3], V[3] = { 0, 0, 0 }, B[3], H[3], HV[3] = { 0, 0, 0 }, yaw = c->heading, mouth = 0.9f;
    float k = c->fix == HAND_PREVIOUS && c->m > 1.0f ? 1.0f - 1.0f / c->m : 0.0f, world[3], carried_k;
    hand_follow_t follow = { 0 };
    hand_carrier_t carrier = { 0 };
    hand_run_t out;
    int dropped = 0;
    memset(&out, 0, sizeof out);
    out.finite = 1;
    foot[0] = c->ground.ox;
    foot[1] = c->ground.oy;
    foot[2] = terrain_z(&c->ground, foot[0], foot[1]);
    B[0] = foot[0];
    B[1] = foot[1];
    B[2] = foot[2] + mouth;
    H[0] = B[0] + 0.31f * cosf(yaw) - 0.15f * sinf(yaw);
    H[1] = B[1] + 0.31f * sinf(yaw) + 0.15f * cosf(yaw);
    H[2] = B[2] - 0.35f;
    for (int step = 0; step < 200000; step++) {
        float t = step * HAND_DT, phase = fmodf(t, 30.0f), in = 1.0f, turn = 0.0f, rest[3] = { 0.31f, 0.15f, -0.35f };
        float a[3], ha[3], target[3], u[3], intensity = c->intensity, r;
        int grounded, band, controlled = 0, snagged = c->snag && fmodf(t, 5.0f) > 4.0f;
        if (t >= c->seconds && out.travelled >= c->metres) break;
        if (c->course_073) {
            turn = 0.3f * cosf(t * 1.1f);
        } else {
            if (phase > 12.0f && phase < 14.0f) in = 0.0f;
            if (phase > 20.0f && phase < 23.0f) turn = 1.2f;
            if (phase > 25.0f && phase < 25.5f) turn = 6.0f;
            if (phase > 16.0f && phase < 18.0f) mouth += (0.5f - mouth) * 0.05f;
            if (c->move_hand && phase > 5.0f && phase < 10.0f) {
                rest[0] = 0.55f;
                rest[1] = 0.05f;
                rest[2] = 0.1f;
                intensity = 0.75f;
                controlled = 1;
            }
        }
        yaw += turn * HAND_DT;
        mouth += (0.9f + 0.04f * sinf(t * 9.0f) - mouth) * 0.05f;
        grounded = foot[2] <= terrain_z(&c->ground, foot[0], foot[1]) + 1e-4f;
        for (int i = 0; i < 2; i++) {
            float dir = i == 0 ? cosf(yaw) : sinf(yaw);
            a[i] = !grounded ? 0.0f : in > 0.0f ? 36.75f * c->m * dir - 10.0f * V[i] : -50.0f * V[i];
        }
        a[2] = -10.0f;
        B[0] = foot[0];
        B[1] = foot[1];
        B[2] = foot[2] + mouth;
        target[0] = B[0] + rest[0] * cosf(yaw) - rest[1] * sinf(yaw);
        target[1] = B[1] + rest[0] * sinf(yaw) + rest[1] * cosf(yaw);
        target[2] = B[2] + rest[2];
        carried_k = c->fix == HAND_FOLLOW && controlled ? hand_frame_share(c->m) : 0.0f;
        if (c->fix == HAND_FOLLOW) {
            hand_follow_enter(&follow, !controlled, B, V, H, HV);
            hand_carrier_step(&carrier, carried_k, HAND_DT, B, V, H, HV);
            if (carrier.valid) hand_damping_shift(carried_k, 1.0f, V, HV);
        }
        for (int i = 0; i < 3; i++) {
            u[i] = HV[i] - 1.25f * V[i] + (c->fix == HAND_PREVIOUS && i < 2 ? 0.25f * k * V[i] : 0.0f);
            target[i] -= H[i];
        }
        if (c->fix == HAND_FOLLOW) {
            hand_follow_leave(&follow, HV);
            if (carrier.valid) hand_damping_shift(carried_k, -1.0f, V, HV);
        }
        if (!dropped && length3(H, B) > HAND_DROP_M) dropped = out.dropped = 1;
        if (dropped)
            ha[0] = ha[1] = ha[2] = 0.0f;
        else
            mass_solution(target, u, intensity, ha);
        ha[2] -= 10.0f;
        if (c->fix == HAND_PREVIOUS)
            for (int i = 0; i < 2; i++) ha[i] += k * a[i];
        if (H[2] <= terrain_z(&c->ground, H[0], H[1])) {
            H[2] = terrain_z(&c->ground, H[0], H[1]);
            if (HV[2] < 0.0f) HV[2] = 0.0f;
            if (ha[2] < 0.0f) ha[2] = 0.0f;
            for (int i = 0; i < 2; i++) ha[i] -= 8.0f * HV[i];
        }
        if (snagged) /* caught on something: the contact takes all of the hand's velocity */
            for (int i = 0; i < 3; i++) HV[i] = ha[i] = 0.0f;
        r = length3(H, B);
        band = out.travelled < 10.0f ? BAND_10 : out.travelled < 50.0f ? BAND_50 : out.travelled < 100.0f ? BAND_100 : BAND_500;
        out.band[band] = fmaxf(out.band[band], r);
        out.worst = fmaxf(out.worst, r);
        for (int i = 0; i < 3; i++) {
            foot[i] += HAND_DT * V[i];
            V[i] += HAND_DT * a[i];
            H[i] += HAND_DT * HV[i];
            HV[i] += HAND_DT * ha[i];
            if (!isfinite(foot[i]) || !isfinite(V[i]) || !isfinite(H[i]) || !isfinite(HV[i])) out.finite = 0;
        }
        if (!out.finite) break;
        out.travelled += HAND_DT * hypotf(V[0], V[1]);
        out.body_speed = fmaxf(out.body_speed, sqrtf(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]));
        hand_follow_world_velocity(&follow, HV, world);
        out.hand_speed = fmaxf(out.hand_speed, sqrtf(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]));
        if (foot[2] <= terrain_z(&c->ground, foot[0], foot[1])) {
            float n[3], vn;
            terrain_normal(&c->ground, foot[0], foot[1], n);
            foot[2] = terrain_z(&c->ground, foot[0], foot[1]);
            vn = V[0] * n[0] + V[1] * n[1] + V[2] * n[2];
            if (vn < 0.0f)
                for (int i = 0; i < 3; i++) V[i] -= vn * n[i];
            if (t > 1.0f && fmodf(t, 7.0f) < HAND_DT * 0.5f) V[2] = HAND_JUMP_SPEED;
        }
    }
    B[0] = foot[0];
    B[1] = foot[1];
    B[2] = foot[2] + mouth;
    out.final_distance = length3(H, B);
    return out;
}

static hand_course_t hand_course(float m, int fix, float bump) {
    hand_course_t c;
    memset(&c, 0, sizeof c);
    c.m = m;
    c.fix = fix;
    c.intensity = 0.7f;
    c.seconds = 60.0f;
    c.metres = 500.0f;
    c.ground.gy = 0.07f;
    c.ground.bump = bump;
    return c;
}

/* Logs 073 and 211: a hand at the retail HandDrop distance (about 1.65 m) is normal; one 55-62 m away has left the
 * skeleton and is put back beside the body; a reach, a held object or a throw never gets past 3.5 m. */
static void test_hand_recovery(void) {
    const float body[3] = { 224.7f, 640.8f, 47.0f }, offset[3] = { 0.3f, -0.2f, 0.1f };
    float out[3], nan_offset[3] = { NAN, 0.0f, 0.0f };
    const float multipliers[] = { 1.0f };
    CHECK(!hand_recovery_needed(0.4f, 1.644f) && !hand_recovery_needed(1.65f, 1.644f) && !hand_recovery_needed(2.0f, 1.644f));
    CHECK(!hand_recovery_needed(3.29f, 1.644f) && !hand_recovery_needed(3.5f, 1.644f));
    CHECK(hand_recovery_needed(3.51f, 1.644f) && hand_recovery_needed(55.206f, 1.644f) && hand_recovery_needed(62.9f, 1.644f));
    CHECK(!hand_recovery_needed(3.9f, 2.0f) && hand_recovery_needed(4.1f, 2.0f));
    CHECK(hand_recovery_needed(NAN, 1.644f) && hand_recovery_needed(INFINITY, 1.644f));
    CHECK(!hand_recovery_needed(3.4f, NAN) && hand_recovery_needed(3.6f, 0.0f));
    hand_rest_position(body, offset, 1, out);
    CHECK(fabsf(out[0] - 225.0f) < 1e-3f && fabsf(out[1] - 640.6f) < 1e-3f && fabsf(out[2] - 47.1f) < 1e-3f);
    hand_rest_position(body, offset, 0, out);
    CHECK(out[0] == body[0] && out[1] == body[1] && fabsf(out[2] - (body[2] - 0.35f)) < 1e-4f);
    hand_rest_position(body, nan_offset, 1, out);
    CHECK(isfinite(out[0]) && out[0] == body[0] && !hand_recovery_needed(hypotf(out[0] - body[0], out[2] - body[2]), 1.644f));
    (void)multipliers;
}

/* A passive hand at rest rides the pelvis exactly, whatever moves the body (velocity set or removed, position rebuilt)
 * and at any Movement Speed; Move Hand, a held object or a throw leave the hand to the game, untouched. */
static void test_hand_follow(void) {
    static const float multipliers[] = { 1.0f, 2.0f, 5.0f, 10.0f };
    CHECK(hand_passive(0, 0, 0.0f) && !hand_passive(1, 0, 0.0f) && !hand_passive(2, 0, 0.0f) && !hand_passive(3, 0, 0.0f));
    CHECK(!hand_passive(0, 1, 0.0f) && !hand_passive(0, 0x01234567, 0.0f) && !hand_passive(0, 0, 0.5f));
    CHECK(!hand_passive(-1, -1, 0.0f) && hand_passive(0, 0, NAN));
    for (size_t m = 0; m < sizeof multipliers / sizeof multipliers[0]; m++) {
        hand_follow_t f = { 0 };
        float body[3] = { 10.0f, -5.0f, 2.0f }, bv[3] = { 0, 0, 0 }, start_body[3], start_hand[3], err = 0.0f;
        float hand[3] = { 10.3f, -4.8f, 1.6f }, hv[3] = { 0, 0, 0 }, moved;
        unsigned seed = 12345u;
        memcpy(start_body, body, sizeof body);
        memcpy(start_hand, hand, sizeof hand);
        for (int step = 0; step < 30000; step++) {
            hand_follow_enter(&f, 1, body, bv, hand, hv);
            for (int i = 0; i < 3; i++) CHECK(fabsf(hv[i] - 1.25f * bv[i]) < 1e-3f * (1.0f + fabsf(bv[i])));
            hand_follow_leave(&f, hv);
            for (int i = 0; i < 3; i++) {
                float kick, acc;
                seed = seed * 1103515245u + 12345u;
                kick = ((seed >> 16) & 1023u) / 1023.0f - 0.5f;
                acc = multipliers[m] * (40.0f * kick + (i == 0 ? 3.0f : 0.0f));
                body[i] += HAND_DT * bv[i];
                hand[i] += HAND_DT * hv[i];
                bv[i] += HAND_DT * acc;
                if ((step % 97) == 0) bv[i] *= 0.6f;
                if ((step % 211) == 0) body[i] += 0.05f * kick;
            }
        }
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        for (int i = 0; i < 3; i++) err = fmaxf(err, fabsf((hand[i] - start_hand[i]) - (body[i] - start_body[i])));
        moved = length3(body, start_body);
        printf("hand at rest, %.0fx: body moved %.0f m, hand off the body's displacement by %.5f m\n", multipliers[m],
               moved, err);
        CHECK(moved > 100.0f * multipliers[m] && err < 2e-3f);
    }
    /* controlled: nothing changes; leaving the passive state gives the world velocity back */
    {
        hand_follow_t f = { 0 };
        float body[3] = { 1, 2, 3 }, bv[3] = { 12.0f, -3.0f, 0.5f }, hand[3] = { 1.3f, 2.1f, 2.7f }, hv[3] = { 4, 5, 6 };
        float world[3];
        for (int phase = 0; phase < 3; phase++) {
            hand_follow_enter(&f, 0, body, bv, hand, hv);
            CHECK(!f.passive && hand[0] == 1.3f && hv[0] == 4.0f && hv[1] == 5.0f && hv[2] == 6.0f);
            hand_follow_leave(&f, hv);
            CHECK(hv[0] == 4.0f);
            body[0] += 0.12f;
        }
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        hand_follow_leave(&f, hv);
        hand_follow_world_velocity(&f, hv, world);
        CHECK(fabsf(world[0] - 4.0f) < 1e-5f && fabsf(hv[0] - (4.0f - 12.0f)) < 1e-5f && hand[0] == 1.3f);
        body[0] += 0.12f;
        hand[0] += HAND_DT * hv[0];
        hand_follow_enter(&f, 0, body, bv, hand, hv); /* Move Hand pressed: last step's ride, then world velocity */
        CHECK(!f.passive && fabsf(hv[0] - 4.0f) < 1e-5f && fabsf(hand[0] - (1.3f - 0.08f + 0.12f)) < 1e-5f);
        body[0] += 0.12f;
        hand_follow_enter(&f, 0, body, bv, hand, hv);
        CHECK(fabsf(hand[0] - (1.3f - 0.08f + 0.12f)) < 1e-5f && fabsf(hv[0] - 4.0f) < 1e-5f);
    }
    /* a teleport or a NaN is not ridden */
    {
        hand_follow_t f = { 0 };
        float body[3] = { 0, 0, 0 }, bv[3] = { 0, 0, 0 }, hand[3] = { 0.3f, 0, -0.3f }, hv[3] = { 0, 0, 0 };
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        hand_follow_leave(&f, hv);
        body[0] = 500.0f;
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        CHECK(hand[0] == 0.3f && f.passive);
        hand_follow_leave(&f, hv);
        body[1] = NAN;
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        CHECK(!f.passive && !f.valid && hand[0] == 0.3f && isfinite(hv[0]));
    }
    /* the safety recovery puts the hand at rest relative to the body */
    {
        hand_follow_t f = { 0 };
        float body[3] = { 0, 0, 0 }, bv[3] = { 9.0f, 0, 0 }, hand[3] = { 0.3f, 0, -0.3f }, hv[3] = { 0, 0, 0 }, world[3];
        hand_follow_enter(&f, 1, body, bv, hand, hv);
        hand_follow_leave(&f, hv);
        hand_follow_rebase(&f, body, bv, hv);
        hand_follow_world_velocity(&f, hv, world);
        CHECK(hv[0] == 0.0f && world[0] == 9.0f);
        f.passive = 0;
        hand_follow_rebase(&f, body, bv, hv);
        CHECK(hv[0] == 9.0f);
    }
}

/* The carrier alone, against a body whose velocity is accelerated, set and removed and whose position is rebuilt with
 * steps it did not integrate: after any number of steps the hand has been moved by k times the body's displacement.
 * The previous fix's carrier (k times the integral of the body's acceleration) falls behind by more with every metre. */
static void test_hand_carrier_exact(void) {
    hand_carrier_t c = { 0 };
    float body[3] = { 10.0f, -5.0f, 2.0f }, bv[3] = { 0, 0, 0 }, hand[3] = { 10.3f, -4.8f, 1.6f }, hv[3] = { 0, 0, 0 };
    float start_body[3], start_hand[3], k = hand_frame_share(7.5f), err = 0.0f, half_err = 0.0f, half_moved = 0.0f;
    float old_pos[3] = { 0, 0, 0 }, old_vel[3] = { 0, 0, 0 }, old_err = 0.0f, old_half = 0.0f, moved;
    unsigned seed = 12345u;
    memcpy(start_body, body, sizeof body);
    memcpy(start_hand, hand, sizeof hand);
    for (int step = 0; step <= 60000; step++) {
        hand_carrier_step(&c, k, HAND_DT, body, bv, hand, hv);
        if (step == 30000 || step == 60000) {
            float e = 0.0f, o = 0.0f;
            for (int i = 0; i < 3; i++) {
                e = fmaxf(e, fabsf(hand[i] - start_hand[i] - k * (body[i] - start_body[i])));
                o = fmaxf(o, fabsf(old_pos[i] - k * (body[i] - start_body[i])));
            }
            if (step == 30000) {
                half_err = e;
                old_half = o;
                half_moved = length3(body, start_body);
            } else {
                err = e;
                old_err = o;
            }
        }
        if (step == 60000) break;
        for (int i = 0; i < 3; i++) {
            float kick, acc;
            seed = seed * 1103515245u + 12345u;
            kick = ((seed >> 16) & 1023u) / 1023.0f - 0.5f;
            acc = 40.0f * kick + (i == 0 ? 2.0f : 0.0f);
            body[i] += HAND_DT * bv[i];
            hand[i] += HAND_DT * hv[i];
            old_pos[i] += HAND_DT * old_vel[i];
            bv[i] += HAND_DT * acc;
            old_vel[i] += HAND_DT * k * acc;
            if ((step % 97) == 0) bv[i] *= 0.6f;
            if ((step % 211) == 0) body[i] += 0.05f * kick;
        }
    }
    moved = length3(body, start_body);
    printf("hand carrier: body moved %.0f m (%.0f m at half way); carried hand off k x that by %.4f m (%.4f), the "
           "previous fix's carrier by %.1f m (%.1f)\n",
           moved, half_moved, err, half_err, old_err, old_half);
    CHECK(moved > 500.0f && half_moved > 100.0f);
    CHECK(err < 5e-4f * moved && half_err < 5e-4f * half_moved); /* float rounding of km-sized coordinates */
    CHECK(old_err > 60.0f && old_err > 1.5f * old_half);
    /* 1x and below, NaN, a teleport: nothing is carried */
    {
        hand_carrier_t d = { 0 };
        float p[3] = { 0, 0, 0 }, v[3] = { 1, 0, 0 }, h[3] = { 1, 2, 3 }, w[3] = { 4, 5, 6 };
        hand_carrier_step(&d, hand_frame_share(1.0f), HAND_DT, p, v, h, w);
        CHECK(!d.valid && h[0] == 1.0f && w[0] == 4.0f);
        hand_carrier_step(&d, hand_frame_share(0.5f), HAND_DT, p, v, h, w);
        CHECK(!d.valid);
        hand_carrier_step(&d, 0.5f, HAND_DT, p, v, h, w);
        p[0] = 500.0f;
        v[0] = 3.0f;
        hand_carrier_step(&d, 0.5f, HAND_DT, p, v, h, w);
        CHECK(d.valid && h[0] == 1.0f && w[0] == 4.0f);
        p[0] = NAN;
        hand_carrier_step(&d, 0.5f, HAND_DT, p, v, h, w);
        CHECK(!d.valid && h[0] == 1.0f);
        w[0] = 4.0f;
        hand_damping_shift(0.0f, 1.0f, v, w);
        CHECK(w[0] == 4.0f);
        hand_damping_shift(0.8f, 1.0f, v, w);
        hand_damping_shift(0.8f, -1.0f, v, w);
        CHECK(w[0] == 4.0f);
    }
    CHECK(hand_frame_share(1.0f) == 0.0f && hand_frame_share(0.5f) == 0.0f && fabsf(hand_frame_share(5.0f) - 0.8f) < 1e-6f);
    CHECK(fabsf(hand_frame_share(10.0f) - 0.9f) < 1e-6f);
}

/* At least 60 s and 500 m at every multiplier, Move Hand off (riding the pelvis) and on (the carrier), on smooth and
 * bumpy ground: never past the drop distance and, at 10, 50, 100 and 500 m alike, never farther from the pelvis than
 * the game's own hand at 1x. */
static void test_hand_long_run(void) {
    static const float speeds[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.5f, 10.0f };
    static const float bumps[] = { 0.0f, 0.2f };
    for (size_t b = 0; b < sizeof bumps / sizeof bumps[0]; b++) {
        hand_course_t one_c = hand_course(1.0f, HAND_RETAIL, bumps[b]);
        hand_run_t one = hand_run(&one_c);
        CHECK(!one.dropped && one.worst < HAND_DROP_M);
        for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
            hand_course_t c = hand_course(speeds[i], HAND_FOLLOW, bumps[b]), on_c = c, retail_c = c;
            hand_run_t r, on, retail;
            on_c.move_hand = 1;
            retail_c.fix = HAND_RETAIL;
            r = hand_run(&c);
            on = hand_run(&on_c);
            retail = hand_run(&retail_c);
            printf("hand %.1fx bumps %.1f: %.0f m, pelvis->hand max %.3f (10 m %.3f, 50 m %.3f, 100 m %.3f, 500 m %.3f), "
                   "Move Hand on %.3f; uncarried %.3f%s\n",
                   speeds[i], bumps[b], r.travelled, r.worst, r.band[BAND_10], r.band[BAND_50], r.band[BAND_100],
                   r.band[BAND_500], on.worst, retail.worst, retail.dropped ? " (dropped)" : "");
            CHECK(r.finite && on.finite && r.travelled >= 500.0f && on.travelled >= 500.0f);
            CHECK(!r.dropped);
            for (int band = 0; band < BANDS; band++) CHECK(r.band[band] <= one.worst + 0.05f);
            CHECK(r.final_distance < 1.0f && on.final_distance < 1.0f);
            CHECK(!on.dropped && on.worst <= one.worst + 0.05f);
        }
    }
    /* uncarried, the hand is let go from 5x on */
    {
        hand_course_t u = hand_course(5.0f, HAND_RETAIL, 0.0f);
        CHECK(hand_run(&u).dropped);
    }
    /* caught on something for 1 s in every 5 (log 213: hand 0.4-3 m/s against a 10-13 m/s body): left behind
     * uncarried at any speed, riding the pelvis at every one */
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        hand_course_t c = hand_course(speeds[i], HAND_FOLLOW, 0.2f), u;
        hand_run_t r, left;
        c.snag = 1;
        u = c;
        u.fix = HAND_RETAIL;
        r = hand_run(&c);
        left = hand_run(&u);
        printf("hand snagged %.1fx: riding max %.3f m (end %.3f); uncarried %.3f m%s\n", speeds[i], r.worst,
               r.final_distance, left.worst, left.dropped ? " (dropped)" : "");
        CHECK(r.finite && !r.dropped && r.worst < 1.0f && r.final_distance < 1.0f);
        if (speeds[i] >= 1.0f) CHECK(left.dropped || left.worst > r.worst + 0.5f);
    }
}

/* Log 073 at 7.5x, Jump Height 3x: from the second HAND FAR (pelvis -1657.4 -706.0 Z 40.1) uphill towards where the
 * pelvis was lost (-1621.5 -640.5 Z 45.6), weaving, then on to 500 m; then the full course on that ground. The previous
 * fix ran the hand at the edge of the drop distance (log 073's 1.645 and 1.650 m) and let it go on rougher ground or at
 * 10x; carried, the hand stays where it is at rest. */
static void test_hand_log_073(void) {
    const float from[3] = { -1657.445f, -706.020f, 40.054f }, to[3] = { -1621.484f, -640.455f, 45.558f };
    float run = hypotf(to[0] - from[0], to[1] - from[1]), grade = (to[2] - from[2]) / run;
    static const float bumps[] = { 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f };
    static const float speeds[] = { 7.5f, 10.0f };
    float previous_worst = 0.0f;
    int previous_drops = 0;
    hand_course_t c;
    memset(&c, 0, sizeof c);
    c.intensity = 0.7f;
    c.seconds = 60.0f;
    c.metres = 500.0f;
    c.heading = atan2f(to[1] - from[1], to[0] - from[0]);
    c.ground.ox = from[0];
    c.ground.oy = from[1];
    c.ground.oz = from[2];
    c.ground.gx = grade * (to[0] - from[0]) / run;
    c.ground.gy = grade * (to[1] - from[1]) / run;
    for (int course = 1; course >= 0; course--)
        for (size_t s = 0; s < sizeof speeds / sizeof speeds[0]; s++)
            for (size_t b = 0; b < sizeof bumps / sizeof bumps[0]; b++) {
                hand_run_t previous, carried;
                c.course_073 = course;
                c.m = speeds[s];
                c.ground.bump = bumps[b];
                c.fix = HAND_PREVIOUS;
                previous = hand_run(&c);
                c.fix = HAND_FOLLOW;
                carried = hand_run(&c);
                if (previous.dropped) previous_drops++;
                else previous_worst = fmaxf(previous_worst, previous.worst);
                CHECK(carried.finite && !carried.dropped && carried.worst < 0.7f && carried.final_distance < 1.0f);
                for (int band = 0; band < BANDS; band++) CHECK(carried.band[band] < 0.7f);
                if (course && s == 0 && b == 1)
                    printf("log 073 course: carried pelvis->hand max %.3f m over %.0f m (end %.3f); previous fix max %.3f m%s\n",
                           carried.worst, carried.travelled, carried.final_distance, previous.worst,
                           previous.dropped ? ", hand let go" : "");
            }
    printf("log 073 ground, 7.5x and 10x: previous fix let the hand go in %d of 24 runs, the others reached %.3f m\n",
           previous_drops, previous_worst);
    CHECK(previous_drops >= 4 && previous_worst > 1.4f);
}

/* InfoBox discipline (0x004DCA47): a box at 100 m/s or more deactivates the pelvis. Body and hand stay well under it at
 * every multiplier, carried, and nothing is NaN or infinite. */
static void test_speed_sanity(void) {
    static const float speeds[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.5f, 10.0f };
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        hand_course_t c = hand_course(speeds[i], HAND_FOLLOW, 0.2f);
        hand_run_t r = hand_run(&c);
        printf("sanity at %.1fx: body %.2f m/s, hand %.2f m/s\n", speeds[i], r.body_speed, r.hand_speed);
        CHECK(r.finite && r.body_speed < 50.0f && r.hand_speed < 50.0f);
    }
}

static void test_window_title(void) {
    static const char base[] = "Jurassic Park: Trespasser Recompiled | x86 | D3D11";
    char out[160];
    title_content_t c;
    /* the in-game counter and the title print the same measured rate the same way */
    title_fps_text(60.04, out, sizeof out);
    CHECK(!strcmp(out, "FPS: 60.0"));
    title_fps_text(59.95, out, sizeof out);
    CHECK(!strcmp(out, "FPS: 60.0") || !strcmp(out, "FPS: 59.9"));
    title_fps_text(144.0, out, 6);
    CHECK(!strcmp(out, "FPS: ") && strlen(out) == 5);
    memset(&c, 0, sizeof c);
    c.base = base;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, base));
    c.width = 2560;
    c.height = 1920;
    c.fps = 60.04;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, base));
    c.show_fps = 1;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, base));
    c.fps_ready = 1;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, "Jurassic Park: Trespasser Recompiled | x86 | D3D11 | FPS: 60.0"));
    c.show_fps = 0;
    c.show_resolution = 1;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, "Jurassic Park: Trespasser Recompiled | x86 | D3D11 | 2560x1920"));
    c.show_fps = 1;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, "Jurassic Park: Trespasser Recompiled | x86 | D3D11 | 2560x1920 | FPS: 60.0"));
    c.fps_ready = 0;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, "Jurassic Park: Trespasser Recompiled | x86 | D3D11 | 2560x1920"));
    c.width = c.height = 0;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, base) && !strstr(out, "0x0"));
    c.width = 1920;
    c.height = 1440;
    c.fps_ready = 1;
    title_compose(&c, out, sizeof out);
    CHECK(!strcmp(out, "Jurassic Park: Trespasser Recompiled | x86 | D3D11 | 1920x1440 | FPS: 60.0"));
    c.width = 3840;
    c.height = 2880;
    title_compose(&c, out, sizeof out);
    CHECK(strstr(out, "| 3840x2880 |") && !strstr(out, "1920x1440") && out[strlen(out) - 1] != '|' && !strstr(out, "| |") && !strstr(out, "||"));
    title_compose(&c, out, 24);
    CHECK(strlen(out) == 23 && !strncmp(out, base, 23));
}

static void test_expected_misses(void) {
    CHECK(host_expected_miss("FindFirstFileA", "*.swp", 2));
    CHECK(host_expected_miss("FindFirstFileA", "C:\\port\\user_data\\saves\\*.SWP", 2));
    CHECK(!host_expected_miss("FindFirstFileA", "*.swp", 3));
    CHECK(!host_expected_miss("FindFirstFileA", "*.sav", 2));
    CHECK(!host_expected_miss("FindFirstFileA", "level.swp", 2));
    CHECK(!host_expected_miss("CreateFileA", "*.swp", 2));
    CHECK(!host_expected_miss("LoadLibraryA", "A3d.dll", 126));
    CHECK(!host_expected_miss(NULL, "*.swp", 2));
    CHECK(host_unavailable_library("A3d.dll"));
    CHECK(host_unavailable_library("a3d.dll"));
    CHECK(host_unavailable_library("A3D.DLL"));
    CHECK(host_unavailable_library("C:\\x\\a3d.dll"));
    CHECK(!host_unavailable_library("DSound.dll"));
    CHECK(!host_unavailable_library("d3dcompiler_47.dll"));
    CHECK(!host_unavailable_library("xA3d.dll"));
    CHECK(!host_unavailable_library("A3d.dll.bak"));
    CHECK(!host_unavailable_library(NULL));
}

/* The game's WM_ACTIVATEAPP handler as it behaves: deactivated it releases the screen; reactivated with the screen gone it
 * recreates the display and rebuilds the sky texture from the sky's source raster, whose pixels a level load keeps in
 * paged memory the loader thread commits as it goes. */
typedef struct {
    int screen, rebuilds, faults, pages_committed, surface_locked;
} game_model_t;

static void game_activate(game_model_t* g, int active) {
    if (!active) {
        g->screen = 0;
        return;
    }
    if (g->screen) return;
    g->screen = 1;
    g->rebuilds++;
    if (!g->pages_committed) g->faults++;
}

static void windows_activation(game_model_t* g, activation_defer_t* a, int loading, int active, int defer) {
    if (!defer || activation_message(a, loading, active)) game_activate(g, active);
}

static void load_level(game_model_t* g, activation_defer_t* a, const int* changes, int count, int defer) {
    int active;
    g->pages_committed = 0;
    g->surface_locked = 1;
    for (int i = 0; i < count; i++) windows_activation(g, a, 1, changes[i], defer);
    g->surface_locked = 0;
    g->pages_committed = 1;
    if (defer && activation_after_load(a, &active)) game_activate(g, active);
}

static void test_activation_during_load(void) {
    static const int away_and_back[] = { 0, 1 };
    static const int many[] = { 0, 1, 0, 1, 0, 1 };
    static const int away[] = { 0 };
    static const int away_back_away[] = { 0, 1, 0 };
    activation_defer_t a;
    game_model_t g;
    int active = -1;

    memset(&g, 0, sizeof g);
    g.screen = g.pages_committed = 1;
    activation_defer_init(&a);
    load_level(&g, &a, away_and_back, 2, 0);
    CHECK(g.faults == 1); /* the model reproduces log 148: the rebuild reads the uncommitted page */

    memset(&g, 0, sizeof g);
    g.screen = g.pages_committed = 1;
    activation_defer_init(&a);
    load_level(&g, &a, away_and_back, 2, 1);
    CHECK(g.faults == 0 && g.rebuilds == 0 && g.screen == 1); /* away and back during the load: nothing to redo */
    load_level(&g, &a, many, 6, 1);
    CHECK(g.faults == 0 && g.rebuilds == 0 && g.screen == 1);

    load_level(&g, &a, away, 1, 1);
    CHECK(g.faults == 0 && g.screen == 0 && a.game_active == 0); /* still away when it ends: the game is told then */
    windows_activation(&g, &a, 0, 1, 1);
    CHECK(g.screen == 1 && g.rebuilds == 1 && g.faults == 0); /* back in gameplay: restored with the pages in place */

    load_level(&g, &a, away_back_away, 3, 1);
    CHECK(g.faults == 0 && g.screen == 0 && a.game_active == 0);
    windows_activation(&g, &a, 0, 1, 1);
    CHECK(g.screen == 1 && g.faults == 0);

    activation_defer_init(&a);
    CHECK(activation_message(&a, 0, 0) && a.game_active == 0); /* outside a load it goes through at once */
    CHECK(activation_message(&a, 0, 1) && a.game_active == 1);
    CHECK(!activation_after_load(&a, &active) && active == -1); /* no load, nothing kept */
    CHECK(!activation_message(&a, 1, 0) && a.game_active == 1 && a.deferred);
    activation_delivered(&a, 1); /* the quit path reactivates the game itself */
    CHECK(activation_after_load(&a, &active) && active == 0 && a.game_active == 0 && !a.deferred);
}

int main(void) {
    test_profile_activate();
    test_activation_during_load();
    test_window_title();
    test_hand_follow();
    test_hand_carrier_exact();
    test_hand_recovery();
    test_hand_long_run();
    test_hand_log_073();
    test_speed_sanity();
    test_fps_counter();
    test_input_state();
    test_capture_policy();
    test_keys();
    test_pointer_relative();
    test_pointer_recapture();
    test_shutdown();
    test_quit_steps();
    test_safemode();
    test_open_press();
    test_target_access_policy();
    test_scene_heap_overflow();
    test_cpu_polygon_tiles();
    test_cpu_polygon_origin();
    test_store_only_polygon_callers();
    test_transparent_skip_store_only();
    test_read_modify_write_needs_the_box();
    test_texture_anomalies();
    test_polygon_spans();
    test_render_state_batches();
    test_dirty_boxes();
    test_colour_keyed_span_store_only();
    test_perf_stat();
    test_lazy_capture();
    test_lazy_marks_not_published();
    test_cache_never_and_groups();
    test_scene_heap_stress();
    test_text_box_capture();
    test_cpu_polygon_box_readback();
    test_readback_makes_the_box_current();
    test_planar_gradient_limit();
    test_terrain_shadow_pool();
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
    test_address_and_cull();
    test_readback_classes();
    test_primitives();
    test_states();
    test_texture_blend();
    test_terrain_blend();
    test_hardware_water();
    test_skip_intro();
    test_pelvis_clamps();
    test_draw_constants();
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
    test_texture_gpu();
    test_texture_gpu_rows();
    test_texture_gpu_session();
    test_texture_gpu_run();
    test_texture_gpu_bump();
    test_texture_simd_convert();
    test_texture_simd_alpha();
    test_texture_simd_mips();
    bench_texture_simd();
    test_texture_gpu_cache();
    test_fill_runs();
    test_overlay_row_sync();
    test_mip_downsample_exact();
    test_dynamic_textures();
    test_sampler_choice();
    test_readback_regions();
    test_write_only_locks();
    test_ui_frame_without_readback();
    test_engine_limits();
    test_engine_texture_layout();
    test_terrain_settings();
    test_engine_upload_clock();
    test_subtitles_default_migration();
    test_page_arena();
    test_expected_misses();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}
