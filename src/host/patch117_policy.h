/* Official Trespasser v1.1 build 117 level fixes over the retail scene files, without the guest. A scene file is a
 * GROFF image; the fixes are applied to its bytes as the game reads them, only to a file whose name, size and SHA-256
 * are the retail ones, and only where every value still holds its retail contents. */
#ifndef PATCH117_POLICY_H
#define PATCH117_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PATCH117_GROFF_MAGIC 0x0ACEBABEu
/* The scene section CTerrain::pcLoad (0x00475010) reads: CRC-32 of "Terrain" in hex. */
#define PATCH117_TERRAIN_SECTION "7CB6A2D"
#define PATCH117_TERRAIN_VERSION 1u

typedef struct {
    float retail, official;
} patch117_float_t;

/* A movable object's saved transform at the start of its scene section (pos, quaternion, scale). */
typedef struct {
    const char* section;
    uint8_t retail[32], official[32];
} patch117_presence_t;

/* A static object's .region record transform (pos, Euler angles, scale) at its offset in the expanded world file. */
typedef struct {
    uint32_t offset;
    uint8_t retail[28], official[28];
} patch117_place_t;

typedef struct {
    const char* file;    /* the scene file name, compared without case */
    const char* id;      /* the level's short name for the log ("BE") */
    uint32_t size;
    const char* sha256;
    patch117_float_t pixel_tol;     /* TrrPixelTol, the near terrain mesh error in pixels */
    patch117_float_t pixel_tol_far; /* TrrPixelTolFar */
    const patch117_presence_t* presences;
    int presence_count;
} patch117_level_t;

/* A level's world file (.grf), SZDD-compressed on the disc; the game reads it expanded through LZRead. */
typedef struct {
    const char* file;
    const char* id;
    uint32_t size;
    const char* sha256;
    uint32_t expanded_size;
    const patch117_place_t* places;
    int count;
} patch117_world_t;

#define PATCH117_PLACES_IJ 172
#define PATCH117_PLACES_IT 5
#define PATCH117_PRESENCES_AS 1
extern const patch117_place_t patch117_places_ij[PATCH117_PLACES_IJ];
extern const patch117_place_t patch117_places_it[PATCH117_PLACES_IT];
extern const patch117_presence_t patch117_presences_as[PATCH117_PRESENCES_AS];

#define PATCH117_WORLD_COUNT 2
extern const patch117_world_t patch117_worlds[PATCH117_WORLD_COUNT];

#define PATCH117_LEVEL_COUNT 8
extern const patch117_level_t patch117_levels[PATCH117_LEVEL_COUNT];

/* The level a path names by its file name, whatever its folder; NULL for any other file. */
const patch117_level_t* patch117_level_named(const char* path);
/* The level only when the bytes are that retail file (size and SHA-256); NULL otherwise. */
const patch117_level_t* patch117_identify(const char* path, const void* data, size_t size);

/* The section's bytes inside a GROFF image, located through its section table and symbol table; NULL when the image is
 * not a GROFF, is truncated or has no such section. */
uint8_t* patch117_groff_section(uint8_t* image, size_t size, const char* name, uint32_t* section_size);

/* Writes the level's official terrain values. Returns the number of values changed, or -1, with the image untouched,
 * when the section is missing or any value is not the retail one (a modified or already fixed file). */
int patch117_fix_terrain(const patch117_level_t* level, uint8_t* image, size_t size);

/* Writes the official transforms of the level's movable objects; same results as patch117_fix_terrain. */
int patch117_fix_presences(const patch117_level_t* level, uint8_t* image, size_t size);

/* Every official fix for the level, all or none; same results as patch117_fix_terrain. */
int patch117_fix_scene(const patch117_level_t* level, uint8_t* image, size_t size);

/* The world file a path names by its file name; NULL for any other file. */
const patch117_world_t* patch117_world_named(const char* path);
/* The world only when the bytes are that retail file as stored on the disc (size and SHA-256); NULL otherwise. */
const patch117_world_t* patch117_world_identify(const char* path, const void* data, size_t size);
/* Writes the official placements into the expanded world image; same results as patch117_fix_terrain. */
int patch117_fix_world(const patch117_world_t* world, uint8_t* image, size_t size);

/* CPartition::bAddChild(child, test_priority) (0x00475F60) asks each child to take the new partition with the test
 * flag 0; build 117 passes its own flag down, so a moved object is never placed under a partition of higher priority
 * that the render traversal skips. The flag a call runs with: its own at the top, the caller's with the fix. */
int patch117_add_child_flag(int fixes_on, int nested, int caller_flag, int own_flag);

#ifdef __cplusplus
}
#endif

#endif
