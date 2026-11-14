/* DEBUG_SAULO: physics_watch.c, the temporary player/hand telemetry (physics_watch_policy.h). */
#ifndef PHYSICS_WATCH_H
#define PHYSICS_WATCH_H

#include <stdint.h>

#include "cheats_policy.h"

typedef struct {
    float move_x, move_y;   /* the SInput the game gets this read */
    uint32_t state, hit;
    uint32_t pad_buttons;
    int lt, mouse_hand;
    float stick_x, stick_y; /* the right stick */
    int interface_up;
} pw_input_t;

/* After each CInput read: the stuck detector, the sanity checks and a requested snapshot. */
void physics_watch_read(const pw_input_t* in);
/* F9: a snapshot at the next gameplay read. */
void physics_watch_request(void);
/* A camera mode change of F10: where the player, the camera and the hand were. */
void physics_watch_camera(ch_camera_t from, ch_camera_t to);

#endif
