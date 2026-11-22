// The few file operations of the installer that differ between Win32 and POSIX: random-access reads of a disc image and
// a write that only replaces its target when it is complete.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace jpt {

class FileReader {
public:
    FileReader() = default;
    ~FileReader() { Close(); }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool Open(const std::filesystem::path& file);
    void Close();
    bool Size(uint64_t& bytes) const;
    bool ReadAt(uint64_t offset, void* out, size_t size);
    bool IsOpen() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

// Writes <target>.part and moves it over the target on Commit. Dropping the writer without a Commit deletes the part.
class PartFileWriter {
public:
    PartFileWriter() = default;
    ~PartFileWriter() { Abort(); }
    PartFileWriter(const PartFileWriter&) = delete;
    PartFileWriter& operator=(const PartFileWriter&) = delete;

    bool Open(const std::filesystem::path& target);
    bool Write(const void* data, size_t size);
    bool Commit();
    void Abort();

private:
    void* handle_ = nullptr;
    std::filesystem::path target_;
    std::filesystem::path part_;
};

}  // namespace jpt
