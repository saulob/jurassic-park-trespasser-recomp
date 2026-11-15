/* The port's own interface text in the chosen language: menus, overlays, panels and the labels redrawn on the game's
 * menu pictures. Every string is looked up by its English source (the key); "Context::Text" keys tell apart the same
 * English text in different places and show "Text" in English. Shared by the host and the tests. */
#ifndef JPT_TR_H
#define JPT_TR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_TR_ASSET "interface"
#define JPT_TR_HEADER "# Trespasser Recompiled interface text 1"
#define JPT_TR_KEEP "keep" /* en.interface.txt: the text may stay the same in a translation (a name, a symbol) */

/* The English text of a key: the part after its context. */
const char* jpt_tr_english(const char* key);

/* The translation in UTF-8, else the English text. */
const char* jpt_tr(const char* key);
/* The same in the game's display code page (jpt_l10n_display_codepage), for text drawn by GDI. */
const char* jpt_tr_cp(const char* key);
/* 1 when the language in use has a translation of the key. */
int jpt_tr_has(const char* key);

/* Loads an interface file (UTF-8) for the language in use, converted to codepage (0: kept UTF-8 only). Returns the
 * entry count, -1 when the header is missing. jpt_tr_reset forgets it (English everywhere). */
int jpt_tr_load(const char* data, size_t n, int codepage);
void jpt_tr_reset(void);

/* Reads an interface file: a header line, then `key<TAB>text[<TAB>flags]` per entry, `\\ \n \t` escaped. Returns the
 * entry count, -1 when the header is missing. */
typedef void (*jpt_tr_entry_fn)(void* ctx, const char* key, const char* text, const char* flags);
int jpt_tr_parse(const char* data, size_t n, jpt_tr_entry_fn fn, void* ctx);

/* The <TOKENS> of a text in order, joined by spaces, into out: a translation keeps the English tokens. */
void jpt_tr_tokens(const char* text, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
