// What a supported Trespasser install is, and the one readiness check (launcher start, Play and the installer).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace jpt {

struct ManifestEntry {
    const char* path;  // relative to the game folder, '/' separated
    uint64_t size;
};

// Where an installed file comes from on the disc. Everything under data/ is copied as it is.
struct DiscFile {
    const char* disc_path;  // lower case, '/' separated
    const char* installed;
};

struct GameProfile {
    const char* id;
    const char* label;              // "Trespasser UK 1.0.116"
    const char* volume_id;          // the disc's primary volume identifier
    const char* executable;         // installed name
    const char* executable_sha256;
    uint64_t executable_size;
    std::span<const DiscFile> disc_files;
    const char* data_prefix;        // "data/"
    std::span<const ManifestEntry> manifest;
};

// The builds the port supports; one today.
std::span<const GameProfile> KnownProfiles();

enum class GameState { kMissing, kReady, kInvalid, kUnreadable };

struct GameCheck {
    GameState state = GameState::kMissing;
    const GameProfile* profile = nullptr;
    std::string detail;  // why it is not ready
    std::string executable_sha256;
};

// Missing: no executable. Invalid: an executable no profile knows, or a file of its manifest missing or of another size.
GameCheck CheckGameFolder(const std::filesystem::path& game_dir, std::span<const GameProfile> profiles = KnownProfiles());

// Lower-case hex SHA-256 of a file; empty when it cannot be read.
std::string Sha256File(const std::filesystem::path& file);

const char* GameStateTitle(GameState state);

}  // namespace jpt
