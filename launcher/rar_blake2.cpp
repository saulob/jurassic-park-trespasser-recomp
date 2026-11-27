#include "rar_blake2.h"

#include <algorithm>
#include <cstring>

namespace jpt::rar {
namespace {

constexpr uint32_t kInitial[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
constexpr uint8_t kOrder[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

uint32_t Turn(uint32_t v, unsigned bits) { return v >> bits | v << (32 - bits); }

} // namespace

void Blake2s::Start(uint32_t node, uint32_t depth, bool last_node) {
    std::copy(std::begin(kInitial), std::end(kInitial), state_.begin());
    state_[0] ^= 32 | 8u << 16 | 2u << 24;
    state_[2] ^= node;
    state_[3] ^= depth << 16 | 32u << 24;
    count_ = 0;
    used_ = 0;
    last_node_ = last_node;
}

void Blake2s::Compress(bool last) {
    uint32_t m[16], v[16];
    for (size_t i = 0; i < 16; ++i) m[i] = uint32_t(block_[4 * i]) | uint32_t(block_[4 * i + 1]) << 8 | uint32_t(block_[4 * i + 2]) << 16 | uint32_t(block_[4 * i + 3]) << 24;
    std::copy(state_.begin(), state_.end(), v);
    std::copy(std::begin(kInitial), std::end(kInitial), v + 8);
    v[12] ^= uint32_t(count_);
    v[13] ^= uint32_t(count_ >> 32);
    if (last) v[14] = ~v[14];
    if (last && last_node_) v[15] = ~v[15];
    for (const auto& order : kOrder) {
        const auto mix = [&](size_t a, size_t b, size_t c, size_t d, size_t i) {
            v[a] += v[b] + m[order[2 * i]];
            v[d] = Turn(v[d] ^ v[a], 16);
            v[c] += v[d];
            v[b] = Turn(v[b] ^ v[c], 12);
            v[a] += v[b] + m[order[2 * i + 1]];
            v[d] = Turn(v[d] ^ v[a], 8);
            v[c] += v[d];
            v[b] = Turn(v[b] ^ v[c], 7);
        };
        for (size_t i = 0; i < 4; ++i) mix(i, 4 + i, 8 + i, 12 + i, i);
        for (size_t i = 0; i < 4; ++i) mix(i, 4 + (i + 1) % 4, 8 + (i + 2) % 4, 12 + (i + 3) % 4, 4 + i);
    }
    for (size_t i = 0; i < 8; ++i) state_[i] ^= v[i] ^ v[i + 8];
}

void Blake2s::Update(const uint8_t* data, size_t size) {
    while (size) {
        if (used_ == block_.size()) {
            count_ += used_;
            Compress(false);
            used_ = 0;
        }
        const size_t take = std::min(size, block_.size() - used_);
        std::memcpy(&block_[used_], data, take);
        used_ += take;
        data += take;
        size -= take;
    }
}

std::array<uint8_t, 32> Blake2s::Finish() {
    count_ += used_;
    std::fill(block_.begin() + ptrdiff_t(used_), block_.end(), uint8_t(0));
    Compress(true);
    std::array<uint8_t, 32> digest{};
    for (size_t i = 0; i < 32; ++i) digest[i] = uint8_t(state_[i / 4] >> (8 * (i % 4)));
    return digest;
}

Blake2sp::Blake2sp() {
    for (size_t i = 0; i < kLeaves; ++i) leaves_[i].Start(uint32_t(i), 0, i + 1 == kLeaves);
}

void Blake2sp::Update(const uint8_t* data, size_t size) {
    while (size) {
        const size_t take = std::min<size_t>(size, kBlock - size_t(count_ % kBlock));
        leaves_[size_t(count_ / kBlock % kLeaves)].Update(data, take);
        count_ += take;
        data += take;
        size -= take;
    }
}

std::array<uint8_t, 32> Blake2sp::Finish() {
    Blake2s root;
    root.Start(0, 1, true);
    for (Blake2s& leaf : leaves_) {
        const auto digest = leaf.Finish();
        root.Update(digest.data(), digest.size());
    }
    return root.Finish();
}

} // namespace jpt::rar
