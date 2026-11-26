/* The Trespasser CE scripts of a custom session (docs/CE_SCRIPTS.md): one VM for a level whose scripts it needs, created
 * when the level begins to load and destroyed when the next one begins, a restart or a save is loaded, or the session ends.
 * A level that needs none has no VM. Only the nvram tables of game scripts outlive a VM: they travel to the next level, are
 * kept at a level's start for its restart, and go into saves with the level's scripts' own. The campaign never makes one.
 * Every call comes from the game's thread, at the host's level, frame and save hooks. */
#ifndef JPT_SCRIPT_SESSION_H
#define JPT_SCRIPT_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_script_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpt_script_session jpt_script_session;
/* A script file of the package by its path in it: from malloc, or NULL when it cannot be read. */
typedef char* (*jpt_script_read_fn)(void* user, const char* path, size_t* size);

typedef struct {
    jpt_script_log_fn log;
    void* user;
    jpt_script_read_fn read;
    long instructions;
    size_t memory_limit;
    const struct jpt_script_allocator* allocator;
    int per_level_files; /* a level reads only the files given for it (jpt_script_session_add_level_file) */
} jpt_script_session_config;

typedef enum {
    JPT_SCRIPT_BEGIN_NEW_GAME,   /* the package's first level, or a level chosen to start from */
    JPT_SCRIPT_BEGIN_TRANSITION, /* the level the one before leads to */
    JPT_SCRIPT_BEGIN_RESTART,    /* Restart Level: the level from its start */
    JPT_SCRIPT_BEGIN_SAVE        /* a saved game */
} jpt_script_begin;

jpt_script_session* jpt_script_session_create(const jpt_script_session_config* config);
/* The session's end: the VM's scripts are told and it is destroyed; nothing is kept. */
void jpt_script_session_destroy(jpt_script_session* s);

/* The package's scripts in load order, and what its worlds name (Script, action 99's ScriptAT), before the first level. */
void jpt_script_session_add_file(jpt_script_session* s, const char* path);
void jpt_script_session_add_reference(jpt_script_session* s, const char* scene, const char* class_name, const char* object, int action);
/* The files a level's VM loads, as the launcher planned them. Once one is given, a level reads only its own and a level without
 * any reads none; without, every file of the package is a candidate. */
void jpt_script_session_add_level_file(jpt_script_session* s, const char* scene, const char* path);

/* A level begins to load (scene: its file name). state: the script block of the save being loaded, or NULL. */
void jpt_script_level_begin(jpt_script_session* s, const char* scene, jpt_script_begin why, const uint8_t* state, size_t size);
/* The game's load returned; ok: the level is there. */
void jpt_script_level_ready(jpt_script_session* s, int ok);
/* The simulation moved on by seconds. */
void jpt_script_level_step(jpt_script_session* s, float seconds);
/* The script block of a save being written (from malloc), or NULL when there is nothing to keep. */
uint8_t* jpt_script_session_save(jpt_script_session* s, size_t* size);

int jpt_script_session_has_vm(const jpt_script_session* s);
/* Counts the VMs created; 0 before the first. */
unsigned jpt_script_session_generation(const jpt_script_session* s);
/* Of the current VM; 0 without one. */
int jpt_script_session_memory(const jpt_script_session* s, jpt_script_memory* out);
/* The scripts of the current VM, and how many of them are called every step. */
int jpt_script_session_scripts(const jpt_script_session* s, int* stepping);

#ifdef __cplusplus
}
#endif

#endif
