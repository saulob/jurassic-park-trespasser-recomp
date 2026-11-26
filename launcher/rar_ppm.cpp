#include "rar_ppm.h"

#include <algorithm>
#include <new>

namespace jpt::rar {
namespace {

constexpr uint32_t kTop = 1u << 24, kBottom = 1u << 15;
constexpr unsigned kTotalBits = 14;
constexpr uint32_t kBinaryScale = 1u << kTotalBits;
constexpr unsigned kMostFrequency = 124;
constexpr uint32_t kFreeStamp = 0xFFFF;

unsigned HighBit(uint8_t symbol) { return symbol >= 0x40 ? 8 : 0; }

} // namespace

uint32_t PpmModel::U32(Ref r) const {
    uint32_t v;
    std::memcpy(&v, &memory_[r], sizeof v);
    return v;
}

void PpmModel::Set16(Ref r, uint32_t v) {
    memory_[r] = uint8_t(v);
    memory_[r + 1] = uint8_t(v >> 8);
}

void PpmModel::Set32(Ref r, uint32_t v) { std::memcpy(&memory_[r], &v, sizeof v); }

void PpmModel::Swap(Ref a, Ref b) {
    uint8_t kept[kState];
    std::memcpy(kept, &memory_[a], kState);
    Move(a, b, kState);
    std::memcpy(&memory_[b], kept, kState);
}

PpmModel::Ref PpmModel::Find(Ref context, uint8_t symbol) {
    if (!IsContext(context)) return 0;
    const uint32_t count = Count(context);
    if (count == 1) return Symbol(OnlyState(context)) == symbol ? OnlyState(context) : 0;
    if (!HasStates(context)) return 0;
    Ref s = States(context);
    for (uint32_t i = 0; i < count; ++i, s += kState)
        if (Symbol(s) == symbol) return s;
    return 0;
}

// ---- memory

bool PpmModel::Start(unsigned order, uint32_t bytes) {
    if (size_ != bytes) {
        memory_.reset();
        size_ = 0;
        // One more unit ends the heap, another is the head of the list of Glue.
        memory_.reset(new (std::nothrow) uint8_t[size_t(bytes) + 2 * kUnit]());
        if (!memory_) return false;
        size_ = bytes;
    }
    order_ = std::min(order, kMostOrder);
    unsigned index = 0, units = 0;
    for (unsigned i = 0; i < kIndexes; ++i) {
        for (unsigned step = i >= 12 ? 4 : i / 4 + 1; step; --step) units_index_[units++] = uint8_t(i);
        index_units_[i] = uint8_t(units);
    }
    for (unsigned i = 0; i < 256; ++i) suffix_index_[i] = uint8_t(i == 0 ? 0 : i == 1 ? 2 : i < 11 ? 4 : 6);
    for (unsigned i = 0; i < 3; ++i) count_index_[i] = uint8_t(i);
    index = 3;
    for (unsigned i = 3, left = 1; i < 256; ++i) {
        count_index_[i] = uint8_t(index);
        if (--left == 0) left = ++index - 2;
    }
    std::fill(std::begin(mask_), std::end(mask_), uint8_t(0));
    epoch_ = 1;
    Restart();
    return true;
}

void PpmModel::PutFree(Ref block, unsigned index) {
    Set32(block, free_[index]);
    free_[index] = block;
}

PpmModel::Ref PpmModel::TakeFree(unsigned index) {
    const Ref block = free_[index];
    free_[index] = U32(block);
    return block;
}

void PpmModel::Split(Ref block, unsigned from, unsigned to) {
    const unsigned over = index_units_[from] - index_units_[to];
    Ref rest = block + index_units_[to] * kUnit;
    unsigned index = units_index_[over - 1];
    if (index_units_[index] != over) {
        const unsigned first = index_units_[--index];
        PutFree(rest, index);
        PutFree(rest + first * kUnit, over - first - 1);
        return;
    }
    PutFree(rest, index);
}

// Joins neighbouring free blocks. A block in the list: stamp, units, next, previous.
void PpmModel::Glue() {
    const Ref head = size_ + kUnit;
    const auto next = [this](Ref r) { return U32(r + 4); };
    const auto previous = [this](Ref r) { return U32(r + 8); };
    const auto remove = [&](Ref r) {
        Set32(previous(r) + 4, next(r));
        Set32(next(r) + 8, previous(r));
    };
    Set16(size_, 0);
    Set32(head + 4, head);
    Set32(head + 8, head);
    if (low_unit_ != high_unit_) U8(low_unit_) = 0;
    for (unsigned i = 0; i < kIndexes; ++i) {
        while (free_[i]) {
            const Ref block = TakeFree(i), first = next(head);
            Set32(block + 4, first);
            Set32(block + 8, head);
            Set32(head + 4, block);
            Set32(first + 8, block);
            Set16(block, kFreeStamp);
            Set16(block + 2, index_units_[i]);
        }
    }
    for (Ref block = next(head); block != head; block = next(block)) {
        for (;;) {
            const uint32_t units = U16(block + 2);
            const Ref after = block + units * kUnit;
            if (after >= size_ || U16(after) != kFreeStamp || units + U16(after + 2) >= 0x10000) break;
            remove(after);
            Set16(block + 2, units + U16(after + 2));
        }
    }
    while (next(head) != head) {
        Ref block = next(head);
        remove(block);
        uint32_t units = U16(block + 2);
        for (; units > 128; units -= 128, block += 128 * kUnit) PutFree(block, kIndexes - 1);
        unsigned index = units_index_[units - 1];
        if (index_units_[index] != units) {
            const unsigned rest = units - index_units_[--index];
            PutFree(block + (units - rest) * kUnit, rest - 1);
        }
        PutFree(block, index);
    }
}

PpmModel::Ref PpmModel::AllocateRare(unsigned index) {
    if (!glue_) {
        glue_ = 255;
        Glue();
        if (free_[index]) return TakeFree(index);
    }
    unsigned larger = index;
    do {
        if (++larger == kIndexes) {
            --glue_;
            const uint32_t bytes = index_units_[index] * kUnit;
            return units_ - text_ > bytes ? units_ -= bytes : 0;
        }
    } while (!free_[larger]);
    const Ref block = TakeFree(larger);
    Split(block, larger, index);
    return block;
}

PpmModel::Ref PpmModel::AllocateUnits(unsigned units) {
    const unsigned index = units_index_[units - 1];
    if (free_[index]) return TakeFree(index);
    const uint32_t bytes = index_units_[index] * kUnit;
    if (bytes > high_unit_ - low_unit_) return AllocateRare(index);
    const Ref block = low_unit_;
    low_unit_ += bytes;
    return block;
}

PpmModel::Ref PpmModel::AllocateContext() {
    if (high_unit_ != low_unit_) return high_unit_ -= kUnit;
    return free_[0] ? TakeFree(0) : AllocateRare(0);
}

PpmModel::Ref PpmModel::Expand(Ref old, unsigned units) {
    const unsigned from = units_index_[units - 1], to = units_index_[units];
    if (from == to) return old;
    const Ref block = AllocateUnits(units + 1);
    if (block) {
        Move(block, old, units * kUnit);
        PutFree(old, from);
    }
    return block;
}

PpmModel::Ref PpmModel::Shrink(Ref old, unsigned units, unsigned fewer) {
    const unsigned from = units_index_[units - 1], to = units_index_[fewer - 1];
    if (from == to) return old;
    if (free_[to]) {
        const Ref block = TakeFree(to);
        Move(block, old, fewer * kUnit);
        PutFree(old, from);
        return block;
    }
    Split(old, from, to);
    return old;
}

// ---- model

void PpmModel::Restart() {
    static constexpr uint16_t kEscapes[8] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
    std::fill(std::begin(free_), std::end(free_), 0u);
    std::fill(std::begin(mask_), std::end(mask_), uint8_t(0));
    text_ = 0;
    high_unit_ = size_;
    low_unit_ = units_ = size_ - size_ / 8 / kUnit * 7 * kUnit;
    glue_ = 0;
    fall_ = order_;
    run_ = first_run_ = -int(std::min(order_, 12u)) - 1;
    success_ = 0;
    min_ = max_ = AllocateContext();
    Set16(min_, 256);
    Set16(min_ + 2, 257);
    Set32(min_ + 8, 0);
    found_ = AllocateUnits(128);
    Set32(min_ + 4, found_);
    for (unsigned i = 0; i < 256; ++i) {
        const Ref s = found_ + i * kState;
        Symbol(s) = uint8_t(i);
        Frequency(s) = 1;
        SetSuccessor(s, 0);
    }
    for (unsigned i = 0; i < 128; ++i)
        for (unsigned k = 0; k < 64; ++k) binary_[i][k] = uint16_t(kBinaryScale - kEscapes[k % 8] / (i + 2));
    for (unsigned i = 0; i < 25; ++i)
        for (Estimate& e : estimates_[i]) e = {uint16_t((5 * i + 10) << (kPeriodBits - 4)), uint8_t(kPeriodBits - 4), 4};
}

void PpmModel::Rescale() {
    const uint32_t old_count = Count(min_);
    uint32_t count = old_count;
    const Ref states = States(min_);
    Ref s = found_;
    for (; s != states; s -= kState) Swap(s, s - kState);
    Frequency(s) = uint8_t(Frequency(s) + 4);
    Set16(min_ + 2, Sum(min_) + 4);
    int escape = int(Sum(min_)) - Frequency(s);
    const unsigned adder = fall_ != 0;
    Frequency(s) = uint8_t((Frequency(s) + adder) >> 1);
    uint32_t sum = Frequency(s);
    for (uint32_t i = 1; i < count; ++i) {
        s += kState;
        escape -= Frequency(s);
        Frequency(s) = uint8_t((Frequency(s) + adder) >> 1);
        sum += Frequency(s);
        if (Frequency(s) <= Frequency(s - kState)) continue;
        uint8_t kept[kState];
        std::memcpy(kept, &memory_[s], kState);
        Ref to = s;
        do {
            Move(to, to - kState, kState);
            to -= kState;
        } while (to != states && kept[1] > Frequency(to - kState));
        std::memcpy(&memory_[to], kept, kState);
    }
    if (Frequency(s) == 0) {
        uint32_t dropped = 0;
        do {
            ++dropped;
            s -= kState;
        } while (s != states && Frequency(s) == 0);
        escape += int(dropped);
        count -= dropped;
        Set16(min_, count);
        if (count == 1) {
            uint8_t kept[kState];
            std::memcpy(kept, &memory_[states], kState);
            do {
                kept[1] = uint8_t(kept[1] - (kept[1] >> 1));
                escape >>= 1;
            } while (escape > 1);
            PutFree(states, units_index_[((old_count + 1) >> 1) - 1]);
            found_ = OnlyState(min_);
            std::memcpy(&memory_[found_], kept, kState);
            return;
        }
    }
    escape -= escape >> 1;
    Set16(min_ + 2, sum + uint32_t(escape));
    const unsigned units = (old_count + 1) >> 1, fewer = (count + 1) >> 1;
    if (units != fewer) Set32(min_ + 4, Shrink(states, units, fewer));
    found_ = States(min_);
}

PpmModel::Ref PpmModel::NewSuccessors(bool skip, Ref state) {
    Ref context = min_;
    const Ref branch = Successor(found_);
    const uint8_t symbol = Symbol(found_);
    Ref path[kMostOrder];
    unsigned depth = 0;
    if (!skip) path[depth++] = found_;
    while (Suffix(context)) {
        context = Suffix(context);
        Ref s = state;
        state = 0;
        if (!s) s = Find(context, symbol);
        if (!s) return 0;
        if (Successor(s) != branch) {
            context = Successor(s);
            break;
        }
        if (depth == kMostOrder) return 0;
        path[depth++] = s;
    }
    if (!IsContext(context)) return 0;
    if (!depth) return context;
    if (branch >= size_) return 0;
    const uint8_t next_symbol = U8(branch);
    unsigned frequency;
    if (Count(context) != 1) {
        const Ref s = Find(context, next_symbol);
        if (!s) return 0;
        const uint32_t cf = Frequency(s) - 1u, s0 = Sum(context) - Count(context) - cf;
        if (2 * cf <= s0) frequency = 1 + (5 * cf > s0);
        else frequency = 1 + (s0 ? (2 * cf + 3 * s0 - 1) / (2 * s0) : 0);
    } else {
        frequency = Frequency(OnlyState(context));
    }
    while (depth) {
        const Ref child = AllocateContext();
        if (!child) return 0;
        Set16(child, 1);
        const Ref only = OnlyState(child);
        Symbol(only) = next_symbol;
        Frequency(only) = uint8_t(frequency);
        SetSuccessor(only, branch + 1);
        Set32(child + 8, context);
        SetSuccessor(path[--depth], child);
        context = child;
    }
    return context;
}

void PpmModel::Update() {
    const uint8_t symbol = Symbol(found_);
    const unsigned frequency = Frequency(found_);
    Ref next = Successor(found_), state = 0;
    const Ref suffix = Suffix(min_);
    const auto restart = [this] {
        Restart();
        epoch_ = 0;
    };
    if (frequency < kMostFrequency / 4 && suffix) {
        state = Find(suffix, symbol);
        if (!state) return restart();
        if (Count(suffix) != 1) {
            if (state != States(suffix) && Frequency(state) >= Frequency(state - kState)) {
                Swap(state, state - kState);
                state -= kState;
            }
            if (Frequency(state) < kMostFrequency - 9) {
                Frequency(state) = uint8_t(Frequency(state) + 2);
                Set16(suffix + 2, Sum(suffix) + 2);
            }
        } else if (Frequency(state) < 32) {
            ++Frequency(state);
        }
    }
    if (!fall_) {
        min_ = max_ = NewSuccessors(true, state);
        SetSuccessor(found_, min_);
        if (!min_) restart();
        return;
    }
    U8(text_++) = symbol;
    Ref successor = text_;
    if (text_ >= units_) return restart();
    if (next) {
        if (next <= text_ && !(next = NewSuccessors(false, state))) return restart();
        if (!--fall_) {
            successor = next;
            if (max_ != min_) --text_;
        }
    } else {
        SetSuccessor(found_, successor);
        next = min_;
    }
    const uint32_t count = Count(min_), rest = Sum(min_) - count - (frequency - 1);
    for (Ref context = max_; context != min_; context = Suffix(context)) {
        if (!IsContext(context)) return restart();
        const uint32_t own = Count(context);
        if (own >= 256) return restart();
        if (own != 1) {
            if (own % 2 == 0) {
                const Ref states = Expand(States(context), own >> 1);
                if (!states) return restart();
                Set32(context + 4, states);
            }
            Set16(context + 2, Sum(context) + (2 * own < count) + 2 * ((4 * own <= count) & (Sum(context) <= 8 * own)));
        } else {
            const Ref states = AllocateUnits(1);
            if (!states) return restart();
            Move(states, OnlyState(context), kState);
            Set32(context + 4, states);
            const unsigned f = Frequency(states);
            Frequency(states) = uint8_t(f < kMostFrequency / 4 - 1 ? 2 * f : kMostFrequency - 4);
            Set16(context + 2, Frequency(states) + escape_ + (count > 3));
        }
        uint32_t cf = 2 * frequency * (Sum(context) + 6);
        const uint32_t sf = rest + Sum(context);
        if (cf < 6 * sf) {
            cf = 1u + (cf > sf) + (cf >= 4 * sf);
            Set16(context + 2, Sum(context) + 3);
        } else {
            cf = 4u + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
            Set16(context + 2, Sum(context) + cf);
        }
        const Ref s = States(context) + own * kState;
        if (uint64_t(s) + kState > size_) return restart();
        Symbol(s) = symbol;
        Frequency(s) = uint8_t(cf);
        SetSuccessor(s, successor);
        Set16(context, own + 1);
    }
    min_ = max_ = next;
}

// ---- range coder

void PpmModel::StartRange(BitSource& in) {
    low_ = code_ = 0;
    range_ = 0xFFFFFFFF;
    for (int i = 0; i < 4; ++i) code_ = code_ << 8 | in.Byte();
}

void PpmModel::Normalize(BitSource& in) {
    while ((low_ ^ (low_ + range_)) < kTop || range_ < kBottom) {
        if ((low_ ^ (low_ + range_)) >= kTop) range_ = (0u - low_) & (kBottom - 1);
        code_ = code_ << 8 | in.Byte();
        range_ <<= 8;
        low_ <<= 8;
        if (in.overrun()) return;
    }
}

uint32_t PpmModel::Scale(uint32_t total) {
    range_ /= total;
    return range_ ? (code_ - low_) / range_ : 0xFFFFFFFF;
}

void PpmModel::Commit() {
    low_ += range_ * span_low_;
    range_ *= span_high_ - span_low_;
}

bool PpmModel::DecodeOnly() {
    static constexpr uint8_t kEscape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};
    const Ref only = OnlyState(min_), suffix = Suffix(min_);
    if (!IsContext(suffix) || !Frequency(only) || Frequency(only) > 128) return false;
    high_bits_ = HighBit(Symbol(found_));
    uint16_t& estimate = binary_[Frequency(only) - 1][success_ + suffix_index_[(Count(suffix) - 1) & 255] + high_bits_ + 2 * HighBit(Symbol(only)) +
                                                      ((uint32_t(run_) >> 26) & 0x20)];
    const uint32_t mean = (estimate + 32u) >> kPeriodBits;
    range_ >>= kTotalBits;
    if (!range_) return false;
    if ((code_ - low_) / range_ < estimate) {
        found_ = only;
        if (Frequency(only) < 128) ++Frequency(only);
        span_low_ = 0;
        span_high_ = estimate;
        estimate = uint16_t(estimate + (1u << kPeriodBits) - mean);
        success_ = 1;
        ++run_;
    } else {
        span_low_ = estimate;
        span_high_ = kBinaryScale;
        estimate = uint16_t(estimate - mean);
        escape_ = kEscape[estimate >> 10];
        masked_ = 1;
        mask_[Symbol(only)] = epoch_;
        success_ = 0;
        found_ = 0;
    }
    return true;
}

bool PpmModel::DecodeFirst() {
    if (!HasStates(min_)) return false;
    const uint32_t total = Sum(min_), count = Count(min_);
    if (!total) return false;
    Ref s = States(min_);
    const uint32_t at = Scale(total);
    if (at >= total) return false;
    uint32_t reached = Frequency(s);
    if (at < reached) {
        span_low_ = 0;
        span_high_ = reached;
        success_ = 2 * reached > total;
        run_ += int(success_);
        found_ = s;
        Frequency(s) = uint8_t(reached + 4);
        Set16(min_ + 2, total + 4);
        if (reached + 4 > kMostFrequency) Rescale();
        return true;
    }
    if (!found_) return false;
    success_ = 0;
    for (uint32_t i = 1; i < count; ++i) {
        s += kState;
        reached += Frequency(s);
        if (reached <= at) continue;
        span_high_ = reached;
        span_low_ = reached - Frequency(s);
        Frequency(s) = uint8_t(Frequency(s) + 4);
        Set16(min_ + 2, total + 4);
        found_ = s;
        if (Frequency(s) > Frequency(s - kState)) {
            Swap(s, s - kState);
            found_ = s - kState;
            if (Frequency(found_) > kMostFrequency) Rescale();
        }
        return true;
    }
    high_bits_ = HighBit(Symbol(found_));
    span_low_ = reached;
    span_high_ = total;
    masked_ = count;
    found_ = 0;
    s = States(min_);
    for (uint32_t i = 0; i < count; ++i, s += kState) mask_[Symbol(s)] = epoch_;
    return true;
}

bool PpmModel::DecodeMasked() {
    const uint32_t count = Count(min_);
    if (count <= masked_ || count > 256 || !HasStates(min_)) return false;
    const uint32_t left = count - masked_;
    Estimate* estimate = nullptr;
    uint32_t total = 1;
    if (count != 256) {
        if (!IsContext(Suffix(min_))) return false;
        estimate = &estimates_[count_index_[left - 1]][(int(left) < int(Count(Suffix(min_))) - int(count)) + 2 * (Sum(min_) < 11 * count) + 4 * (masked_ > left) + high_bits_];
        const uint32_t mean = uint32_t(estimate->sum) >> estimate->shift;
        estimate->sum = uint16_t(estimate->sum - mean);
        total = mean + (mean == 0);
    }
    Ref open[256];
    uint32_t found = 0, reached = 0;
    Ref s = States(min_);
    for (uint32_t i = 0; i < count && found < left; ++i, s += kState) {
        if (mask_[Symbol(s)] == epoch_) continue;
        reached += Frequency(s);
        open[found++] = s;
    }
    if (found != left) return false;
    total += reached;
    const uint32_t at = Scale(total);
    if (at >= total) return false;
    if (at < reached) {
        uint32_t i = 0, high = Frequency(open[0]);
        while (high <= at) high += Frequency(open[++i]);
        s = open[i];
        span_high_ = high;
        span_low_ = high - Frequency(s);
        if (estimate && estimate->shift < kPeriodBits && --estimate->count == 0) {
            estimate->sum = uint16_t(estimate->sum * 2);
            estimate->count = uint8_t(3u << estimate->shift++);
        }
        found_ = s;
        Frequency(s) = uint8_t(Frequency(s) + 4);
        Set16(min_ + 2, Sum(min_) + 4);
        if (Frequency(s) > kMostFrequency) Rescale();
        ++epoch_;
        run_ = first_run_;
    } else {
        span_low_ = reached;
        span_high_ = total;
        for (uint32_t i = 0; i < left; ++i) mask_[Symbol(open[i])] = epoch_;
        if (estimate) estimate->sum = uint16_t(estimate->sum + total);
        masked_ = count;
    }
    return true;
}

int PpmModel::Decode(BitSource& in) {
    if (!IsContext(min_) || !found_) return -1;
    if (!(Count(min_) == 1 ? DecodeOnly() : DecodeFirst())) return -1;
    Commit();
    while (!found_) {
        Normalize(in);
        do {
            ++fall_;
            min_ = Suffix(min_);
            if (!IsContext(min_)) return -1;
        } while (Count(min_) == masked_);
        if (!DecodeMasked()) return -1;
        Commit();
    }
    const uint8_t symbol = Symbol(found_);
    if (!fall_ && Successor(found_) > text_) min_ = max_ = Successor(found_);
    else Update();
    if (!epoch_) {
        epoch_ = 1;
        std::fill(std::begin(mask_), std::end(mask_), uint8_t(0));
    }
    Normalize(in);
    return in.overrun() ? -1 : symbol;
}

} // namespace jpt::rar
