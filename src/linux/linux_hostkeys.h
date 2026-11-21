/* The port's hotkeys on Linux, decided by the same table as the Windows host (key_policy in host_policy.c): F2 controls, F3
 * objectives, F4 show objective, F5 minimap, F6 frame counter, F7 and F8 the Quick keys of trespasser.ini, F10 cheats, F11
 * fullscreen, Alt+F4 close. The keys of the table never reach the game; the rest do. */
#ifndef JPT_LINUX_HOSTKEYS_H
#define JPT_LINUX_HOSTKEYS_H

#include "host_policy.h"

typedef struct {
    unsigned fullscreen, frame_counter, quick_save, quick_load, close;
    unsigned unavailable; /* keys of the table whose panel has no Linux renderer yet: consumed, nothing is drawn */
} lx_hostkeys_counts_t;

void lx_hostkeys_reset(void);
/* The Quick Save and Quick Load keys (virtual keys) from trespasser.ini; 0 keeps the default of the table. */
void lx_hostkeys_set_quick_keys(unsigned save_vk, unsigned load_vk);
/* One key message (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN or WM_SYSKEYUP): 1 when the key is the port's and the game must not see it. */
int lx_hostkeys_filter(unsigned msg, unsigned vk, int repeat, int shift, int ctrl, int alt);
const lx_hostkeys_counts_t* lx_hostkeys_counts(void);

#endif
