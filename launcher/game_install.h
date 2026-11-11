// Install game: the player's own disc image is read directly (iso9660.h) into `<game>.install-temp`, checked with
// CheckGameFolder, and only then swapped in. Any failure or Cancel leaves the current installation as it was.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "game_files.h"
#include "iso9660.h"

namespace jpt {

enum class InstallStatus {
    kInstalled,
    kCancelled,
    kNotAnImage,
    kWrongDisc,
    kInsideGameFolder,
    kNoSpace,
    kExtractFailed,
    kValidationFailed,
    kActivateFailed,
};

struct InstallResult {
    InstallStatus status = InstallStatus::kExtractFailed;
    std::string detail;
    const GameProfile* profile = nullptr;
    size_t files = 0;
    uint64_t bytes = 0;
    bool ok() const { return status == InstallStatus::kInstalled; }
};

struct InstallHooks {
    std::function<void(const char* phase)> phase;
    std::function<bool(uint64_t done, uint64_t total)> progress;  // false cancels
    std::function<void(const std::string& line)> log;
};

struct PlannedFile {
    const IsoFile* source;
    std::string installed;  // relative, '/' separated
};

// The supported build this image holds; nullptr with the reason otherwise (another disc, or another version of it).
const GameProfile* IdentifyDisc(const IsoImage& image, std::span<const GameProfile> profiles, std::string& why);

// The files the profile installs from this image, or an empty list with the reason.
std::vector<PlannedFile> PlanInstall(const IsoImage& image, const GameProfile& profile, std::string& why);

std::filesystem::path StagingFolder(const std::filesystem::path& game_dir);
std::filesystem::path BackupFolder(const std::filesystem::path& game_dir);

// Replaces game_dir with the checked staging folder; the saves and the registry store of the old folder are copied
// to user_dir first. On failure game_dir is what it was before.
bool ActivateStaging(const std::filesystem::path& staging, const std::filesystem::path& game_dir,
                     const std::filesystem::path& user_dir, std::string& error,
                     const std::function<void(const std::string&)>& log);

InstallResult InstallFromIso(const std::filesystem::path& iso, const std::filesystem::path& game_dir,
                             const std::filesystem::path& user_dir, const InstallHooks& hooks,
                             std::span<const GameProfile> profiles = KnownProfiles());

const char* InstallStatusTitle(InstallStatus status);

}  // namespace jpt
