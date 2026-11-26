// The expanded files of an archive that can only be read in order: a temporary folder of files named by their number, removed
// with this object.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>

#include "archive_reader.h"

namespace jpt {

class ArchiveSpool {
public:
    ArchiveSpool() = default;
    ~ArchiveSpool();
    ArchiveSpool(const ArchiveSpool&) = delete;
    ArchiveSpool& operator=(const ArchiveSpool&) = delete;

    bool Create();
    bool created() const { return !folder_.empty(); }
    std::ofstream Write(size_t index) const;
    ArchiveError Read(size_t index, uint64_t size, const ArchiveReader::Sink& sink) const;

private:
    std::filesystem::path folder_;
};

} // namespace jpt
