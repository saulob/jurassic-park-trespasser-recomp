#include "rar_stream.h"

#include <algorithm>
#include <cstdlib>
#include <new>

namespace jpt::rar {
namespace {

constexpr size_t kMostFilters = 8192;
constexpr uint32_t kFilterMemory = 0x40000;
constexpr uint32_t kMostChannels = 1024;

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void Store32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// x86 calls and jumps, back from absolute to relative.
void UndoCalls(std::vector<uint8_t>& data, uint64_t file_offset, bool jumps, bool wrap) {
    constexpr uint32_t kSpace = 1u << 24;
    for (size_t at = 0; at + 4 < data.size();) {
        const uint8_t code = data[at++];
        if (code != 0xE8 && !(jumps && code == 0xE9)) continue;
        const uint32_t here = uint32_t(at + file_offset) & (wrap ? kSpace - 1 : 0xFFFFFFFFu), address = Load32(&data[at]);
        if (address >= 0x80000000u) {
            if (((address + here) & 0x80000000u) == 0) Store32(&data[at], address + kSpace);
        } else if (address < kSpace) {
            Store32(&data[at], address - here);
        }
        at += 4;
    }
}

void UndoItanium(std::vector<uint8_t>& data, uint64_t file_offset) {
    static constexpr uint8_t kSlots[16] = {4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
    uint32_t bundle = uint32_t(file_offset >> 4);
    for (size_t at = 0; at + 21 < data.size(); at += 16, ++bundle) {
        const unsigned format = data[at] & 0x1F;
        if (format < 16) continue;
        for (unsigned slot = 0; slot < 3; ++slot) {
            if (!(kSlots[format - 16] >> slot & 1)) continue;
            const unsigned start = slot * 41 + 5;
            const auto field = [&](unsigned bit) { return Load32(&data[at + bit / 8]) >> (bit % 8); };
            if ((field(start + 37) & 15) != 5) continue;
            const unsigned bit = start + 13, shift = bit % 8;
            const uint32_t mask = 0xFFFFFu << shift, word = Load32(&data[at + bit / 8]);
            Store32(&data[at + bit / 8], (word & ~mask) | (((word >> shift) - bundle) << shift & mask));
        }
    }
}

void UndoArm(std::vector<uint8_t>& data, uint64_t file_offset) {
    for (size_t at = 0; at + 3 < data.size(); at += 4) {
        if (data[at + 3] != 0xEB) continue;
        const uint32_t offset = (Load32(&data[at]) & 0xFFFFFF) - uint32_t((file_offset + at) / 4);
        for (int i = 0; i < 3; ++i) data[at + size_t(i)] = uint8_t(offset >> (8 * i));
    }
}

void UndoDelta(std::vector<uint8_t>& data, size_t channels) {
    std::vector<uint8_t> out(data.size());
    size_t from = 0;
    for (size_t channel = 0; channel < channels; ++channel) {
        uint8_t last = 0;
        for (size_t to = channel; to < out.size(); to += channels) out[to] = last = uint8_t(last - data[from++]);
    }
    data.swap(out);
}

// Rows of three-byte pixels, each byte predicted from its left, upper and upper left neighbour.
void UndoRgb(std::vector<uint8_t>& data, size_t stride, size_t red) {
    std::vector<uint8_t> out(data.size());
    size_t from = 0;
    for (size_t channel = 0; channel < 3; ++channel) {
        uint8_t last = 0;
        for (size_t to = channel; to < out.size(); to += 3) {
            int guess = last;
            if (to >= stride + 3) {
                const int up = out[to - stride], corner = out[to - stride - 3], sum = last + up - corner;
                const int from_left = std::abs(sum - last), from_up = std::abs(sum - up), from_corner = std::abs(sum - corner);
                guess = from_left <= from_up && from_left <= from_corner ? last : from_up <= from_corner ? up : corner;
            }
            out[to] = last = uint8_t(guess - data[from++]);
        }
    }
    for (size_t at = red; at + 2 < out.size(); at += 3) {
        out[at] = uint8_t(out[at] + out[at + 1]);
        out[at + 2] = uint8_t(out[at + 2] + out[at + 1]);
    }
    data.swap(out);
}

void UndoAudio(std::vector<uint8_t>& data, size_t channels) {
    std::vector<uint8_t> out(data.size());
    size_t from = 0;
    for (size_t channel = 0; channel < channels; ++channel) {
        uint32_t last = 0, errors[7] = {};
        int delta = 0, older[3] = {}, weights[3] = {};
        for (size_t to = channel, count = 0; to < out.size(); to += channels, ++count) {
            older[2] = older[1];
            older[1] = delta - older[0];
            older[0] = delta;
            const uint32_t sum = 8 * last + uint32_t(weights[0] * older[0] + weights[1] * older[1] + weights[2] * older[2]);
            const uint8_t residue = data[from++];
            const uint8_t value = uint8_t((sum >> 3) - residue);
            out[to] = value;
            delta = int8_t(uint8_t(value - last));
            last = value;
            const int scaled = int(int8_t(residue)) * 8;
            errors[0] += uint32_t(std::abs(scaled));
            for (int i = 0; i < 3; ++i) {
                errors[1 + 2 * i] += uint32_t(std::abs(scaled - older[i]));
                errors[2 + 2 * i] += uint32_t(std::abs(scaled + older[i]));
            }
            if (count % 32) continue;
            const size_t best = size_t(std::min_element(std::begin(errors), std::end(errors)) - errors);
            std::fill(std::begin(errors), std::end(errors), 0u);
            if (!best) continue;
            int& weight = weights[(best - 1) / 2];
            if (best % 2 ? weight >= -16 : weight < 16) weight += best % 2 ? -1 : 1;
        }
    }
    data.swap(out);
}

} // namespace

bool ApplyFilter(const Filter& filter, std::vector<uint8_t>& data, uint64_t file_offset) {
    switch (filter.kind) {
    case FilterKind::kE8:
    case FilterKind::kE8E9:
        if (data.size() >= 4) UndoCalls(data, file_offset, filter.kind == FilterKind::kE8E9, filter.wrap);
        return true;
    case FilterKind::kItanium: UndoItanium(data, file_offset); return data.size() <= kFilterMemory;
    case FilterKind::kArm: UndoArm(data, file_offset); return true;
    case FilterKind::kDelta:
        if (!filter.channels || filter.channels > kMostChannels) return false;
        UndoDelta(data, filter.channels);
        return true;
    case FilterKind::kRgb:
        if (filter.channels < 3 || filter.position > 2 || data.size() < 3 || data.size() > kFilterMemory / 2 || filter.channels - 3 > data.size()) return false;
        UndoRgb(data, filter.channels - 3, filter.position);
        return true;
    case FilterKind::kAudio:
        if (!filter.channels || filter.channels > 128 || data.size() > kFilterMemory / 2) return false;
        UndoAudio(data, filter.channels);
        return true;
    case FilterKind::kUnknown: break;
    }
    return false;
}

BitSource::BitSource(FileReader& file, uint64_t offset, uint64_t size) : file_(file), next_(offset), left_(size), buffer_(kChunk + 2 * kReach) {}

void BitSource::Fill() {
    if (!left_ || failed_) return;
    const size_t kept = filled_ - at_;
    std::memmove(buffer_.data(), buffer_.data() + at_, kept);
    base_ += at_;
    at_ = 0;
    const size_t count = size_t(std::min<uint64_t>(left_, kChunk - kept));
    if (!file_.ReadAt(next_, buffer_.data() + kept, count)) {
        failed_ = true;
        left_ = 0;
        filled_ = kept;
    } else {
        next_ += count;
        left_ -= count;
        filled_ = kept + count;
    }
    std::fill(buffer_.begin() + ptrdiff_t(filled_), buffer_.end(), uint8_t(0));
}

bool PrefixCode::Build(const uint8_t* lengths, size_t count) {
    std::array<uint32_t, kBits + 2> counts{};
    for (size_t i = 0; i < count; ++i) ++counts[lengths[i]];
    counts[0] = 0;
    uint32_t code = 0, index = 0;
    limit_[0] = 0;
    for (unsigned length = 1; length <= kBits; ++length) {
        first_[length] = index;
        index += counts[length];
        code += counts[length] << (16 - length);
        if (code > 0x10000) return false;
        limit_[length] = code;
    }
    std::array<uint32_t, kBits + 2> next = first_;
    for (size_t i = 0; i < count; ++i)
        if (lengths[i]) symbols_[next[lengths[i]]++] = uint16_t(i);
    quick_.fill(0);
    for (unsigned length = 1; length <= kQuick; ++length) {
        const uint32_t step = 1u << (kQuick - length);
        uint32_t slot = limit_[length - 1] >> (16 - kQuick);
        for (uint32_t i = 0; i < counts[length]; ++i)
            for (uint32_t k = 0; k < step; ++k) quick_[slot++] = uint16_t(symbols_[first_[length] + i] << 4 | length);
    }
    return true;
}

int PrefixCode::DecodeLong(BitSource& in, uint32_t bits) const {
    for (unsigned length = kQuick + 1; length <= kBits; ++length) {
        if (bits >= limit_[length]) continue;
        in.Skip(length);
        return symbols_[first_[length] + ((bits - limit_[length - 1]) >> (16 - length))];
    }
    return -1;
}

bool Output::Add(const Filter& filter) {
    if (!filter.length) return true;
    if (filters_.size() >= kMostFilters) return false;
    filters_.push_back(filter);
    return true;
}

void Output::Send(const uint8_t* data, size_t count) {
    count = size_t(std::min<uint64_t>(count, size_ - sent_));
    if (!count) return;
    sent_ += count;
    if (!sink_(data, count)) state_ = OutputState::kStopped;
}

void Output::Write(const uint8_t* data, size_t count) {
    while (count && state_ == OutputState::kOk) {
        size_t take = count;
        if (filters_.empty()) {
            Send(data, take);
        } else if (decoded_ < filters_.front().start) {
            take = size_t(std::min<uint64_t>(take, filters_.front().start - decoded_));
            Send(data, take);
        } else {
            const Filter filter = filters_.front();
            if (block_.empty() && decoded_ != filter.start) return void(state_ = OutputState::kDamaged);
            take = std::min<size_t>(take, filter.length - block_.size());
            block_.insert(block_.end(), data, data + take);
            if (block_.size() == filter.length) {
                while (!filters_.empty() && filters_.front().start == filter.start && filters_.front().length == filter.length) {
                    if (!ApplyFilter(filters_.front(), block_, filter.start)) return void(state_ = OutputState::kDamaged);
                    filters_.pop_front();
                }
                Send(block_.data(), block_.size());
                block_.clear();
            }
        }
        decoded_ += take;
        data += take;
        count -= take;
    }
}

bool Window::Allocate(size_t size) {
    if (size_ != size) data_.reset(new (std::nothrow) uint8_t[size]);
    size_ = data_ ? size : 0;
    Reset();
    return size_ != 0;
}

void Window::Copy(uint64_t distance, size_t length) {
    if (!distance || distance > size_ || (distance > at_ && !whole_)) {
        while (length--) Put(0);
        return;
    }
    size_t from = at_ >= distance ? at_ - size_t(distance) : at_ + size_ - size_t(distance);
    if (from + length <= size_ && at_ + length < size_) {
        uint8_t* to = &data_[at_];
        const uint8_t* source = &data_[from];
        if (distance >= length) std::memcpy(to, source, length);
        else
            for (size_t i = 0; i < length; ++i) to[i] = source[i];
        at_ += length;
        pending_ += length;
        return;
    }
    while (length--) {
        Put(data_[from]);
        if (++from == size_) from = 0;
    }
}

void Window::Flush(Output& out) {
    if (!pending_) return;
    const size_t from = at_ >= pending_ ? at_ - pending_ : at_ + size_ - pending_;
    const size_t tail = std::min(pending_, size_ - from);
    out.Write(&data_[from], tail);
    out.Write(data_.get(), pending_ - tail);
    pending_ = 0;
}

} // namespace jpt::rar
