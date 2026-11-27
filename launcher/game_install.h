// Import game: the player's own disc image (iso9660.h, is_cabinet.h), or folders of the game they already have (the disc
// copied to a folder, an installation), are read directly into <game>\versions\<id>.install-temp, every file checked
// against its pressed SHA-256, and only then swapped in. Any failure or Cancel leaves every installed version as it was.
// Sources are only ever read. The original executables are import inputs only: one becomes the guest image, the Russian
// release's one the string table; nothing found in a folder is ever run.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "game_files.h"
#include "is_cabinet.h"
#include "iso9660.h"

namespace jpt {

enum class InstallStatus {
    kInstalled,
    kUpToDate,
    kCancelled,
    kNotAnImage,
    kWrongDisc,
    kUnsupportedBuild,
    kDamagedDisc,
    kNoGuestImageSource,  // the disc lacks the executable the guest image (or the release's text) is made from
    kInsideGameFolder,
    kNoSpace,
    kExtractFailed,
    kValidationFailed,
    kActivateFailed,
    kNotAnInstallation,       // a folder without the game's data files
    kIncompleteInstallation,  // a folder of a known version without all of its files; the disc's folder can complete it
};

struct InstallResult {
    InstallStatus status = InstallStatus::kExtractFailed;
    std::string detail;
    const GameProfile* profile = nullptr;
    const DiscIdentity* disc = nullptr;
    std::filesystem::path dir;            // where the version is installed
    std::vector<std::string> diagnostics;  // an unsupported build's data file fingerprints, to add it later
    std::vector<std::string> missing;      // a folder import: the version's files no chosen folder has with the pressed bytes
    size_t files = 0;
    size_t repaired = 0;  // damaged files taken from another installed version with the pressed digest
    uint64_t bytes = 0;
    bool ok() const { return status == InstallStatus::kInstalled || status == InstallStatus::kUpToDate; }
};

struct InstallHooks {
    std::function<void(const char* phase)> phase;
    std::function<bool(uint64_t done, uint64_t total)> progress;  // false cancels
    std::function<void(const std::string& line)> log;
    bool replace = false;  // install again even when the version is already installed and ready
};

struct PlannedFile {
    const InstallFile* file;
    const IsoFile* disc = nullptr;
    const CabinetFile* cabinet = nullptr;
    std::filesystem::path folder_file;  // a folder import: the file with the pressed bytes
    // false: the source lacks the file of a guest image or table
    bool present() const { return disc || cabinet || !folder_file.empty(); }
};

struct DiscMatch {
    const GameProfile* profile = nullptr;
    const DiscIdentity* disc = nullptr;
    InstallStatus status = InstallStatus::kWrongDisc;  // when nothing matched
    std::vector<std::string> diagnostics;
};

// Opens the profile's cabinet when it has one; false with the reason.
bool OpenCabinet(IsoImage& image, const GameProfile& profile, Cabinet& cabinet, std::string& why);

// The version this image holds: a profile's identity data file with its digest, and every data file of that version
// on the disc. No executable is read. Otherwise the reason, with the data file fingerprints of a Trespasser disc no
// profile knows.
DiscMatch IdentifyDisc(IsoImage& image, std::span<const GameProfile> profiles, std::string& why);

// The files the profile installs from this image (and its cabinet), or an empty list with the reason. Every data file
// must be there with its size; the source of the guest image or of a string table may be absent (not present()).
std::vector<PlannedFile> PlanInstall(const IsoImage& image, const Cabinet* cabinet, const GameProfile& profile,
                                     std::string& why);

struct ReleaseString {
    uint32_t id;
    std::string bytes;  // in the release's code page
};

// The jpt_string_table.h file holding `strings` in the order given.
std::vector<uint8_t> BuildStringTable(const std::vector<ReleaseString>& strings);

// The string table of the string resources of an executable's bytes. The loader needs a file: it is written to
// `scratch_dir` and deleted again, and only its resources are read (it is never executed). False with a reason.
bool DeriveStringTable(const std::vector<uint8_t>& executable, const std::filesystem::path& scratch_dir,
                       std::vector<uint8_t>& table, std::string& error);

std::filesystem::path VersionFolder(const std::filesystem::path& game_root, const GameProfile& profile);
std::filesystem::path StagingFolder(const std::filesystem::path& dir);
std::filesystem::path BackupFolder(const std::filesystem::path& dir);

// Replaces dir with the checked staging folder. On failure dir is what it was before.
bool ActivateStaging(const std::filesystem::path& staging, const std::filesystem::path& dir, std::string& error,
                     const std::function<void(const std::string&)>& log);

InstallResult InstallFromIso(const std::filesystem::path& iso, const std::filesystem::path& game_root,
                             const InstallHooks& hooks, std::span<const GameProfile> profiles = KnownProfiles());

// What folders of the game hold, read only: the version whose identity file one of them has, and for each file of it
// a copy with the pressed bytes wherever it is (the disc's own place, an installation's root or data folder, any name
// for an executable). Several folders complete each other, the first one first.
struct FolderMatch {
    const GameProfile* profile = nullptr;
    InstallStatus status = InstallStatus::kNotAnInstallation;  // when nothing can be installed
    std::vector<PlannedFile> plan;
    std::vector<std::string> missing;      // installed names no folder has with the pressed bytes
    std::vector<std::string> modified;     // of those, the ones a folder has with other bytes (a patch or a mod)
    std::vector<std::string> diagnostics;  // what the folders hold: executables, data file fingerprints
    bool complete() const { return profile && missing.empty(); }
};
FolderMatch IdentifyFolders(const std::vector<std::filesystem::path>& folders, std::span<const GameProfile> profiles,
                            std::string& why);

InstallResult InstallFromFolders(const std::vector<std::filesystem::path>& folders, const std::filesystem::path& game_root,
                                 const InstallHooks& hooks, std::span<const GameProfile> profiles = KnownProfiles());

const char* InstallStatusTitle(InstallStatus status);

}  // namespace jpt
