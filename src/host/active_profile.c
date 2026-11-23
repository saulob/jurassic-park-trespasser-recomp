#include "active_profile.h"

#include <stdio.h>
#include <string.h>

uint32_t jpt_gva[JPT_GVA_COUNT];
void (*jpt_gsub[JPT_GSUB_COUNT])(void);
jpt_layout_t jpt_layout;
unsigned jpt_patch_groups;

static const jpt_game_version_t* s_profile;
static jpt_lift_t s_lift = JPT_LIFT_NONE;

int jpt_lift_find(const char* lift_id, jpt_lift_t* out) {
    if (!lift_id) return 0;
    for (int i = 0; i < JPT_LIFT_COUNT; i++) {
        if (!strcmp(jpt_lift_data[i].id, lift_id)) {
            if (out) *out = (jpt_lift_t)i;
            return 1;
        }
    }
    return 0;
}

uint32_t jpt_guest_reservation_size(void) {
    uint32_t span = 0;
    for (int i = 0; i < JPT_LIFT_COUNT; i++)
        if (jpt_lift_data[i].image_size > span) span = jpt_lift_data[i].image_size;
    return span;
}

int jpt_lift_missing_address(const jpt_lift_data_t* lift) {
    for (int i = 0; i < JPT_GVA_COUNT; i++) {
        if (lift->gva[i]) continue;
        if (!jpt_gva_gate[i] || (lift->patch_groups & jpt_gva_gate[i])) return i;
    }
    return -1;
}

static int fail(char* error, size_t cap, const char* fmt, const char* a, const char* b) {
    if (error && cap) snprintf(error, cap, fmt, a, b);
    return 0;
}

int jpt_profile_activate(const jpt_game_version_t* version, char* error, size_t cap) {
    jpt_lift_t lift;
    int missing;
    if (!version) return fail(error, cap, "no game version to run%s%s", "", "");
    if (s_profile) {
        if (s_profile == version) return 1;
        return fail(error, cap, "%s is active: the profile is chosen once per run, not %s", s_profile->id, version->id);
    }
    if (!jpt_lift_find(version->lift, &lift))
        return fail(error, cap, "%s needs the lift %s, which this program does not carry", version->id, version->lift);
    missing = jpt_lift_missing_address(&jpt_lift_data[lift]);
    if (missing >= 0) {
        if (error && cap)
            snprintf(error, cap, "%s: %s lacks the guest address 0x%08X (1.0.116) that the host needs", version->id,
                     version->lift, (unsigned)jpt_gva_canonical[missing]);
        return 0;
    }
    memcpy(jpt_gva, jpt_lift_data[lift].gva, sizeof jpt_gva);
    jpt_layout = jpt_lift_data[lift].layout;
    jpt_patch_groups = jpt_lift_data[lift].patch_groups;
    s_lift = lift;
    s_profile = version;
    return 1;
}

void jpt_profile_reset(void) {
    memset(jpt_gva, 0, sizeof jpt_gva);
    memset(jpt_gsub, 0, sizeof jpt_gsub);
    memset(&jpt_layout, 0, sizeof jpt_layout);
    jpt_patch_groups = 0;
    s_profile = NULL;
    s_lift = JPT_LIFT_NONE;
}

const jpt_game_version_t* jpt_active_profile(void) { return s_profile; }

jpt_lift_t jpt_active_lift(void) { return s_lift; }

const jpt_lift_data_t* jpt_active_lift_data(void) { return s_lift == JPT_LIFT_NONE ? NULL : &jpt_lift_data[s_lift]; }

uint32_t jpt_gva_or_canonical(unsigned id) { return jpt_active_profile() ? jpt_gva[id] : jpt_gva_canonical[id]; }

const uint32_t* jpt_gva_table(void) { return jpt_active_profile() ? jpt_gva : jpt_gva_canonical; }
