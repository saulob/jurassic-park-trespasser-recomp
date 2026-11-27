/* The player's retail Stream.tpa as the game sees it: the corrected English captions and the restored audio embedded in the
 * program are applied through a virtual file (stream_pack.h), so no modified TPA is ever written or needed. */
#ifndef STREAM_AUDIO_H
#define STREAM_AUDIO_H

#include "host.h"
#include "stream_pack.h"

/* Loads the embedded restored audio and the English captions. Safe to call twice. */
void stream_audio_init(void);
/* Called with every handle the game opens and the path it asked for: each one stream_vfile_kind knows reads the virtual view.
 * `package_file`: the active custom level answered the path with a file of its own. */
void stream_audio_attach(HANDLE file, const char* path, DWORD access, DWORD flags, int package_file);
void stream_audio_detach(HANDLE file);
/* A handle the game reads `bytes` through (from malloc, taken over); INVALID_HANDLE_VALUE when there is none. */
HANDLE stream_audio_open_memory(uint8_t* bytes, size_t size);
/* The virtual view of a Stream.tpa handle of the host's own (the caption scan), or NULL; free with stream_pack_close. */
stream_pack_t* stream_audio_open_view(HANDLE file, uint64_t size);

void shim_ReadFile(void);
void shim_SetFilePointer(void);
void shim_GetFileSize(void);

#endif
