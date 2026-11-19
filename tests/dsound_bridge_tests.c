/* The 32-bit Windows binding of the DirectSound layer: the portable ABI against dsound.h (every constant, structure and
 * vtable slot, compared with dsound_windows_abi.c), the stdcall entry points the retail code and SMACKW32.DLL call
 * natively, and the import gate that keeps the system DSound.dll from loading. Over a mixer with no audio device;
 * nothing is played. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_sync.h"
#include "platform/windows_dsound_bridge.h"
#include "platform/windows_dsound_imports.h"

#include "dsound_abi_pairs.h"

unsigned windows_abi_count(void);
uint32_t windows_abi_value(unsigned index);
int windows_abi_iid(unsigned which, unsigned char out[16]);

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

static struct {
    int opens, closes;
    audio_mutex_t* lock;
} B;

static void test_lock(void* context) {
    (void)context;
    audio_mutex_lock(B.lock);
}

static void test_unlock(void* context) {
    (void)context;
    audio_mutex_unlock(B.lock);
}

static audio_mixer_t* backend_open(uint32_t rate) {
    audio_lock_ops_t ops = { NULL, test_lock, test_unlock };
    if (!B.lock) B.lock = audio_mutex_create();
    B.opens++;
    return audio_mixer_create(rate, &ops);
}

static void backend_set_rate(uint32_t rate) { audio_mixer_set_rate(dsound_compat_mixer(), rate); }

static void backend_close(void) {
    B.closes++;
    audio_mixer_destroy(dsound_compat_mixer());
}

static const char* backend_name(void) { return "test"; }

static const audio_backend_t k_backend = { backend_open, backend_set_rate, backend_close, backend_name };

/* ---- the portable ABI against dsound.h -------------------------------------------------------------------------- */

#define PORTABLE_VALUE(portable, windows) (uint32_t)(portable),
static void test_abi_matches_dsound_h(void) {
    const uint32_t k_portable[] = { DSOUND_ABI_PAIRS(PORTABLE_VALUE) 0 };
    unsigned count = (unsigned)(sizeof k_portable / sizeof k_portable[0]) - 1u, wrong = 0;
    CHECK(windows_abi_count() == count && count > 100, "both sides list the same %u values", count);
    for (unsigned i = 0; i < count && i < windows_abi_count(); i++) {
        if (k_portable[i] == windows_abi_value(i)) continue;
        wrong++;
        printf("FAIL value %u: portable 0x%08X, dsound.h 0x%08X\n", i, k_portable[i], windows_abi_value(i));
    }
    g_checks += (int)count;
    g_failures += (int)wrong;
    {
        static const unsigned char k_3d_buffer[16] = { 0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
        static const unsigned char k_listener[16] = { 0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
        static const unsigned char k_buffer[16] = { 0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
        static const unsigned char k_device[16] = { 0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
        unsigned char iid[16];
        windows_abi_iid(0, iid);
        CHECK(!memcmp(iid, k_3d_buffer, 16), "IID_IDirectSound3DBuffer");
        windows_abi_iid(1, iid);
        CHECK(!memcmp(iid, k_listener, 16), "IID_IDirectSound3DListener");
        windows_abi_iid(2, iid);
        CHECK(!memcmp(iid, k_buffer, 16), "IID_IDirectSoundBuffer");
        windows_abi_iid(3, iid);
        CHECK(!memcmp(iid, k_device, 16), "IID_IDirectSound");
    }
}

static void test_entries(void) {
    uint32_t seen[DSM_COUNT];
    int distinct = 1, zero = 0;
    for (int i = 0; i < DSM_COUNT; i++) {
        seen[i] = jpt_dsound_bridge_entry((dsound_method_t)i);
        if (!seen[i]) zero++;
        for (int j = 0; j < i; j++)
            if (seen[j] == seen[i]) distinct = 0;
    }
    CHECK(zero == 0 && distinct, "every method has its own entry point (%d methods)", DSM_COUNT);
    CHECK(jpt_dsound_bridge_entry(DSM_COUNT) == 0, "no entry past the last method");
    {
        typedef uint32_t(WINAPI * create_fn)(uint32_t, uint32_t, uint32_t);
        CHECK(jpt_dsound_create(NULL, NULL, NULL) == ((create_fn)(uintptr_t)jpt_dsound_bridge_entry(DSM_Create))(0, 0, 0),
              "DirectSoundCreate and the Create entry are the same call");
    }
}

/* ---- calls as the retail code makes them: stdcall through the vtable of an object the host handed out ------------- */

typedef uint32_t(WINAPI* call1_t)(uint32_t);
typedef uint32_t(WINAPI* call2_t)(uint32_t, uint32_t);
typedef uint32_t(WINAPI* call3_t)(uint32_t, uint32_t, uint32_t);
typedef uint32_t(WINAPI* call4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(WINAPI* call5_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(WINAPI* call8_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

static void* slot_of(uint32_t object, unsigned slot) { return ((void**)(*(uintptr_t**)(uintptr_t)object))[slot]; }
static uint32_t c1(uint32_t o, unsigned s) { return ((call1_t)slot_of(o, s))(o); }
static uint32_t c2(uint32_t o, unsigned s, uint32_t a) { return ((call2_t)slot_of(o, s))(o, a); }
static uint32_t c3(uint32_t o, unsigned s, uint32_t a, uint32_t b) { return ((call3_t)slot_of(o, s))(o, a, b); }
static uint32_t c4(uint32_t o, unsigned s, uint32_t a, uint32_t b, uint32_t c) { return ((call4_t)slot_of(o, s))(o, a, b, c); }
static uint32_t c5(uint32_t o, unsigned s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return ((call5_t)slot_of(o, s))(o, a, b, c, d); }
static uint32_t c8(uint32_t o, unsigned s, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    return ((call8_t)slot_of(o, s))(o, a, b, c, d, e, f, g);
}

static uint32_t addr(const void* p) { return (uint32_t)(uintptr_t)p; }
static uint32_t bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static const unsigned char k_iid_unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static void test_native_calls(void) {
    uint32_t ds = 0, primary = 0, b = 0, again = 0, s3d = 0, listener = 0, q = 0;
    unsigned char iid_3d[16], iid_listener[16];
    ds_waveformatex_t wfx;
    ds_bufferdesc_t desc;
    ds_caps_t caps;
    uint32_t p1 = 0, n1 = 0, p2 = 0, n2 = 0, status = 0, speakers = 0;
    int32_t volume = 5;
    float out[8];
    uint32_t hr;
    windows_abi_iid(0, iid_3d);
    windows_abi_iid(1, iid_listener);
    CHECK(jpt_dsound_bridge_install(&k_backend) == 1, "the bridge installs");
    hr = jpt_dsound_create(NULL, (LPVOID*)&ds, NULL);
    CHECK(hr == DS_OK && ds && B.opens == 1, "DirectSoundCreate through the stdcall entry");
    CHECK(ds >= 0x10000 && *(uintptr_t*)(uintptr_t)ds != 0, "the guest gets a block with a vtable of host entry points");
    CHECK(c1(ds, 1) == 2 && c1(ds, 2) == 1, "AddRef and Release through the vtable");
    CHECK(c3(ds, 0, addr(k_iid_unknown), addr(&q)) == DS_OK && q == ds && c1(ds, 2) == 1, "QueryInterface for IUnknown");
    memset(&caps, 0, sizeof caps);
    caps.size = sizeof caps;
    CHECK(c2(ds, 4, addr(&caps)) == DS_OK && caps.max_hw_3d_all == 0 && (caps.flags & DSCAPS_SECONDARY16BIT), "GetCaps");
    CHECK(c3(ds, 6, 0, DSSCL_PRIORITY) == DS_OK, "SetCooperativeLevel");
    CHECK(c2(ds, 8, addr(&speakers)) == DS_OK && speakers == DSSPEAKER_STEREO, "GetSpeakerConfig");

    memset(&desc, 0, sizeof desc);
    desc.size = DS_BUFFERDESC_V1_SIZE;
    desc.flags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    CHECK(c4(ds, 3, addr(&desc), addr(&primary), 0) == DS_OK && primary, "the primary buffer");
    CHECK(c3(primary, 0, addr(iid_listener), addr(&listener)) == DS_OK && listener, "the listener");

    memset(&wfx, 0, sizeof wfx);
    wfx.format_tag = DS_WAVE_FORMAT_PCM;
    wfx.channels = 1;
    wfx.samples_per_second = 22050;
    wfx.bits_per_sample = 16;
    wfx.block_align = 2;
    wfx.bytes_per_second = 44100;
    memset(&desc, 0, sizeof desc);
    desc.size = DS_BUFFERDESC_V1_SIZE;
    desc.flags = DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    desc.buffer_bytes = 8;
    desc.format_address = addr(&wfx);
    CHECK(c4(ds, 3, addr(&desc), addr(&b), 0) == DS_OK && b, "a secondary buffer");
    CHECK(c8(b, 11, 0, 8, addr(&p1), addr(&n1), addr(&p2), addr(&n2), 0) == DS_OK && p1 && n1 == 8 && p2 == 0 && n2 == 0, "Lock");
    {
        int16_t* samples = (int16_t*)(uintptr_t)p1;
        for (int i = 0; i < 4; i++) samples[i] = 16384;
    }
    CHECK(c5(b, 19, p1, n1, p2, n2) == DS_OK, "Unlock");
    CHECK(c4(b, 12, 0, 0, DSBPLAY_LOOPING) == DS_OK && c2(b, 9, addr(&status)) == DS_OK && status == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "Play(looping)");
    audio_mixer_mix(dsound_compat_mixer(), out, 1);
    CHECK(out[0] == 0.5f, "the sample the guest wrote through Lock is what the mixer plays");
    CHECK(c2(b, 15, (uint32_t)-602) == DS_OK && c2(b, 6, addr(&volume)) == DS_OK && volume == -602, "the menu click's -6.02 dB");
    audio_mixer_mix(dsound_compat_mixer(), out, 1);
    CHECK(fabsf(out[0] - 0.25f) < 0.002f, "at half the amplitude (%f)", out[0]);
    CHECK(c3(b, 0, addr(iid_3d), addr(&s3d)) == DS_OK && s3d, "the 3D buffer interface");
    CHECK(c3(s3d, 17, bits(2.0f), DS3D_IMMEDIATE) == DS_OK && c5(s3d, 19, bits(0.0f), bits(0.0f), bits(8.0f), DS3D_IMMEDIATE) == DS_OK,
          "3D setters take their float arguments from the stack");
    c2(b, 15, 0);
    audio_mixer_mix(dsound_compat_mixer(), out, 1);
    CHECK(fabsf(out[0] - 0.125f) < 1e-3f, "four times the minimum distance: a quarter of the level (%f)", out[0]);
    CHECK(c5(listener, 14, bits(1.0f), bits(2.0f), bits(3.0f), DS3D_DEFERRED) == DS_OK &&
              c8(listener, 13, bits(0), bits(0), bits(1), bits(0), bits(1), bits(0), DS3D_DEFERRED) == DS_OK && c1(listener, 17) == DS_OK,
          "the listener's calls, the longest with eight words");
    CHECK(c3(ds, 5, b, addr(&again)) == DS_OK && again && again != b, "DuplicateSoundBuffer");
    CHECK(c2(b, 17, 44100) == DSERR_CONTROLUNAVAIL, "no frequency control without the flag");
    CHECK(c1(b, 18) == DS_OK && c1(b, 20) == DS_OK, "Stop and Restore");
    c1(again, 2);
    c1(s3d, 2);
    c1(listener, 2);
    c1(b, 2);
    c1(primary, 2);
    CHECK(c1(ds, 2) == 0, "everything is released");
    dsound_compat_shutdown();
    CHECK(B.closes == 1, "the shutdown closes the output once");
}

/* ---- imports ---------------------------------------------------------------------------------------------------- */

static DWORD WINAPI replacement_pid(void) { return 4321; }

static void test_import_gate(void) {
    HMODULE host = dsound_host_module(), kernel = GetModuleHandleA("kernel32.dll");
    FARPROC original = GetProcAddress(kernel, "GetCurrentProcessId");
    CHECK(host != NULL && (uintptr_t)host >= 0x20, "the stand-in module handle passes Smacker's 'handle < 0x20' test");
    CHECK(dsound_load_library_a("DSOUND.DLL") == host && dsound_load_library_a("dsound.dll") == host && dsound_load_library_a("C:\\x\\DSound.dll") == host,
          "every request for DSound.dll gets the host's");
    CHECK(dsound_get_proc_address(host, "DirectSoundCreate") == (FARPROC)jpt_dsound_create, "DirectSoundCreate is the host's");
    CHECK(dsound_get_proc_address(host, "DirectSoundCreate8") == NULL && dsound_get_proc_address(host, "DirectSoundEnumerateA") == NULL &&
              dsound_get_proc_address(host, (LPCSTR)(uintptr_t)1) == NULL,
          "nothing else is answered, ordinals included");
    CHECK(dsound_free_library(host) == TRUE, "FreeLibrary of the stand-in");
    CHECK(dsound_get_proc_address(kernel, "GetCurrentProcessId") == original && original != NULL, "other modules pass through");
    CHECK(dsound_load_library_a("kernel32.dll") == kernel, "other libraries load normally");
    CHECK(GetModuleHandleA("dsound.dll") == NULL, "the system DSound.dll was never loaded by any of this");
    CHECK(dsound_patch_import(GetModuleHandleA(NULL), "kernel32.dll", "NoSuchImport", (void*)replacement_pid) == 0, "an import that is not there is not patched");
    CHECK(dsound_patch_import(GetModuleHandleA(NULL), "nosuch.dll", "GetCurrentProcessId", (void*)replacement_pid) == 0, "nor is another library's");
    CHECK(dsound_patch_import(NULL, "kernel32.dll", "GetCurrentProcessId", (void*)replacement_pid) == 0, "no module");
    if (dsound_patch_import(GetModuleHandleA(NULL), "kernel32.dll", "GetCurrentProcessId", (void*)replacement_pid)) {
        volatile DWORD seen = GetCurrentProcessId();
        CHECK(seen == 4321, "a patched import is what the module calls");
        CHECK(dsound_patch_import(GetModuleHandleA(NULL), "KERNEL32.DLL", "GetCurrentProcessId", (void*)original) == 1 && GetCurrentProcessId() != 4321, "and it is restored");
    } else {
        printf("dsound_bridge_tests: GetCurrentProcessId is not imported by this program, the live patch check is skipped\n");
    }
}

int main(void) {
    test_abi_matches_dsound_h();
    test_entries();
    test_native_calls();
    test_import_gate();
    printf("dsound_bridge_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
