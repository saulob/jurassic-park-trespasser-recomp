#ifndef TRESPASSER_WINDOWS_DSOUND_BRIDGE_H
#define TRESPASSER_WINDOWS_DSOUND_BRIDGE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../audio_backend.h"
#include "../dsound_compat.h"

/* The 32-bit Windows binding of the portable DirectSound layer. The process is 32-bit and the guest's memory is its
 * own, so a guest address is a host address; the guest reaches each method through a stdcall entry point of the host
 * that forwards its stack words to dsound_compat_call. Nothing of Windows DirectSound is used. */

/* Chooses the audio backend and builds the interface vtables. Returns 0 on failure. */
int jpt_dsound_bridge_install(const audio_backend_t* backend);

/* The host's DirectSoundCreate: what GetProcAddress("DirectSoundCreate") returns for the host's DSound module. */
HRESULT WINAPI jpt_dsound_create(const void* device, void** out, void* outer);

/* The stdcall entry the guest calls for a method, and the guest memory of this binding (tests use both). */
uint32_t jpt_dsound_bridge_entry(dsound_method_t method);
const jpt_guest_ops_t* jpt_dsound_bridge_guest_ops(void);

#endif
