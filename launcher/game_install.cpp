#include "game_install.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <map>
#include <system_error>

#include "jpt_hash.h"
#include "jpt_l10n.h"
#include "jpt_string_table.h"
#include "os_file.h"
#include "pe_resources.h"
#include "portable.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr uint64_t kSpaceMargin = 64ull << 20;
constexpr uint32_t kStringResourceType = 6;  // RT_STRING
// Data files whose presence marks a Trespasser disc, for the fingerprints of one no profile knows.
constexpr const char* kTrespasserMarkers[] = {"data/be.scn", "data/menu/loader.ddf", "tresp.ttf"};

fs::path Sibling(const fs::path& dir, const char* suffix) {
    fs::path clean = dir;
    if (!clean.has_filename()) clean = clean.parent_path();
    fs::path out = clean;
    out += suffix;
    return out;
}

bool Inside(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec), p = fs::weakly_canonical(parent, ec);
#ifdef _WIN32
    const std::wstring cs = c.wstring(), ps = p.wstring();
    return cs.size() > ps.size() && _wcsnicmp(cs.c_str(), ps.c_str(), ps.size()) == 0 &&
           (cs[ps.size()] == L'\\' || cs[ps.size()] == L'/');
#else
    const std::string cs = c.string(), ps = p.string();
    return cs.size() > ps.size() && cs.compare(0, ps.size(), ps) == 0 && cs[ps.size()] == '/';
#endif
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

std::string Digest(const std::vector<uint8_t>& bytes) {
    char hex[65];
    jpt_sha256_buffer_hex(bytes.data(), bytes.size(), hex);
    return hex;
}

bool WriteBytes(const fs::path& to, const std::vector<uint8_t>& bytes, std::string& error) {
    PartFileWriter out;
    if (!out.Open(to)) {
        error = "cannot create " + Utf8(to);
        return false;
    }
    if (!out.Write(bytes.data(), bytes.size()) || !out.Commit()) {
        error = "cannot write " + Utf8(to) + " (disk full?)";
        return false;
    }
    return true;
}

// Another installed version's copy of a file with the pressed digest (the five level files every 1998 disc shares).
bool Repair(const fs::path& game_root, std::span<const GameProfile> profiles, const fs::path& staging,
            const InstallFile& f, const InstallHooks& hooks) {
    std::error_code ec;
    for (const InstalledVersion& v : FindInstalledVersions(game_root, profiles)) {
        if (!v.profile) continue;
        const fs::path candidate = v.dir / PathFromUtf8(f.installed);
        if (fs::file_size(candidate, ec) != f.size || ec || Sha256File(candidate) != f.sha256) continue;
        const fs::path to = staging / PathFromUtf8(f.installed);
        fs::copy_file(candidate, to, fs::copy_options::overwrite_existing, ec);
        if (ec || Sha256File(to) != f.sha256) continue;
        Say(hooks, std::string("repaired ") + f.installed + " from the installed " + v.profile->label());
        return true;
    }
    return false;
}

uint64_t SourceSize(const InstallFile& f) { return f.role == FileRole::kStringTable ? f.source_size : f.size; }

bool ReadSource(IsoImage& image, const Cabinet& cabinet, const PlannedFile& f, std::vector<uint8_t>& bytes,
                std::string& error) {
    return f.disc ? image.ReadAll(*f.disc, bytes, error) : cabinet.Read(*f.cabinet, bytes, error);
}

}  // namespace

std::vector<uint8_t> BuildStringTable(const std::vector<ReleaseString>& strings) {
    std::vector<uint32_t> ids, lengths;
    std::vector<const char*> texts;
    size_t text_bytes = 0;
    for (const ReleaseString& s : strings) {
        ids.push_back(s.id);
        lengths.push_back(static_cast<uint32_t>(s.bytes.size()));
        texts.push_back(s.bytes.data());
        text_bytes += s.bytes.size();
    }
    const uint32_t count = static_cast<uint32_t>(strings.size());
    std::vector<uint8_t> table(jpt_string_table_size(count, text_bytes));
    table.resize(jpt_string_table_write(table.data(), table.size(), ids.data(), texts.data(), lengths.data(), count));
    return table;
}

// One block of 16 length-prefixed UTF-16 strings of the string table, as the unit below 0x100 or '?'.
void AppendStringBlock(uint32_t block, const uint8_t* bytes, size_t size, std::vector<ReleaseString>& strings) {
    const size_t words = size / 2;
    size_t at = 0;
    auto word = [&](size_t index) { return static_cast<uint32_t>(bytes[index * 2] | (bytes[index * 2 + 1] << 8)); };
    for (uint32_t i = 0; i < 16 && at < words; ++i) {
        const size_t units = word(at++);
        if (units > words - at) break;
        if (units) {
            ReleaseString s{(block - 1) * 16 + i, {}};
            for (size_t u = 0; u < units; ++u) {
                const uint32_t unit = word(at + u);
                s.bytes.push_back(unit < 0x100 ? static_cast<char>(unit) : '?');
            }
            strings.push_back(std::move(s));
        }
        at += units;
    }
}

bool DeriveStringTable(const std::vector<uint8_t>& executable, const fs::path& scratch_dir, std::vector<uint8_t>& table,
                       std::string& error) {
    std::vector<ReleaseString> strings;
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(scratch_dir, ec);
    const fs::path file = fs::absolute(scratch_dir, ec) / "resources.tmp";
    if (!WriteBytes(file, executable, error)) return false;
    // Resources only: the file is mapped as data and never run.
    HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        DeleteFileW(file.c_str());
        error = "the file has no readable resources";
        return false;
    }
    for (uint32_t block = 1; block <= JPT_L10N_ID_LIMIT / 16; ++block) {
        HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(block), MAKEINTRESOURCEW(kStringResourceType));
        HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
        const uint8_t* at = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        if (at) AppendStringBlock(block, at, SizeofResource(module, found), strings);
    }
    FreeLibrary(module);
    DeleteFileW(file.c_str());
#else
    (void)scratch_dir;
    for (uint32_t block = 1; block <= JPT_L10N_ID_LIMIT / 16; ++block) {
        const uint8_t* at = nullptr;
        size_t size = 0;
        if (FindPeResource(executable, kStringResourceType, block, at, size)) AppendStringBlock(block, at, size, strings);
    }
    if (strings.empty() && !(executable.size() > 1 && executable[0] == 'M' && executable[1] == 'Z')) {
        error = "the file has no readable resources";
        return false;
    }
#endif
    if (strings.empty()) {
        error = "the executable has no string resources";
        return false;
    }
    table = BuildStringTable(strings);
    return true;
}

fs::path VersionFolder(const fs::path& game_root, const GameProfile& profile) {
    return game_root / JPT_VERSIONS_DIR / PathFromUtf8(profile.id());
}
fs::path StagingFolder(const fs::path& dir) { return Sibling(dir, ".install-temp"); }
fs::path BackupFolder(const fs::path& dir) { return Sibling(dir, ".install-backup"); }

bool OpenCabinet(IsoImage& image, const GameProfile& profile, Cabinet& cabinet, std::string& why) {
    if (!profile.cabinet) return true;
    const IsoFile* f = image.Find(profile.cabinet);
    std::vector<uint8_t> bytes;
    if (!f) {
        why = std::string("the disc has no ") + profile.cabinet;
        return false;
    }
    return image.ReadAll(*f, bytes, why) && cabinet.Open(std::move(bytes), why);
}

std::vector<PlannedFile> PlanInstall(const IsoImage& image, const Cabinet* cabinet, const GameProfile& profile,
                                     std::string& why) {
    std::vector<PlannedFile> plan;
    for (const InstallFile& f : profile.files) {
        PlannedFile p{&f};
        if (f.kind == SourceKind::kDisc) p.disc = image.Find(f.source);
        else if (cabinet) p.cabinet = cabinet->Find(f.source);
        if (!p.present() && f.role != FileRole::kData) {
            plan.push_back(p);
            continue;
        }
        const uint64_t size = p.disc ? p.disc->size : p.cabinet ? p.cabinet->size : ~0ull;
        if (size != SourceSize(f)) {
            why = (size == ~0ull ? std::string("the disc has no ") : std::string("another size of ")) + f.source;
            return {};
        }
        plan.push_back(p);
    }
    return plan;
}

DiscMatch IdentifyDisc(IsoImage& image, std::span<const GameProfile> profiles, std::string& why) {
    DiscMatch match;
    std::map<std::string, std::string> digests;
    auto digest_of = [&](const char* path) -> const std::string& {
        auto it = digests.find(path);
        if (it != digests.end()) return it->second;
        std::string& d = digests[path];
        std::vector<uint8_t> bytes;
        std::string error;
        const IsoFile* f = image.Find(path);
        if (f && image.ReadAll(*f, bytes, error)) d = Digest(bytes);
        return d;
    };
    std::string near_miss;
    for (const GameProfile& p : profiles) {
        if (p.discs.empty() || digest_of(p.identity_file) != p.identity_sha256) continue;
        Cabinet cabinet;
        std::string plan_error;
        if (OpenCabinet(image, p, cabinet, plan_error) &&
            !PlanInstall(image, p.cabinet ? &cabinet : nullptr, p, plan_error).empty()) {
            match.profile = &p;
            match.disc = &p.discs[0];
            for (const DiscIdentity& d : p.discs)  // the label tells the pressings of one version apart
                if (_stricmp(d.volume_id, image.volume_id().c_str()) == 0) match.disc = &d;
            return match;
        }
        if (near_miss.empty()) near_miss = std::string("a ") + p.label() + " disc without all of its files (" + plan_error + ")";
    }
    bool trespasser = false;
    match.diagnostics.push_back("volume \"" + image.volume_id() + "\", " + std::to_string(image.files().size()) + " files");
    for (const char* marker : kTrespasserMarkers) {
        const IsoFile* f = image.Find(marker);
        if (!f) continue;
        trespasser = true;
        match.diagnostics.push_back(std::string(marker) + ": " + std::to_string(f->size) + " bytes, SHA-256 " + digest_of(marker));
    }
    if (!near_miss.empty()) {
        match.status = InstallStatus::kDamagedDisc;
        why = "this is " + near_miss;
    } else if (trespasser) {
        match.status = InstallStatus::kUnsupportedBuild;
        why = "Unsupported Trespasser build (volume \"" + image.volume_id() + "\"); its fingerprints are in the log";
    } else {
        why = "this is not a Trespasser disc image (volume \"" + image.volume_id() + "\")";
    }
    return match;
}

bool ActivateStaging(const fs::path& staging, const fs::path& dir, std::string& error,
                     const std::function<void(const std::string&)>& log) {
    std::error_code ec;
    const fs::path backup = BackupFolder(dir);
    const bool replacing = fs::exists(dir, ec);
    if (replacing) {
        Remove(backup);
        fs::rename(dir, backup, ec);
        if (ec) {
            error = "the version folder is in use (close anything using " + Utf8(dir) + ")";
            return false;
        }
    } else if (dir.has_parent_path()) {
        fs::create_directories(dir.parent_path(), ec);
    }
    fs::rename(staging, dir, ec);
    if (ec) {
        error = "the new installation could not be moved into place";
        if (replacing) {
            std::error_code restore;
            fs::rename(backup, dir, restore);
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

InstallResult InstallFromIso(const fs::path& iso, const fs::path& game_root, const InstallHooks& hooks,
                             std::span<const GameProfile> profiles) {
    std::string why;
    std::error_code ec;
    if (hooks.phase) hooks.phase("Reading the disc image...");
    IsoImage image;
    if (!image.Open(iso, why)) return Fail(InstallStatus::kNotAnImage, why);
    Say(hooks, "disc image " + Utf8(iso) + ": volume \"" + image.volume_id() + "\", " +
                   std::to_string(image.files().size()) + " files, " + (image.joliet() ? "Joliet names" : "ISO names"));
    if (hooks.phase) hooks.phase("Identifying the game version...");
    DiscMatch match = IdentifyDisc(image, profiles, why);
    if (!match.profile) {
        for (const std::string& line : match.diagnostics) Say(hooks, "unsupported disc: " + line);
        InstallResult r = Fail(match.status, why);
        r.diagnostics = std::move(match.diagnostics);
        return r;
    }
    const GameProfile& profile = *match.profile;
    Say(hooks, std::string("identified ") + match.disc->edition + ": " + profile.label() + " (" + profile.id() + ")");

    if (!hooks.replace) {
        for (const InstalledVersion& v : FindInstalledVersions(game_root, profiles)) {
            if (v.profile != &profile || v.check.state != GameState::kReady) continue;
            InstallResult r;
            r.status = InstallStatus::kUpToDate;
            r.profile = &profile;
            r.disc = match.disc;
            r.dir = v.dir;
            r.detail = std::string(profile.label()) + " is already installed in " + Utf8(v.dir);
            return r;
        }
    }

    const fs::path target = VersionFolder(game_root, profile);
    if (Inside(iso, target)) return Fail(InstallStatus::kInsideGameFolder, Utf8(iso));
    Cabinet cabinet;
    if (!OpenCabinet(image, profile, cabinet, why)) return Fail(InstallStatus::kDamagedDisc, why);
    const std::vector<PlannedFile> plan = PlanInstall(image, profile.cabinet ? &cabinet : nullptr, profile, why);
    if (plan.empty()) return Fail(InstallStatus::kWrongDisc, why);
    uint64_t total = 0;
    for (const PlannedFile& f : plan) {
        if (!f.present())
            return Fail(InstallStatus::kNoGuestImageSource,
                        std::string("the disc lacks ") + f.file->source + ", the file the " +
                            (f.file->role == FileRole::kStringTable ? "game text" : "guest image") + " is made from");
        total += SourceSize(*f.file);
    }

    const fs::path staging = StagingFolder(target);
    Remove(staging);
    fs::create_directories(staging, ec);
    if (ec) return Fail(InstallStatus::kExtractFailed, "cannot create " + Utf8(staging));
    const fs::space_info space = fs::space(staging, ec);
    if (!ec && space.available < total + kSpaceMargin) {
        Remove(staging);
        return Fail(InstallStatus::kNoSpace, std::to_string((total + kSpaceMargin) >> 20) + " MB needed, " +
                                                 std::to_string(space.available >> 20) + " MB free");
    }

    if (hooks.phase) hooks.phase("Copying the game files...");
    uint64_t done = 0;
    std::vector<const InstallFile*> damaged;
    for (const PlannedFile& f : plan) {
        const fs::path to = staging / PathFromUtf8(f.file->installed);
        fs::create_directories(to.parent_path(), ec);
        std::string sha;
        bool copied;
        if (f.file->role == FileRole::kStringTable) {
            std::vector<uint8_t> bytes, table;
            copied = ReadSource(image, cabinet, f, bytes, why);
            if (copied) {
                done += bytes.size();
                if (hooks.progress && !hooks.progress(done, total)) {
                    why = "cancelled";
                    copied = false;
                } else if (Digest(bytes) != f.file->source_sha256) {
                    damaged.push_back(f.file);
                    continue;
                }
            }
            copied = copied && DeriveStringTable(bytes, staging, table, why) && WriteBytes(to, table, why);
            if (copied) sha = Digest(table);
        } else if (f.disc) {
            copied = image.Extract(*f.disc, to, [&](uint64_t n) {
                done += n;
                return !hooks.progress || hooks.progress(done, total);
            }, why, &sha);
        } else {
            std::vector<uint8_t> bytes;
            copied = cabinet.Read(*f.cabinet, bytes, why) && WriteBytes(to, bytes, why);
            if (copied) {
                sha = Digest(bytes);
                done += bytes.size();
                if (hooks.progress && !hooks.progress(done, total)) {
                    why = "cancelled";
                    copied = false;
                }
            }
        }
        if (!copied) {
            Remove(staging);
            return Fail(why == "cancelled" ? InstallStatus::kCancelled : InstallStatus::kExtractFailed, why);
        }
        if (sha != f.file->sha256) damaged.push_back(f.file);
    }

    size_t repaired = 0;
    std::string unrepaired;
    for (const InstallFile* f : damaged) {
        Say(hooks, std::string(f->installed) + " on the disc differs from the pressed file (damaged copy)");
        if (Repair(game_root, profiles, staging, *f, hooks)) {
            ++repaired;
        } else {
            unrepaired += unrepaired.empty() ? "" : ", ";
            unrepaired += f->installed;
        }
    }
    if (!unrepaired.empty()) {
        Remove(staging);
        return Fail(InstallStatus::kDamagedDisc,
                    "the image is damaged: " + unrepaired +
                        " differ from the pressed files. Use another copy of the disc, or install another version "
                        "first: its identical files repair them");
    }

    if (hooks.phase) hooks.phase("Checking the installed files...");
    const GameCheck check = CheckGameFolder(staging, profile);
    if (check.state != GameState::kReady) {
        Remove(staging);
        return Fail(InstallStatus::kValidationFailed, check.detail);
    }
    if (hooks.progress && !hooks.progress(total, total)) {
        Remove(staging);
        return Fail(InstallStatus::kCancelled, "cancelled");
    }

    if (hooks.phase) hooks.phase("Activating the installation...");
    if (!ActivateStaging(staging, target, why, hooks.log)) {
        Remove(staging);
        return Fail(InstallStatus::kActivateFailed, why);
    }
    InstallResult result;
    result.status = InstallStatus::kInstalled;
    result.profile = &profile;
    result.disc = match.disc;
    result.dir = target;
    result.files = plan.size();
    result.repaired = repaired;
    result.bytes = total;
    result.detail = std::string(match.disc->edition) + ", " + profile.label();
    return result;
}

const char* InstallStatusTitle(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled: return "Game installed";
    case InstallStatus::kUpToDate: return "Already installed";
    case InstallStatus::kCancelled: return "Installation cancelled";
    case InstallStatus::kNotAnImage: return "Not a disc image";
    case InstallStatus::kWrongDisc: return "Unsupported disc";
    case InstallStatus::kUnsupportedBuild: return "Unsupported Trespasser build";
    case InstallStatus::kDamagedDisc: return "Damaged disc image";
    case InstallStatus::kNoGuestImageSource: return "The disc lacks a file the game needs";
    case InstallStatus::kInsideGameFolder: return "Image inside the game folder";
    case InstallStatus::kNoSpace: return "Not enough disk space";
    case InstallStatus::kExtractFailed: return "Copy failed";
    case InstallStatus::kValidationFailed: return "The copied files are not valid";
    case InstallStatus::kActivateFailed: return "Could not activate the installation";
    }
    return "";
}

}  // namespace jpt
