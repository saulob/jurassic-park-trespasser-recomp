#include "is_cabinet.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "inflate.h"

namespace jpt {
namespace {

constexpr uint32_t kSignature = 0x28635349;  // "ISc("
constexpr uint16_t kCompressed = 4;
constexpr size_t kDescriptorSize = 0x2A;
constexpr uint32_t kMaxEntries = 65536;

bool EqualNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

bool Cabinet::Open(std::vector<uint8_t> bytes, std::string& error) {
    bytes_ = std::move(bytes);
    files_.clear();
    const size_t n = bytes_.size();
    auto u32 = [&](size_t at, uint32_t& v) {
        if (at > n || n - at < 4) return false;
        std::memcpy(&v, bytes_.data() + at, 4);
        return true;
    };
    auto u16 = [&](size_t at, uint16_t& v) {
        if (at > n || n - at < 2) return false;
        std::memcpy(&v, bytes_.data() + at, 2);
        return true;
    };
    auto name = [&](size_t at, std::string& s) {
        if (at >= n) return false;
        const void* end = std::memchr(bytes_.data() + at, 0, std::min<size_t>(n - at, 260));
        if (!end) return false;
        s.assign(reinterpret_cast<const char*>(bytes_.data() + at), static_cast<const uint8_t*>(end) - bytes_.data() - at);
        return s.find("..") == std::string::npos && s.find(':') == std::string::npos;
    };
    uint32_t sig = 0, version = 0, desc = 0, table_offset = 0, dirs = 0, count = 0;
    if (!u32(0, sig) || sig != kSignature || !u32(4, version) || (version >> 24) != 1 || (version & 0xFFFF) != 4) {
        error = "not an InstallShield 5 cabinet";
        return false;
    }
    if (!u32(12, desc) || !u32(desc + 0x0C, table_offset) || !u32(desc + 0x1C, dirs) || !u32(desc + 0x28, count) ||
        dirs > kMaxEntries || count > kMaxEntries) {
        error = "the cabinet's file table is damaged";
        return false;
    }
    const size_t base = size_t(desc) + table_offset;
    std::vector<std::string> folders(dirs);
    for (uint32_t i = 0; i < dirs; ++i) {
        uint32_t at;
        if (!u32(base + 4ull * i, at) || !name(base + at, folders[i])) {
            error = "the cabinet's folder names are damaged";
            return false;
        }
        std::replace(folders[i].begin(), folders[i].end(), '\\', '/');
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at, name_at, folder, size, packed, offset;
        uint16_t flags;
        std::string file;
        if (!u32(base + 4ull * (dirs + i), at) || base + at + kDescriptorSize > n || !u32(base + at, name_at) ||
            !u32(base + at + 4, folder) || !u16(base + at + 8, flags) || !u32(base + at + 10, size) ||
            !u32(base + at + 14, packed) || !u32(base + at + 0x26, offset) || folder >= dirs ||
            !name(base + name_at, file) || file.empty() || file.find_first_of("/\\") != std::string::npos ||
            offset > n || n - offset < packed) {
            error = "the cabinet's file table is damaged";
            return false;
        }
        CabinetFile f;
        f.path = folders[folder].empty() ? file : folders[folder] + "/" + file;
        f.size = size;
        f.packed = packed;
        f.offset = offset;
        f.compressed = (flags & kCompressed) != 0;
        files_.push_back(std::move(f));
    }
    return true;
}

const CabinetFile* Cabinet::Find(const std::string& path) const {
    for (const CabinetFile& f : files_)
        if (EqualNoCase(f.path, path)) return &f;
    return nullptr;
}

bool Cabinet::Read(const CabinetFile& file, std::vector<uint8_t>& out, std::string& error) const {
    out.clear();
    const uint8_t* data = bytes_.data() + file.offset;
    if (!file.compressed) {
        out.assign(data, data + file.packed);
    } else {
        out.reserve(static_cast<size_t>(file.size));
        if (!InflateRaw(data, static_cast<size_t>(file.packed), out, static_cast<size_t>(file.size))) {
            error = file.path + " in the cabinet is damaged";
            return false;
        }
    }
    if (out.size() != file.size) {
        error = file.path + " in the cabinet has " + std::to_string(out.size()) + " bytes, expected " +
                std::to_string(file.size);
        return false;
    }
    return true;
}

}  // namespace jpt
