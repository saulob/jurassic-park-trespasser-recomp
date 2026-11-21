#include "linux_hostkeys.h"

#include <string.h>

#include "physics_watch.h"

#include "debug_menu.h"
#include "guest_features.h"
#include "host_input.h"
#include "linux_features.h"
#include "linux_guest_ui.h"
#include "linux_log.h"
#include "overlay_policy.h"
#include "linux_overlay.h"
#include "linux_quick.h"
#include "linux_shutdown.h"
#include "linux_window.h"

static lx_hostkeys_counts_t s_counts;
static unsigned s_unavailable_logged;

void lx_hostkeys_reset(void) {
    memset(&s_counts, 0, sizeof s_counts);
    s_unavailable_logged = 0;
    key_policy_set_quick_keys(0, 0);
}

void lx_hostkeys_set_quick_keys(unsigned save_vk, unsigned load_vk) { key_policy_set_quick_keys(save_vk, load_vk); }

static void no_renderer(const char* what) {
    s_counts.unavailable++;
    if (s_unavailable_logged++ < 4) lx_log("hotkeys", "%s has no Linux renderer yet: the key is the port's and the game does not see it", what);
}

static int menu_keys_apply(key_action_t action) {
    return action != KEY_HOST_MINIMAP && action != KEY_HOST_CHEATS && action != KEY_HOST_OBJECTIVES && action != KEY_HOST_CONTROLS &&
           action != KEY_HOST_FULLSCREEN && action != KEY_HOST_CLOSE && action != KEY_HOST_SHOW_OBJECTIVE && action != KEY_HOST_FRAME_COUNTER;
}

int lx_hostkeys_filter(unsigned msg, unsigned vk, int repeat, int shift, int ctrl, int alt) {
    key_action_t action = key_policy(msg, vk, repeat, ctrl, alt);
    const int down = msg == 0x100u || msg == 0x104u, host = lx_features_running();
    if (host && overlay_key_event(vk, down, repeat)) return 1;
    if (host && menu_keys_apply(action) && debug_menu_key_event(vk, down, repeat, shift, ctrl)) return 1;
    switch (action) {
    case KEY_TO_GAME: return 0;
    case KEY_DROP: return 1;
    case KEY_HOST_FULLSCREEN:
        s_counts.fullscreen++;
        if (lx_window_backend()->toggle_fullscreen) lx_window_backend()->toggle_fullscreen(lx_window_backend());
        return 1;
    case KEY_HOST_FRAME_COUNTER:
        s_counts.frame_counter++;
        lx_overlay_toggle_frame_counter();
        lx_log("fps", "F6: frame counter %s", lx_overlay_frame_counter_on() ? "on" : "off");
        return 1;
    case KEY_HOST_QUICK_SAVE:
        s_counts.quick_save++;
        lx_quick_request(1);
        return 1;
    case KEY_HOST_QUICK_LOAD:
        s_counts.quick_load++;
        lx_quick_request(0);
        return 1;
    case KEY_HOST_CLOSE:
        s_counts.close++;
        lx_shutdown_request(LX_QUIT_ALT_F4);
        return 1;
    case KEY_HOST_OBJECTIVES:
        if (host) overlay_request(OV_OBJECTIVES, "F3");
        else no_renderer("F3 Objectives");
        return 1;
    case KEY_HOST_MINIMAP:
        if (host) lx_features_toggle_minimap("F5");
        else no_renderer("F5 minimap");
        return 1;
    case KEY_HOST_CHEATS:
        if (host) overlay_request(OV_CHEATS, "F10");
        else no_renderer("F10 Cheats");
        return 1;
    case KEY_HOST_CONTROLS:
        if (host) overlay_request(OV_CONTROLS, "F2");
        else no_renderer("F2 controls card");
        return 1;
    case KEY_HOST_SHOW_OBJECTIVE: {
        guest_ui_view_t v;
        if (host && lx_guest_ui_probe(&v) && v.ui == GUEST_UI_GAMEPLAY && !debug_menu_open_menu()) debug_menu_show_objective();
        else if (!host) no_renderer("F4 Show Objective");
        return 1;
    }
    case KEY_HOST_SNAPSHOT:
        if (host) physics_watch_request();
        return 1;
    }
    return 0;
}

const lx_hostkeys_counts_t* lx_hostkeys_counts(void) { return &s_counts; }
