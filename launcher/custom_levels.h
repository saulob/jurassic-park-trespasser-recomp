// The custom level packages of user_data/custom_levels: import, listing, README and removal. A package is data only: nothing in
// it is ever run, and the installed game versions are never written.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "archive_reader.h"

namespace jpt {

enum class ImportStatus {
    kInstalled,
    kAlreadyInstalled,
    kCancelled,
    kUnreadable,
    kUnsupported,
    kDamaged,
    kUnsafe,
    kTooLarge,
    kNoLevel,
    kSeveralPackages,
    kNoSpace,
    kWriteFailed,
};

const char* ImportStatusTitle(ImportStatus status);

struct ImportResult {
    ImportStatus status = ImportStatus::kUnreadable;
    std::string detail;
    std::string package;
    size_t files = 0;
    size_t skipped = 0; // programs and other files that are not level data
    bool ok() const { return status == ImportStatus::kInstalled; }
};

struct ImportHooks {
    std::function<void(const char*)> phase;
    std::function<bool(uint64_t done, uint64_t total)> progress; // false cancels
};

std::filesystem::path CustomLevelsDir(const std::filesystem::path& user_dir);

// Everything is unpacked and checked in a staging folder beside the packages; the package appears whole or not at all.
ImportResult ImportLevelArchive(const std::filesystem::path& archive, const std::filesystem::path& user_dir,
                                const ImportHooks& hooks, const ArchiveLimits& limits = {});
ImportResult ImportLevelFolder(const std::filesystem::path& folder, const std::filesystem::path& user_dir,
                               const ImportHooks& hooks, const ArchiveLimits& limits = {});
// What an interrupted import or removal left behind. Never called while one runs.
void CleanLevelStaging(const std::filesystem::path& user_dir);

// Whether the file is level data, a text or a picture; anything else in a package is left out.
bool IsLevelPackageFile(const std::string& relative_path);

struct LevelScene {
    std::string path; // relative to the package
    std::string file;
    bool valid = false; // a Trespasser scene
    bool loadable = false; // its world file is in the package or the game
    std::vector<std::string> missing;
};

struct LevelPackage {
    std::string id;
    std::filesystem::path dir;
    std::vector<LevelScene> scenes;
    std::string readme; // relative to the package; empty when it has no instructions
    bool mentions_atx = false;
    bool mentions_ce = false;
};

// data_dir: the selected game version's data folder, for the original files a level may reuse.
std::vector<LevelPackage> ListLevelPackages(const std::filesystem::path& user_dir, const std::filesystem::path& data_dir);

// Removes the package's folder and nothing else; its saves stay.
bool DeleteLevelPackage(const std::filesystem::path& user_dir, const std::string& id, std::string& error);

// The package's main instructions among its files; empty when none of them reads like one.
std::string ChooseReadme(const std::vector<std::string>& relative_paths);
// The readme file of an installed package, for the system to open: inside the package's own folder, a plain file (never a
// link) and of a kind ChooseReadme accepts, so nothing executable is ever handed over.
bool ResolveReadme(const std::filesystem::path& user_dir, const LevelPackage& package, std::filesystem::path& file);
// UTF-8 and UTF-16 with or without a BOM, else Windows-1252; line ends as '\n', other control characters dropped.
std::string DecodeText(const std::string& bytes);
bool ReadLevelText(const std::filesystem::path& file, std::string& text);

} // namespace jpt
