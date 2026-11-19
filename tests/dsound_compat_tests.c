/* The portable DirectSound layer as the guest and SMACKW32 use it: methods called through guest vtables at the slots the
 * retail code calls, over a mixer with no audio device, in guest memory that is NOT the host's (a guest address is an
 * offset into an arena, never a host pointer). No Windows header: the same source runs on Windows and Linux. Nothing is
 * played. */
#include "dsound_guest_harness.h"


/* ---- ABI -------------------------------------------------------------------------------------------------------- */

static void test_abi_layout(void) {
    CHECK(sizeof(ds_caps_t) == 0x60 && offsetof(ds_caps_t, max_hw_3d_all) == 0x2C, "DSCAPS as the guest reads it");
    CHECK(sizeof(ds_guid_t) == 16 && sizeof(ds_waveformatex_t) == 18 && sizeof(ds_bufferdesc_t) == 36, "GUID, WAVEFORMATEX, DSBUFFERDESC");
    CHECK(offsetof(ds_waveformatex_t, samples_per_second) == 4 && offsetof(ds_waveformatex_t, block_align) == 12 &&
              offsetof(ds_waveformatex_t, bits_per_sample) == 14,
          "WAVEFORMATEX offsets");
    CHECK(offsetof(ds_bufferdesc_t, buffer_bytes) == 8 && offsetof(ds_bufferdesc_t, format_address) == 16, "DSBUFFERDESC offsets");
    CHECK(sizeof(uint32_t) == 4 && DSOUND_DEVICE_SLOTS == 11 && DSOUND_BUFFER_SLOTS == 21 && DSOUND_BUFFER3D_SLOTS == 21 &&
              DSOUND_LISTENER_SLOTS == 18,
          "vtable sizes: 11, 21, 21 and 18 slots of 4 bytes");
    CHECK(dsound_method_slot(DSM_DeviceCreateSoundBuffer) * 4 == 0x0C && dsound_method_slot(DSM_DeviceGetCaps) * 4 == 0x10 &&
              dsound_method_slot(DSM_DeviceDuplicateSoundBuffer) * 4 == 0x14 && dsound_method_slot(DSM_DeviceSetCooperativeLevel) * 4 == 0x18 &&
              dsound_method_slot(DSM_DeviceGetSpeakerConfig) * 4 == 0x20 && dsound_method_slot(DSM_DeviceSetSpeakerConfig) * 4 == 0x24,
          "IDirectSound slots the game calls");
    CHECK(dsound_method_slot(DSM_BufferGetCurrentPosition) * 4 == 0x10 && dsound_method_slot(DSM_BufferGetVolume) * 4 == 0x18 &&
              dsound_method_slot(DSM_BufferGetStatus) * 4 == 0x24 && dsound_method_slot(DSM_BufferLock) * 4 == 0x2C &&
              dsound_method_slot(DSM_BufferPlay) * 4 == 0x30 && dsound_method_slot(DSM_BufferSetCurrentPosition) * 4 == 0x34 &&
              dsound_method_slot(DSM_BufferSetFormat) * 4 == 0x38 && dsound_method_slot(DSM_BufferSetVolume) * 4 == 0x3C &&
              dsound_method_slot(DSM_BufferSetFrequency) * 4 == 0x44 && dsound_method_slot(DSM_BufferStop) * 4 == 0x48 &&
              dsound_method_slot(DSM_BufferUnlock) * 4 == 0x4C && dsound_method_slot(DSM_BufferRestore) * 4 == 0x50,
          "IDirectSoundBuffer slots the game and Smacker call");
    CHECK(dsound_method_slot(DSM_Buffer3DSetConeAngles) * 4 == 0x34 && dsound_method_slot(DSM_Buffer3DSetConeOrientation) * 4 == 0x38 &&
              dsound_method_slot(DSM_Buffer3DSetConeOutsideVolume) * 4 == 0x3C && dsound_method_slot(DSM_Buffer3DSetMinDistance) * 4 == 0x44 &&
              dsound_method_slot(DSM_Buffer3DSetPosition) * 4 == 0x4C,
          "IDirectSound3DBuffer slots the game calls");
    CHECK(dsound_method_slot(DSM_ListenerSetOrientation) * 4 == 0x34 && dsound_method_slot(DSM_ListenerSetPosition) * 4 == 0x38 &&
              dsound_method_slot(DSM_ListenerCommitDeferredSettings) * 4 == 0x44,
          "IDirectSound3DListener slots the game calls");
    CHECK(dsound_method_words(DSM_BufferLock) == 8 && dsound_method_words(DSM_Buffer3DSetPosition) == 5 &&
              dsound_method_words(DSM_ListenerSetOrientation) == 8 && dsound_method_words(DSM_DeviceCreateSoundBuffer) == 4 &&
              dsound_method_words(DSM_Create) == 3,
          "stack words of the calls with the most arguments, this included");
    CHECK(dsound_method_interface(DSM_BufferPlay) == DSI_BUFFER && dsound_method_interface(DSM_Buffer3DSetPosition) == DSI_BUFFER3D &&
              dsound_method_interface(DSM_ListenerCommitDeferredSettings) == DSI_LISTENER && dsound_method_interface(DSM_DeviceRelease) == DSI_DEVICE,
          "each method belongs to its interface");
    for (int m = 0; m < DSM_COUNT; m++)
        CHECK(dsound_method_words((dsound_method_t)m) >= 1 && dsound_method_name((dsound_method_t)m)[0], "method %d has a name and a size", m);
}

static void test_install(void) {
    dsound_binding_t incomplete = binding();
    CHECK(dsound_compat_install(&k_backend, &incomplete) == 1, "a complete binding installs");
    CHECK(dsound_compat_vtables_complete(), "no method lacks a handler and every vtable was built");
    CHECK(dsound_compat_vtable(DSI_BUFFER) >= GUEST_BASE && dsound_compat_vtable(DSI_DEVICE) >= GUEST_BASE &&
              dsound_compat_vtable(DSI_BUFFER3D) >= GUEST_BASE && dsound_compat_vtable(DSI_LISTENER) >= GUEST_BASE,
          "the vtables are in guest memory");
    {
        uint32_t table = dsound_compat_vtable(DSI_BUFFER);
        int all = 1;
        for (unsigned i = 0; i < DSOUND_BUFFER_SLOTS; i++) all = all && get32(table + i * 4u) == thunk_address(NULL, (dsound_method_t)(DSM_BufferQueryInterface + i));
        CHECK(all, "every slot holds the entry the binding gave for that method");
    }
    incomplete.method_address = NULL;
    CHECK(dsound_compat_install(&k_backend, &incomplete) == 0, "a binding without entry points is refused");
    incomplete = binding();
    incomplete.guest.resolve = NULL;
    CHECK(dsound_compat_install(&k_backend, &incomplete) == 0, "a binding without guest memory is refused");
    CHECK(dsound_compat_install(NULL, &incomplete) == 0, "no backend");
    {
        dsound_binding_t again = binding();
        CHECK(dsound_compat_install(&k_backend, &again) == 1, "installing again before any output exists is fine");
    }
}

static void test_create(void) {
    uint32_t ds, again, out = scratch(4), q = scratch(4);
    ds_guid_t other = { 1, 2, 3, { 4, 5, 6, 7, 8, 9, 10, 11 } };
    B.fail_open = 1;
    CHECK(call_method(DSM_Create, 0, out, 0) == DSERR_NODRIVER && get32(out) == 0, "a device that cannot be opened is no driver");
    B.fail_open = 0;
    CHECK(call_method(DSM_Create, 0, 0, 0) == DSERR_INVALIDPARAM, "no output pointer");
    CHECK(call_method(DSM_Create, 0, out, 0x1234) == DS_CLASS_E_NOAGGREGATION, "no aggregation");
    CHECK(call_method(DSM_Create, guid_in_guest(&other), out, 0) == DSERR_NODRIVER, "another device GUID");
    ds = make_device();
    CHECK(ds >= GUEST_BASE && ds < GUEST_BASE + ARENA_BYTES, "the device is a guest address (%08X)", ds);
    CHECK(B.opens == 1 && B.last_rate == 22050, "the first create opens the output at the primary default of 22050 Hz");
    again = make_device();
    CHECK(again && again != ds && B.opens == 1, "a second device shares the output");
    CHECK(vcall(ds, DEV_ADDREF) == 2 && vcall(ds, DEV_RELEASE) == 1, "AddRef and Release");
    CHECK(vcall(ds, DEV_QI, guid_in_guest(&k_iid_unknown), q) == DS_OK && get32(q) == ds && vcall(ds, DEV_RELEASE) == 1, "QueryInterface for IUnknown");
    CHECK(vcall(ds, DEV_QI, guid_in_guest(&k_iid_ks_property_set), q) == DS_E_NOINTERFACE && get32(q) == 0, "no other interface");
    CHECK(vcall(ds, DEV_QI, guid_in_guest(&k_iid_unknown), 0u) == DS_E_POINTER, "no output pointer for QueryInterface");
    CHECK(vcall(again, DEV_RELEASE) == 0, "the second device is released");
    CHECK(vcall(ds, DEV_RELEASE) == 0, "the first device is released");
    CHECK(B.closes == 0, "the output outlives its devices: a menu or level never reopens it");
}

static void test_game_initialisation(void) {
    uint32_t ds = make_device(), caps = scratch(sizeof(ds_caps_t)), primary, dummy, listener = scratch(4), speaker = scratch(4);
    uint32_t out = scratch(4), hr, desc = scratch(sizeof(ds_bufferdesc_t)), stereo = scratch(sizeof(ds_waveformatex_t));
    ds_caps_t c;
    ds_bufferdesc_t d;
    ds_waveformatex_t f;
    memset(&c, 0, sizeof c);
    c.size = 0x20;
    memcpy(arena_resolve(NULL, caps, sizeof c), &c, sizeof c);
    CHECK(vcall(ds, DEV_GET_CAPS, caps) == DSERR_INVALIDPARAM, "a wrong DSCAPS size is refused");
    c.size = sizeof c;
    c.max_hw_3d_all = 9;
    memcpy(arena_resolve(NULL, caps, sizeof c), &c, sizeof c);
    CHECK(vcall(ds, DEV_GET_CAPS, caps) == DS_OK, "GetCaps");
    memcpy(&c, arena_resolve(NULL, caps, sizeof c), sizeof c);
    CHECK(c.max_hw_3d_all == 0 && c.max_secondary_rate >= 44100 && c.min_secondary_rate <= 11025 && (c.flags & DSCAPS_SECONDARY16BIT),
          "no hardware 3D voices: the game takes its software path");
    CHECK(vcall(ds, DEV_SET_COOP, 0u, DSSCL_PRIORITY) == DS_OK, "SetCooperativeLevel(priority)");
    CHECK(vcall(ds, DEV_SET_COOP, 0u, 99u) == DSERR_INVALIDPARAM, "an unknown level");
    primary = make_primary(ds, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, &hr);
    CHECK(hr == DS_OK && primary, "the primary buffer (flags 0x91)");
    memset(&d, 0, sizeof d);
    d.size = DS_BUFFERDESC_V1_SIZE;
    d.flags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    d.format_address = stereo;
    memcpy(arena_resolve(NULL, desc, sizeof d), &d, sizeof d);
    CHECK(vcall(ds, DEV_CREATE_BUFFER, desc, out, 0u) == DSERR_INVALIDPARAM && get32(out) == 0, "a primary with a format is refused");
    CHECK(make_primary(ds, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, &hr) == primary && hr == DS_OK, "the primary is one object");
    vcall(primary, BUF_RELEASE);
    memset(&f, 0, sizeof f);
    f.format_tag = DS_WAVE_FORMAT_PCM;
    f.channels = 2;
    f.samples_per_second = 22050;
    f.bits_per_sample = 16;
    f.block_align = 4;
    f.bytes_per_second = 88200;
    memcpy(arena_resolve(NULL, stereo, sizeof f), &f, sizeof f);
    CHECK(vcall(primary, BUF_SET_FORMAT, stereo) == DS_OK && B.rate_changes == 0, "SetFormat(22050, 2, 16) keeps the output rate");
    CHECK(vcall(primary, BUF_QI, guid_in_guest(&k_iid_listener), listener) == DS_OK && get32(listener), "the 3D listener of the primary");
    CHECK(vcall(primary, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK, "Play(looping)");
    CHECK(vcall(primary, BUF_PLAY, 0u, 0u, 0u) == DSERR_INVALIDPARAM, "the primary only plays looping");
    CHECK(status_of(primary) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "primary status");
    CHECK(vcall(primary, BUF_SET_VOLUME, (uint32_t)-5000) == DS_OK && vcall(primary, BUF_GET_VOLUME, out) == DS_OK && (int32_t)get32(out) == -5000,
          "the master volume");
    CHECK(vcall(primary, BUF_SET_VOLUME, 0u) == DS_OK, "reset");
    CHECK(vcall(get32(listener), LST_SET_POSITION, float_bits(1.0f), float_bits(2.0f), float_bits(3.0f), DS3D_DEFERRED) == DS_OK, "listener SetPosition, deferred");
    CHECK(vcall(get32(listener), LST_SET_ORIENTATION, float_bits(0), float_bits(0), float_bits(1), float_bits(0), float_bits(1), float_bits(0), DS3D_DEFERRED) == DS_OK,
          "listener SetOrientation, deferred");
    CHECK(vcall(get32(listener), LST_COMMIT) == DS_OK, "CommitDeferredSettings");
    CHECK(vcall(get32(listener), LST_SET_POSITION, 0u, 0u, 0u, 7u) == DSERR_INVALIDPARAM, "an unknown apply flag");

    dummy = make_secondary(ds, DSBCAPS_STATIC | DSBCAPS_CTRL3D, 22050, 2, 16, 0x80, &hr);
    CHECK(hr == DSERR_INVALIDPARAM && dummy == 0, "the EAX probe fails as it does on Windows, so the game leaves EAX off");

    CHECK(vcall(ds, DEV_GET_SPEAKERS, speaker) == DS_OK && get32(speaker) == DSSPEAKER_STEREO, "speaker configuration");
    CHECK(vcall(ds, DEV_SET_SPEAKERS, 2u) == DS_OK && vcall(ds, DEV_GET_SPEAKERS, speaker) == DS_OK && get32(speaker) == 2, "set speakers");
    CHECK(vcall(ds, DEV_SET_SPEAKERS, 0u) == DSERR_INVALIDPARAM, "invalid speakers");
    CHECK(vcall(primary, BUF_STOP) == DS_OK && status_of(primary) == 0, "primary Stop");
    CHECK(vcall(get32(listener), BUF_RELEASE) >= 1, "listener Release");
    CHECK(vcall(primary, BUF_RELEASE) == 0, "the primary is released");
    CHECK(vcall(ds, DEV_RELEASE) == 0, "the device is released");
}

static void test_primary_format_and_master_volume(void) {
    uint32_t ds = make_device(), primary = make_primary(ds, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME, NULL), b;
    uint32_t wfx = scratch(sizeof(ds_waveformatex_t));
    ds_waveformatex_t f;
    float out[8];
    memset(&f, 0, sizeof f);
    f.format_tag = DS_WAVE_FORMAT_PCM;
    f.channels = 2;
    f.samples_per_second = 44100;
    f.bits_per_sample = 16;
    memcpy(arena_resolve(NULL, wfx, sizeof f), &f, sizeof f);
    CHECK(vcall(primary, BUF_SET_FORMAT, wfx) == DSERR_PRIOLEVELNEEDED, "SetFormat needs the priority level");
    vcall(ds, DEV_SET_COOP, 0u, DSSCL_PRIORITY);
    f.bits_per_sample = 24;
    memcpy(arena_resolve(NULL, wfx, sizeof f), &f, sizeof f);
    CHECK(vcall(primary, BUF_SET_FORMAT, wfx) == DSERR_INVALIDPARAM, "24 bits are refused");
    f.bits_per_sample = 16;
    memcpy(arena_resolve(NULL, wfx, sizeof f), &f, sizeof f);
    CHECK(vcall(primary, BUF_SET_FORMAT, wfx) == DS_OK && B.last_rate == 44100 && audio_mixer_rate(dsound_compat_mixer()) == 44100,
          "a new primary rate moves the output");
    f.samples_per_second = 22050;
    memcpy(arena_resolve(NULL, wfx, sizeof f), &f, sizeof f);
    CHECK(vcall(primary, BUF_SET_FORMAT, wfx) == DS_OK && audio_mixer_rate(dsound_compat_mixer()) == 22050, "and back");

    b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL);
    fill16(b, 8, 16384);
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    vcall(primary, BUF_SET_VOLUME, (uint32_t)-602);
    mix(out, 2);
    CHECK(near_to(out[0], 0.25f, 0.002f), "the primary volume scales every buffer: -6.02 dB of 0.5 (%f)", out[0]);
    vcall(primary, BUF_SET_VOLUME, 0u);
    mix(out, 2);
    CHECK(out[0] == 0.5f, "full level again");
    vcall(b, BUF_RELEASE);
    vcall(primary, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

/* ---- secondary buffers ------------------------------------------------------------------------------------------ */

static void test_secondary_creation(void) {
    uint32_t ds = make_device(), hr, b, desc = scratch(sizeof(ds_bufferdesc_t)), out = scratch(4);
    ds_bufferdesc_t d;
    static const struct {
        uint32_t rate;
        uint16_t channels, bits;
    } k_formats[] = { { 22050, 1, 16 }, { 22050, 2, 16 }, { 11025, 1, 16 }, { 11025, 2, 16 }, { 22050, 1, 8 }, { 44100, 1, 16 } };
    b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 4410, &hr);
    CHECK(hr == DS_OK && b, "a mono 16-bit buffer");
    if (b) vcall(b, BUF_RELEASE);
    for (size_t i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++) {
        b = make_secondary(ds, DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC, k_formats[i].rate, k_formats[i].channels, k_formats[i].bits, 4096, &hr);
        CHECK(hr == DS_OK && b, "game format %u Hz %u ch %u bit", k_formats[i].rate, k_formats[i].channels, k_formats[i].bits);
        if (b) vcall(b, BUF_RELEASE);
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
    memset(&d, 0, sizeof d);
    d.size = DS_BUFFERDESC_V1_SIZE;
    d.buffer_bytes = 4096;
    memcpy(arena_resolve(NULL, desc, sizeof d), &d, sizeof d);
    CHECK(vcall(ds, DEV_CREATE_BUFFER, desc, out, 0u) == DSERR_INVALIDPARAM, "a secondary needs a format");
    d.size = 7;
    memcpy(arena_resolve(NULL, desc, sizeof d), &d, sizeof d);
    CHECK(vcall(ds, DEV_CREATE_BUFFER, desc, out, 0u) == DSERR_INVALIDPARAM, "a wrong descriptor size");
    CHECK(vcall(ds, DEV_CREATE_BUFFER, 0u, out, 0u) == DSERR_INVALIDPARAM, "no descriptor");
    CHECK(vcall(ds, DEV_CREATE_BUFFER, 0xFFFFFFF0u, out, 0u) == DSERR_INVALIDPARAM, "a descriptor outside guest memory");
    G.fail_alloc = 1;
    b = make_secondary(ds, 0, 22050, 1, 16, 4096, &hr);
    G.fail_alloc = 0;
    CHECK(hr == DSERR_OUTOFMEMORY && !b, "no guest memory for a buffer: out of memory, nothing left behind");
    vcall(ds, DEV_RELEASE);
}

static void test_playback_state_machine(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY, 22050, 1, 16, 8, NULL);
    float out[16];
    fill16(b, 8, 16384);
    CHECK(status_of(b) == 0, "a new buffer is stopped");
    CHECK(vcall(b, BUF_PLAY, 1u, 0u, 0u) == DSERR_INVALIDPARAM, "reserved parameter");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, 0u) == DS_OK && status_of(b) == DSBSTATUS_PLAYING, "Play");
    mix(out, 2);
    CHECK(out[0] == 0.5f, "audible");
    mix(out, 8);
    CHECK(status_of(b) == 0, "a one-shot is stopped once it ends");
    CHECK(position_of(b, NULL) == 0, "and rewound");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK && status_of(b) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "looping status");
    mix(out, 6);
    CHECK((status_of(b) & DSBSTATUS_LOOPING) != 0, "a loop keeps going");
    CHECK(vcall(b, BUF_STOP) == DS_OK && status_of(b) == 0, "Stop");
    CHECK(position_of(b, NULL) == 4, "Stop keeps the position (6 frames mod 4)");
    CHECK(vcall(b, BUF_STOP) == DS_OK, "Stop twice");
    {
        uint32_t write = 99;
        CHECK(vcall(b, BUF_SET_POSITION, 0u) == DS_OK && position_of(b, &write) == 0 && write == 0, "SetCurrentPosition(0)");
    }
    CHECK(vcall(b, BUF_SET_POSITION, 8u) == DSERR_INVALIDPARAM, "a position past the end");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK && vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK, "Play on a playing buffer");
    mix(out, 1);
    CHECK(out[0] == 0.5f, "still one voice");
    CHECK(vcall(b, BUF_RESTORE) == DS_OK, "Restore");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_volume_and_menu_click(void) {
    uint32_t ds = make_device(), click = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL),
             plain = make_secondary(ds, 0, 22050, 1, 16, 8, NULL), at = scratch(4);
    float out[4];
    fill16(click, 8, 32000);
    vcall(click, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    mix(out, 1);
    CHECK(near_to(out[0], 32000.0f / 32768.0f, 1e-6f), "0 dB leaves the sample alone");
    CHECK(vcall(click, BUF_SET_VOLUME, (uint32_t)-602) == DS_OK && vcall(click, BUF_GET_VOLUME, at) == DS_OK && (int32_t)get32(at) == -602,
          "the menu click's -6.02 dB");
    mix(out, 1);
    CHECK(near_to(out[0], 0.5f * 32000.0f / 32768.0f, 0.001f), "plays at half the amplitude (%f)", out[0]);
    vcall(click, BUF_SET_VOLUME, (uint32_t)-2000);
    mix(out, 1);
    CHECK(near_to(out[0], 0.1f * 32000.0f / 32768.0f, 0.001f), "-20 dB");
    vcall(click, BUF_SET_VOLUME, (uint32_t)-10000);
    mix(out, 1);
    CHECK(out[0] == 0.0f, "DSBVOLUME_MIN is silent");
    CHECK(vcall(click, BUF_SET_VOLUME, 1u) == DSERR_INVALIDPARAM && vcall(click, BUF_SET_VOLUME, (uint32_t)-10001) == DSERR_INVALIDPARAM, "out of range");
    CHECK(vcall(plain, BUF_SET_VOLUME, (uint32_t)-100) == DSERR_CONTROLUNAVAIL && vcall(plain, BUF_GET_VOLUME, at) == DSERR_CONTROLUNAVAIL,
          "a buffer without DSBCAPS_CTRLVOLUME has no volume control");
    vcall(click, BUF_RELEASE);
    vcall(plain, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_frequency(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRLFREQUENCY, 11025, 1, 16, 8, NULL),
             nofreq = make_secondary(ds, 0, 11025, 1, 16, 8, NULL);
    const int16_t wave[4] = { 8192, 16384, 24576, -8192 };
    uint32_t p1, n1, p2, n2;
    float out[8];
    lock_buffer(b, 0, 8, 0, &p1, &n1, &p2, &n2);
    memcpy(arena_resolve(NULL, p1, 8), wave, 8);
    vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
    CHECK(vcall(b, BUF_SET_FREQUENCY, 44100u) == DS_OK, "SetFrequency");
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    mix(out, 2);
    CHECK(out[0] == 0.25f && out[2] == 0.75f, "plays at the new rate: every second frame at twice the output rate");
    CHECK(vcall(b, BUF_SET_FREQUENCY, 50u) == DSERR_INVALIDPARAM && vcall(b, BUF_SET_FREQUENCY, 400000u) == DSERR_INVALIDPARAM, "out of range");
    CHECK(vcall(b, BUF_SET_FREQUENCY, DSBFREQUENCY_ORIGINAL) == DS_OK, "the original rate");
    CHECK(vcall(nofreq, BUF_SET_FREQUENCY, 22050u) == DSERR_CONTROLUNAVAIL, "no DSBCAPS_CTRLFREQUENCY, no control");
    vcall(b, BUF_RELEASE);
    vcall(nofreq, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_lock_unlock(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, 0, 22050, 1, 16, 16, NULL), p1, n1, p2, n2, base;
    CHECK(lock_buffer(b, 0, 16, 0, &p1, &n1, &p2, &n2) == DS_OK && n1 == 16 && p2 == 0 && n2 == 0, "Lock with no second region");
    base = p1;
    CHECK(base >= GUEST_BASE && base < GUEST_BASE + ARENA_BYTES && arena_resolve(NULL, base, 16) != (void*)(uintptr_t)base,
          "Lock returns a guest address, not the host pointer behind it");
    vcall(b, BUF_UNLOCK, p1, n1, 0u, 0u);
    CHECK(lock_buffer(b, 12, 8, 0, &p1, &n1, &p2, &n2) == DS_OK && p1 == base + 12 && n1 == 4 && p2 == base && n2 == 4,
          "a wrapping range gives two regions, as the streaming thread uses it");
    CHECK(vcall(b, BUF_UNLOCK, p1, n1, p2, n2) == DS_OK, "Unlock both");
    CHECK(lock_buffer(b, 4, 4, DSBLOCK_ENTIREBUFFER, &p1, &n1, &p2, &n2) == DS_OK && p1 == base && n1 == 16 && p2 == 0 && n2 == 0,
          "ENTIREBUFFER ignores offset and length");
    vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
    vcall(b, BUF_SET_POSITION, 4u);
    CHECK(lock_buffer(b, 0, 4, DSBLOCK_FROMWRITECURSOR, &p1, &n1, &p2, &n2) == DS_OK && p1 == base + 4 && n1 == 4,
          "FROMWRITECURSOR of a stopped buffer starts at its position");
    {
        uint32_t a = scratch(16);
        put32(a, 1);
        put32(a + 4, 5);
        put32(a + 8, 1);
        put32(a + 12, 5);
        CHECK(vcall(b, BUF_LOCK, 16u, 4u, a, a + 4, a + 8, a + 12, 0u) == DSERR_INVALIDPARAM && get32(a) == 0 && get32(a + 4) == 0 && get32(a + 8) == 0 && get32(a + 12) == 0,
              "a bad Lock leaves the outputs empty, which is what the game tests");
        CHECK(vcall(b, BUF_LOCK, 0u, 17u, a, a + 4, a + 8, a + 12, 0u) == DSERR_INVALIDPARAM, "too long");
        CHECK(vcall(b, BUF_LOCK, 0u, 4u, 0u, a + 4, a + 8, a + 12, 0u) == DSERR_INVALIDPARAM, "no pointer");
    }
    CHECK(vcall(b, BUF_UNLOCK, 0u, 0u, 0u, 0u) == DSERR_INVALIDPARAM, "Unlock of nothing");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_streaming_like_smacker(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, 0x18000 | DSBCAPS_CTRLVOLUME, 22050, 2, 16, 45056, NULL);
    float* out = (float*)malloc(2 * 4096 * sizeof(float));
    uint32_t play = 0, write = 0, filled = 0, delta, p1, n1, p2, n2;
    CHECK(b != 0, "Smacker's buffer: global focus, current position, volume, 22050 Hz stereo 16-bit");
    fill16(b, 45056, 0);
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK, "looping from the start");
    for (int round = 0; round < 40; round++) {
        mix(out, 2205);
        play = position_of(b, &write);
        delta = (play + 45056 - filled) % 45056;
        if (!delta) continue;
        CHECK(lock_buffer(b, filled, delta, 0, &p1, &n1, &p2, &n2) == DS_OK && n1 + n2 == delta, "refill Lock");
        vcall(b, BUF_UNLOCK, p1, n1, p2, n2);
        filled = (filled + delta) % 45056;
    }
    CHECK(play == (40u * 2205 * 4) % 45056, "the play cursor follows the output through the wrap (%u)", play);
    CHECK(vcall(b, BUF_STOP) == DS_OK && vcall(b, BUF_SET_POSITION, 0u) == DS_OK && vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING) == DS_OK,
          "Smacker's restart: Stop, SetCurrentPosition(0), Play");
    free(out);
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_duplicate(void) {
    uint32_t ds = make_device(), a = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL), out = scratch(4), d, bad;
    uint32_t primary, pa, pd, n, p2, n2;
    float mixed[4];
    fill16(a, 8, 16384);
    CHECK(vcall(ds, DEV_DUPLICATE, a, out) == DS_OK, "DuplicateSoundBuffer");
    d = get32(out);
    CHECK(d && d != a, "a new object");
    lock_buffer(a, 0, 8, 0, &pa, &n, &p2, &n2);
    lock_buffer(d, 0, 8, 0, &pd, &n, &p2, &n2);
    CHECK(pa == pd, "the duplicate shares the sample data");
    vcall(a, BUF_SET_VOLUME, (uint32_t)-10000);
    vcall(d, BUF_PLAY, 0u, 0u, 0u);
    CHECK(status_of(a) == 0, "playback state is independent");
    mix(mixed, 1);
    CHECK(mixed[0] == 0.5f, "the duplicate has its own volume");
    vcall(a, BUF_RELEASE);
    mix(mixed, 1);
    CHECK(mixed[0] == 0.5f, "and outlives the original");
    {
        uint8_t* data = (uint8_t*)arena_resolve(NULL, pd, 8);
        CHECK(data[0] == 0x00 && data[1] == 0x40, "its samples are not freed with the original");
    }
    primary = make_primary(ds, DSBCAPS_PRIMARYBUFFER, NULL);
    bad = scratch(4);
    CHECK(vcall(ds, DEV_DUPLICATE, primary, bad) == DSERR_INVALIDPARAM && get32(bad) == 0, "the primary cannot be duplicated");
    CHECK(vcall(ds, DEV_DUPLICATE, 0u, bad) == DSERR_INVALIDPARAM, "no source");
    vcall(primary, BUF_RELEASE);
    vcall(d, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_unsupported_methods(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRLPAN, 22050, 1, 16, 8, NULL), at = scratch(16);
    CHECK(vcall(b, BUF_SET_PAN, 0u) == DSERR_UNSUPPORTED && vcall(b, BUF_GET_PAN, at) == DSERR_UNSUPPORTED, "pan is not used by the game");
    CHECK(vcall(b, BUF_GET_CAPS, at) == DSERR_UNSUPPORTED, "unused queries answer unsupported");
    CHECK(vcall(b, BUF_SET_FORMAT, 0u) == DSERR_INVALIDCALL, "a secondary's format is fixed");
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

/* ---- 3D --------------------------------------------------------------------------------------------------------- */

static void test_3d(void) {
    uint32_t ds = make_device(), primary = make_primary(ds, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, NULL);
    uint32_t b = make_secondary(ds, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL), plain = make_secondary(ds, 0, 22050, 1, 16, 8, NULL);
    uint32_t q = scratch(4), listener, s3d;
    float out[4];
    vcall(primary, BUF_QI, guid_in_guest(&k_iid_listener), q);
    listener = get32(q);
    CHECK(vcall(b, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q) == DS_OK && get32(q), "3D buffer interface");
    s3d = get32(q);
    CHECK(vcall(plain, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q) == DS_E_NOINTERFACE && get32(q) == 0, "none on a buffer without DSBCAPS_CTRL3D");
    CHECK(vcall(b, BUF_QI, guid_in_guest(&k_iid_listener), q) == DS_E_NOINTERFACE, "the listener belongs to the primary");
    CHECK(vcall(b, BUF_QI, guid_in_guest(&k_iid_notify), q) == DS_E_NOINTERFACE, "no notifications");
    CHECK(vcall(b, BUF_QI, guid_in_guest(&k_iid_buffer), q) == DS_OK && get32(q) == b, "its own interface");
    vcall(b, BUF_RELEASE);
    fill16(b, 8, 16384);
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    mix(out, 1);
    CHECK(out[0] == 0.5f && out[1] == 0.5f, "a 3D buffer at the listener plays at full level");

    CHECK(vcall(s3d, S3D_SET_MIN_DISTANCE, float_bits(2.0f), DS3D_IMMEDIATE) == DS_OK, "SetMinDistance");
    CHECK(vcall(s3d, S3D_SET_POSITION, 0u, 0u, float_bits(8.0f), DS3D_IMMEDIATE) == DS_OK, "SetPosition");
    mix(out, 1);
    CHECK(near_to(out[0], 0.125f, 1e-4f) && near_to(out[1], 0.125f, 1e-4f), "four times the minimum distance: a quarter (%f)", out[0]);
    CHECK(vcall(s3d, S3D_SET_POSITION, float_bits(8.0f), 0u, 0u, DS3D_DEFERRED) == DS_OK, "deferred SetPosition");
    mix(out, 1);
    CHECK(near_to(out[0], 0.125f, 1e-4f), "a deferred setting changes nothing until it is committed");
    CHECK(vcall(listener, LST_COMMIT) == DS_OK, "commit");
    mix(out, 1);
    CHECK(out[0] < 1e-3f && near_to(out[1], 0.125f, 1e-3f), "committed: the source is on the right, the left channel falls silent (%f, %f)", out[0], out[1]);
    CHECK(vcall(listener, LST_SET_POSITION, float_bits(8.0f), 0u, 0u, DS3D_IMMEDIATE) == DS_OK, "an immediate listener move");
    mix(out, 1);
    CHECK(near_to(out[0], out[1], 1e-6f) && near_to(out[0], 0.5f, 1e-4f), "the listener at the source is centred and at full level");
    CHECK(vcall(listener, LST_SET_ORIENTATION, float_bits(1), 0u, 0u, 0u, float_bits(1), 0u, DS3D_IMMEDIATE) == DS_OK, "SetOrientation");

    CHECK(vcall(s3d, S3D_SET_CONE_ANGLES, 60u, 120u, DS3D_IMMEDIATE) == DS_OK && vcall(s3d, S3D_SET_CONE_OUTSIDE, (uint32_t)-2000, DS3D_IMMEDIATE) == DS_OK &&
              vcall(s3d, S3D_SET_CONE_ORIENTATION, 0u, 0u, float_bits(1), DS3D_IMMEDIATE) == DS_OK,
          "cone setters");
    CHECK(vcall(s3d, S3D_SET_CONE_ANGLES, 361u, 120u, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "cone angles over 360 are refused");
    CHECK(vcall(s3d, S3D_SET_CONE_OUTSIDE, 5u, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "an outside volume above 0");
    CHECK(vcall(s3d, S3D_SET_MIN_DISTANCE, float_bits(-1.0f), DS3D_IMMEDIATE) == DSERR_INVALIDPARAM, "a negative distance");
    CHECK(vcall(s3d, S3D_SET_MIN_DISTANCE, float_bits(1.0f), 5u) == DSERR_INVALIDPARAM, "an unknown apply flag");
    CHECK(vcall(s3d, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q) == DS_OK && get32(q) == s3d, "QueryInterface from the 3D face");
    vcall(s3d, BUF_RELEASE);
    vcall(s3d, BUF_RELEASE);
    vcall(listener, BUF_RELEASE);
    vcall(primary, BUF_RELEASE);
    vcall(plain, BUF_RELEASE);
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

/* ---- the object table ------------------------------------------------------------------------------------------- */

static void test_table_rejects_forged_and_stale_objects(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL), forged = scratch(16), at = scratch(4);
    uint32_t again;
    CHECK(vcall(ds, DEV_CREATE_BUFFER, 0u, 0u, 0u) == DSERR_INVALIDPARAM, "no output pointer");
    put32(forged, dsound_compat_vtable(DSI_BUFFER));
    put32(forged + 4, 0x00010000u);
    CHECK(dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ forged, at }) == DSERR_INVALIDPARAM, "a forged object with a made-up token is refused");
    put32(forged + 4, get32(b + 4));
    CHECK(dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ forged, at }) == DSERR_INVALIDPARAM, "a copy of a real token at another address is refused");
    CHECK(dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ 0u, at }) == DSERR_INVALIDPARAM &&
              dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ 0xFFFFFFF8u, at }) == DSERR_INVALIDPARAM &&
              dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ 0x1000u, at }) == DSERR_INVALIDPARAM,
          "null and outside guest memory");
    CHECK(dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ ds, at }) == DSERR_INVALIDPARAM, "a device is not a buffer");
    CHECK(dsound_compat_call(DSM_DeviceGetSpeakerConfig, (const uint32_t[]){ b, at }) == DSERR_INVALIDPARAM, "a buffer is not a device");
    CHECK(dsound_compat_is_device(ds) && !dsound_compat_is_device(b) && !dsound_compat_is_device(0) && !dsound_compat_is_device(forged), "is_device knows only devices");
    CHECK(vcall(b, BUF_RELEASE) == 0, "the buffer is released");
    CHECK(dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ b, at }) == DSERR_INVALIDPARAM &&
              dsound_compat_call(DSM_BufferAddRef, (const uint32_t[]){ b }) == 0 && dsound_compat_call(DSM_BufferRelease, (const uint32_t[]){ b }) == 0,
          "its stale address answers nothing and changes nothing");
    again = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 8, NULL);
    CHECK(again != 0 && dsound_compat_call(DSM_BufferGetStatus, (const uint32_t[]){ b, at }) == DSERR_INVALIDPARAM,
          "a new buffer in the same table slot does not revive the stale address");
    vcall(again, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_guest_addresses_never_leak_host_pointers(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRL3D, 22050, 1, 16, 4096, NULL), q = scratch(4), p1, n1, p2, n2;
    uint32_t addresses[8];
    int inside = 1;
    vcall(b, BUF_QI, guid_in_guest(&k_iid_buffer_3d), q);
    lock_buffer(b, 0, 16, 0, &p1, &n1, &p2, &n2);
    addresses[0] = ds;
    addresses[1] = b;
    addresses[2] = get32(q);
    addresses[3] = p1;
    addresses[4] = get32(ds);
    addresses[5] = get32(b);
    addresses[6] = get32(get32(b) + 4 * DSOUND_BUFFER_SLOTS - 4);
    addresses[7] = get32(dsound_compat_vtable(DSI_DEVICE));
    for (int i = 0; i < 4; i++) inside = inside && addresses[i] >= GUEST_BASE && addresses[i] < GUEST_BASE + ARENA_BYTES;
    CHECK(inside, "device, buffer, 3D face and Lock pointer are guest addresses");
    CHECK(addresses[4] == dsound_compat_vtable(DSI_DEVICE) && addresses[5] == dsound_compat_vtable(DSI_BUFFER), "the first word of an object is its interface vtable");
    CHECK(addresses[7] == thunk_address(NULL, DSM_DeviceQueryInterface) && addresses[6] >= THUNK_BASE, "vtable slots hold the entry addresses of the binding");
    {
        int clean = 1;
        const uint32_t host_low = (uint32_t)(uintptr_t)G.memory;
        for (uint32_t at = 0; at + 4 <= G.used; at += 4) {
            uint32_t word = get32(GUEST_BASE + at);
            if (word == host_low && host_low) clean = 0;
        }
        CHECK(clean, "no guest word holds the low half of a host pointer into the arena");
    }
    CHECK((uintptr_t)G.memory != (uintptr_t)GUEST_BASE,
          "the arena is not mapped at its guest address (%zu-byte host pointers): the layer only ever saw guest addresses", sizeof(void*));
    vcall(get32(q), BUF_RELEASE);
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

/* ---- threads, lifetime, shutdown -------------------------------------------------------------------------------- */

static int SDLCALL hammer(void* context) {
    uint32_t b = (uint32_t)(uintptr_t)context, scratch_at = scratch(40);
    for (int i = 0; i < 3000; i++) {
        vcall(b, BUF_GET_POSITION, scratch_at, scratch_at + 4);
        if (vcall(b, BUF_LOCK, get32(scratch_at), 64u, scratch_at + 8, scratch_at + 12, scratch_at + 16, scratch_at + 20, 0u) == DS_OK)
            vcall(b, BUF_UNLOCK, get32(scratch_at + 8), get32(scratch_at + 12), get32(scratch_at + 16), get32(scratch_at + 20));
        vcall(b, BUF_GET_STATUS, scratch_at + 24);
        if (i % 500 == 0) {
            vcall(b, BUF_STOP);
            vcall(b, BUF_SET_POSITION, 0u);
            vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
        }
    }
    return 0;
}

static void test_threads(void) {
    uint32_t ds = make_device(), b = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 2, 16, 45056, NULL);
    float* out = (float*)malloc(2 * 512 * sizeof(float));
    SDL_Thread* thread;
    int status = -1;
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    thread = SDL_CreateThread(hammer, "hammer", (void*)(uintptr_t)b);
    CHECK(thread != NULL, "a second thread");
    for (int i = 0; i < 400; i++) {
        mix(out, 512);
        vcall(b, BUF_SET_VOLUME, (uint32_t)-(i % 100));
    }
    if (thread) {
        SDL_WaitThread(thread, &status);
        CHECK(status == 0, "the streaming thread finishes: no deadlock");
    }
    free(out);
    vcall(b, BUF_RELEASE);
    vcall(ds, DEV_RELEASE);
}

static void test_lifetime_and_shutdown(void) {
    dsound_compat_counts_t before, after;
    uint32_t ds, a, b, s3d = scratch(4), at = scratch(16), out = scratch(4);
    uint32_t live_before;
    dsound_compat_counts(&before);
    CHECK(before.buffers_live == 0, "every buffer of the earlier tests was released (%u alive)", before.buffers_live);
    live_before = (uint32_t)G.live_blocks;
    for (int level = 0; level < 5; level++) {
        ds = make_device();
        for (int i = 0; i < 20; i++) {
            uint32_t s = make_secondary(ds, DSBCAPS_CTRLVOLUME, 22050, 1, 16, 4096, NULL);
            vcall(s, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
            vcall(s, BUF_RELEASE);
        }
        vcall(ds, DEV_RELEASE);
    }
    dsound_compat_counts(&after);
    CHECK(after.buffers_live == 0 && after.buffers_created == before.buffers_created + 100 && audio_mixer_voice_count(dsound_compat_mixer()) == 0,
          "100 buffers over five levels leave nothing behind");
    CHECK((uint32_t)G.live_blocks == live_before, "and no guest memory: every face, device and sample block was returned (%d vs %u)", G.live_blocks, live_before);
    ds = make_device();
    a = make_secondary(ds, DSBCAPS_CTRL3D, 22050, 1, 16, 4096, NULL);
    b = make_secondary(ds, 0, 22050, 1, 16, 4096, NULL);
    vcall(a, BUF_QI, guid_in_guest(&k_iid_buffer_3d), s3d);
    vcall(b, BUF_PLAY, 0u, 0u, DSBPLAY_LOOPING);
    vcall(ds, DEV_RELEASE);
    CHECK(status_of(b) != 0, "buffers keep playing after their device is released");
    dsound_compat_shutdown();
    CHECK(B.closes == 1 && dsound_compat_mixer() == NULL, "the shutdown closes the output once, before anything is freed");
    CHECK(status_of(b) == DSBSTATUS_BUFFERLOST, "later calls report the buffer lost");
    CHECK(vcall(b, BUF_PLAY, 0u, 0u, 0u) == DSERR_BUFFERLOST && vcall(b, BUF_STOP) == DSERR_BUFFERLOST && vcall(b, BUF_GET_POSITION, at, 0u) == DSERR_BUFFERLOST &&
              vcall(b, BUF_LOCK, 0u, 4u, at, at + 4, 0u, 0u, 0u) == DSERR_BUFFERLOST && vcall(b, BUF_SET_POSITION, 0u) == DSERR_BUFFERLOST,
          "and fail without touching freed memory");
    CHECK(vcall(get32(s3d), S3D_SET_POSITION, float_bits(1), float_bits(2), float_bits(3), DS3D_IMMEDIATE) == DS_OK, "3D setters stay safe");
    CHECK(call_method(DSM_Create, 0, out, 0) == DSERR_NODRIVER && B.opens == 1, "no output is opened once the host is shutting down");
    CHECK(vcall(a, BUF_RELEASE) == 1 && vcall(get32(s3d), BUF_RELEASE) == 0 && vcall(b, BUF_RELEASE) == 0, "releasing after the shutdown is safe");
    dsound_compat_shutdown();
    CHECK(B.closes == 1, "a second shutdown does nothing");
    CHECK(G.live_blocks == (int)live_before, "everything went back to guest memory after the shutdown too (%d vs %u)", G.live_blocks, live_before);
}

int main(void) {
    G.memory = (uint8_t*)calloc(1, ARENA_BYTES);
    if (!G.memory) return 2;
    test_abi_layout();
    test_install();
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
    test_table_rejects_forged_and_stale_objects();
    test_guest_addresses_never_leak_host_pointers();
    test_threads();
    test_lifetime_and_shutdown();
    printf("dsound_compat_tests: %d checks, %d failures\n", g_checks, g_failures);
    free(G.memory);
    return g_failures ? 1 : 0;
}
