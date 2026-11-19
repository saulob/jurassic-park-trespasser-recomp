#ifndef TRESPASSER_AUDIO_HOST_H
#define TRESPASSER_AUDIO_HOST_H

#include "audio_backend.h"

/* The host's audio lifecycle, without the DirectSound types (those stay in the compatibility layer's header). */

/* Chooses the output before the first sound device is created. */
void dsound_compat_install(const audio_backend_t* backend);
/* Stops the output first, then detaches every buffer from the mixer; later calls fail cleanly. */
void dsound_compat_shutdown(void);

#endif
