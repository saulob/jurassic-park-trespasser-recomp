#include "assao_host.h"

#include "embedded_file.h"

namespace jpt {
extern const EmbeddedFile kAssaoShaderFiles[];
}

extern "C" const char* assao_embedded_shader(size_t* size) {
    *size = jpt::kAssaoShaderFiles[0].size;
    return reinterpret_cast<const char*>(jpt::kAssaoShaderFiles[0].data);
}
