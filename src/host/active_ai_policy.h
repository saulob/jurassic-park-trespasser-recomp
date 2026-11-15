/* Active AI: the CAISystem limit of simultaneously active animals (retail 3, Trespasser CE's MaxActiveAi), without
 * guest memory (tests/active_ai_tests.c). */
#ifndef ACTIVE_AI_POLICY_H
#define ACTIVE_AI_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACTIVE_AI_ORIGINAL_LIMIT 3
#define ACTIVE_AI_ENHANCED_LIMIT 6
#define ACTIVE_AI_MAX_LIMIT 6
/* CAISystem +0x3188 (animals) and +0x31A0 (slots) hold entries 0..3; entry 0 never holds an animal. */
#define ACTIVE_AI_RETAIL_ENTRIES 4
/* Every save holds this many animal handles, whatever the limit. */
#define ACTIVE_AI_SAVE_ENTRIES 10
/* CAnimal +0xAC..+0xC8: the body parts, one physics box each while simulated. */
#define ACTIVE_AI_ANIMAL_PARTS 8
/* The physics box pool (0x00699D40, 0x1090 bytes per box): its allocator gives up past 50. */
#define ACTIVE_AI_BOX_POOL 50

int active_ai_limit(int enhanced);

/* The retail loops visit the active entries from the limit down to 1: out[0] = limit. Returns the count. */
int active_ai_descending(int limit, int out[ACTIVE_AI_MAX_LIMIT]);

/* The think scheduler (0x00419E30): *cursor advances over entries 0..limit; returns the entry whose animal thinks this
 * tick, or -1 when the cursor passed the last one (it restarts at -1). active has limit + 1 entries. */
int active_ai_think_next(const uint32_t* active, int limit, int32_t* cursor);

/* Activate's choice, fed one entry at a time in descending order: the lowest empty entry, otherwise the farthest
 * animal that is farther than the new one (ties keep the first visited). */
typedef struct {
    float best;
    int empty, farthest;
} active_ai_pick_t;

void active_ai_pick_begin(active_ai_pick_t* p, float new_distance);
void active_ai_pick_slot(active_ai_pick_t* p, int index, int occupied, double distance);

/* Squared distance of a - b, each difference and product in double as the lifted x87 code computes it, summed in the
 * order the function at hand uses: (z + y) + x or (z + x) + y. */
double active_ai_dist2_zyx(const float a[3], const float b[3]);
double active_ai_dist2_zxy(const float a[3], const float b[3]);

/* x87 compare of st0 with src: C0 (below, or unordered) and C0|C3 (below or equal, or unordered). */
int active_ai_x87_c0(double st0, double src);
int active_ai_x87_c0c3(double st0, double src);

int active_ai_count_set(const uint32_t* values, int count);
/* CE's guard on an activation: the pool must hold the boxes in use, less the evicted animal's, plus the new one's. */
int active_ai_boxes_allow(int in_use, int adding, int removing, int pool);

/* The 10 values Save hands to the handle writer: entries 0..limit, then zeros. active has limit + 1 entries. */
void active_ai_save_values(const uint32_t* active, int limit, uint32_t out[ACTIVE_AI_SAVE_ENTRIES]);
/* How many leading save entries Load hands to Activate (the rest are read and dropped). */
int active_ai_load_activations(int limit);

/* Every function the Enhanced limit replaces, sorted. */
extern const uint32_t active_ai_override_vas[];
extern const unsigned active_ai_override_count;
int active_ai_overrides(uint32_t va);

#ifdef __cplusplus
}
#endif

#endif
