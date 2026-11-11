// Read-only ISO 9660 reader for the player's own disc image: the Joliet tree when there is one (it keeps the long
// names the game asks for), else the primary one. No mounting, no external tools.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jpt {

struct IsoFile {
    std::string path;  // '/' separated, as on the disc
    uint32_t extent = 0;
    uint64_t size = 0;
};

class IsoImage {
public:
    IsoImage() = default;
    ~IsoImage();
    IsoImage(const IsoImage&) = delete;
    IsoImage& operator=(const IsoImage&) = delete;

    // Reads the volume descriptors and the whole directory tree. False with a reason when it is not a usable image.
    bool Open(const std::filesystem::path& image, std::string& error);

    const std::string& volume_id() const { return volume_id_; }
    bool joliet() const { return joliet_; }
    const std::vector<IsoFile>& files() const { return files_; }
    // Case-insensitive lookup; nullptr when absent.
    const IsoFile* Find(const std::string& path) const;

    // Called with the bytes just copied; returning false cancels.
    using Progress = std::function<bool(uint64_t bytes)>;
    // Copies one file through `<to>.part`. False with a reason; "cancelled" when Progress said so.
    bool Extract(const IsoFile& file, const std::filesystem::path& to, const Progress& progress, std::string& error);

private:
    bool Read(uint64_t offset, void* out, size_t size);
    bool Walk(uint32_t extent, uint32_t size, const std::string& parent, int depth, std::string& error);

    void* handle_ = nullptr;
    uint64_t image_size_ = 0;
    std::string volume_id_;
    bool joliet_ = false;
    std::vector<IsoFile> files_;
    std::vector<uint32_t> visited_;
};

}  // namespace jpt
