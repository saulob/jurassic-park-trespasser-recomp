/* Official Trespasser v1.1 build 117 gameplay rules over the retail 1.0 build 116 state, without the guest. Each rule
 * takes `on`: 0 gives the retail 116 result, 1 the official 117 one. */
#ifndef PATCH117_RULES_H
#define PATCH117_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0.25 s of 10 ms physics steps (117 truncates 0.25 / 0.01f). */
#define PATCH117_JUMP_TTL_STEPS 25

/* The pelvis jump impulse (116 globals 0x0067AFA0..A8, voluntary byte 0x0067AF5D); z != 0 is a pending request.
 * ttl exists only in 117. */
typedef struct {
    float x, y, z;
    int voluntary;
    int ttl;
} patch117_jump_t;

/* CPlayer::Process's jump request for a frame: amount 0.9 on the key's hit frame; 117 also asks while it is held,
 * without the new-press mark. scale: the impulse's z when it arms, 1 in the game (a CE script's jump factor). */
typedef struct {
    float amount;
    int is_new;
    float scale;
} patch117_jump_request_t;

patch117_jump_request_t patch117_jump_request(int on, int hit, int held);
/* The per-frame hand-off (0x004F0960): 116 replaces the impulse every frame; 117 arms it on a new press or when
 * nothing is pending, and otherwise leaves it alone. */
void patch117_jump_frame(int on, patch117_jump_t* j, patch117_jump_request_t request);
/* A ground check of Foot_IDOF that could not jump: 117 expires the request after PATCH117_JUMP_TTL_STEPS of them. */
void patch117_jump_missed(int on, patch117_jump_t* j);
/* An impulse the game applies by itself (a hit or a throw): 117 gives it the same lifetime. */
void patch117_jump_impulse(int on, patch117_jump_t* j, float x, float y, float z);
/* One physics step of the jump: 1 when it jumps. `able` = human pelvis alive; `grounded` = its foot support test. */
int patch117_jump_step(int on, patch117_jump_t* j, int able, int grounded);

/* The player's hand (CPlayer +0xE28): 0 down, 1 raising, 2 raised; 117 adds 3, raised and once inside the view box. */
#define PATCH117_HAND_RAISE_SECONDS 0.4f
#define PATCH117_HAND_BLOCKED_ABOVE -10.0f

/* Grabbing with the raised hand. */
int patch117_grab_allowed(int on, int hand_state);
/* The mouse moving the raised hand. */
int patch117_hand_steered(int on, int hand_state);
/* An empty hand's reach (and its stow target distance). */
float patch117_empty_reach(int on);
/* How long after a pick-up the hand keeps its strong grip before pulling the object in to carry it. */
float patch117_pickup_hold_seconds(int on);
/* How far above its target a picked-up object is lifted at once. */
float patch117_pickup_lift(int on);
/* A raising empty hand (117 only) settles after 0.4 s or once nothing but terrain lies on the way; until then a
 * blocked hand stays tucked at the shoulder. `obstacle` is the highest top of what lies on the way, relative to the
 * player, or -100 for nothing. */
int patch117_hand_settles(double raising_seconds, float obstacle);

/* Raising the hand while holding something: 116 always puts the held object back inside the view box; 117 leaves a
 * free gun where it is aimed. */
int patch117_held_hand_recentred(int on, int holds_free_gun);

/* The view while the raised hand is held (Move): 116 follows the hand at state 2; 117 marks state 3 once the hand
 * needs no turn and only then follows it. `turned` = the hand pushes the view. Returns 1 when the view turns. */
int patch117_view_follows_hand(int on, int* hand_state, int turned);

/* Mouse look past the 30 degree head range with the hand down: the body turn, before its +-1.49 rad clamp. 116 turns
 * the body by the excess only; 117 by the whole angle. */
float patch117_body_turn(int on, float yaw, float head_range);

/* Box-box contact (0x004E9780). A corner of the other box is tested against each face of a sub-box in that sub-box's
 * frame; eps is the contact skin (0.09 m). */
#define PATCH117_CONTACT_EPS 0.09f

typedef struct {
    int axis; /* 0 x, 1 y, 2 z */
    int sign; /* the face: +1 or -1 */
    float d;  /* depth below the skin */
    float w;  /* weight */
} patch117_face_t;

/* 117 pushes out a corner already deeper than eps behind every face, along the direction from the sub-box's inner
 * core (each half extent less the smallest) to the corner; 116 gives such a corner no face. Faces in z, x, y order;
 * 0 when the corner is not that deep (116 and 117 then agree). */
int patch117_deep_corner(const float c[3], const float h[3], patch117_face_t out[3]);

typedef struct {
    float cs, cd, cf; /* stiffness, damping, friction of the pair */
    float work;       /* the work factor the game sums */
} patch117_contact_coeffs_t;

typedef struct {
    float f[3];          /* force in the sub-box frame */
    float normal_work, friction_work;
} patch117_face_force_t;

/* The force of one face, as every face of 0x004E9780 computes it; v = relative velocity in the sub-box frame. */
patch117_face_force_t patch117_face_force(const patch117_face_t* face, const float v[3], const patch117_contact_coeffs_t* k);
/* The face's code for the contact (+z 0, -z 1, +x 2, -x 3, +y 4, -y 5). */
int patch117_face_code(const patch117_face_t* face);
/* v = S v with S row-major: a sub-box frame direction in its box's frame. */
void patch117_sub_box_rotate(const float s[9], float v[3]);

/* The smallest half extent over a box's sub-boxes (hx, hy, hz each `stride` floats apart), taken when it is made. */
float patch117_min_half(const float* extents, int stride, int count);
/* Whether a box pair runs edge-edge contact from this side: never in 116; in 117 from the box whose smallest half
 * extent is larger, or the lower index on a tie. */
int patch117_edge_contact(int on, float this_min, float other_min, int this_index, int other_index);

typedef struct {
    int movable;
    int state;          /* +0x424 */
    int since, last, active; /* +0x430, +0x438, +0x43C */
} patch117_wake_box_t;

/* An overlapping pair: each movable box hands its activity to the other. 116 skips pairs that ignore collisions with
 * each other; 117 wakes them too. `awake` is the state that wakes the other box. */
void patch117_pair_wake(int on, int ignores, patch117_wake_box_t* a, int saved_a, patch117_wake_box_t* b, int saved_b,
                        int awake);

#ifdef __cplusplus
}
#endif

#endif
