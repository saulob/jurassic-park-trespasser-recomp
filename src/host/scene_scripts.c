#include "guest_abi.h"
#include "guest_features.h"
#include "jpt_custom_level.h"
#include "jpt_scene.h"

#define OBJECT_LOAD_VA GVA(0x00614F50) /* CInstance::pcLoad(buffer), thiscall: returns the buffer after what it read */
#define SAVE_VERSION GVA(0x00665130)
#define SCRIPT_STATE 1u
#define SCRIPT_BYTES_MAX (1u << 20)

static recomp_func_t s_lifted;
static int s_logged;

/* Trespasser CE's scenes end each object's common part with a mark and, for an object with a script, its state. */
static void object_load(void) {
    uint32_t mark = 0, bytes = 0;
    s_lifted();
    if (g_alias_entry || !jpt_custom_level_requires(JPT_LEVEL_CAP_SCRIPT_STATE)) return;
    if ((int32_t)MEM32(SAVE_VERSION) < JPT_SCENE_VERSION_SCRIPTS || !guest_read32(g_eax, &mark)) return;
    g_eax += 4u;
    if (!(mark & SCRIPT_STATE) || !guest_read32(g_eax, &bytes) || bytes > SCRIPT_BYTES_MAX) return;
    g_eax += 4u + bytes;
    if (!s_logged++) host_log("custom-level", "the scene holds the state of object scripts (Trespasser CE): it is left out");
}

void scene_scripts_install(void) {
    if (!guest_override_install(OBJECT_LOAD_VA, object_load, &s_lifted))
        host_log("host", "warning: scenes of version %d and later cannot be read", JPT_SCENE_VERSION_SCRIPTS);
}
