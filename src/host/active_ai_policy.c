#include "active_ai_policy.h"

#include <string.h>

const uint32_t active_ai_override_vas[] = {
    0x00419330u, /* CAISystem constructor */
    0x004195A0u, /* CAISystem destructor */
    0x00419800u, /* Activate */
    0x004199D0u, /* Deactivate */
    0x00419C60u, /* deactivate within a radius */
    0x00419CE0u, /* the first active animal near an object sees it */
    0x00419DB0u, /* every active animal near a point hears it */
    0x00419E30u, /* think scheduler */
    0x0041A200u, /* Process */
    0x0041AE50u, /* Save */
    0x0041AF00u, /* Load */
    0x004213F0u, /* brain influences (wrapped) */
    0x004218C0u, /* its tail call, where entries 4..6 join the influences */
};
const unsigned active_ai_override_count = sizeof active_ai_override_vas / sizeof active_ai_override_vas[0];

int active_ai_overrides(uint32_t va) {
    for (unsigned i = 0; i < active_ai_override_count; i++)
        if (active_ai_override_vas[i] == va) return 1;
    return 0;
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
