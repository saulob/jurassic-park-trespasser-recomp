#ifndef TRESPASSER_DSOUND_ABI_H
#define TRESPASSER_DSOUND_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "../abi/static_assert.h"

/* The part of DirectSound the retail game and SMACKW32 observe, as the 32-bit Windows ABI lays it out: fixed-width
 * types, the constants the layer answers with and the structures it reads from or writes to guest memory. Nothing here
 * includes a Windows header; the Windows bridge's tests compare every value with dsound.h. */

typedef uint32_t ds_hresult_t;

#define DS_OK 0x00000000u
#define DS_E_FAIL 0x80004005u
#define DS_E_NOTIMPL 0x80004001u
#define DS_E_NOINTERFACE 0x80004002u
#define DS_E_POINTER 0x80004003u
#define DS_E_INVALIDARG 0x80070057u
#define DS_E_OUTOFMEMORY 0x8007000Eu
#define DS_CLASS_E_NOAGGREGATION 0x80040110u
#define DSERR_ALLOCATED 0x8878000Au
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_INVALIDPARAM DS_E_INVALIDARG
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_PRIOLEVELNEEDED 0x88780046u
#define DSERR_OUTOFMEMORY DS_E_OUTOFMEMORY
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_UNSUPPORTED DS_E_NOTIMPL
#define DSERR_NODRIVER 0x88780078u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define DSERR_BUFFERLOST 0x88780096u

#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBCAPS_STATIC 0x00000002u
#define DSBCAPS_LOCHARDWARE 0x00000004u
#define DSBCAPS_CTRL3D 0x00000010u
#define DSBCAPS_CTRLFREQUENCY 0x00000020u
#define DSBCAPS_CTRLPAN 0x00000040u
#define DSBCAPS_CTRLVOLUME 0x00000080u
#define DSBCAPS_STICKYFOCUS 0x00004000u
#define DSBCAPS_GLOBALFOCUS 0x00008000u
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000u

#define DSBPLAY_LOOPING 0x00000001u
#define DSBSTATUS_PLAYING 0x00000001u
#define DSBSTATUS_BUFFERLOST 0x00000002u
#define DSBSTATUS_LOOPING 0x00000004u
#define DSBLOCK_FROMWRITECURSOR 0x00000001u
#define DSBLOCK_ENTIREBUFFER 0x00000002u

#define DSSCL_NORMAL 1u
#define DSSCL_PRIORITY 2u
#define DSSCL_EXCLUSIVE 3u
#define DSSCL_WRITEPRIMARY 4u
#define DSSPEAKER_STEREO 4u

#define DSBSIZE_MIN 4u
#define DSBSIZE_MAX 0x0FFFFFFFu
#define DSBFREQUENCY_ORIGINAL 0u

#define DSCAPS_PRIMARYMONO 0x00000001u
#define DSCAPS_PRIMARYSTEREO 0x00000002u
#define DSCAPS_PRIMARY8BIT 0x00000004u
#define DSCAPS_PRIMARY16BIT 0x00000008u
#define DSCAPS_CONTINUOUSRATE 0x00000010u
#define DSCAPS_SECONDARYMONO 0x00000100u
#define DSCAPS_SECONDARYSTEREO 0x00000200u
#define DSCAPS_SECONDARY8BIT 0x00000400u
#define DSCAPS_SECONDARY16BIT 0x00000800u

#define DS3D_IMMEDIATE 0x00000000u
#define DS3D_DEFERRED 0x00000001u

#define DS_WAVE_FORMAT_PCM 1u

#pragma pack(push, 1)

typedef struct {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
} ds_guid_t;

typedef struct {
    uint16_t format_tag, channels;
    uint32_t samples_per_second, bytes_per_second;
    uint16_t block_align, bits_per_sample, extra_size;
} ds_waveformatex_t;

/* DSBUFFERDESC1 is the first 20 bytes; the DirectX 7 descriptor adds the 3D algorithm. */
typedef struct {
    uint32_t size, flags, buffer_bytes, reserved;
    uint32_t format_address; /* guest address of a ds_waveformatex_t */
    ds_guid_t algorithm_3d;
} ds_bufferdesc_t;

typedef struct {
    uint32_t size, flags, min_secondary_rate, max_secondary_rate, primary_buffers;
    uint32_t max_hw_mixing_all, max_hw_mixing_static, max_hw_mixing_streaming;
    uint32_t free_hw_mixing_all, free_hw_mixing_static, free_hw_mixing_streaming;
    uint32_t max_hw_3d_all, max_hw_3d_static, max_hw_3d_streaming;
    uint32_t free_hw_3d_all, free_hw_3d_static, free_hw_3d_streaming;
    uint32_t total_hw_memory_bytes, free_hw_memory_bytes, max_contig_free_hw_memory_bytes;
    uint32_t unlock_transfer_rate_hw_buffers, play_cpu_overhead_sw_buffers;
    uint32_t reserved1, reserved2;
} ds_caps_t;

#pragma pack(pop)

#define DS_BUFFERDESC_V1_SIZE 20u
#define DS_BUFFERDESC_V2_SIZE 36u

JPT_STATIC_ASSERT(sizeof(ds_guid_t) == 16, guid_size);
JPT_STATIC_ASSERT(sizeof(ds_waveformatex_t) == 18, waveformatex_size);
JPT_STATIC_ASSERT(offsetof(ds_waveformatex_t, samples_per_second) == 4 && offsetof(ds_waveformatex_t, bits_per_sample) == 14,
                  waveformatex_offsets);
JPT_STATIC_ASSERT(sizeof(ds_bufferdesc_t) == DS_BUFFERDESC_V2_SIZE, bufferdesc_size);
JPT_STATIC_ASSERT(offsetof(ds_bufferdesc_t, format_address) == DS_BUFFERDESC_V1_SIZE - 4, bufferdesc_format_offset);
JPT_STATIC_ASSERT(sizeof(ds_caps_t) == 0x60, caps_size);
JPT_STATIC_ASSERT(offsetof(ds_caps_t, max_hw_3d_all) == 0x2C, caps_3d_offset);
JPT_STATIC_ASSERT(offsetof(ds_caps_t, min_secondary_rate) == 8 && offsetof(ds_caps_t, primary_buffers) == 16, caps_rate_offsets);

#endif
