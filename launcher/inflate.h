// Raw DEFLATE (RFC 1951) decoder for the files of an InstallShield cabinet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jpt {

// Appends the decoded stream to out. False when the data is not valid DEFLATE or would decode past max_out bytes.
bool InflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t max_out);

}  // namespace jpt
