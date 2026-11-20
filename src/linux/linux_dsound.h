/* dsound.dll on Linux: the portable DirectSound compatibility layer (src/host/audio) over the SDL3 audio backend. The guest's
 * IDirectSound and buffer objects are guest memory; every method is a thunk that runs dsound_compat_call. SDL chooses PipeWire,
 * PulseAudio or ALSA (SDL_AUDIODRIVER overrides it). */
#ifndef JPT_LINUX_DSOUND_H
#define JPT_LINUX_DSOUND_H

#include <stddef.h>

#include "linux_hle.h"

const hle_entry_t* hle_dsound_table(size_t* count);
/* Stops the output and forgets the guest binding (the run is over). */
void hle_dsound_shutdown(void);
/* Headless runs open SDL's dummy audio driver, never a sound device. */
void lx_dsound_set_headless(int headless);

#endif
