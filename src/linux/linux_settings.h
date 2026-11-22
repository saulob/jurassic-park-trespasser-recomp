/* The settings file (trespasser.ini, the one the launcher writes) as the Linux runtime reads it: the same parser and defaults as the
 * Windows host (jpt_config). The values the Linux host uses are applied here: the F6 frame counter at start, the Quick Save and
 * Quick Load keys, the mouse sensitivity and the gamepad settings. */
#ifndef JPT_LINUX_SETTINGS_H
#define JPT_LINUX_SETTINGS_H

#include "jpt_config.h"

/* Settings given on the command line: each overrides the file for this run. The strings must outlive the boot. */
void lx_settings_set_overrides(const char* const* keys, const char* const* values, int count);

/* Defaults, then the valid lines of the file (a missing file means the defaults), then the command-line overrides; resets and
 * configures the host features that use them. `out` keeps the values for the rest of the boot. */
void lx_settings_load(const char* settings_file, jpt_config_t* out);

/* The settings of the last load (the defaults before any). */
const jpt_config_t* lx_settings_current(void);

#endif
