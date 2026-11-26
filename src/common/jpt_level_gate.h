/* Whether a level or a save may be loaded, asked before the engine touches anything: what it cannot load is refused while the
 * game is still as it was. */
#ifndef JPT_LEVEL_GATE_H
#define JPT_LEVEL_GATE_H

#include <stddef.h>

#include "jpt_save.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JPT_GATE_OPEN, JPT_GATE_DAMAGED, JPT_GATE_INCOMPATIBLE, JPT_GATE_MISSING, JPT_GATE_UNSAFE } jpt_gate_t;

/* For the player; NULL when it is open. */
const char* jpt_gate_message(jpt_gate_t gate);
/* file: the host's path of the scene or save; asked: the game's. why: for the log. */
jpt_gate_t jpt_gate_load(const char* file, const char* asked, char* why, size_t cap);
/* Of a file in the saves folder: whether the game's list may show it. */
int jpt_gate_lists(const char* file, char* why, size_t cap);
/* Whether the game may read it, as a save or a level state of its own. */
int jpt_gate_reads(const char* file, char* why, size_t cap);

/* A save being written, between the game's open and close: 0 when the file is not one. */
int jpt_gate_save_begin(const char* path, char* temp, size_t cap);
int jpt_gate_save_end(const char* path, const char* temp, char* why, size_t cap);
/* Whether the last save written was kept back; asking forgets it. */
int jpt_gate_save_failed(void);

#ifdef __cplusplus
}
#endif

#endif
