/* Each pair is a value of the portable DirectSound ABI (dsound_abi.h, dsound_methods.h) and the same value as the real
 * dsound.h defines it. dsound_windows_abi.c evaluates the second of each pair and dsound_bridge_tests.c the first, in
 * separate translation units because the two headers define the same names. */
#define DSOUND_ABI_PAIRS(X)                                                                                             \
    X(DS_OK, DS_OK)                                                                                                     \
    X(DS_E_NOINTERFACE, E_NOINTERFACE)                                                                                  \
    X(DS_E_POINTER, E_POINTER)                                                                                          \
    X(DS_E_FAIL, E_FAIL)                                                                                                \
    X(DS_E_NOTIMPL, E_NOTIMPL)                                                                                          \
    X(DS_E_INVALIDARG, E_INVALIDARG)                                                                                    \
    X(DS_E_OUTOFMEMORY, E_OUTOFMEMORY)                                                                                  \
    X(DS_CLASS_E_NOAGGREGATION, CLASS_E_NOAGGREGATION)                                                                  \
    X(DSERR_ALLOCATED, DSERR_ALLOCATED)                                                                                 \
    X(DSERR_CONTROLUNAVAIL, DSERR_CONTROLUNAVAIL)                                                                       \
    X(DSERR_INVALIDPARAM, DSERR_INVALIDPARAM)                                                                           \
    X(DSERR_INVALIDCALL, DSERR_INVALIDCALL)                                                                             \
    X(DSERR_PRIOLEVELNEEDED, DSERR_PRIOLEVELNEEDED)                                                                     \
    X(DSERR_OUTOFMEMORY, DSERR_OUTOFMEMORY)                                                                             \
    X(DSERR_BADFORMAT, DSERR_BADFORMAT)                                                                                 \
    X(DSERR_UNSUPPORTED, DSERR_UNSUPPORTED)                                                                             \
    X(DSERR_NODRIVER, DSERR_NODRIVER)                                                                                   \
    X(DSERR_ALREADYINITIALIZED, DSERR_ALREADYINITIALIZED)                                                               \
    X(DSERR_BUFFERLOST, DSERR_BUFFERLOST)                                                                               \
    X(DSBCAPS_PRIMARYBUFFER, DSBCAPS_PRIMARYBUFFER)                                                                     \
    X(DSBCAPS_STATIC, DSBCAPS_STATIC)                                                                                   \
    X(DSBCAPS_LOCHARDWARE, DSBCAPS_LOCHARDWARE)                                                                         \
    X(DSBCAPS_CTRL3D, DSBCAPS_CTRL3D)                                                                                   \
    X(DSBCAPS_CTRLFREQUENCY, DSBCAPS_CTRLFREQUENCY)                                                                     \
    X(DSBCAPS_CTRLPAN, DSBCAPS_CTRLPAN)                                                                                 \
    X(DSBCAPS_CTRLVOLUME, DSBCAPS_CTRLVOLUME)                                                                           \
    X(DSBCAPS_STICKYFOCUS, DSBCAPS_STICKYFOCUS)                                                                         \
    X(DSBCAPS_GLOBALFOCUS, DSBCAPS_GLOBALFOCUS)                                                                         \
    X(DSBCAPS_GETCURRENTPOSITION2, DSBCAPS_GETCURRENTPOSITION2)                                                         \
    X(DSBPLAY_LOOPING, DSBPLAY_LOOPING)                                                                                 \
    X(DSBSTATUS_PLAYING, DSBSTATUS_PLAYING)                                                                             \
    X(DSBSTATUS_BUFFERLOST, DSBSTATUS_BUFFERLOST)                                                                       \
    X(DSBSTATUS_LOOPING, DSBSTATUS_LOOPING)                                                                             \
    X(DSBLOCK_FROMWRITECURSOR, DSBLOCK_FROMWRITECURSOR)                                                                 \
    X(DSBLOCK_ENTIREBUFFER, DSBLOCK_ENTIREBUFFER)                                                                       \
    X(DSSCL_NORMAL, DSSCL_NORMAL)                                                                                       \
    X(DSSCL_PRIORITY, DSSCL_PRIORITY)                                                                                   \
    X(DSSCL_EXCLUSIVE, DSSCL_EXCLUSIVE)                                                                                 \
    X(DSSCL_WRITEPRIMARY, DSSCL_WRITEPRIMARY)                                                                           \
    X(DSSPEAKER_STEREO, DSSPEAKER_STEREO)                                                                               \
    X(DSBSIZE_MIN, DSBSIZE_MIN)                                                                                         \
    X(DSBSIZE_MAX, DSBSIZE_MAX)                                                                                         \
    X(DSBFREQUENCY_ORIGINAL, DSBFREQUENCY_ORIGINAL)                                                                     \
    X(DSCAPS_PRIMARYMONO, DSCAPS_PRIMARYMONO)                                                                           \
    X(DSCAPS_PRIMARYSTEREO, DSCAPS_PRIMARYSTEREO)                                                                       \
    X(DSCAPS_PRIMARY8BIT, DSCAPS_PRIMARY8BIT)                                                                           \
    X(DSCAPS_PRIMARY16BIT, DSCAPS_PRIMARY16BIT)                                                                         \
    X(DSCAPS_CONTINUOUSRATE, DSCAPS_CONTINUOUSRATE)                                                                     \
    X(DSCAPS_SECONDARYMONO, DSCAPS_SECONDARYMONO)                                                                       \
    X(DSCAPS_SECONDARYSTEREO, DSCAPS_SECONDARYSTEREO)                                                                   \
    X(DSCAPS_SECONDARY8BIT, DSCAPS_SECONDARY8BIT)                                                                       \
    X(DSCAPS_SECONDARY16BIT, DSCAPS_SECONDARY16BIT)                                                                     \
    X(DS3D_IMMEDIATE, DS3D_IMMEDIATE)                                                                                   \
    X(DS3D_DEFERRED, DS3D_DEFERRED)                                                                                     \
    X(DS_WAVE_FORMAT_PCM, WAVE_FORMAT_PCM)                                                                              \
    X(sizeof(ds_guid_t), sizeof(GUID))                                                                                  \
    X(sizeof(ds_waveformatex_t), sizeof(WAVEFORMATEX))                                                                  \
    X(offsetof(ds_waveformatex_t, samples_per_second), offsetof(WAVEFORMATEX, nSamplesPerSec))                          \
    X(offsetof(ds_waveformatex_t, block_align), offsetof(WAVEFORMATEX, nBlockAlign))                                    \
    X(offsetof(ds_waveformatex_t, bits_per_sample), offsetof(WAVEFORMATEX, wBitsPerSample))                             \
    X(sizeof(ds_bufferdesc_t), sizeof(DSBUFFERDESC))                                                                    \
    X(DS_BUFFERDESC_V1_SIZE, sizeof(DSBUFFERDESC1))                                                                     \
    X(offsetof(ds_bufferdesc_t, buffer_bytes), offsetof(DSBUFFERDESC, dwBufferBytes))                                   \
    X(offsetof(ds_bufferdesc_t, format_address), offsetof(DSBUFFERDESC, lpwfxFormat))                                   \
    X(sizeof(ds_caps_t), sizeof(DSCAPS))                                                                                \
    X(offsetof(ds_caps_t, min_secondary_rate), offsetof(DSCAPS, dwMinSecondarySampleRate))                              \
    X(offsetof(ds_caps_t, primary_buffers), offsetof(DSCAPS, dwPrimaryBuffers))                                         \
    X(offsetof(ds_caps_t, max_hw_3d_all), offsetof(DSCAPS, dwMaxHw3DAllBuffers))                                        \
    X(DSOUND_DEVICE_SLOTS * 4, sizeof(IDirectSoundVtbl))                                                                \
    X(DSOUND_BUFFER_SLOTS * 4, sizeof(IDirectSoundBufferVtbl))                                                          \
    X(DSOUND_BUFFER3D_SLOTS * 4, sizeof(IDirectSound3DBufferVtbl))                                                      \
    X(DSOUND_LISTENER_SLOTS * 4, sizeof(IDirectSound3DListenerVtbl))                                                    \
    X(dsound_method_slot(DSM_DeviceCreateSoundBuffer) * 4, offsetof(IDirectSoundVtbl, CreateSoundBuffer))               \
    X(dsound_method_slot(DSM_DeviceGetCaps) * 4, offsetof(IDirectSoundVtbl, GetCaps))                                   \
    X(dsound_method_slot(DSM_DeviceDuplicateSoundBuffer) * 4, offsetof(IDirectSoundVtbl, DuplicateSoundBuffer))         \
    X(dsound_method_slot(DSM_DeviceSetCooperativeLevel) * 4, offsetof(IDirectSoundVtbl, SetCooperativeLevel))           \
    X(dsound_method_slot(DSM_DeviceCompact) * 4, offsetof(IDirectSoundVtbl, Compact))                                   \
    X(dsound_method_slot(DSM_DeviceGetSpeakerConfig) * 4, offsetof(IDirectSoundVtbl, GetSpeakerConfig))                 \
    X(dsound_method_slot(DSM_DeviceSetSpeakerConfig) * 4, offsetof(IDirectSoundVtbl, SetSpeakerConfig))                 \
    X(dsound_method_slot(DSM_DeviceInitialize) * 4, offsetof(IDirectSoundVtbl, Initialize))                             \
    X(dsound_method_slot(DSM_BufferGetCaps) * 4, offsetof(IDirectSoundBufferVtbl, GetCaps))                             \
    X(dsound_method_slot(DSM_BufferGetCurrentPosition) * 4, offsetof(IDirectSoundBufferVtbl, GetCurrentPosition))       \
    X(dsound_method_slot(DSM_BufferGetFormat) * 4, offsetof(IDirectSoundBufferVtbl, GetFormat))                         \
    X(dsound_method_slot(DSM_BufferGetVolume) * 4, offsetof(IDirectSoundBufferVtbl, GetVolume))                         \
    X(dsound_method_slot(DSM_BufferGetPan) * 4, offsetof(IDirectSoundBufferVtbl, GetPan))                               \
    X(dsound_method_slot(DSM_BufferGetFrequency) * 4, offsetof(IDirectSoundBufferVtbl, GetFrequency))                   \
    X(dsound_method_slot(DSM_BufferGetStatus) * 4, offsetof(IDirectSoundBufferVtbl, GetStatus))                         \
    X(dsound_method_slot(DSM_BufferInitialize) * 4, offsetof(IDirectSoundBufferVtbl, Initialize))                       \
    X(dsound_method_slot(DSM_BufferLock) * 4, offsetof(IDirectSoundBufferVtbl, Lock))                                   \
    X(dsound_method_slot(DSM_BufferPlay) * 4, offsetof(IDirectSoundBufferVtbl, Play))                                   \
    X(dsound_method_slot(DSM_BufferSetCurrentPosition) * 4, offsetof(IDirectSoundBufferVtbl, SetCurrentPosition))       \
    X(dsound_method_slot(DSM_BufferSetFormat) * 4, offsetof(IDirectSoundBufferVtbl, SetFormat))                         \
    X(dsound_method_slot(DSM_BufferSetVolume) * 4, offsetof(IDirectSoundBufferVtbl, SetVolume))                         \
    X(dsound_method_slot(DSM_BufferSetPan) * 4, offsetof(IDirectSoundBufferVtbl, SetPan))                               \
    X(dsound_method_slot(DSM_BufferSetFrequency) * 4, offsetof(IDirectSoundBufferVtbl, SetFrequency))                   \
    X(dsound_method_slot(DSM_BufferStop) * 4, offsetof(IDirectSoundBufferVtbl, Stop))                                   \
    X(dsound_method_slot(DSM_BufferUnlock) * 4, offsetof(IDirectSoundBufferVtbl, Unlock))                               \
    X(dsound_method_slot(DSM_BufferRestore) * 4, offsetof(IDirectSoundBufferVtbl, Restore))                             \
    X(dsound_method_slot(DSM_Buffer3DGetAllParameters) * 4, offsetof(IDirectSound3DBufferVtbl, GetAllParameters))       \
    X(dsound_method_slot(DSM_Buffer3DGetConeAngles) * 4, offsetof(IDirectSound3DBufferVtbl, GetConeAngles))             \
    X(dsound_method_slot(DSM_Buffer3DGetMode) * 4, offsetof(IDirectSound3DBufferVtbl, GetMode))                         \
    X(dsound_method_slot(DSM_Buffer3DSetConeAngles) * 4, offsetof(IDirectSound3DBufferVtbl, SetConeAngles))             \
    X(dsound_method_slot(DSM_Buffer3DSetConeOrientation) * 4, offsetof(IDirectSound3DBufferVtbl, SetConeOrientation))   \
    X(dsound_method_slot(DSM_Buffer3DSetConeOutsideVolume) * 4, offsetof(IDirectSound3DBufferVtbl, SetConeOutsideVolume)) \
    X(dsound_method_slot(DSM_Buffer3DSetMaxDistance) * 4, offsetof(IDirectSound3DBufferVtbl, SetMaxDistance))           \
    X(dsound_method_slot(DSM_Buffer3DSetMinDistance) * 4, offsetof(IDirectSound3DBufferVtbl, SetMinDistance))           \
    X(dsound_method_slot(DSM_Buffer3DSetMode) * 4, offsetof(IDirectSound3DBufferVtbl, SetMode))                         \
    X(dsound_method_slot(DSM_Buffer3DSetPosition) * 4, offsetof(IDirectSound3DBufferVtbl, SetPosition))                 \
    X(dsound_method_slot(DSM_Buffer3DSetVelocity) * 4, offsetof(IDirectSound3DBufferVtbl, SetVelocity))                 \
    X(dsound_method_slot(DSM_ListenerGetAllParameters) * 4, offsetof(IDirectSound3DListenerVtbl, GetAllParameters))     \
    X(dsound_method_slot(DSM_ListenerGetOrientation) * 4, offsetof(IDirectSound3DListenerVtbl, GetOrientation))         \
    X(dsound_method_slot(DSM_ListenerSetAllParameters) * 4, offsetof(IDirectSound3DListenerVtbl, SetAllParameters))     \
    X(dsound_method_slot(DSM_ListenerSetOrientation) * 4, offsetof(IDirectSound3DListenerVtbl, SetOrientation))         \
    X(dsound_method_slot(DSM_ListenerSetPosition) * 4, offsetof(IDirectSound3DListenerVtbl, SetPosition))               \
    X(dsound_method_slot(DSM_ListenerSetVelocity) * 4, offsetof(IDirectSound3DListenerVtbl, SetVelocity))               \
    X(dsound_method_slot(DSM_ListenerCommitDeferredSettings) * 4, offsetof(IDirectSound3DListenerVtbl, CommitDeferredSettings))
