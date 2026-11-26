/* Saved games: whether one can be read, what it is of, and writing one whole or not at all. The game's reader trusts every size
 * and count of the file, and its writer truncates the previous save before the new one is built. */
#ifndef JPT_SAVE_H
#define JPT_SAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_SAVE_WORLDS 5
#define JPT_SAVE_TEXT_MAX 64
#define JPT_SAVE_PACKAGE_MAX 64
/* The game keeps a level's name in 20 bytes. */
#define JPT_SAVE_LEVEL_NAME_MAX 19
/* A section passes through a buffer of the game's, which a custom level's session makes larger. */
#define JPT_SAVE_SECTION_BYTES (1u << 20)
#define JPT_SAVE_SECTION_BYTES_MAX (4u << 20)

typedef struct {
    char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX]; /* file names */
    char scene[JPT_SAVE_TEXT_MAX];
    int version;
    int stamped; /* of a custom level: */
    char package[JPT_SAVE_PACKAGE_MAX];
    uint32_t content; /* of its worlds' files */
    size_t image_size; /* without the stamp */
    char problem[128]; /* empty when the game's reader can take it */
} jpt_save_info_t;

/* SAVEGAME.<digits>, SAVEGAME.QCK; not the file the game keeps while its display is rebuilt (temp.scx). */
int jpt_save_is_name(const char* path);
int jpt_save_is_temporary(const char* path);

/* The size of that buffer, JPT_SAVE_SECTION_BYTES_MAX until it is told. */
void jpt_save_set_buffer(uint32_t bytes);
/* A container as the game's reader walks it: every section in place and small enough for its buffer, every symbol ended. */
int jpt_save_container(const void* bytes, size_t size, char* problem, size_t cap);
/* The same, and what its version 22 sections count fits them and the engine's arrays. */
int jpt_save_state(const void* bytes, size_t size, char* problem, size_t cap);
int jpt_save_inspect(const void* bytes, size_t size, jpt_save_info_t* out);
int jpt_save_inspect_file(const char* path, jpt_save_info_t* out);

/* Whether the save belongs to the running session: the custom level's package, or none. why: for the log. */
int jpt_save_belongs(const jpt_save_info_t* save, const char* package, uint32_t content, char* why, size_t cap);

/* While a save is written it is another file: temp, then finish puts it in the save's place when it is whole. package may be
 * empty. 0 keeps the previous save, and problem says why. */
typedef uint32_t (*jpt_save_content_fn)(const jpt_save_info_t* save);
/* What a custom level's session keeps behind the mark of its saves; NULL for nothing. */
typedef size_t (*jpt_save_state_fn)(uint8_t* out, size_t cap);
void jpt_save_set_state(jpt_save_state_fn write);
/* The bytes kept in a save; 0 when it has none or they do not fit. */
size_t jpt_save_read_state(const char* path, uint8_t* out, size_t cap);
int jpt_save_begin(const char* path, char* temp, size_t cap);
int jpt_save_finish(const char* path, const char* temp, const char* package, jpt_save_content_fn content, char* problem, size_t cap);
/* The worlds an expanded scene or save is a state of. */
int jpt_save_worlds(const void* bytes, size_t size, char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX]);

#ifdef __cplusplus
}
#endif

#endif
