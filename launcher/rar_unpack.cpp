#include "rar_unpack.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#include "rar_ppm.h"
#include "third_party/lzma/7zCrc.h"

namespace jpt::rar {
namespace {

enum class Step { kGoOn, kEndOfFile, kDamaged, kUnsupported, kNoMemory };

constexpr size_t kLengthCodes = 28, kMostDistances = 80, kPreCodes = 20;

// Lengths and distances of the 2.x and 3.x methods: a base and the bits added to it.
struct Slots {
    uint32_t base[kMostDistances];
    uint8_t bits[kMostDistances];
};

constexpr Slots MakeLengths() {
    Slots s{};
    uint32_t base = 0;
    for (size_t i = 0; i < kLengthCodes; ++i) {
        s.bits[i] = uint8_t(i < 8 ? 0 : i / 4 - 1);
        s.base[i] = base;
        base += 1u << s.bits[i];
    }
    return s;
}

// The codes of one bit count come in pairs, but for the first and the last ones.
constexpr Slots MakeDistances(bool long_ones) {
    Slots s{};
    uint32_t base = 0;
    for (size_t i = 0; i < (long_ones ? 60u : 48u); ++i) {
        s.bits[i] = uint8_t(i < 4 ? 0 : i < 34 ? i / 2 - 1 : i < 48 ? 16 : 18);
        s.base[i] = base;
        base += 1u << s.bits[i];
    }
    return s;
}

constexpr Slots kLengths = MakeLengths();
constexpr Slots kDistances20 = MakeDistances(false);
constexpr Slots kDistances29 = MakeDistances(true);
constexpr uint32_t kShortBase[8] = {0, 4, 8, 16, 32, 64, 128, 192};
constexpr uint8_t kShortBits[8] = {2, 2, 3, 4, 5, 6, 6, 6};

// The code that the code lengths are packed with.
bool ReadPreCode(BitSource& in, PrefixCode& code) {
    uint8_t lengths[kPreCodes] = {};
    for (size_t i = 0; i < kPreCodes;) {
        if (!in.Need()) return false;
        const uint32_t length = in.Take(4);
        if (length != 15) {
            lengths[i++] = uint8_t(length);
            continue;
        }
        const uint32_t zeros = in.Take(4);
        if (!zeros) lengths[i++] = 15;
        else i = std::min<size_t>(kPreCodes, i + zeros + 2);
    }
    return code.Build(lengths, kPreCodes);
}

// Each length is the one before it at the same place plus a code (RAR 3), or the code itself (RAR 5).
bool ReadLengths(BitSource& in, const PrefixCode& code, uint8_t* table, size_t count, bool add) {
    for (size_t i = 0; i < count;) {
        if (!in.Need()) return false;
        const int symbol = code.Decode(in);
        if (symbol < 0) return false;
        if (symbol < 16) {
            table[i] = uint8_t(add ? (symbol + table[i]) & 15 : symbol);
            ++i;
            continue;
        }
        const bool repeat = symbol < 18;
        if (repeat && !i) return false;
        size_t run = symbol % 2 ? in.Take(7) + 11 : in.Take(3) + 3;
        const uint8_t value = repeat ? table[i - 1] : 0;
        for (; run && i < count; --run) table[i++] = value;
    }
    return true;
}

// Four latest distances, the newest first.
struct Distances {
    uint64_t at[4] = {};
    void Add(uint64_t distance) {
        std::copy_backward(at, at + 3, at + 4);
        at[0] = distance;
    }
    uint64_t Use(size_t index) {
        const uint64_t distance = at[index];
        std::copy_backward(at, at + index, at + index + 1);
        return at[0] = distance;
    }
};

} // namespace

// ---- RAR 2.0: LZ with prefix codes, or up to four channels of samples

class Unpacker::Lz20 {
public:
    void Reset() { *this = Lz20(); }

    Step Run(Window& window, BitSource& in, Output& out) {
        if (!tables_ && !ReadTables(in)) return Step::kDamaged;
        while (out.decoded() + window.pending() < out.size()) {
            if (!in.Need()) return Step::kDamaged;
            if (window.due()) window.Flush(out);
            if (out.state() != OutputState::kOk) return Step::kDamaged;
            const Step step = audio_ ? Sample(window, in) : Symbol(window, in);
            if (step != Step::kGoOn) return step;
        }
        LastTables(in);
        return Step::kEndOfFile;
    }

private:
    static constexpr size_t kMain = 298, kDistance = 48, kSamples = 257;
    static constexpr uint32_t kNone = 0xFFFFFFFF;

    struct Channel {
        int weights[5] = {}, older[4] = {}, delta = 0, last = 0;
        uint32_t errors[11] = {}, count = 0;
    };

    bool ReadTables(BitSource& in) {
        if (!in.Need()) return false;
        audio_ = in.Take(1) != 0;
        if (!in.Take(1)) std::fill(std::begin(lengths_), std::end(lengths_), uint8_t(0));
        size_t count = kMain + kDistance + kLengthCodes;
        if (audio_) {
            channels_ = in.Take(2) + 1;
            if (channel_ >= channels_) channel_ = 0;
            count = kSamples * channels_;
        }
        uint8_t pre[19];
        for (uint8_t& length : pre) {
            if (!in.Need()) return false;
            length = uint8_t(in.Take(4));
        }
        PrefixCode code;
        if (!code.Build(pre, sizeof pre)) return false;
        for (size_t i = 0; i < count;) {
            if (!in.Need()) return false;
            const int symbol = code.Decode(in);
            if (symbol < 0) return false;
            if (symbol < 16) {
                lengths_[i] = uint8_t((symbol + lengths_[i]) & 15);
                ++i;
                continue;
            }
            if (symbol == 16 && !i) return false;
            const uint8_t value = symbol == 16 ? lengths_[i - 1] : 0;
            size_t run = symbol == 16 ? in.Take(2) + 3 : symbol == 17 ? in.Take(3) + 3 : in.Take(7) + 11;
            for (; run && i < count; --run) lengths_[i++] = value;
        }
        if (audio_) {
            for (size_t i = 0; i < channels_; ++i)
                if (!sample_[i].Build(lengths_ + i * kSamples, kSamples)) return false;
        } else if (!main_.Build(lengths_, kMain) || !distance_.Build(lengths_ + kMain, kDistance) || !length_.Build(lengths_ + kMain + kDistance, kLengthCodes)) {
            return false;
        }
        return tables_ = true;
    }

    // What the next solid file starts with.
    void LastTables(BitSource& in) {
        if (!in.Need() || in.left() < 5) return;
        if ((audio_ ? sample_[channel_].Decode(in) : main_.Decode(in)) == (audio_ ? 256 : 269)) ReadTables(in);
    }

    Step Copy(Window& window, uint32_t length, uint32_t distance) {
        last_distance_ = distance;
        last_length_ = length;
        old_[oldest_++ & 3] = distance;
        window.Copy(distance, length);
        return Step::kGoOn;
    }

    Step Symbol(Window& window, BitSource& in) {
        const int symbol = main_.Decode(in);
        if (symbol < 0) return Step::kDamaged;
        if (symbol < 256) {
            window.Put(uint8_t(symbol));
            return Step::kGoOn;
        }
        if (symbol == 269) return ReadTables(in) ? Step::kGoOn : Step::kDamaged;
        if (symbol == 256) return Copy(window, last_length_, last_distance_);
        if (symbol >= 261 && symbol < 269) {
            const size_t slot = size_t(symbol - 261);
            const uint32_t distance = kShortBase[slot] + 1 + in.Take(kShortBits[slot]);
            return Copy(window, 2, distance);
        }
        if (symbol < 261) {
            const uint32_t distance = old_[(oldest_ - uint32_t(symbol - 256)) & 3];
            const int slot = length_.Decode(in);
            if (slot < 0) return Step::kDamaged;
            const uint32_t length = kLengths.base[slot] + 2 + in.Take(kLengths.bits[slot]);
            return Copy(window, length + (distance >= 0x101) + (distance >= 0x2000) + (distance >= 0x40000), distance);
        }
        const size_t slot = size_t(symbol - 270);
        const uint32_t length = kLengths.base[slot] + 3 + in.Take(kLengths.bits[slot]);
        const int far = distance_.Decode(in);
        if (far < 0) return Step::kDamaged;
        const uint32_t distance = kDistances20.base[far] + 1 + in.Take(kDistances20.bits[far]);
        return Copy(window, length + (distance >= 0x2000) + (distance >= 0x40000), distance);
    }

    Step Sample(Window& window, BitSource& in) {
        const int symbol = sample_[channel_].Decode(in);
        if (symbol < 0) return Step::kDamaged;
        if (symbol == 256) return ReadTables(in) ? Step::kGoOn : Step::kDamaged;
        window.Put(Predict(channel_at_[channel_], uint8_t(symbol)));
        if (++channel_ == channels_) channel_ = 0;
        return Step::kGoOn;
    }

    uint8_t Predict(Channel& c, uint8_t residue) {
        ++c.count;
        c.older[3] = c.older[2];
        c.older[2] = c.older[1];
        c.older[1] = c.delta - c.older[0];
        c.older[0] = c.delta;
        const int near[5] = {c.older[0], c.older[1], c.older[2], c.older[3], neighbour_};
        int sum = 8 * c.last;
        for (int i = 0; i < 5; ++i) sum += c.weights[i] * near[i];
        const uint8_t value = uint8_t((uint32_t(sum) >> 3) - residue);
        const int scaled = int(int8_t(residue)) * 8;
        c.errors[0] += uint32_t(std::abs(scaled));
        for (int i = 0; i < 5; ++i) {
            c.errors[1 + 2 * i] += uint32_t(std::abs(scaled - near[i]));
            c.errors[2 + 2 * i] += uint32_t(std::abs(scaled + near[i]));
        }
        neighbour_ = c.delta = int8_t(uint8_t(value - c.last));
        c.last = value;
        if (c.count % 32) return value;
        const size_t best = size_t(std::min_element(std::begin(c.errors), std::end(c.errors)) - c.errors);
        std::fill(std::begin(c.errors), std::end(c.errors), 0u);
        if (best) {
            int& weight = c.weights[(best - 1) / 2];
            if (best % 2 ? weight >= -16 : weight < 16) weight += best % 2 ? -1 : 1;
        }
        return value;
    }

    uint8_t lengths_[kSamples * 4] = {};
    PrefixCode main_, distance_, length_, sample_[4];
    Channel channel_at_[4];
    uint32_t old_[4] = {kNone, kNone, kNone, kNone}, oldest_ = 0, last_length_ = 0, last_distance_ = kNone;
    size_t channels_ = 1, channel_ = 0;
    int neighbour_ = 0;
    bool audio_ = false, tables_ = false;
};

// ---- RAR 2.9: LZ with prefix codes or PPM, and filters as programs of a virtual machine. The programs are known ones.

class Unpacker::Lz29 {
public:
    void Reset() {
        std::fill(std::begin(lengths_), std::end(lengths_), uint8_t(0));
        old_ = Distances();
        last_length_ = last_distance_ = low_ = 0;
        low_repeats_ = 0;
        tables_ = ppm_ = false;
        escape_ = 2;
        ResetFilters();
    }

    Step Run(Window& window, BitSource& in, Output& out) {
        if (!tables_) {
            const Step step = ReadTables(in);
            if (step != Step::kGoOn) return step;
        }
        for (;;) {
            if (!in.Need()) return out.decoded() + window.pending() == out.size() ? Step::kEndOfFile : Step::kDamaged;
            if (window.due()) window.Flush(out);
            if (out.state() != OutputState::kOk || out.decoded() > out.size()) return Step::kDamaged;
            const Step step = ppm_ ? PpmSymbol(window, in, out) : Symbol(window, in, out);
            if (step != Step::kGoOn) return step;
        }
    }

private:
    static constexpr size_t kMain = 299, kDistance = 60, kLow = 17, kAll = kMain + kDistance + kLow + kLengthCodes;
    static constexpr size_t kMostPrograms = 1024;

    struct Program {
        FilterKind kind = FilterKind::kUnknown;
        uint32_t length = 0;
    };

    void ResetFilters() {
        programs_.clear();
        last_program_ = 0;
    }

    Step ReadTables(BitSource& in) {
        tables_ = false;
        in.Align();
        if (!in.Need()) return Step::kDamaged;
        if (in.Peek16() & 0x8000) return StartPpm(in);
        in.Skip(1);
        ppm_ = false;
        low_ = 0;
        low_repeats_ = 0;
        if (!in.Take(1)) std::fill(std::begin(lengths_), std::end(lengths_), uint8_t(0));
        PrefixCode code;
        if (!ReadPreCode(in, code) || !ReadLengths(in, code, lengths_, kAll, true)) return Step::kDamaged;
        const uint8_t* at = lengths_;
        if (!main_.Build(at, kMain) || !distance_.Build(at += kMain, kDistance) || !low_code_.Build(at += kDistance, kLow) || !length_.Build(at + kLow, kLengthCodes))
            return Step::kDamaged;
        tables_ = true;
        return Step::kGoOn;
    }

    Step StartPpm(BitSource& in) {
        const uint32_t flags = in.Byte();
        const bool fresh = (flags & 0x20) != 0;
        uint32_t megabytes = 0;
        if (fresh) megabytes = in.Byte() + 1;
        else if (!ppm_model_.started()) return Step::kDamaged;
        if (flags & 0x40) escape_ = int(in.Byte());
        ppm_model_.StartRange(in);
        if (in.overrun()) return Step::kDamaged;
        if (fresh) {
            unsigned order = (flags & 31) + 1;
            if (order > 16) order = 16 + (order - 16) * 3;
            if (order == 1) return Step::kDamaged;
            if (!ppm_model_.Start(order, megabytes << 20)) return Step::kNoMemory;
        }
        ppm_ = true;
        return Step::kGoOn;
    }

    Step PpmSymbol(Window& window, BitSource& in, Output& out) {
        const int symbol = ppm_model_.Decode(in);
        if (symbol < 0) return Step::kDamaged;
        if (symbol != escape_) {
            window.Put(uint8_t(symbol));
            return Step::kGoOn;
        }
        const auto next = [&] { return ppm_model_.Decode(in); };
        const int code = next();
        switch (code) {
        case -1: return Step::kDamaged;
        case 0: return ReadTables(in);
        case 2: return Step::kEndOfFile;
        case 3: return ReadFilter(window, out, next);
        case 4: {
            uint32_t distance = 0;
            for (int i = 0; i < 3; ++i) {
                const int byte = next();
                if (byte < 0) return Step::kDamaged;
                distance = distance << 8 | uint32_t(byte);
            }
            const int length = next();
            if (length < 0) return Step::kDamaged;
            window.Copy(uint64_t(distance) + 2, size_t(length) + 32);
            return Step::kGoOn;
        }
        case 5: {
            const int length = next();
            if (length < 0) return Step::kDamaged;
            window.Copy(1, size_t(length) + 4);
            return Step::kGoOn;
        }
        default: window.Put(uint8_t(symbol)); return Step::kGoOn;
        }
    }

    Step Symbol(Window& window, BitSource& in, Output& out) {
        const int symbol = main_.Decode(in);
        if (symbol < 0) return Step::kDamaged;
        if (symbol < 256) {
            window.Put(uint8_t(symbol));
            return Step::kGoOn;
        }
        if (symbol >= 271) {
            const size_t slot = size_t(symbol - 271);
            const uint32_t length = kLengths.base[slot] + 3 + in.Take(kLengths.bits[slot]);
            const int far = distance_.Decode(in);
            if (far < 0) return Step::kDamaged;
            uint32_t distance = kDistances29.base[far] + 1;
            const unsigned bits = kDistances29.bits[far];
            if (far <= 9) {
                distance += in.Take(bits);
            } else {
                if (bits > 4) distance += in.Take(bits - 4) << 4;
                if (low_repeats_) {
                    --low_repeats_;
                } else {
                    const int low = low_code_.Decode(in);
                    if (low < 0) return Step::kDamaged;
                    if (low == 16) low_repeats_ = 15;
                    else low_ = uint32_t(low);
                }
                distance += low_;
            }
            old_.Add(distance);
            return Copy(window, length + (distance >= 0x2000) + (distance >= 0x40000), distance);
        }
        if (symbol == 256) return EndOfBlock(in);
        if (symbol == 257) {
            return ReadFilter(window, out, [&in] { return in.Need() ? int(in.Take(8)) : -1; });
        }
        if (symbol == 258) {
            if (last_length_) window.Copy(last_distance_, last_length_);
            return Step::kGoOn;
        }
        if (symbol < 263) {
            const uint32_t distance = uint32_t(old_.Use(size_t(symbol - 259)));
            const int slot = length_.Decode(in);
            if (slot < 0) return Step::kDamaged;
            return Copy(window, kLengths.base[slot] + 2 + in.Take(kLengths.bits[slot]), distance);
        }
        const size_t slot = size_t(symbol - 263);
        const uint32_t distance = kShortBase[slot] + 1 + in.Take(kShortBits[slot]);
        old_.Add(distance);
        return Copy(window, 2, distance);
    }

    Step Copy(Window& window, uint32_t length, uint32_t distance) {
        last_length_ = length;
        last_distance_ = distance;
        window.Copy(distance, length);
        return Step::kGoOn;
    }

    Step EndOfBlock(BitSource& in) {
        if (in.Take(1)) return ReadTables(in);
        tables_ = !in.Take(1);
        return Step::kEndOfFile;
    }

    // The bits of a filter's code.
    class CodeBits {
    public:
        explicit CodeBits(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
        uint32_t Take(unsigned bits) {
            uint32_t value = 0;
            for (; bits; --bits, ++at_) value = value << 1 | (at_ / 8 < bytes_.size() ? bytes_[at_ / 8] >> (7 - at_ % 8) & 1u : 0u);
            return value;
        }
        uint32_t Number() {
            switch (Take(2)) {
            case 0: return Take(4);
            case 1: {
                const uint32_t high = Take(4);
                return high ? high << 4 | Take(4) : 0xFFFFFF00u | Take(8);
            }
            case 2: return Take(16);
            default: return Take(32);
            }
        }
        bool past_end() const { return at_ > bytes_.size() * 8; }

    private:
        const std::vector<uint8_t>& bytes_;
        size_t at_ = 0;
    };

    static FilterKind KnownProgram(const std::vector<uint8_t>& code) {
        static constexpr struct {
            uint32_t length, crc;
            FilterKind kind;
        } kKnown[] = {{53, 0xAD576887, FilterKind::kE8},    {57, 0x3CD7E57E, FilterKind::kE8E9},  {120, 0x3769893F, FilterKind::kItanium},
                      {29, 0x0E06077D, FilterKind::kDelta}, {149, 0x1C2C5DC8, FilterKind::kRgb}, {216, 0xBC85E701, FilterKind::kAudio}};
        const uint32_t crc = CrcCalc(code.data(), code.size());
        for (const auto& known : kKnown)
            if (known.length == code.size() && known.crc == crc) return known.kind;
        return FilterKind::kUnknown;
    }

    template <typename NextByte>
    Step ReadFilter(Window& window, Output& out, const NextByte& next) {
        const int first = next();
        if (first < 0) return Step::kDamaged;
        uint32_t length = uint32_t(first & 7) + 1;
        if (length > 6) {
            const bool wide = length == 8;
            const int high = wide ? next() : 0, low = next();
            if (high < 0 || low < 0) return Step::kDamaged;
            length = wide ? uint32_t(high) << 8 | uint32_t(low) : uint32_t(low) + 7;
        }
        std::vector<uint8_t> code(length);
        for (uint8_t& to : code) {
            const int byte = next();
            if (byte < 0) return Step::kDamaged;
            to = uint8_t(byte);
        }
        return length ? AddFilter(uint32_t(first), code, window, out) : Step::kDamaged;
    }

    Step AddFilter(uint32_t flags, const std::vector<uint8_t>& code, Window& window, Output& out) {
        constexpr uint32_t kWindowMask = (1u << 22) - 1, kMostLength = 0x40000;
        CodeBits bits(code);
        size_t index = last_program_;
        if (flags & 0x80) {
            index = bits.Number();
            if (!index) {
                ResetFilters();
                out.DropFilters();
            } else {
                --index;
            }
        }
        if (index > programs_.size() || index >= kMostPrograms) return Step::kDamaged;
        last_program_ = index;
        const bool fresh = index == programs_.size();
        if (fresh) programs_.push_back({});
        Program& program = programs_[index];
        Filter filter;
        uint32_t start = bits.Number();
        if (flags & 0x40) start += 258;
        filter.start = out.decoded() + window.pending() + (start & kWindowMask);
        if (flags & 0x20) program.length = bits.Number();
        filter.length = program.length;
        uint32_t registers[7] = {0, 0, 0, 0, filter.length, 0, 0};
        if (flags & 0x10) {
            const uint32_t mask = bits.Take(7);
            for (int i = 0; i < 7; ++i)
                if (mask >> i & 1) registers[i] = bits.Number();
        }
        if (fresh) {
            const uint32_t size = bits.Number();
            if (!size || size > code.size()) return Step::kDamaged;
            std::vector<uint8_t> bytes(size);
            for (uint8_t& byte : bytes) byte = uint8_t(bits.Take(8));
            program.kind = KnownProgram(bytes);
        }
        if (bits.past_end() || filter.length > kMostLength) return Step::kDamaged;
        if (program.kind == FilterKind::kUnknown || registers[4] != filter.length) return Step::kUnsupported;
        filter.kind = program.kind;
        filter.channels = registers[0];
        filter.position = registers[1];
        return out.Add(filter) ? Step::kGoOn : Step::kDamaged;
    }

    uint8_t lengths_[kAll] = {};
    PrefixCode main_, distance_, low_code_, length_;
    Distances old_;
    uint32_t last_length_ = 0, last_distance_ = 0, low_ = 0;
    int low_repeats_ = 0, escape_ = 2;
    bool tables_ = false, ppm_ = false;
    PpmModel ppm_model_;
    std::vector<Program> programs_;
    size_t last_program_ = 0;
};

// ---- RAR 5.0 and its 7.0 variant: LZ in blocks of a known size

class Unpacker::Lz50 {
public:
    void Reset() {
        old_ = Distances();
        last_length_ = 0;
        tables_ = false;
    }
    void UseLongDistances(bool use) {
        if (use != (distances_ == kMostDistances)) tables_ = false;
        distances_ = use ? kMostDistances : kDistance;
    }

    Step Run(Window& window, BitSource& in, Output& out) {
        bool last = false;
        while (!last) {
            if (!ReadBlock(in, last) || !tables_) return Step::kDamaged;
            while (in.bit_position() < block_end_) {
                if (!in.Need()) return Step::kDamaged;
                if (window.due()) window.Flush(out);
                if (out.state() != OutputState::kOk || out.decoded() > out.size()) return Step::kDamaged;
                const Step step = Symbol(window, in, out);
                if (step != Step::kGoOn) return step;
            }
        }
        return Step::kEndOfFile;
    }

private:
    static constexpr size_t kMain = 306, kDistance = 64, kLow = 16, kLength = 44;

    bool ReadBlock(BitSource& in, bool& last) {
        in.Align();
        if (!in.Need()) return false;
        const uint32_t flags = in.Take(8), check = in.Take(8), size_bytes = (flags >> 3 & 3) + 1;
        if (size_bytes == 4) return false;
        uint32_t size = 0;
        for (uint32_t i = 0; i < size_bytes; ++i) size |= in.Take(8) << (8 * i);
        if (((0x5A ^ flags ^ size ^ (size >> 8) ^ (size >> 16)) & 0xFF) != check) return false;
        last = (flags & 0x40) != 0;
        block_end_ = in.bit_position() + (size ? (uint64_t(size) - 1) * 8 + (flags & 7) + 1 : 0);
        return !(flags & 0x80) || ReadTables(in);
    }

    bool ReadTables(BitSource& in) {
        uint8_t lengths[kMain + kMostDistances + kLow + kLength] = {};
        PrefixCode code;
        if (!ReadPreCode(in, code) || !ReadLengths(in, code, lengths, kMain + distances_ + kLow + kLength, false)) return false;
        const uint8_t* at = lengths;
        return tables_ = main_.Build(at, kMain) && distance_.Build(at += kMain, distances_) && low_code_.Build(at += distances_, kLow) && length_.Build(at + kLow, kLength);
    }

    static uint32_t Length(BitSource& in, uint32_t slot) {
        if (slot < 8) return 2 + slot;
        const unsigned bits = slot / 4 - 1;
        return 2 + ((4 | (slot & 3)) << bits) + in.Take(bits);
    }

    Step Symbol(Window& window, BitSource& in, Output& out) {
        const int symbol = main_.Decode(in);
        if (symbol < 0) return Step::kDamaged;
        if (symbol < 256) {
            window.Put(uint8_t(symbol));
            return Step::kGoOn;
        }
        if (symbol >= 262) {
            uint32_t length = Length(in, uint32_t(symbol - 262));
            const int slot = distance_.Decode(in);
            if (slot < 0) return Step::kDamaged;
            uint64_t distance = 1 + uint64_t(slot);
            if (slot >= 4) {
                const unsigned bits = unsigned(slot) / 2 - 1;
                distance = 1 + (uint64_t(2 | (slot & 1)) << bits);
                if (bits < 4) {
                    distance += in.Take(bits);
                } else {
                    distance += in.TakeLong(bits - 4) << 4;
                    const int low = low_code_.Decode(in);
                    if (low < 0) return Step::kDamaged;
                    distance += uint64_t(low);
                }
            }
            length += (distance > 0x100) + (distance > 0x2000) + (distance > 0x40000);
            old_.Add(distance);
            last_length_ = length;
            window.Copy(distance, length);
            return Step::kGoOn;
        }
        if (symbol == 256) return ReadFilter(window, in, out);
        if (symbol == 257) {
            if (last_length_) window.Copy(old_.at[0], last_length_);
            return Step::kGoOn;
        }
        const uint64_t distance = old_.Use(size_t(symbol - 258));
        const int slot = length_.Decode(in);
        if (slot < 0) return Step::kDamaged;
        last_length_ = Length(in, uint32_t(slot));
        window.Copy(distance, last_length_);
        return Step::kGoOn;
    }

    static uint32_t Number(BitSource& in) {
        const uint32_t bytes = in.Take(2) + 1;
        uint32_t value = 0;
        for (uint32_t i = 0; i < bytes; ++i) value |= in.Take(8) << (8 * i);
        return value;
    }

    Step ReadFilter(Window& window, BitSource& in, Output& out) {
        constexpr uint32_t kMostLength = 0x400000;
        static constexpr FilterKind kKinds[] = {FilterKind::kDelta, FilterKind::kE8, FilterKind::kE8E9, FilterKind::kArm};
        Filter filter;
        filter.start = out.decoded() + window.pending() + Number(in);
        filter.length = Number(in);
        const uint32_t kind = in.Take(3);
        if (filter.length > kMostLength) return Step::kDamaged;
        if (kind > 3) return Step::kUnsupported;
        filter.kind = kKinds[kind];
        filter.wrap = true;
        if (filter.kind == FilterKind::kDelta) filter.channels = in.Take(5) + 1;
        return out.Add(filter) ? Step::kGoOn : Step::kDamaged;
    }

    PrefixCode main_, distance_, low_code_, length_;
    Distances old_;
    size_t distances_ = kDistance;
    uint32_t last_length_ = 0;
    uint64_t block_end_ = 0;
    bool tables_ = false;
};

Unpacker::Unpacker() = default;
Unpacker::~Unpacker() = default;

template <typename Decoder>
ArchiveError Unpacker::Run(std::unique_ptr<Decoder>& decoder, bool solid, BitSource& in, Output& out) {
    if (!decoder) decoder = std::make_unique<Decoder>();
    if (!solid) decoder->Reset();
    const Step step = decoder->Run(window_, in, out);
    window_.Flush(out);
    if (in.failed()) return ArchiveError::kUnreadable;
    if (out.state() == OutputState::kStopped) return ArchiveError::kCancelled;
    if (step == Step::kUnsupported) return ArchiveError::kUnsupported;
    if (step == Step::kNoMemory) return ArchiveError::kOutOfMemory;
    return step == Step::kEndOfFile && out.complete() ? ArchiveError::kNone : ArchiveError::kDamaged;
}

ArchiveError Unpacker::Unpack(Method method, bool solid, BitSource& in, Output& out) {
    if (!window_.size() && !window_.Allocate(window_size_)) return ArchiveError::kOutOfMemory;
    if (!solid) window_.Reset();
    if (method == Method::k20) return Run(lz20_, solid, in, out);
    if (method == Method::k29) return Run(lz29_, solid, in, out);
    if (!lz50_) lz50_ = std::make_unique<Lz50>();
    lz50_->UseLongDistances(method == Method::k70);
    return Run(lz50_, solid, in, out);
}

} // namespace jpt::rar
