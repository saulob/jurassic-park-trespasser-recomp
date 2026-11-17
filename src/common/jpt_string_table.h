/* A release's own string table as an installed data file (text\strings.dat), made by the importer from the release's
 * string resources: "JPTS", count, then per string its ID, byte length and bytes in the release's code page. */
#ifndef JPT_STRING_TABLE_H
#define JPT_STRING_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_STRING_TABLE_MAGIC 0x5354504Au /* "JPTS" */
#define JPT_STRING_TABLE_VERSION 1u

typedef void (*jpt_string_table_fn)(void* user, uint32_t id, const char* bytes, uint32_t length);

/* Calls fn for each string; the count, or -1 when `data` is not a table or is cut short. */
int jpt_string_table_read(const uint8_t* data, size_t size, jpt_string_table_fn fn, void* user);

/* Bytes the table of `count` strings with `text_bytes` bytes of text takes. */
size_t jpt_string_table_size(uint32_t count, size_t text_bytes);

/* The table in `out` (jpt_string_table_size bytes); 0 when `cap` is too small. */
size_t jpt_string_table_write(uint8_t* out, size_t cap, const uint32_t* ids, const char* const* texts,
                              const uint32_t* lengths, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
