#include "guest_va.h"
#include "video_test_state.h"

const vt_video_t vt_videos[VT_VIDEO_COUNT] = {
    { "tpassintro.smk", GVA_ID(0x00660B88) },
    { "newgame.smk", GVA_ID(0x00661298) },
    { "credits.smk", GVA_ID(0x00660B98) },
    { "win.smk", GVA_ID(0x00660BA8) },
};

int vt_f5(vt_state_t* s, int main_menu_front) {
    if (s->playing) return 0;
    if (s->open) {
        s->open = 0;
        s->pending = 0;
        return 1;
    }
    if (!main_menu_front) return 0;
    s->open = 1;
    return 1;
}

int vt_request(vt_state_t* s, int video, vl_play_mode_t mode) {
    if (!s->open || s->playing || s->pending || video < 0 || video >= VT_VIDEO_COUNT || mode == VL_PLAY_AUTO) return 0;
    s->pending = 1;
    s->video = video;
    s->mode = mode;
    return 1;
}

int vt_start(vt_state_t* s, int main_menu_front, int* video, vl_play_mode_t* mode) {
    if (!s->pending || s->playing || !main_menu_front) return 0;
    s->pending = 0;
    s->playing = 1;
    *video = s->video;
    *mode = s->mode;
    return 1;
}

void vt_finished(vt_state_t* s) { s->playing = 0; }

int vt_visible(const vt_state_t* s) { return s->open && !s->playing; }
