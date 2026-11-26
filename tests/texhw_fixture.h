/* A custom level with texhw pictures, in a scratch folder, and the pictures. */
#ifndef TEXHW_FIXTURE_H
#define TEXHW_FIXTURE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_user_data.h"

static inline void fx_put(uint8_t* p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* One colour (BGRA), without packing. */
static inline size_t fx_bmp(uint8_t* p, uint32_t w, uint32_t h, uint32_t bits, uint32_t bgra) {
    const uint32_t stride = (w * bits / 8 + 3) & ~3u;
    memset(p, 0, 54 + stride * h);
    p[0] = 'B', p[1] = 'M';
    fx_put(p + 10, 54, 4), fx_put(p + 14, 40, 4), fx_put(p + 18, w, 4), fx_put(p + 22, h, 4), fx_put(p + 26, 1, 2), fx_put(p + 28, bits, 2);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) fx_put(p + 54 + y * stride + x * (bits / 8), bgra, (int)bits / 8);
    return 54 + stride * h;
}

static inline size_t fx_tga(uint8_t* p, uint32_t w, uint32_t h, uint32_t bgra) {
    memset(p, 0, 18);
    p[2] = 2, p[16] = 32, p[17] = 0x28;
    fx_put(p + 12, w, 2), fx_put(p + 14, h, 2);
    for (uint32_t i = 0; i < w * h; i++) fx_put(p + 18 + 4 * i, bgra, 4);
    return 18 + 4 * (size_t)w * h;
}

static inline size_t fx_dds_head(uint8_t* p, uint32_t w, uint32_t h, uint32_t mips, const char* fourcc) {
    memset(p, 0, 128);
    memcpy(p, "DDS ", 4);
    fx_put(p + 4, 124, 4), fx_put(p + 8, 0x1007 | (mips ? 0x20000 : 0), 4), fx_put(p + 12, h, 4), fx_put(p + 16, w, 4), fx_put(p + 28, mips, 4), fx_put(p + 76, 32, 4);
    if (fourcc) {
        fx_put(p + 80, 4, 4);
        memcpy(p + 84, fourcc, 4);
    } else {
        fx_put(p + 80, 0x41, 4), fx_put(p + 88, 32, 4);
        fx_put(p + 92, 0xFF0000, 4), fx_put(p + 96, 0xFF00, 4), fx_put(p + 100, 0xFF, 4), fx_put(p + 104, 0xFF000000u, 4);
    }
    return 128;
}

static inline int fx_file(const char* dir, const char* relative, const void* bytes, size_t size) {
    char path[1024], folder[1024];
    char* slash;
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", dir, relative);
    snprintf(folder, sizeof folder, "%s", path);
    slash = strrchr(folder, '/');
    *slash = 0;
#ifdef _WIN32
    for (char* c = folder; *c; c++)
        if (*c == '/') *c = '\\';
#endif
    jpt_make_dirs(folder);
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(bytes, 1, size, f);
    return fclose(f) == 0;
}

/* The package "Tex" of user, whose files are in the map (data/<file>). */
static inline int fx_activate(const char* user, const char* const* files, int count) {
    char dir[1024], map[4096];
    jpt_custom_level_request_t request;
    char error[256];
    int n = snprintf(map, sizeof map, "trespasser-runtime 1\nscene w.scn\n");
    snprintf(dir, sizeof dir, "%s/custom_levels/Tex", user);
    for (int i = 0; i < count; i++) n += snprintf(map + n, sizeof map - (size_t)n, "file %s|%s\n", files[i], files[i] + 5);
    if (!fx_file(dir, "w.scn", "scene", 5) || !fx_file(dir, "runtime.map", map, strlen(map))) return 0;
    memset(&request, 0, sizeof request);
    strcpy(request.package, "Tex");
    strcpy(request.scene, "w.scn");
    if (!jpt_custom_level_activate(user, &request, error, sizeof error)) {
        printf("%s\n", error);
        return 0;
    }
    return 1;
}

#endif
