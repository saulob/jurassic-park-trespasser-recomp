#include "../audio/dsound_compat.h"
#include "smacker_compat.h"

audio_mixer_t* smk_dsound_mixer_for_device(void* context, uint32_t device) {
    (void)context;
    return dsound_compat_is_device(device) ? dsound_compat_mixer() : NULL;
}
