/* The virtual registry's value store: the game's one key as a text file, in the format the Windows host writes
 * (recomp-registry.txt), with fixed-width types so a file written on one platform reads on the other.
 *
 *   Name=dword:0000000A          a REG_DWORD, eight hex digits
 *   Name=sz:text                 a REG_SZ without line breaks
 *   Name=hex(3):00ff...          any other value: its Win32 type number and the bytes in hex
 */
#ifndef JPT_REG_STORE_H
#define JPT_REG_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { JPT_REG_SZ = 1, JPT_REG_BINARY = 3, JPT_REG_DWORD = 4 };

typedef struct jpt_reg_store jpt_reg_store_t;

jpt_reg_store_t* jpt_reg_new(void);
void jpt_reg_free(jpt_reg_store_t* r);

/* Names are case-insensitive, at most 63 characters; a store holds up to 160 values. Returns 0 when it cannot hold one more. */
int jpt_reg_set(jpt_reg_store_t* r, const char* name, uint32_t type, const void* data, uint32_t size);
int jpt_reg_set_dword(jpt_reg_store_t* r, const char* name, uint32_t value);
int jpt_reg_set_string(jpt_reg_store_t* r, const char* name, const char* text);
/* The stored bytes of a value, or NULL when it is not set. */
const void* jpt_reg_get(const jpt_reg_store_t* r, const char* name, uint32_t* type, uint32_t* size);
int jpt_reg_get_dword(const jpt_reg_store_t* r, const char* name, uint32_t* value);
int jpt_reg_delete(jpt_reg_store_t* r, const char* name);
int jpt_reg_count(const jpt_reg_store_t* r);

/* Loads a file (a missing file is an empty store; returns the number of values read, -1 on a read error) and saves
 * through a temporary file renamed over the target. Malformed lines are skipped and counted in *ignored when given. */
int jpt_reg_load(jpt_reg_store_t* r, const char* path, int* ignored);
int jpt_reg_save(const jpt_reg_store_t* r, const char* path);

#ifdef __cplusplus
}
#endif

#endif
