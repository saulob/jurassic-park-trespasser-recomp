#include "pad_sdl.h"

#include <string.h>

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

jpt_pad_type_t pad_sdl_type(SDL_GamepadType t) {
    return t > SDL_GAMEPAD_TYPE_UNKNOWN && (int)t < (int)JPT_PAD_TYPE_COUNT ? (jpt_pad_type_t)t : JPT_PAD_TYPE_UNKNOWN;
}

void pad_sdl_read(SDL_Gamepad* g, pad_state_t* s) {
    memset(s, 0, sizeof *s);
    for (int b = 0; b <= JPT_PAD_DPAD_RIGHT; b++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) s->buttons |= JPT_PAD_BIT(b);
    s->lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
    s->ly = axis(g, SDL_GAMEPAD_AXIS_LEFTY);
    s->rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    s->ry = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    s->lt = axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s->rt = axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (s->lt < 0) s->lt = 0;
    if (s->rt < 0) s->rt = 0;
}
