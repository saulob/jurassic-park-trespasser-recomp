/*
 * Official Trespasser v1.1 build 117 physics fixes over the lifted 1.0 build 116 box-box contact (0x004E9780) and
 * broad phase (0x004DC600): deep corners pushed out, contact normals in the sub-box frame, edge-edge contact (code 116
 * already holds but never reaches) and ignore-collision pairs waking each other. Off leaves the 116 code untouched.
 */
#include "guest_va.h"
#include "host.h"

#include <float.h>
#include <string.h>

#include "patch117_rules.h"

#define BOX_INIT_VA GVA(0x004EDFA0)   /* thiscall, ecx = the box */
#define KILL_VELOCITY_VA GVA(0x004E85A0) /* thiscall (x, y, z, nx, ny, nz, other), ret 0x1C */
#define APPLY_CONTACT_VA GVA(0x004E86B0) /* thiscall (other, F[3], P_this[3], P_other[3], sub, other_sub), ret 0x30 */
#define BOX_POOL GVA(0x00699D40)
#define BOX_STRIDE GLAYOUT_BOX_POOL_STRIDE
#define BOX_COUNT 50u
#define BOX_SUB_COUNT 0x80u   /* float */
#define BOX_SUB_ROTATED 0xCDu /* byte per sub-box */
#define BOX_SUB_EXTENTS 0xD8u /* hx, hy, hz per sub-box */
#define BOX_SUB_STRIDE 0x18u
#define BOX_SUB_MAX 10
#define BOX_SUB_ROTATION 0x1C8u /* 3x3 row-major per sub-box */
#define BOX_ROTATION_STRIDE 0x24u
#define BOX_EDGE_PENDING 0xFB4u /* byte per other box */
#define BOX_MOVABLE 0xC4u
#define BOX_STATE 0x424u
#define BOX_SINCE 0x430u
#define BOX_LAST 0x438u
#define BOX_ACTIVE 0x43Cu
#define PAIR_B_BASE 0x104Cu
#define STATE_AWAKE GVA(0x0064D86C)
#define CONTACT_SAVE_TANGENTS GVA(0x006CD960)
#define CONTACT_TANGENT_X GVA(0x006CD964)
#define CONTACT_TANGENT_Y GVA(0x006CD968)
#define CONTACT_TANGENT_Z GVA(0x006CD96C)
#define CONTACT_NORMAL_WORK GVA(0x006CD974)
#define CONTACT_FRICTION_WORK GVA(0x006CD978)
#define CONTACT_WORK GVA(0x006CD97C)
#define CONTACT_STIFFNESS GVA(0x006CD984)
#define CONTACT_DAMPING GVA(0x006CD988)
#define CONTACT_FRICTION GVA(0x006CD98C)
#define CONTACT_FACE_CODE GVA(0x00668500)

static int s_on;
static float s_min_half[BOX_COUNT];
static physics_counts_t s_counts; /* DEBUG_SAULO */
static int s_box_init_hooked;
static recomp_func_t s_box_init_lifted;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }
static uint32_t bits(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static int box_index(uint32_t box) {
    uint32_t off = box - BOX_POOL;
    return off < BOX_COUNT * BOX_STRIDE && !(off % BOX_STRIDE) ? (int)(off / BOX_STRIDE) : -1;
}

/* DEBUG_SAULO */
static void count_box(unsigned* per_box, uint32_t box) {
    int i = box_index(box);
    if (i >= 0) per_box[i]++;
}

void physics_counts_take(physics_counts_t* out) {
    *out = s_counts;
    for (uint32_t i = 0; i < BOX_COUNT && i < PHYSICS_BOXES; i++) {
        uint32_t box = BOX_POOL + BOX_STRIDE * i;
        float sub = rdf(box + BOX_SUB_COUNT);
        out->box_sub_count[i] = sub > 0.0f && sub < 1000.0f ? (unsigned)sub : 0u;
        out->box_awake[i] = MEM32(box + BOX_STATE) == MEM32(STATE_AWAKE);
        out->box_movable[i] = MEM8(box + BOX_MOVABLE) != 0;
        if (out->box_awake[i]) out->awake++;
        if (MEM32(box + BOX_ACTIVE)) out->active++;
    }
    memset(&s_counts, 0, sizeof s_counts);
}

static const float* sub_rotation(uint32_t box, uint32_t sub) {
    return (const float*)ADDR(box + BOX_SUB_ROTATION + BOX_ROTATION_STRIDE * sub);
}

/* 117 keeps the smallest half extent a box is made with; later size changes (a crouch) do not count. */
static void box_init(void) {
    uint32_t box = g_ecx;
    int alias = g_alias_entry != 0, index, count;
    s_box_init_lifted();
    if (alias || (index = box_index(box)) < 0) return;
    count = (int)rdf(box + BOX_SUB_COUNT);
    if (count < 0) count = 0;
    if (count > BOX_SUB_MAX) count = BOX_SUB_MAX;
    s_min_half[index] = patch117_min_half((const float*)ADDR(box + BOX_SUB_EXTENTS), BOX_SUB_STRIDE / 4, count);
}

/* Box-box, at a velocity kill call with its arguments pushed: esi = this box, ebp = its sub-box. */
static void kill_normal_in_box_frame(void) {
    float n[3];
    s_counts.kills++;
    count_box(s_counts.box_kills, g_esi);
    if (!s_on || !MEM8(g_esi + BOX_SUB_ROTATED + g_ebp)) return;
    for (int i = 0; i < 3; i++) n[i] = rdf(g_esp + 0xC + 4u * i);
    patch117_sub_box_rotate(sub_rotation(g_esi, g_ebp), n);
    for (int i = 0; i < 3; i++) wrf(g_esp + 0xC + 4u * i, n[i]);
}

void GHOOK(0x004EB46B)(void) { kill_normal_in_box_frame(); }
void GHOOK(0x004EB781)(void) { kill_normal_in_box_frame(); }
void GHOOK(0x004EBA7B)(void) { kill_normal_in_box_frame(); }
void GHOOK(0x004EBD85)(void) { kill_normal_in_box_frame(); }
void GHOOK(0x004EC07C)(void) { kill_normal_in_box_frame(); }
void GHOOK(0x004EC386)(void) { kill_normal_in_box_frame(); }

static void save_tangents(int axis, const float f[3]) {
    if (!MEM8(CONTACT_SAVE_TANGENTS)) return;
    if (axis != 0) wrf(CONTACT_TANGENT_X, f[0]);
    if (axis != 1) wrf(CONTACT_TANGENT_Y, f[1]);
    if (axis != 2) wrf(CONTACT_TANGENT_Z, f[2]);
}

/* Box-box, after the six face tests of one corner of the other sub-box (esi this box, edi the other, ebp/ebx their
 * sub-boxes). The frame holds the corner in this sub-box's frame, the sub-box half extents, the relative velocity
 * and the contact point in both boxes' frames, as the face tests used them. */
void GHOOK(0x004EC586)(void) {
    uint32_t self = g_esi, other = g_edi, sp = g_esp, sub = g_ebp, other_sub = g_ebx;
    float c[3], h[3], v[3];
    patch117_face_t faces[3];
    patch117_contact_coeffs_t k;
    int n, rotated;
    s_counts.corner_tests++;
    if (!s_on) return;
    c[0] = rdf(sp + 0x88), c[1] = rdf(sp + 0x28), c[2] = rdf(sp + 0xB4);
    h[0] = rdf(sp + 0xA0), h[1] = rdf(sp + 0x8C), h[2] = rdf(sp + 0x7C);
    n = patch117_deep_corner(c, h, faces);
    if (!n) return;
    v[0] = rdf(sp + 0x80), v[1] = rdf(sp + 0x70), v[2] = rdf(sp + 0x74);
    k.cs = rdf(CONTACT_STIFFNESS), k.cd = rdf(CONTACT_DAMPING), k.cf = rdf(CONTACT_FRICTION), k.work = rdf(CONTACT_WORK);
    rotated = MEM8(self + BOX_SUB_ROTATED + sub) != 0;
    for (int i = 0; i < n; i++) {
        float normal[3] = { 0.0f, 0.0f, 0.0f };
        patch117_face_force_t f;
        uint32_t kill[7], apply[12];
        normal[faces[i].axis] = (float)faces[i].sign;
        if (rotated) patch117_sub_box_rotate(sub_rotation(self, sub), normal);
        for (int a = 0; a < 3; a++) kill[a] = bits(c[a]), kill[3 + a] = bits(normal[a]);
        kill[6] = other;
        guest_call(KILL_VELOCITY_VA, self, kill, 7, 0, NULL);
        f = patch117_face_force(&faces[i], v, &k);
        save_tangents(faces[i].axis, f.f);
        wrf(CONTACT_NORMAL_WORK, f.normal_work);
        wrf(CONTACT_FRICTION_WORK, f.friction_work);
        if (rotated) patch117_sub_box_rotate(sub_rotation(self, sub), f.f);
        apply[0] = other;
        for (int a = 0; a < 3; a++) apply[1 + a] = bits(f.f[a]);
        apply[4] = MEM32(sp + 0x10), apply[5] = MEM32(sp + 0x30), apply[6] = MEM32(sp + 0x24);
        apply[7] = MEM32(sp + 0x4C), apply[8] = MEM32(sp + 0x6C), apply[9] = MEM32(sp + 0x84);
        apply[10] = sub, apply[11] = other_sub;
        MEM32(CONTACT_FACE_CODE) = (uint32_t)patch117_face_code(&faces[i]);
        guest_call(APPLY_CONTACT_VA, self, apply, 12, 0, NULL);
    }
}

/* Box-box, at the edge-edge gate after the corners of a sub-box pair: ecx = the other box's index ([esp+0x68]),
 * [esp+0x118] = this box's. 116 runs the edge pass only when both pending bytes are negative, which they never are. */
void GHOOK(0x004EC5A2)(void) {
    uint32_t other = MEM32(g_esp + 0x68), self = MEM32(g_esp + 0x118);
    uint8_t run;
    s_counts.sub_pairs++;
    if (self < PHYSICS_BOXES) s_counts.box_sub_pairs[self]++;
    if (other < PHYSICS_BOXES) s_counts.box_sub_pairs[other]++;
    if (!s_on || !s_box_init_hooked || other >= BOX_COUNT || self >= BOX_COUNT) return;
    run = patch117_edge_contact(1, s_min_half[self], s_min_half[other], (int)self, (int)other) ? 0xFFu : 0u;
    MEM8(g_esi + BOX_EDGE_PENDING + other) = run;
    MEM8(g_edi + BOX_EDGE_PENDING + self) = run;
}

static patch117_wake_box_t wake_read(uint32_t box) {
    patch117_wake_box_t w;
    w.movable = MEM8(box + BOX_MOVABLE) != 0;
    w.state = (int)MEM32(box + BOX_STATE);
    w.since = (int)MEM32(box + BOX_SINCE);
    w.last = (int)MEM32(box + BOX_LAST);
    w.active = (int)MEM32(box + BOX_ACTIVE);
    return w;
}

static void wake_write(uint32_t box, const patch117_wake_box_t* w) {
    MEM32(box + BOX_STATE) = (uint32_t)w->state;
    MEM32(box + BOX_SINCE) = (uint32_t)w->since;
    MEM32(box + BOX_ACTIVE) = (uint32_t)w->active;
}

/* Broad phase, an overlapping pair after the ignore-collision test (al): ebp = box A, ebx - 0x104C = box B; their
 * states at the start of the pass are behind [esp+0x1C] and [esp+0x50]. 116 skips the wake of an ignoring pair. */
void GHOOK(0x004DDEFD)(void) {
    uint32_t a = g_ebp, b = g_ebx - PAIR_B_BASE;
    patch117_wake_box_t wa, wb;
    s_counts.pairs++;
    count_box(s_counts.box_pairs, a);
    count_box(s_counts.box_pairs, b);
    if (!s_on || !(g_eax & 0xFFu)) return;
    wa = wake_read(a);
    wb = wake_read(b);
    patch117_pair_wake(1, 1, &wa, (int)MEM32(MEM32(g_esp + 0x1C)), &wb, (int)MEM32(MEM32(g_esp + 0x50)),
                       (int)MEM32(STATE_AWAKE));
    wake_write(a, &wa);
    wake_write(b, &wb);
}

void patch117_physics_configure(int on) {
    s_on = on != 0;
    for (uint32_t i = 0; i < BOX_COUNT; i++) s_min_half[i] = FLT_MAX;
}

void patch117_physics_install(void) {
    if (!JPT_P117_BOX_BOX) return;
    s_box_init_hooked = guest_override_install(BOX_INIT_VA, box_init, &s_box_init_lifted);
    if (!s_box_init_hooked)
        host_log("patch117", "warning: the physics box set-up could not be hooked: edge-edge contact stays off");
}
