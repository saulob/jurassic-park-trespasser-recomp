// PPMd variant H with the range coder RAR 3 gives it. The model lives in one block of memory and refers into it by offset.
#pragma once

#include <cstdint>
#include <memory>

#include "rar_stream.h"

namespace jpt::rar {

class PpmModel {
public:
    bool started() const { return size_ != 0; }
    // False when there is not enough memory.
    bool Start(unsigned order, uint32_t bytes);
    void StartRange(BitSource& in);
    int Decode(BitSource& in); // -1: damaged

private:
    using Ref = uint32_t; // into memory_, 0: none
    static constexpr unsigned kUnit = 12, kState = 6, kIndexes = 38, kPeriodBits = 7, kMostOrder = 64;

    struct Estimate {
        uint16_t sum;
        uint8_t shift, count;
    };

    uint8_t& U8(Ref r) { return memory_[r]; }
    uint32_t U16(Ref r) const { return uint32_t(memory_[r]) | uint32_t(memory_[r + 1]) << 8; }
    uint32_t U32(Ref r) const;
    void Set16(Ref r, uint32_t v);
    void Set32(Ref r, uint32_t v);

    // A state: symbol, frequency, successor. A context: count, sum, states, suffix; with one state it is in place of sum and states.
    uint8_t& Symbol(Ref s) { return memory_[s]; }
    uint8_t& Frequency(Ref s) { return memory_[s + 1]; }
    Ref Successor(Ref s) const { return U32(s + 2); }
    void SetSuccessor(Ref s, Ref to) { Set32(s + 2, to); }
    uint32_t Count(Ref c) const { return U16(c); }
    uint32_t Sum(Ref c) const { return U16(c + 2); }
    Ref States(Ref c) const { return U32(c + 4); }
    Ref Suffix(Ref c) const { return U32(c + 8); }
    Ref OnlyState(Ref c) const { return c + 2; }
    void Move(Ref to, Ref from, unsigned bytes) { std::memcpy(&memory_[to], &memory_[from], bytes); }
    void Swap(Ref a, Ref b);
    Ref Find(Ref context, uint8_t symbol);

    bool IsContext(Ref c) const { return c > text_ && c <= size_ - kUnit; }
    bool HasStates(Ref c) const { return States(c) > text_ && uint64_t(States(c)) + uint64_t(Count(c)) * kState <= size_; }

    void Restart();
    void Glue();
    void PutFree(Ref block, unsigned index);
    Ref TakeFree(unsigned index);
    void Split(Ref block, unsigned from, unsigned to);
    Ref AllocateRare(unsigned index);
    Ref AllocateUnits(unsigned units);
    Ref AllocateContext();
    Ref Expand(Ref old, unsigned units);
    Ref Shrink(Ref old, unsigned units, unsigned fewer);

    bool DecodeOnly();
    bool DecodeFirst();
    bool DecodeMasked();
    void Rescale();
    Ref NewSuccessors(bool skip, Ref state);
    void Update();
    void Normalize(BitSource& in);
    void Commit();
    uint32_t Scale(uint32_t total);

    std::unique_ptr<uint8_t[]> memory_;
    uint32_t size_ = 0;
    Ref text_ = 0, units_ = 0, low_unit_ = 0, high_unit_ = 0;
    Ref free_[kIndexes] = {};
    unsigned glue_ = 0;

    unsigned order_ = 0, fall_ = 0;
    int run_ = 0, first_run_ = 0;
    unsigned success_ = 0, escape_ = 0, high_bits_ = 0;
    Ref min_ = 0, max_ = 0, found_ = 0;
    uint32_t masked_ = 0;
    uint8_t mask_[256] = {}, epoch_ = 1;

    uint8_t index_units_[kIndexes], units_index_[128], suffix_index_[256], count_index_[256];
    uint16_t binary_[128][64];
    Estimate estimates_[25][16];

    uint32_t low_ = 0, code_ = 0, range_ = 0;
    uint32_t span_low_ = 0, span_high_ = 0;
};

} // namespace jpt::rar
