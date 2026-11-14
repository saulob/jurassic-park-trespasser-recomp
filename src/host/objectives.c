/*
 * Objectives follow CSetHintAction::iCurrentHint, the hint F1's CHintWnd shows: the level's triggers set it as the
 * player progresses and every CSetHintAction saves and loads it, so a loaded game resumes the same objective. Targets
 * and route anchors are instances of the level found by name (u4Hash, CWorld::pinsFindInstance) and kept as positions
 * only, resolved again whenever the objective, the level or a load changes.
 */
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "objective_policy.h"
#include "objectives.h"

#define CURRENT_HINT 0x0066F01Cu
#define GAME_INSTANCE 0x0066E05Cu
#define WORLD 0x0073F444u
#define U4_HASH 0x005F6100u
#define FIND_INSTANCE 0x00607C10u
#define NAME_BYTES 0x40u
#define LOG_LIMIT 64
#define TARGET_REFRESH_MS 1000u

static struct {
    char level[DM_LEVEL_ID_MAX];
    const ob_level_t* table;
    int current, hint, logs, quiet;
    uint32_t aimed;
    cp_goal_t goal;
    int text_hint;
    char text[DM_LINE_MAX];
} O = { .current = -1, .hint = -1, .text_hint = -1 };

static void note(const char* fmt, const char* a, int b) {
    if (O.logs >= LOG_LIMIT) return;
    if (++O.logs == LOG_LIMIT) host_log("objectives", "log limit reached");
    else host_log("objectives", fmt, a, b);
}

static uint32_t find_instance(const char* name, uint32_t scratch) {
    uint32_t world, args[3], handle = 0, instance = 0;
    size_t n = strlen(name) + 1;
    if (n > NAME_BYTES || !guest_read32(WORLD, &world) || !world) return 0;
    memcpy((void*)ADDR(scratch), name, n);
    args[0] = scratch;
    args[1] = 0;
    args[2] = 1;
    if (!guest_call(U4_HASH, 0, args, 3, 1, &handle) || !guest_call(FIND_INSTANCE, world, &handle, 1, 0, &instance))
        return 0;
    return instance;
}

static int locate(const char* name, uint32_t scratch, float out[3]) {
    dm_placement_t at;
    uint32_t instance = find_instance(name, scratch);
    if (!instance || !debug_menu_placement(instance, scratch + NAME_BYTES, &at) || !dm_teleport_valid(at.pos))
        return 0;
    memcpy(out, at.pos, sizeof at.pos);
    return 1;
}

/* Objects move (a keycard is carried): the target is found again every TARGET_REFRESH_MS; triggers simply stay put. */
static void aim(const dm_world_t* w, uint32_t scratch) {
    const ob_objective_t* o = &O.table->items[O.current];
    float pos[OB_NAMES_MAX][3];
    int found[OB_NAMES_MAX] = { 0 }, best;
    for (int i = 0; i < OB_NAMES_MAX && o->target[i]; i++) found[i] = locate(o->target[i], scratch, pos[i]);
    best = ob_nearest(pos, found, OB_NAMES_MAX, w->at.pos);
    O.aimed = GetTickCount();
    O.goal.target = best >= 0;
    if (best < 0) {
        if (o->target[0]) note("%s: target not found, no marker", o->target[0], 0);
        return;
    }
    memcpy(O.goal.pos, pos[best], sizeof O.goal.pos);
    if (o->carried && w->player && cp_distance_m(w->at.pos, pos[best]) <= OB_CARRIED_M &&
        locate(o->carried, scratch, pos[best]))
        memcpy(O.goal.pos, pos[best], sizeof O.goal.pos);
}

static void resolve(const dm_world_t* w, uint32_t scratch) {
    const ob_objective_t* o = &O.table->items[O.current];
    memset(&O.goal, 0, sizeof O.goal);
    O.goal.set = 1;
    snprintf(O.goal.title, sizeof O.goal.title, "%s", o->title);
    aim(w, scratch);
    if (!O.goal.target) return;
    for (int i = 0; i < CP_ROUTE_MAX && o->route[i]; i++) {
        if (locate(o->route[i], scratch, O.goal.route[O.goal.route_count])) {
            O.goal.route_count++;
            continue;
        }
        note("%s: route anchor not found, no trail", o->route[i], 0);
        O.goal.route_count = 0;
        break;
    }
}

/* Positions only: a window over the level or a load may move or replace every instance. */
static void forget(void) {
    memset(&O.goal, 0, sizeof O.goal);
    O.quiet = 1;
}

static const char* hint_text(int hint) {
    HINSTANCE game = (HINSTANCE)(uintptr_t)MEM32(GAME_INSTANCE);
    if (hint != O.text_hint) {
        O.text_hint = hint;
        O.text[0] = 0;
        if (hint < 0 || !LoadStringA(game, OB_HINT_STRING_BASE + (UINT)hint, O.text, sizeof O.text)) O.text[0] = 0;
    }
    return O.text;
}

static void fill_list(dm_world_t* w) {
    w->goals.available = O.table != NULL;
    w->goals.count = 0;
    if (!O.table) return;
    for (int i = 0; i < O.table->count && i < DM_GOALS_MAX; i++) {
        w->goals.item[i].status = ob_status(i, O.current);
        snprintf(w->goals.item[i].title, DM_LINE_MAX, "%s", O.table->items[i].title);
        w->goals.item[i].hint[0] = 0;
        if (i == O.current && O.table->items[i].hint == O.hint)
            snprintf(w->goals.item[i].hint, DM_LINE_MAX, "%s", hint_text(O.hint));
        w->goals.count++;
    }
}

const char* objectives_frame(dm_world_t* w, int list, uint32_t scratch) {
    uint32_t hint;
    int current, moved;
    if (w->place != DM_PLACE_LEVEL || !w->level[0]) {
        if (w->place != DM_PLACE_LEVEL) forget();
        if (w->place == DM_PLACE_MAIN_MENU) O.level[0] = 0;
        return NULL;
    }
    if (strcmp(w->level, O.level)) {
        snprintf(O.level, sizeof O.level, "%s", w->level);
        O.table = ob_level(w->level);
        forget();
        O.current = O.hint = -1;
        note("level %s: %d objectives (-1: none known)", w->level, O.table ? O.table->count : -1);
    }
    if (!O.table) {
        if (list) fill_list(w);
        return NULL;
    }
    if (!guest_read32(CURRENT_HINT, &hint)) return NULL;
    current = ob_current(O.table, (int)hint, O.current);
    moved = O.current >= 0 && current != O.current && !O.quiet;
    if (current != O.current) note("current: %s (hint %d)", O.table->items[current].title, (int)hint);
    if (current != O.current || !O.goal.set) {
        O.current = current;
        resolve(w, scratch);
    } else if (O.table->items[current].target[0] && GetTickCount() - O.aimed >= TARGET_REFRESH_MS)
        aim(w, scratch);
    O.hint = (int)hint;
    O.quiet = 0;
    if (list) fill_list(w);
    return moved ? "Objective updated" : NULL;
}

const cp_goal_t* objectives_goal(void) { return O.goal.set ? &O.goal : NULL; }
