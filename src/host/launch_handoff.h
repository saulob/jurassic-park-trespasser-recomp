/* The window between the launcher and the game (jpt_launch_handoff.h): Windows only; every call does nothing when the launcher
 * did not start one. */
#ifndef LAUNCH_HANDOFF_H
#define LAUNCH_HANDOFF_H

#include "jpt_launch_handoff.h"
#include "jpt_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The launcher's callback: returns once the handoff window is painted below the launcher's. */
void launch_handoff_begin(const jpt_launch_handoff_request_t* request);
void launch_handoff_launcher_closed(void);
void launch_handoff_phase(jpt_handoff_phase_t phase);
void launch_handoff_game_window_created(void);
/* The game window is shown with a valid picture: the handoff window goes. */
void launch_handoff_game_window_ready(void* game_window);
/* The run ends: before the game window was ready the handoff window tells why and stays until the player closes it. */
void launch_handoff_finish(int exit_code);

#ifdef __cplusplus
}
#endif

#endif
