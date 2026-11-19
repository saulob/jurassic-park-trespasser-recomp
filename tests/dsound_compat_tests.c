/* The DirectSound compatibility layer as the guest and SMACKW32.DLL use it: COM objects called through their vtables at
 * the slots the retail code calls, over a mixer with no audio device, and the import gate that keeps the system
 * DSound.dll from loading. Nothing is played. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsound_compat.h"
#include "dsound_imports.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}

/* A backend without a device: the mixer is pulled by the tests, under a lock like SDL's callback thread would use. */
static struct {
    int opens, closes, fail_open, rate_changes;
    uint32_t last_rate;
    CRITICAL_SECTION cs;
    int cs_ready;
} B;

static void test_lock(void* context) {
    (void)context;
    EnterCriticalSection(&B.cs);
}

static void test_unlock(void* context) {
    (void)context;
    LeaveCriticalSection(&B.cs);
}

static audio_mixer_t* backend_open(uint32_t rate) {
    audio_lock_ops_t ops = { NULL, test_lock, test_unlock };
    if (B.fail_open) return NULL;
    if (!B.cs_ready) {
        InitializeCriticalSection(&B.cs);
        B.cs_ready = 1;
    }
    B.opens++;
    B.last_rate = rate;
    return audio_mixer_create(rate, &ops);
}

static void backend_set_rate(uint32_t rate) {
    B.rate_changes++;
    B.last_rate = rate;
    audio_mixer_set_rate(dsound_compat_mixer(), rate);
}

static void backend_close(void) {
    B.closes++;
    audio_mixer_destroy(dsound_compat_mixer());
}

static const char* backend_name(void) { return "test"; }

static const audio_backend_t k_backend = { backend_open, backend_set_rate, backend_close, backend_name };

static const GUID k_iid_unknown = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID k_iid_buffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_listener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_buffer_3d = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_iid_ks_property_set = { 0x31EFAC30, 0x515C, 0x11D0, { 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93 } };
static const GUID k_iid_notify = { 0xB0210783, 0x89CD, 0x11D0, { 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 } };

static int near_to(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

#define VT(p) ((p)->lpVtbl)

static IDirectSound* make_device(void) {
    IDirectSound* ds = NULL;
    HRESULT hr = compat_DirectSoundCreate(NULL, &ds, NULL);
    CHECK(hr == DS_OK && ds, "DirectSoundCreate (%08lX)", (unsigned long)hr);
    return ds;
}

static IDirectSoundBuffer* make_secondary(IDirectSound* ds, DWORD flags, DWORD rate, WORD channels, WORD bits, DWORD bytes,
                                          HRESULT* result) {
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    IDirectSoundBuffer* buffer = NULL;
    HRESULT hr;
    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = (WORD)(channels * bits / 8);
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwFlags = flags;
    desc.dwBufferBytes = bytes;
    desc.lpwfxFormat = &wfx;
    hr = VT(ds)->CreateSoundBuffer(ds, &desc, &buffer, NULL);
    if (result) *result = hr;
    return buffer;
}

static void fill16(IDirectSoundBuffer* b, DWORD bytes, int16_t value) {
    LPVOID p1 = NULL, p2 = NULL;
    DWORD n1 = 0, n2 = 0;
    HRESULT hr = VT(b)->Lock(b, 0, bytes, &p1, &n1, &p2, &n2, 0);
    CHECK(hr == DS_OK && p1 && n1 == bytes, "Lock for filling");
    for (DWORD i = 0; i < n1 / 2; i++) ((int16_t*)p1)[i] = value;
    CHECK(VT(b)->Unlock(b, p1, n1, p2, n2) == DS_OK, "Unlock");
}

static void mix(float* out, unsigned frames) { audio_mixer_mix(dsound_compat_mixer(), out, frames); }

/* ---- ABI -------------------------------------------------------------------------------------------------------- */

static void test_abi_offsets(void) {
    CHECK(dsound_compat_vtables_complete(), "no vtable has an empty slot");
    CHECK(sizeof(DSCAPS) == 0x60 && offsetof(DSCAPS, dwMaxHw3DAllBuffers) == 0x2C, "DSCAPS as the guest reads it");
    CHECK(offsetof(IDirectSoundVtbl, CreateSoundBuffer) == 0x0C && offsetof(IDirectSoundVtbl, GetCaps) == 0x10 &&
              offsetof(IDirectSoundVtbl, DuplicateSoundBuffer) == 0x14 && offsetof(IDirectSoundVtbl, SetCooperativeLevel) == 0x18 &&
              offsetof(IDirectSoundVtbl, GetSpeakerConfig) == 0x20 && offsetof(IDirectSoundVtbl, SetSpeakerConfig) == 0x24,
          "IDirectSound slots the game calls");
    CHECK(offsetof(IDirectSoundBufferVtbl, GetCurrentPosition) == 0x10 && offsetof(IDirectSoundBufferVtbl, GetVolume) == 0x18 &&
              offsetof(IDirectSoundBufferVtbl, GetStatus) == 0x24 && offsetof(IDirectSoundBufferVtbl, Lock) == 0x2C &&
              offsetof(IDirectSoundBufferVtbl, Play) == 0x30 && offsetof(IDirectSoundBufferVtbl, SetCurrentPosition) == 0x34 &&
              offsetof(IDirectSoundBufferVtbl, SetFormat) == 0x38 && offsetof(IDirectSoundBufferVtbl, SetVolume) == 0x3C &&
              offsetof(IDirectSoundBufferVtbl, SetFrequency) == 0x44 && offsetof(IDirectSoundBufferVtbl, Stop) == 0x48 &&
              offsetof(IDirectSoundBufferVtbl, Unlock) == 0x4C && offsetof(IDirectSoundBufferVtbl, Restore) == 0x50,
          "IDirectSoundBuffer slots the game and Smacker call");
    CHECK(offsetof(IDirectSound3DBufferVtbl, SetConeAngles) == 0x34 && offsetof(IDirectSound3DBufferVtbl, SetConeOrientation) == 0x38 &&
              offsetof(IDirectSound3DBufferVtbl, SetConeOutsideVolume) == 0x3C && offsetof(IDirectSound3DBufferVtbl, SetMinDistance) == 0x44 &&
              offsetof(IDirectSound3DBufferVtbl, SetPosition) == 0x4C,
          "IDirectSound3DBuffer slots the game calls");
    CHECK(offsetof(IDirectSound3DListenerVtbl, SetOrientation) == 0x34 && offsetof(IDirectSound3DListenerVtbl, SetPosition) == 0x38 &&
              offsetof(IDirectSound3DListenerVtbl, CommitDeferredSettings) == 0x44,
          "IDirectSound3DListener slots the game calls");
}

static void test_create(void) {
    IDirectSound *ds = NULL, *again = NULL;
    GUID other = { 1, 2, 3, { 4, 5, 6, 7, 8, 9, 10, 11 } };
    CHECK(compat_DirectSoundCreate(NULL, &ds, NULL) == DSERR_NODRIVER && ds == NULL, "before the backend is installed there is no driver");
    dsound_compat_install(&k_backend);
    B.fail_open = 1;
    CHECK(compat_DirectSoundCreate(NULL, &ds, NULL) == DSERR_NODRIVER && ds == NULL, "a device that cannot be opened is no driver");
    B.fail_open = 0;
    CHECK(compat_DirectSoundCreate(NULL, NULL, NULL) == DSERR_INVALIDPARAM, "no output pointer");
    CHECK(compat_DirectSoundCreate(NULL, &ds, (LPUNKNOWN)&ds) == CLASS_E_NOAGGREGATION, "no aggregation");
    CHECK(compat_DirectSoundCreate(&other, &ds, NULL) == DSERR_NODRIVER, "another device GUID");
    CHECK(compat_DirectSoundCreate(NULL, &ds, NULL) == DS_OK && ds && B.opens == 1 && B.last_rate == 22050, "the first create opens the output at the primary default of 22050 Hz");
    CHECK(compat_DirectSoundCreate(NULL, &again, NULL) == DS_OK && again && B.opens == 1, "a second device shares the output");
    CHECK(VT(ds)->AddRef(ds) == 2 && VT(ds)->Release(ds) == 1, "AddRef and Release");
    {
        void* q = NULL;
        CHECK(VT(ds)->QueryInterface(ds, &k_iid_unknown, &q) == DS_OK && q == ds && VT(ds)->Release(ds) == 1, "QueryInterface for IUnknown");
        CHECK(VT(ds)->QueryInterface(ds, &k_iid_ks_property_set, &q) == E_NOINTERFACE && q == NULL, "no other interface");
    }
    CHECK(VT(again)->Release(again) == 0, "the second device is released");
    CHECK(VT(ds)->Release(ds) == 0, "the first device is released");
    CHECK(B.closes == 0, "the output outlives its devices: a menu or level never reopens it");
}

static void test_game_initialisation(void) {
    IDirectSound* ds = make_device();
    DSCAPS caps;
    DSBUFFERDESC desc;
    IDirectSoundBuffer *primary = NULL, *dummy = NULL;
    IDirectSound3DListener* listener = NULL;
    WAVEFORMATEX stereo;
    DWORD speaker = 0, status = 0;
    LONG volume = 1;
    HRESULT hr;
    memset(&caps, 0, sizeof caps);
    caps.dwSize = 0x20;
    CHECK(VT(ds)->GetCaps(ds, &caps) == DSERR_INVALIDPARAM, "a wrong DSCAPS size is refused");
    caps.dwSize = sizeof caps;
    caps.dwMaxHw3DAllBuffers = 9;
    CHECK(VT(ds)->GetCaps(ds, &caps) == DS_OK && caps.dwMaxHw3DAllBuffers == 0 && caps.dwMaxSecondarySampleRate >= 44100 &&
              caps.dwMinSecondarySampleRate <= 11025 && (caps.dwFlags & DSCAPS_SECONDARY16BIT),
          "no hardware 3D voices: the game takes its software path");
    CHECK(VT(ds)->SetCooperativeLevel(ds, NULL, DSSCL_PRIORITY) == DS_OK, "SetCooperativeLevel(priority)");
    CHECK(VT(ds)->SetCooperativeLevel(ds, NULL, 99) == DSERR_INVALIDPARAM, "an unknown level");
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    CHECK(VT(ds)->CreateSoundBuffer(ds, &desc, &primary, NULL) == DS_OK && primary, "the primary buffer (flags 0x91)");
    desc.lpwfxFormat = &stereo;
    {
        IDirectSoundBuffer* second = NULL;
        CHECK(VT(ds)->CreateSoundBuffer(ds, &desc, &second, NULL) == DSERR_INVALIDPARAM && second == NULL, "a primary with a format is refused");
        desc.lpwfxFormat = NULL;
        CHECK(VT(ds)->CreateSoundBuffer(ds, &desc, &second, NULL) == DS_OK && second == primary, "the primary is one object");
        VT(second)->Release(second);
    }
    memset(&stereo, 0, sizeof stereo);
    stereo.wFormatTag = WAVE_FORMAT_PCM;
    stereo.nChannels = 2;
    stereo.nSamplesPerSec = 22050;
    stereo.wBitsPerSample = 16;
    stereo.nBlockAlign = 4;
    stereo.nAvgBytesPerSec = 88200;
    CHECK(VT(primary)->SetFormat(primary, &stereo) == DS_OK && B.rate_changes == 0, "SetFormat(22050, 2, 16) keeps the output rate");
    CHECK(VT(primary)->QueryInterface(primary, &k_iid_listener, (void**)&listener) == DS_OK && listener, "the 3D listener of the primary");
    CHECK(VT(primary)->Play(primary, 0, 0, DSBPLAY_LOOPING) == DS_OK, "Play(looping)");
    CHECK(VT(primary)->Play(primary, 0, 0, 0) == DSERR_INVALIDPARAM, "the primary only plays looping");
    CHECK(VT(primary)->GetStatus(primary, &status) == DS_OK && status == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "primary status");
    CHECK(VT(primary)->SetVolume(primary, -5000) == DS_OK && VT(primary)->GetVolume(primary, &volume) == DS_OK && volume == -5000,
          "the master volume");
    CHECK(VT(primary)->SetVolume(primary, 0) == DS_OK, "reset");
    CHECK(VT(listener)->SetPosition(listener, 1.0f, 2.0f, 3.0f, DS3D_DEFERRED) == DS_OK, "listener SetPosition, deferred");
    CHECK(VT(listener)->SetOrientation(listener, 0, 0, 1, 0, 1, 0, DS3D_DEFERRED) == DS_OK, "listener SetOrientation, deferred");
    CHECK(VT(listener)->CommitDeferredSettings(listener) == DS_OK, "CommitDeferredSettings");
    CHECK(VT(listener)->SetPosition(listener, 0, 0, 0, 7) == DSERR_INVALIDPARAM, "an unknown apply flag");

    /* the EAX dummy buffer: static, 3D, stereo, which DirectSound refuses */
    desc.dwFlags = DSBCAPS_STATIC | DSBCAPS_CTRL3D;
    desc.dwBufferBytes = 0x80;
    desc.lpwfxFormat = &stereo;
    hr = VT(ds)->CreateSoundBuffer(ds, &desc, &dummy, NULL);
    CHECK(hr == DSERR_INVALIDPARAM && dummy == NULL, "the EAX probe fails as it does on Windows, so the game leaves EAX off");

    CHECK(VT(ds)->GetSpeakerConfig(ds, &speaker) == DS_OK && speaker == DSSPEAKER_STEREO, "speaker configuration");
    CHECK(VT(ds)->SetSpeakerConfig(ds, 2) == DS_OK && VT(ds)->GetSpeakerConfig(ds, &speaker) == DS_OK && speaker == 2, "set speakers");
    CHECK(VT(ds)->SetSpeakerConfig(ds, 0) == DSERR_INVALIDPARAM, "invalid speakers");
    CHECK(VT(primary)->Stop(primary) == DS_OK && VT(primary)->GetStatus(primary, &status) == DS_OK && status == 0, "primary Stop");

    CHECK(VT(listener)->Release(listener) >= 1, "listener Release");
    CHECK(VT(primary)->Release(primary) == 0, "the primary is released");
    CHECK(VT(ds)->Release(ds) == 0, "the device is released");
}

static void test_primary_format_and_master_volume(void) {
    IDirectSound* ds = make_device();
    DSBUFFERDESC desc;
    IDirectSoundBuffer *primary = NULL, *b;
    WAVEFORMATEX wfx;
    float out[8];
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME;
    VT(ds)->CreateSoundBuffer(ds, &desc, &primary, NULL);
    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    CHECK(VT(primary)->SetFormat(primary, &wfx) == DSERR_PRIOLEVELNEEDED, "SetFormat needs the priority level");
    VT(ds)->SetCooperativeLevel(ds, NULL, DSSCL_PRIORITY);
    wfx.wBitsPerSample = 24;
    CHECK(VT(primary)->SetFormat(primary, &wfx) == DSERR_INVALIDPARAM, "24 bits are refused");
    wfx.wBitsPerSample = 16;
    CHECK(VT(primary)->SetFormat(primary, &wfx) == DS_OK && B.last_rate == 44100 && audio_mixer_rate(dsound_compat_mixer()) == 44100,
          "a new primary rate moves the output");
    wfx.nSamplesPerSec = 22050;
    CHECK(VT(primary)->SetFormat(primary, &wfx) == DS_OK && audio_mixer_rate(dsound_compat_mixer()) == 22050, "and back");

    b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL);
    fill16(b, 8, 16384);
    VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
    VT(primary)->SetVolume(primary, -602);
    mix(out, 2);
    CHECK(near_to(out[0], 0.25f, 0.002f), "the primary volume scales every buffer: -6.02 dB of 0.5 (%f)", out[0]);
    VT(primary)->SetVolume(primary, 0);
    mix(out, 2);
    CHECK(out[0] == 0.5f, "full level again");
    VT(b)->Release(b);
    VT(primary)->Release(primary);
    VT(ds)->Release(ds);
}

/* ---- secondary buffers ------------------------------------------------------------------------------------------ */

static void test_secondary_creation(void) {
    IDirectSound* ds = make_device();
    HRESULT hr;
    IDirectSoundBuffer* b;
    DSBUFFERDESC desc;
    b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 4410, &hr);
    CHECK(hr == DS_OK && b, "a mono 16-bit buffer");
    if (b) VT(b)->Release(b);
    static const struct {
        DWORD rate;
        WORD channels, bits;
    } k_formats[] = { { 22050, 1, 16 }, { 22050, 2, 16 }, { 11025, 1, 16 }, { 11025, 2, 16 }, { 22050, 1, 8 }, { 44100, 1, 16 } };
    for (size_t i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++) {
        b = make_secondary(ds, DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC, k_formats[i].rate, k_formats[i].channels, k_formats[i].bits, 4096, &hr);
        CHECK(hr == DS_OK && b, "game format %lu Hz %u ch %u bit", k_formats[i].rate, k_formats[i].channels, k_formats[i].bits);
        if (b) VT(b)->Release(b);
    }
    b = make_secondary(ds, 0, 22050, 1, 24, 4096, &hr);
    CHECK(hr == DSERR_BADFORMAT && !b, "24-bit audio is a bad format");
    b = make_secondary(ds, 0, 22050, 3, 16, 4096, &hr);
    CHECK(hr == DSERR_BADFORMAT && !b, "three channels");
    b = make_secondary(ds, 0, 50, 1, 16, 4096, &hr);
    CHECK(hr == DSERR_BADFORMAT && !b, "a rate below the minimum");
    b = make_secondary(ds, 0, 22050, 1, 16, 2, &hr);
    CHECK(hr == DSERR_INVALIDPARAM && !b, "a buffer under DSBSIZE_MIN");
    b = make_secondary(ds, 0, 22050, 2, 16, 4098, &hr);
    CHECK(hr == DSERR_INVALIDPARAM && !b, "a size that is not a multiple of the block");
    b = make_secondary(ds, DSBCAPS_CTRL3D, 22050, 2, 16, 4096, &hr);
    CHECK(hr == DSERR_INVALIDPARAM && !b, "3D buffers must be mono");
    b = make_secondary(ds, DSBCAPS_LOCHARDWARE | DSBCAPS_CTRL3D, 22050, 1, 16, 4096, &hr);
    CHECK(hr != DS_OK && !b, "hardware buffers are not available");
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwBufferBytes = 4096;
    CHECK(VT(ds)->CreateSoundBuffer(ds, &desc, &b, NULL) == DSERR_INVALIDPARAM, "a secondary needs a format");
    desc.dwSize = 7;
    CHECK(VT(ds)->CreateSoundBuffer(ds, &desc, &b, NULL) == DSERR_INVALIDPARAM, "a wrong descriptor size");
    CHECK(VT(ds)->CreateSoundBuffer(ds, NULL, &b, NULL) == DSERR_INVALIDPARAM, "no descriptor");
    VT(ds)->Release(ds);
}

static void test_playback_state_machine(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer* b = make_secondary(ds, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY, 22050, 1, 16, 8, NULL);
    DWORD status = 99, play = 99, write = 99;
    float out[16];
    fill16(b, 8, 16384);
    CHECK(VT(b)->GetStatus(b, &status) == DS_OK && status == 0, "a new buffer is stopped");
    CHECK(VT(b)->Play(b, 1, 0, 0) == DSERR_INVALIDPARAM, "reserved parameter");
    CHECK(VT(b)->Play(b, 0, 0, 0) == DS_OK && VT(b)->GetStatus(b, &status) == DS_OK && status == DSBSTATUS_PLAYING, "Play");
    mix(out, 2);
    CHECK(out[0] == 0.5f, "audible");
    mix(out, 8);
    CHECK(VT(b)->GetStatus(b, &status) == DS_OK && status == 0, "a one-shot is stopped once it ends");
    CHECK(VT(b)->GetCurrentPosition(b, &play, &write) == DS_OK && play == 0, "and rewound");
    CHECK(VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING) == DS_OK && VT(b)->GetStatus(b, &status) == DS_OK &&
              status == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING),
          "looping status");
    mix(out, 6);
    CHECK(VT(b)->GetStatus(b, &status) == DS_OK && (status & DSBSTATUS_LOOPING), "a loop keeps going");
    CHECK(VT(b)->Stop(b) == DS_OK && VT(b)->GetStatus(b, &status) == DS_OK && status == 0, "Stop");
    CHECK(VT(b)->GetCurrentPosition(b, &play, NULL) == DS_OK && play == 4, "Stop keeps the position (6 frames mod 4)");
    CHECK(VT(b)->Stop(b) == DS_OK, "Stop twice");
    CHECK(VT(b)->SetCurrentPosition(b, 0) == DS_OK && VT(b)->GetCurrentPosition(b, &play, &write) == DS_OK && play == 0 && write == 0, "SetCurrentPosition(0)");
    CHECK(VT(b)->SetCurrentPosition(b, 8) == DSERR_INVALIDPARAM, "a position past the end");
    CHECK(VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING) == DS_OK && VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING) == DS_OK, "Play on a playing buffer");
    mix(out, 1);
    CHECK(out[0] == 0.5f, "still one voice");
    CHECK(VT(b)->Restore(b) == DS_OK, "Restore");
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

static void test_volume_and_menu_click(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer *click = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL),
                       *plain = make_secondary(ds, 0, 22050, 1, 16, 8, NULL);
    LONG v = 7;
    float out[4];
    fill16(click, 8, 32000);
    VT(click)->Play(click, 0, 0, DSBPLAY_LOOPING);
    mix(out, 1);
    CHECK(near_to(out[0], 32000.0f / 32768.0f, 1e-6f), "0 dB leaves the sample alone");
    CHECK(VT(click)->SetVolume(click, -602) == DS_OK && VT(click)->GetVolume(click, &v) == DS_OK && v == -602, "the menu click's -6.02 dB");
    mix(out, 1);
    CHECK(near_to(out[0], 0.5f * 32000.0f / 32768.0f, 0.001f), "plays at half the amplitude (%f)", out[0]);
    VT(click)->SetVolume(click, -2000);
    mix(out, 1);
    CHECK(near_to(out[0], 0.1f * 32000.0f / 32768.0f, 0.001f), "-20 dB");
    VT(click)->SetVolume(click, DSBVOLUME_MIN);
    mix(out, 1);
    CHECK(out[0] == 0.0f, "DSBVOLUME_MIN is silent");
    CHECK(VT(click)->SetVolume(click, 1) == DSERR_INVALIDPARAM && VT(click)->SetVolume(click, -10001) == DSERR_INVALIDPARAM, "out of range");
    CHECK(VT(plain)->SetVolume(plain, -100) == DSERR_CONTROLUNAVAIL && VT(plain)->GetVolume(plain, &v) == DSERR_CONTROLUNAVAIL,
          "a buffer without DSBCAPS_CTRLVOLUME has no volume control");
    VT(click)->Release(click);
    VT(plain)->Release(plain);
    VT(ds)->Release(ds);
}

static void test_frequency(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer *b = make_secondary(ds, DSBCAPS_CTRLFREQUENCY, 11025, 1, 16, 8, NULL),
                       *nofreq = make_secondary(ds, 0, 11025, 1, 16, 8, NULL);
    const int16_t wave[4] = { 8192, 16384, 24576, -8192 };
    LPVOID p1, p2;
    DWORD n1, n2;
    float out[8];
    VT(b)->Lock(b, 0, 8, &p1, &n1, &p2, &n2, 0);
    memcpy(p1, wave, 8);
    VT(b)->Unlock(b, p1, n1, p2, n2);
    CHECK(VT(b)->SetFrequency(b, 44100) == DS_OK, "SetFrequency");
    VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
    mix(out, 2);
    CHECK(out[0] == 0.25f && out[2] == 0.75f, "plays at the new rate: every second frame at twice the output's rate");
    CHECK(VT(b)->SetFrequency(b, 50) == DSERR_INVALIDPARAM && VT(b)->SetFrequency(b, 400000) == DSERR_INVALIDPARAM, "out of range");
    CHECK(VT(b)->SetFrequency(b, DSBFREQUENCY_ORIGINAL) == DS_OK, "the original rate");
    CHECK(VT(nofreq)->SetFrequency(nofreq, 22050) == DSERR_CONTROLUNAVAIL, "no DSBCAPS_CTRLFREQUENCY, no control");
    VT(b)->Release(b);
    VT(nofreq)->Release(nofreq);
    VT(ds)->Release(ds);
}

static void test_lock_unlock(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer* b = make_secondary(ds, 0, 22050, 1, 16, 16, NULL);
    LPVOID p1 = (LPVOID)1, p2 = (LPVOID)1, base;
    DWORD n1 = 5, n2 = 5, play;
    CHECK(VT(b)->Lock(b, 0, 16, &p1, &n1, NULL, NULL, 0) == DS_OK && n1 == 16, "Lock with no second region");
    base = p1;
    VT(b)->Unlock(b, p1, n1, NULL, 0);
    CHECK(VT(b)->Lock(b, 12, 8, &p1, &n1, &p2, &n2, 0) == DS_OK && p1 == (BYTE*)base + 12 && n1 == 4 && p2 == base && n2 == 4,
          "a wrapping range gives two regions, as the streaming thread uses it");
    CHECK(VT(b)->Unlock(b, p1, n1, p2, n2) == DS_OK, "Unlock both");
    CHECK(VT(b)->Lock(b, 4, 4, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER) == DS_OK && p1 == base && n1 == 16 && p2 == NULL && n2 == 0,
          "ENTIREBUFFER ignores offset and length");
    VT(b)->Unlock(b, p1, n1, p2, n2);
    VT(b)->SetCurrentPosition(b, 4);
    CHECK(VT(b)->Lock(b, 0, 4, &p1, &n1, &p2, &n2, DSBLOCK_FROMWRITECURSOR) == DS_OK && p1 == (BYTE*)base + 4 && n1 == 4,
          "FROMWRITECURSOR of a stopped buffer starts at its position");
    VT(b)->GetCurrentPosition(b, &play, NULL);
    p1 = (LPVOID)1;
    n1 = 5;
    CHECK(VT(b)->Lock(b, 16, 4, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM && p1 == NULL && n1 == 0 && p2 == NULL && n2 == 0,
          "a bad Lock leaves the outputs empty, which is what the game tests");
    CHECK(VT(b)->Lock(b, 0, 17, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM, "too long");
    CHECK(VT(b)->Lock(b, 0, 4, NULL, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM, "no pointer");
    CHECK(VT(b)->Unlock(b, NULL, 0, NULL, 0) == DSERR_INVALIDPARAM, "Unlock of nothing");
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

static void test_streaming_like_smacker(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer* b = make_secondary(ds, 0x18000 | DSBCAPS_CTRLVOLUME, 22050, 2, 16, 45056, NULL);
    float* out = (float*)malloc(2 * 4096 * sizeof(float));
    DWORD play = 0, write = 0, filled = 0, delta;
    LPVOID p1, p2;
    DWORD n1, n2;
    CHECK(b != NULL, "Smacker's buffer: global focus, current position, volume, 22050 Hz stereo 16-bit");
    fill16(b, 45056, 0);
    CHECK(VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING) == DS_OK, "looping from the start");
    for (int round = 0; round < 40; round++) {
        mix(out, 2205);
        CHECK(VT(b)->GetCurrentPosition(b, &play, &write) == DS_OK, "GetCurrentPosition");
        delta = (play + 45056 - filled) % 45056;
        if (!delta) continue;
        CHECK(VT(b)->Lock(b, filled, delta, &p1, &n1, &p2, &n2, 0) == DS_OK && n1 + n2 == delta, "refill Lock");
        VT(b)->Unlock(b, p1, n1, p2, n2);
        filled = (filled + delta) % 45056;
    }
    CHECK(play == (40u * 2205 * 4) % 45056, "the play cursor follows the output through the wrap (%lu)", (unsigned long)play);
    CHECK(VT(b)->Stop(b) == DS_OK && VT(b)->SetCurrentPosition(b, 0) == DS_OK && VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING) == DS_OK,
          "Smacker's restart: Stop, SetCurrentPosition(0), Play");
    free(out);
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

static void test_duplicate(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer *a = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL), *d = NULL, *bad = NULL;
    DSBUFFERDESC desc;
    IDirectSoundBuffer* primary = NULL;
    LPVOID pa, pd, p2;
    DWORD na, nd, n2, status;
    float out[4];
    fill16(a, 8, 16384);
    CHECK(VT(ds)->DuplicateSoundBuffer(ds, a, &d) == DS_OK && d && d != a, "DuplicateSoundBuffer");
    VT(a)->Lock(a, 0, 8, &pa, &na, &p2, &n2, 0);
    VT(d)->Lock(d, 0, 8, &pd, &nd, &p2, &n2, 0);
    CHECK(pa == pd, "the duplicate shares the sample data");
    VT(a)->SetVolume(a, -10000);
    VT(d)->Play(d, 0, 0, 0);
    VT(a)->GetStatus(a, &status);
    CHECK(status == 0, "playback state is independent");
    mix(out, 1);
    CHECK(out[0] == 0.5f, "the duplicate has its own volume");
    VT(a)->Release(a);
    mix(out, 1);
    CHECK(out[0] == 0.5f, "and outlives the original");
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    VT(ds)->CreateSoundBuffer(ds, &desc, &primary, NULL);
    CHECK(VT(ds)->DuplicateSoundBuffer(ds, primary, &bad) == DSERR_INVALIDPARAM && !bad, "the primary cannot be duplicated");
    CHECK(VT(ds)->DuplicateSoundBuffer(ds, NULL, &bad) == DSERR_INVALIDPARAM, "no source");
    VT(primary)->Release(primary);
    VT(d)->Release(d);
    VT(ds)->Release(ds);
}

static void test_unsupported_methods(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer* b = make_secondary(ds, DSBCAPS_CTRLPAN, 22050, 1, 16, 8, NULL);
    DSBCAPS caps;
    LONG pan;
    CHECK(VT(b)->SetPan(b, 0) == DSERR_UNSUPPORTED && VT(b)->GetPan(b, &pan) == DSERR_UNSUPPORTED, "pan is not used by the game");
    caps.dwSize = sizeof caps;
    CHECK(VT(b)->GetCaps(b, &caps) == DSERR_UNSUPPORTED, "unused queries answer unsupported");
    CHECK(VT(b)->SetFormat(b, NULL) == DSERR_INVALIDCALL, "a secondary's format is fixed");
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

/* ---- 3D --------------------------------------------------------------------------------------------------------- */

static void test_3d(void) {
    IDirectSound* ds = make_device();
    DSBUFFERDESC desc;
    IDirectSoundBuffer *primary = NULL, *b = make_secondary(ds, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL),
                       *plain = make_secondary(ds, 0, 22050, 1, 16, 8, NULL);
    IDirectSound3DListener* listener = NULL;
    IDirectSound3DBuffer *s3d = NULL, *none = NULL;
    float out[4];
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof(DSBUFFERDESC1);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    VT(ds)->CreateSoundBuffer(ds, &desc, &primary, NULL);
    VT(primary)->QueryInterface(primary, &k_iid_listener, (void**)&listener);
    CHECK(VT(b)->QueryInterface(b, &k_iid_buffer_3d, (void**)&s3d) == DS_OK && s3d, "3D buffer interface");
    CHECK(VT(plain)->QueryInterface(plain, &k_iid_buffer_3d, (void**)&none) == E_NOINTERFACE && none == NULL, "none on a buffer without DSBCAPS_CTRL3D");
    CHECK(VT(b)->QueryInterface(b, &k_iid_listener, (void**)&none) == E_NOINTERFACE, "the listener belongs to the primary");
    CHECK(VT(b)->QueryInterface(b, &k_iid_notify, (void**)&none) == E_NOINTERFACE, "no notifications");
    CHECK(VT(b)->QueryInterface(b, &k_iid_buffer, (void**)&none) == DS_OK && none == (IDirectSound3DBuffer*)b, "its own interface");
    VT(b)->Release(b);
    fill16(b, 8, 16384);
    VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
    mix(out, 1);
    CHECK(out[0] == 0.5f && out[1] == 0.5f, "a 3D buffer at the listener plays at full level");

    CHECK(VT(s3d)->SetMinDistance(s3d, 2.0f, DS3D_IMMEDIATE) == DS_OK, "SetMinDistance");
    CHECK(VT(s3d)->SetPosition(s3d, 0, 0, 8.0f, DS3D_IMMEDIATE) == DS_OK, "SetPosition");
    mix(out, 1);
    CHECK(near_to(out[0], 0.125f, 1e-4f) && near_to(out[1], 0.125f, 1e-4f), "four times the minimum distance: a quarter (%f)", out[0]);
    CHECK(VT(s3d)->SetPosition(s3d, 8.0f, 0, 0, DS3D_DEFERRED) == DS_OK, "deferred SetPosition");
    mix(out, 1);
    CHECK(near_to(out[0], 0.125f, 1e-4f), "a deferred setting changes nothing until it is committed");
    CHECK(VT(listener)->CommitDeferredSettings(listener) == DS_OK, "commit");
    mix(out, 1);
    CHECK(out[0] < 1e-3f && near_to(out[1], 0.125f, 1e-3f), "committed: the source is on the right, the left channel falls silent (%f, %f)", out[0], out[1]);
    CHECK(VT(listener)->SetPosition(listener, 8.0f, 0, 0, DS3D_IMMEDIATE) == DS_OK, "an immediate listener move");
    mix(out, 1);
    CHECK(near_to(out[0], out[1], 1e-6f) && near_to(out[0], 0.5f, 1e-4f), "the listener at the source is centred and at full level");
    CHECK(VT(listener)->SetOrientation(listener, 1, 0, 0, 0, 1, 0, DS3D_IMMEDIATE) == DS_OK, "SetOrientation");

    CHECK(VT(s3d)->SetConeAngles(s3d, 60, 120, DS3D_IMMEDIATE) == DS_OK && VT(s3d)->SetConeOutsideVolume(s3d, -2000, DS3D_IMMEDIATE) == DS_OK &&
              VT(s3d)->SetConeOrientation(s3d, 0, 0, 1, DS3D_IMMEDIATE) == DS_OK,
          "cone setters");
    CHECK(VT(s3d)->SetConeAngles(s3d, 361, 120, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "cone angles over 360 are refused");
    CHECK(VT(s3d)->SetConeOutsideVolume(s3d, 5, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "an outside volume above 0");
    CHECK(VT(s3d)->SetMinDistance(s3d, -1.0f, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "a negative distance");
    CHECK(VT(s3d)->SetMinDistance(s3d, 1.0f, 5) == DSERR_INVALIDPARAM, "an unknown apply flag");

    {
        IDirectSound3DBuffer* extra = NULL;
        CHECK(VT(s3d)->QueryInterface(s3d, &k_iid_buffer_3d, (void**)&extra) == DS_OK && extra == s3d, "QueryInterface from the 3D face");
        VT(extra)->Release(extra);
    }
    VT(s3d)->Release(s3d);
    VT(listener)->Release(listener);
    VT(primary)->Release(primary);
    VT(plain)->Release(plain);
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

/* ---- threads, lifetime, shutdown -------------------------------------------------------------------------------- */

static DWORD WINAPI hammer(LPVOID context) {
    IDirectSoundBuffer* b = (IDirectSoundBuffer*)context;
    LPVOID p1, p2;
    DWORD n1, n2, play, write, status;
    for (int i = 0; i < 3000; i++) {
        VT(b)->GetCurrentPosition(b, &play, &write);
        if (VT(b)->Lock(b, play, 64, &p1, &n1, &p2, &n2, 0) == DS_OK) VT(b)->Unlock(b, p1, n1, p2, n2);
        VT(b)->GetStatus(b, &status);
        if (i % 500 == 0) {
            VT(b)->Stop(b);
            VT(b)->SetCurrentPosition(b, 0);
            VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
        }
    }
    return 0;
}

static void test_threads(void) {
    IDirectSound* ds = make_device();
    IDirectSoundBuffer* b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 2, 16, 45056, NULL);
    HANDLE thread;
    float* out = (float*)malloc(2 * 512 * sizeof(float));
    VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
    thread = CreateThread(NULL, 0, hammer, b, 0, NULL);
    CHECK(thread != NULL, "a second thread");
    for (int i = 0; i < 400; i++) {
        mix(out, 512);
        VT(b)->SetVolume(b, -(i % 100));
    }
    if (thread) {
        CHECK(WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0, "the streaming thread finishes: no deadlock");
        CloseHandle(thread);
    }
    free(out);
    VT(b)->Release(b);
    VT(ds)->Release(ds);
}

static void test_lifetime_and_shutdown(void) {
    dsound_compat_counts_t before, after;
    IDirectSound* ds;
    IDirectSoundBuffer *a, *b, *late = NULL;
    IDirectSound3DBuffer* s3d = NULL;
    DWORD status = 0, play;
    LPVOID p1;
    DWORD n1;
    dsound_compat_counts(&before);
    CHECK(before.buffers_live == 0, "every buffer of the earlier tests was released (%u alive)", before.buffers_live);
    for (int level = 0; level < 5; level++) {
        ds = make_device();
        for (int i = 0; i < 20; i++) {
            IDirectSoundBuffer* s = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 4096, NULL);
            VT(s)->Play(s, 0, 0, DSBPLAY_LOOPING);
            VT(s)->Release(s);
        }
        VT(ds)->Release(ds);
    }
    dsound_compat_counts(&after);
    CHECK(after.buffers_live == 0 && after.buffers_created == before.buffers_created + 100 && audio_mixer_voice_count(dsound_compat_mixer()) == 0,
          "100 buffers over five levels leave nothing behind");
    ds = make_device();
    a = make_secondary(ds, DSBCAPS_CTRL3D, 22050, 1, 16, 4096, NULL);
    b = make_secondary(ds, 0, 22050, 1, 16, 4096, NULL);
    VT(a)->QueryInterface(a, &k_iid_buffer_3d, (void**)&s3d);
    VT(b)->Play(b, 0, 0, DSBPLAY_LOOPING);
    VT(ds)->Release(ds);
    CHECK(VT(b)->GetStatus(b, &status) == DS_OK && status != 0, "buffers keep playing after their device is released");
    dsound_compat_shutdown();
    CHECK(B.closes == 1 && dsound_compat_mixer() == NULL, "the shutdown closes the output once, before anything is freed");
    CHECK(VT(b)->GetStatus(b, &status) == DS_OK && status == DSBSTATUS_BUFFERLOST, "later calls report the buffer lost");
    CHECK(VT(b)->Play(b, 0, 0, 0) == DSERR_BUFFERLOST && VT(b)->Stop(b) == DSERR_BUFFERLOST && VT(b)->GetCurrentPosition(b, &play, NULL) == DSERR_BUFFERLOST &&
              VT(b)->Lock(b, 0, 4, &p1, &n1, NULL, NULL, 0) == DSERR_BUFFERLOST && VT(b)->SetCurrentPosition(b, 0) == DSERR_BUFFERLOST,
          "and fail without touching freed memory");
    CHECK(VT(s3d)->SetPosition(s3d, 1, 2, 3, DS3D_IMMEDIATE) == DS_OK, "3D setters stay safe");
    {
        IDirectSound* again = NULL;
        CHECK(compat_DirectSoundCreate(NULL, &again, NULL) == DSERR_NODRIVER && B.opens == 1, "no output is opened once the host is shutting down");
    }
    CHECK(VT(a)->Release(a) == 1 && VT(s3d)->Release(s3d) == 0 && VT(b)->Release(b) == 0, "releasing after the shutdown is safe");
    dsound_compat_shutdown();
    CHECK(B.closes == 1, "a second shutdown does nothing");
}

/* ---- imports ---------------------------------------------------------------------------------------------------- */

static DWORD WINAPI replacement_pid(void) { return 4321; }

static void test_import_gate(void) {
    HMODULE host = dsound_host_module(), kernel = GetModuleHandleA("kernel32.dll");
    FARPROC original = GetProcAddress(kernel, "GetCurrentProcessId");
    CHECK(host != NULL && (uintptr_t)host >= 0x20, "the stand-in module handle passes Smacker's 'handle < 0x20' test");
    CHECK(dsound_load_library_a("DSOUND.DLL") == host && dsound_load_library_a("dsound.dll") == host && dsound_load_library_a("C:\\x\\DSound.dll") == host,
          "every request for DSound.dll gets the host's");
    CHECK(dsound_get_proc_address(host, "DirectSoundCreate") == (FARPROC)compat_DirectSoundCreate, "DirectSoundCreate is the host's");
    CHECK(dsound_get_proc_address(host, "DirectSoundCreate8") == NULL && dsound_get_proc_address(host, "DirectSoundEnumerateA") == NULL &&
              dsound_get_proc_address(host, (LPCSTR)(uintptr_t)1) == NULL,
          "nothing else is answered, ordinals included");
    CHECK(dsound_free_library(host) == TRUE, "FreeLibrary of the stand-in");
    CHECK(dsound_get_proc_address(kernel, "GetCurrentProcessId") == original && original != NULL, "other modules pass through");
    {
        HMODULE other = dsound_load_library_a("kernel32.dll");
        CHECK(other == kernel, "other libraries load normally");
    }
    CHECK(GetModuleHandleA("dsound.dll") == NULL, "the system DSound.dll was never loaded by any of this");

    CHECK(dsound_patch_import(GetModuleHandleA(NULL), "kernel32.dll", "NoSuchImport", (void*)replacement_pid) == 0, "an import that is not there is not patched");
    CHECK(dsound_patch_import(GetModuleHandleA(NULL), "nosuch.dll", "GetCurrentProcessId", (void*)replacement_pid) == 0, "nor is another library's");
    CHECK(dsound_patch_import(NULL, "kernel32.dll", "GetCurrentProcessId", (void*)replacement_pid) == 0, "no module");
    if (dsound_patch_import(GetModuleHandleA(NULL), "kernel32.dll", "GetCurrentProcessId", (void*)replacement_pid)) {
        volatile DWORD seen = GetCurrentProcessId();
        CHECK(seen == 4321, "a patched import is what the module calls");
        CHECK(dsound_patch_import(GetModuleHandleA(NULL), "KERNEL32.DLL", "GetCurrentProcessId", (void*)original) == 1 && GetCurrentProcessId() != 4321,
              "and it is restored");
    } else {
        printf("dsound_compat_tests: GetCurrentProcessId is not imported by this program, the live patch check is skipped\n");
    }
}

int main(void) {
    test_abi_offsets();
    test_create();
    test_game_initialisation();
    test_primary_format_and_master_volume();
    test_secondary_creation();
    test_playback_state_machine();
    test_volume_and_menu_click();
    test_frequency();
    test_lock_unlock();
    test_streaming_like_smacker();
    test_duplicate();
    test_unsupported_methods();
    test_3d();
    test_threads();
    test_import_gate();
    test_lifetime_and_shutdown();
    printf("dsound_compat_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
