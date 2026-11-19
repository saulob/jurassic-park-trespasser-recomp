#ifndef TRESPASSER_AUDIO_BACKEND_H
#define TRESPASSER_AUDIO_BACKEND_H

#include <stdint.h>

#include "audio_mixer.h"

/* Where the mixer's output goes. One output exists at a time; tests supply a backend without a device. */
typedef struct {
    /* Starts the output at `rate` and returns the mixer it pulls from, NULL when no device can be opened. */
    audio_mixer_t* (*open)(uint32_t rate);
    void (*set_rate)(uint32_t rate);
    /* No mixing happens once this returns; the mixer and its voices are freed. */
    void (*close)(void);
    const char* (*name)(void);
} audio_backend_t;

/* The SDL3 audio backend. */
const audio_backend_t* sdl_audio_backend(void);

#endif
