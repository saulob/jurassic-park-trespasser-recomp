#include "objective_policy.h"

#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Each target is the trigger that sets the next hint, the level's exit, or the object the next hint's trigger waits on;
 * route anchors are the level's own Teleport points. tests/test_objectives_retail.py checks every name against the
 * retail level data. */
static const ob_objective_t k_be[] = {
    { 100, "Find out where you are", { "TrigHint101-00" }, NULL, { NULL } },
    { 101, "Search the construction site for clues", { "TrigHint102-00" }, NULL, { NULL } },
    { 102, "Head inland", { "Trig_Level_Bch-00", "Trig_Level_Bch-01" }, NULL,
      { "Teleport01", "Teleport02", "Teleport_start", NULL } },
};
static const ob_objective_t k_jr[] = {
    { 200, "Follow the monorail tracks", { "TrigHint201-00" }, NULL,
      { "Teleport_Area01", "Teleport_Area02", "Teleport_Area03", "TeleportA", "Teleport_Superjumpll", NULL } },
    { 201, "Explore, then return to the monorail", { "TrigHint202-00" }, NULL,
      { "Teleport_Area05", "Teleport_Area06", "Teleport_Monorailt", NULL } },
    { 202, "Cross the ridge on the monorail tracks", { "Trig_Level_JR-00" }, NULL, { NULL } },
};
static const ob_objective_t k_ij[] = {
    { 300, "Push on through the jungle", { "TrigHint301-00" }, NULL,
      { "Teleport_Pond", "Teleport_TRexEntrance", "Teleport_CanyonEntrance", "Teleport_TopOfRidgeClimb", NULL } },
    { 301, "Find a way down", { "TrigHint302-00" }, NULL, { NULL } },
    { 302, "Follow the power lines", { "Trig_LevelIJ-00" }, NULL, { "Teleport_NearEnd", "Teleport_End", NULL } },
};
static const ob_objective_t k_it[] = {
    { 400, "Find a building with a line out", { "TrigHint401-00" }, NULL, { NULL } },
    { 401, "Search the Operations Center", { "TrigHint402-00" }, NULL, { NULL } },
    { 402, "Use the control console", { "TrigGateOpen-00" }, NULL, { NULL } },
    { 403, "Go to Hammond's place", { "TrigHint404-00" }, NULL, { NULL } },
    { 404, "Leave by the northeastern gate", { "Trig_Level_IT-00" }, NULL,
      { "TeleportPlainsMidPoint-00", "TeleportDam-00", NULL } },
};
static const ob_objective_t k_lab[] = {
    { 600, "Find a way through to the computer lab", { "TrigHint601-00" }, NULL, { NULL } },
    { 601, "Find a green keycard", { "PCardSctyGRN00-01", "PCardSctyGRN00-02" }, "FCrdRderGRN00-00", { NULL } },
    { 603, "Search the offices for a keycard", { "PCardSctyGRN00-01", "PCardSctyGRN00-02" }, "FCrdRderGRN00-00",
      { NULL } },
    { 604, "Get to the computer lab", { "TrigHint605-00" }, NULL, { "Teleport_RoadMidPoint", "Teleport_Lab", NULL } },
    { 605, "Start up the computer", { "TrigHint606-00" }, NULL, { NULL } },
    { 606, "Head for the mountaintop", { "Trig_Level_IL-00" }, NULL, { "Teleport_Exit", NULL } },
};
static const ob_objective_t k_as[] = {
    { 700, "Circle around to find a way up", { "TrigHint701-00" }, NULL, { NULL } },
    { 701, "Climb up through the ruins", { "TrigLevelChange-00" }, NULL,
      { "Teleport_Stairs", "Teleport_City", "Teleport_End", NULL } },
};
static const ob_objective_t k_as2[] = {
    { 800, "Find a place to climb", { "TrigHint801-00" }, NULL, { NULL } },
    { 801, "Climb the cliffs", { "Trig_Level_AS2-00" }, NULL, { "Teleport0-00", "Teleport3-00", "Teleport1-00", NULL } },
};
static const ob_objective_t k_sum[] = {
    { 900, "Explore the summit facility", { "PEmrgncyBoxBttn01-00" }, NULL, { NULL } },
    { 901, "Race to meet the helicopter", { "TrigEndGame-00" }, NULL, { "TeleportDestRamp-00", NULL } },
};

static const ob_level_t k_levels[] = {
    { "BE", k_be, COUNT(k_be) },     { "JR", k_jr, COUNT(k_jr) },     { "IJ", k_ij, COUNT(k_ij) },
    { "IT", k_it, COUNT(k_it) },     { "LAB", k_lab, COUNT(k_lab) },  { "AS", k_as, COUNT(k_as) },
    { "AS2", k_as2, COUNT(k_as2) },  { "SUM", k_sum, COUNT(k_sum) },
};

const ob_level_t* ob_level(const char* level_id) {
    if (!level_id) return NULL;
    for (int i = 0; i < COUNT(k_levels); i++)
        if (!strcmp(k_levels[i].level, level_id)) return &k_levels[i];
    return NULL;
}

int ob_level_count(void) { return COUNT(k_levels); }

const ob_level_t* ob_level_at(int index) { return index >= 0 && index < COUNT(k_levels) ? &k_levels[index] : NULL; }

int ob_index_of(const ob_level_t* l, int hint) {
    for (int i = 0; l && i < l->count; i++)
        if (l->items[i].hint == hint) return i;
    return -1;
}

int ob_current(const ob_level_t* l, int hint, int held) {
    int i = ob_index_of(l, hint);
    if (!l || !l->count) return -1;
    if (i >= 0) return i;
    return held >= 0 && held < l->count ? held : 0;
}

dm_goal_status_t ob_status(int index, int current) {
    return index < current ? DM_GOAL_DONE : index == current ? DM_GOAL_CURRENT : DM_GOAL_UNKNOWN;
}

int ob_nearest(const float pos[][3], const int found[], int count, const float at[3]) {
    int best = -1;
    float d = 0.0f;
    for (int i = 0; i < count; i++) {
        float e;
        if (!found[i]) continue;
        e = cp_distance_m(at, pos[i]);
        if (best < 0 || e < d) {
            best = i;
            d = e;
        }
    }
    return best;
}
