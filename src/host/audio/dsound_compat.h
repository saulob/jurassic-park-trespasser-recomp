#ifndef TRESPASSER_DSOUND_COMPAT_H
#define TRESPASSER_DSOUND_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include "audio_backend.h"
#include "audio_host.h"

/* The DirectSound the game sees: the guest's ABI (COM objects with the standard vtables) over the audio mixer. Only the
 * calls the game and Smacker make are implemented; every other method answers DSERR_UNSUPPORTED. DSound.dll is never
 * used: dsound.h supplies the types and nothing here is linked against it. */

/* The one entry point the guest and SMACKW32.DLL reach through GetProcAddress. */
HRESULT WINAPI compat_DirectSoundCreate(LPCGUID device, LPDIRECTSOUND* out, LPUNKNOWN outer);

typedef struct {
    unsigned devices_created, buffers_created, buffers_released, buffers_live, buffers_peak;
} dsound_compat_counts_t;
void dsound_compat_counts(dsound_compat_counts_t* counts);
/* 1 when no vtable of the layer has an empty slot. */
int dsound_compat_vtables_complete(void);
/* The mixer behind the layer, NULL when no output is open. */
audio_mixer_t* dsound_compat_mixer(void);

#endif
