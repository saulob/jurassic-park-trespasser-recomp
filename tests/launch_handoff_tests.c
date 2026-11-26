/* PLAY to the game window (jpt_launch_handoff.c), headless: no window opens. Each step is checked for a moment without any of
 * our windows and for launcher controls that still take input. */
#include <stdio.h>
#include <string.h>

#include "jpt_launch_handoff.h"

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

/* Never zero surfaces from PLAY until the game window is ready, and no input to the launcher after PLAY. */
static void invariant(const jpt_handoff_t* h) {
    const int launching = h->state != JPT_HANDOFF_LAUNCHER_INTERACTIVE && h->state != JPT_HANDOFF_COMPLETE;
    CHECK(!launching || jpt_handoff_visible_surfaces(h) > 0);
    CHECK(!launching || !jpt_handoff_controls_enabled(h));
    CHECK(h->blank_moments == 0);
}

/* The launcher's side: PLAY, the handoff painted below it, then its own window closed. */
static void play(jpt_handoff_t* h, int custom, const char* level) {
    CHECK(jpt_handoff_controls_enabled(h) && jpt_handoff_cancel(h));
    CHECK(jpt_handoff_request(h, custom, level));
    invariant(h);
    CHECK(!jpt_handoff_request(h, custom, level) && !jpt_handoff_cancel(h));
    CHECK(!jpt_handoff_may_close_launcher(h));
    jpt_handoff_shown(h);
    invariant(h);
    CHECK(h->state == JPT_HANDOFF_VISIBLE && jpt_handoff_may_close_launcher(h));
    jpt_handoff_launcher_closed(h);
    invariant(h);
    CHECK(h->state == JPT_HANDOFF_GAME_INITIALIZING && h->handoff_visible && !h->launcher_visible);
}

/* The host's side up to a ready game window, with as many steps as it takes. */
static void start(jpt_handoff_t* h, int steps) {
    for (int i = 0; i < steps; i++) {
        jpt_handoff_phase(h, JPT_HANDOFF_PHASE_RUNTIME);
        invariant(h);
    }
    jpt_handoff_phase(h, JPT_HANDOFF_PHASE_RENDERER);
    CHECK(!strcmp(jpt_handoff_status(h), "Initializing renderer..."));
    jpt_handoff_phase(h, JPT_HANDOFF_PHASE_WINDOW);
    invariant(h);
    jpt_handoff_game_window_created(h);
    invariant(h);
    CHECK(h->state == JPT_HANDOFF_GAME_WINDOW_CREATED && !jpt_handoff_should_hide(h) && h->handoff_visible);
}

static void ready(jpt_handoff_t* h) {
    jpt_handoff_game_window_ready(h);
    invariant(h);
    CHECK(h->state == JPT_HANDOFF_GAME_WINDOW_READY && jpt_handoff_should_hide(h) && h->game_visible && h->handoff_visible);
    jpt_handoff_hidden(h);
    CHECK(h->state == JPT_HANDOFF_COMPLETE && !h->handoff_visible && h->game_visible && !h->blank_moments);
}

static void test_campaign_and_custom(void) {
    jpt_handoff_t campaign, custom;
    jpt_handoff_init(&campaign, 1);
    CHECK(campaign.state == JPT_HANDOFF_LAUNCHER_INTERACTIVE && jpt_handoff_visible_surfaces(&campaign) == 1);
    play(&campaign, 0, "ignored");
    CHECK(!strcmp(jpt_handoff_title(&campaign), "Jurassic Park: Trespasser") && !strcmp(jpt_handoff_status(&campaign), "Starting game...") && !campaign.level[0]);
    start(&campaign, 1);
    ready(&campaign);

    jpt_handoff_init(&custom, 1);
    play(&custom, 1, "");
    CHECK(!strcmp(jpt_handoff_status(&custom), "Starting custom level..."));
    jpt_handoff_init(&custom, 1);
    play(&custom, 1, "Some_Package");
    CHECK(!strcmp(jpt_handoff_status(&custom), "Loading %s...") && !strcmp(custom.level, "Some_Package"));
    jpt_handoff_phase(&custom, JPT_HANDOFF_PHASE_RUNTIME);
    CHECK(!strcmp(jpt_handoff_status(&custom), "Preparing runtime..."));
    start(&custom, 1);
    CHECK(!strcmp(jpt_handoff_status(&custom), "Creating game window..."));
    ready(&custom);
    /* The same steps for both: the mechanism does not depend on what is played. */
    CHECK(campaign.state == custom.state && campaign.game_visible == custom.game_visible);
}

/* Slow or fast, the handoff stays until the game window is ready; a created window is not yet a ready one. */
static void test_speeds(void) {
    jpt_handoff_t h;
    jpt_handoff_init(&h, 1);
    play(&h, 0, NULL);
    start(&h, 5000);
    CHECK(h.handoff_visible && !h.game_visible && !jpt_handoff_should_hide(&h));
    ready(&h);

    /* Fast: the game window is ready before the handoff could even report its phases. */
    jpt_handoff_init(&h, 1);
    play(&h, 0, NULL);
    jpt_handoff_game_window_ready(&h);
    invariant(&h);
    CHECK(jpt_handoff_should_hide(&h));
    jpt_handoff_phase(&h, JPT_HANDOFF_PHASE_RENDERER);
    jpt_handoff_hidden(&h);
    CHECK(h.state == JPT_HANDOFF_COMPLETE);

    /* Phases never go back. */
    jpt_handoff_init(&h, 1);
    play(&h, 0, NULL);
    jpt_handoff_phase(&h, JPT_HANDOFF_PHASE_WINDOW);
    jpt_handoff_phase(&h, JPT_HANDOFF_PHASE_RUNTIME);
    CHECK(h.phase == JPT_HANDOFF_PHASE_WINDOW);
}

/* The black frame and the first present are the same signal; whichever comes first ends the handoff, the second changes nothing. */
static void test_black_frame_and_first_present(void) {
    jpt_handoff_t h;
    jpt_handoff_init(&h, 1);
    play(&h, 1, "Pkg");
    start(&h, 1);
    ready(&h);
    jpt_handoff_game_window_ready(&h);
    jpt_handoff_hidden(&h);
    CHECK(h.state == JPT_HANDOFF_COMPLETE && !h.blank_moments);
}

/* D3D11 and Vulkan reach the same states: the handoff only sees the game window. */
static void test_renderers(void) {
    const char* renderers[] = { "d3d11", "vulkan", "software" };
    jpt_handoff_state_t states[3];
    for (int i = 0; i < 3; i++) {
        jpt_handoff_t h;
        jpt_handoff_init(&h, 1);
        play(&h, i == 1, renderers[i]);
        start(&h, i + 1);
        ready(&h);
        states[i] = h.state;
    }
    CHECK(states[0] == states[1] && states[1] == states[2] && states[0] == JPT_HANDOFF_COMPLETE);
}

/* A failure before the window: the handoff says so and stays until the player closes it; no endless spinner. */
static void test_failure(void) {
    jpt_handoff_t h;
    jpt_handoff_init(&h, 1);
    play(&h, 0, NULL);
    jpt_handoff_phase(&h, JPT_HANDOFF_PHASE_RENDERER);
    jpt_handoff_fail(&h, "the renderer could not start");
    CHECK(h.state == JPT_HANDOFF_FAILED && !strcmp(h.stage, "renderer") && !strcmp(h.reason, "the renderer could not start"));
    CHECK(h.handoff_visible && !strcmp(jpt_handoff_title(&h), "The game could not start") && strstr(jpt_handoff_status(&h), "Bug Report"));
    invariant(&h);
    jpt_handoff_phase(&h, JPT_HANDOFF_PHASE_WINDOW);
    jpt_handoff_game_window_ready(&h);
    CHECK(h.state == JPT_HANDOFF_FAILED && h.phase == JPT_HANDOFF_PHASE_RENDERER);
    jpt_handoff_dismissed(&h);
    CHECK(h.state == JPT_HANDOFF_COMPLETE && !jpt_handoff_visible_surfaces(&h));

    /* After a ready window there is nothing to fail; before PLAY neither. */
    jpt_handoff_init(&h, 1);
    jpt_handoff_fail(&h, "x");
    CHECK(h.state == JPT_HANDOFF_LAUNCHER_INTERACTIVE);
    play(&h, 0, NULL);
    start(&h, 1);
    ready(&h);
    jpt_handoff_fail(&h, "late");
    CHECK(h.state == JPT_HANDOFF_COMPLETE && !h.reason[0]);

    /* A failure while the game window exists but is not ready. */
    jpt_handoff_init(&h, 1);
    play(&h, 1, "Pkg");
    start(&h, 1);
    jpt_handoff_fail(&h, NULL);
    CHECK(h.state == JPT_HANDOFF_FAILED && !strcmp(h.stage, "game_window") && !strcmp(h.reason, "unknown"));
}

/* Without a handoff window (it could not be shown, or the platform has none) the launcher may close, and the gap is counted. */
static void test_without_presenter(void) {
    jpt_handoff_t h;
    jpt_handoff_init(&h, 0);
    CHECK(jpt_handoff_request(&h, 0, NULL) && jpt_handoff_may_close_launcher(&h));
    jpt_handoff_launcher_closed(&h);
    CHECK(h.blank_moments == 1 && !jpt_handoff_visible_surfaces(&h));
    jpt_handoff_game_window_ready(&h);
    CHECK(h.state == JPT_HANDOFF_GAME_WINDOW_READY && !jpt_handoff_should_hide(&h) && jpt_handoff_visible_surfaces(&h) == 1);

    /* Closing the launcher before the handoff is painted is exactly what may_close forbids. */
    jpt_handoff_init(&h, 1);
    jpt_handoff_request(&h, 0, NULL);
    CHECK(!jpt_handoff_may_close_launcher(&h));
    jpt_handoff_launcher_closed(&h);
    CHECK(h.blank_moments == 1);
}

/* Shutdown and a second PLAY: only one launch, and the state names for the log. */
static void test_shutdown_and_names(void) {
    jpt_handoff_t h;
    jpt_handoff_init(&h, 1);
    play(&h, 0, NULL);
    start(&h, 1);
    ready(&h);
    CHECK(!jpt_handoff_request(&h, 0, NULL) && !jpt_handoff_controls_enabled(&h));
    jpt_handoff_launcher_closed(&h);
    CHECK(h.state == JPT_HANDOFF_COMPLETE && !h.blank_moments);
    CHECK(!strcmp(jpt_handoff_state_name(JPT_HANDOFF_LAUNCHER_INTERACTIVE), "LAUNCHER_INTERACTIVE"));
    CHECK(!strcmp(jpt_handoff_state_name(JPT_HANDOFF_GAME_WINDOW_READY), "GAME_WINDOW_READY"));
    CHECK(!strcmp(jpt_handoff_state_name(JPT_HANDOFF_COMPLETE), "HANDOFF_COMPLETE"));
    CHECK(!strcmp(jpt_handoff_phase_stage(JPT_HANDOFF_PHASE_RUNTIME), "runtime"));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_campaign_and_custom();
    test_speeds();
    test_black_frame_and_first_present();
    test_renderers();
    test_failure();
    test_without_presenter();
    test_shutdown_and_names();
    printf("launch_handoff_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
