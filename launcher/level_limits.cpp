#include "level_limits.h"

#include "jpt_groff.h"
#include "level_settings.h"

namespace jpt {

const std::vector<EngineLimit>& EngineLimits() {
    constexpr unsigned long kMegabyte = 1ul << 20;
    static const std::vector<EngineLimit> limits = {
        {"MESH_EXCEEDS_LIMIT", "largest_mesh", 2048, JPT_GROFF_MESH_FACES_MAX, false, "CE_MESH_FACES"},
        {"TEXTURE_POOL_EXCEEDS_LIMIT", "set_bytes", JPT_GROFF_POOL_BYTES, JPT_GROFF_POOL_BYTES_MAX - JPT_GROFF_POOL_ROOM, false, "CE_TEXTURE_POOL"},
        {"ALLOCATION_REQUIREMENT_EXCEEDS_CAP", "mesh_bytes", 32 * kMegabyte, (unsigned long)kMeshHeapMegabytesMax * kMegabyte, true, "CE_MESH_HEAP"},
    };
    return limits;
}

std::vector<std::string> LimitsExceeded(const WorldFacts& facts, bool of_1998) {
    std::vector<std::string> names;
    for (const EngineLimit& limit : EngineLimits())
        if (facts.count(limit.count) > (of_1998 ? limit.original : limit.port)) names.push_back(limit.name);
    return names;
}

} // namespace jpt
