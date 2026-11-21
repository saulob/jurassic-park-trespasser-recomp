#ifndef TRESPASSER_SDL_AUDIO_H
#define TRESPASSER_SDL_AUDIO_H

#include "audio_backend.h"
#include "audio_pacer.h"

/* The running output's queue statistics (zero when none runs). */
void sdl_audio_stats(audio_pacer_stats_t* out);

#endif
