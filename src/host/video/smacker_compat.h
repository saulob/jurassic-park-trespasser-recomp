#ifndef TRESPASSER_SMACKER_COMPAT_H
#define TRESPASSER_SMACKER_COMPAT_H

#include <stdint.h>

#include "../abi/guest_ops.h"
#include "../abi/platform_id.h"
#include "../audio/audio_mixer.h"
#include "smacker_backend.h"

/* SMACKW32.DLL as the retail game uses it, implemented in the host: the 14 exports the executable imports, called with
 * the stack words of the original call. The guest still sees the Smack structure at the same offsets. Used where the
 * original DLL cannot run (Linux); Windows keeps the DLL (smk_binding_for_platform). Called on the guest thread only. */

#define SMACK_EXPORTS(X)                                                                                                \
    X(SmackOpen, 3, "_SmackOpen@12")                                                                                    \
    X(SmackClose, 1, "_SmackClose@4")                                                                                   \
    X(SmackSoundOnOff, 2, "_SmackSoundOnOff@8")                                                                         \
    X(SmackWait, 1, "_SmackWait@4")                                                                                     \
    X(SmackBufferOpen, 6, "_SmackBufferOpen@24")                                                                        \
    X(SmackNextFrame, 1, "_SmackNextFrame@4")                                                                           \
    X(SmackColorRemapWithTrans, 5, "_SmackColorRemapWithTrans@20")                                                      \
    X(SmackDoFrame, 1, "_SmackDoFrame@4")                                                                               \
    X(SmackToBufferRect, 2, "_SmackToBufferRect@8")                                                                     \
    X(SmackSoundUseDirectSound, 1, "_SmackSoundUseDirectSound@4")                                                       \
    X(SmackToBuffer, 7, "_SmackToBuffer@28")                                                                            \
    X(SmackBufferNewPalette, 3, "_SmackBufferNewPalette@12")                                                            \
    X(SmackBufferBlit, 8, "_SmackBufferBlit@32")                                                                        \
    X(SmackBufferClose, 1, "_SmackBufferClose@4")

typedef enum {
#define SMACK_ENUM_ENTRY(name, words, decorated) SMKX_##name,
    SMACK_EXPORTS(SMACK_ENUM_ENTRY)
#undef SMACK_ENUM_ENTRY
    SMKX_COUNT
} smk_export_t;

/* Flags of SmackOpen, SmackToBuffer and the header, as the game passes them. */
#define SMK_OPEN_NEEDVOLUME 0x00000040u
#define SMK_OPEN_FILEHANDLE 0x00001000u
#define SMK_OPEN_TRACKS 0x000FE000u
#define SMK_BUFFER_555 0x80000000u
#define SMK_BUFFER_565 0xC0000000u
#define SMK_BUFFER_FORMAT_MASK 0xC0000000u

/* The public part of the Smack structure, byte offsets in guest memory. */
#define SMK_OFF_VERSION 0x00u
#define SMK_OFF_WIDTH 0x04u
#define SMK_OFF_HEIGHT 0x08u
#define SMK_OFF_FRAMES 0x0Cu
#define SMK_OFF_RATE 0x10u
#define SMK_OFF_FLAGS 0x14u
#define SMK_OFF_NEW_PALETTE 0x68u
#define SMK_OFF_PALETTE 0x6Cu
#define SMK_OFF_FRAME_NUMBER 0x374u
#define SMK_OFF_RECT_X 0x380u
#define SMK_OFF_RECT_Y 0x384u
#define SMK_OFF_RECT_W 0x388u
#define SMK_OFF_RECT_H 0x38Cu
#define SMK_STRUCT_BYTES 0x400u

typedef struct {
    jpt_guest_ops_t guest;
    const smk_backend_ops_t* backend;
    /* Sources for SMACKFILEHANDLE (a handle the game opened and closes itself) and for a file name. 0 on failure. */
    int (*open_handle)(void* context, uint32_t handle, smk_source_t* out);
    int (*open_path)(void* context, const char* path, smk_source_t* out);
    void (*close_source)(void* context, smk_source_t* source);
    /* The mixer of the game output when `device` is its IDirectSound, NULL for no sound. */
    audio_mixer_t* (*mixer_for_device)(void* context, uint32_t device);
    uint64_t (*now_us)(void* context);
    void* context;
    /* Optional: one line per open and close, for the host's log. */
    void (*log)(void* context, const char* line);
    /* Optional: the name of the file behind a SMACKFILEHANDLE the game opened. 0 when unknown. */
    int (*handle_name)(void* context, uint32_t handle, char* out, size_t capacity);
    /* Optional: a video's life for the host's overlays. `frame` is the frame on screen, in the destination's pixels at (x, y, w, h). */
    void (*video_opened)(void* context, uint32_t video, const char* name, const smk_source_t* source, uint32_t frames);
    void (*video_frame)(void* context, uint32_t video, uint32_t frame, int32_t x, int32_t y, int32_t w, int32_t h);
    void (*video_closed)(void* context, uint32_t video);
} smk_host_t;

/* What the HLE did since the last reset (kept across smacker_hle_shutdown): the test and log evidence of a real playback. */
typedef struct {
    uint32_t opened, closed, open_failed;
    uint64_t frames_decoded, frames_advanced, audio_bytes_pushed;
    uint32_t audio_streams, ended_by_eof;
} smk_stats_t;
void smacker_hle_stats(smk_stats_t* out);
void smacker_hle_stats_reset(void);

/* The mixer of the DirectSound layer when `device` is one of its devices: the host callback of a build that also runs
 * the game audio through dsound_compat. */
audio_mixer_t* smk_dsound_mixer_for_device(void* context, uint32_t device);

int smacker_hle_install(const smk_host_t* host);
/* Closes every open Smack and forgets the host. */
void smacker_hle_shutdown(void);
uint32_t smacker_hle_call(smk_export_t export_id, const uint32_t* args);

const char* smacker_export_name(smk_export_t export_id);
uint32_t smacker_export_words(smk_export_t export_id);
/* The decorated import name ("_SmackOpen@12"), or the plain one ("SmackOpen"). */
int smacker_find_export(const char* name, smk_export_t* out);
/* "SMACKW32.DLL" by any path and case. */
int smacker_is_library(const char* name);
unsigned smacker_open_count(void);

/* Which implementation answers the Smacker imports of the game: Windows runs the original DLL, every other platform the
 * HLE. The DLL is never a requirement of the HLE and the HLE never replaces the DLL where the DLL runs. */
typedef enum { SMK_BINDING_ORIGINAL_DLL, SMK_BINDING_HLE } smk_binding_t;
smk_binding_t smk_binding_for_platform(jpt_platform_t platform);
int smk_binding_needs_dll(smk_binding_t binding);

/* The audio the HLE keeps queued ahead of the picture, in milliseconds. */
#define SMK_AUDIO_LEAD_MS 500u

#endif
