#include "pe_resources.h"

#include <cstring>

namespace jpt {
namespace {

uint16_t U16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16) |
           (static_cast<uint32_t>(b[at + 3]) << 24);
}

struct Section {
    uint32_t rva, size, raw;
};

struct Directory {
    const std::vector<uint8_t>* image = nullptr;
    size_t base = 0;  // file offset of the resource directory
    size_t end = 0;

    // The entry with this numeric id: its offset field, false when absent. A named entry never matches.
    bool Find(size_t directory, uint32_t id, uint32_t& offset) const {
        if (directory + 16 > end) return false;
        const size_t named = U16(*image, directory + 12), ids = U16(*image, directory + 14);
        const size_t first = directory + 16 + named * 8;
        for (size_t i = 0; i < ids; ++i) {
            const size_t entry = first + i * 8;
            if (entry + 8 > end) return false;
            if (U32(*image, entry) == id) {
                offset = U32(*image, entry + 4);
                return true;
            }
        }
        return false;
    }
    bool FirstEntry(size_t directory, uint32_t& offset) const {
        if (directory + 16 > end) return false;
        const size_t count = static_cast<size_t>(U16(*image, directory + 12)) + U16(*image, directory + 14);
        if (count == 0 || directory + 16 + 8 > end) return false;
        offset = U32(*image, directory + 16 + 4);
        return true;
    }
};

}  // namespace

bool FindPeResource(const std::vector<uint8_t>& image, uint32_t type, uint32_t name, const uint8_t*& data, size_t& size) {
    if (image.size() < 0x100 || image[0] != 'M' || image[1] != 'Z') return false;
    const size_t pe = U32(image, 0x3C);
    if (pe + 24 > image.size() || std::memcmp(&image[pe], "PE\0\0", 4) != 0) return false;
    const size_t sections = U16(image, pe + 6), optional_size = U16(image, pe + 20), optional = pe + 24;
    if (optional + optional_size > image.size() || optional_size < 96 + 16) return false;
    const uint16_t magic = U16(image, optional);
    const size_t directories = optional + (magic == 0x20B ? 112 : 96);
    if (magic != 0x10B && magic != 0x20B) return false;
    if (directories + 16 > optional + optional_size) return false;
    const uint32_t resource_rva = U32(image, directories + 2 * 8);
    const uint32_t resource_size = U32(image, directories + 2 * 8 + 4);
    if (resource_rva == 0 || resource_size == 0) return false;

    std::vector<Section> table;
    const size_t headers = optional + optional_size;
    for (size_t i = 0; i < sections; ++i) {
        const size_t at = headers + i * 40;
        if (at + 40 > image.size()) return false;
        const uint32_t virtual_size = U32(image, at + 8), raw_size = U32(image, at + 16);
        table.push_back({U32(image, at + 12), virtual_size > raw_size ? virtual_size : raw_size, U32(image, at + 20)});
    }
    auto to_offset = [&](uint32_t rva, size_t& offset) {
        for (const Section& s : table) {
            if (rva >= s.rva && rva - s.rva < s.size) {
                offset = static_cast<size_t>(s.raw) + (rva - s.rva);
                return offset < image.size();
            }
        }
        return false;
    };

    size_t base = 0;
    if (!to_offset(resource_rva, base)) return false;
    Directory dir;
    dir.image = &image;
    dir.base = base;
    dir.end = base + resource_size < image.size() ? base + resource_size : image.size();

    uint32_t type_offset = 0, name_offset = 0, leaf = 0;
    if (!dir.Find(base, type, type_offset) || !(type_offset & 0x80000000u)) return false;
    if (!dir.Find(base + (type_offset & 0x7FFFFFFFu), name, name_offset) || !(name_offset & 0x80000000u)) return false;
    if (!dir.FirstEntry(base + (name_offset & 0x7FFFFFFFu), leaf) || (leaf & 0x80000000u)) return false;
    const size_t entry = base + leaf;
    if (entry + 16 > dir.end) return false;
    size_t offset = 0;
    const uint32_t length = U32(image, entry + 4);
    if (!to_offset(U32(image, entry), offset) || length > image.size() - offset) return false;
    data = image.data() + offset;
    size = length;
    return true;
}

}  // namespace jpt
