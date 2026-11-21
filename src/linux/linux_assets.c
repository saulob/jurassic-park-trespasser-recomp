#include <string.h>

#include "embedded_files.h"
#include "l10n_assets.h"

extern const jpt_embedded_file_t lx_translation_files[];
extern const jpt_embedded_file_t lx_restored_audio_files[];

static const jpt_embedded_file_t* find(const jpt_embedded_file_t* table, const char* name) {
    for (; table->name; table++)
        if (!strcmp(table->name, name)) return table;
    return NULL;
}

int l10n_asset(const char* language, const char* kind, const char** data, size_t* size) {
    char name[96];
    const jpt_embedded_file_t* file;
    if (strlen(language) + strlen(kind) + 6 > sizeof name) return 0;
    strcpy(name, language);
    strcat(name, ".");
    strcat(name, kind);
    strcat(name, ".txt");
    file = find(lx_translation_files, name);
    if (!file) return 0;
    *data = (const char*)file->data;
    *size = file->size;
    return 1;
}

int restored_audio_asset(const char* name, const uint8_t** data, size_t* size) {
    const jpt_embedded_file_t* file = find(lx_restored_audio_files, name);
    if (!file) return 0;
    *data = file->data;
    *size = file->size;
    return 1;
}
