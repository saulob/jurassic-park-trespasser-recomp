/* The game's own way out, driven from the host. The game ignores WM_CLOSE and Alt+F4, and leaves only through its screens: Exit on
 * the main menu, Quit in the game. A host that wants it gone ends the window in front with that window's EndModal at the points where the
 * game's loops read messages, one screen per pass, until WinMain returns. The addresses are those of the 1.0.116 executable (lift116). */
#ifndef JPT_LINUX_GUEST_QUIT_H
#define JPT_LINUX_GUEST_QUIT_H

#include <stdint.h>

typedef enum { LX_QUIT_STEP_NONE, LX_QUIT_STEP_ENDED_WINDOW, LX_QUIT_STEP_SKIP_VIDEO } lx_quit_step_t;

/* Called from PeekMessageA while a quit is requested; pumping_function is the lifted function that polls. LX_QUIT_STEP_SKIP_VIDEO means the
 * caller must hand the game an Escape key-up, which its video player takes as "skip". */
lx_quit_step_t lx_guest_quit_step(uint32_t pumping_function);

/* How many screens the host ended in this run. */
unsigned lx_guest_quit_windows_ended(void);
void lx_guest_quit_reset(void);

#endif
