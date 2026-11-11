#define WIN32_LEAN_AND_MEAN
#include "game_install.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <system_error>

#include "jpt_user_data.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr uint64_t kSpaceMargin = 64ull << 20;

bool StartsWithNoCase(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

bool EqualNoCase(const std::string& a, const std::string& b) { return a.size() == b.size() && StartsWithNoCase(a, b); }

fs::path Sibling(const fs::path& dir, const wchar_t* suffix) {
    fs::path clean = dir;
    if (!clean.has_filename()) clean = clean.parent_path();
    fs::path out = clean;
    out += suffix;
    return out;
}

bool Inside(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec), p = fs::weakly_canonical(parent, ec);
    const std::wstring cs = c.wstring(), ps = p.wstring();
    return cs.size() > ps.size() && _wcsnicmp(cs.c_str(), ps.c_str(), ps.size()) == 0 &&
           (cs[ps.size()] == L'\\' || cs[ps.size()] == L'/');
}

void Remove(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void Say(const InstallHooks& hooks, const std::string& line) {
    if (hooks.log) hooks.log(line);
}

InstallResult Fail(InstallStatus status, std::string detail) {
    InstallResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

}  // namespace

fs::path StagingFolder(const fs::path& game_dir) { return Sibling(game_dir, L".install-temp"); }
fs::path BackupFolder(const fs::path& game_dir) { return Sibling(game_dir, L".install-backup"); }

const GameProfile* IdentifyDisc(const IsoImage& image, std::span<const GameProfile> profiles, std::string& why) {
    for (const GameProfile& p : profiles) {
        const IsoFile* exe = image.Find(p.disc_files.front().disc_path);
        const bool volume = EqualNoCase(image.volume_id(), p.volume_id);
        if (volume && exe && exe->size == p.executable_size) return &p;
        if (volume || exe) {
            why = std::string("this disc is not ") + p.label + " (volume \"" + image.volume_id() + "\"" +
                  (exe ? ", " + std::to_string(exe->size) + "-byte executable" : ", no " + std::string(p.disc_files.front().disc_path)) +
                  ")";
            return nullptr;
        }
    }
    why = "this is not a Trespasser disc image (volume \"" + image.volume_id() + "\")";
    return nullptr;
}

std::vector<PlannedFile> PlanInstall(const IsoImage& image, const GameProfile& profile, std::string& why) {
    std::vector<PlannedFile> plan;
    for (const DiscFile& d : profile.disc_files) {
        const IsoFile* f = image.Find(d.disc_path);
        if (!f) {
            why = std::string("the disc has no ") + d.disc_path;
            return {};
        }
        plan.push_back({f, d.installed});
    }
    const std::string prefix = profile.data_prefix;
    for (const IsoFile& f : image.files())
        if (StartsWithNoCase(f.path, prefix)) plan.push_back({&f, f.path});
    if (plan.size() == profile.disc_files.size()) {
        why = std::string("the disc has no ") + profile.data_prefix + " folder";
        return {};
    }
    return plan;
}

bool ActivateStaging(const fs::path& staging, const fs::path& game_dir, const fs::path& user_dir, std::string& error,
                     const std::function<void(const std::string&)>& log) {
    std::error_code ec;
    const fs::path backup = BackupFolder(game_dir);
    const bool replacing = fs::exists(game_dir, ec);
    if (replacing) {
        if (jpt_carry_user_files(game_dir.string().c_str(), user_dir.string().c_str(), nullptr) < 0) {
            error = "the saves of the current installation could not be copied to " + Utf8(user_dir);
            return false;
        }
        Remove(backup);
        fs::rename(game_dir, backup, ec);
        if (ec) {
            error = "the game folder is in use (close anything using " + Utf8(game_dir) + ")";
            return false;
        }
    } else if (game_dir.has_parent_path()) {
        fs::create_directories(game_dir.parent_path(), ec);
    }
    fs::rename(staging, game_dir, ec);
    if (ec) {
        error = "the new installation could not be moved into place";
        if (replacing) {
            std::error_code restore;
            fs::rename(backup, game_dir, restore);
            if (restore) error += "; the previous installation is in " + Utf8(backup);
        }
        return false;
    }
    if (replacing) {
        fs::remove_all(backup, ec);
        if (ec && log) log("the previous installation could not be removed completely: " + Utf8(backup));
    }
    return true;
}

InstallResult InstallFromIso(const fs::path& iso, const fs::path& game_dir, const fs::path& user_dir,
                             const InstallHooks& hooks, std::span<const GameProfile> profiles) {
    std::string why;
    std::error_code ec;
    if (hooks.phase) hooks.phase("Reading the disc image...");
    if (Inside(iso, game_dir)) return Fail(InstallStatus::kInsideGameFolder, Utf8(iso));
    IsoImage image;
    if (!image.Open(iso, why)) return Fail(InstallStatus::kNotAnImage, why);
    Say(hooks, "disc image " + Utf8(iso) + ": volume \"" + image.volume_id() + "\", " +
                   std::to_string(image.files().size()) + " files, " + (image.joliet() ? "Joliet names" : "ISO names"));
    const GameProfile* profile = IdentifyDisc(image, profiles, why);
    if (!profile) return Fail(InstallStatus::kWrongDisc, why);
    const std::vector<PlannedFile> plan = PlanInstall(image, *profile, why);
    if (plan.empty()) return Fail(InstallStatus::kWrongDisc, why);
    uint64_t total = 0;
    for (const PlannedFile& f : plan) total += f.source->size;

    const fs::path staging = StagingFolder(game_dir);
    Remove(staging);
    fs::create_directories(staging, ec);
    if (ec) return Fail(InstallStatus::kExtractFailed, "cannot create " + Utf8(staging));
    ULARGE_INTEGER free_bytes;
    if (GetDiskFreeSpaceExW(staging.c_str(), &free_bytes, nullptr, nullptr) &&
        free_bytes.QuadPart < total + kSpaceMargin) {
        Remove(staging);
        return Fail(InstallStatus::kNoSpace, std::to_string((total + kSpaceMargin) >> 20) + " MB needed, " +
                                                 std::to_string(free_bytes.QuadPart >> 20) + " MB free");
    }

    if (hooks.phase) hooks.phase("Copying the game files...");
    uint64_t done = 0;
    for (const PlannedFile& f : plan) {
        const fs::path to = staging / PathFromUtf8(f.installed);
        fs::create_directories(to.parent_path(), ec);
        const bool copied = image.Extract(*f.source, to, [&](uint64_t n) {
            done += n;
            return !hooks.progress || hooks.progress(done, total);
        }, why);
        if (!copied) {
            Remove(staging);
            return Fail(why == "cancelled" ? InstallStatus::kCancelled : InstallStatus::kExtractFailed, why);
        }
    }

    if (hooks.phase) hooks.phase("Checking the installed files...");
    const GameCheck check = CheckGameFolder(staging, std::span<const GameProfile>(profile, 1));
    if (check.state != GameState::kReady) {
        Remove(staging);
        return Fail(InstallStatus::kValidationFailed, check.detail);
    }
    if (hooks.progress && !hooks.progress(total, total)) {
        Remove(staging);
        return Fail(InstallStatus::kCancelled, "cancelled");
    }

    if (hooks.phase) hooks.phase("Activating the installation...");
    if (!ActivateStaging(staging, game_dir, user_dir, why, hooks.log)) {
        Remove(staging);
        return Fail(InstallStatus::kActivateFailed, why);
    }
    InstallResult result;
    result.status = InstallStatus::kInstalled;
    result.profile = profile;
    result.files = plan.size();
    result.bytes = total;
    result.detail = std::string(profile->label) + ", SHA-256 " + check.executable_sha256;
    return result;
}

const char* InstallStatusTitle(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled: return "Game installed";
    case InstallStatus::kCancelled: return "Installation cancelled";
    case InstallStatus::kNotAnImage: return "Not a disc image";
    case InstallStatus::kWrongDisc: return "Unsupported disc";
    case InstallStatus::kInsideGameFolder: return "Image inside the game folder";
    case InstallStatus::kNoSpace: return "Not enough disk space";
    case InstallStatus::kExtractFailed: return "Copy failed";
    case InstallStatus::kValidationFailed: return "The copied files are not valid";
    case InstallStatus::kActivateFailed: return "Could not activate the installation";
    }
    return "";
}

}  // namespace jpt
