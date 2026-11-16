/* The port's text translations: the language choice, the credits, the formats the translated text is kept in (GameStrings,
 * TPA captions and the port's own strings and subtitles files, embedded in the program) and the readers of them.
 * Shared by the host, the launcher and the tests. */
#ifndef JPT_L10N_H
#define JPT_L10N_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_L10N_ENGLISH "en"
#define JPT_L10N_CODE_MAX 8
#define JPT_L10N_STRINGS_ASSET "strings"
#define JPT_L10N_SUBTITLES_ASSET "subtitles"

enum { JPT_L10N_GAME_TEXT = 1, JPT_L10N_SUBTITLES = 2 };

/* The one language choice: its game text, menus, interface and subtitles. The voices are always English. */
typedef struct {
    const char* code;         /* the `language` setting and the asset names */
    const char* name;         /* English */
    int codepage;             /* the ANSI code page the game draws it in: 1252 or 1251 */
} jpt_l10n_language_t;

#define JPT_L10N_LANGUAGE_COUNT 7
extern const jpt_l10n_language_t jpt_l10n_languages[JPT_L10N_LANGUAGE_COUNT];
/* NULL for a code that is not a language of the table. */
const jpt_l10n_language_t* jpt_l10n_language(const char* code);
/* The language at a position of the launcher's list: English first, then the others by English name. */
const jpt_l10n_language_t* jpt_l10n_language_listed(int position);
/* The choice that replaces two earlier settings (game_text_language, subtitle_language): the first that names a
 * translation, else "en". */
const char* jpt_l10n_infer_language(const char* game_text, const char* subtitles);

typedef struct {
    const char* language;     /* code */
    const char* content;
    const char* author;
    const char* converter;    /* of the GameStrings, or NULL */
    const char* source;       /* "official" or "community" */
} jpt_l10n_credit_t;

#define JPT_L10N_CREDIT_COUNT 7
extern const jpt_l10n_credit_t jpt_l10n_credits[JPT_L10N_CREDIT_COUNT];
#define JPT_L10N_PRESERVATION "TresCom community"
/* Everything the releases above do not cover was written for the port, never attributed to them. */
#define JPT_L10N_PORT_COMPLETION "Trespasser Recompiled"
#define JPT_L10N_PORT_CONTENT "subtitles, menus and interface in every language, Italian game text, missing German and Russian strings, translated restored dialogue"

/* The embedded files: <code>.<kind>.txt. The port's own completions are separate files, loaded after the release's:
 * game text only for what the release lacks, captions also in place of the release's where they carry the corrected
 * timing (en.subtitles.txt holds the canonical timings). */
#define JPT_L10N_STRINGS_PORT_ASSET "strings.port"
#define JPT_L10N_SUBTITLES_PORT_ASSET "subtitles.port"
/* String IDs that are never translated: 10 is the save files' name, which must not change with the language. */
int jpt_l10n_id_fixed(unsigned id);

/* ---- code pages (1252, 1251) and UTF-8 */

int jpt_utf8_valid(const char* s, size_t n);
/* Converts n bytes of the code page to UTF-8, always NUL-terminated. Returns the length written, or (size_t)-1 when
 * out is too small. */
size_t jpt_cp_to_utf8(int codepage, const char* in, size_t n, char* out, size_t cap);
/* UTF-8 to the code page. Characters it lacks become their unaccented letter when there is one, else '?'. Returns
 * the length written (NUL-terminated), or (size_t)-1 when out is too small or the input is not UTF-8. */
size_t jpt_utf8_to_cp(int codepage, const char* in, char* out, size_t cap);
/* LOGFONT lfCharSet for the code page: ANSI_CHARSET or RUSSIAN_CHARSET. */
int jpt_cp_charset(int codepage);

/* The code page the game's text is drawn in this run: 0 while every choice is English (nothing is changed). */
void jpt_l10n_set_display_codepage(int codepage);
int jpt_l10n_display_codepage(void);
/* The charset fonts the port creates for the game's text use: `fallback` while the display code page is 0. */
int jpt_l10n_display_charset(int fallback);
/* The running translation of a string ID in the display code page (the host registers it), else `english`. */
typedef const char* (*jpt_l10n_text_lookup_fn)(unsigned id);
void jpt_l10n_set_lookup(jpt_l10n_text_lookup_fn fn);
const char* jpt_l10n_text_or(unsigned id, const char* english);

/* ---- GameStrings (game_strings.txt: `ID="text"` lines in an ANSI code page) */

#define JPT_L10N_ID_LIMIT 65536

typedef struct {
    int entries;              /* accepted */
    int duplicates;           /* later lines of an ID already taken: the first one is kept */
    int invalid;              /* lines that are neither entries, comments nor blank */
    int first_invalid_line;
    int normalized;           /* sentences given back the space after their full stop */
} jpt_gs_stats_t;

typedef void (*jpt_text_fn)(void* ctx, unsigned id, const char* utf8);

/* Parses a GameStrings file: blank lines and `;` comments are skipped, `\r \n \t \" \\` and `""` are escapes, the
 * text becomes UTF-8. With normalize, "word.Next" gets its missing space (lowercase letter, then . ! or ?, then an
 * uppercase letter), except in key names (16000..16999). Returns 0 when no entry was found. */
int jpt_gs_parse(const char* data, size_t n, int codepage, int normalize, jpt_text_fn fn, void* ctx,
                 jpt_gs_stats_t* stats);

/* The printf conversions of a translation match the English text's, in order: a mismatch must not reach the game's
 * formatting. */
int jpt_format_compatible(const char* english, const char* translated);

/* ---- installed files (UTF-8, one record per line, `\\ \n \r \t` escaped) */

#define JPT_L10N_STRINGS_HEADER "# Trespasser Recompiled game text 1"
#define JPT_L10N_SUBTITLES_HEADER "# Trespasser Recompiled subtitles 1"

/* Escapes text for one line. Returns the length, or (size_t)-1 when out is too small. */
size_t jpt_l10n_escape(const char* text, char* out, size_t cap);
/* Reads strings.txt. Returns the entry count, -1 when the header is missing. */
int jpt_l10n_read_strings(const char* data, size_t n, jpt_text_fn fn, void* ctx);

#define JPT_CAPTION_MAX_LINES 32

typedef void (*jpt_sample_fn)(void* ctx, const char* pack, uint32_t sample, int line_count, const uint32_t* units,
                              const char* const* lines);
/* Reads subtitles.txt (`S<TAB>pack<TAB>sample` then a `C<TAB>units<TAB>text` line per caption). Returns the sample
 * count, -1 when the header is missing. */
int jpt_l10n_read_subtitles(const char* data, size_t n, jpt_sample_fn fn, void* ctx);

/* ---- TPA captions (the caption block between a sample's descriptor and its audio) */

#define JPT_TPA_MAGIC 0x150u
#define JPT_TPA_HEADER 0x18u
#define JPT_TPA_ENTRY 0x38u
#define JPT_TPA_DESCRIPTOR 0x24u
#define JPT_TPA_MAX_SAMPLES 8192u
#define JPT_CAPTION_UNIT_MS 200u /* a caption's time is a count of 0.2 s (0x00649C9C) */

/* A caption block: u32 length (itself included), u32 count, then count times { u32 units, NUL-terminated text }. */
typedef void (*jpt_caption_line_fn)(void* ctx, uint32_t units, const char* text, size_t n);
/* Returns the line count, -1 when the block is malformed. */
int jpt_caption_lines(const uint8_t* block, size_t n, jpt_caption_line_fn fn, void* ctx);
/* Identifies a block by its captions (count, times and text): FNV-1a 64 of everything after the length. */
uint64_t jpt_caption_fingerprint(const uint8_t* block, size_t n);
/* Builds a block from lines already in the display code page. Returns its length, 0 when cap is too small. */
size_t jpt_caption_build(int count, const uint32_t* units, const char* const* lines, uint8_t* out, size_t cap);

typedef int (*jpt_read_fn)(void* ctx, uint64_t offset, void* buf, size_t n); /* 1 when all n bytes were read */
typedef void (*jpt_tpa_caption_fn)(void* ctx, uint32_t sample, const uint8_t* block, size_t n);

typedef struct {
    int samples;
    int captioned;
    int duplicates; /* samples whose ID came earlier: the first is kept */
    int malformed;
} jpt_tpa_stats_t;

/* Walks a TPA's directory and hands every caption block to fn. Returns 0 when the file is not a TPA. */
int jpt_tpa_captions(jpt_read_fn read, void* rctx, uint64_t size, jpt_tpa_caption_fn fn, void* ctx,
                     jpt_tpa_stats_t* stats);

/* ---- menu labels: the English DDF "Initial Text" values the GameStrings give by ID (10000 + the DDF's own number) */

int jpt_ddf_text_id(const char* english);

/* The Data Drive file a guest menu DDF path names: "menu\x.ddf", "data\menu\x.ddf", <user dir>\menu\x.ddf (the
 * Installed Directory probe) or <game dir>\data\menu\x.ddf. Source is always <game dir>\data\menu\x.ddf; 0 for any other
 * path (other folders, "..", subfolders, not a .ddf). */
int jpt_menu_ddf_source(const char* game_dir, const char* user_dir, const char* path, char* source, size_t cap,
                        char* name, size_t name_cap);

typedef const char* (*jpt_lookup_fn)(void* ctx, unsigned id);
/* Rewrites a DDF: each listed `"English" ; Initial Text` takes the looked-up text when there is one. *out is
 * malloc'd. Returns the number of labels replaced, -1 on allocation failure. */
int jpt_ddf_translate(const char* in, size_t n, jpt_lookup_fn lookup, void* ctx, char** out, size_t* out_n);

/* The beach tutorial's lines: the level's TEXT actions hold their English text, not a string ID. A line gets its key in
 * the interface table (jpt_tr.h), where its translations write the controls as <TOKENS>; blanks do not matter in the
 * match. NULL for any other text. */
#define JPT_L10N_TUTORIAL_COUNT 29
const char* jpt_tutorial_key(const char* english);
const char* jpt_tutorial_english(int index);
const char* jpt_tutorial_key_at(int index);

#ifdef __cplusplus
}
#endif

#endif
