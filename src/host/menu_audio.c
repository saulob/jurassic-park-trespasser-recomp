#include "host.h"

#include <string.h>

#include "guest_va.h"
#include "menu_audio_policy.h"

#define SELECTION_LOAD_VA GVA(0x00414D50)
#define SAMPLE_SET_VOLUME_VA GVA(0x0043D7E0)
#define WINDOW_SELECTION_SAMPLE 0x160
#define SAMPLE_VOLUME_DB 0x0C
#define SAMPLE_ATTENUATION_DB 0x5C

static recomp_func_t s_load_lifted;

static float guest_float(uint32_t va) {
    uint32_t bits = MEM32(va);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* Loads the window's selection sound (this in ecx), then lowers it to the menu level. */
static void selection_load(void) {
    uint32_t window = g_ecx, sample;
    s_load_lifted();
    if (g_alias_entry || !window || !guest_read32(window + WINDOW_SELECTION_SAMPLE, &sample) || !sample) return;
    {
        float arg = menu_effect_volume_arg(guest_float(sample + SAMPLE_VOLUME_DB), guest_float(sample + SAMPLE_ATTENUATION_DB));
        uint32_t bits;
        memcpy(&bits, &arg, sizeof bits);
        guest_call(SAMPLE_SET_VOLUME_VA, sample, &bits, 1, 0, NULL);
    }
}

void menu_audio_install(void) {
    if (!guest_override_install(SELECTION_LOAD_VA, selection_load, &s_load_lifted))
        host_log("audio", "the menu selection sound override is missing: the click keeps the retail volume");
}
