/* Scene files (.scn) of the formats after the 1998 game's own (22), as its loader reads them: Trespasser CE's 23 to 26. */
#ifndef JPT_SCENE_H
#define JPT_SCENE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_SCENE_VERSION_OWN 22
#define JPT_SCENE_VERSION_MAX 26
/* From here on each object's state ends its common part with a script mark (scene_scripts.c reads past it). */
#define JPT_SCENE_VERSION_SCRIPTS 26
#define JPT_SCENE_NOTES_MAX 12

typedef struct {
    int version; /* 0: not a scene whose format can be told */
    char problem[160]; /* why it cannot be read; empty when it can */
    char notes[JPT_SCENE_NOTES_MAX][160]; /* what of the file the port has no use for */
    int note_count;
} jpt_scene_report_t;

/* The format of an expanded scene; 0 when it cannot be told. */
int jpt_scene_version(const void* bytes, size_t size);
/* 1 when the loader can take the scene, as it is or adapted. report may be NULL. */
int jpt_scene_readable(const void* bytes, size_t size, jpt_scene_report_t* report);
/* The scene in the loader's own layout, from malloc; NULL when it needs no change (no problem) or cannot be adapted. */
uint8_t* jpt_scene_adapt(const void* bytes, size_t size, size_t* out_size, jpt_scene_report_t* report);

#ifdef __cplusplus
}
#endif

#endif
