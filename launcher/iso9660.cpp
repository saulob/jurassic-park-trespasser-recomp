#define WIN32_LEAN_AND_MEAN
#include "iso9660.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace jpt {
namespace {

constexpr uint32_t kSector = 2048;
constexpr int kMaxDepth = 16;
constexpr size_t kMaxFiles = 100000;
constexpr uint32_t kMaxDirectoryBytes = 16u << 20;

uint32_t Le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
    return s;
}

std::string Utf16BeToUtf8(const unsigned char* p, size_t bytes) {
    std::wstring wide;
    for (size_t i = 0; i + 1 < bytes; i += 2) wide.push_back(static_cast<wchar_t>((p[i] << 8) | p[i + 1]));
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool SafeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:*?\"<>|") == std::string::npos &&
           std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

bool SamePath(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

IsoImage::~IsoImage() {
    if (handle_) CloseHandle(handle_);
}

bool IsoImage::Read(uint64_t offset, void* out, size_t size) {
    if (offset > image_size_ || size > image_size_ - offset) return false;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD got = 0;
    return SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) &&
           ReadFile(handle_, out, static_cast<DWORD>(size), &got, nullptr) && got == size;
}

bool IsoImage::Open(const std::filesystem::path& image, std::string& error) {
    handle_ = CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        error = "the file cannot be opened";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) {
        error = "the file size cannot be read";
        return false;
    }
    image_size_ = static_cast<uint64_t>(size.QuadPart);
    unsigned char vd[kSector];
    unsigned char primary_root[34] = {}, joliet_root[34] = {};
    bool primary = false;
    for (uint32_t sector = 16; sector < 64; ++sector) {
        if (!Read(uint64_t(sector) * kSector, vd, sizeof vd) || std::memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 255) break;
        if (vd[0] == 1 && !primary) {
            primary = true;
            volume_id_ = Trim(std::string(reinterpret_cast<const char*>(vd + 40), 32));
            std::memcpy(primary_root, vd + 156, sizeof primary_root);
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            joliet_ = true;
            std::memcpy(joliet_root, vd + 156, sizeof joliet_root);
        }
    }
    if (!primary) {
        error = "not an ISO 9660 disc image";
        return false;
    }
    const unsigned char* root = joliet_ ? joliet_root : primary_root;
    return Walk(Le32(root + 2), Le32(root + 10), "", 0, error);
}

bool IsoImage::Walk(uint32_t extent, uint32_t size, const std::string& parent, int depth, std::string& error) {
    if (depth > kMaxDepth || size > kMaxDirectoryBytes ||
        std::find(visited_.begin(), visited_.end(), extent) != visited_.end()) {
        error = "the directory tree is damaged";
        return false;
    }
    visited_.push_back(extent);
    std::vector<unsigned char> block(size);
    if (!Read(uint64_t(extent) * kSector, block.data(), block.size())) {
        error = "a directory lies outside the image (truncated file?)";
        return false;
    }
    size_t pos = 0;
    while (pos < block.size()) {
        const unsigned length = block[pos];
        if (length == 0) {
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        if (length < 34 || pos + length > block.size()) {
            error = "a directory record is damaged";
            return false;
        }
        const unsigned char* rec = &block[pos];
        pos += length;
        const unsigned name_len = rec[32];
        if (33u + name_len > length) {
            error = "a directory record is damaged";
            return false;
        }
        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) continue;
        std::string name = joliet_ ? Utf16BeToUtf8(rec + 33, name_len)
                                   : std::string(reinterpret_cast<const char*>(rec + 33), name_len);
        if (const size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
        if (!joliet_ && !name.empty() && name.back() == '.') name.pop_back();
        if (!SafeName(name)) {
            error = "the image holds a file name that cannot be installed";
            return false;
        }
        const uint32_t child_extent = Le32(rec + 2), child_size = Le32(rec + 10);
        const std::string path = parent.empty() ? name : parent + "/" + name;
        if (rec[25] & 2) {
            if (!Walk(child_extent, child_size, path, depth + 1, error)) return false;
        } else {
            if (files_.size() == kMaxFiles) {
                error = "the image lists too many files";
                return false;
            }
            files_.push_back({path, child_extent, child_size});
        }
    }
    return true;
}

const IsoFile* IsoImage::Find(const std::string& path) const {
    for (const IsoFile& f : files_)
        if (SamePath(f.path, path)) return &f;
    return nullptr;
}

bool IsoImage::Extract(const IsoFile& file, const std::filesystem::path& to, const Progress& progress,
                       std::string& error) {
    const uint64_t start = uint64_t(file.extent) * kSector;
    if (start > image_size_ || file.size > image_size_ - start) {
        error = file.path + " lies outside the image (truncated file?)";
        return false;
    }
    std::filesystem::path part = to;
    part += L".part";
    HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = "cannot create " + file.path + " in the installation folder";
        return false;
    }
    std::vector<unsigned char> buffer(1 << 20);
    uint64_t done = 0;
    bool ok = true;
    while (ok && done < file.size) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file.size - done));
        DWORD written = 0;
        if (!Read(start + done, buffer.data(), n)) {
            error = "cannot read " + file.path + " from the image";
            ok = false;
        } else if (!WriteFile(out, buffer.data(), static_cast<DWORD>(n), &written, nullptr) || written != n) {
            error = "cannot write " + file.path + " (disk full?)";
            ok = false;
        } else {
            done += n;
            if (progress && !progress(n)) {
                error = "cancelled";
                ok = false;
            }
        }
    }
    CloseHandle(out);
    if (ok && !MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "cannot finish " + file.path;
        ok = false;
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

}  // namespace jpt
