#include "archive_spool.h"

#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace jpt {

ArchiveSpool::~ArchiveSpool() {
    std::error_code ec;
    if (created()) fs::remove_all(folder_, ec);
}

bool ArchiveSpool::Create() {
    std::error_code ec;
    std::random_device random;
    const fs::path base = fs::temp_directory_path(ec);
    for (int i = 0; i < 16 && !created() && !ec; ++i) {
        const fs::path candidate = base / ("jpt-archive-" + std::to_string(random()) + std::to_string(random()));
        if (fs::create_directory(candidate, ec)) folder_ = candidate;
    }
    return created();
}

std::ofstream ArchiveSpool::Write(size_t index) const { return std::ofstream(folder_ / std::to_string(index), std::ios::binary | std::ios::trunc); }

ArchiveError ArchiveSpool::Read(size_t index, uint64_t size, const ArchiveReader::Sink& sink) const {
    std::ifstream in(folder_ / std::to_string(index), std::ios::binary);
    std::vector<char> buffer(1 << 20);
    uint64_t read = 0;
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        const size_t got = size_t(in.gcount());
        read += got;
        if (got && !sink(reinterpret_cast<const uint8_t*>(buffer.data()), got)) return ArchiveError::kCancelled;
    }
    return in.eof() && read == size ? ArchiveError::kNone : ArchiveError::kUnreadable;
}

} // namespace jpt
