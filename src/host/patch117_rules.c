#include "patch117_rules.h"

#include <float.h>
#include <math.h>

#define JUMP_AMOUNT 0.9f

patch117_jump_request_t patch117_jump_request(int on, int hit, int held) {
    patch117_jump_request_t r = { 0.0f, 0, 1.0f };
    if (hit) {
        r.amount = JUMP_AMOUNT;
        r.is_new = 1;
    } else if (on && held) {
        r.amount = JUMP_AMOUNT;
    }
    return r;
}

static void arm(patch117_jump_t* j, float scale) {
    j->x = j->y = 0.0f;
    j->z = scale;
    j->voluntary = 1;
    j->ttl = PATCH117_JUMP_TTL_STEPS;
}

static void clear(patch117_jump_t* j) { j->x = j->y = j->z = 0.0f; }

void patch117_jump_frame(int on, patch117_jump_t* j, patch117_jump_request_t request) {
    if (!on) {
        if (request.amount != 0.0f)
            arm(j, request.scale);
        else
            clear(j);
    } else if (request.amount != 0.0f && (request.is_new || j->z == 0.0f)) {
        arm(j, request.scale);
    }
}

void patch117_jump_missed(int on, patch117_jump_t* j) {
    if (on && --j->ttl <= 0) clear(j);
}

void patch117_jump_impulse(int on, patch117_jump_t* j, float x, float y, float z) {
    j->x = x;
    j->y = y;
    j->z = z;
    j->voluntary = 0;
    if (on) j->ttl = PATCH117_JUMP_TTL_STEPS;
}

int patch117_jump_step(int on, patch117_jump_t* j, int able, int grounded) {
    if (!able || j->z == 0.0f) return 0;
    if (!grounded) {
        patch117_jump_missed(on, j);
        return 0;
    }
    clear(j);
    return 1;
}

#define CONTACT_STIFFNESS 1.4f
#define CONTACT_DAMPING 1.4f
#define SEPARATING_DAMPING 0.25f
#define FRICTION -1.0f

int patch117_deep_corner(const float c[3], const float h[3], patch117_face_t out[3]) {
    static const int order[3] = { 2, 0, 1 };
    float e[3], m, inv;
    int n = 0;
    for (int a = 0; a < 3; a++)
        if (!((float)((fabs(c[a]) - h[a]) - PATCH117_CONTACT_EPS) < -PATCH117_CONTACT_EPS)) return 0;
    m = h[0] <= h[1] ? h[0] : h[1];
    if (h[2] < m) m = h[2];
    for (int a = 0; a < 3; a++) {
        e[a] = (float)(fabs(c[a]) - (double)(h[a] - m));
        if (e[a] < 0.0f) e[a] = 0.0f;
    }
    inv = (float)(1.0 / sqrt((double)e[2] * e[2] + (double)e[1] * e[1] + (double)e[0] * e[0]));
    for (int q = 0; q < 3; q++) {
        int a = order[q];
        if (e[a] == 0.0f) continue;
        out[n].axis = a;
        out[n].sign = c[a] <= 0.0f ? -1 : 1;
        out[n].d = -PATCH117_CONTACT_EPS;
        out[n].w = (float)((double)inv * e[a]);
        n++;
    }
    return n;
}

patch117_face_force_t patch117_face_force(const patch117_face_t* face, const float v[3], const patch117_contact_coeffs_t* k) {
    static const int tangent[3][2] = { { 1, 2 }, { 0, 2 }, { 0, 1 } };
    patch117_face_force_t r = { { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f };
    int a = face->axis, b = tangent[a][0], c = tangent[a][1];
    double w = face->w, va = v[a];
    double damp = va * k->cd, stiff = (double)face->d * CONTACT_STIFFNESS * k->cs;
    if (face->sign > 0 ? va > 0.0 : va < 0.0) damp *= SEPARATING_DAMPING;
    r.f[a] = (float)((face->sign > 0 ? -stiff - damp * CONTACT_DAMPING : stiff - damp * CONTACT_DAMPING) * w);
    r.f[b] = (float)(w * v[b] * FRICTION * k->cf);
    r.f[c] = (float)(w * v[c] * FRICTION * k->cf);
    r.normal_work = (float)(damp * w * va * CONTACT_DAMPING * k->work);
    r.friction_work = (float)-(((double)v[c] * v[c] + (double)v[b] * v[b]) * w * FRICTION * k->cf * k->work);
    return r;
}

int patch117_face_code(const patch117_face_t* face) {
    static const int base[3] = { 2, 4, 0 };
    return base[face->axis] + (face->sign < 0);
}

void patch117_sub_box_rotate(const float s[9], float v[3]) {
    float x = v[0], y = v[1], z = v[2];
    for (int r = 0; r < 3; r++) v[r] = (float)((double)x * s[3 * r] + (double)y * s[3 * r + 1] + (double)z * s[3 * r + 2]);
}

float patch117_min_half(const float* extents, int stride, int count) {
    float m = FLT_MAX;
    for (int i = 0; i < count; i++) {
        const float* h = extents + (size_t)i * stride;
        if (h[0] < m) m = h[0];
        if (m > h[1]) m = h[1];
        if (m > h[2]) m = h[2];
    }
    return m;
}

int patch117_edge_contact(int on, float this_min, float other_min, int this_index, int other_index) {
    if (!on) return 0;
    return this_min > other_min || (this_min == other_min && this_index < other_index);
}

static void pass_activity(const patch117_wake_box_t* from, int saved, patch117_wake_box_t* to, int awake) {
    int latest;
    if (!from->movable) return;
    if (saved == awake) to->state = awake;
    latest = to->active >= to->last ? to->active : to->last;
    if (from->active > latest) to->since = from->since;
    if (to->active < from->active) to->active = from->active;
}

void patch117_pair_wake(int on, int ignores, patch117_wake_box_t* a, int saved_a, patch117_wake_box_t* b, int saved_b,
                        int awake) {
    if (ignores && !on) return;
    pass_activity(a, saved_a, b, awake);
    pass_activity(b, saved_b, a, awake);
}

int patch117_grab_allowed(int on, int hand_state) { return hand_state >= (on ? 3 : 2); }

int patch117_hand_steered(int on, int hand_state) { return hand_state > (on ? 1 : 0); }

float patch117_empty_reach(int on) { return on ? 0.7f : 0.8f; }

float patch117_pickup_hold_seconds(int on) { return on ? 2.0f : 1.0f; }

float patch117_pickup_lift(int on) { return on ? 0.1f : 0.0f; }

int patch117_hand_settles(double raising_seconds, float obstacle) {
    return !(raising_seconds < PATCH117_HAND_RAISE_SECONDS) || !(obstacle > PATCH117_HAND_BLOCKED_ABOVE);
}

int patch117_held_hand_recentred(int on, int holds_free_gun) { return !on || !holds_free_gun; }

int patch117_view_follows_hand(int on, int* hand_state, int turned) {
    if (!on) return *hand_state == 2 && turned;
    if (!turned) {
        *hand_state = 3;
        return 0;
    }
    return *hand_state != 2;
}

float patch117_body_turn(int on, float yaw, float head_range) {
    if (on) return yaw;
    return yaw > 0.0f ? (float)(yaw - (double)head_range) : (float)(yaw + (double)head_range);
}
