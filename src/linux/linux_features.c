#include "linux_features.h"

#include <stdio.h>
#include <string.h>

#include "active_profile.h"
#include "cheats.h"
#include "debug_menu.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "guest_memory.h"
#include "guest_ui_model.h"
#include "guest_va.h"
#include "host_input.h"
#include "l10n.h"
#include "linux_gamepad.h"
#include "linux_guest_ui.h"
#include "linux_host.h"
#include "linux_log.h"
#include "linux_mouse.h"
#include "linux_overrides.h"
#include "linux_panel.h"
#include "linux_resource.h"
#include "linux_card.h"
#include "linux_frontend.h"
#include "linux_text.h"
#include "jpt_l10n.h"
#include "linux_user32.h"
#include "minimap.h"
#include "overlay_text.h"
#include "tutorial_prompts.h"

#define RT_STRING 6u
#define STRING_BLOCK 16u
#define DATA_DIR_MAX 512

static char s_game_dir[DATA_DIR_MAX], s_settings_file[DATA_DIR_MAX];
static int s_started;
static recomp_func_t s_cinput_lifted;

int guest_ui_probe(guest_ui_view_t* out) { return lx_guest_ui_probe(out); }

const char* guest_ui_class_name(uint32_t vtable) { return guest_ui_model_class_name(jpt_gva_table(), vtable); }

int guest_writable(uint32_t va, uint32_t bytes) { return bytes == 0 || gm_ptr_checked(va, bytes, 1) != NULL; }

int guest_data_dir(char* out, size_t cap) { return s_game_dir[0] && snprintf(out, cap, "%s/data", s_game_dir) < (int)cap; }

int guest_load_string(unsigned id, char* out, int cap) {
    const jpt_lift_data_t* lift = jpt_active_lift_data();
    uint32_t data, size;
    const uint8_t* p;
    unsigned index = id % STRING_BLOCK;
    if (!lift || cap <= 0 || !lx_resource_find(lift->expected.image_base, RT_STRING, id / STRING_BLOCK + 1u, &data, &size)) return 0;
    p = (const uint8_t*)gm_ptr_checked(data, size, 0);
    if (!p) return 0;
    for (unsigned i = 0; i < STRING_BLOCK; i++) {
        uint32_t length;
        if (size < 2) return 0;
        length = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        if (size < 2 + 2 * length) return 0;
        if (i == index) {
            int n = 0;
            for (uint32_t k = 0; k < length && n < cap - 1; k++) {
                uint32_t unit = (uint32_t)p[2 + 2 * k] | (uint32_t)p[3 + 2 * k] << 8;
                out[n++] = unit < 0x100u ? (char)unit : '?';
            }
            out[n] = 0;
            return length > 0;
        }
        p += 2 + 2 * length;
        size -= 2 + 2 * length;
    }
    return 0;
}

int host_key_held(unsigned vk) { return lx_user32_key_held(vk); }

uint32_t host_pad_buttons(void) {
#ifdef JPT_LINUX_SDL_VIDEO
    const pad_state_t* s = lx_gamepad_state();
    return s ? s->buttons : 0;
#else
    return 0;
#endif
}

int host_input_focused(void) {
    lx_mouse_snapshot_t m;
    lx_mouse_snapshot(&m);
    return m.focused;
}

int host_panel_visible(void) { return debug_menu_visible(); }

void host_post_key(unsigned vk) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.down = 1, ev.synthetic = 1;
    lx_user32_inject_event(&ev);
    ev.down = 0;
    lx_user32_inject_event(&ev);
}

void host_overlay_changed(void) { lx_gamepad_overlay_changed(); }

void host_post_key_up(unsigned vk) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.down = 0, ev.synthetic = 1;
    lx_user32_inject_event(&ev);
}

void host_post_char(char c) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_CHAR, ev.vk = (uint32_t)(unsigned char)c, ev.synthetic = 1;
    lx_user32_inject_event(&ev);
}

void host_pad_cursor_set(int x, int y) { lx_user32_cursor_set(x, y); }

int host_game_cursor(int* x, int* y) {
    long lx, ly;
    if (!lx_user32_cursor_get(&lx, &ly)) return 0;
    *x = (int)lx, *y = (int)ly;
    return 1;
}

void host_post_mouse(int kind) {
    static const uint32_t messages[] = { 0x0201u, 0x0202u, 0x0203u, 0x0200u };
    long x = 0, y = 0;
    uint32_t window = lx_user32_main_window();
    if (kind < 0 || kind > HOST_MOUSE_MOVE || !window || !lx_user32_cursor_get(&x, &y)) return;
    lx_user32_post(window, messages[kind], kind == HOST_MOUSE_UP ? 0u : 1u, ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF));
}

void tutorial_prompts_overlay_state(int* card, int* game_text) {
    overlay_text_state(card, game_text);
    if (card) *card = lx_card_shown();
}

int tutorial_prompts_card_shown(void) { return lx_card_shown(); }

void tutorial_prompts_show_card(void) { lx_card_show(); }

void tutorial_prompts_hide_card(void) { lx_card_hide(); }

int tutorial_prompts_hint(void) { return lx_card_hint(); }

int lx_features_start(const jpt_config_t* cfg, const char* game_dir, const char* version, const char* settings_file, const char* cache_dir) {
    char error[256];
    const jpt_game_version_t* v = jpt_game_version_by_id(version ? version : "en-116");
    snprintf(s_game_dir, sizeof s_game_dir, "%s", game_dir ? game_dir : "");
    snprintf(s_settings_file, sizeof s_settings_file, "%s", settings_file ? settings_file : "");
    if (!v || !jpt_profile_activate(v, error, sizeof error)) {
        lx_log("host", "no profile for %s: %s", version ? version : "en-116", v ? error : "unknown version");
        return 0;
    }
    if (!guest_overrides_enable()) {
        jpt_profile_reset();
        return 0;
    }
    {
        jpt_config_t language = *cfg;
        jpt_config_select_language(&language, v);
        l10n_init(cache_dir ? cache_dir : "cache", "C:", game_dir, language.language, v);
        l10n_set_user_folder("D:\\");
        lx_text_set_codepage(jpt_l10n_display_codepage());
    }
    guest_features_configure(cfg);
    cheats_start(cfg->cheat);
    minimap_configure(cfg->minimap);
    guest_features_install();
    widescreen_install_camera(cfg->aspect);
    terrain_quality_install();
    skip_intro_install();
    l10n_install();
    lx_frontend_install();
    debug_menu_set_hooks(lx_mouse_update, lx_panel_hits);
    lx_features_install_cinput();
    s_started = 1;
    return 1;
}

void lx_features_stop(void) {
    if (!s_started) return;
    s_started = 0;
    guest_features_stop();
    lx_panel_release();
    lx_frontend_reset();
    lx_card_reset();
    lx_text_set_codepage(1252);
    lx_overrides_reset();
    lx_host_reset();
    jpt_profile_reset();
}

int lx_features_running(void) { return s_started; }

#define SINPUT_IN_CINPUT 0x08u
#define SINPUT_ROTATE_X 0x04u
#define SINPUT_MOVE_X 0x0Cu
#define SINPUT_MOVE_Y 0x10u
#define SINPUT_STATE 0x14u
#define SINPUT_HIT 0x18u

/* The read's result only (look, move, held commands, new presses): what CInput itself keeps is left alone. */
static void clear_result(uint32_t sinput) {
    const float zero = 0.0f;
    uint32_t bits;
    memcpy(&bits, &zero, sizeof bits);
    MEM32(sinput + SINPUT_ROTATE_X) = bits;
    MEM32(sinput + SINPUT_ROTATE_X + 4) = bits;
    MEM32(sinput + SINPUT_MOVE_X) = bits;
    MEM32(sinput + SINPUT_MOVE_Y) = bits;
    MEM32(sinput + SINPUT_STATE) = 0;
    MEM32(sinput + SINPUT_HIT) = 0;
}

/* CInput::tinReadDefaultControls(this): the gamepad's own hook runs around the original, then a menu of the host empties the result
 * and the cheats that take the input (the free camera) do. */
static void cinput_read(void) {
    uint32_t self = g_ecx;
    if (g_alias_entry) {
        s_cinput_lifted();
        return;
    }
    lx_gamepad_read_around(self, s_cinput_lifted);
    if (overlay_blocks_gameplay() || cheats_take_input(self + SINPUT_IN_CINPUT)) clear_result(self + SINPUT_IN_CINPUT);
}

int lx_features_install_cinput(void) {
    return guest_override_install(GVA(0x005F80B0), cinput_read, &s_cinput_lifted);
}

int lx_features_pointer_move(int x, int y) { return s_started && debug_menu_visible() && debug_menu_pointer(0, x, y); }

int lx_features_pointer_button(unsigned button, int down, int x, int y) {
    if (!s_started || !debug_menu_visible()) return 0;
    if (button == 1 && down) debug_menu_pointer(1, x, y);
    return 1;
}

void lx_features_toggle_minimap(const char* source) {
    if (s_started) overlay_toggle_minimap(source, s_settings_file[0] ? s_settings_file : NULL);
}

void lx_host_ui_loop(void) {
    if (!s_started) return;
    lx_frontend_ui_frame();
    lx_gamepad_ui_loop();
    overlay_ui_loop();
    debug_menu_ui_loop();
    guest_features_step();
}
