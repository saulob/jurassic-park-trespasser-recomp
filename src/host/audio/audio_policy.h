#ifndef TRESPASSER_AUDIO_POLICY_H
#define TRESPASSER_AUDIO_POLICY_H

#include <stdint.h>

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

#endif
