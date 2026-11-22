#include "game_files.h"
#include "portable.h"
#include "utf8.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

#include <cstdio>
#include <fstream>
#include <vector>

#include "jpt_hash.h"
#include <system_error>

namespace jpt {
namespace {

namespace fs = std::filesystem;

constexpr InstallFile kEn116[] = {
#include "game_manifest_en_116.inc"
};
constexpr InstallFile kRu116[] = {
#include "game_manifest_ru_116.inc"
};
constexpr InstallFile kEs117[] = {
#include "game_manifest_es_117.inc"
};
constexpr InstallFile kPtBr117[] = {
#include "game_manifest_ptbr_117.inc"
};

constexpr DiscIdentity kEn116Discs[] = {
    {"English retail (UK)", "Trespass_UK_116"},
    {"English retail (US)", "Trespasser_116d"},
};
constexpr DiscIdentity kRu116Discs[] = {
    {"Russian (Fargus)", "TRESPASSER_116D"},
};
constexpr DiscIdentity kEs117Discs[] = {
    {"Spanish (Spain)", "TRESPASSER_S"},
};
constexpr DiscIdentity kPtBr117Discs[] = {
    {"Brazilian Portuguese (Brazil)", "Trespasser"},
};

// The smallest data file of each version whose digest no other version's disc has.
const GameProfile kProfiles[] = {
    {&jpt_game_versions[0], "data/menu/loader.ddf", "0ba6a442592880a407ff98fa2c1b6a703b5a842b94b330461aa4c6576390b0f7",
     kEn116Discs, nullptr, kEn116},
    {&jpt_game_versions[1], "tresp.ttf", "a7767c39415d46623146fad472e4e512185cb66bdc7497a7a3fb372ad191fa7b", kRu116Discs,
     "setup/data1.cab", kRu116},
    {&jpt_game_versions[2], "data/menu/loader.ddf", "7cb5236e3e02ff8cd73e642b2aad1316cc08abb6e1cc58a4d099c559ac146e5c",
     kEs117Discs, nullptr, kEs117},
    {&jpt_game_versions[3], "data/menu/loader.ddf", "d5c6f83dac5e6300bb6682a5770ee52f4aebd994a3ec5834c1401d3ba0af0916",
     kPtBr117Discs, nullptr, kPtBr117},
};

}  // namespace

std::span<const GameProfile> KnownProfiles() { return kProfiles; }

const GameProfile* ProfileById(std::span<const GameProfile> profiles, const std::string& id) {
    for (const GameProfile& p : profiles)
        if (_stricmp(p.id(), id.c_str()) == 0) return &p;
    return nullptr;
}

#ifndef _WIN32
std::string Sha256File(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    jpt_sha256_t digest;
    jpt_sha256_init(&digest);
    std::vector<char> block(1 << 20);
    while (in.read(block.data(), static_cast<std::streamsize>(block.size())) || in.gcount() > 0)
        jpt_sha256_update(&digest, block.data(), static_cast<size_t>(in.gcount()));
    if (in.bad()) return {};
    char hex[65];
    jpt_sha256_hex(&digest, hex);
    return hex;
}
#else
std::string Sha256File(const fs::path& file) {
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
#endif

GameCheck CheckGameFolder(const fs::path& dir, const GameProfile& expected) {
    GameCheck check;
    std::error_code ec;
    if (!fs::is_regular_file(dir / JPT_GUEST_IMAGE, ec)) {
        check.detail = "no guest image in " + Utf8(dir);
        return check;
    }
    check.state = GameState::kInvalid;
    check.profile = &expected;
    for (const InstallFile& f : expected.files) {
        const uintmax_t size = fs::file_size(dir / PathFromUtf8(f.installed), ec);
        if (ec) {
            check.detail = std::string("missing ") + f.installed;
            return check;
        }
        if (size != f.size) {
            check.detail = std::string(f.installed) + " has " + std::to_string(size) + " bytes, expected " +
                           std::to_string(f.size);
            return check;
        }
    }
    check.state = GameState::kReady;
    return check;
}

std::vector<InstalledVersion> FindInstalledVersions(const fs::path& game_root, std::span<const GameProfile> profiles) {
    std::vector<InstalledVersion> found;
    std::error_code ec;
    for (const GameProfile& p : profiles) {
        const fs::path dir = game_root / JPT_VERSIONS_DIR / PathFromUtf8(p.id());
        if (fs::is_regular_file(dir / JPT_GUEST_IMAGE, ec)) found.push_back({&p, dir, CheckGameFolder(dir, p)});
    }
    return found;
}

const InstalledVersion* ChooseVersion(const std::vector<InstalledVersion>& versions, const std::string& wanted) {
    for (const InstalledVersion& v : versions)
        if (v.profile && v.check.state == GameState::kReady && _stricmp(v.profile->id(), wanted.c_str()) == 0) return &v;
    for (const InstalledVersion& v : versions)
        if (v.profile && v.check.state == GameState::kReady && jpt_game_version_playable(v.profile->version)) return &v;
    for (const InstalledVersion& v : versions)
        if (v.profile && v.check.state == GameState::kReady) return &v;
    return nullptr;
}

bool RemoveVersion(const InstalledVersion& version, std::string& error) {
    std::error_code ec;
    if (!version.profile || version.dir.filename() != PathFromUtf8(version.profile->id()) ||
        version.dir.parent_path().filename() != JPT_VERSIONS_DIR) {
        error = "not a version folder: " + Utf8(version.dir);
        return false;
    }
    fs::path removing = version.dir;
    removing += ".removing";
    fs::remove_all(removing, ec);
    fs::rename(version.dir, removing, ec);
    if (ec) {
        error = "the version folder is in use (close anything using " + Utf8(version.dir) + ")";
        return false;
    }
    fs::remove_all(removing, ec);
    if (ec) error = "some files could not be deleted: " + Utf8(removing);
    return true;
}

const char* GameStateTitle(GameState state) {
    switch (state) {
    case GameState::kReady: return "Ready to play";
    case GameState::kMissing: return "Game not installed";
    case GameState::kInvalid: return "Invalid game files";
    }
    return "";
}

}  // namespace jpt
