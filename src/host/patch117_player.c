/*
 * Official Trespasser v1.1 build 117 player fixes over the lifted 1.0 build 116 code: whole-function wrappers and lift
 * hooks (src/host/lift_hook_list.h) that change 116 state exactly where 117's code differs. Off leaves the 116 code
 * running untouched. The rules themselves are in patch117_rules.c.
 */
#include "guest_va.h"
#include "guest_abi.h"
#include "guest_features.h"

#include <string.h>

#include "patch117_rules.h"

#define JUMP_HANDOFF_VA GVA(0x004F0960) /* CPhysicsInfoPlayer jump/walk hand-off, thiscall (request, instance), ret 8 */
#define JUMP_X GVA(0x0067AFA0)
#define JUMP_Y GVA(0x0067AFA4)
#define JUMP_Z GVA(0x0067AFA8)
#define JUMP_VOLUNTARY GVA(0x0067AF5D)
#define REQUEST_AMOUNT 0x84u
#define REQUEST_NEW 0x8Cu
#define MSG_HELD 0x24u
#define MSG_HIT 0x28u

/* CPlayer and its tuning (0x0066ED10, built once by 0x00440DA0). */
#define PLAYER_POS 0x30u
#define PLAYER_ROTATION 0x3Cu /* quaternion, w first */
#define PLAYER_SCALE 0x4Cu
#define PLAYER_SKELETON 0x90u
#define PLAYER_HEAD 0xD88u
#define PLAYER_HAND_TARGET 0xD98u
#define PLAYER_HAND_BOX 0xE24u
#define PLAYER_HAND_STATE 0xE28u
#define PLAYER_GRAB 0xE30u
#define PLAYER_STOW 0xE44u
#define PLAYER_E48 0xE48u
#define PLAYER_HAND_AIMED 0xE54u
#define PLAYER_E56 0xE56u
#define PLAYER_RAISED_AT 0xE58u
#define GRAB_HOLDING 4
#define GRAB_HOLDING_LAST 5
#define GRAB_MIN_HAND_STATE 3
#define HELD_HAND 0x40u
#define VIEW_YAW_MIN GVA(0x0066ED8C)
#define VIEW_PITCH_MIN GVA(0x0066ED90)
#define VIEW_YAW_MAX GVA(0x0066ED94)
#define VIEW_PITCH_MAX GVA(0x0066ED98)
#define HOLDING_AIMS_CENTRE GVA(0x0066ED67)
#define EMPTY_REACH GVA(0x0066ED28)
#define PICKUP_HOLD GVA(0x0066ED50)
#define SHOULDER_HEIGHT GVA(0x0066EF00)
#define RAISE_PHASE GVA(0x0066EF10)
#define HEAD_RANGE GVA(0x0066EF20)
#define NOW GVA(0x0073F34C)
/* 117's new tuning: the tucked hand's offset from the shoulder bone. */
#define TUCK_X 0.3f
#define TUCK_Y 0.1f
#define TUCK_Z 0.2f
#define TUCK_BONE 10u
#define BONE_STRIDE 0x30u
#define BONE_POS 0x24u
#define OBSTACLE_RADIUS 0.3f
#define OBSTACLE_NONE -100.0f
/* 116 functions the raise uses. */
#define ANGLES_OF_QUAT GVA(0x00441DA0) /* cdecl (out[2], quat) */
#define ANGLES_OF_DIR GVA(0x00441D50)  /* cdecl (out[2], dir[3]) */
#define DIR_OF_ANGLES GVA(0x00442380)  /* cdecl (out[3], angles[2]) */
#define RSQRT GVA(0x0041E1D0)          /* cdecl (float), st0 */
#define RSQRT_TABLE GVA(0x006715A4)
#define ROTATE GVA(0x004CD090)         /* thiscall quat (out[3], v[3]) */
#define ROTATION_TO GVA(0x004CCCE0)    /* thiscall out quat (from[3], to[3]) */
#define FORWARD_AXIS GVA(0x00675A40)
#define QUERY_OPEN GVA(0x00487300)     /* thiscall (placement, radius, length): objects in a capsule */
#define QUERY_NEXT GVA(0x00487900)
#define QUERY_CLOSE GVA(0x004877F0)
#define SET_HAND_POSE GVA(0x0045F660)  /* thiscall (int) */
#define HOLDS_FREE_GUN GVA(0x0045FC10) /* thiscall, al */
#define HAND_REACH GVA(0x0045B470)     /* thiscall, st0 */
#define SLOT_POSITION 0x20u
#define SLOT_TERRAIN 0x54u
#define SLOT_BOUNDS 0x120u
#define SLOT_OWNER 0x17Cu
#define NODE_OBJECT 0x20u
#define SCRATCH_BYTES 0x400u

static int s_on;
static int s_jump_ttl;
static recomp_func_t s_handoff_lifted;
static uint32_t s_head_range, s_reach;
static int s_head_range_swapped, s_reach_swapped;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static uint32_t bits(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static void rdv(uint32_t va, float v[3]) {
    for (int i = 0; i < 3; i++) v[i] = rdf(va + 4u * i);
}

static void wrv(uint32_t va, const float v[3]) {
    for (int i = 0; i < 3; i++) wrf(va + 4u * i, v[i]);
}

static patch117_jump_t jump_read(void) {
    patch117_jump_t j;
    j.x = rdf(JUMP_X);
    j.y = rdf(JUMP_Y);
    j.z = rdf(JUMP_Z);
    j.voluntary = MEM8(JUMP_VOLUNTARY);
    j.ttl = s_jump_ttl;
    return j;
}

static void jump_write(const patch117_jump_t* j) {
    wrf(JUMP_X, j->x);
    wrf(JUMP_Y, j->y);
    wrf(JUMP_Z, j->z);
    MEM8(JUMP_VOLUNTARY) = (uint8_t)j->voluntary;
    s_jump_ttl = j->ttl;
}

/* The lifted body still does the walk request; only its jump write at the tail is replaced. */
static void jump_handoff(void) {
    uint32_t request;
    patch117_jump_request_t r;
    patch117_jump_t j;
    if (g_alias_entry || !s_on) {
        s_handoff_lifted();
        return;
    }
    request = GUEST_ARG(0);
    r.amount = rdf(request + REQUEST_AMOUNT);
    r.is_new = MEM8(request + REQUEST_NEW) != 0;
    j = jump_read();
    s_handoff_lifted();
    patch117_jump_frame(1, &j, r);
    jump_write(&j);
}

/* CPlayer::Process after its hit-frame request: the request local is at [ebp-0x94], the control message at [ebp+8]. */
void GHOOK(0x00455B6B)(void) {
    uint32_t msg;
    patch117_jump_request_t r;
    if (!s_on) return;
    msg = MEM32(g_ebp + 8);
    if (MEM32(msg + MSG_HIT) & 1u) return;
    r = patch117_jump_request(1, 0, MEM32(msg + MSG_HELD) & 1u);
    if (r.amount == 0.0f) return;
    wrf(g_ebp - 0x94, r.amount);
    wrf(g_ebp - 0x90, 1.0f);
    MEM8(g_ebp - 0x8C) = (uint8_t)r.is_new;
}

/* The game's own impulses (0x00456490, 0x004609F0) just wrote the jump globals. */
static void jump_impulse_written(void) {
    patch117_jump_t j;
    if (!s_on) return;
    j = jump_read();
    patch117_jump_impulse(1, &j, j.x, j.y, j.z);
    s_jump_ttl = j.ttl;
}

void GHOOK(0x004569C8)(void) { jump_impulse_written(); }
void GHOOK(0x00460CF5)(void) { jump_impulse_written(); }

/* Foot_IDOF: a human, alive pelvis with a pending jump whose box/slope test failed; al = the box-support flag. */
void GHOOK(0x004E78D2)(void) {
    patch117_jump_t j;
    if (!s_on || (g_eax & 0xFFu)) return;
    j = jump_read();
    patch117_jump_missed(1, &j);
    jump_write(&j);
}

/* Guest scratch below the caller's frame, handed out in 16-byte slots; the registers the hooked code holds are kept. */
typedef struct {
    uint32_t esp, eax, ebx, esi, edi, base, used;
    int failed;
} guest_scratch_t;

static void scratch_open(guest_scratch_t* s) {
    s->esp = g_esp, s->eax = g_eax, s->ebx = g_ebx, s->esi = g_esi, s->edi = g_edi;
    s->base = (g_esp - SCRATCH_BYTES) & ~0xFu;
    s->used = 0;
    s->failed = 0;
    g_esp = s->base;
}

static void scratch_close(const guest_scratch_t* s) {
    g_esp = s->esp, g_eax = s->eax, g_ebx = s->ebx, g_esi = s->esi, g_edi = s->edi;
}

static uint32_t slot(guest_scratch_t* s, uint32_t bytes) {
    uint32_t at = s->base + s->used;
    s->used += (bytes + 15u) & ~15u;
    if (s->used > SCRATCH_BYTES) s->failed = 1;
    return s->failed ? s->base : at;
}

static uint32_t call(guest_scratch_t* s, uint32_t va, uint32_t self, const uint32_t* args, int count, int cdecl_call) {
    uint32_t r = 0;
    if (!s->failed && !guest_call(va, self, args, count, cdecl_call, &r)) s->failed = 1;
    return r;
}

static uint32_t call_virtual(guest_scratch_t* s, uint32_t self, uint32_t slot_offset, const uint32_t* args, int count) {
    uint32_t r = 0;
    if (!s->failed && !guest_call_virtual(self, slot_offset, args, count, &r)) s->failed = 1;
    return r;
}

static float call_float(guest_scratch_t* s, uint32_t va, uint32_t self, const uint32_t* args, int count, int cdecl_call) {
    call(s, va, self, args, count, cdecl_call);
    return s->failed ? 0.0f : (float)RECOMP_X87_POP();
}

/* The 116 inverse square roots as 117 uses them: the table estimate and one Newton step. */
static float rsqrt_step(float l, float r) { return (float)((3.0f - (double)l * r * r) * (0.5f * (double)r)); }

static float table_rsqrt(float l) {
    uint32_t u = bits(l), r;
    float f;
    r = ((0x5F000000u - ((u & 0x7F800000u) >> 1)) & 0x7F800000u) | MEM32(RSQRT_TABLE + 4u * ((u & 0xFFFFFFu) >> 12));
    memcpy(&f, &r, sizeof f);
    return f;
}

static void normalise(guest_scratch_t* s, float v[3]) {
    float l = (float)((double)v[2] * v[2] + (double)v[1] * v[1] + (double)v[0] * v[0]), k;
    uint32_t arg = bits(l);
    k = rsqrt_step(l, call_float(s, RSQRT, 0, &arg, 1, 1));
    for (int i = 0; i < 3; i++) v[i] = (float)((double)v[i] * k);
}

/* 117's 0x442EF0: the highest top, relative to the player, of what lies within OBSTACLE_RADIUS of the segment,
 * ignoring the player and the terrain; OBSTACLE_NONE when nothing does. */
static float obstacle(guest_scratch_t* s, uint32_t self, const float from[3], const float to[3]) {
    uint32_t placement = slot(s, 32), q = slot(s, 16), dir = slot(s, 16), query = slot(s, 16), cast = slot(s, 16),
             lo = slot(s, 16), hi = slot(s, 16), args[3], node;
    float d[3], l, best = OBSTACLE_NONE;
    for (int i = 0; i < 3; i++) d[i] = (float)((double)to[i] - from[i]);
    l = (float)((double)d[2] * d[2] + (double)d[1] * d[1] + (double)d[0] * d[0]);
    normalise(s, d);
    wrv(dir, d);
    args[0] = FORWARD_AXIS, args[1] = dir;
    call(s, ROTATION_TO, q, args, 2, 0);
    if (s->failed) return OBSTACLE_NONE;
    wrv(placement, from);
    memcpy((void*)ADDR(placement + 12), (const void*)ADDR(q), 16);
    wrf(placement + 28, 1.0f);
    args[0] = placement, args[1] = bits(OBSTACLE_RADIUS), args[2] = bits((float)((double)rsqrt_step(l, table_rsqrt(l)) * l));
    call(s, QUERY_OPEN, query, args, 3, 0);
    while (!s->failed && (node = call(s, QUERY_NEXT, query, NULL, 0, 0)) != 0) {
        uint32_t object = MEM32(node + NODE_OBJECT);
        float top;
        if (call_virtual(s, object, SLOT_OWNER, NULL, 0) == self) continue;
        MEM32(cast) = object;
        if (object) call_virtual(s, object, SLOT_TERRAIN, &cast, 1);
        if (MEM32(cast)) continue;
        args[0] = lo, args[1] = hi;
        call_virtual(s, object, SLOT_BOUNDS, args, 2);
        top = (float)((double)rdf(hi + 8) - rdf(self + PLAYER_POS + 8));
        if (best < top) best = top;
    }
    call(s, QUERY_CLOSE, query, NULL, 0, 0);
    return best;
}

/* The hand target: the shoulder plus `reach` along the view-relative angles turned by the view. */
static void aim_hand(guest_scratch_t* s, uint32_t self, const float angles[2], const float view[2], float reach) {
    uint32_t a = slot(s, 16), out = slot(s, 16), args[2];
    float d[3], target[3];
    wrf(a, (float)((double)angles[0] + view[0]));
    wrf(a + 4, (float)((double)angles[1] + view[1]));
    args[0] = out, args[1] = a;
    call(s, DIR_OF_ANGLES, 0, args, 2, 1);
    if (s->failed) return;
    rdv(out, d);
    target[0] = (float)((double)reach * d[0]);
    target[1] = (float)((double)reach * d[1]);
    target[2] = (float)((double)(float)((double)reach * d[2]) + rdf(SHOULDER_HEIGHT));
    wrv(self + PLAYER_HAND_TARGET, target);
}

static void view_box_centre(float a[2]) {
    a[0] = (float)((double)(float)((double)rdf(VIEW_YAW_MIN) + rdf(VIEW_YAW_MAX)) * 0.5f);
    a[1] = (float)((double)(float)((double)rdf(VIEW_PITCH_MIN) + rdf(VIEW_PITCH_MAX)) * 0.5f);
}

static int clamp_into(float* v, uint32_t lo, uint32_t hi) {
    if (*v < rdf(lo))
        *v = rdf(lo);
    else if (*v > rdf(hi))
        *v = rdf(hi);
    else
        return 0;
    return 1;
}

static void set_hand_pose(guest_scratch_t* s, uint32_t self) {
    uint32_t zero = 0;
    call(s, SET_HAND_POSE, self, &zero, 1, 0);
}

/* An empty hand rising to the view box centre; while it may still be rising, a hand that something other than the
 * terrain blocks is tucked at the shoulder. Returns 1 while the hand stays tucked. */
static int raise_empty_hand(guest_scratch_t* s, uint32_t self, const float view[2], float reach) {
    uint32_t rotated = slot(s, 16), out = slot(s, 16), args[2], from, bone;
    float a[2], hand[3], top, k, tuck[3];
    view_box_centre(a);
    aim_hand(s, self, a, view, reach);
    MEM8(self + PLAYER_HAND_AIMED) = 0;
    if (s->failed || !((double)rdf(NOW) - rdf(self + PLAYER_RAISED_AT) < PATCH117_HAND_RAISE_SECONDS)) return 0;
    if (rdf(self + PLAYER_ROTATION) != 1.0f) {
        args[0] = rotated, args[1] = self + PLAYER_HAND_TARGET;
        call(s, ROTATE, self + PLAYER_ROTATION, args, 2, 0);
    } else {
        memcpy((void*)ADDR(rotated), (const void*)ADDR(self + PLAYER_HAND_TARGET), 12);
    }
    for (int i = 0; i < 3; i++) hand[i] = (float)((double)rdf(rotated + 4u * i) + rdf(self + PLAYER_POS + 4u * i));
    args[0] = out;
    from = call_virtual(s, MEM32(self + PLAYER_HAND_BOX), SLOT_POSITION, args, 1);
    top = s->failed ? OBSTACLE_NONE : obstacle(s, self, (const float*)ADDR(from), hand);
    if (s->failed || patch117_hand_settles(0.0, top)) return 0;
    bone = MEM32(self + PLAYER_SKELETON) + TUCK_BONE * BONE_STRIDE + BONE_POS;
    k = rdf(self + PLAYER_SCALE);
    tuck[0] = (float)((double)(float)((double)k * rdf(bone)) + TUCK_X);
    tuck[1] = (float)((double)(float)((double)k * rdf(bone + 4)) + TUCK_Y);
    tuck[2] = (float)((double)(float)((double)k * rdf(bone + 8)) + TUCK_Z);
    wrv(self + PLAYER_HAND_TARGET, tuck);
    return 1;
}

/* A held object (not a free gun): the hand target is kept unless it left the view box, then put back on its edge. */
static void keep_held_in_view(guest_scratch_t* s, uint32_t self, const float view[2], float reach) {
    float a[2];
    int aim = MEM8(HOLDING_AIMS_CENTRE) != 0;
    if (aim) {
        view_box_centre(a);
    } else {
        uint32_t rel = slot(s, 16), out = slot(s, 16), args[2];
        float r[3];
        rdv(self + PLAYER_HAND_TARGET, r);
        r[2] = (float)((double)r[2] - rdf(SHOULDER_HEIGHT));
        normalise(s, r);
        wrv(rel, r);
        args[0] = out, args[1] = rel;
        call(s, ANGLES_OF_DIR, 0, args, 2, 1);
        if (s->failed) return;
        a[0] = (float)((double)rdf(out) - view[0]);
        a[1] = (float)((double)rdf(out + 4) - view[1]);
        aim = clamp_into(&a[0], VIEW_YAW_MIN, VIEW_YAW_MAX) | clamp_into(&a[1], VIEW_PITCH_MIN, VIEW_PITCH_MAX);
    }
    if (aim) aim_hand(s, self, a, view, reach);
}

/* 117's 0x443320 while the hand button is held, run where 117 runs it in CPlayer::Process. */
static void raise_hand(uint32_t self, uint32_t msg) {
    guest_scratch_t s;
    uint32_t view_out, args[2];
    float view[2], reach;
    int state;
    if (!(MEM32(msg + MSG_HELD) & HELD_HAND)) return;
    scratch_open(&s);
    view_out = slot(&s, 16);
    args[0] = view_out, args[1] = self + PLAYER_HEAD;
    call(&s, ANGLES_OF_QUAT, 0, args, 2, 1);
    view[0] = rdf(view_out), view[1] = rdf(view_out + 4);
    reach = call_float(&s, HAND_REACH, self, NULL, 0, 0);
    state = (int)MEM32(self + PLAYER_HAND_STATE);
    if (!s.failed && (int)MEM32(self + PLAYER_GRAB) < GRAB_HOLDING) {
        if (state == 0) {
            MEM32(self + PLAYER_RAISED_AT) = MEM32(NOW);
            MEM32(self + PLAYER_HAND_STATE) = 1;
            set_hand_pose(&s, self);
            state = 1;
        }
        if (state <= 1 && !raise_empty_hand(&s, self, view, reach)) {
            set_hand_pose(&s, self);
            MEM32(self + PLAYER_HAND_STATE) = 2;
        }
    } else if (!s.failed && state < 3) {
        int free_gun = (call(&s, HOLDS_FREE_GUN, self, NULL, 0, 0) & 0xFFu) != 0;
        if (!s.failed && patch117_held_hand_recentred(1, free_gun)) keep_held_in_view(&s, self, view, reach);
        MEM32(self + PLAYER_HAND_STATE) = MEM8(RAISE_PHASE) ? 2u : 3u;
    }
    if (s.failed) host_log("patch117", "warning: the official hand raise could not run this frame");
    scratch_close(&s);
}

/* CPlayer::Process, where 117 calls its hand raise: this at [ebp-0x1E68], the control message at [ebp+8]. */
void GHOOK(0x0045125D)(void) {
    if (s_on) raise_hand(MEM32(g_ebp - 0x1E68), MEM32(g_ebp + 8));
}

/* The 116 raising step (state 1 to 2 once the hand is in the view box) is replaced by the raise above; al = the raise
 * phase flag it tests. */
void GHOOK(0x004512FF)(void) {
    if (s_on) g_eax = 0;
}

/* The grab gate: the lowest hand state a grab accepts was just stored at [ebp-0x3FC]. */
void GHOOK(0x0044CF9F)(void) {
    if (s_on) MEM32(g_ebp - 0x3FC) = GRAB_MIN_HAND_STATE;
}

/* After PickUp: 117 lifts the hand's target at once. */
void GHOOK(0x00451212)(void) {
    uint32_t z = MEM32(g_ebp - 0x1E68) + PLAYER_HAND_TARGET + 8;
    if (s_on) wrf(z, (float)((double)patch117_pickup_lift(1) + rdf(z)));
}

/* The mouse steers the hand when 0 (ecx) is below the hand state; 117 needs the hand raised. */
void GHOOK(0x004519A1)(void) {
    if (s_on && g_ecx == 0) g_ecx = 1;
}

/* Move, after the hand pushed the view (yaw [ebp-0x1C], pitch [ebp-0x18]; before: [ebp-0x38], [ebp-0x34]); this at
 * [ebp-0x59C]. The pushes ran only inside this gate, and the yaw push only at state 2. */
void GHOOK(0x0044C1B2)(void) {
    uint32_t self = MEM32(g_ebp - 0x59C), yaw = g_ebp - 0x1C, pitch = g_ebp - 0x18;
    int state = (int)MEM32(self + PLAYER_HAND_STATE), turned;
    if (!s_on || state < 2 || MEM32(self + PLAYER_E48) || MEM32(self + PLAYER_STOW) || MEM8(self + PLAYER_E56)) return;
    if (state != 2) {
        float x = rdf(g_ebp - 0x5C);
        if (x < rdf(VIEW_YAW_MIN))
            wrf(yaw, (float)((double)x - rdf(VIEW_YAW_MIN) + rdf(yaw)));
        else if (x > rdf(VIEW_YAW_MAX))
            wrf(yaw, (float)((double)x - rdf(VIEW_YAW_MAX) + rdf(yaw)));
    }
    turned = MEM32(yaw) != MEM32(g_ebp - 0x38) || MEM32(pitch) != MEM32(g_ebp - 0x34);
    if (!patch117_view_follows_hand(1, &state, turned) && turned) {
        MEM32(yaw) = MEM32(g_ebp - 0x38);
        MEM32(pitch) = MEM32(g_ebp - 0x34);
    }
    MEM32(self + PLAYER_HAND_STATE) = (uint32_t)state;
}

/* Mouse look past the head range with the hand down subtracts the range from the body turn; 117 does not. The two
 * loads of the range are bracketed, so nothing else sees it zeroed. */
static void head_range_off(void) {
    if (!s_on) return;
    s_head_range = MEM32(HEAD_RANGE);
    MEM32(HEAD_RANGE) = 0;
    s_head_range_swapped = 1;
}

static void head_range_back(void) {
    if (!s_head_range_swapped) return;
    MEM32(HEAD_RANGE) = s_head_range;
    s_head_range_swapped = 0;
}

void GHOOK(0x004546D7)(void) { head_range_off(); }
void GHOOK(0x004546E2)(void) { head_range_back(); }
void GHOOK(0x0045472E)(void) { head_range_off(); }
void GHOOK(0x00454734)(void) { head_range_back(); }

/* The reach function's last load ([0x0066ED28]), reached for an empty hand and for a held object without reach
 * information; 117 gives the empty hand a shorter reach. this at [ebp-0x114]. */
void GHOOK(0x0045B916)(void) {
    int grab;
    if (!s_on) return;
    grab = (int)MEM32(MEM32(g_ebp - 0x114) + PLAYER_GRAB);
    if (grab >= GRAB_HOLDING && grab <= GRAB_HOLDING_LAST) return;
    s_reach = MEM32(EMPTY_REACH);
    MEM32(EMPTY_REACH) = bits(patch117_empty_reach(1));
    s_reach_swapped = 1;
}

void GHOOK(0x0045B91C)(void) {
    if (!s_reach_swapped) return;
    MEM32(EMPTY_REACH) = s_reach;
    s_reach_swapped = 0;
}

/* The stow handler's empty-hand target distance, just loaded into eax. */
void GHOOK(0x00461491)(void) {
    if (s_on) g_eax = bits(patch117_empty_reach(1));
}

/* The tuning object was just built: 117 holds a picked-up object twice as long before carrying it. */
void GHOOK(0x00441D1D)(void) {
    if (s_on && rdf(PICKUP_HOLD) == patch117_pickup_hold_seconds(0)) wrf(PICKUP_HOLD, patch117_pickup_hold_seconds(1));
}

void patch117_player_configure(int on) {
    s_on = on != 0;
    s_jump_ttl = 0;
}

void patch117_player_install(void) {
    if (!JPT_P117_HAND_JUMP) return;
    if (!guest_override_install(JUMP_HANDOFF_VA, jump_handoff, &s_handoff_lifted))
        host_log("patch117", "warning: the jump hand-off could not be hooked: its official fix is off");
}
