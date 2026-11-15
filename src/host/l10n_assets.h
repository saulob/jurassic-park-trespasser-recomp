#ifndef L10N_ASSETS_H
#define L10N_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The embedded strings or subtitles file of a language (kind JPT_L10N_STRINGS_ASSET or JPT_L10N_SUBTITLES_ASSET). */
int l10n_asset(const char* language, const char* kind, const char** data, size_t* size);

/* A file of the embedded restored audio (jpt_restored_audio.h): its manifest or a sample record. */
int restored_audio_asset(const char* name, const uint8_t** data, size_t* size);

#ifdef __cplusplus
}
#endif

#endif
