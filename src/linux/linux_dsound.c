#include "linux_dsound.h"

#include <stdlib.h>

#include "audio_backend.h"
#include "dsound_compat.h"
#include "guest_memory.h"
#include "linux_crash.h"
#include "linux_log.h"

static int s_installed, s_headless;
static dsound_method_t s_method[DSM_COUNT];

static void* guest_resolve(void* context, uint32_t address, uint32_t size) {
    void* p;
    (void)context;
    p = gm_ptr_checked(address, size, 1);
    return p ? p : gm_ptr_checked(address, size, 0);
}

static uint32_t guest_alloc(void* context, uint32_t size) {
    (void)context;
    return gm_heap_alloc(size, 1);
}

static void guest_release(void* context, uint32_t address) {
    (void)context;
    gm_heap_free(address);
}

static uint64_t method_call(const uint32_t* a, void* user) {
    dsound_method_t method = *(const dsound_method_t*)user;
    lx_crash_audio_stage(method == DSM_BufferLock ? LX_AUDIO_BUFFER_LOCK : method == DSM_BufferUnlock ? LX_AUDIO_BUFFER_UNLOCK : LX_AUDIO_METHOD);
    return dsound_compat_call(method, a);
}

static uint32_t method_address(void* context, dsound_method_t method) {
    (void)context;
    return ht_register(dsound_method_name(method), method_call, (int)dsound_method_words(method), HT_STDCALL, &s_method[method]);
}

static int ensure_installed(void) {
    dsound_binding_t binding;
    if (s_installed) return 1;
    for (int i = 0; i < DSM_COUNT; i++) s_method[i] = (dsound_method_t)i;
    if (s_headless) setenv("SDL_AUDIODRIVER", "dummy", 1);
    binding.guest.context = NULL;
    binding.guest.resolve = guest_resolve;
    binding.guest.alloc = guest_alloc;
    binding.guest.release = guest_release;
    binding.method_address = method_address;
    binding.context = NULL;
    lx_crash_audio_stage(LX_AUDIO_DEVICE_OPEN);
    if (!dsound_compat_install(sdl_audio_backend(), &binding)) return 0;
    s_installed = 1;
    return 1;
}

static uint64_t d_DirectSoundCreate(const uint32_t* a, void* u) {
    (void)u;
    if (!ensure_installed()) {
        lx_log("Linux DSOUND", "the DirectSound layer could not be installed");
        return DSERR_NODRIVER;
    }
    return dsound_compat_call(DSM_Create, a);
}

void lx_dsound_set_headless(int headless) { s_headless = headless; }

void hle_dsound_shutdown(void) {
    if (s_installed) dsound_compat_shutdown();
    s_installed = 0;
}

static const hle_entry_t k_table[] = { { "dsound.dll", "DirectSoundCreate", d_DirectSoundCreate, 3, HT_STDCALL } };

const hle_entry_t* hle_dsound_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
