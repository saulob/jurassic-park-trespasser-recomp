/* Which window of the game is in front on Linux: the same window-stack model as the Windows host, read from guest memory. */
#ifndef JPT_LINUX_GUEST_UI_H
#define JPT_LINUX_GUEST_UI_H

#include "guest_ui_model.h"

/* 1 when the stack was read. Before the game builds its window the view is "none". */
int lx_guest_ui_probe(guest_ui_view_t* out);

/* Headless suites without the retail guest: a function that answers instead of guest memory; NULL restores the real one. */
typedef int (*lx_guest_ui_fn)(guest_ui_view_t* out);
void lx_guest_ui_set_probe(lx_guest_ui_fn fn);

#endif
