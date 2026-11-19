#include "audio_sync.h"

#include <SDL3/SDL_mutex.h>

audio_mutex_t* audio_mutex_create(void) { return (audio_mutex_t*)SDL_CreateMutex(); }
void audio_mutex_destroy(audio_mutex_t* mutex) { SDL_DestroyMutex((SDL_Mutex*)mutex); }
void audio_mutex_lock(audio_mutex_t* mutex) { SDL_LockMutex((SDL_Mutex*)mutex); }
void audio_mutex_unlock(audio_mutex_t* mutex) { SDL_UnlockMutex((SDL_Mutex*)mutex); }
