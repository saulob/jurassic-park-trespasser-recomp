#include "archive_reader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>

#include "archive_spool.h"
#include "os_file.h"
#include "rar_reader.h"
#include "seven_zip_block.h"
#include "third_party/lzma/7z.h"
#include "third_party/lzma/7zCrc.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_zip.h"
#include "utf8.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace jpt {
namespace {

void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += char(code);
    } else if (code < 0x800) {
        out += char(0xC0 | (code >> 6));
        out += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += char(0xE0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    } else {
        out += char(0xF0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3F));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
}

// ---- ZIP

class ZipReader final : public ArchiveReader {
public:
    ~ZipReader() override {
        if (open_) mz_zip_reader_end(&zip_);
    }

    ArchiveError Open(const fs::path& archive, const ArchiveLimits& limits) {
        uint64_t size = 0;
        if (!file_.Open(archive) || !file_.Size(size)) return ArchiveError::kUnreadable;
        zip_.m_pRead = &ZipReader::Read;
        zip_.m_pIO_opaque = this;
        open_ = mz_zip_reader_init(&zip_, size, 0) != MZ_FALSE;
        if (!open_) return ArchiveError::kDamaged;
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        if (count > limits.max_entries) return ArchiveError::kTooLarge;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            const mz_uint length = mz_zip_reader_get_filename(&zip_, i, nullptr, 0);
            if (mz_zip_reader_file_stat(&zip_, i, &stat) == MZ_FALSE || length <= 1) return ArchiveError::kDamaged;
            std::vector<char> raw(length);
            mz_zip_reader_get_filename(&zip_, i, raw.data(), length);
            const std::string name(raw.data(), length - 1);
            ArchiveEntry entry;
            entry.name = ForwardSlashes(IsUtf8(name) ? name : FromWindows1252(name));
            entry.size = stat.m_uncomp_size;
            entry.directory = stat.m_is_directory || entry.name.back() == '/';
            entry.link = trg::update::IsSymlinkZipEntry(stat.m_external_attr);
            entry.encrypted = stat.m_is_encrypted != MZ_FALSE || (!entry.directory && !stat.m_is_supported);
            entries_.push_back(std::move(entry));
        }
        return ArchiveError::kNone;
    }

    ArchiveError Extract(size_t index, const Sink& sink) override {
        sink_ = &sink;
        stopped_ = false;
        const bool ok = mz_zip_reader_extract_to_callback(&zip_, mz_uint(index), &ZipReader::Write, this, 0) != MZ_FALSE;
        sink_ = nullptr;
        return stopped_ ? ArchiveError::kCancelled : ok ? ArchiveError::kNone : ArchiveError::kDamaged;
    }

private:
    static size_t Read(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
        return static_cast<ZipReader*>(opaque)->file_.ReadAt(offset, buffer, size) ? size : 0;
    }

    static size_t Write(void* opaque, mz_uint64, const void* data, size_t size) {
        ZipReader* self = static_cast<ZipReader*>(opaque);
        if (self->stopped_ || !(*self->sink_)(static_cast<const uint8_t*>(data), size)) {
            self->stopped_ = true;
            return 0;
        }
        return size;
    }

    FileReader file_;
    mz_zip_archive zip_{};
    bool open_ = false;
    const Sink* sink_ = nullptr;
    bool stopped_ = false;
};

// ---- 7z

void* SzAlloc(ISzAllocPtr, size_t size) { return size ? std::malloc(size) : nullptr; }
void SzFree(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kAlloc = {SzAlloc, SzFree};

ArchiveError FromSz(SRes result) {
    switch (result) {
    case SZ_OK: return ArchiveError::kNone;
    case SZ_ERROR_MEM: return ArchiveError::kOutOfMemory;
    case SZ_ERROR_UNSUPPORTED: return ArchiveError::kUnsupported;
    default: return ArchiveError::kDamaged;
    }
}

struct SevenZipStream {
    ISeekInStream vt;
    FileReader* file;
    uint64_t position;
    uint64_t size;
};

SRes SevenZipRead(ISeekInStreamPtr stream, void* buffer, size_t* size) {
    SevenZipStream* s = Z7_container_of(stream, SevenZipStream, vt);
    *size = size_t(std::min<uint64_t>(*size, s->size - s->position));
    if (*size && !s->file->ReadAt(s->position, buffer, *size)) return SZ_ERROR_READ;
    s->position += *size;
    return SZ_OK;
}

SRes SevenZipSeek(ISeekInStreamPtr stream, Int64* position, ESzSeek origin) {
    SevenZipStream* s = Z7_container_of(stream, SevenZipStream, vt);
    const Int64 base = origin == SZ_SEEK_CUR ? Int64(s->position) : origin == SZ_SEEK_END ? Int64(s->size) : 0;
    const Int64 to = base + *position;
    if (to < 0 || uint64_t(to) > s->size) return SZ_ERROR_INPUT_EOF;
    s->position = uint64_t(to);
    *position = to;
    return SZ_OK;
}

class SevenZipReader final : public ArchiveReader {
public:
    SevenZipReader() { SzArEx_Init(&db_); }

    ~SevenZipReader() override {
        ISzAlloc_Free(&kAlloc, out_);
        SzArEx_Free(&db_, &kAlloc);
        ISzAlloc_Free(&kAlloc, look_.buf);
    }

    ArchiveError Open(const fs::path& archive, const ArchiveLimits& limits) {
        static const bool crc_ready = (CrcGenerateTable(), true);
        constexpr size_t kBuffer = 1 << 18;
        (void)crc_ready;
        limits_ = limits;
        stream_ = {{SevenZipRead, SevenZipSeek}, &file_, 0, 0};
        if (!file_.Open(archive) || !file_.Size(stream_.size)) return ArchiveError::kUnreadable;
        LookToRead2_CreateVTable(&look_, False);
        look_.buf = static_cast<Byte*>(ISzAlloc_Alloc(&kAlloc, kBuffer));
        if (!look_.buf) return ArchiveError::kOutOfMemory;
        look_.bufSize = kBuffer;
        look_.realStream = &stream_.vt;
        LookToRead2_INIT(&look_)
        const SRes opened = SzArEx_Open(&db_, &look_.vt, &kAlloc, &kAlloc);
        if (opened != SZ_OK) return FromSz(opened);
        if (db_.NumFiles > limits.max_entries) return ArchiveError::kTooLarge;
        for (UInt32 i = 0; i < db_.NumFiles; ++i) {
            const size_t length = SzArEx_GetFileNameUtf16(&db_, i, nullptr);
            if (length <= 1 || length > 4096) return ArchiveError::kDamaged;
            std::vector<UInt16> raw(length);
            SzArEx_GetFileNameUtf16(&db_, i, raw.data());
            ArchiveEntry entry;
            entry.name = ForwardSlashes(FromUtf16(raw.data(), length - 1));
            entry.size = SzArEx_GetFileSize(&db_, i);
            entry.directory = SzArEx_IsDir(&db_, i) != 0;
            if (SzBitWithVals_Check(&db_.Attribs, i)) {
                constexpr UInt32 kReparsePoint = 0x400, kUnixMode = 0x8000;
                const UInt32 attributes = db_.Attribs.Vals[i];
                entry.link = (attributes & kReparsePoint) != 0 ||
                             ((attributes & kUnixMode) != 0 && ((attributes >> 16) & 0170000u) == 0120000u);
            }
            entries_.push_back(std::move(entry));
        }
        return ArchiveError::kNone;
    }

    ArchiveError Extract(size_t index, const Sink& sink) override {
        const UInt32 block = db_.FileToFolder[index];
        const uint64_t expanded = block == UInt32(-1) ? 0 : SzAr_GetFolderUnpackSize(&db_.db, block);
        if (expanded > limits_.max_memory_bytes && IsStreamableBlock(db_, block)) return FromSpool(block, index, sink);
        if (expanded > limits_.max_block_bytes) return ArchiveError::kTooLarge;
        size_t offset = 0, size = 0;
        const SRes result = SzArEx_Extract(&db_, &look_.vt, UInt32(index), &block_, &out_, &out_size_, &offset, &size,
                                           &kAlloc, &kAlloc);
        if (result != SZ_OK) return FromSz(result);
        constexpr size_t kChunk = 1 << 20;
        for (size_t done = 0; done < size; done += kChunk)
            if (!sink(out_ + offset + done, std::min(kChunk, size - done))) return ArchiveError::kCancelled;
        return ArchiveError::kNone;
    }

private:
    // The whole block is decoded for the first of its files.
    ArchiveError FromSpool(UInt32 block, size_t index, const Sink& sink) {
        if (!spool_.created() && !spool_.Create()) return ArchiveError::kUnreadable;
        const auto [at, first] = spooled_.try_emplace(block, ArchiveError::kNone);
        if (first) at->second = Spool(block);
        return at->second == ArchiveError::kNone ? spool_.Read(index, entries_[index].size, sink) : at->second;
    }

    ArchiveError Spool(UInt32 block) {
        const uint64_t size = SzAr_GetFolderUnpackSize(&db_.db, block);
        if (size > limits_.max_total_bytes - std::min(spooled_bytes_, limits_.max_total_bytes)) return ArchiveError::kTooLarge;
        spooled_bytes_ += size;
        const UInt32 end = db_.FolderToFile[size_t(block) + 1];
        UInt32 file = db_.FolderToFile[block], crc = CRC_INIT_VAL;
        bool written = true, intact = true;
        std::ofstream out;
        uint64_t left = 0;
        // The next file with contents in this block.
        const auto next = [&] {
            for (; file < end; ++file) {
                if (db_.FileToFolder[file] != block) continue;
                out = spool_.Write(file);
                crc = CRC_INIT_VAL;
                left = SzArEx_GetFileSize(&db_, file);
                if (left) return;
                out.close();
            }
        };
        const auto store = [&](const uint8_t* data, size_t count) {
            while (count && file < end) {
                const size_t take = size_t(std::min<uint64_t>(count, left));
                crc = CrcUpdate(crc, data, take);
                out.write(reinterpret_cast<const char*>(data), std::streamsize(take));
                data += take;
                count -= take;
                left -= take;
                if (!out) return written = false;
                if (left) continue;
                out.close();
                written = bool(out);
                intact = !SzBitWithVals_Check(&db_.CRCs, file) || db_.CRCs.Vals[file] == CRC_GET_DIGEST(crc);
                if (!written || !intact) return false;
                ++file;
                next();
            }
            return true;
        };
        next();
        const ArchiveError error = StreamBlock(db_, block, file_, store);
        if (!written) return ArchiveError::kUnreadable;
        if (!intact) return ArchiveError::kDamaged;
        return error == ArchiveError::kNone && file < end ? ArchiveError::kDamaged : error;
    }

    FileReader file_;
    SevenZipStream stream_{};
    CLookToRead2 look_{};
    CSzArEx db_{};
    ArchiveLimits limits_;
    ArchiveSpool spool_;
    std::map<UInt32, ArchiveError> spooled_;
    uint64_t spooled_bytes_ = 0;
    UInt32 block_ = UInt32(-1);
    Byte* out_ = nullptr;
    size_t out_size_ = 0;
};

// ---- folder

bool IsLink(const fs::path& path, const fs::file_status& status) {
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

class FolderReader final : public ArchiveReader {
public:
    ArchiveError Open(const fs::path& folder, const ArchiveLimits& limits) {
        std::error_code ec;
        root_ = folder;
        if (!fs::is_directory(folder, ec)) return ArchiveError::kUnreadable;
        fs::recursive_directory_iterator it(folder, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code file_ec;
            const fs::file_status status = it->symlink_status(file_ec);
            if (file_ec) return ArchiveError::kUnreadable;
            ArchiveEntry entry;
            entry.name = ForwardSlashes(Utf8(it->path().lexically_relative(folder)));
            entry.link = IsLink(it->path(), status);
            entry.directory = !entry.link && fs::is_directory(status);
            if (entry.link) it.disable_recursion_pending();
            if (!entry.link && !entry.directory) {
                if (!fs::is_regular_file(status)) return ArchiveError::kUnsupported;
                entry.size = it->file_size(file_ec);
                if (file_ec) return ArchiveError::kUnreadable;
            }
            if (entries_.size() == limits.max_entries) return ArchiveError::kTooLarge;
            entries_.push_back(std::move(entry));
        }
        return ec ? ArchiveError::kUnreadable : ArchiveError::kNone;
    }

    ArchiveError Extract(size_t index, const Sink& sink) override {
        std::ifstream in(root_ / PathFromUtf8(entries_[index].name), std::ios::binary);
        std::vector<char> buffer(1 << 20);
        while (in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            const size_t got = size_t(in.gcount());
            if (got && !sink(reinterpret_cast<const uint8_t*>(buffer.data()), got)) return ArchiveError::kCancelled;
        }
        return in.eof() ? ArchiveError::kNone : ArchiveError::kUnreadable;
    }

private:
    fs::path root_;
};

template <typename Reader>
std::unique_ptr<ArchiveReader> OpenWith(const fs::path& path, const ArchiveLimits& limits, ArchiveError& error) {
    auto reader = std::make_unique<Reader>();
    error = reader->Open(path, limits);
    if (error != ArchiveError::kNone) return nullptr;
    return reader;
}

struct Magic {
    uint64_t size = 0;
    uint8_t bytes[6] = {};
    bool Read(const fs::path& archive) {
        FileReader file;
        const bool ok = file.Open(archive) && file.Size(size) && size >= sizeof bytes && file.ReadAt(0, bytes, sizeof bytes);
        file.Close();
        return ok;
    }
    bool SevenZip() const { return !std::memcmp(bytes, "7z\xBC\xAF\x27\x1C", sizeof bytes); }
    bool Rar() const { return !std::memcmp(bytes, "Rar!\x1A\x07", sizeof bytes); }
    bool Zip() const { return bytes[0] == 'P' && bytes[1] == 'K' && (bytes[2] == 3 || bytes[2] == 5); }
};

} // namespace

bool IsArchiveKnown(const fs::path& archive) {
    Magic magic;
    return magic.Read(archive) && (magic.SevenZip() || magic.Zip() || magic.Rar());
}

std::unique_ptr<ArchiveReader> OpenArchive(const fs::path& archive, const ArchiveLimits& limits, ArchiveError& error) {
    Magic magic;
    if (!magic.Read(archive)) {
        error = ArchiveError::kUnreadable;
        return nullptr;
    }
    const uint64_t size = magic.size;
    if (size > limits.max_archive_bytes) {
        error = ArchiveError::kTooLarge;
        return nullptr;
    }
    if (magic.SevenZip()) return OpenWith<SevenZipReader>(archive, limits, error);
    if (magic.Zip()) return OpenWith<ZipReader>(archive, limits, error);
    if (magic.Rar()) return OpenRar(archive, limits, error);
    error = ArchiveError::kUnsupported;
    return nullptr;
}

std::unique_ptr<ArchiveReader> OpenFolder(const fs::path& folder, const ArchiveLimits& limits, ArchiveError& error) {
    return OpenWith<FolderReader>(folder, limits, error);
}

bool IsUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xF0 && c < 0xF5 ? 4 : 0;
        if (!length || i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}

std::string FromUtf16(const uint16_t* text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; ++i) {
        uint32_t code = text[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < length && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000)
            code = 0x10000 + ((code - 0xD800) << 10) + (text[++i] - 0xDC00u);
        else if (code >= 0xD800 && code < 0xE000)
            code = 0xFFFD;
        AppendUtf8(out, code);
    }
    return out;
}

std::string ForwardSlashes(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

std::string FromWindows1252(const std::string& text) {
    static constexpr uint16_t kHigh[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        AppendUtf8(out, byte >= 0x80 && byte < 0xA0 ? kHigh[byte - 0x80] : byte);
    }
    return out;
}

} // namespace jpt
