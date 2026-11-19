#ifndef TRESPASSER_AUDIO_POLICY_H
#define TRESPASSER_AUDIO_POLICY_H

#include <stdint.h>

#include "../abi/platform_id.h"

/* DirectSound volume is in hundredths of a decibel: 0 is full level, DSBVOLUME_MIN is silence. */
#define DS_VOLUME_MIN (-10000)
#define DS_VOLUME_MAX 0
#define DS_FREQUENCY_MIN 100
#define DS_FREQUENCY_MAX 200000

int audio_clamp_volume(int hundredths_db);
/* Linear amplitude of a volume (0 for DS_VOLUME_MIN). */
float audio_volume_gain(int hundredths_db);
/* Linear amplitude of an attenuation in decibels (dB <= 0 attenuates). */
float audio_db_gain(float db);
/* Frequency of a buffer whose DSBPLAY rate is `requested` (0 = the sample's own); -1 when out of range. */
int audio_resolve_frequency(uint32_t requested, uint32_t original);

/* The names the game and Smacker use to reach DirectSound: the library is DSound.dll (by any path) and its one entry
 * point DirectSoundCreate. Both are answered by the host. */
int audio_is_dsound_library(const char* name);
int audio_is_dsound_create(const char* name);

/* Which SDL3 audio driver the host asks for, by platform:
 *  - SDL_AUDIODRIVER set by the user or the environment: respected as is, SDL reads it itself (no hint is set);
 *  - a headless run (JPT_AUDIO_HEADLESS=1): SDL's dummy driver, no device is opened;
 *  - Windows: WASAPI only, SDL's DirectSound and WinMM drivers are never a fallback;
 *  - anywhere else (Linux): none, SDL picks the first driver its build and the system offer (PipeWire, PulseAudio,
 *    ALSA, ...). */
typedef enum { AUDIO_DRIVER_PLATFORM, AUDIO_DRIVER_AUTOMATIC, AUDIO_DRIVER_EXPLICIT, AUDIO_DRIVER_HEADLESS } audio_driver_source_t;
typedef struct {
    const char* hint; /* the value for SDL_HINT_AUDIO_DRIVER; NULL leaves SDL's own selection alone */
    audio_driver_source_t source;
} audio_driver_choice_t;

audio_driver_choice_t audio_choose_driver(jpt_platform_t platform, const char* sdl_audio_driver, int headless);
/* 1 for a headless request ("1" or "true"). */
int audio_headless_requested(const char* value);
/* The single start-up line of the log: "Audio: SDL3 backend initialized, 22050 Hz, stereo, driver wasapi". */
int audio_backend_line(char* out, unsigned capacity, uint32_t rate, const char* driver);

#endif
