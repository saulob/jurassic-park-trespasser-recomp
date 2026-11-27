// What an import says in the log: enough to understand a package from the log alone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "custom_levels.h"
#include "level_analysis.h"

namespace jpt {

inline constexpr size_t kLogDocuments = 256;
inline constexpr int kInstructionsScore = 60; // LevelReadmeScore from here on: a readme or installation notes

// A text of the package is described, never copied: its words belong to its author.
struct LogDocument {
    std::string path, sha256;
    uint64_t bytes = 0;
    bool readable = false, readme = false;
};

struct ImportRecord {
    std::string type, name, path; // "zip", "7z", "rar", "folder"; the source's own name and where it is
    uint64_t bytes = 0;
    std::string sha256; // of the archive
    std::string package;
    size_t found = 0;
    std::vector<LogDocument> documents;
    size_t documents_skipped = 0;
    std::vector<std::string> accepted;
    std::vector<RefusedFile> refused;
    PackageAnalysis analysis;
    std::vector<LevelScene> scenes;
    ImportStatus status = ImportStatus::kUnreadable;
    std::string detail;
};

std::vector<std::string> ImportLog(const ImportRecord& record);

} // namespace jpt
