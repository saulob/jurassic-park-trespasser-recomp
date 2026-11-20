/*
 * The one owner of the host's interfaces over gameplay: the controls card (an entry of the game's text overlay),
 * Objectives and Cheats. Requests from keys and the gamepad run in the game's UI loop, the previous
 * interface closed first; the game's Hint (F1) is let through only once nothing of the host is left on the picture.
 */
#include "host.h"

#include "overlay_policy.h"
#include "tutorial_prompts.h"

#define LOG_LIMIT 48
#define HINT_WAIT_PASSES 8

static struct {
    ov_kind_t request, active;
    const char* source;
    int card, level, hint_wait, logs;
    ov_keys_t keys;
} O;

static void note(const char* fmt, const char* a, const char* b, const char* c) {
    if (O.logs >= LOG_LIMIT) return;
    if (++O.logs == LOG_LIMIT) host_log("overlay", "log limit reached");
    else host_log("overlay", fmt, a, b, c);
}

void overlay_request(int kind, const char* source) {
    O.request = (ov_kind_t)kind;
    O.source = source;
}

static ov_kind_t menu_kind(void) {
    switch (debug_menu_open_menu()) {
    case 1: return OV_CHEATS;
    case 2: return OV_OBJECTIVES;
    default: return OV_NONE;
    }
}

static void close_kind(ov_kind_t kind) {
    if (kind == OV_CONTROLS) tutorial_prompts_hide_card();
    else if (kind == OV_OBJECTIVES || kind == OV_CHEATS) debug_menu_close();
}

static void open_kind(ov_kind_t kind) {
    if (kind == OV_CONTROLS) {
        if (O.level) tutorial_prompts_show_card();
    } else if (kind == OV_OBJECTIVES) {
        debug_menu_f3();
    } else if (kind == OV_CHEATS) {
        debug_menu_f10();
    }
}

void overlay_ui_loop(void) {
    guest_ui_view_t v;
    ov_plan_t plan;
    int native;
    guest_ui_probe(&v);
    O.level = v.depth == 1 && v.kind == GUEST_WND_GAME && v.ui == GUEST_UI_GAMEPLAY;
    native = !O.level && !(v.depth == 1 && v.kind == GUEST_WND_MAIN_MENU);
    O.card = O.level && tutorial_prompts_card_shown();
    O.active = menu_kind() != OV_NONE ? menu_kind() : O.card ? OV_CONTROLS : OV_NONE;
    if (O.hint_wait) {
        if (!O.level) {
            O.hint_wait = 0;
            return;
        }
        if (O.active != OV_NONE && ++O.hint_wait <= HINT_WAIT_PASSES) return;
        O.hint_wait = 0;
        window_host_post_key(VK_F1);
        return;
    }
    if (O.request == OV_NONE && !O.source) return;
    plan = ov_plan(O.active, O.request, native);
    if (plan.close != OV_NONE) close_kind(plan.close);
    if (plan.open == OV_HINT) {
        if (plan.close != OV_NONE) O.hint_wait = 1;
        else window_host_post_key(VK_F1);
    } else if (plan.open != OV_NONE) {
        open_kind(plan.open);
    }
    if (plan.close != OV_NONE || plan.open != OV_NONE)
        note("%s -> %s (%s)", ov_name(plan.close), ov_name(plan.open), O.source ? O.source : "?");
    O.request = OV_NONE;
    O.source = NULL;
    if (plan.close != OV_NONE || plan.open != OV_NONE) gamepad_overlay_changed();
}

ov_kind_t overlay_active(void) { return O.active; }

int overlay_blocks_gameplay(void) { return debug_menu_blocks_gameplay() || O.card || O.hint_wait; }

int overlay_key(UINT msg, WPARAM wparam, LPARAM lparam) {
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, repeat = (lparam & 0x40000000) != 0;
    unsigned vk = (unsigned)wparam & 0xFFu;
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return 0;
    if (ov_keys_filter(&O.keys, vk, down)) return 1;
    if (!down || repeat) return 0;
    if (vk == VK_F1 && (O.active != OV_NONE || debug_menu_visible())) {
        ov_keys_take(&O.keys, vk);
        overlay_request(OV_HINT, "F1");
        return 1;
    }
    if (vk == VK_ESCAPE && O.active == OV_CONTROLS) {
        ov_keys_take(&O.keys, vk);
        overlay_request(OV_NONE, "Esc");
        note("%s closed by %s, %s held back until released", "controls", "Esc", "Esc");
        return 1;
    }
    return 0;
}
