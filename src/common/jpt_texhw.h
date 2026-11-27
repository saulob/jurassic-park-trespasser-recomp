/* Trespasser CE's hardware texture overrides (texhw) of the running custom level: which picture stands for a texture of its
 * worlds when the renderer draws it. */
#ifndef JPT_TEXHW_H
#define JPT_TEXHW_H

#include <stdint.h>

#include "jpt_picture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_TEXHW_FILES_MAX 2048

/* What the texture's own format keeps of alpha: the override never has more. A normal map keeps its own, where the surface is
 * specular. */
typedef enum { JPT_TEXHW_OPAQUE, JPT_TEXHW_CUTOUT, JPT_TEXHW_BLENDED, JPT_TEXHW_NORMAL, JPT_TEXHW_CLASSES } jpt_texhw_class_t;

/* A texture of a world that is read, by the engine's key for it. */
void jpt_texhw_name(uint32_t hash, const char* texture);
/* Whether any texture of the worlds read has an override. */
int jpt_texhw_active(void);
/* The override's id, 1 to JPT_TEXHW_FILES_MAX; 0 when it has none. */
int jpt_texhw_find(uint32_t hash);
/* The id of the normal map beside that override; 0 when it has none. */
int jpt_texhw_normal(uint32_t hash);
/* Whether any texture has one. */
int jpt_texhw_normals(void);
/* Changes whenever every id is void. */
unsigned jpt_texhw_generation(void);
/* A frame begins: a few pictures are loaded in each. */
void jpt_texhw_frame(void);
/* Its picture with every level, as the class takes it, from malloc: 1. 0: it cannot be read or does not fit the budget, and the game's
 * own texture is drawn; -1: so it is in this frame. */
int jpt_texhw_picture(int id, jpt_texhw_class_t as, jpt_picture_t* out);
/* The end of the session. */
void jpt_texhw_reset(void);

typedef void (*jpt_texhw_log_fn)(const char* line);
void jpt_texhw_set_log(jpt_texhw_log_fn log);

#ifdef __cplusplus
}
#endif

#endif
