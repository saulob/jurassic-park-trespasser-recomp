/* The settings file (trespasser.ini, the one the launcher writes) as the Linux runtime reads it: the same parser and defaults as the
 * Windows host (jpt_config). The values the Linux host uses are applied here: the F6 frame counter at start, the Quick Save and
 * Quick Load keys, the mouse sensitivity and the gamepad settings. */
#ifndef JPT_LINUX_SETTINGS_H
#define JPT_LINUX_SETTINGS_H

#include "jpt_config.h"

/* Defaults, then the valid lines of the file (a missing file means the defaults); resets and configures the host features that
 * use them. `out` keeps the values for the rest of the boot. */
void lx_settings_load(const char* settings_file, jpt_config_t* out);

#endif
