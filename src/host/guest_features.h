/* The features that change the lifted game's own behaviour (gameplay, physics, camera): written against guest_abi.h, so the
 * same source installs them on Windows and on Linux. guest_features_install runs the installs both hosts share. */
#ifndef JPT_GUEST_FEATURES_H
#define JPT_GUEST_FEATURES_H

#include <stddef.h>
#include <stdint.h>

#include "guest_abi.h"
#include "jpt_config.h"

/* Reads the settings the features use (Active AI, Dual stow, Maximum object detail, Official Patch 1.1, FOV, the saved
 * cheats). Before guest_overrides_enable. */
void guest_features_configure(const jpt_config_t* cfg);
/* The overrides of the shared features, once the target is verified: physics fixes, the CE gameplay, Active AI, the
 * official Patch 1.1 and the camera's field of view. */
void guest_features_install(void);
/* Where the game could call it itself (its UI loop on Windows, a presented frame on Linux). */
void guest_features_step(void);

/* active_ai.c: Active AI (CE's MaxActiveAi), 6 animals active instead of 3; Original installs nothing. */
void active_ai_configure(int enhanced);
void active_ai_install(void);
int active_ai_installed(void);
/* Active animals per AI update, at exit. */
void active_ai_summary(void);
/* The living animals the game's AI holds, awake (engaged) or not, with their world position; read-only. */
typedef struct {
    float x, y;
    float scale; /* the instance's scale: how big the animal is */
    int engaged;
} host_animal_t;
int active_ai_animals(uint32_t scratch, host_animal_t* out, int max);

/* patch117.c: the official v1.1 build 117 level fixes, applied to retail level scenes as the game reads them. */
void patch117_configure(int on);
void patch117_install(void);
void patch117_lz_opened(const char* path, int handle);
/* A level file the game opens, as it must read it: the retail bytes with the official fixes in a new buffer (free it), or
 * NULL when the fixes are off, the file is not a retail level or the fixes did not match. `raw` is the file as stored;
 * `expand` returns the expanded bytes of a world file (a new buffer of that size, or NULL). */
typedef uint8_t* (*patch117_expand_fn)(void* context, size_t expanded_size);
int patch117_file_wanted(const char* path);
uint8_t* patch117_level_bytes(const char* path, const uint8_t* raw, size_t raw_size, patch117_expand_fn expand, void* context,
                              size_t* size);
/* patch117_player.c: the official v1.1 build 117 player fixes over the lifted 116 code. */
void patch117_player_configure(int on);
void patch117_player_install(void);
/* patch117_physics.c: the official v1.1 build 117 box-box and broad-phase fixes over the lifted 116 code. */
void patch117_physics_configure(int on);
void patch117_physics_install(void);

/* physics_fixes.c: the pelvis height clamps at the game's x87 precision, the wave model's step pacing. */
void physics_fixes_install(void);
/* Hand escapes and recoveries at exit. */
void physics_fixes_summary(void);

/* what the rigid-box physics did since the last take, by box of the 50-box pool (its broad-phase pairs,
 * sub-box pairs it was tested in, velocity kills of its contacts) and the pool's state now. */
#define PHYSICS_BOXES 50u
typedef struct {
    unsigned pairs, sub_pairs, corner_tests, kills, awake, active;
    unsigned box_pairs[PHYSICS_BOXES], box_sub_pairs[PHYSICS_BOXES], box_kills[PHYSICS_BOXES];
    unsigned box_sub_count[PHYSICS_BOXES];
    unsigned char box_awake[PHYSICS_BOXES], box_movable[PHYSICS_BOXES];
} physics_counts_t;
void physics_counts_take(physics_counts_t* out);

/* terrain_quality.c: terrain texture distance and mesh tolerance. */
void terrain_quality_install(void);
/* Mesh tolerances are in render pixels: they are compared at 480 lines. */
void terrain_quality_set_render_height(int height);
void terrain_quality_log(void);

/* skip_intro.c: Skip Intro answers the game's own "NoVideo" for the two openings CVideoWnd::Play shows. */
void skip_intro_configure(int enabled);
/* Runs the Play call `lifted` through the game's no-video path when the opening is to be skipped: 1 when it did. */
int skip_intro_try(const char* guest_name, int video_test_request, recomp_func_t lifted);
/* Installs the Play override (the Windows host installs its own, which calls skip_intro_try); 0 when it cannot. */
int skip_intro_install(void);

/* widescreen.c: the main camera's field of view (and Hor+ on a wide view). */
int widescreen_install_camera(int aspect);
/* Screen pixels per metre at unit distance for the main camera's view; 0 when there is none. */
float widescreen_pixels_per_metre(void);
/* tan of half the main camera's vertical view, as the picture shows it; 0 when there is none. */
float widescreen_tan_half_height(void);
/* The same with the camera's zoom: the projection its Direct3D vertices were made with. */
float widescreen_view_tan_half_height(void);
int widescreen_active(void);
/* Field of view in degrees across the 4:3 view (JPT_FOV_*), set from the settings; the main camera takes a change at
 * widescreen_apply_fov. */
void widescreen_set_fov(int degrees);
int widescreen_fov(void);
void widescreen_apply_fov(void);

#endif
