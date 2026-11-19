#ifndef TRESPASSER_AUDIO_SYNC_H
#define TRESPASSER_AUDIO_SYNC_H

/* The one lock primitive of the audio layer: an SDL3 mutex. Nothing in the portable audio code takes a platform lock. */

typedef struct audio_mutex audio_mutex_t;

audio_mutex_t* audio_mutex_create(void);
void audio_mutex_destroy(audio_mutex_t* mutex);
void audio_mutex_lock(audio_mutex_t* mutex);
void audio_mutex_unlock(audio_mutex_t* mutex);

#endif
