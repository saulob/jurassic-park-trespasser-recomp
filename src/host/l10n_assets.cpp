// The translated text and the restored audio compiled into the executable (CMakeLists.txt, launcher/embed_files.cmake).
#include <cstring>
#include <string>

#include "embedded_file.h"
#include "l10n_assets.h"

namespace jpt {
extern const EmbeddedFile kTranslationFiles[];
extern const EmbeddedFile kRestoredAudioFiles[];
}

extern "C" int l10n_asset(const char* language, const char* kind, const char** data, size_t* size) {
    const std::string name = std::string(language) + "." + kind + ".txt";
    for (const jpt::EmbeddedFile* file = jpt::kTranslationFiles; file->name; ++file) {
        if (name == file->name) {
            *data = reinterpret_cast<const char*>(file->data);
            *size = file->size;
            return 1;
        }
    }
    return 0;
}

extern "C" int restored_audio_asset(const char* name, const uint8_t** data, size_t* size) {
    for (const jpt::EmbeddedFile* file = jpt::kRestoredAudioFiles; file->name; ++file) {
        if (!std::strcmp(name, file->name)) {
            *data = file->data;
            *size = file->size;
            return 1;
        }
    }
    return 0;
}
