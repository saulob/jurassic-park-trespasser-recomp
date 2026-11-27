// A level's loading picture (TGA) as the game's own loading screen takes it: 640x480, 16 bits, uncompressed.
#pragma once

#include <cstdint>
#include <string>

namespace jpt {

inline constexpr uint64_t kPictureLimit = 32ull << 20;

// An uncompressed or run-length TGA of 16, 24 or 32 bits, complete.
bool IsLoadingPicture(const std::string& bytes);
// Empty when the picture cannot be read. One of another shape keeps it, centred on black.
std::string MakeLoadingPicture(const std::string& bytes);

} // namespace jpt
