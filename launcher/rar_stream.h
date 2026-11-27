// What every RAR method shares: the bit input, prefix codes, the window, and the output with its filters.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "os_file.h"

namespace jpt::rar {

// The packed bytes of one file, most significant bit first. Need() makes room for what one symbol takes; everything read
// until the next one is unchecked and stays inside the buffer. Past the end there are zeros.
class BitSource {
public:
    BitSource(FileReader& file, uint64_t offset, uint64_t size);

    bool Need() {
        if (at_ + kReach > filled_) Fill();
        return at_ <= filled_ + kTolerance;
    }
    uint32_t Peek16() const {
        const uint8_t* p = &buffer_[at_];
        return ((uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]) >> (8 - bit_)) & 0xFFFF;
    }
    void Skip(unsigned bits) {
        bits += bit_;
        at_ += bits >> 3;
        bit_ = bits & 7;
    }
    // Up to 16 bits.
    uint32_t Take(unsigned bits) {
        const uint32_t value = Peek16() >> (16 - bits);
        Skip(bits);
        return value;
    }
    uint64_t TakeLong(unsigned bits) {
        uint64_t value = 0;
        for (; bits > 16; bits -= 16) value = value << 16 | Take(16);
        return value << bits | Take(bits);
    }
    void Align() {
        if (bit_) Skip(8 - bit_);
    }
    // A whole byte, checked.
    uint32_t Byte() {
        if (!Need()) return overrun_ = true, 0;
        return Take(8);
    }
    bool overrun() const { return overrun_; }
    bool failed() const { return failed_; }
    uint64_t left() const { return left_ + filled_ - std::min(at_, filled_); }
    uint64_t bit_position() const { return (base_ + at_) * 8 + bit_; }

private:
    static constexpr size_t kChunk = 1 << 18, kReach = 64, kTolerance = 8;
    void Fill();

    FileReader& file_;
    uint64_t next_, left_;
    uint64_t base_ = 0; // of the buffer, in the packed bytes
    std::vector<uint8_t> buffer_;
    size_t at_ = 0, filled_ = 0;
    unsigned bit_ = 0;
    bool overrun_ = false, failed_ = false;
};

// A canonical prefix code of up to 15 bits.
class PrefixCode {
public:
    static constexpr size_t kMost = 320;
    bool Build(const uint8_t* lengths, size_t count);
    int Decode(BitSource& in) const {
        const uint32_t bits = in.Peek16();
        if (const uint16_t quick = quick_[bits >> (16 - kQuick)]) {
            in.Skip(quick & 15);
            return quick >> 4;
        }
        return DecodeLong(in, bits);
    }

private:
    static constexpr unsigned kQuick = 10, kBits = 15;
    int DecodeLong(BitSource& in, uint32_t bits) const;

    std::array<uint16_t, 1 << kQuick> quick_{};
    std::array<uint32_t, kBits + 1> limit_{}; // the first code past those of a length, left aligned in 16 bits
    std::array<uint32_t, kBits + 2> first_{};
    std::array<uint16_t, kMost> symbols_{};
};

enum class FilterKind { kE8, kE8E9, kItanium, kDelta, kRgb, kAudio, kArm, kUnknown };

struct Filter {
    FilterKind kind = FilterKind::kUnknown;
    uint64_t start = 0; // of the file's output
    uint32_t length = 0;
    uint32_t channels = 0, position = 0;
    bool wrap = false; // calls: positions modulo 16 MB
};

// False when its parameters make no sense.
bool ApplyFilter(const Filter& filter, std::vector<uint8_t>& data, uint64_t file_offset);

enum class OutputState { kOk, kDamaged, kStopped };

// The output of one file: the decoded bytes in order, with the filters applied, cut at the file's size.
class Output {
public:
    using Sink = std::function<bool(const uint8_t*, size_t)>;
    Output(uint64_t size, const Sink& sink) : size_(size), sink_(sink) {}

    bool Add(const Filter& filter);
    void DropFilters() { filters_.clear(), block_.clear(); }
    void Write(const uint8_t* data, size_t count);
    uint64_t decoded() const { return decoded_; }
    uint64_t size() const { return size_; }
    OutputState state() const { return state_; }
    bool complete() const { return state_ == OutputState::kOk && sent_ == size_; }

private:
    void Send(const uint8_t* data, size_t count);

    uint64_t size_, decoded_ = 0, sent_ = 0;
    const Sink& sink_;
    std::deque<Filter> filters_;
    std::vector<uint8_t> block_;
    OutputState state_ = OutputState::kOk;
};

class Window {
public:
    static constexpr size_t kSmallest = 1 << 18;
    bool Allocate(size_t size);
    size_t size() const { return size_; }
    void Reset() { at_ = pending_ = 0, whole_ = false; }

    void Put(uint8_t byte) {
        data_[at_] = byte;
        ++pending_;
        if (++at_ == size_) at_ = 0, whole_ = true;
    }
    // Where nothing was written yet there are zeros.
    void Copy(uint64_t distance, size_t length);

    size_t pending() const { return pending_; }
    bool due() const { return pending_ >= kSmallest / 2; }
    void Flush(Output& out);

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0, at_ = 0, pending_ = 0;
    bool whole_ = false;
};

} // namespace jpt::rar
