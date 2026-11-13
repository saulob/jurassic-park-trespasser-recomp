/* HD pictures of saved games, kept beside the saves as <save folder>\previews\<save file>.jpg. The save itself and the
 * 100x75 thumbnail inside it are never touched; a save without a picture shows that thumbnail. */
#ifndef SAVE_PREVIEW_H
#define SAVE_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAVE_PREVIEW_PIXELS (960 * 540)

typedef struct {
    int width, height;
} sp_size_t;

/* The picture's size for a screen: the screen's shape, about SAVE_PREVIEW_PIXELS, never larger than the screen. */
sp_size_t save_preview_size(int screen_w, int screen_h);

/* "<dir>\previews\<name>.jpg" for "<dir>\<name>"; 0 when it does not fit. */
int save_preview_path(const char* save_path, char* out, size_t cap);

/* A save of the game: <user_dir>\<base>.<three digits>. */
int save_preview_is_save(const char* path, const char* user_dir, const char* base);

/* From the game's RGB565 screen picture; 0 when it could not be written (the save goes on regardless). */
int save_preview_write(const char* save_path, const uint16_t* rgb565, int w, int h, long pitch);
int save_preview_delete(const char* save_path);

typedef struct {
    uint32_t* pixels; /* opaque BGRA rows */
    int width, height;
} sp_image_t;

/* 0 when the save has no picture or it cannot be read. */
int save_preview_load(const char* save_path, sp_image_t* out);
void save_preview_free(sp_image_t* image);
/* Any picture file WIC reads, opaque BGRA, made smaller (Fant) to fit fit_w x fit_h when larger; 0 when unreadable. */
int sp_image_load(const char* path, int fit_w, int fit_h, sp_image_t* out);

/* The picture of a screen at save_preview_size, area averaged (opaque BGRA). */
int save_preview_shrink(const uint16_t* rgb565, int w, int h, long pitch, sp_image_t* out);

#ifdef __cplusplus
}
#endif

#endif
