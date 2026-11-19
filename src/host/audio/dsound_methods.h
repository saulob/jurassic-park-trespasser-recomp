#ifndef TRESPASSER_DSOUND_METHODS_H
#define TRESPASSER_DSOUND_METHODS_H

/* Every entry point the guest can reach, in vtable order within its interface: X(name, words) where `words` counts the
 * 32-bit stack words of the call, `this` included. The Windows bridge generates its stdcall entry points from this list
 * and a 64-bit host does the same for its thunks. */

#define DSOUND_ENTRY_METHODS(X) X(Create, 3)

#define DSOUND_DEVICE_METHODS(X)                                                                                        \
    X(DeviceQueryInterface, 3)                                                                                          \
    X(DeviceAddRef, 1)                                                                                                  \
    X(DeviceRelease, 1)                                                                                                 \
    X(DeviceCreateSoundBuffer, 4)                                                                                       \
    X(DeviceGetCaps, 2)                                                                                                 \
    X(DeviceDuplicateSoundBuffer, 3)                                                                                    \
    X(DeviceSetCooperativeLevel, 3)                                                                                     \
    X(DeviceCompact, 1)                                                                                                 \
    X(DeviceGetSpeakerConfig, 2)                                                                                        \
    X(DeviceSetSpeakerConfig, 2)                                                                                        \
    X(DeviceInitialize, 2)

#define DSOUND_BUFFER_METHODS(X)                                                                                        \
    X(BufferQueryInterface, 3)                                                                                          \
    X(BufferAddRef, 1)                                                                                                  \
    X(BufferRelease, 1)                                                                                                 \
    X(BufferGetCaps, 2)                                                                                                 \
    X(BufferGetCurrentPosition, 3)                                                                                      \
    X(BufferGetFormat, 4)                                                                                               \
    X(BufferGetVolume, 2)                                                                                               \
    X(BufferGetPan, 2)                                                                                                  \
    X(BufferGetFrequency, 2)                                                                                            \
    X(BufferGetStatus, 2)                                                                                               \
    X(BufferInitialize, 3)                                                                                              \
    X(BufferLock, 8)                                                                                                    \
    X(BufferPlay, 4)                                                                                                    \
    X(BufferSetCurrentPosition, 2)                                                                                      \
    X(BufferSetFormat, 2)                                                                                               \
    X(BufferSetVolume, 2)                                                                                               \
    X(BufferSetPan, 2)                                                                                                  \
    X(BufferSetFrequency, 2)                                                                                            \
    X(BufferStop, 1)                                                                                                    \
    X(BufferUnlock, 5)                                                                                                  \
    X(BufferRestore, 1)

#define DSOUND_BUFFER3D_METHODS(X)                                                                                      \
    X(Buffer3DQueryInterface, 3)                                                                                        \
    X(Buffer3DAddRef, 1)                                                                                                \
    X(Buffer3DRelease, 1)                                                                                               \
    X(Buffer3DGetAllParameters, 2)                                                                                      \
    X(Buffer3DGetConeAngles, 3)                                                                                         \
    X(Buffer3DGetConeOrientation, 2)                                                                                    \
    X(Buffer3DGetConeOutsideVolume, 2)                                                                                  \
    X(Buffer3DGetMaxDistance, 2)                                                                                        \
    X(Buffer3DGetMinDistance, 2)                                                                                        \
    X(Buffer3DGetMode, 2)                                                                                               \
    X(Buffer3DGetPosition, 2)                                                                                           \
    X(Buffer3DGetVelocity, 2)                                                                                           \
    X(Buffer3DSetAllParameters, 3)                                                                                      \
    X(Buffer3DSetConeAngles, 4)                                                                                         \
    X(Buffer3DSetConeOrientation, 5)                                                                                    \
    X(Buffer3DSetConeOutsideVolume, 3)                                                                                  \
    X(Buffer3DSetMaxDistance, 3)                                                                                        \
    X(Buffer3DSetMinDistance, 3)                                                                                        \
    X(Buffer3DSetMode, 3)                                                                                               \
    X(Buffer3DSetPosition, 5)                                                                                           \
    X(Buffer3DSetVelocity, 5)

#define DSOUND_LISTENER_METHODS(X)                                                                                      \
    X(ListenerQueryInterface, 3)                                                                                        \
    X(ListenerAddRef, 1)                                                                                                \
    X(ListenerRelease, 1)                                                                                               \
    X(ListenerGetAllParameters, 2)                                                                                      \
    X(ListenerGetDistanceFactor, 2)                                                                                     \
    X(ListenerGetDopplerFactor, 2)                                                                                      \
    X(ListenerGetOrientation, 3)                                                                                        \
    X(ListenerGetPosition, 2)                                                                                           \
    X(ListenerGetRolloffFactor, 2)                                                                                      \
    X(ListenerGetVelocity, 2)                                                                                           \
    X(ListenerSetAllParameters, 3)                                                                                      \
    X(ListenerSetDistanceFactor, 3)                                                                                     \
    X(ListenerSetDopplerFactor, 3)                                                                                      \
    X(ListenerSetOrientation, 8)                                                                                        \
    X(ListenerSetPosition, 5)                                                                                           \
    X(ListenerSetRolloffFactor, 3)                                                                                      \
    X(ListenerSetVelocity, 5)                                                                                           \
    X(ListenerCommitDeferredSettings, 1)

#define DSOUND_ALL_METHODS(X)                                                                                           \
    DSOUND_ENTRY_METHODS(X)                                                                                             \
    DSOUND_DEVICE_METHODS(X)                                                                                            \
    DSOUND_BUFFER_METHODS(X)                                                                                            \
    DSOUND_BUFFER3D_METHODS(X)                                                                                          \
    DSOUND_LISTENER_METHODS(X)

typedef enum {
#define DSOUND_ENUM_ENTRY(name, words) DSM_##name,
    DSOUND_ALL_METHODS(DSOUND_ENUM_ENTRY)
#undef DSOUND_ENUM_ENTRY
    DSM_COUNT
} dsound_method_t;

typedef enum { DSI_DEVICE, DSI_BUFFER, DSI_BUFFER3D, DSI_LISTENER, DSI_COUNT } dsound_interface_t;

#define DSOUND_DEVICE_SLOTS 11
#define DSOUND_BUFFER_SLOTS 21
#define DSOUND_BUFFER3D_SLOTS 21
#define DSOUND_LISTENER_SLOTS 18

#endif
