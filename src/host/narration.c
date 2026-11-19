/*
 * Disable Hammond Narration: the VOICEOVER action (CVoiceOverAction, execute 0x0046CD80) holds the sample ID at +8 and
 * posts the message that starts the sample and its captions, so a blocked sample is skipped here and never starts. The
 * function is only reached through its vtable, so the dispatch answers it (recomp_lookup_manual) instead of a stable entry.
 */
#include "guest_va.h"
#include "host.h"

#include "narration_policy.h"

#define VOICE_OVER_VA GVA(0x0046CD80)
#define VOICE_OVER_SAMPLE 0x8u
#define VOICE_OVER_BYTES 12u
#define LOG_LIMIT 4

static recomp_func_t s_lifted;
static uint32_t s_va;
static int s_logged;

static void voice_over(void) {
    uint32_t action = g_ecx;
    if (guest_readable(action, VOICE_OVER_BYTES) && narration_blocked(MEM32(action + VOICE_OVER_SAMPLE))) {
        if (s_logged++ < LOG_LIMIT)
            host_log("audio", "Disable Hammond Narration: voice-over sample %08X not started", MEM32(action + VOICE_OVER_SAMPLE));
        GUEST_RETURN_STDCALL(0);
        return;
    }
    s_lifted();
}

recomp_func_t narration_lookup(uint32_t va) { return s_va && va == s_va ? voice_over : NULL; }

void narration_configure(int disabled) {
    narration_set_disabled(disabled);
    if (disabled) host_log("settings", "Disable Hammond Narration on: John Hammond's voice-overs and captions are not played");
}

void narration_install(void) {
    s_lifted = (recomp_func_t)GSUB(0x0046CD80);
    s_va = s_lifted ? VOICE_OVER_VA : 0;
    if (!s_va) host_log("audio", "Disable Hammond Narration: the voice-over action is not in this build, the option does nothing");
}
