#include "linux_settings.h"

#include "jpt_custom_level.h"
#include "linux_hostkeys.h"
#include "linux_log.h"
#include "linux_mouse.h"
#include "linux_overlay.h"
#include "linux_quick.h"
#include "linux_vulkan.h"

#define MAX_SETTING_OVERRIDES 32

static const char* s_override_key[MAX_SETTING_OVERRIDES];
static const char* s_override_value[MAX_SETTING_OVERRIDES];
static int s_override_count;
static jpt_config_t s_current;
static int s_current_set;
static jpt_config_t s_players;

static void session_ended(void) { s_current = s_players; }

void lx_settings_set_overrides(const char* const* keys, const char* const* values, int count) {
    s_override_count = 0;
    for (int i = 0; i < count && i < MAX_SETTING_OVERRIDES; i++) {
        s_override_key[s_override_count] = keys[i];
        s_override_value[s_override_count++] = values[i];
    }
}

const jpt_config_t* lx_settings_current(void) {
    if (!s_current_set) jpt_config_defaults(&s_current), s_current_set = 1;
    return &s_current;
}

void lx_settings_load(const char* settings_file, jpt_config_t* out) {
    const char *key, *value;
    int read = 0;
    jpt_config_defaults(out);
    if (settings_file) read = jpt_config_load(settings_file, out);
    for (int i = 0; i < s_override_count; i++) {
        if (jpt_config_apply(out, s_override_key[i], s_override_value[i]))
            lx_log("Linux settings", "%s = %s (command line)", s_override_key[i], s_override_value[i]);
        else
            lx_log("Linux settings", "%s = %s (command line) is not a valid value and is ignored", s_override_key[i], s_override_value[i]);
    }
    s_players = *out;
    jpt_custom_level_on_end(session_ended);
    for (int i = 0; jpt_custom_level_setting(i, &key, &value); i++)
        if (jpt_config_apply(out, key, value)) lx_log("custom-level", "%s = %s for this session (the saved setting stays)", key, value);
    s_current = *out;
    s_current_set = 1;
    lx_overlay_reset();
    lx_quick_reset();
    lx_hostkeys_reset();
    lx_overlay_configure(out->show_frame_counter);
    lx_hostkeys_set_quick_keys(out->hotkey_vk[JPT_HOTKEY_QUICK_SAVE], out->hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
    lx_mouse_set_sensitivity(out->mouse_sensitivity);
    lx_vk_configure(out, NULL);
    lx_log("Linux settings", "%s: renderer %s, frame counter %s, mouse sensitivity %.2f, Quick Save key 0x%02X, Quick Load key 0x%02X", read ? "read" : "defaults",
           jpt_renderer_key(lx_vk_decision()->chosen), out->show_frame_counter ? "on" : "off", (double)out->mouse_sensitivity, out->hotkey_vk[JPT_HOTKEY_QUICK_SAVE], out->hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
}
