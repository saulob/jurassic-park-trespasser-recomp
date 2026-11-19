#ifndef TRESPASSER_AUDIO_HOST_H
#define TRESPASSER_AUDIO_HOST_H

/* The host's audio lifecycle, without the DirectSound types (those stay in the compatibility layer's header). */

/* Stops the output first, then detaches every buffer from the mixer; later calls fail cleanly. */
void dsound_compat_shutdown(void);

#endif
