// One block of a 7z archive, decoded as a stream.
#pragma once

#include "archive_reader.h"
#include "os_file.h"
#include "third_party/lzma/7z.h"

namespace jpt {

// Copy, LZMA or LZMA2, with at most one filter (x86, ARM, ARM Thumb, PowerPC, SPARC, delta).
bool IsStreamableBlock(const CSzArEx& archive, UInt32 block);
ArchiveError StreamBlock(const CSzArEx& archive, UInt32 block, FileReader& file, const ArchiveReader::Sink& sink);

} // namespace jpt
