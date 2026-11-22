/* temporary investigation of the player's state (Anne stuck, the hand broken) without the guest: the
 * stuck detector, sanity checks and the snapshot's lines. */
#ifndef PHYSICS_WATCH_POLICY_H
#define PHYSICS_WATCH_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_MOVE_INPUT 0.25f
#define PW_STUCK_SECONDS 0.5
#define PW_STUCK_METRES 0.05f
#define PW_REARM_METRES 1.0f
#define PW_SNAPSHOTS_MAX 8
#define PW_POSITION_LIMIT 100000.0f
#define PW_SPEED_LIMIT 200.0f
#define PW_LINE_MAX 256
#define PW_LINES 15
#define PW_HAND_FAR_M 1.6f
#define PW_HAND_NEAR_M 1.0f
#define PW_ARM_DROP_FACTOR 1.5f /* Update_Right_Arm's HandDrop: wrist farther than 1.5 arm lengths from the shoulder */
#define PW_SANE_SPEED 100.0f    /* InfoBox discipline: a box this fast deactivates its pelvis (0x004DCA47) */
#define PW_SANE_REGION 60.0f

typedef struct {
    double t;
    int watching; /* gameplay, Anne alive, no interface up, no free camera */
    float move;   /* the move input the game got this read */
    float pos[3];
} pw_tick_t;

typedef struct {
    int armed, window;
    double since;
    float origin[2], fired_at[2];
} pw_stuck_t;

void pw_stuck_init(pw_stuck_t* s);
/* 1 on the read that finds Anne trying to move without moving; once, until she has moved PW_REARM_METRES away. */
int pw_stuck_step(pw_stuck_t* s, const pw_tick_t* t, float* seconds, float* moved);

enum { PW_PART_BODY, PW_PART_RIGHT_FOOT, PW_PART_LEFT_FOOT, PW_PART_HAND, PW_PARTS };

typedef struct {
    const char* reason;
    double t;
    char level[16];
    int hint;
    const char *speed, *jump, *camera;
    float speed_real;
    int player_ok;
    float player_pos[3], player_yaw;
    int pelvis; /* -1: none found */
    float pelvis_pos[3], pelvis_vel[3], pelvis_orient[4], alive, hp, hp_max;
    float mouth, mouth_vel, jump_request[3];
    int jump_voluntary;
    float move_x, move_y;
    uint32_t state, hit, pad_buttons;
    int lt, hand_seen, mouse_hand, interface_up;
    float stick_x, stick_y;
    float hand_pos[3], hand_vel[3];
    int hand_phase, grab, reach_crouch;
    int camera_ok;
    float camera_pos[3];
    float stuck_seconds, stuck_moved;
    float drive[6]; /* Kontrol: the pelvis's move and turn request */
    int krouch;
    float foot_contact;
    float arm_urgency, arm_target[3]; /* ControlArm, in the body's frame */
    float hand_limit;                 /* pw_hand_limit of this pelvis */
    int drift_ok, left_foot_moves;
    float drift[PW_PARTS]; /* farthest from the pelvis over the last 1-2 s */
} pw_snapshot_t;

/* The farthest the wrist may be from the pelvis before the game's own HandDrop rule holds for certain: the shoulder's
 * offset (Pel_Data[13] + [37], [38], [39]) plus 1.5 arm lengths (Pel_Data[40]); PW_HAND_FAR_M without valid data. */
float pw_hand_limit(float hip, float shoulder_x, float shoulder_y, float shoulder_z, float arm);

/* The fastest each part of the body moved lately, for the pelvis-lost report; also the farthest it was from the pelvis. */
typedef struct {
    double window_start;
    float now[PW_PARTS], previous[PW_PARTS];
} pw_peaks_t;

void pw_peaks_add(pw_peaks_t* p, double t, const float speeds[PW_PARTS]);
/* The peaks over the last one to two seconds. */
void pw_peaks_get(const pw_peaks_t* p, float out[PW_PARTS]);

/* 1 when the hand has just gone farther than PW_HAND_FAR_M from the pelvis; again once it has come back within
 * PW_HAND_NEAR_M. */
int pw_hand_far_step(int* armed, float distance, float limit);

int pw_finite(const float* v, int n, float limit);
/* The first field out of range ("player position", "pelvis velocity", ...), NULL when every one is sane. */
const char* pw_invalid(const pw_snapshot_t* s);
/* The snapshot's index-th line, 0 when there is none. */
int pw_format(const pw_snapshot_t* s, int index, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
