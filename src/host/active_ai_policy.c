#include "guest_va.h"
#include "active_ai_policy.h"

#include <string.h>

static const unsigned k_override_ids[] = {
    GVA_ID(0x00419330), /* CAISystem constructor */
    GVA_ID(0x004195A0), /* CAISystem destructor */
    GVA_ID(0x00419800), /* Activate */
    GVA_ID(0x004199D0), /* Deactivate */
    GVA_ID(0x00419C60), /* deactivate within a radius */
    GVA_ID(0x00419CE0), /* the first active animal near an object sees it */
    GVA_ID(0x00419DB0), /* every active animal near a point hears it */
    GVA_ID(0x00419E30), /* think scheduler */
    GVA_ID(0x0041A200), /* Process */
    GVA_ID(0x0041AE50), /* Save */
    GVA_ID(0x0041AF00), /* Load */
    GVA_ID(0x004213F0), /* brain influences (wrapped) */
    GVA_ID(0x004218C0), /* its tail call, where entries 4..6 join the influences */
};
const unsigned active_ai_override_count = sizeof k_override_ids / sizeof k_override_ids[0];

uint32_t active_ai_override_va(unsigned i) { return GVA_AT(k_override_ids[i]); }

int active_ai_overrides(uint32_t va) {
    for (unsigned i = 0; i < active_ai_override_count; i++)
        if (active_ai_override_va(i) == va) return 1;
    return 0;
}

int ai_vocal_slot_valid(int32_t dinosaur, int32_t vocal) {
    return dinosaur >= 0 && dinosaur < AI_VOCAL_DINOSAURS && vocal >= 0 && vocal < AI_VOCAL_KINDS;
}

int active_ai_limit(int enhanced) { return enhanced ? ACTIVE_AI_ENHANCED_LIMIT : ACTIVE_AI_ORIGINAL_LIMIT; }

int active_ai_descending(int limit, int out[ACTIVE_AI_MAX_LIMIT]) {
    int n = 0;
    if (limit > ACTIVE_AI_MAX_LIMIT) limit = ACTIVE_AI_MAX_LIMIT;
    for (int i = limit; i >= 1; i--) out[n++] = i;
    return n;
}

int active_ai_think_next(const uint32_t* active, int limit, int32_t* cursor) {
    int32_t i = *cursor + 1;
    *cursor = i;
    while (i >= 0 && i <= limit) {
        if (active[i]) return i;
        *cursor = ++i;
    }
    *cursor = -1;
    return -1;
}

void active_ai_pick_begin(active_ai_pick_t* p, float new_distance) {
    p->best = new_distance;
    p->empty = -1;
    p->farthest = -1;
}

void active_ai_pick_slot(active_ai_pick_t* p, int index, int occupied, double distance) {
    if (!occupied) {
        p->empty = index;
        return;
    }
    if (!active_ai_x87_c0c3(distance, (double)p->best)) {
        p->best = (float)distance;
        p->farthest = index;
    }
}

double active_ai_dist2_zyx(const float a[3], const float b[3]) {
    double x = (double)a[0] - (double)b[0], y = (double)a[1] - (double)b[1], z = (double)a[2] - (double)b[2];
    return (z * z + y * y) + x * x;
}

double active_ai_dist2_zxy(const float a[3], const float b[3]) {
    double x = (double)a[0] - (double)b[0], y = (double)a[1] - (double)b[1], z = (double)a[2] - (double)b[2];
    return (z * z + x * x) + y * y;
}

int active_ai_x87_c0(double st0, double src) { return !(st0 >= src); }

int active_ai_x87_c0c3(double st0, double src) { return !(st0 > src); }

int active_ai_count_set(const uint32_t* values, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) n += values[i] != 0;
    return n;
}

int active_ai_boxes_allow(int in_use, int adding, int removing, int pool) { return in_use - removing + adding <= pool; }

void active_ai_save_values(const uint32_t* active, int limit, uint32_t out[ACTIVE_AI_SAVE_ENTRIES]) {
    memset(out, 0, sizeof(uint32_t) * ACTIVE_AI_SAVE_ENTRIES);
    if (limit < ACTIVE_AI_ORIGINAL_LIMIT) limit = ACTIVE_AI_ORIGINAL_LIMIT;
    if (limit > ACTIVE_AI_MAX_LIMIT) limit = ACTIVE_AI_MAX_LIMIT;
    for (int i = 0; i <= limit; i++) out[i] = active[i];
}

int active_ai_load_activations(int limit) {
    if (limit < ACTIVE_AI_ORIGINAL_LIMIT) limit = ACTIVE_AI_ORIGINAL_LIMIT;
    if (limit > ACTIVE_AI_MAX_LIMIT) limit = ACTIVE_AI_MAX_LIMIT;
    return limit + 1;
}
