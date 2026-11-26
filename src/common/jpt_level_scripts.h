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

/* At the session's start: with a custom level active, its package's scripts and the classes its worlds name. */
void jpt_level_scripts_start(jpt_session_kind_t kind, jpt_level_scripts_log_fn log);
/* save: the file being loaded (a host path), for JPT_REASON_LOAD_SAVE or the game's temporary save; NULL otherwise. */
void jpt_level_scripts_begin(jpt_level_reason_t reason, const char* scene, const char* save);
void jpt_level_scripts_ready(int ok);
void jpt_level_scripts_step(float seconds);
void jpt_level_scripts_end(void);
/* The custom session's scripts, NULL in the campaign. */
jpt_script_session* jpt_level_scripts_session(void);

#ifdef __cplusplus
}
#endif

#endif
