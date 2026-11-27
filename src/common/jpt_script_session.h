/* The Trespasser CE scripts of a custom session (docs/CE_SCRIPTS.md): one VM for a level whose scripts it needs, created
 * when the level begins to load and destroyed when the next one begins, a restart or a save is loaded, or the session ends.
 * A level that needs none has no VM. Only the nvram tables of game scripts outlive a VM: they travel to the next level, are
 * kept at a level's start for its restart, and go into saves with the level's scripts' own. The campaign never makes one.
 * Every call comes from the game's thread, at the host's level, frame and save hooks. */
#ifndef JPT_SCRIPT_SESSION_H
#define JPT_SCRIPT_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_script_draw.h"
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
    const struct jpt_script_engine* engine; /* the game's world; NULL: CE's API raises script errors */
    unsigned random_seed;
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
/* The AI of the animal of that unique handle executes a Bite (feint 0) or Feint action at its target: OnActBite of its instance
 * script, when it has one. */
void jpt_script_level_act_bite(jpt_script_session* s, unsigned animal, unsigned target, const float location[3], float distance, int feint);
/* The CPlayer instance script of the player (its unique handle), as CE calls it from the player's own code:
 * fGetSpeedFactor(speed) for the walk speed of every movement request (speed unchanged without it, when it fails or returns no
 * finite number; never below 0); fGetJumpFactor() for a request that asks to jump (1 likewise); OnJump when the player's own
 * jump left the ground. drives_player: the level has such a script with either factor. */
int jpt_script_level_drives_player(const jpt_script_session* s);
float jpt_script_level_speed_factor(jpt_script_session* s, unsigned player, float speed);
float jpt_script_level_jump_factor(jpt_script_session* s, unsigned player);
void jpt_script_level_jump(jpt_script_session* s, unsigned player);
/* A console command the game does not know: OnConsoleCommand(command, args) of the game scripts in their order until one returns
 * true (a bool); 1 when one did. An empty command reaches none; NULL args are "". */
int jpt_script_level_console(jpt_script_session* s, const char* command, const char* args);
/* A frame of gameplay: OnPaintHW3D of the game scripts that declare it; the commands they drew, valid until the next paint or level. */
const jpt_draw_frame_t* jpt_script_level_paint(jpt_script_session* s);
/* The frame of the last paint; empty once its VM is gone. */
const jpt_draw_frame_t* jpt_script_level_paint_frame(const jpt_script_session* s);

/* The game read its controls: the level's uCMD_USER keys (bit n for USER n+1), those pressed since the last frame, and the movement
 * input before any speed factor. */
typedef struct {
    uint32_t user_held, user_hit;
    float move_x, move_y;
} jpt_script_input;
typedef struct {
    int modal; /* a game script is modal: the player's controls do nothing */
    int look;  /* ... but the mouse still looks around (OnHandleModalInput returned true) */
} jpt_script_input_result;
void jpt_script_level_input(jpt_script_session* s, const jpt_script_input* in, jpt_script_input_result* out);
/* A key of the keyboard (a virtual key code) while a game script is modal: 1 when the script took it. Escape ends modal mode. */
int jpt_script_level_key(jpt_script_session* s, unsigned vk, int down, int shift, int ctrl, int alt);
int jpt_script_level_modal(const jpt_script_session* s);
/* The uCMD_USER commands the current level's scripts use (bit n for USER n+1); 0 without a VM. */
unsigned jpt_script_session_user_commands(const jpt_script_session* s);

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
