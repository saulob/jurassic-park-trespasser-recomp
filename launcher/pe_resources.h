// Resources of a Windows executable held in memory, found by walking its resource directory. The installer reads the
// Russian release's string table this way on systems without Win32's resource functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jpt {

// The data of resource (type, name) in the first language, as a view into image. False when the image is not a PE file
// or has no such resource.
bool FindPeResource(const std::vector<uint8_t>& image, uint32_t type, uint32_t name, const uint8_t*& data, size_t& size);

}  // namespace jpt
