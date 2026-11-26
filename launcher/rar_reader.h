// RAR 2.0 to 4.x and RAR 5 archives.
#pragma once

#include <filesystem>
#include <memory>

#include "archive_reader.h"

namespace jpt {

std::unique_ptr<ArchiveReader> OpenRar(const std::filesystem::path& archive, const ArchiveLimits& limits, ArchiveError& error);

} // namespace jpt
