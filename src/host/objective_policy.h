/* The campaign's objectives without the guest: each is one of the game's own hints (CSetHintAction HintID, the text
 * F1 shows) and the instance of the level that hint leads to, with the real anchors on the way. */
#ifndef OBJECTIVE_POLICY_H
#define OBJECTIVE_POLICY_H

#include "compass_policy.h"
#include "debug_menu_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OB_HINT_STRING_BASE 18000 /* IDS_STR_HINTS: CHintWnd loads this + iCurrentHint */

#define OB_NAMES_MAX 2
#define OB_CARRIED_M 2.5f

typedef struct {
    int hint;
    const char* title;
    const char* target[OB_NAMES_MAX]; /* instance names of the level, the nearest to Anne wins; none: no target */
    const char* carried;              /* where to point once the target is in Anne's hands; NULL: never */
    const char* route[CP_ROUTE_MAX];  /* anchor instance names in order, NULL-terminated; empty: no trail */
} ob_objective_t;

typedef struct {
    const char* level; /* the level id, "BE" */
    const ob_objective_t* items;
    int count;
} ob_level_t;

const ob_level_t* ob_level(const char* level_id);
int ob_level_count(void);
const ob_level_t* ob_level_at(int index);
int ob_index_of(const ob_level_t* l, int hint);
/* The current objective's index: the hint's own, else the one held (a hint of another level or the tutorial keeps the
 * previous objective), else the first. -1 for a level without objectives. */
int ob_current(const ob_level_t* l, int hint, int held);
dm_goal_status_t ob_status(int index, int current);
/* Of the target's positions found (found[i] set), the nearest to Anne; -1 when none was found. */
int ob_nearest(const float pos[][3], const int found[], int count, const float at[3]);

#ifdef __cplusplus
}
#endif

#endif
