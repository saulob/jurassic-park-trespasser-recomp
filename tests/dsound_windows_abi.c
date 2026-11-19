/* The Windows side of the ABI comparison: the values of dsound.h, in the order of dsound_abi_pairs.h. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsound_abi_pairs.h"

#define WINDOWS_VALUE(portable, windows) (uint32_t)(windows),
static const uint32_t k_windows[] = { DSOUND_ABI_PAIRS(WINDOWS_VALUE) 0 };

unsigned windows_abi_count(void) { return (unsigned)(sizeof k_windows / sizeof k_windows[0]) - 1u; }
uint32_t windows_abi_value(unsigned index) { return k_windows[index]; }

/* The interface IDs, as the 16 bytes the guest passes to QueryInterface. */
int windows_abi_iid(unsigned which, unsigned char out[16]) {
    const GUID* guid = which == 0 ? &IID_IDirectSound3DBuffer : which == 1 ? &IID_IDirectSound3DListener : which == 2 ? &IID_IDirectSoundBuffer : &IID_IDirectSound;
    memcpy(out, guid, 16);
    return which < 4;
}
