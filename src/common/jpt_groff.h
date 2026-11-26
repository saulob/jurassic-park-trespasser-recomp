/* World files (.grf) and texture directories (.pid), read for what loading a level needs of them. Bounds checked: the bytes are
 * a package's. */
#ifndef JPT_GROFF_H
#define JPT_GROFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_GROFF_TEXT_MAX 128
#define JPT_GROFF_ACTIONS_MAX 64
#define JPT_GROFF_LEVELS_MAX 32
#define JPT_GROFF_FAULTS_MAX 16
/* The engine builds a mesh of 2048 faces at most; a custom level's session gives it four times the room. */
#define JPT_GROFF_MESH_FACES_MAX 8192u
/* Of the engine's 128 MB texture pool, which a custom level's session makes larger, with room for textures outside the set. */
#define JPT_GROFF_POOL_BYTES 0x8000000u
#define JPT_GROFF_POOL_BYTES_MAX 0x10000000u
#define JPT_GROFF_POOL_ROOM 0x2000000u
#define JPT_GROFF_ACTION_OWN_LAST 35 /* of the 1998 game */
#define JPT_GROFF_ACTION_LOAD_LEVEL 16
#define JPT_GROFF_ACTION_MAP 102 /* ATX: the map picture, below MAPS */

typedef struct jpt_groff jpt_groff_t;
typedef struct jpt_texture_set jpt_texture_set_t;

typedef struct {
    int type;
    unsigned count;
} jpt_groff_action_t;

/* A premise of the engine's that the world breaks, by kind: the first place, and how many. */
typedef struct {
    const char* code; /* "TRIGGER_ACTIONS_EXCEED_LIMIT" */
    char subject[JPT_GROFF_TEXT_MAX];
    uint32_t value, limit;
    unsigned count;
} jpt_groff_fault_t;

typedef struct {
    unsigned objects;
    int optimized; /* its meshes are stored as the engine builds them */
    unsigned terrains;
    char terrain[JPT_GROFF_TEXT_MAX]; /* the terrain data file of the level, without ".wtd"; empty when none is named */
    jpt_groff_action_t actions[JPT_GROFF_ACTIONS_MAX];
    unsigned action_count;
    char levels[JPT_GROFF_LEVELS_MAX][JPT_GROFF_TEXT_MAX]; /* the scenes its LOAD_LEVEL actions name */
    unsigned level_count;
    char maps[JPT_GROFF_LEVELS_MAX][JPT_GROFF_TEXT_MAX]; /* the pictures its map actions name */
    unsigned map_count;
    unsigned animals, triggers;
    unsigned more_dinosaurs; /* that name one past the 7 kinds of the 1998 game's sounds */
    unsigned meshes; /* kinds of them, which instances share */
    uint64_t points, vertices, faces; /* of one mesh of each kind */
    uint64_t mesh_bytes; /* of the loaded meshes' heap, at most */
    unsigned largest_mesh; /* in faces */
    unsigned mesh_build; /* what the mesh builder needs for the largest of them, in faces as it counts its arrays */
    char largest_mesh_object[JPT_GROFF_TEXT_MAX];
    jpt_groff_fault_t faults[JPT_GROFF_FAULTS_MAX];
    unsigned fault_count;
} jpt_groff_report_t;

typedef struct {
    char name[JPT_GROFF_TEXT_MAX], bump[JPT_GROFF_TEXT_MAX];
    uint32_t hash;
    int in_set, picture; /* picture: its entry of the set has pixels of its own */
    int odd_size;        /* a side of it in the set is no power of two */
    int drawn;           /* by a polygon, or it cannot be told */
    char object[JPT_GROFF_TEXT_MAX]; /* the first that draws it */
    int bumped;          /* on a mesh with a bump-mapped surface, which the engine draws itself */
    unsigned curved, curved_missing; /* its polygons with a curved bump map, and those whose map is not in the set */
} jpt_groff_texture_t;

typedef struct {
    jpt_groff_texture_t* items; /* by name */
    unsigned count;
    unsigned curved, curved_missing;
} jpt_groff_textures_t;

/* bytes: the expanded file, which must outlive it; name: its file name. NULL, with the reason, when it is not a world the
 * engine can read. */
jpt_groff_t* jpt_groff_open(const char* name, const uint8_t* bytes, size_t size, char* problem, size_t cap);
void jpt_groff_close(jpt_groff_t* g);
/* 0: out of memory, here and below. */
int jpt_groff_describe(const jpt_groff_t* g, jpt_groff_report_t* report);

/* bytes: a whole .pid, which must outlive it. */
jpt_texture_set_t* jpt_texture_set_open(const uint8_t* bytes, size_t size);
void jpt_texture_set_close(jpt_texture_set_t* s);
unsigned jpt_texture_set_count(const jpt_texture_set_t* s);
/* Of the pool, its pictures. */
uint64_t jpt_texture_set_bytes(const jpt_texture_set_t* s);
/* What of the set the engine cannot hold or follow. 0: nothing. */
int jpt_texture_set_fault(const jpt_texture_set_t* s, jpt_groff_fault_t* out);
/* The set was made for another world: the one of the same name it came with, NULL when that cannot be read. */
int jpt_texture_set_borrow(jpt_texture_set_t* set, const jpt_groff_t* original);

/* The textures of the world's meshes, looked up in the set as the engine does. set may be NULL: the level has none. */
int jpt_groff_textures(const jpt_groff_t* g, const jpt_texture_set_t* set, jpt_groff_textures_t* out);
void jpt_groff_textures_free(jpt_groff_textures_t* textures);

/* A set made for another build of the world holds no curved bump map for its polygons, and the engine has no source to make one
 * of. world: the same bytes as g's, changed so that such a polygon of a texture with a picture is lit flat. Returns how many. */
unsigned jpt_groff_flatten_curved(const jpt_groff_t* g, const jpt_texture_set_t* set, uint8_t* world);
/* Of the set's bytes: the entries without a picture, kept for curved maps that this world cannot use, are no longer found, so
 * that their texture is read as any other that is not in the set. hidden: how many. */
int jpt_groff_hide_unusable(const jpt_groff_t* g, const jpt_texture_set_t* set, uint8_t* pid, unsigned* hidden);

/* The objects of the world as its region places them, and their properties, for tools that read a level's layout. */
typedef enum { JPT_GROFF_VALUE_NONE, JPT_GROFF_VALUE_BOOL, JPT_GROFF_VALUE_CHAR, JPT_GROFF_VALUE_INT, JPT_GROFF_VALUE_FLOAT, JPT_GROFF_VALUE_TEXT, JPT_GROFF_VALUE_GROUP } jpt_groff_value_type_t;

typedef struct {
    const char* name; /* "" when it has none */
    const char* type; /* its Class; "" when none */
    int has_values;
    const void* values; /* its properties, for jpt_groff_value_at */
    double position[3], rotation[3], scale;
} jpt_groff_object_t;

typedef struct {
    const char* key;
    int type; /* jpt_groff_value_type_t */
    int32_t number;
    float real;
    const char* text;
    const void* group;
} jpt_groff_value_t;

unsigned jpt_groff_object_count(const jpt_groff_t* g);
void jpt_groff_object(const jpt_groff_t* g, unsigned index, jpt_groff_object_t* out);
/* The index-th value of a group of properties, in stored order; 0 past the last. key is "" for a value without a name. */
int jpt_groff_value_at(const jpt_groff_t* g, const void* group, unsigned index, jpt_groff_value_t* out);
/* The first value of that name; 0 when the group has none. */
int jpt_groff_value_named(const jpt_groff_t* g, const void* group, const char* key, jpt_groff_value_t* out);
/* The points of the object's own geometry, in its frame (x, y, z each), from malloc: the stored points, or the wrap of a built mesh. */
int jpt_groff_object_points(const jpt_groff_t* g, unsigned index, float** points, unsigned* count);
/* A built mesh's points and triangles (three point indices each), both from malloc; 0 for any other geometry. */
int jpt_groff_object_mesh(const jpt_groff_t* g, unsigned index, float** points, unsigned* count, uint32_t** triangles, unsigned* triangle_count);

#ifdef __cplusplus
}
#endif

#endif
