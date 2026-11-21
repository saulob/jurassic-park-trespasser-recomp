/* The mouse policy of the Windows host (host_policy.h) on Linux: the guest's ClipCursor and SetCapture stay logical, menus, dialogs
 * and videos get the free pointer, and only gameplay (the game's own gameplay window in front, read from its window stack) takes
 * SDL's relative mouse mode and hides the pointer. */
#ifndef JPT_LINUX_MOUSE_H
#define JPT_LINUX_MOUSE_H

#include <stdint.h>

#include "host_policy.h"

typedef struct {
    input_state_t state;
    int focused;
    int relative;           /* the backend's relative mouse mode as applied */
    int cursor_hidden;      /* the backend's pointer as applied */
    int game_cursor_hidden; /* the guest's ShowCursor counter is below zero */
    int guest_capture;      /* the window the guest captured (logical only), 0 when none */
    int guest_clip;         /* the guest's ClipCursor rectangle is set (logical only) */
    guest_ui_t ui;          /* what the game's window stack shows */
    guest_wnd_t top_kind;   /* the window in front */
} lx_mouse_snapshot_t;

typedef enum { LX_MN_RAW_MOTION, LX_MN_ABSOLUTE_MOTION, LX_MN_RELATIVE_MOTION, LX_MN_BUTTON, LX_MN_WHEEL } lx_mouse_note_t;

typedef struct {
    unsigned raw_motions, absolute_motions, relative_motions, buttons, wheels, context_switches;
} lx_mouse_stats_t;

/* Counts an input event; JPT_INPUT_STATS=1 logs the totals at each change of input context and at shutdown, never per event. */
void lx_mouse_note(lx_mouse_note_t kind);
void lx_mouse_stats(lx_mouse_stats_t* out);
void lx_mouse_log_summary(void);

void lx_mouse_reset(void);
/* Mouse sensitivity (the launcher setting, 0.25 to 4.00): scales the mouse look only, the fractions carry to the next motion. */
void lx_mouse_set_sensitivity(float value);
void lx_mouse_set_focus(int focused);
void lx_mouse_set_cursor_count(int count);
/* Derives the input state and applies it to the backend; logs when the applied state changes. */
void lx_mouse_update(void);
/* Relative motion for the virtual cursor: 1 when the cursor moved (only while gameplay holds the pointer). */
int lx_mouse_motion(long dx, long dy, long* x, long* y);
/* The game reads a cursor it re-centres itself (gameplay, or focus lost in it): the real pointer's position is not its cursor. */
int lx_mouse_cursor_virtual(void);
void lx_mouse_snapshot(lx_mouse_snapshot_t* out);

#endif
