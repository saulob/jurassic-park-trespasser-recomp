#ifndef TRESPASSER_DSOUND_COMPAT_H
#define TRESPASSER_DSOUND_COMPAT_H

#include <stdint.h>

#include "../abi/guest_ops.h"
#include "audio_backend.h"
#include "audio_host.h"
#include "dsound_abi.h"
#include "dsound_methods.h"

/* The DirectSound the game and SMACKW32 see, over the audio mixer: the guest's COM objects are blocks of guest memory
 * ([vtable address][token]) and every method is one entry of dsound_method_t called with the stack words of the
 * original call. A host object is found through a table keyed by the token, never through a pointer the guest holds.
 * Only what the retail code calls is implemented; every other slot answers DSERR_UNSUPPORTED. No platform header, no
 * DSound.dll: the platform supplies guest memory and the address each method has for the guest. */

typedef struct {
    jpt_guest_ops_t guest;
    /* The address a guest call for `method` must reach: an entry point the platform made for it. */
    uint32_t (*method_address)(void* context, dsound_method_t method);
    void* context;
} dsound_binding_t;

/* Chooses the output and the guest binding before the first sound device is created. Allocates the interface vtables
 * in guest memory; returns 0 when the binding is incomplete. */
int dsound_compat_install(const audio_backend_t* backend, const dsound_binding_t* binding);

/* Runs one method. `args` holds dsound_method_words(method) stack words (`this` first, floats as their bit patterns);
 * the result is the HRESULT, or the reference count for AddRef and Release. */
uint32_t dsound_compat_call(dsound_method_t method, const uint32_t* args);
uint32_t dsound_method_words(dsound_method_t method);
const char* dsound_method_name(dsound_method_t method);
dsound_interface_t dsound_method_interface(dsound_method_t method);
/* Index of the method inside its interface's vtable. */
unsigned dsound_method_slot(dsound_method_t method);

typedef struct {
    unsigned devices_created, buffers_created, buffers_released, buffers_live, buffers_peak;
} dsound_compat_counts_t;
void dsound_compat_counts(dsound_compat_counts_t* counts);
/* 1 when every method has a handler and every vtable was built. */
int dsound_compat_vtables_complete(void);
/* The mixer behind the layer, NULL when no output is open. */
audio_mixer_t* dsound_compat_mixer(void);
/* 1 when `address` is a live IDirectSound the layer handed out. */
int dsound_compat_is_device(uint32_t address);
/* The vtable address of an interface in guest memory, 0 before install. */
uint32_t dsound_compat_vtable(dsound_interface_t interface);

#endif
