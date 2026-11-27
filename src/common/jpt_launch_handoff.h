/* From PLAY to the game's own window: which of our surfaces shows meanwhile. The launcher, then a handoff window, then the game
 * window; from the click until the game window is ready at least one of them is visible, and the launcher takes no input. */
#ifndef JPT_LAUNCH_HANDOFF_H
#define JPT_LAUNCH_HANDOFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPT_HANDOFF_LAUNCHER_INTERACTIVE,
    JPT_HANDOFF_LAUNCH_REQUESTED,
    JPT_HANDOFF_VISIBLE,
    JPT_HANDOFF_GAME_INITIALIZING,
    JPT_HANDOFF_GAME_WINDOW_CREATED,
    JPT_HANDOFF_GAME_WINDOW_READY,
    JPT_HANDOFF_COMPLETE,
    JPT_HANDOFF_FAILED
} jpt_handoff_state_t;

/* Only phases the host really goes through, in order. */
typedef enum {
    JPT_HANDOFF_PHASE_STARTING,
    JPT_HANDOFF_PHASE_RUNTIME,
    JPT_HANDOFF_PHASE_RENDERER,
    JPT_HANDOFF_PHASE_WINDOW
} jpt_handoff_phase_t;

#define JPT_HANDOFF_TEXT_MAX 160

typedef struct {
    jpt_handoff_state_t state;
    jpt_handoff_phase_t phase;
    int presenter; /* this platform can show the handoff window */
    int custom;
    int launcher_visible, handoff_visible, game_visible;
    unsigned blank_moments; /* times no surface was left between the click and the game window */
    char level[64];
    char stage[32];
    char reason[JPT_HANDOFF_TEXT_MAX];
} jpt_handoff_t;

const char* jpt_handoff_state_name(jpt_handoff_state_t state);
const char* jpt_handoff_phase_stage(jpt_handoff_phase_t phase);

void jpt_handoff_init(jpt_handoff_t* h, int presenter);
int jpt_handoff_controls_enabled(const jpt_handoff_t* h);
/* PLAY: 0 when a launch is already under way. level: a custom level's scene, NULL or "" for the game. */
int jpt_handoff_request(jpt_handoff_t* h, int custom, const char* level);
/* Before the game is started nothing is lost; after PLAY there is no way back. */
int jpt_handoff_cancel(jpt_handoff_t* h);
void jpt_handoff_shown(jpt_handoff_t* h);
int jpt_handoff_may_close_launcher(const jpt_handoff_t* h);
void jpt_handoff_launcher_closed(jpt_handoff_t* h);
void jpt_handoff_phase(jpt_handoff_t* h, jpt_handoff_phase_t phase);
void jpt_handoff_game_window_created(jpt_handoff_t* h);
/* Shown black or with its first frame: either is a valid picture. */
void jpt_handoff_game_window_ready(jpt_handoff_t* h);
int jpt_handoff_should_hide(const jpt_handoff_t* h);
void jpt_handoff_hidden(jpt_handoff_t* h);
/* The game stopped before its window was ready: the handoff window says so until the player closes it. */
void jpt_handoff_fail(jpt_handoff_t* h, const char* reason);
void jpt_handoff_dismissed(jpt_handoff_t* h);
int jpt_handoff_visible_surfaces(const jpt_handoff_t* h);
/* What the handoff window shows, as English texts to translate: a title and a status, which may hold one %s for the level. */
const char* jpt_handoff_title(const jpt_handoff_t* h);
const char* jpt_handoff_status(const jpt_handoff_t* h);

#ifdef __cplusplus
}
#endif

#endif
