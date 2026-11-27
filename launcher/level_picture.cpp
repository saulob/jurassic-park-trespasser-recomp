#include "level_picture.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace jpt {
namespace {

constexpr int kWidth = 640, kHeight = 480;
constexpr int kMaxSide = 4096;
constexpr size_t kHeader = 18;

struct Picture {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels; // 0x00RRGGBB, from the top
};

uint32_t PixelAt(const unsigned char* p, int bytes) {
    if (bytes == 2) {
        const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8, r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
        return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
}

bool Decode(const std::string& source, Picture& picture) {
    const auto* data = reinterpret_cast<const unsigned char*>(source.data());
    if (source.size() < kHeader) return false;
    const int type = data[2], bits = data[16], bytes = bits / 8;
    const bool packed = type == 10, from_top = (data[17] & 0x20) != 0;
    picture.width = data[12] | data[13] << 8;
    picture.height = data[14] | data[15] << 8;
    if (data[1] || (type != 2 && !packed) || (bits != 16 && bits != 24 && bits != 32) || (data[17] & 0x10)) return false;
    if (picture.width < 1 || picture.height < 1 || picture.width > kMaxSide || picture.height > kMaxSide) return false;
    const size_t total = size_t(picture.width) * size_t(picture.height);
    size_t at = kHeader + data[0], done = 0;
    picture.pixels.resize(total);
    const auto put = [&](uint32_t pixel) {
        const size_t row = done / size_t(picture.width), column = done % size_t(picture.width);
        picture.pixels[(from_top ? row : size_t(picture.height) - 1 - row) * size_t(picture.width) + column] = pixel;
        ++done;
    };
    while (done < total) {
        size_t count = 1;
        bool repeat = false;
        if (packed) {
            if (at >= source.size()) return false;
            repeat = (data[at] & 0x80) != 0;
            count = size_t(data[at++] & 0x7F) + 1;
            if (count > total - done) return false;
        }
        const size_t stored = repeat ? 1 : count;
        if (source.size() < at || (source.size() - at) / size_t(bytes) < stored) return false;
        for (size_t i = 0; i < count; ++i) put(PixelAt(data + at + (repeat ? 0 : i * size_t(bytes)), bytes));
        at += stored * size_t(bytes);
    }
    return true;
}

// The mean of the source pixels a target pixel covers.
uint32_t Sample(const Picture& p, int x, int y, int width, int height) {
    const int x0 = x * p.width / width, x1 = std::max(x0 + 1, (x + 1) * p.width / width);
    const int y0 = y * p.height / height, y1 = std::max(y0 + 1, (y + 1) * p.height / height);
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx, ++n) {
            const uint32_t v = p.pixels[size_t(sy) * size_t(p.width) + size_t(sx)];
            r += v >> 16 & 255, g += v >> 8 & 255, b += v & 255;
        }
    return r / n << 16 | g / n << 8 | b / n;
}

} // namespace

bool IsLoadingPicture(const std::string& bytes) {
    Picture picture;
    return Decode(bytes, picture);
}

std::string MakeLoadingPicture(const std::string& bytes) {
    Picture picture;
    if (!Decode(bytes, picture)) return {};
    const double scale = std::min(double(kWidth) / picture.width, double(kHeight) / picture.height);
    const int width = std::clamp(int(picture.width * scale + 0.5), 1, kWidth), height = std::clamp(int(picture.height * scale + 0.5), 1, kHeight);
    const int left = (kWidth - width) / 2, top = (kHeight - height) / 2;
    static constexpr unsigned char kHead[kHeader] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, kWidth & 255, kWidth >> 8, kHeight & 255, kHeight >> 8, 16, 1};
    std::string out(kHeader + size_t(kWidth) * kHeight * 2, '\0');
    std::copy(std::begin(kHead), std::end(kHead), out.begin());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint32_t v = Sample(picture, x, y, width, height);
            const uint32_t pixel = (v >> 19 & 31) << 10 | (v >> 11 & 31) << 5 | (v >> 3 & 31);
            const size_t at = kHeader + (size_t(kHeight - 1 - top - y) * kWidth + size_t(left + x)) * 2;
            out[at] = char(pixel & 255);
            out[at + 1] = char(pixel >> 8);
        }
    return out;
}

} // namespace jpt
