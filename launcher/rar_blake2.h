// BLAKE2sp (RFC 7693, eight parallel leaves), the other checksum of RAR 5.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace jpt::rar {

class Blake2s {
public:
    void Start(uint32_t node, uint32_t depth, bool last_node);
    void Update(const uint8_t* data, size_t size);
    std::array<uint8_t, 32> Finish();

private:
    void Compress(bool last);

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> block_{};
    uint64_t count_ = 0;
    size_t used_ = 0;
    bool last_node_ = false;
};

class Blake2sp {
public:
    Blake2sp();
    void Update(const uint8_t* data, size_t size);
    std::array<uint8_t, 32> Finish();

private:
    static constexpr size_t kLeaves = 8, kBlock = 64;
    std::array<Blake2s, kLeaves> leaves_;
    uint64_t count_ = 0;
};

} // namespace jpt::rar
