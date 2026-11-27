/* What CE's script API reaches of the game's world. Objects are named by their unique handle (the engine's hash of their name)
 * and looked up again at every call, so a script never holds an engine pointer: an object gone from the world answers 0.
 * box: -1 for the object itself, or an EBoundaryBoxes value for that boundary box of an animate.
 * The host implements it over the guest (src/host/script_engine.c); tests over a world of their own. */
#ifndef JPT_SCRIPT_ENGINE_H
#define JPT_SCRIPT_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, z;
} jpt_vec3;

typedef struct {
    float w, x, y, z;
} jpt_rot3;

typedef struct jpt_script_engine {
    void* user;
    /* CE's native class name of the object (CInstance, CAnimal, CPlayer, ...), or NULL when the world has no such object. */
    const char* (*native_type)(void* user, uint32_t handle);
    /* The unique handle of a named object of the world, 0 when there is none. */
    uint32_t (*find)(void* user, const char* name);
    int (*placement)(void* user, uint32_t handle, int box, jpt_vec3* position, jpt_rot3* rotation);
    int (*head_position)(void* user, uint32_t handle, jpt_vec3* out);
    int (*teleport)(void* user, uint32_t handle, const jpt_vec3* position, const jpt_rot3* rotation);
    /* CE's fixed velocity of actions 110 and 111: 1 when it is set; 0 when the object cannot move, or already moves at a fixed
     * velocity and fail_if_moving is set. */
    int (*fixed_velocity)(void* user, uint32_t handle, const jpt_vec3* velocity, int milliseconds, int fail_if_moving);
    float (*sim_time)(void* user);
    /* The player's unique handle, 0 without one; the object in the player's hand, 0 for none. */
    uint32_t (*player)(void* user);
    uint32_t (*held_object)(void* user, uint32_t player);
    /* An animate's hit points and whether the engine counts it dead. */
    int (*health)(void* user, uint32_t handle, float* hit_points, int* dead);
    /* A gun's rounds and the most it holds; set keeps 0 <= ammo <= max_ammo. */
    int (*gun_ammo)(void* user, uint32_t gun, int* ammo, int* max_ammo);
    int (*set_gun_ammo)(void* user, uint32_t gun, int ammo, int max_ammo);
    /* The game's text overlay: a text shown for seconds after the entry previous (0: now), its handle or 0; the last entry of a
     * type, 0 for none. previous is one the overlay holds, or 0. */
    uint32_t (*show_text)(void* user, const char* text, float seconds, uint32_t format, uint32_t rgb, uint32_t previous, uint32_t type);
    uint32_t (*text_end)(void* user, uint32_t type);
    /* A picture of the package by a path relative to its game paths, as straight-alpha RGBA (from malloc); NULL when there is none. */
    uint8_t* (*load_texture)(void* user, const char* name, int* width, int* height);
    /* The game's main rendering surface, in its pixels. */
    int (*screen_size)(void* user, int* width, int* height);
    /* A player's head: the rotation it looks with relative to its body (body rotation then this one is the view). */
    int (*head_rotation)(void* user, uint32_t player, jpt_rot3* out);
    /* A player speaks the sample of that ID through the game's voice: 1 when the call reached the game, which itself ignores a
     * sample it does not have or a player who cannot speak. */
    int (*say)(void* user, uint32_t player, uint32_t sample);
} jpt_script_engine;

/* The text overlay's format bits CE's TEXT_FORMAT_* name; the others are the overlay's own state. */
#define JPT_TEXT_FORMATS 0x21Fu
#define JPT_TEXT_MAX 1024u

#define JPT_SCRIPT_BOXES 8

#ifdef __cplusplus
}
#endif

#endif
