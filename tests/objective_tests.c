/* The objectives' policy, headless: HintID to objective, statuses, levels without objectives, target choice. The table's
 * names are checked against the retail levels by tests/test_objectives_retail.py. */
#include <stdio.h>
#include <string.h>

#include "objective_policy.h"

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

static void test_tables(void) {
    static const char* const campaign[] = { "BE", "JR", "IJ", "IT", "LAB", "AS", "AS2", "SUM" };
    static const int family[] = { 1, 2, 3, 4, 6, 7, 8, 9 };
    CHECK(ob_level_count() == 8, "the eight campaign levels");
    for (int i = 0; i < 8; i++) {
        const ob_level_t* l = ob_level(campaign[i]);
        int ordered = 1, families = 1, named = 1;
        CHECK(l && l == ob_level_at(i) && l->count >= 2, "%s has objectives", campaign[i]);
        if (!l) continue;
        for (int j = 0; j < l->count; j++) {
            const ob_objective_t* o = &l->items[j];
            if (j && o->hint <= l->items[j - 1].hint) ordered = 0;
            if (o->hint / 100 != family[i]) families = 0;
            if (!o->title || !o->title[0] || !o->target[0]) named = 0;
            if (o->route[0] && !o->target[0]) named = 0;
        }
        CHECK(ordered && families && named, "%s: hints in order, in the level's family, each with a title and a target",
              campaign[i]);
    }
    CHECK(!ob_level("TESTSCENE") && !ob_level("MYLEVEL") && !ob_level("") && !ob_level(NULL),
          "test scenes and custom levels have none");
    CHECK(ob_index_of(ob_level("LAB"), 602) == -1 && ob_index_of(ob_level("LAB"), 603) == 2,
          "LAB has no hint 602 (retail never sets it)");
}

static void test_current(void) {
    const ob_level_t* be = ob_level("BE");
    const ob_level_t* it = ob_level("IT");
    CHECK(ob_current(be, 100, -1) == 0 && ob_current(be, 101, 0) == 1 && ob_current(be, 102, 1) == 2,
          "each BE hint is its objective");
    CHECK(ob_current(be, 102, -1) == 2, "a save loaded at hint 102 resumes there, whatever came before");
    CHECK(ob_current(it, 403, -1) == 3, "IT: the gate latch's hint 403");
    CHECK(ob_current(ob_level("JR"), 102, -1) == 0, "entering JR with BE's hint: its first objective until 200 is set");
    CHECK(ob_current(be, 1290, 1) == 1, "a hint the level does not map keeps the objective held");
    CHECK(ob_current(be, 0, -1) == 0, "no hint yet: the first");
    CHECK(ob_current(be, 100, 2) == 0, "the game set an earlier hint: the objective follows it, as F1 does");
    CHECK(ob_current(NULL, 100, 0) == -1, "no table: no objective");
    CHECK(ob_status(0, 1) == DM_GOAL_DONE && ob_status(1, 1) == DM_GOAL_CURRENT && ob_status(2, 1) == DM_GOAL_UNKNOWN,
          "Completed, Current, then not revealed");
}

static void test_nearest(void) {
    const float pos[2][3] = { { 0, 100, 0 }, { 0, 20, 0 } };
    const float at[3] = { 0, 0, 0 };
    int both[2] = { 1, 1 }, first[2] = { 1, 0 }, none[2] = { 0, 0 };
    CHECK(ob_nearest(pos, both, 2, at) == 1, "the nearer of two targets");
    CHECK(ob_nearest(pos, first, 2, at) == 0, "only the one found");
    CHECK(ob_nearest(pos, none, 2, at) == -1, "none found: no target");
    {
        const ob_objective_t* card = &ob_level("LAB")->items[1];
        CHECK(card->target[0] && card->target[1] && card->carried && OB_CARRIED_M > 1.0f,
              "the green keycard: either card, then its reader once Anne carries it");
    }
}

void objective_tests(int* checks, int* failures) {
    test_tables();
    test_current();
    test_nearest();
    *checks += g_checks;
    *failures += g_failures;
}
