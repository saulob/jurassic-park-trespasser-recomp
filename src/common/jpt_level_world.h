/* The world a custom level loads, against the texture set the game will read with it: what the engine cannot survive is changed
 * in memory as it is read, and the files stay as they are. */
#ifndef JPT_LEVEL_WORLD_H
#define JPT_LEVEL_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "jpt_custom_level.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char world[JPT_CUSTOM_LEVEL_PATH_MAX]; /* its file name */
    char set[JPT_CUSTOM_LEVEL_PATH_MAX];   /* of its textures; empty when there is none */
    char terrain[JPT_CUSTOM_LEVEL_PATH_MAX]; /* "x.wtd"; empty when it names none */
    char problem[128];
    int failed; /* the engine must not read it */
    int borrowed; /* the set is the installed game's */
    unsigned textures, textures_missing;
    unsigned curved, curved_missing, flattened, hidden;
} jpt_level_world_t;

/* "x-130.pid" for the world file "x.grf", wherever it is. */
int jpt_level_world_set_name(const char* world, char* out, size_t cap);
/* A package's expanded world as the game is about to read it; bytes are changed in place. */
void jpt_level_world_prepare(const char* path, uint8_t* bytes, size_t size, jpt_level_world_t* out);
/* A world of the installed game that the session reads as it is: its textures are known by name. */
void jpt_level_world_original(const char* host);
/* The texture directory of the world prepared last, from malloc; NULL when the game reads the file itself. */
uint8_t* jpt_level_world_take_set(const char* path, size_t* size);
/* Whether the world prepared last cannot be built without this file. */
int jpt_level_world_needs(const char* path);
/* Its report, for the log: 0 past the last line. */
int jpt_level_world_line(const jpt_level_world_t* world, int index, char* out, size_t cap);
void jpt_level_world_forget(void);

/* A file, expanded when it is packed, from malloc. */
uint8_t* jpt_level_world_load(const char* path, size_t* size);
uint32_t jpt_level_world_file_hash(const char* path, uint32_t seed);
/* What a world file needs to be built; problem: why it cannot be read (0), or what of it the engine cannot hold. */
int jpt_level_world_read(const char* path, char* terrain, size_t terrain_cap, char* problem, size_t problem_cap);

#ifdef __cplusplus
}
#endif

#endif
