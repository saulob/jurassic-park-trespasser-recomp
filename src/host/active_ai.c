/*
 * Active AI (Trespasser CE's MaxActiveAi): CAISystem keeps up to 6 animals active instead of 3. Entries 1..3 stay in
 * the game's arrays (+0x3188 animals, +0x31A0 slots); entries 4..6 live here, their slots allocated by the game's own
 * operator new and slot constructor. Every function that walks those arrays is replaced with the retail logic over
 * 1..6, in the retail order; Activate also refuses an animal the physics box pool could not hold (CE's guard, with the
 * retail pool). Original installs nothing.
 */
#include "guest_va.h"
#include "host.h"

#include <math.h>
#include <string.h>

#include "active_ai_policy.h"
#include "minimap.h"

#define AI_SYSTEM GVA(0x0066E77C)
#define PLAYER GVA(0x0066EF64)
#define THINK_CURSOR GVA(0x00661570)
#define AI_HALTED GVA(0x006D0E99)
#define SAVE_VERSION GVA(0x00665130)
#define PHYSICS_ACTIVE_ARG GVA(0x0067A088)
#define BRAIN_RESET_ARG GVA(0x00661758)
#define NODE_FREE GVA(0x0066E794)
#define NODE_NEXT GVA(0x0066E790)
#define NODE_END GVA(0x0066E78C)
#define NODE_BYTES 0xCu
#define BOX_IN_USE GVA(0x0069A138)
#define BOX_STRIDE GLAYOUT_BOX_POOL_STRIDE

#define CTOR_VA GVA(0x00419330)
#define DTOR_VA GVA(0x004195A0)
#define ACTIVATE_VA GVA(0x00419800)
#define DEACTIVATE_VA GVA(0x004199D0)
#define DEACTIVATE_WITHIN_VA GVA(0x00419C60)
#define SEEN_BY_NEAREST_VA GVA(0x00419CE0)
#define HEARD_BY_NEAR_VA GVA(0x00419DB0)
#define THINK_NEXT_VA GVA(0x00419E30)
#define PROCESS_VA GVA(0x0041A200)
#define SAVE_VA GVA(0x0041AE50)
#define LOAD_VA GVA(0x0041AF00)
#define INFLUENCES_VA GVA(0x004213F0)
#define INFLUENCES_TAIL_VA GVA(0x004218C0)

#define OPERATOR_NEW GVA(0x00626B8C)
#define SLOT_CTOR GVA(0x0041CE10)
#define SLOT_BYTES 0x40u
#define SLOT_NODES 0x32u
#define NODE_GROW GVA(0x0041BC00)
#define BRAIN_ACTIVATE GVA(0x00421080)
#define BRAIN_DEACTIVATE GVA(0x004211B0)
#define BRAIN_RESET GVA(0x004233D0)
#define BRAIN_THINK GVA(0x00421220)
#define BRAIN_SEE GVA(0x00424AB0)
#define BRAIN_HEAR GVA(0x00422650)
#define THINK_TICK GVA(0x0041A520)
#define PHYSICS_OFF GVA(0x00614A60)
#define WRITE_HANDLE GVA(0x00608150)
#define READ_HANDLE GVA(0x00608170)

#define SYS_ENABLED 0x50u
#define SYS_HALT_A 0x51u
#define SYS_HALT_B 0x52u
#define SYS_THINK_INTERVAL 0x54u
#define SYS_LAST_THINK 0x58u
#define SYS_SAVED_PART 0x60u
#define SYS_TIME 0x3178u
#define SYS_THINK_COST 0x3180u
#define SYS_ACTIVE 0x3188u
#define SYS_LIST 0x3198u
#define SYS_LIST_COUNT 0x319Cu
#define SYS_SLOTS 0x31A0u
#define SYS_THINK_DUE 0x31CCu

#define ANIMAL_PHYSICS 0x28u
#define PHYSICS_INFO 0xCu
#define ANIMAL_PARTS 0xACu
#define ANIMAL_BRAIN 0xDCu
#define BRAIN_INFLUENCE_LIST 0x8u
#define BRAIN_SLOT 0xCu
#define BRAIN_TIMER 0x128u
#define BRAIN_TIMER_VALUE 0x1ACu
#define BRAIN_SEEN 0x33Cu
#define BRAIN_WAKE_RADIUS 0x340u
#define BRAIN_KEEP_RADIUS 0x344u
#define SLOT_ANIMAL 0x38u

#define VF_SAVE 0x0u
#define VF_LOAD 0x4u
#define VF_PHYSICS_ON 0x18u
#define VF_POSITION 0x20u
#define VF_DEAD 0x1FCu
#define VF_RELEASE 0x200u
#define VF_TIMER_RESET 0x8u
#define VF_DELETE 0x0u

#define FAR_RADIUS 100000.0f
#define SCRATCH 0x40u
#define EXTRA (ACTIVE_AI_MAX_LIMIT + 1 - ACTIVE_AI_RETAIL_ENTRIES)
#define SYSTEMS 2

/* The frame of 0x004213F0 seen from 0x004218C0's entry: its locals sit 4 bytes up (the return slot). */
#define INFLUENCES_FRAME 0x64u
#define INFLUENCES_POINT 0x1Cu
#define INFLUENCES_RADIUS2 0xCu

typedef struct {
    uint32_t sys;
    uint32_t active[EXTRA], slot[EXTRA];
} extra_t;

static struct {
    int enhanced, installed, guard_logged;
    extra_t extra[SYSTEMS];
    uint32_t think_brain, think_esp;
    recomp_func_t ctor, dtor, activate, deactivate, within, seen, heard, think_next, process, save, load, influences,
        influences_tail;
} A;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

static void read_vec(uint32_t va, float v[3]) {
    for (int i = 0; i < 3; i++) v[i] = va ? rdf(va + 4u * (uint32_t)i) : 0.0f;
}

static extra_t* extra_of(uint32_t sys) {
    for (int i = 0; i < SYSTEMS; i++)
        if (sys && A.extra[i].sys == sys) return &A.extra[i];
    return NULL;
}

static int limit_of(uint32_t sys) { return extra_of(sys) ? ACTIVE_AI_ENHANCED_LIMIT : ACTIVE_AI_ORIGINAL_LIMIT; }

static uint32_t active_at(uint32_t sys, int i) {
    extra_t* e;
    if (i < ACTIVE_AI_RETAIL_ENTRIES) return MEM32(sys + SYS_ACTIVE + 4u * (uint32_t)i);
    e = extra_of(sys);
    return e ? e->active[i - ACTIVE_AI_RETAIL_ENTRIES] : 0;
}

static void set_active(uint32_t sys, int i, uint32_t animal) {
    extra_t* e;
    if (i < ACTIVE_AI_RETAIL_ENTRIES) {
        MEM32(sys + SYS_ACTIVE + 4u * (uint32_t)i) = animal;
        return;
    }
    if ((e = extra_of(sys)) != NULL) e->active[i - ACTIVE_AI_RETAIL_ENTRIES] = animal;
}

static uint32_t slot_at(uint32_t sys, int i) {
    extra_t* e;
    if (i < ACTIVE_AI_RETAIL_ENTRIES) return MEM32(sys + SYS_SLOTS + 4u * (uint32_t)i);
    e = extra_of(sys);
    return e ? e->slot[i - ACTIVE_AI_RETAIL_ENTRIES] : 0;
}

static uint32_t call0(uint32_t va, uint32_t self) {
    uint32_t r = 0;
    guest_call(va, self, NULL, 0, 0, &r);
    return r;
}

static uint32_t call1(uint32_t va, uint32_t self, uint32_t arg) {
    uint32_t r = 0;
    guest_call(va, self, &arg, 1, 0, &r);
    return r;
}

static uint32_t cdecl2(uint32_t va, uint32_t a, uint32_t b) {
    uint32_t args[2] = { a, b }, r = 0;
    guest_call(va, 0, args, 2, 1, &r);
    return r;
}

static int dead(uint32_t animal) {
    uint32_t r = 0;
    guest_call_virtual(animal, VF_DEAD, NULL, 0, &r);
    return (r & 0xFFu) != 0;
}

/* The object's position: vfn 0x20 fills buf and returns the vector it reports. */
static uint32_t position(uint32_t object, uint32_t buf) {
    uint32_t r = 0;
    guest_call_virtual(object, VF_POSITION, &buf, 1, &r);
    return r;
}

static float s_player[3];
static int s_player_ready;

static float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static uint32_t brain_of(uint32_t animal);

static int note_animal(uint32_t animal, int active, uint32_t scratch, host_animal_t* out, int n) {
    float at[3];
    if (!animal || !guest_readable(animal, ANIMAL_BRAIN + 4u) || dead(animal)) return n;
    read_vec(position(animal, scratch), at);
    out[n].x = at[0];
    out[n].y = at[1];
    out[n].engaged = minimap_creature_engaged(active, s_player_ready,
                                              dist3(at, s_player), rdf(brain_of(animal) + BRAIN_KEEP_RADIUS));
    return n + 1;
}

int active_ai_animals(uint32_t scratch, host_animal_t* out, int max) {
    uint32_t sys = MEM32(AI_SYSTEM), head, node, player_va;
    int n = 0, guard = 0;
    if (!sys || !guest_readable(sys, SYS_THINK_DUE + 4u) || !out) return 0;
    player_va = MEM32(PLAYER);
    s_player_ready = 0;
    if (player_va && guest_readable(player_va, 8u)) {
        read_vec(position(player_va, scratch + 0x20u), s_player);
        s_player_ready = 1;
    }
    for (int i = 1; i <= limit_of(sys) && n < max; i++) n = note_animal(active_at(sys, i), 1, scratch, out, n);
    head = MEM32(sys + SYS_LIST);
    if (!head || !guest_readable(head, 12u)) return n;
    for (node = MEM32(head); node != head && node && guard < 64 && n < max; node = MEM32(node), guard++) {
        if (!guest_readable(node, 12u)) break;
        n = note_animal(MEM32(node + 8u), 0, scratch, out, n);
    }
    return n;
}

static uint32_t brain_of(uint32_t animal) { return MEM32(animal + ANIMAL_BRAIN); }

static int halted(uint32_t sys) { return MEM8(sys + SYS_HALT_A) || MEM8(sys + SYS_HALT_B); }

static int parts_of(uint32_t animal) {
    uint32_t parts[ACTIVE_AI_ANIMAL_PARTS];
    if (!animal) return 0;
    for (uint32_t i = 0; i < ACTIVE_AI_ANIMAL_PARTS; i++) parts[i] = MEM32(animal + ANIMAL_PARTS + 4u * i);
    return active_ai_count_set(parts, ACTIVE_AI_ANIMAL_PARTS);
}

/* Boxes in use, as 0x004CEF50 counts them. */
static int boxes_in_use(void) {
    uint32_t flags[ACTIVE_AI_BOX_POOL];
    for (uint32_t i = 0; i < ACTIVE_AI_BOX_POOL; i++) flags[i] = MEM32(BOX_IN_USE + BOX_STRIDE * i);
    return active_ai_count_set(flags, ACTIVE_AI_BOX_POOL);
}

static int boxes_allow(uint32_t animal, uint32_t evicted) {
    int in_use = boxes_in_use(), adding = parts_of(animal), removing = parts_of(evicted);
    if (active_ai_boxes_allow(in_use, adding, removing, ACTIVE_AI_BOX_POOL)) return 1;
    if (!A.guard_logged++)
        host_log("ce", "active AI: an animal was left inactive, the physics could not hold it (%d of %d boxes in use, "
                 "it needs %d%s)", in_use, ACTIVE_AI_BOX_POOL, adding, evicted ? " less the one it would replace" : "");
    return 0;
}

static void deactivate(uint32_t sys, uint32_t animal) {
    int limit = limit_of(sys), i;
    uint32_t head, node, cur, slot;
    for (i = limit; i >= 1; i--)
        if (active_at(sys, i) == animal) break;
    if (i < 1) return;
    set_active(sys, i, 0);
    head = MEM32(sys + SYS_LIST);
    node = MEM32(NODE_FREE);
    if (node) {
        MEM32(NODE_FREE) = MEM32(node);
    } else {
        cur = MEM32(NODE_NEXT);
        if (cur == MEM32(NODE_END)) {
            call0(NODE_GROW, sys + SYS_LIST);
            cur = MEM32(NODE_NEXT);
        }
        node = cur;
        MEM32(NODE_NEXT) = cur + NODE_BYTES;
    }
    if (node + 8u) MEM32(node + 8u) = animal;
    MEM32(node) = head;
    MEM32(node + 4u) = MEM32(head + 4u);
    MEM32(MEM32(head + 4u)) = node;
    MEM32(head + 4u) = node;
    MEM32(sys + SYS_LIST_COUNT)++;
    slot = slot_at(sys, i);
    MEM32(slot + SLOT_ANIMAL) = 0;
    MEM32(brain_of(animal) + BRAIN_SLOT) = 0;
    call0(BRAIN_DEACTIVATE, brain_of(animal));
}

static void place(uint32_t sys, int i, uint32_t animal) {
    uint32_t head = MEM32(sys + SYS_LIST), node;
    set_active(sys, i, animal);
    for (node = MEM32(head); node != head; node = MEM32(node)) {
        if (MEM32(node + 8u) != animal) continue;
        MEM32(MEM32(node + 4u)) = MEM32(node);
        MEM32(MEM32(node) + 4u) = MEM32(node + 4u);
        MEM32(node) = MEM32(NODE_FREE);
        MEM32(NODE_FREE) = node;
        MEM32(sys + SYS_LIST_COUNT)--;
        break;
    }
    MEM32(slot_at(sys, i) + SLOT_ANIMAL) = animal;
    MEM32(brain_of(animal) + BRAIN_SLOT) = slot_at(sys, i);
    call0(BRAIN_ACTIVATE, brain_of(animal));
}

static void activate(uint32_t sys, uint32_t animal) {
    uint32_t buf, pa, pp;
    float a[3], p[3];
    active_ai_pick_t pick;
    if (dead(animal)) return;
    g_esp -= SCRATCH;
    buf = g_esp;
    for (;;) {
        int limit = limit_of(sys), i, found = 0;
        uint32_t victim;
        if (halted(sys)) break;
        pa = position(animal, buf);
        pp = position(MEM32(PLAYER), buf + 0x10u);
        read_vec(pa, a);
        read_vec(pp, p);
        active_ai_pick_begin(&pick, (float)active_ai_dist2_zyx(p, a));
        for (i = limit; i >= 1; i--) {
            uint32_t other = active_at(sys, i);
            if (other == animal) {
                found = 1;
                break;
            }
            if (!other) {
                active_ai_pick_slot(&pick, i, 0, 0.0);
                continue;
            }
            pa = position(other, buf + 0x20u);
            pp = position(MEM32(PLAYER), buf + 0x30u);
            read_vec(pa, a);
            read_vec(pp, p);
            active_ai_pick_slot(&pick, i, 1, active_ai_dist2_zyx(p, a));
        }
        if (found) break;
        if (pick.empty < 0) {
            if (pick.farthest < 0) break;
            victim = active_at(sys, pick.farthest);
            if (!boxes_allow(animal, victim)) break;
            deactivate(sys, victim);
            if (dead(animal)) break;
            continue;
        }
        if (boxes_allow(animal, 0)) place(sys, pick.empty, animal);
        break;
    }
    g_esp += SCRATCH;
}

static void deactivate_within(uint32_t sys, float x, float y, float z, float radius) {
    float point[3] = { x, y, z }, v[3], r2 = (float)((double)radius * (double)radius);
    int limit = limit_of(sys);
    uint32_t buf;
    g_esp -= SCRATCH;
    buf = g_esp;
    for (int i = limit; i >= 1; i--) {
        uint32_t animal = active_at(sys, i);
        if (!animal) continue;
        read_vec(position(animal, buf), v);
        if (active_ai_x87_c0(active_ai_dist2_zyx(v, point), (double)r2)) deactivate(sys, active_at(sys, i));
    }
    g_esp += SCRATCH;
}

static void process(uint32_t sys, uint32_t msg) {
    int limit = limit_of(sys);
    uint32_t buf, head, node;
    float a[3], p[3], t;
    double sum;
    if (!MEM8(sys + SYS_ENABLED)) return;
    if (halted(sys)) deactivate_within(sys, 0.0f, 0.0f, 0.0f, FAR_RADIUS);
    g_esp -= SCRATCH;
    buf = g_esp;
    for (int i = limit; i >= 1; i--) {
        uint32_t animal = active_at(sys, i), release = 0;
        if (!animal) continue;
        guest_call_virtual(animal, VF_RELEASE, NULL, 0, &release);
        if (!(release & 0xFFu)) {
            uint32_t pp = position(MEM32(PLAYER), buf), pa = position(animal, buf + 0x10u);
            double r = (double)rdf(brain_of(animal) + BRAIN_KEEP_RADIUS);
            read_vec(pp, p);
            read_vec(pa, a);
            if (!active_ai_x87_c0(r * r, active_ai_dist2_zyx(a, p))) continue;
        }
        deactivate(sys, animal);
        call1(BRAIN_RESET, brain_of(animal), BRAIN_RESET_ARG);
    }
    t = rdf(msg + 0x14u);
    *(float*)ADDR(sys + SYS_TIME) = t;
    sum = (double)rdf(sys + SYS_LAST_THINK) + (double)rdf(sys + SYS_THINK_INTERVAL);
    if (!active_ai_x87_c0c3((double)t, sum)) {
        MEM8(sys + SYS_THINK_DUE) = 1;
        if (!MEM8(AI_HALTED)) call0(THINK_TICK, sys);
    }
    for (int i = limit; i >= 1; i--) {
        uint32_t animal = active_at(sys, i), args[3], brain;
        if (!animal) continue;
        args[0] = animal;
        args[1] = 0;
        args[2] = PHYSICS_ACTIVE_ARG;
        guest_call_virtual(MEM32(MEM32(animal + ANIMAL_PHYSICS) + PHYSICS_INFO), VF_PHYSICS_ON, args, 3, NULL);
        brain = brain_of(active_at(sys, i));
        guest_call_virtual(brain + BRAIN_TIMER, VF_TIMER_RESET, NULL, 0, NULL);
        MEM32(brain + BRAIN_TIMER_VALUE) = 0;
    }
    head = MEM32(sys + SYS_LIST);
    for (node = MEM32(head); node != MEM32(sys + SYS_LIST); node = MEM32(node)) {
        uint32_t animal = MEM32(node + 8u);
        if (!halted(sys) && !dead(animal)) {
            uint32_t pp = position(MEM32(PLAYER), buf), pa = position(animal, buf + 0x10u);
            double r = (double)rdf(brain_of(animal) + BRAIN_WAKE_RADIUS);
            read_vec(pp, p);
            read_vec(pa, a);
            if (!active_ai_x87_c0c3(r * r, active_ai_dist2_zyx(a, p))) continue;
        }
        if (!dead(animal)) call0(PHYSICS_OFF, animal);
    }
    head = MEM32(sys + SYS_LIST);
    for (node = MEM32(head); node != MEM32(sys + SYS_LIST); node = MEM32(node)) {
        uint32_t animal = MEM32(node + 8u);
        if (!halted(sys) && !dead(animal)) {
            uint32_t pp = position(MEM32(PLAYER), buf), pa = position(animal, buf + 0x10u);
            double r = (double)rdf(brain_of(animal) + BRAIN_WAKE_RADIUS);
            read_vec(pp, p);
            read_vec(pa, a);
            if (!active_ai_x87_c0c3(r * r, active_ai_dist2_zyx(a, p))) {
                g_esp += SCRATCH;
                activate(sys, animal);
                return;
            }
        }
        dead(animal);
        MEM8(brain_of(animal) + BRAIN_SEEN) = 0;
    }
    g_esp += SCRATCH;
}

/* Overrides: the stable entry is reached with g_esp at the return slot; each emulates its retail `ret n`. */

static void ai_ctor(void) {
    uint32_t sys = g_ecx, eax;
    extra_t* e = NULL;
    if (g_alias_entry) {
        A.ctor();
        return;
    }
    A.ctor();
    eax = g_eax;
    for (int i = 0; i < SYSTEMS && !e; i++)
        if (!A.extra[i].sys || A.extra[i].sys == sys) e = &A.extra[i];
    if (!e) {
        host_log("ce", "active AI: a second AI system kept the original limit");
        return;
    }
    memset(e, 0, sizeof *e);
    for (int i = EXTRA - 1; i >= 0; i--) {
        uint32_t size = SLOT_BYTES, mem = 0, slot = 0, args[2] = { 0, SLOT_NODES };
        guest_call(OPERATOR_NEW, 0, &size, 1, 1, &mem);
        if (mem) guest_call(SLOT_CTOR, mem, args, 2, 0, &slot);
        e->slot[i] = slot;
    }
    e->sys = sys;
    g_eax = eax;
}

static void ai_dtor(void) {
    uint32_t sys = g_ecx;
    extra_t* e;
    if (!g_alias_entry && (e = extra_of(sys)) != NULL) {
        uint32_t one = 1;
        for (int i = EXTRA - 1; i >= 0; i--)
            if (e->slot[i]) guest_call_virtual(e->slot[i], VF_DELETE, &one, 1, NULL);
        memset(e, 0, sizeof *e);
        g_ecx = sys;
    }
    A.dtor();
}

static void ai_activate(void) {
    if (g_alias_entry) {
        A.activate();
        return;
    }
    activate(g_ecx, GUEST_ARG(0));
    GUEST_RETURN_STDCALL(1);
}

static void ai_deactivate(void) {
    if (g_alias_entry) {
        A.deactivate();
        return;
    }
    deactivate(g_ecx, GUEST_ARG(0));
    GUEST_RETURN_STDCALL(1);
}

static void ai_deactivate_within(void) {
    if (g_alias_entry) {
        A.within();
        return;
    }
    deactivate_within(g_ecx, rdf(g_esp + 4u), rdf(g_esp + 8u), rdf(g_esp + 12u), rdf(g_esp + 16u));
    GUEST_RETURN_STDCALL(4);
}

/* (object, radius): the first active animal, from the top entry down, within radius of the object sees it. */
static void ai_seen_by_nearest(void) {
    uint32_t sys = g_ecx, object = GUEST_ARG(0), buf;
    double r2;
    float o[3], v[3];
    if (g_alias_entry) {
        A.seen();
        return;
    }
    r2 = (double)rdf(g_esp + 8u) * (double)rdf(g_esp + 8u);
    g_esp -= SCRATCH;
    buf = g_esp;
    read_vec(position(object, buf), o);
    for (int i = limit_of(sys); i >= 1; i--) {
        uint32_t animal = active_at(sys, i), args[2];
        if (!animal) continue;
        read_vec(position(animal, buf + 0x10u), v);
        if (active_ai_x87_c0c3(r2, active_ai_dist2_zyx(v, o))) continue;
        args[0] = object;
        args[1] = MEM32(MEM32(AI_SYSTEM) + SYS_TIME);
        guest_call(BRAIN_SEE, MEM32(brain_of(active_at(sys, i)) + BRAIN_INFLUENCE_LIST), args, 2, 0, NULL);
        break;
    }
    g_esp += SCRATCH;
    GUEST_RETURN_STDCALL(2);
}

/* (point, radius): every active animal within radius of the point hears it. */
static void ai_heard_by_near(void) {
    uint32_t sys = g_ecx, point = GUEST_ARG(0), buf;
    double r2;
    float pt[3], v[3];
    if (g_alias_entry) {
        A.heard();
        return;
    }
    r2 = (double)rdf(g_esp + 8u) * (double)rdf(g_esp + 8u);
    g_esp -= SCRATCH;
    buf = g_esp;
    for (int i = limit_of(sys); i >= 1; i--) {
        uint32_t animal = active_at(sys, i);
        if (!animal) continue;
        read_vec(position(animal, buf), v);
        read_vec(point, pt);
        if (active_ai_x87_c0c3(r2, active_ai_dist2_zxy(v, pt))) continue;
        call1(BRAIN_HEAR, brain_of(active_at(sys, i)), point);
    }
    g_esp += SCRATCH;
    GUEST_RETURN_STDCALL(2);
}

/* One animal thinks per AI tick, round robin over entries 0..limit (a tick past the last restarts the round). */
static void ai_think_next(void) {
    uint32_t sys = g_ecx, active[ACTIVE_AI_MAX_LIMIT + 1], r = 0;
    int32_t cursor;
    int limit, i;
    if (g_alias_entry) {
        A.think_next();
        return;
    }
    limit = limit_of(sys);
    for (i = 0; i <= limit; i++) active[i] = active_at(sys, i);
    cursor = (int32_t)MEM32(THINK_CURSOR);
    i = active_ai_think_next(active, limit, &cursor);
    MEM32(THINK_CURSOR) = (uint32_t)cursor;
    if (i >= 0) r = call0(BRAIN_THINK, brain_of(active[i]));
    g_eax = r;
    GUEST_RETURN_STDCALL(0);
}

static void ai_process(void) {
    if (g_alias_entry) {
        A.process();
        return;
    }
    process(g_ecx, GUEST_ARG(0));
    GUEST_RETURN_STDCALL(1);
}

static void ai_save(void) {
    uint32_t sys = g_ecx, out = GUEST_ARG(0), value, active[ACTIVE_AI_MAX_LIMIT + 1], values[ACTIVE_AI_SAVE_ENTRIES], head, node;
    int limit;
    if (g_alias_entry) {
        A.save();
        return;
    }
    limit = limit_of(sys);
    MEM32(out) = MEM8(sys + SYS_HALT_A);
    value = out + 4u;
    guest_call_virtual(sys + SYS_SAVED_PART, VF_SAVE, &value, 1, &out);
    MEM32(out) = MEM32(sys + SYS_TIME);
    MEM32(out + 4u) = MEM32(sys + SYS_LAST_THINK);
    MEM32(out + 8u) = MEM32(sys + SYS_THINK_COST);
    out += 12u;
    for (int i = 0; i <= limit; i++) active[i] = active_at(sys, i);
    active_ai_save_values(active, limit, values);
    for (int i = 0; i < ACTIVE_AI_SAVE_ENTRIES; i++) out = cdecl2(WRITE_HANDLE, out, values[i]);
    MEM32(out) = MEM32(sys + SYS_LIST_COUNT);
    out += 4u;
    head = MEM32(sys + SYS_LIST);
    for (node = MEM32(head); node != MEM32(sys + SYS_LIST); node = MEM32(node)) out = cdecl2(WRITE_HANDLE, out, MEM32(node + 8u));
    g_eax = out;
    GUEST_RETURN_STDCALL(1);
}

static void ai_load(void) {
    uint32_t sys = g_ecx, in = GUEST_ARG(0), value;
    int32_t count;
    int activations;
    if (g_alias_entry) {
        A.load();
        return;
    }
    activations = active_ai_load_activations(limit_of(sys));
    g_esp -= SCRATCH;
    value = g_esp;
    if ((int32_t)MEM32(SAVE_VERSION) >= 13) {
        MEM8(sys + SYS_HALT_A) = (uint8_t)(MEM32(in) != 0);
        in += 4u;
    }
    {
        uint32_t from = in;
        guest_call_virtual(sys + SYS_SAVED_PART, VF_LOAD, &from, 1, &in);
    }
    MEM32(sys + SYS_TIME) = MEM32(in);
    MEM32(sys + SYS_LAST_THINK) = MEM32(in + 4u);
    MEM32(sys + SYS_THINK_COST) = MEM32(in + 8u);
    in += 12u;
    deactivate_within(sys, 0.0f, 0.0f, 0.0f, FAR_RADIUS);
    for (int i = 0; i < ACTIVE_AI_SAVE_ENTRIES; i++) {
        MEM32(value) = 0;
        in = cdecl2(READ_HANDLE, in, value);
        if (i < activations && MEM32(value)) activate(sys, MEM32(value));
    }
    count = (int32_t)MEM32(in);
    in += 4u;
    MEM32(value) = 0;
    for (int32_t i = 0; i < count; i++) in = cdecl2(READ_HANDLE, in, value);
    g_esp += SCRATCH;
    g_eax = in;
    GUEST_RETURN_STDCALL(1);
}

static void ai_influences(void) {
    uint32_t brain = A.think_brain, esp = A.think_esp;
    if (g_alias_entry) {
        A.influences();
        return;
    }
    A.think_brain = g_ecx;
    A.think_esp = g_esp;
    A.influences();
    A.think_brain = brain;
    A.think_esp = esp;
}

/* Entered from 0x00421829, right after 0x004213F0's loop over entries 0..3: entries 4..6 add their parts here, with
 * the point and squared radius that loop used, before the influences are weighed. */
static void ai_influences_tail(void) {
    uint32_t brain = g_ecx, frame = g_esp + 4u, buf;
    float point[3], v[3], r2;
    if (g_alias_entry || !A.think_brain || brain != A.think_brain || g_esp + 4u + INFLUENCES_FRAME != A.think_esp) {
        A.influences_tail();
        return;
    }
    read_vec(frame + INFLUENCES_POINT, point);
    r2 = rdf(frame + INFLUENCES_RADIUS2);
    g_esp -= SCRATCH;
    buf = g_esp;
    for (int i = ACTIVE_AI_RETAIL_ENTRIES; i <= limit_of(MEM32(AI_SYSTEM)); i++) {
        uint32_t animal = active_at(MEM32(AI_SYSTEM), i);
        if (!animal) continue;
        read_vec(position(animal, buf), v);
        if (!active_ai_x87_c0(active_ai_dist2_zyx(v, point), (double)r2)) continue;
        for (uint32_t part = 0; part < ACTIVE_AI_ANIMAL_PARTS; part++) {
            uint32_t sys = MEM32(AI_SYSTEM), args[2];
            args[0] = MEM32(active_at(sys, i) + ANIMAL_PARTS + 4u * part);
            args[1] = MEM32(sys + SYS_TIME);
            guest_call(BRAIN_SEE, MEM32(brain + BRAIN_INFLUENCE_LIST), args, 2, 0, NULL);
        }
    }
    g_esp += SCRATCH;
    g_ecx = brain;
    A.influences_tail();
}

void active_ai_configure(int enhanced) { A.enhanced = enhanced != 0; }

int active_ai_installed(void) { return A.installed; }

void active_ai_install(void) {
    const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } o[] = {
        { CTOR_VA, ai_ctor, &A.ctor },
        { DTOR_VA, ai_dtor, &A.dtor },
        { ACTIVATE_VA, ai_activate, &A.activate },
        { DEACTIVATE_VA, ai_deactivate, &A.deactivate },
        { DEACTIVATE_WITHIN_VA, ai_deactivate_within, &A.within },
        { SEEN_BY_NEAREST_VA, ai_seen_by_nearest, &A.seen },
        { HEARD_BY_NEAR_VA, ai_heard_by_near, &A.heard },
        { THINK_NEXT_VA, ai_think_next, &A.think_next },
        { PROCESS_VA, ai_process, &A.process },
        { SAVE_VA, ai_save, &A.save },
        { LOAD_VA, ai_load, &A.load },
        { INFLUENCES_VA, ai_influences, &A.influences },
        { INFLUENCES_TAIL_VA, ai_influences_tail, &A.influences_tail },
    };
    size_t n = sizeof o / sizeof o[0], done = 0;
    if (!A.enhanced) return;
    if (n != active_ai_override_count) {
        host_log("ce", "active AI: the override table is out of step with the policy: original limit kept");
        return;
    }
    for (size_t i = 0; i < n; i++)
        if (o[i].va != active_ai_override_va((unsigned)i) || !guest_override_lifted(o[i].va)) {
            host_log("ce", "active AI: sub_%08X has no stable entry: original limit kept", o[i].va);
            return;
        }
    /* All or none: a partial set would leave entries 4..6 half handled (the entries are REQUIRED, so a refusal
     * stops the run in guest_overrides_check). */
    for (size_t i = 0; i < n; i++) done += (size_t)guest_override_install(o[i].va, o[i].replacement, o[i].lifted);
    A.installed = done == n;
    if (!A.installed) host_log("ce", "active AI: %u of %u hooks installed", (unsigned)done, (unsigned)n);
}
