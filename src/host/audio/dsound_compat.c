#include "dsound_compat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_log.h"
#include "audio_mixer.h"
#include "audio_policy.h"
#include "audio_spatial.h"

#define DEFAULT_RATE 22050u
#define SPEAKER_CONFIG_MAX 8
#define DS3D_IMMEDIATE_APPLY 0u
#define DS3D_DEFERRED_APPLY 1u

static const GUID k_iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID k_iid_direct_sound = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_listener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_buffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_buffer_3d = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_default_playback = { 0xDEF00000, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };
static const GUID k_guid_null = { 0 };
static const GUID k_default_voice = { 0xDEF00002, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };

typedef struct {
    const void* vtbl;
    void* owner;
} face_t;

typedef struct ds_buffer ds_buffer_t;

typedef struct {
    face_t face;
    LONG refs;
    DWORD coop, speakers;
    ds_buffer_t* primary;
    audio_listener_t listener, pending;
} ds_device_t;

struct ds_buffer {
    face_t face, face_3d, face_listener;
    LONG refs;
    ds_device_t* device;
    ds_buffer_t* next;
    int primary, primary_playing, released_voice;
    DWORD caps;
    audio_format_t format;
    audio_voice_t* voice;
    LONG volume;
    DWORD frequency;
    audio_source_t source, pending_source;
};

static struct {
    const audio_backend_t* backend;
    audio_mixer_t* mixer;
    CRITICAL_SECTION cs;
    int ready, down, announced;
    ds_buffer_t* buffers;
    dsound_compat_counts_t counts;
} S;

static const void* device_vtbl(void);
static const void* buffer_vtbl(void);
static const void* buffer_3d_vtbl(void);
static const void* listener_vtbl(void);

#define ENTER() EnterCriticalSection(&S.cs)
#define LEAVE() LeaveCriticalSection(&S.cs)
#define RETURN(value)                                                                                                   \
    do {                                                                                                                \
        HRESULT result_ = (value);                                                                                      \
        LEAVE();                                                                                                        \
        return result_;                                                                                                 \
    } while (0)

#define DEVICE(This) ((ds_device_t*)((face_t*)(This))->owner)
#define BUFFER(This) ((ds_buffer_t*)((face_t*)(This))->owner)

static int same_guid(const GUID* a, const GUID* b) { return a && b && !memcmp(a, b, sizeof *a); }

void dsound_compat_install(const audio_backend_t* backend) {
    S.backend = backend;
    if (!S.ready) {
        InitializeCriticalSection(&S.cs);
        S.ready = 1;
    }
}

audio_mixer_t* dsound_compat_mixer(void) { return S.mixer; }

void dsound_compat_counts(dsound_compat_counts_t* counts) {
    ENTER();
    *counts = S.counts;
    LEAVE();
}

/* ---- the output ------------------------------------------------------------------------------------------------- */

static int output_open(void) {
    if (S.down) return 0;
    if (S.mixer) return 1;
    if (!S.backend) return 0;
    S.mixer = S.backend->open(DEFAULT_RATE);
    if (!S.mixer) {
        host_log("audio", "the SDL3 audio device could not be opened: DirectSound reports no driver");
        return 0;
    }
    if (!S.announced) {
        host_log("audio", "SDL3 backend initialized, %u Hz, stereo (%s)", audio_mixer_rate(S.mixer), S.backend->name());
        host_log("audio", "DirectSound compatibility active");
        S.announced = 1;
    }
    return 1;
}

void dsound_compat_shutdown(void) {
    ds_buffer_t* b;
    if (!S.ready) return;
    ENTER();
    S.down = 1;
    if (S.mixer) {
        S.backend->close();
        S.mixer = NULL;
    }
    for (b = S.buffers; b; b = b->next) b->voice = NULL;
    host_log_perf("audio", "buffers created %u, released %u, peak %u, still alive %u", S.counts.buffers_created,
                  S.counts.buffers_released, S.counts.buffers_peak, S.counts.buffers_live);
    LEAVE();
}

/* ---- objects ---------------------------------------------------------------------------------------------------- */

static void init_buffer_faces(ds_buffer_t* b) {
    b->face.vtbl = buffer_vtbl();
    b->face_3d.vtbl = buffer_3d_vtbl();
    b->face_listener.vtbl = listener_vtbl();
    b->face.owner = b->face_3d.owner = b->face_listener.owner = b;
}

static void device_release_ref(ds_device_t* d) {
    if (InterlockedDecrement(&d->refs) == 0) free(d);
}

static void register_buffer(ds_buffer_t* b) {
    b->next = S.buffers;
    S.buffers = b;
    S.counts.buffers_created++;
    S.counts.buffers_live++;
    if (S.counts.buffers_live > S.counts.buffers_peak) S.counts.buffers_peak = S.counts.buffers_live;
}

static void unregister_buffer(ds_buffer_t* b) {
    ds_buffer_t** at;
    for (at = &S.buffers; *at && *at != b; at = &(*at)->next) {
    }
    if (*at) *at = b->next;
    S.counts.buffers_released++;
    S.counts.buffers_live--;
}

static void refresh_spatial(ds_buffer_t* b) {
    float left, right;
    if (!b->voice || !(b->caps & DSBCAPS_CTRL3D)) return;
    audio_spatial_gains(&b->device->listener, &b->source, &left, &right);
    audio_voice_set_spatial(b->voice, left, right);
}

static void refresh_gain(ds_buffer_t* b) {
    if (b->voice) audio_voice_set_gain(b->voice, audio_volume_gain(b->volume));
}

/* ---- IUnknown, shared by every interface ------------------------------------------------------------------------ */

static ULONG buffer_add_ref(ds_buffer_t* b) { return (ULONG)InterlockedIncrement(&b->refs); }

static ULONG buffer_release(ds_buffer_t* b) {
    LONG refs = InterlockedDecrement(&b->refs);
    if (refs != 0) return (ULONG)refs;
    ENTER();
    unregister_buffer(b);
    if (b->voice) audio_voice_destroy(b->voice);
    if (b->device->primary == b) b->device->primary = NULL;
    device_release_ref(b->device);
    LEAVE();
    free(b);
    return 0;
}

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDirectSound* This, REFIID riid, LPVOID* out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!same_guid(riid, &k_iid_unknown) && !same_guid(riid, &k_iid_direct_sound)) return E_NOINTERFACE;
    InterlockedIncrement(&DEVICE(This)->refs);
    *out = This;
    return DS_OK;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDirectSound* This) { return (ULONG)InterlockedIncrement(&DEVICE(This)->refs); }

static ULONG STDMETHODCALLTYPE device_Release(IDirectSound* This) {
    ds_device_t* d = DEVICE(This);
    LONG refs = InterlockedDecrement(&d->refs);
    if (refs == 0) free(d);
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE buffer_QueryInterface(IDirectSoundBuffer* This, REFIID riid, LPVOID* out) {
    ds_buffer_t* b = BUFFER(This);
    face_t* face = NULL;
    if (!out) return E_POINTER;
    *out = NULL;
    if (same_guid(riid, &k_iid_unknown) || same_guid(riid, &k_iid_buffer)) face = &b->face;
    else if (same_guid(riid, &k_iid_buffer_3d) && !b->primary && (b->caps & DSBCAPS_CTRL3D)) face = &b->face_3d;
    else if (same_guid(riid, &k_iid_listener) && b->primary && (b->caps & DSBCAPS_CTRL3D)) face = &b->face_listener;
    if (!face) return E_NOINTERFACE;
    buffer_add_ref(b);
    *out = face;
    return DS_OK;
}

static ULONG STDMETHODCALLTYPE buffer_AddRef(IDirectSoundBuffer* This) { return buffer_add_ref(BUFFER(This)); }
static ULONG STDMETHODCALLTYPE buffer_Release(IDirectSoundBuffer* This) { return buffer_release(BUFFER(This)); }

static HRESULT STDMETHODCALLTYPE buffer_3d_QueryInterface(IDirectSound3DBuffer* This, REFIID riid, LPVOID* out) {
    return buffer_QueryInterface((IDirectSoundBuffer*)&BUFFER(This)->face, riid, out);
}
static ULONG STDMETHODCALLTYPE buffer_3d_AddRef(IDirectSound3DBuffer* This) { return buffer_add_ref(BUFFER(This)); }
static ULONG STDMETHODCALLTYPE buffer_3d_Release(IDirectSound3DBuffer* This) { return buffer_release(BUFFER(This)); }

static HRESULT STDMETHODCALLTYPE listener_QueryInterface(IDirectSound3DListener* This, REFIID riid, LPVOID* out) {
    return buffer_QueryInterface((IDirectSoundBuffer*)&BUFFER(This)->face, riid, out);
}
static ULONG STDMETHODCALLTYPE listener_AddRef(IDirectSound3DListener* This) { return buffer_add_ref(BUFFER(This)); }
static ULONG STDMETHODCALLTYPE listener_Release(IDirectSound3DListener* This) { return buffer_release(BUFFER(This)); }

/* ---- IDirectSound ----------------------------------------------------------------------------------------------- */

static int format_valid_for_buffer(const WAVEFORMATEX* f, audio_format_t* out) {
    if (!f || f->wFormatTag != WAVE_FORMAT_PCM) return 0;
    out->rate = f->nSamplesPerSec;
    out->channels = f->nChannels;
    out->bits = f->wBitsPerSample;
    return audio_format_valid(out) && f->nSamplesPerSec >= DS_FREQUENCY_MIN && f->nSamplesPerSec <= DS_FREQUENCY_MAX;
}

static ds_buffer_t* new_buffer(ds_device_t* d, DWORD caps) {
    ds_buffer_t* b = (ds_buffer_t*)calloc(1, sizeof *b);
    if (!b) return NULL;
    init_buffer_faces(b);
    b->refs = 1;
    b->device = d;
    b->caps = caps;
    audio_source_default(&b->source);
    b->pending_source = b->source;
    InterlockedIncrement(&d->refs);
    register_buffer(b);
    return b;
}

static HRESULT create_primary(ds_device_t* d, const DSBUFFERDESC* desc, LPDIRECTSOUNDBUFFER* out) {
    if (desc->lpwfxFormat || desc->dwBufferBytes) return DSERR_INVALIDPARAM;
    if (d->primary) {
        buffer_add_ref(d->primary);
        *out = (LPDIRECTSOUNDBUFFER)&d->primary->face;
        return DS_OK;
    }
    d->primary = new_buffer(d, desc->dwFlags);
    if (!d->primary) return DSERR_OUTOFMEMORY;
    d->primary->primary = 1;
    d->primary->format.rate = DEFAULT_RATE;
    d->primary->format.channels = 1;
    d->primary->format.bits = 8;
    *out = (LPDIRECTSOUNDBUFFER)&d->primary->face;
    return DS_OK;
}

static HRESULT create_secondary(ds_device_t* d, const DSBUFFERDESC* desc, LPDIRECTSOUNDBUFFER* out) {
    audio_format_t format;
    ds_buffer_t* b;
    if (!desc->lpwfxFormat) return DSERR_INVALIDPARAM;
    if (!format_valid_for_buffer(desc->lpwfxFormat, &format)) return DSERR_BADFORMAT;
    if (desc->dwBufferBytes < DSBSIZE_MIN || desc->dwBufferBytes > DSBSIZE_MAX ||
        desc->dwBufferBytes % audio_format_block(&format))
        return DSERR_INVALIDPARAM;
    if ((desc->dwFlags & DSBCAPS_CTRL3D) && (format.channels != 1 || (desc->dwFlags & DSBCAPS_CTRLPAN)))
        return DSERR_INVALIDPARAM;
    if (desc->dwFlags & DSBCAPS_LOCHARDWARE) return DSERR_ALLOCATED;
    if (!S.mixer) return DSERR_NODRIVER;
    b = new_buffer(d, desc->dwFlags);
    if (!b) return DSERR_OUTOFMEMORY;
    b->format = format;
    b->voice = audio_voice_create(S.mixer, &format, desc->dwBufferBytes);
    if (!b->voice) {
        buffer_release(b);
        return DSERR_OUTOFMEMORY;
    }
    *out = (LPDIRECTSOUNDBUFFER)&b->face;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE device_CreateSoundBuffer(IDirectSound* This, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out,
                                                          LPUNKNOWN outer) {
    HRESULT hr;
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!desc || (desc->dwSize != sizeof(DSBUFFERDESC1) && desc->dwSize != sizeof(DSBUFFERDESC))) return DSERR_INVALIDPARAM;
    ENTER();
    if (S.down || !S.mixer) RETURN(DSERR_NODRIVER);
    hr = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) ? create_primary(DEVICE(This), desc, out)
                                                 : create_secondary(DEVICE(This), desc, out);
    LEAVE();
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_GetCaps(IDirectSound* This, LPDSCAPS caps) {
    (void)This;
    if (!caps || caps->dwSize != sizeof(DSCAPS)) return DSERR_INVALIDPARAM;
    memset((BYTE*)caps + sizeof caps->dwSize, 0, sizeof *caps - sizeof caps->dwSize);
    caps->dwFlags = DSCAPS_CONTINUOUSRATE | DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT |
                    DSCAPS_PRIMARY16BIT | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT |
                    DSCAPS_SECONDARY16BIT;
    caps->dwMinSecondarySampleRate = DS_FREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = DS_FREQUENCY_MAX;
    caps->dwPrimaryBuffers = 1;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE device_DuplicateSoundBuffer(IDirectSound* This, LPDIRECTSOUNDBUFFER original,
                                                              LPDIRECTSOUNDBUFFER* out) {
    ds_buffer_t *src, *b;
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (!original || ((face_t*)original)->vtbl != buffer_vtbl()) return DSERR_INVALIDPARAM;
    ENTER();
    src = BUFFER(original);
    if (src->primary || !src->voice || S.down || !S.mixer) RETURN(src->primary ? DSERR_INVALIDPARAM : DSERR_BUFFERLOST);
    b = new_buffer(DEVICE(This), src->caps);
    if (!b) RETURN(DSERR_OUTOFMEMORY);
    b->format = src->format;
    b->volume = src->volume;
    b->frequency = src->frequency;
    b->source = src->source;
    b->pending_source = src->pending_source;
    b->voice = audio_voice_duplicate(S.mixer, src->voice);
    if (!b->voice) {
        buffer_release(b);
        RETURN(DSERR_OUTOFMEMORY);
    }
    audio_voice_set_frequency(b->voice, b->frequency);
    refresh_gain(b);
    refresh_spatial(b);
    *out = (LPDIRECTSOUNDBUFFER)&b->face;
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE device_SetCooperativeLevel(IDirectSound* This, HWND window, DWORD level) {
    (void)window;
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY) return DSERR_INVALIDPARAM;
    DEVICE(This)->coop = level;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE device_Compact(IDirectSound* This) {
    return DEVICE(This)->coop >= DSSCL_PRIORITY ? DS_OK : DSERR_PRIOLEVELNEEDED;
}

static HRESULT STDMETHODCALLTYPE device_GetSpeakerConfig(IDirectSound* This, LPDWORD config) {
    if (!config) return DSERR_INVALIDPARAM;
    *config = DEVICE(This)->speakers;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE device_SetSpeakerConfig(IDirectSound* This, DWORD config) {
    DWORD speakers = config & 0xFF;
    if (speakers < 1 || speakers > SPEAKER_CONFIG_MAX) return DSERR_INVALIDPARAM;
    DEVICE(This)->speakers = config;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE device_Initialize(IDirectSound* This, LPCGUID guid) {
    (void)This;
    (void)guid;
    return DSERR_ALREADYINITIALIZED;
}

static const IDirectSoundVtbl k_device_vtbl = {
    device_QueryInterface, device_AddRef, device_Release, device_CreateSoundBuffer, device_GetCaps,
    device_DuplicateSoundBuffer, device_SetCooperativeLevel, device_Compact, device_GetSpeakerConfig,
    device_SetSpeakerConfig, device_Initialize,
};

HRESULT WINAPI compat_DirectSoundCreate(LPCGUID device, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    ds_device_t* d;
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!S.ready) return DSERR_NODRIVER;
    if (device && !same_guid(device, &k_default_playback) && !same_guid(device, &k_default_voice) &&
        !same_guid(device, &k_guid_null))
        return DSERR_NODRIVER;
    ENTER();
    if (!output_open()) RETURN(DSERR_NODRIVER);
    d = (ds_device_t*)calloc(1, sizeof *d);
    if (!d) RETURN(DSERR_OUTOFMEMORY);
    d->face.vtbl = device_vtbl();
    d->face.owner = d;
    d->refs = 1;
    d->coop = DSSCL_NORMAL;
    d->speakers = DSSPEAKER_STEREO;
    audio_listener_default(&d->listener);
    d->pending = d->listener;
    S.counts.devices_created++;
    *out = (LPDIRECTSOUND)&d->face;
    RETURN(DS_OK);
}

/* ---- IDirectSoundBuffer ----------------------------------------------------------------------------------------- */

#define SECONDARY_VOICE(b) ((b)->voice)

static HRESULT STDMETHODCALLTYPE buffer_GetCurrentPosition(IDirectSoundBuffer* This, LPDWORD play, LPDWORD write) {
    ds_buffer_t* b = BUFFER(This);
    uint32_t p = 0, w = 0;
    ENTER();
    if (b->primary) RETURN(DSERR_INVALIDCALL);
    if (!SECONDARY_VOICE(b)) RETURN(DSERR_BUFFERLOST);
    audio_voice_positions(b->voice, &p, &w);
    if (play) *play = p;
    if (write) *write = w;
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_GetVolume(IDirectSoundBuffer* This, LPLONG volume) {
    ds_buffer_t* b = BUFFER(This);
    if (!volume) return DSERR_INVALIDPARAM;
    if (!(b->caps & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    *volume = b->volume;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buffer_GetStatus(IDirectSoundBuffer* This, LPDWORD status) {
    ds_buffer_t* b = BUFFER(This);
    if (!status) return DSERR_INVALIDPARAM;
    ENTER();
    if (b->primary) *status = b->primary_playing ? (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING) : 0;
    else *status = b->voice ? audio_voice_status(b->voice) : DSBSTATUS_BUFFERLOST;
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_Lock(IDirectSoundBuffer* This, DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1,
                                              LPVOID* p2, LPDWORD n2, DWORD flags) {
    ds_buffer_t* b = BUFFER(This);
    uint8_t *r1 = NULL, *r2 = NULL;
    uint32_t l1 = 0, l2 = 0;
    if (p1) *p1 = NULL;
    if (n1) *n1 = 0;
    if (p2) *p2 = NULL;
    if (n2) *n2 = 0;
    if (!p1 || !n1) return DSERR_INVALIDPARAM;
    ENTER();
    if (b->primary) RETURN(DSERR_INVALIDCALL);
    if (!b->voice) RETURN(DSERR_BUFFERLOST);
    if (flags & DSBLOCK_FROMWRITECURSOR) audio_voice_positions(b->voice, NULL, &offset);
    if (flags & DSBLOCK_ENTIREBUFFER) {
        offset = 0;
        bytes = audio_voice_size(b->voice);
    }
    if (!audio_voice_lock(b->voice, offset, bytes, &r1, &l1, &r2, &l2)) RETURN(DSERR_INVALIDPARAM);
    *p1 = r1;
    *n1 = l1;
    if (p2) *p2 = r2;
    if (n2) *n2 = l2;
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_Play(IDirectSoundBuffer* This, DWORD reserved1, DWORD priority, DWORD flags) {
    ds_buffer_t* b = BUFFER(This);
    (void)priority;
    if (reserved1) return DSERR_INVALIDPARAM;
    ENTER();
    if (b->primary) {
        if (!(flags & DSBPLAY_LOOPING)) RETURN(DSERR_INVALIDPARAM);
        b->primary_playing = 1;
        RETURN(DS_OK);
    }
    if (!b->voice) RETURN(DSERR_BUFFERLOST);
    audio_voice_play(b->voice, (flags & DSBPLAY_LOOPING) != 0);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_SetCurrentPosition(IDirectSoundBuffer* This, DWORD position) {
    ds_buffer_t* b = BUFFER(This);
    ENTER();
    if (b->primary) RETURN(DSERR_INVALIDCALL);
    if (!b->voice) RETURN(DSERR_BUFFERLOST);
    RETURN(audio_voice_set_position(b->voice, position) ? DS_OK : DSERR_INVALIDPARAM);
}

static HRESULT STDMETHODCALLTYPE buffer_SetFormat(IDirectSoundBuffer* This, LPCWAVEFORMATEX wfx) {
    ds_buffer_t* b = BUFFER(This);
    audio_format_t format;
    if (!b->primary) return DSERR_INVALIDCALL;
    if (b->device->coop < DSSCL_PRIORITY) return DSERR_PRIOLEVELNEEDED;
    if (!format_valid_for_buffer(wfx, &format)) return DSERR_INVALIDPARAM;
    ENTER();
    b->format = format;
    if (S.mixer && format.rate != audio_mixer_rate(S.mixer)) S.backend->set_rate(format.rate);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_SetVolume(IDirectSoundBuffer* This, LONG volume) {
    ds_buffer_t* b = BUFFER(This);
    if (!(b->caps & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (volume < DS_VOLUME_MIN || volume > DS_VOLUME_MAX) return DSERR_INVALIDPARAM;
    ENTER();
    b->volume = volume;
    if (b->primary) {
        if (S.mixer) audio_mixer_set_master_gain(S.mixer, audio_volume_gain(volume));
    } else {
        refresh_gain(b);
    }
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_SetFrequency(IDirectSoundBuffer* This, DWORD frequency) {
    ds_buffer_t* b = BUFFER(This);
    int hz;
    if (b->primary) return DSERR_INVALIDCALL;
    if (!(b->caps & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    hz = audio_resolve_frequency(frequency, 0);
    if (hz < 0) return DSERR_INVALIDPARAM;
    ENTER();
    b->frequency = (DWORD)hz;
    if (b->voice) audio_voice_set_frequency(b->voice, (uint32_t)hz);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_Stop(IDirectSoundBuffer* This) {
    ds_buffer_t* b = BUFFER(This);
    ENTER();
    if (b->primary) b->primary_playing = 0;
    else if (b->voice) audio_voice_stop(b->voice);
    else RETURN(DSERR_BUFFERLOST);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_Unlock(IDirectSoundBuffer* This, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) {
    ds_buffer_t* b = BUFFER(This);
    (void)n1;
    (void)n2;
    if (b->primary) return DSERR_INVALIDCALL;
    if (!p1 && !p2) return DSERR_INVALIDPARAM;
    return b->voice ? DS_OK : DSERR_BUFFERLOST;
}

static HRESULT STDMETHODCALLTYPE buffer_Restore(IDirectSoundBuffer* This) {
    return BUFFER(This)->voice || BUFFER(This)->primary ? DS_OK : DSERR_BUFFERLOST;
}

#define UNSUPPORTED_BUFFER(name, ...)                                                                                   \
    static HRESULT STDMETHODCALLTYPE buffer_##name(IDirectSoundBuffer* This, __VA_ARGS__) {                             \
        (void)This;                                                                                                     \
        return DSERR_UNSUPPORTED;                                                                                       \
    }

UNSUPPORTED_BUFFER(GetCaps, LPDSBCAPS caps)
UNSUPPORTED_BUFFER(GetFormat, LPWAVEFORMATEX format, DWORD size, LPDWORD written)
UNSUPPORTED_BUFFER(GetPan, LPLONG pan)
UNSUPPORTED_BUFFER(GetFrequency, LPDWORD frequency)
UNSUPPORTED_BUFFER(Initialize, LPDIRECTSOUND device, LPCDSBUFFERDESC desc)
UNSUPPORTED_BUFFER(SetPan, LONG pan)

static const IDirectSoundBufferVtbl k_buffer_vtbl = {
    buffer_QueryInterface, buffer_AddRef, buffer_Release, buffer_GetCaps, buffer_GetCurrentPosition, buffer_GetFormat,
    buffer_GetVolume, buffer_GetPan, buffer_GetFrequency, buffer_GetStatus, buffer_Initialize, buffer_Lock, buffer_Play,
    buffer_SetCurrentPosition, buffer_SetFormat, buffer_SetVolume, buffer_SetPan, buffer_SetFrequency, buffer_Stop,
    buffer_Unlock, buffer_Restore,
};

/* ---- IDirectSound3DBuffer --------------------------------------------------------------------------------------- */

#define SOURCE_EDIT(b, apply, ...)                                                                                      \
    do {                                                                                                                \
        audio_source_t* s = &(b)->pending_source;                                                                       \
        __VA_ARGS__;                                                                                                    \
        if ((apply) == DS3D_IMMEDIATE_APPLY) {                                                                          \
            s = &(b)->source;                                                                                           \
            __VA_ARGS__;                                                                                                \
            refresh_spatial(b);                                                                                         \
        }                                                                                                               \
    } while (0)

static int apply_valid(DWORD apply) { return apply == DS3D_IMMEDIATE_APPLY || apply == DS3D_DEFERRED_APPLY; }

static HRESULT STDMETHODCALLTYPE buffer_3d_SetPosition(IDirectSound3DBuffer* This, float x, float y, float z, DWORD apply) {
    ds_buffer_t* b = BUFFER(This);
    if (!apply_valid(apply)) return DSERR_INVALIDPARAM;
    ENTER();
    SOURCE_EDIT(b, apply, s->position.x = x; s->position.y = y; s->position.z = z);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_3d_SetMinDistance(IDirectSound3DBuffer* This, float distance, DWORD apply) {
    ds_buffer_t* b = BUFFER(This);
    if (!apply_valid(apply) || !(distance >= 0.0f)) return DSERR_INVALIDPARAM;
    ENTER();
    SOURCE_EDIT(b, apply, s->min_distance = distance);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_3d_SetConeAngles(IDirectSound3DBuffer* This, DWORD inside, DWORD outside, DWORD apply) {
    ds_buffer_t* b = BUFFER(This);
    if (!apply_valid(apply) || inside > 360 || outside > 360) return DSERR_INVALIDPARAM;
    ENTER();
    SOURCE_EDIT(b, apply, s->inside_angle = inside; s->outside_angle = outside);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_3d_SetConeOrientation(IDirectSound3DBuffer* This, float x, float y, float z,
                                                              DWORD apply) {
    ds_buffer_t* b = BUFFER(This);
    if (!apply_valid(apply)) return DSERR_INVALIDPARAM;
    ENTER();
    SOURCE_EDIT(b, apply, s->cone_orientation.x = x; s->cone_orientation.y = y; s->cone_orientation.z = z);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE buffer_3d_SetConeOutsideVolume(IDirectSound3DBuffer* This, LONG volume, DWORD apply) {
    ds_buffer_t* b = BUFFER(This);
    if (!apply_valid(apply) || volume < DS_VOLUME_MIN || volume > DS_VOLUME_MAX) return DSERR_INVALIDPARAM;
    ENTER();
    SOURCE_EDIT(b, apply, s->outside_volume = volume);
    RETURN(DS_OK);
}

#define UNSUPPORTED_3D(name, ...)                                                                                       \
    static HRESULT STDMETHODCALLTYPE buffer_3d_##name(IDirectSound3DBuffer* This, __VA_ARGS__) {                        \
        (void)This;                                                                                                     \
        return DSERR_UNSUPPORTED;                                                                                       \
    }

UNSUPPORTED_3D(GetAllParameters, LPDS3DBUFFER p)
UNSUPPORTED_3D(GetConeAngles, LPDWORD inside, LPDWORD outside)
UNSUPPORTED_3D(GetConeOrientation, LPD3DVECTOR v)
UNSUPPORTED_3D(GetConeOutsideVolume, LPLONG volume)
UNSUPPORTED_3D(GetMaxDistance, float* distance)
UNSUPPORTED_3D(GetMinDistance, float* distance)
UNSUPPORTED_3D(GetMode, LPDWORD mode)
UNSUPPORTED_3D(GetPosition, LPD3DVECTOR v)
UNSUPPORTED_3D(GetVelocity, LPD3DVECTOR v)
UNSUPPORTED_3D(SetAllParameters, LPCDS3DBUFFER p, DWORD apply)
UNSUPPORTED_3D(SetMaxDistance, float distance, DWORD apply)
UNSUPPORTED_3D(SetMode, DWORD mode, DWORD apply)
UNSUPPORTED_3D(SetVelocity, float x, float y, float z, DWORD apply)

static const IDirectSound3DBufferVtbl k_buffer_3d_vtbl = {
    buffer_3d_QueryInterface, buffer_3d_AddRef, buffer_3d_Release, buffer_3d_GetAllParameters, buffer_3d_GetConeAngles,
    buffer_3d_GetConeOrientation, buffer_3d_GetConeOutsideVolume, buffer_3d_GetMaxDistance, buffer_3d_GetMinDistance,
    buffer_3d_GetMode, buffer_3d_GetPosition, buffer_3d_GetVelocity, buffer_3d_SetAllParameters, buffer_3d_SetConeAngles,
    buffer_3d_SetConeOrientation, buffer_3d_SetConeOutsideVolume, buffer_3d_SetMaxDistance, buffer_3d_SetMinDistance,
    buffer_3d_SetMode, buffer_3d_SetPosition, buffer_3d_SetVelocity,
};

/* ---- IDirectSound3DListener ------------------------------------------------------------------------------------- */

#define LISTENER_EDIT(d, apply, ...)                                                                                    \
    do {                                                                                                                \
        audio_listener_t* l = &(d)->pending;                                                                            \
        __VA_ARGS__;                                                                                                    \
        if ((apply) == DS3D_IMMEDIATE_APPLY) {                                                                          \
            l = &(d)->listener;                                                                                         \
            __VA_ARGS__;                                                                                                \
            refresh_listener(d);                                                                                        \
        }                                                                                                               \
    } while (0)

static void refresh_listener(ds_device_t* d) {
    for (ds_buffer_t* b = S.buffers; b; b = b->next)
        if (b->device == d) refresh_spatial(b);
}

static HRESULT STDMETHODCALLTYPE listener_SetPosition(IDirectSound3DListener* This, float x, float y, float z, DWORD apply) {
    ds_device_t* d = BUFFER(This)->device;
    if (!apply_valid(apply)) return DSERR_INVALIDPARAM;
    ENTER();
    LISTENER_EDIT(d, apply, l->position.x = x; l->position.y = y; l->position.z = z);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE listener_SetOrientation(IDirectSound3DListener* This, float fx, float fy, float fz, float tx,
                                                         float ty, float tz, DWORD apply) {
    ds_device_t* d = BUFFER(This)->device;
    if (!apply_valid(apply)) return DSERR_INVALIDPARAM;
    ENTER();
    LISTENER_EDIT(d, apply, l->front.x = fx; l->front.y = fy; l->front.z = fz; l->top.x = tx; l->top.y = ty; l->top.z = tz);
    RETURN(DS_OK);
}

static HRESULT STDMETHODCALLTYPE listener_CommitDeferredSettings(IDirectSound3DListener* This) {
    ds_device_t* d = BUFFER(This)->device;
    ENTER();
    d->listener = d->pending;
    for (ds_buffer_t* b = S.buffers; b; b = b->next) {
        if (b->device != d || !(b->caps & DSBCAPS_CTRL3D)) continue;
        b->source = b->pending_source;
        refresh_spatial(b);
    }
    RETURN(DS_OK);
}

#define UNSUPPORTED_LISTENER(name, ...)                                                                                 \
    static HRESULT STDMETHODCALLTYPE listener_##name(IDirectSound3DListener* This, __VA_ARGS__) {                       \
        (void)This;                                                                                                     \
        return DSERR_UNSUPPORTED;                                                                                       \
    }

UNSUPPORTED_LISTENER(GetAllParameters, LPDS3DLISTENER p)
UNSUPPORTED_LISTENER(GetDistanceFactor, float* v)
UNSUPPORTED_LISTENER(GetDopplerFactor, float* v)
UNSUPPORTED_LISTENER(GetOrientation, LPD3DVECTOR front, LPD3DVECTOR top)
UNSUPPORTED_LISTENER(GetPosition, LPD3DVECTOR v)
UNSUPPORTED_LISTENER(GetRolloffFactor, float* v)
UNSUPPORTED_LISTENER(GetVelocity, LPD3DVECTOR v)
UNSUPPORTED_LISTENER(SetAllParameters, LPCDS3DLISTENER p, DWORD apply)
UNSUPPORTED_LISTENER(SetDistanceFactor, float v, DWORD apply)
UNSUPPORTED_LISTENER(SetDopplerFactor, float v, DWORD apply)
UNSUPPORTED_LISTENER(SetRolloffFactor, float v, DWORD apply)
UNSUPPORTED_LISTENER(SetVelocity, float x, float y, float z, DWORD apply)

static const IDirectSound3DListenerVtbl k_listener_vtbl = {
    listener_QueryInterface, listener_AddRef, listener_Release, listener_GetAllParameters, listener_GetDistanceFactor,
    listener_GetDopplerFactor, listener_GetOrientation, listener_GetPosition, listener_GetRolloffFactor,
    listener_GetVelocity, listener_SetAllParameters, listener_SetDistanceFactor, listener_SetDopplerFactor,
    listener_SetOrientation, listener_SetPosition, listener_SetRolloffFactor, listener_SetVelocity,
    listener_CommitDeferredSettings,
};

/* ---- layout and completeness ------------------------------------------------------------------------------------ */

static const void* device_vtbl(void) { return &k_device_vtbl; }
static const void* buffer_vtbl(void) { return &k_buffer_vtbl; }
static const void* buffer_3d_vtbl(void) { return &k_buffer_3d_vtbl; }
static const void* listener_vtbl(void) { return &k_listener_vtbl; }

C_ASSERT(sizeof(IDirectSoundVtbl) == 11 * sizeof(void*));
C_ASSERT(sizeof(IDirectSoundBufferVtbl) == 21 * sizeof(void*));
C_ASSERT(sizeof(IDirectSound3DBufferVtbl) == 21 * sizeof(void*));
C_ASSERT(sizeof(IDirectSound3DListenerVtbl) == 18 * sizeof(void*));

static int slots_filled(const void* const* slots, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!slots[i]) return 0;
    return 1;
}

int dsound_compat_vtables_complete(void) {
    return slots_filled((const void* const*)&k_device_vtbl, sizeof k_device_vtbl / sizeof(void*)) &&
           slots_filled((const void* const*)&k_buffer_vtbl, sizeof k_buffer_vtbl / sizeof(void*)) &&
           slots_filled((const void* const*)&k_buffer_3d_vtbl, sizeof k_buffer_3d_vtbl / sizeof(void*)) &&
           slots_filled((const void* const*)&k_listener_vtbl, sizeof k_listener_vtbl / sizeof(void*));
}
