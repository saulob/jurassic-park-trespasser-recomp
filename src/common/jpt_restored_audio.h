/* The restored audio compiled into the executable: samples the retail Stream.tpa lacks, kept as the TPA's own records
 * (descriptor, caption block, payload) and described by a manifest. Shared by the host, the tests and the generator. */
#ifndef JPT_RESTORED_AUDIO_H
#define JPT_RESTORED_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_RESTORED_MANIFEST "restored_audio.manifest.txt"
#define JPT_RESTORED_HEADER "# Trespasser Recompiled restored audio 1"
#define JPT_RESTORED_MAX 16

typedef struct {
    uint32_t id;               /* the sample ID: the CRC-32 of its lower-case name */
    char name[32];
    char file[32];             /* the record's embedded file */
    size_t record_size, payload_size;
    char sha256[65];           /* of the payload */
    uint32_t format;           /* the TPA descriptor's format field */
    char codec[8];
    int channels, bits;
    uint32_t rate, duration_ms;
    char replaces[65];         /* SHA-256 of the retail payload this record replaces, "-" for a sample retail lacks */
} jpt_restored_entry_t;

/* Reads the manifest. Returns the entry count, -1 when the header is missing or a line is malformed. */
int jpt_restored_parse(const char* data, size_t n, jpt_restored_entry_t* out, int cap);

#ifdef __cplusplus
}
#endif

#endif
