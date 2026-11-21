#include "linux_settings.h"

#include "linux_hostkeys.h"
#include "linux_log.h"
#include "linux_mouse.h"
#include "linux_overlay.h"
#include "linux_quick.h"

void lx_settings_load(const char* settings_file, jpt_config_t* out) {
    int read = 0;
    jpt_config_defaults(out);
    if (settings_file) read = jpt_config_load(settings_file, out);
    lx_overlay_reset();
    lx_quick_reset();
    lx_hostkeys_reset();
    lx_overlay_configure(out->show_frame_counter);
    lx_hostkeys_set_quick_keys(out->hotkey_vk[JPT_HOTKEY_QUICK_SAVE], out->hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
    lx_mouse_set_sensitivity(out->mouse_sensitivity);
    lx_log("Linux settings", "%s: frame counter %s, mouse sensitivity %.2f, Quick Save key 0x%02X, Quick Load key 0x%02X", read ? "read" : "defaults",
           out->show_frame_counter ? "on" : "off", (double)out->mouse_sensitivity, out->hotkey_vk[JPT_HOTKEY_QUICK_SAVE], out->hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
}
