#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "jpt_launch_handoff.h"

const char* jpt_handoff_state_name(jpt_handoff_state_t state) {
    switch (state) {
    case JPT_HANDOFF_LAUNCHER_INTERACTIVE: return "LAUNCHER_INTERACTIVE";
    case JPT_HANDOFF_LAUNCH_REQUESTED: return "LAUNCH_REQUESTED";
    case JPT_HANDOFF_VISIBLE: return "HANDOFF_VISIBLE";
    case JPT_HANDOFF_GAME_INITIALIZING: return "GAME_INITIALIZING";
    case JPT_HANDOFF_GAME_WINDOW_CREATED: return "GAME_WINDOW_CREATED";
    case JPT_HANDOFF_GAME_WINDOW_READY: return "GAME_WINDOW_READY";
    case JPT_HANDOFF_COMPLETE: return "HANDOFF_COMPLETE";
    case JPT_HANDOFF_FAILED: return "FAILED";
    }
    return "?";
}

const char* jpt_handoff_phase_stage(jpt_handoff_phase_t phase) {
    switch (phase) {
    case JPT_HANDOFF_PHASE_STARTING: return "starting";
    case JPT_HANDOFF_PHASE_RUNTIME: return "runtime";
    case JPT_HANDOFF_PHASE_RENDERER: return "renderer";
    case JPT_HANDOFF_PHASE_WINDOW: return "game_window";
    }
    return "?";
}

static int launching(const jpt_handoff_t* h) { return h->state != JPT_HANDOFF_LAUNCHER_INTERACTIVE && h->state != JPT_HANDOFF_COMPLETE; }

static void check_visible(jpt_handoff_t* h) {
    if (launching(h) && !jpt_handoff_visible_surfaces(h)) h->blank_moments++;
}

void jpt_handoff_init(jpt_handoff_t* h, int presenter) {
    memset(h, 0, sizeof *h);
    h->presenter = presenter;
    h->launcher_visible = 1;
}

int jpt_handoff_controls_enabled(const jpt_handoff_t* h) { return h->state == JPT_HANDOFF_LAUNCHER_INTERACTIVE; }

int jpt_handoff_request(jpt_handoff_t* h, int custom, const char* level) {
    if (h->state != JPT_HANDOFF_LAUNCHER_INTERACTIVE) return 0;
    h->state = JPT_HANDOFF_LAUNCH_REQUESTED;
    h->custom = custom;
    snprintf(h->level, sizeof h->level, "%s", custom && level ? level : "");
    return 1;
}

int jpt_handoff_cancel(jpt_handoff_t* h) { return h->state == JPT_HANDOFF_LAUNCHER_INTERACTIVE; }

void jpt_handoff_shown(jpt_handoff_t* h) {
    if (h->state != JPT_HANDOFF_LAUNCH_REQUESTED) return;
    h->handoff_visible = 1;
    h->state = JPT_HANDOFF_VISIBLE;
}

int jpt_handoff_may_close_launcher(const jpt_handoff_t* h) {
    return h->state != JPT_HANDOFF_LAUNCHER_INTERACTIVE && (h->handoff_visible || h->game_visible || !h->presenter);
}

void jpt_handoff_launcher_closed(jpt_handoff_t* h) {
    h->launcher_visible = 0;
    if (h->state == JPT_HANDOFF_LAUNCH_REQUESTED || h->state == JPT_HANDOFF_VISIBLE) h->state = JPT_HANDOFF_GAME_INITIALIZING;
    check_visible(h);
}

void jpt_handoff_phase(jpt_handoff_t* h, jpt_handoff_phase_t phase) {
    if (h->state == JPT_HANDOFF_FAILED || h->state == JPT_HANDOFF_COMPLETE || phase < h->phase) return;
    h->phase = phase;
}

void jpt_handoff_game_window_created(jpt_handoff_t* h) {
    if (h->state == JPT_HANDOFF_GAME_INITIALIZING || h->state == JPT_HANDOFF_VISIBLE || h->state == JPT_HANDOFF_LAUNCH_REQUESTED) {
        h->state = JPT_HANDOFF_GAME_WINDOW_CREATED;
        h->phase = JPT_HANDOFF_PHASE_WINDOW;
    }
}

void jpt_handoff_game_window_ready(jpt_handoff_t* h) {
    if (h->state == JPT_HANDOFF_FAILED || h->state == JPT_HANDOFF_COMPLETE) return;
    h->game_visible = 1;
    if (h->state != JPT_HANDOFF_LAUNCHER_INTERACTIVE) h->state = JPT_HANDOFF_GAME_WINDOW_READY;
}

int jpt_handoff_should_hide(const jpt_handoff_t* h) { return h->handoff_visible && h->game_visible && h->state == JPT_HANDOFF_GAME_WINDOW_READY; }

void jpt_handoff_hidden(jpt_handoff_t* h) {
    h->handoff_visible = 0;
    if (h->state == JPT_HANDOFF_GAME_WINDOW_READY) h->state = JPT_HANDOFF_COMPLETE;
    check_visible(h);
}

void jpt_handoff_fail(jpt_handoff_t* h, const char* reason) {
    if (h->state == JPT_HANDOFF_COMPLETE || h->state == JPT_HANDOFF_FAILED || h->state == JPT_HANDOFF_LAUNCHER_INTERACTIVE) return;
    snprintf(h->stage, sizeof h->stage, "%s", jpt_handoff_phase_stage(h->phase));
    snprintf(h->reason, sizeof h->reason, "%s", reason && *reason ? reason : "unknown");
    h->state = JPT_HANDOFF_FAILED;
}

void jpt_handoff_dismissed(jpt_handoff_t* h) {
    h->handoff_visible = 0;
    if (h->state == JPT_HANDOFF_FAILED) h->state = JPT_HANDOFF_COMPLETE;
}

int jpt_handoff_visible_surfaces(const jpt_handoff_t* h) { return h->launcher_visible + h->handoff_visible + h->game_visible; }

const char* jpt_handoff_title(const jpt_handoff_t* h) {
    if (h->state == JPT_HANDOFF_FAILED) return "The game could not start";
    return "Jurassic Park: Trespasser";
}

const char* jpt_handoff_status(const jpt_handoff_t* h) {
    if (h->state == JPT_HANDOFF_FAILED) return "The launcher's Bug Report has the log of this run.";
    switch (h->phase) {
    case JPT_HANDOFF_PHASE_RUNTIME: return "Preparing runtime...";
    case JPT_HANDOFF_PHASE_RENDERER: return "Initializing renderer...";
    case JPT_HANDOFF_PHASE_WINDOW: return "Creating game window...";
    default: break;
    }
    if (h->custom && h->level[0]) return "Loading %s...";
    return h->custom ? "Starting custom level..." : "Starting game...";
}
