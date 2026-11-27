/* A custom level's texture that nothing answers: Trespasser CE's grey one, and one report for each in the log. */
#ifndef MISSING_TEXTURES_H
#define MISSING_TEXTURES_H

#include <stddef.h>
#include <stdint.h>

/* After an open found nothing. 1 for an attempt of the texture loader, which is reported here alone; on its last one *picture is the
 * bitmap to read instead (from malloc), since the engine takes a null texture for a real one. */
int missing_texture_attempt(const char* path, uint8_t** picture, size_t* size);

#endif
