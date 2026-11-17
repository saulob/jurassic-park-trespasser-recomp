#include "jpt_game_version.h"

#include <stdio.h>
#include <string.h>

#include "jpt_hash.h"

const jpt_game_version_t jpt_game_versions[JPT_GAME_VERSION_COUNT] = {
    { "en-116", "English \xC2\xB7 Build 116", "English", "en", "DreamWorks Interactive retail (UK and US discs)", 116,
      "lift116", NULL, 1252, NULL, NULL, NULL, NULL },
    { "ru-116", "Russian \xC2\xB7 Build 116", "Russian", "ru", "Fargus localization of the retail release", 116,
      "lift116", "text\\strings.dat", 1251, "text\\tresp.ttf", "Tresp", "versions\\ru-116\\saves", NULL },
    { "es-117", "Spanish \xC2\xB7 Build 117", "Spanish", "es", "official Spanish release", 117,
      "lift_es117", NULL, 1252, NULL, NULL,
      "versions\\es-117\\saves", NULL },
    { "ptbr-117", "Brazilian Portuguese \xC2\xB7 Build 117", "Brazilian Portuguese", "pt-BR",
      "official Brazilian Portuguese release", 117,
      "lift_ptbr117", NULL, 1252, NULL, NULL,
      "versions\\ptbr-117\\saves", NULL },
};

const jpt_game_version_t* jpt_game_version_by_id(const char* id) {
    if (!id) return NULL;
    for (int i = 0; i < JPT_GAME_VERSION_COUNT; i++)
        if (!_stricmp(jpt_game_versions[i].id, id)) return &jpt_game_versions[i];
    return NULL;
}

int jpt_game_version_playable(const jpt_game_version_t* v) { return v && !v->unplayable; }

int jpt_file_sha256(const char* path, char out[65]) {
    unsigned char block[1 << 16];
    size_t n;
    jpt_sha256_t ctx;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    jpt_sha256_init(&ctx);
    while ((n = fread(block, 1, sizeof block, f)) > 0) jpt_sha256_update(&ctx, block, n);
    n = (size_t)ferror(f);
    fclose(f);
    if (n) return 0;
    jpt_sha256_hex(&ctx, out);
    return 1;
}

static int fits(int written, size_t cap) { return written > 0 && (size_t)written < cap; }

int jpt_game_version_folder(const char* game_root, const jpt_game_version_t* v, char* out, size_t cap) {
    return v && fits(snprintf(out, cap, "%s\\" JPT_VERSIONS_DIR "\\%s", game_root, v->id), cap);
}

static int has_guest_image(const char* dir) {
    char path[520];
    FILE* f;
    if (!fits(snprintf(path, sizeof path, "%s\\" JPT_GUEST_IMAGE, dir), sizeof path) || !(f = fopen(path, "rb"))) return 0;
    fclose(f);
    return 1;
}

int jpt_game_version_installed(const char* game_root, const jpt_game_version_t* v, char* out, size_t cap) {
    return v && jpt_game_version_folder(game_root, v, out, cap) && has_guest_image(out);
}

int jpt_game_version_resolve(const char* game_root, const char* wanted, int playable_only, char* dir, size_t cap,
                             const jpt_game_version_t** out) {
    const jpt_game_version_t* first = jpt_game_version_by_id(wanted);
    if (first && (!playable_only || jpt_game_version_playable(first)) &&
        jpt_game_version_installed(game_root, first, dir, cap)) {
        if (out) *out = first;
        return 1;
    }
    for (int i = 0; i < JPT_GAME_VERSION_COUNT; i++) {
        const jpt_game_version_t* v = &jpt_game_versions[i];
        if (v == first || (playable_only && !jpt_game_version_playable(v))) continue;
        if (jpt_game_version_installed(game_root, v, dir, cap)) {
            if (out) *out = v;
            return 1;
        }
    }
    return 0;
}

int jpt_game_version_saves_dir(const char* user_root, const char* default_saves, const jpt_game_version_t* v,
                               char* out, size_t cap) {
    if (!v || !v->saves) return fits(snprintf(out, cap, "%s", default_saves), cap);
    return fits(snprintf(out, cap, "%s\\%s", user_root, v->saves), cap);
}
