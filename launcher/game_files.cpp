#define WIN32_LEAN_AND_MEAN
#include "game_files.h"
#include "utf8.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <system_error>
#include <vector>

namespace jpt {
namespace {

constexpr ManifestEntry kUk116Manifest[] = {
#include "game_manifest_uk_116.inc"
};

constexpr DiscFile kUk116DiscFiles[] = {
    {"setup/tpassp6.exe", "trespass.exe"},
    {"setup/msvcirt.dll", "MSVCIRT.DLL"},
    {"setup/smackw32.dll", "SMACKW32.DLL"},
    {"setup/processor.dll", "Processor.dll"},
    {"readme.txt", "Readme.txt"},
};

const GameProfile kProfiles[] = {
    {"uk-1.0.116", "Trespasser UK 1.0.116", "Trespass_UK_116", "trespass.exe",
     "d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed", 2568192, kUk116DiscFiles, "data/",
     kUk116Manifest},
};

}  // namespace

std::span<const GameProfile> KnownProfiles() { return kProfiles; }

std::string Sha256File(const std::filesystem::path& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<unsigned char> block(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(h, block.data(), static_cast<DWORD>(block.size()), &got, nullptr) && got)
        ok = BCryptHashData(hash, block.data(), got, 0) >= 0;
    ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    if (!ok) return {};
    std::string hex;
    char byte[3];
    for (unsigned char b : digest) {
        std::snprintf(byte, sizeof byte, "%02x", b);
        hex += byte;
    }
    return hex;
}

GameCheck CheckGameFolder(const std::filesystem::path& game_dir, std::span<const GameProfile> profiles) {
    GameCheck check;
    std::error_code ec;
    if (profiles.empty()) return check;
    const std::filesystem::path exe = game_dir / profiles.front().executable;
    if (!std::filesystem::is_regular_file(exe, ec)) {
        check.detail = "no game executable in " + Utf8(game_dir);
        return check;
    }
    check.executable_sha256 = Sha256File(exe);
    if (check.executable_sha256.empty()) {
        check.state = GameState::kUnreadable;
        check.detail = "cannot read " + Utf8(exe);
        return check;
    }
    for (const GameProfile& p : profiles)
        if (check.executable_sha256 == p.executable_sha256) check.profile = &p;
    check.state = GameState::kInvalid;
    if (!check.profile) {
        check.detail = "unknown executable, SHA-256 " + check.executable_sha256;
        return check;
    }
    for (const ManifestEntry& e : check.profile->manifest) {
        const std::filesystem::path file = game_dir / PathFromUtf8(e.path);
        const uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec) {
            check.detail = std::string("missing ") + e.path;
            return check;
        }
        if (size != e.size) {
            check.detail = std::string(e.path) + " has " + std::to_string(size) + " bytes, expected " +
                           std::to_string(e.size);
            return check;
        }
    }
    check.state = GameState::kReady;
    return check;
}

const char* GameStateTitle(GameState state) {
    switch (state) {
    case GameState::kReady: return "Ready to play";
    case GameState::kMissing: return "Game not installed";
    case GameState::kInvalid: return "Invalid game files";
    case GameState::kUnreadable: return "Game files unreadable";
    }
    return "";
}

}  // namespace jpt
