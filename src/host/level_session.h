/* The [session], [level] and [restart] lines of this run (jpt_level_session.h), fed by the host's hooks. */
#ifndef LEVEL_SESSION_H
#define LEVEL_SESSION_H

#include "jpt_level_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Once the renderer is known, before the guest runs. */
void level_session_start(const char* build, const char* renderer, const char* language);
/* The package's when the custom session holds a scene of that name, else the game's. */
jpt_level_source_t level_session_source(const char* scene);

#ifdef __cplusplus
}
#endif

#endif
