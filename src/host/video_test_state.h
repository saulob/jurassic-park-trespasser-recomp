#ifndef TRESPASSER_VIDEO_TEST_STATE_H
#define TRESPASSER_VIDEO_TEST_STATE_H

#include <stdint.h>

#include "video_layout.h"

#define VT_VIDEO_COUNT 4

typedef struct {
    const char* file;
    unsigned name_id; /* GVA id of the name string the game itself passes to CVideoWnd::Play */
} vt_video_t;

extern const vt_video_t vt_videos[VT_VIDEO_COUNT];

typedef struct {
    int open;    /* F5 opened it in the main menu */
    int playing; /* a playback it asked for runs */
    int pending;
    int video;
    vl_play_mode_t mode;
} vt_state_t;

/* F5: 1 when it opened or closed the panel; outside the main menu it does nothing. */
int vt_f5(vt_state_t* s, int main_menu_front);
/* A button of the panel; 0 while another playback waits or runs. */
int vt_request(vt_state_t* s, int video, vl_play_mode_t mode);
/* At the menu's frame: 1 when the waiting playback starts now. */
int vt_start(vt_state_t* s, int main_menu_front, int* video, vl_play_mode_t* mode);
void vt_finished(vt_state_t* s);
int vt_visible(const vt_state_t* s);

#endif
