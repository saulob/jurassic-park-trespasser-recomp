#include "guest_abi.h"
#include "guest_features.h"

#define VIDEO_PLAY_VA GVA(0x00418AE0) /* CVideoWnd::Play(name, ...), thiscall */
#define NAME_MAX_BYTES 64

static int s_enabled;
static recomp_func_t s_play_lifted;

void skip_intro_configure(int enabled) { s_enabled = enabled != 0; }

/* The game's own NoVideo branch: one message pumped, Play returns 1 and the caller goes on as after the video. */
int skip_intro_try(const char* guest_name, int video_test_request, recomp_func_t lifted) {
    intro_video_t video = intro_video_of(guest_name);
    if (!skip_intro_applies(s_enabled, video, video_test_request)) return 0;
    guest_skip_video(1);
    lifted();
    guest_skip_video(0);
    host_log("video", "%s skipped (Skip Intro): the game's own no-video path, %s", intro_video_name(video),
             video == INTRO_STARTUP ? "the main menu follows" : "the level loads next");
    if (g_eax != 1) host_log("video", "warning: Play returned %u for the skipped %s", g_eax, intro_video_name(video));
    return 1;
}

static void play(void) {
    char name[NAME_MAX_BYTES];
    if (g_alias_entry) {
        s_play_lifted();
        return;
    }
    if (!guest_read_string(MEM32(g_esp + 4), name, sizeof name)) name[0] = 0;
    if (!skip_intro_try(name, 0, s_play_lifted)) s_play_lifted();
}

int skip_intro_install(void) {
    if (!s_enabled) return 1;
    if (guest_override_install(VIDEO_PLAY_VA, play, &s_play_lifted)) return 1;
    host_log("video", "warning: CVideoWnd::Play could not be hooked: Skip Intro is off");
    return 0;
}
