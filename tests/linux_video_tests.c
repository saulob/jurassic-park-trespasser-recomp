/* The game's own Smacker videos through the Linux HLE and FFmpeg, headless: the guest plays its intro and then newgame.smk
 * against the virtual window, the HLE counters say what was decoded and pushed to the mixer, and an Escape key-up skips each
 * video the way a player does. Real time (the dummy audio device keeps the clock), so each video runs a few seconds only.
 * Needs the installed game and an FFmpeg build; otherwise DATA UNAVAILABLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_boot.h"
#include "linux_log.h"
#include "linux_screen.h"
#include "linux_smack.h"
#include "linux_video_subtitles.h"
#include "jpt_user_data.h"
#include "linux_window.h"
#include "smacker_compat.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

enum { PLAY_FRAMES = 120, NEWGAME_FRAMES = 440, NEW_GAME_X = 310, NEW_GAME_Y = 105, AFTER_SKIP_FRAMES = 240 };

typedef enum { S_INTRO, S_INTRO_SKIPPED, S_MENU, S_NEWGAME, S_NEWGAME_SKIPPED, S_DONE } phase_t;

static struct {
    phase_t phase;
    unsigned mark, intro_decoded, intro_bytes, newgame_decoded, newgame_bytes;
    unsigned intro_frame, newgame_frame;
    const char* dump_prefix;
    unsigned intro_cues, newgame_cues, ghost_frames, grabbed_frames, grabbed_in_menu, relative_before_level, focus_lost, focus_back;
    int white_plain, white_cue, plain_done, cue_done, cue_after_intro, cue_after_newgame;
} T;

#define SAMPLE_PATH "build/linux_video_tests_user/sample.ppm"

/* Nearly white pixels in the lower half of the video picture: the subtitle's text on the dark videos. */
static int white_in_band(void) {
    FILE* f;
    int w = 0, h = 0, max = 0, count = 0;
    unsigned char* rgb;
    if (!lx_virtual_dump_ppm(SAMPLE_PATH) || !(f = fopen(SAMPLE_PATH, "rb"))) return -1;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || fgetc(f) == EOF) {
        fclose(f);
        return -1;
    }
    rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (rgb && fread(rgb, 1, (size_t)w * (size_t)h * 3, f) == (size_t)w * (size_t)h * 3)
        for (int y = 66 + 348 / 2; y < 66 + 348 && y < h; y++)
            for (int x = 0; x < w; x++) {
                const unsigned char* p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
                count += p[0] >= 245 && p[1] >= 245 && p[2] >= 245;
            }
    free(rgb);
    fclose(f);
    return count;
}

static void key(uint32_t vk, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static void focus(int gained) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_FOCUS, ev.down = gained;
    lx_virtual_queue_event(&ev);
}

static void mouse(lx_event_kind_t kind, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.x = NEW_GAME_X, ev.y = NEW_GAME_Y, ev.vk = 1, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static void dump(const char* name, unsigned frame) {
    char path[256];
    if (!T.dump_prefix) return;
    snprintf(path, sizeof path, "%s_%s_%u.ppm", T.dump_prefix, name, frame);
    lx_virtual_dump_ppm(path);
}

static void hook(unsigned frame, void* user) {
    smk_stats_t s;
    (void)user;
    int cue = lx_vsub_cue();
    const lx_virtual_state_t* host = lx_virtual_state();
    if (host->grabbed) T.grabbed_frames++;
    if (host->grabbed && T.phase == S_MENU) T.grabbed_in_menu++;
    if (host->relative && T.phase != S_DONE) T.relative_before_level++;
    smacker_hle_stats(&s);
    if (getenv("JPT_LINUX_VIDEO_TRACE") && frame % 50 == 0) {
        printf("  frame %u phase %d decoded %llu\n", frame, (int)T.phase, (unsigned long long)s.frames_decoded);
        fflush(stdout);
    }
    switch (T.phase) {
    case S_INTRO:
        if (cue >= 0) T.intro_cues |= 1u << cue;
        if (s.frames_decoded == 60 && !T.focus_lost) focus(0), T.focus_lost = 1;
        if (s.frames_decoded >= 75 && T.focus_lost && !T.focus_back) focus(1), T.focus_back = 1;
        if (s.frames_decoded >= PLAY_FRAMES) {
            T.intro_decoded = (unsigned)s.frames_decoded, T.intro_bytes = (unsigned)s.audio_bytes_pushed, T.intro_frame = frame;
            dump("intro", frame);
            key(0x1B, 1);
            key(0x1B, 0);
            T.phase = S_INTRO_SKIPPED;
        }
        break;
    case S_INTRO_SKIPPED:
        if (s.closed >= 1) T.phase = S_MENU, T.mark = frame, T.cue_after_intro = lx_vsub_cue();
        break;
    case S_MENU:
        if (cue >= 0) T.ghost_frames++;
        if (frame == T.mark + 300) mouse(LX_EV_MOUSE_MOVE, 0);
        if (frame == T.mark + 302) mouse(LX_EV_MOUSE_BUTTON, 1);
        if (frame == T.mark + 304) mouse(LX_EV_MOUSE_BUTTON, 0);
        if (s.opened >= 2) T.phase = S_NEWGAME, T.mark = (unsigned)s.frames_decoded;
        break;
    case S_NEWGAME:
        if (cue >= 0) T.newgame_cues |= 1u << cue;
        if (!T.plain_done && cue < 0 && s.frames_decoded >= T.mark + 200) T.white_plain = white_in_band(), T.plain_done = 1;
        if (!T.cue_done && cue >= 0) T.white_cue = white_in_band(), T.cue_done = 1;
        if (s.frames_decoded >= T.mark + NEWGAME_FRAMES) {
            T.newgame_decoded = (unsigned)s.frames_decoded - T.mark, T.newgame_bytes = (unsigned)s.audio_bytes_pushed, T.newgame_frame = frame;
            dump("newgame", frame);
            key(0x1B, 1);
            key(0x1B, 0);
            T.phase = S_NEWGAME_SKIPPED;
        }
        break;
    case S_NEWGAME_SKIPPED:
        if (s.closed >= 2) T.phase = S_DONE, T.mark = frame, T.cue_after_newgame = lx_vsub_cue();
        break;
    case S_DONE:
        if (cue >= 0) T.ghost_frames++;
        if (frame >= T.mark + AFTER_SKIP_FRAMES) lx_screen_set_frame_limit(frame);
        break;
    }
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    lx_boot_options_t o;
    lx_boot_result_t r;
    smk_stats_t s;
    memset(&o, 0, sizeof o);
    o.game_dir = argc > 1 ? argv[1] : env && *env ? env : "game";
    o.version = "en-116";
    o.user_root = "build/linux_video_tests_user";
    o.play_videos = 1;
    o.idle_limit = 0xFFFFFFFFu;
    o.frame_limit = 200000;
    o.frame_hook = hook;
    T.dump_prefix = getenv("JPT_LINUX_VIDEO_DUMP");
    jpt_make_dirs("build/linux_video_tests_user");

    if (!lx_smack_can_decode()) {
        printf("linux_video_tests: skipped - DATA UNAVAILABLE (this build has no FFmpeg: JPT_HAS_FFMPEG=0)\n");
        printf("linux_video_tests: 1 checks, 0 failures\n");
        return 0;
    }
    lx_boot_headless(&o, &r);
    if (r.status == LX_BOOT_DATA_UNAVAILABLE) {
        printf("linux_video_tests: skipped - DATA UNAVAILABLE (no guest image under %s)\n", o.game_dir);
        printf("linux_video_tests: 1 checks, 0 failures\n");
        return 0;
    }
    smacker_hle_stats(&s);
    printf("  run: %s (%s); SmackOpen %u, SmackClose %u, failed opens %u, decoded %llu, advanced %llu, audio %llu bytes in %u streams, EOF %u\n",
           lx_boot_status_name(r.status), r.detail, s.opened, s.closed, s.open_failed, (unsigned long long)s.frames_decoded, (unsigned long long)s.frames_advanced, (unsigned long long)s.audio_bytes_pushed,
           s.audio_streams, s.ended_by_eof);
    CHECK(r.status == LX_BOOT_FRAME_PRESENTED, "the run ends at the frame limit, not at a missing import: %s (%s)", lx_boot_status_name(r.status), r.detail);
    CHECK(T.intro_decoded >= PLAY_FRAMES, "the intro video was decoded (%u frames)", T.intro_decoded);
    CHECK(T.intro_bytes > 0, "its sound reached the mixer (%u bytes)", T.intro_bytes);
    CHECK(T.phase >= S_MENU, "an Escape key-up skips the intro and the menu follows (phase %d)", (int)T.phase);
    printf("  subtitles: intro cues seen 0x%X, newgame cues seen 0x%X, white pixels without a cue %d, with one %d, ghost frames %u\n", T.intro_cues,
           T.newgame_cues, T.white_plain, T.white_cue, T.ghost_frames);
    CHECK((T.intro_cues & 3u) == 3u, "the intro showed its first two cues while it played (0x%X)", T.intro_cues);
    CHECK((T.newgame_cues & 3u) == 3u, "newgame.smk showed its first two cues (0x%X)", T.newgame_cues);
    CHECK(T.white_cue >= 0 && T.white_cue > T.white_plain + 40, "the presented frame carries the subtitle's text (%d against %d white pixels)", T.white_cue, T.white_plain);
    CHECK(T.cue_after_intro == -1 && T.cue_after_newgame == -1, "a skipped video leaves no cue behind (%d, %d)", T.cue_after_intro, T.cue_after_newgame);
    CHECK(T.ghost_frames == 0, "no frame of the menu or the level carries a video subtitle (%u)", T.ghost_frames);
    CHECK(lx_vsub_cue() == -1, "and none at the end");
    CHECK(T.focus_lost && T.focus_back && T.intro_decoded >= PLAY_FRAMES, "focus was lost and regained while the intro played and it went on (%u frames)", T.intro_decoded);
    CHECK(T.relative_before_level == 0, "relative mouse mode never came on during the videos, the menu or the load (%u frames)", T.relative_before_level);
    CHECK(lx_virtual_state()->relative == 1, "and the level, where the game re-centres its cursor, has it on for mouse-look");
    CHECK(T.grabbed_frames == 0, "the host pointer is never confined by a video, the menu or the level load (%u frames, %u in the menu)", T.grabbed_frames, T.grabbed_in_menu);
    CHECK(T.newgame_decoded >= PLAY_FRAMES, "newgame.smk was opened by New Game and decoded (%u frames)", T.newgame_decoded);
    CHECK(T.newgame_bytes > T.intro_bytes, "newgame.smk's sound reached the mixer (%u bytes in all)", T.newgame_bytes);
    CHECK(T.phase == S_DONE, "the second skip ends the video and the game goes on to the level (phase %d)", (int)T.phase);
    CHECK(s.opened == 2 && s.closed == 2 && s.open_failed == 0, "two videos opened and closed, none failed (%u/%u/%u)", s.opened, s.closed, s.open_failed);
    CHECK(smacker_open_count() == 0, "no Smack object is left open");
    printf("linux_video_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
