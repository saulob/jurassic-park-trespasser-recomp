/* The SDL3 side of the gamepad layer both platforms share: a controller's state as the policy sees it. */
#ifndef PAD_SDL_H
#define PAD_SDL_H

#include <SDL3/SDL.h>

#include "gamepad_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

void pad_sdl_read(SDL_Gamepad* gamepad, pad_state_t* out);
jpt_pad_type_t pad_sdl_type(SDL_GamepadType type);

#ifdef __cplusplus
}
#endif

#endif
