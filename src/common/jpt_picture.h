/* Pictures of custom levels as 32 bit pixels: BMP (8, 24 and 32 bits), TGA (grey, palette, 16, 24 and 32 bits, packed or not) and
 * DDS (DXT1 to DXT5, 24 and 32 bits). Any bytes are safe to decode. */
#ifndef JPT_PICTURE_H
#define JPT_PICTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_PICTURE_SIDE_MAX 4096u
#define JPT_PICTURE_LEVELS_MAX 13

typedef struct {
    uint32_t width, height;
    int levels;    /* 1 or more, each half of the one before it */
    int has_alpha; /* a pixel of the first level is not opaque */
    uint8_t* bgra; /* the levels one after another, rows from the top */
    size_t size;
} jpt_picture_t;

/* 0 leaves out empty. */
int jpt_picture_decode(const void* bytes, size_t size, jpt_picture_t* out);
int jpt_picture_load(const char* host_path, jpt_picture_t* out);
void jpt_picture_free(jpt_picture_t* picture);

/* Makes the levels down to 1 x 1 that the file did not have; cutout: alpha is on or off, never in between. */
int jpt_picture_complete(jpt_picture_t* picture, int cutout);
/* A level's pixels and size; NULL past the last one. */
const uint8_t* jpt_picture_level(const jpt_picture_t* picture, int level, uint32_t* width, uint32_t* height);
/* Leaves out the largest levels until both sides are at most `side`. */
void jpt_picture_limit(jpt_picture_t* picture, uint32_t side);

#ifdef __cplusplus
}
#endif

#endif
