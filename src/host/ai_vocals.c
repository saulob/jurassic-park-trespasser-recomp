/* CAISystem's vocal table bounds. The game indexes it with the Dinosaur and Vocal numbers of a level's "AI Command"
 * objects unchecked, and the slot past its end is the physics system's object. */
#include "guest_abi.h"
#include "guest_features.h"

#include <string.h>

#include "active_ai_policy.h"

#define VOCAL_SLOT_VA GVA(0x0041ADF0)
#define OPERATOR_NEW GVA(0x00626B8C)
#define LOG_LIMIT 8

static recomp_func_t s_lifted;
static uint32_t s_spare;
static int s_logged;

static void vocal_slot(void) {
    uint32_t bytes = AI_VOCAL_SLOT_BYTES;
    int32_t dinosaur, vocal;
    if (g_alias_entry) {
        s_lifted();
        return;
    }
    dinosaur = (int32_t)GUEST_ARG(0);
    vocal = (int32_t)GUEST_ARG(1);
    if (ai_vocal_slot_valid(dinosaur, vocal)) {
        s_lifted();
        return;
    }
    if (!s_spare) guest_call(OPERATOR_NEW, 0, &bytes, 1, 1, &s_spare);
    if (!s_spare) {
        s_lifted();
        return;
    }
    memset((void*)ADDR(s_spare), 0, AI_VOCAL_SLOT_BYTES);
    if (s_logged < LOG_LIMIT)
        host_log("level", "AI Command Dinosaur %d, Vocal %d is outside the game's %d x %d vocal table: its sounds are left out%s",
                 (int)dinosaur, (int)vocal, AI_VOCAL_DINOSAURS, AI_VOCAL_KINDS,
                 s_logged + 1 == LOG_LIMIT ? " (later ones are not logged)" : "");
    if (s_logged < LOG_LIMIT) s_logged++;
    g_eax = s_spare;
    GUEST_RETURN_STDCALL(2);
}

void ai_vocals_install(void) {
    if (!guest_override_install(VOCAL_SLOT_VA, vocal_slot, &s_lifted))
        host_log("host", "warning: the AI vocal table bounds could not be hooked");
}
