// The engine's limits a level's content can reach, in 1998 and here.
#pragma once

#include <string>
#include <vector>

#include "level_check.h"

namespace jpt {

struct EngineLimit {
    const char* name;  // as the reports spell it
    const char* count; // of WorldFacts
    unsigned long original, port;
    bool fatal; // beyond the port's, the level cannot be loaded
    const char* feature;
};

const std::vector<EngineLimit>& EngineLimits();
// The names of the limits the world is beyond.
std::vector<std::string> LimitsExceeded(const WorldFacts& facts, bool of_1998);

} // namespace jpt
