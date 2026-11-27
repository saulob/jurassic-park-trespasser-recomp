/* The Trespasser CE scripts of a custom session as the host's level hooks drive them (docs/CE_SCRIPTS.md). A campaign session
 * never creates their session: every call below returns at once, and no save of it gets a script block. */
#ifndef JPT_LEVEL_SCRIPTS_H
#define JPT_LEVEL_SCRIPTS_H

#include <stddef.h>

#include "jpt_level_session.h"
#include "jpt_script_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*jpt_level_scripts_log_fn)(const char* line);
struct jpt_script_engine;

/* The game's world as CE's API reaches it (src/host/script_engine.c), for the sessions started after. */
void jpt_level_scripts_set_engine(const struct jpt_script_engine* engine);

/* At the session's start: with a custom level active, its package's scripts and the classes its worlds name. */
void jpt_level_scripts_start(jpt_session_kind_t kind, jpt_level_scripts_log_fn log);
/* save: the file being loaded (a host path), for JPT_REASON_LOAD_SAVE or the game's temporary save; NULL otherwise. */
void jpt_level_scripts_begin(jpt_level_reason_t reason, const char* scene, const char* save);
void jpt_level_scripts_ready(int ok);
void jpt_level_scripts_step(float seconds);
/* A frame of gameplay: OnPaintHW3D; the frame of the last paint (NULL without a session), as the renderers draw it. */
void jpt_level_scripts_paint(void);
const jpt_draw_frame_t* jpt_level_scripts_frame(void);
/* The game read its controls; out says whether a script is modal. */
void jpt_level_scripts_input(const jpt_script_input* in, jpt_script_input_result* out);
/* A key while a script is modal: 1 when a script took it. */
int jpt_level_scripts_key(unsigned vk, int down, int shift, int ctrl, int alt);
unsigned jpt_level_scripts_user_commands(void);
/* The player's CPlayer script, by the player's unique handle: whether the level has one that changes its movement; the walk
 * speed factor of a request (unchanged without one); the jump scale of a request that asks to jump (1 without one); the
 * player jumped. */
int jpt_level_scripts_drives_player(void);
float jpt_level_scripts_speed_factor(unsigned player, float speed);
float jpt_level_scripts_jump_factor(unsigned player);
void jpt_level_scripts_jump(unsigned player);
/* A console command the game does not know, for OnConsoleCommand: 1 when a script handled it. The port has no console. */
int jpt_level_scripts_console(const char* command, const char* args);
/* The AI's Bite or Feint action of an animal (unique handles), for OnActBite. */
void jpt_level_scripts_act_bite(unsigned animal, unsigned target, const float location[3], float distance, int feint);
void jpt_level_scripts_end(void);
/* The custom session's scripts, NULL in the campaign. */
jpt_script_session* jpt_level_scripts_session(void);

#ifdef __cplusplus
}
#endif

#endif
