// The generated files of an installed package: manifest.json, the record of its analysis, and runtime.map, what the game reads
// of it (src/common/jpt_custom_level.c). Both are made again whenever they are missing or differ from the package's analysis.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "jpt_custom_level.h"
#include "level_analysis.h"

namespace jpt {

inline constexpr int kManifestSchema = 1;
inline constexpr char kManifestFile[] = "manifest.json";

// Where a package came from; the only part of a manifest that its files do not tell.
struct PackageOrigin {
    std::string source_type; // "zip", "7z", "rar", "folder", "migrated", "external" (linked, never copied)
    std::string source_name;
    std::string source_hash; // SHA-256 of the installed files: names and contents
    std::vector<std::string> refused; // "name - reason", files of the source that are not installed
    int64_t imported_at = 0; // seconds since 1970; 0: not recorded
};

std::string DateText(int64_t seconds); // "2026-11-25T10:12:48Z"
int64_t DateOf(const std::string& text); // 0 when it is none

// The result of one level, which its siblings never change.
struct LevelVerdict {
    std::string scn, status; // LevelClassName
    bool playable = false, scripted = false;
    std::vector<std::string> script_files, script_required, blockers; // blockers: "CODE subject"
};

struct PackageManifest {
    std::string package_id;
    PackageOrigin origin;
    PackageAnalysis analysis;
    std::vector<std::string> missing; // "PV.scn: Pv-130.pid", where the installed game was known
    std::vector<LevelVerdict> verdicts;
};

std::string ManifestJson(const PackageManifest& manifest);
// False for a file that is no manifest, or one of a schema this version does not know.
bool ReadManifestOrigin(const std::string& json, PackageOrigin& origin);
// A manifest a later version wrote: never replaced.
bool ManifestIsNewer(const std::string& json);

std::string RuntimeMap(const PackageAnalysis& analysis);
std::string LoadingPictureFile(size_t index); // relative to the package
// Every target a safe path in a folder of the game's and every source a file of the package.
// A file the game reads: no program, library, script or settings file.
bool IsGamePath(const std::string& target);
bool RuntimeIsSafe(const PackageAnalysis& analysis, const std::vector<PackageFile>& files, std::string& problem);

// Generated files: never part of what a package is compared by.
bool IsGeneratedPath(const std::string& relative_path);

std::string AnalysisReport(const PackageManifest& manifest);

} // namespace jpt
