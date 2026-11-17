#include "inflate.h"

#include <array>

namespace jpt {
namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, 288> symbol{};
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool Bits(int n, uint32_t& value) {
        while (have_ < n) {
            if (pos_ >= size_) return false;
            buffer_ |= static_cast<uint32_t>(data_[pos_++]) << have_;
            have_ += 8;
        }
        value = buffer_ & ((1u << n) - 1);
        buffer_ >>= n;
        have_ -= n;
        return true;
    }

    void AlignToByte() {
        buffer_ = 0;
        have_ = 0;
    }

    bool Byte(uint8_t& b) {
        if (pos_ >= size_) return false;
        b = data_[pos_++];
        return true;
    }

    // Canonical code, read bit by bit.
    bool Decode(const Huffman& h, int& out) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t bit;
            if (!Bits(1, bit)) return false;
            code |= static_cast<int>(bit);
            const int count = h.count[len];
            if (code - count < first) {
                out = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t buffer_ = 0;
    int have_ = 0;
};

// False for an over-subscribed set of lengths; an incomplete one is allowed (a single distance code is legal).
bool Build(Huffman& h, const uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::array<uint16_t, kMaxBits + 1> offs{};
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
}

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Reader& in, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out, size_t start, size_t max_out) {
    for (;;) {
        int sym;
        if (!in.Decode(lit, sym)) return false;
        if (sym < 256) {
            if (out.size() - start >= max_out) return false;
            out.push_back(static_cast<uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        uint32_t extra;
        if (!in.Bits(kLengthExtra[sym], extra)) return false;
        const size_t len = kLengthBase[sym] + extra;
        int dsym;
        if (!in.Decode(dist, dsym) || dsym >= 30) return false;
        if (!in.Bits(kDistExtra[dsym], extra)) return false;
        const size_t back = kDistBase[dsym] + extra;
        if (back > out.size() - start || out.size() - start + len > max_out) return false;
        const size_t from = out.size() - back;
        for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
}

bool Fixed(Reader& in, std::vector<uint8_t>& out, size_t start, size_t max_out) {
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        uint8_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Build(t.first, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        Build(t.second, lengths, 30);
        return t;
    }();
    return Codes(in, tables.first, tables.second, out, start, max_out);
}

bool Dynamic(Reader& in, std::vector<uint8_t>& out, size_t start, size_t max_out) {
    static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint32_t nlen, ndist, ncode;
    if (!in.Bits(5, nlen) || !in.Bits(5, ndist) || !in.Bits(4, ncode)) return false;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {};
    for (uint32_t i = 0; i < ncode; ++i) {
        uint32_t v;
        if (!in.Bits(3, v)) return false;
        lengths[kOrder[i]] = static_cast<uint8_t>(v);
    }
    Huffman lencode, distcode;
    if (!Build(lencode, lengths, 19)) return false;
    uint32_t index = 0;
    while (index < nlen + ndist) {
        int sym;
        if (!in.Decode(lencode, sym)) return false;
        if (sym < 16) {
            lengths[index++] = static_cast<uint8_t>(sym);
            continue;
        }
        uint8_t value = 0;
        uint32_t repeat;
        if (sym == 16) {
            if (index == 0 || !in.Bits(2, repeat)) return false;
            value = lengths[index - 1];
            repeat += 3;
        } else if (sym == 17) {
            if (!in.Bits(3, repeat)) return false;
            repeat += 3;
        } else {
            if (!in.Bits(7, repeat)) return false;
            repeat += 11;
        }
        if (index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;
    if (!Build(lencode, lengths, static_cast<int>(nlen)) || !Build(distcode, lengths + nlen, static_cast<int>(ndist)))
        return false;
    return Codes(in, lencode, distcode, out, start, max_out);
}

bool Stored(Reader& in, std::vector<uint8_t>& out, size_t start, size_t max_out) {
    in.AlignToByte();
    uint8_t b[4];
    for (uint8_t& x : b)
        if (!in.Byte(x)) return false;
    const unsigned len = b[0] | (b[1] << 8), nlen = b[2] | (b[3] << 8);
    if (len != (~nlen & 0xFFFFu) || out.size() - start + len > max_out) return false;
    for (unsigned i = 0; i < len; ++i) {
        uint8_t x;
        if (!in.Byte(x)) return false;
        out.push_back(x);
    }
    return true;
}

}  // namespace

// The cabinet's streams end on a sync flush rather than a final block: running out of input at a block boundary
// is the end.
bool InflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t max_out) {
    Reader in(data, size);
    const size_t start = out.size();
    for (;;) {
        uint32_t last, type;
        if (!in.Bits(1, last)) return true;
        if (!in.Bits(2, type)) return false;
        bool ok = false;
        if (type == 0) ok = Stored(in, out, start, max_out);
        else if (type == 1) ok = Fixed(in, out, start, max_out);
        else if (type == 2) ok = Dynamic(in, out, start, max_out);
        if (!ok) return false;
        if (last) return true;
    }
}

}  // namespace jpt
