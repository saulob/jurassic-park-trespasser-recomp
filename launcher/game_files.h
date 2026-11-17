// The game versions the launcher installs (src/common/jpt_game_version.h), every file each one installs, and the
// readiness check of an installed version (launcher start, Play and the installer).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "jpt_game_version.h"

namespace jpt {

enum class SourceKind { kDisc, kCabinet };

// What the importer makes of a source file. The original executables are only ever import inputs: they become the
// guest image and the release's string table, and are never installed under their own names.
enum class FileRole {
    kData,         // copied as is
    kGuestImage,   // copied as is to JPT_GUEST_IMAGE; the disc may lack its source (then nothing is installed)
    kStringTable,  // the string resources of the source, as a jpt_string_table.h file
};

struct InstallFile {
    SourceKind kind;
    const char* source;     // path on the disc, or in the profile's cabinet; '/' separated
    const char* installed;  // relative to the version folder, '/' separated
    uint64_t size;          // of the installed file
    const char* sha256;     // of the installed file; a copy that differs is damaged
    FileRole role = FileRole::kData;
    uint64_t source_size = 0;                // kStringTable: the source as pressed
    const char* source_sha256 = nullptr;     // kStringTable: the source as pressed
};

// A pressing of a version, told apart from other pressings of it by its volume label only; the version itself is
// recognised by GameProfile::identity_file.
struct DiscIdentity {
    const char* edition;    // "English retail (UK)"
    const char* volume_id;  // informational: copies made with other tools change it
};

struct GameProfile {
    const jpt_game_version_t* version;
    const char* identity_file;    // a data file on the disc whose SHA-256 differs between the versions' discs
    const char* identity_sha256;
    std::span<const DiscIdentity> discs;
    const char* cabinet;  // disc path of the InstallShield cabinet the cabinet files come from, or nullptr
    std::span<const InstallFile> files;

    const char* id() const { return version->id; }
    const char* label() const { return version->label; }
};

std::span<const GameProfile> KnownProfiles();
const GameProfile* ProfileById(std::span<const GameProfile> profiles, const std::string& id);

enum class GameState { kMissing, kReady, kInvalid };

struct GameCheck {
    GameState state = GameState::kMissing;
    const GameProfile* profile = nullptr;
    std::string detail;  // why it is not ready
};

// Missing: no guest image. Invalid: a file of the manifest missing or of another size. Sizes only: the digests are
// checked when the files are made.
GameCheck CheckGameFolder(const std::filesystem::path& dir, const GameProfile& expected);

struct InstalledVersion {
    const GameProfile* profile = nullptr;
    std::filesystem::path dir;
    GameCheck check;
};

// Every version whose folder <game_root>\versions\<id> holds a guest image.
std::vector<InstalledVersion> FindInstalledVersions(const std::filesystem::path& game_root,
                                                    std::span<const GameProfile> profiles = KnownProfiles());
// The version to show and play: `wanted` when it is ready, else the first ready one the port can run, else the first
// ready one; nullptr when none is.
const InstalledVersion* ChooseVersion(const std::vector<InstalledVersion>& versions, const std::string& wanted);

// Removes the launcher's copy of a version: its managed folder. False with a reason; nothing outside it is touched.
bool RemoveVersion(const InstalledVersion& version, std::string& error);

// Lower-case hex SHA-256 of a file; empty when it cannot be read.
std::string Sha256File(const std::filesystem::path& file);

const char* GameStateTitle(GameState state);

}  // namespace jpt
