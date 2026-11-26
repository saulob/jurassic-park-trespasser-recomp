// Read-only access to the files of a ZIP, 7z or RAR archive, or of a folder, for the custom level import. Nothing here runs
// anything or writes outside its own temporary folder; names are reported as the source spells them and must be checked before use.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpt {

struct ArchiveLimits {
    uint64_t max_archive_bytes = 1ull << 30;
    uint64_t max_file_bytes = 512ull << 20;
    uint64_t max_total_bytes = 2ull << 30;
    size_t max_entries = 8192;
    // A 7z block up to max_memory_bytes is expanded in memory as a whole. A larger one is decoded into temporary files, or, when
    // its coders cannot be decoded as a stream, in memory up to max_block_bytes.
    uint64_t max_memory_bytes = 128ull << 20;
    uint64_t max_block_bytes = 768ull << 20;
};

enum class ArchiveError { kNone, kUnreadable, kUnsupported, kDamaged, kTooLarge, kCancelled, kOutOfMemory };

struct ArchiveEntry {
    std::string name; // UTF-8
    uint64_t size = 0;
    bool directory = false;
    bool link = false;
    bool encrypted = false;
};

class ArchiveReader {
public:
    // False stops the extraction.
    using Sink = std::function<bool(const uint8_t* data, size_t size)>;

    virtual ~ArchiveReader() = default;
    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    virtual ArchiveError Extract(size_t index, const Sink& sink) = 0;

protected:
    std::vector<ArchiveEntry> entries_;
};

// The format comes from the file's contents, never from its name.
bool IsArchiveKnown(const std::filesystem::path& archive);
std::unique_ptr<ArchiveReader> OpenArchive(const std::filesystem::path& archive, const ArchiveLimits& limits,
                                           ArchiveError& error);
std::unique_ptr<ArchiveReader> OpenFolder(const std::filesystem::path& folder, const ArchiveLimits& limits,
                                          ArchiveError& error);

bool IsUtf8(const std::string& text);
// Windows-1252, the code page of the period's English tools, as UTF-8.
std::string FromWindows1252(const std::string& text);
std::string FromUtf16(const uint16_t* text, size_t length);
std::string ForwardSlashes(std::string name);

} // namespace jpt
