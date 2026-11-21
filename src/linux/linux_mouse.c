#include "linux_mouse.h"

#include <stdlib.h>
#include <string.h>

#include "active_profile.h"
#include "debug_menu.h"
#include "linux_guest_ui.h"
#include "linux_log.h"
#include "linux_screen.h"
#include "linux_shutdown.h"
#include "linux_user32.h"
#include "linux_window.h"

static struct {
    int focused;
    int count;
    input_state_t state;
    int relative, cursor_hidden;
    int applied;
    guest_ui_view_t view;
    float sensitivity;
    mouse_scale_t scale;
    lx_mouse_stats_t stats;
    int stats_logging;
} S = { .focused = 1, .state = INPUT_MENU, .sensitivity = 1.0f };

void lx_mouse_reset(void) {
    memset(&S, 0, sizeof S);
    S.focused = 1;
    S.state = INPUT_MENU;
    S.sensitivity = 1.0f;
}

static int stats_wanted(void) {
    const char* value = getenv("JPT_INPUT_STATS");
    return value && *value && *value != '0';
}

void lx_mouse_note(lx_mouse_note_t kind) {
    switch (kind) {
    case LX_MN_RAW_MOTION: S.stats.raw_motions++; break;
    case LX_MN_ABSOLUTE_MOTION: S.stats.absolute_motions++; break;
    case LX_MN_RELATIVE_MOTION: S.stats.relative_motions++; break;
    case LX_MN_BUTTON: S.stats.buttons++; break;
    case LX_MN_WHEEL: S.stats.wheels++; break;
    }
}

void lx_mouse_stats(lx_mouse_stats_t* out) { *out = S.stats; }

void lx_mouse_log_summary(void) {
    if (!stats_wanted()) return;
    lx_log("Linux input", "mouse: %u SDL motions (%u absolute, %u relative reached the guest), %u button events, %u wheel events, %u context switches",
           S.stats.raw_motions, S.stats.absolute_motions, S.stats.relative_motions, S.stats.buttons, S.stats.wheels, S.stats.context_switches);
}

void lx_mouse_set_sensitivity(float value) {
    S.sensitivity = value;
    memset(&S.scale, 0, sizeof S.scale);
}

void lx_mouse_set_focus(int focused) {
    S.focused = focused != 0;
    lx_mouse_update();
}

void lx_mouse_set_cursor_count(int count) {
    S.count = count;
    lx_mouse_update();
}

static input_state_t derive(void) {
    input_signals_t signals;
    memset(&signals, 0, sizeof signals);
    lx_guest_ui_probe(&S.view);
    signals.shutdown = lx_shutdown_state() >= LX_QUIT_REQUESTED;
    signals.focused = S.focused;
    signals.ui = S.view.ui == GUEST_UI_GAMEPLAY && debug_menu_visible() ? GUEST_UI_MENU : S.view.ui;
    return input_state_from(&signals);
}

void lx_mouse_update(void) {
    input_state_t before = S.state, state = derive();
    int relative = input_captured(state), hidden = input_hide_pointer(state, S.count < 0), changed = 0;
    long x, y;
    int width, height;
    lx_display_mode_t mode = lx_screen_mode();
    if (!lx_screen_window_is_open()) {
        S.state = state;
        return;
    }
    width = mode.width, height = mode.height;
    if (relative != S.relative || !S.applied) {
        lx_window_backend()->set_relative_mouse(lx_window_backend(), relative);
        S.relative = relative;
        changed = 1;
    }
    if (hidden != S.cursor_hidden || !S.applied) {
        lx_window_backend()->set_cursor_visible(lx_window_backend(), !hidden);
        S.cursor_hidden = hidden;
        changed = 1;
    }
    S.state = state;
    if (state != before && S.applied) {
        S.stats.context_switches++;
        lx_mouse_log_summary();
    }
    if (lx_user32_cursor_get(&x, &y) && pointer_recapture(before, state, &x, &y, width, height)) lx_user32_cursor_set((int)x, (int)y);
    if (changed && S.applied)
        lx_log("Linux mouse", "relative %s, pointer %s (%s, the game's %s in front); the guest's ClipCursor and capture stay logical", S.relative ? "on" : "off",
               S.cursor_hidden ? "hidden" : "shown", input_state_name(state), guest_ui_model_class_name(jpt_gva_canonical, S.view.top_vtable));
    S.applied = 1;
}

int lx_mouse_motion(long dx, long dy, long* x, long* y) {
    long sx = dx, sy = dy;
    if (input_captured(S.state)) mouse_scale_counts(&S.scale, S.sensitivity, dx, dy, &sx, &sy);
    return pointer_add_relative(S.state, x, y, sx, sy);
}

int lx_mouse_cursor_virtual(void) { return input_cursor_virtual(S.state); }

void lx_mouse_snapshot(lx_mouse_snapshot_t* out) {
    memset(out, 0, sizeof *out);
    out->state = S.state;
    out->ui = S.view.ui;
    out->top_kind = S.view.kind;
    out->focused = S.focused;
    out->relative = S.relative;
    out->cursor_hidden = S.cursor_hidden;
    out->game_cursor_hidden = S.count < 0;
    out->guest_capture = (int)lx_user32_capture();
    out->guest_clip = lx_user32_clipped();
}
