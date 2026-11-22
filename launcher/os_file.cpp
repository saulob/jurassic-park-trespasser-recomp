#include "os_file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdio>
#include <sys/types.h>
#endif

namespace jpt {

#ifdef _WIN32

bool FileReader::Open(const std::filesystem::path& file) {
    Close();
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
    return true;
}

void FileReader::Close() {
    if (handle_) CloseHandle(handle_);
    handle_ = nullptr;
}

bool FileReader::Size(uint64_t& bytes) const {
    LARGE_INTEGER size;
    if (!handle_ || !GetFileSizeEx(handle_, &size)) return false;
    bytes = static_cast<uint64_t>(size.QuadPart);
    return true;
}

bool FileReader::ReadAt(uint64_t offset, void* out, size_t size) {
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD got = 0;
    return handle_ && SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) &&
           ReadFile(handle_, out, static_cast<DWORD>(size), &got, nullptr) && got == size;
}

bool PartFileWriter::Open(const std::filesystem::path& target) {
    Abort();
    target_ = target;
    part_ = target;
    part_ += ".part";
    HANDLE h = CreateFileW(part_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
    return true;
}

bool PartFileWriter::Write(const void* data, size_t size) {
    DWORD written = 0;
    return handle_ && (size == 0 || (WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr) && written == size));
}

bool PartFileWriter::Commit() {
    if (!handle_) return false;
    CloseHandle(handle_);
    handle_ = nullptr;
    if (MoveFileExW(part_.c_str(), target_.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(part_.c_str());
    return false;
}

void PartFileWriter::Abort() {
    if (!handle_) return;
    CloseHandle(handle_);
    handle_ = nullptr;
    DeleteFileW(part_.c_str());
}

#else

bool FileReader::Open(const std::filesystem::path& file) {
    Close();
    handle_ = std::fopen(file.c_str(), "rb");
    return handle_ != nullptr;
}

void FileReader::Close() {
    if (handle_) std::fclose(static_cast<FILE*>(handle_));
    handle_ = nullptr;
}

bool FileReader::Size(uint64_t& bytes) const {
    FILE* f = static_cast<FILE*>(handle_);
    if (!f || fseeko(f, 0, SEEK_END) != 0) return false;
    const off_t end = ftello(f);
    if (end < 0) return false;
    bytes = static_cast<uint64_t>(end);
    return true;
}

bool FileReader::ReadAt(uint64_t offset, void* out, size_t size) {
    FILE* f = static_cast<FILE*>(handle_);
    return f && fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
}

bool PartFileWriter::Open(const std::filesystem::path& target) {
    Abort();
    target_ = target;
    part_ = target;
    part_ += ".part";
    handle_ = std::fopen(part_.c_str(), "wb");
    return handle_ != nullptr;
}

bool PartFileWriter::Write(const void* data, size_t size) {
    return handle_ && (size == 0 || std::fwrite(data, 1, size, static_cast<FILE*>(handle_)) == size);
}

bool PartFileWriter::Commit() {
    if (!handle_) return false;
    const bool flushed = std::fflush(static_cast<FILE*>(handle_)) == 0;
    const bool closed = std::fclose(static_cast<FILE*>(handle_)) == 0;
    handle_ = nullptr;
    std::error_code ec;
    if (flushed && closed) std::filesystem::rename(part_, target_, ec);
    if (flushed && closed && !ec) return true;
    std::filesystem::remove(part_, ec);
    return false;
}

void PartFileWriter::Abort() {
    if (!handle_) return;
    std::fclose(static_cast<FILE*>(handle_));
    handle_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(part_, ec);
}

#endif

}  // namespace jpt
