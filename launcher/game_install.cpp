#include "game_install.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

bool SamePlace(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

// The install empties the version folder's staging and backup folders before it copies: a source there would be lost.
bool WithinInstallPlaces(const fs::path& source, const fs::path& target) {
    for (const fs::path& place : {target, StagingFolder(target), BackupFolder(target)})
        if (Inside(source, place) || Inside(place, source) || SamePlace(source, place)) return true;
    return false;
}

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
    return text;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Whether a '/' path ends with that path, at a folder boundary.
bool EndsWithPath(const std::string& path, const std::string& tail) {
    if (tail.empty() || path.size() < tail.size() || path.compare(path.size() - tail.size(), tail.size(), tail)) return false;
    return path.size() == tail.size() || path[path.size() - tail.size() - 1] == '/';
}

std::optional<InstallResult> AlreadyInstalled(const fs::path& game_root, std::span<const GameProfile> profiles,
                                              const GameProfile& profile, const InstallHooks& hooks) {
    if (hooks.replace) return std::nullopt;
    for (const InstalledVersion& v : FindInstalledVersions(game_root, profiles)) {
        if (v.profile != &profile || v.check.state != GameState::kReady) continue;
        InstallResult r;
        r.status = InstallStatus::kUpToDate;
        r.profile = &profile;
        r.dir = v.dir;
        r.detail = std::string(profile.label()) + " is already installed in " + Utf8(v.dir);
        return r;
    }
    return std::nullopt;
}

// Where a plan's files come from: a disc image with its cabinet, or folders.
class PlanSource {
public:
    using Progress = std::function<bool(uint64_t bytes)>;
    virtual ~PlanSource() = default;
    virtual bool Extract(const PlannedFile& f, const fs::path& to, const Progress& progress, std::string& sha,
                         std::string& error) = 0;
    virtual bool ReadAll(const PlannedFile& f, std::vector<uint8_t>& bytes, std::string& error) = 0;
};

bool ExtractCabinet(const Cabinet& cabinet, const CabinetFile& file, const fs::path& to, const PlanSource::Progress& progress,
                    std::string& sha, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!cabinet.Read(file, bytes, error) || !WriteBytes(to, bytes, error)) return false;
    sha = Digest(bytes);
    if (progress && !progress(bytes.size())) {
        error = "cancelled";
        return false;
    }
    return true;
}

class IsoSource final : public PlanSource {
public:
    IsoSource(IsoImage& image, const Cabinet& cabinet) : image_(image), cabinet_(cabinet) {}
    bool Extract(const PlannedFile& f, const fs::path& to, const Progress& progress, std::string& sha,
                 std::string& error) override {
        return f.disc ? image_.Extract(*f.disc, to, progress, error, &sha) : ExtractCabinet(cabinet_, *f.cabinet, to, progress, sha, error);
    }
    bool ReadAll(const PlannedFile& f, std::vector<uint8_t>& bytes, std::string& error) override {
        return f.disc ? image_.ReadAll(*f.disc, bytes, error) : cabinet_.Read(*f.cabinet, bytes, error);
    }

private:
    IsoImage& image_;
    const Cabinet& cabinet_;
};

bool CopyHashed(const fs::path& from, const fs::path& to, const PlanSource::Progress& progress, std::string& sha,
                std::string& error) {
    std::ifstream in(from, std::ios::binary);
    PartFileWriter out;
    if (!in) {
        error = "cannot read " + Utf8(from);
        return false;
    }
    if (!out.Open(to)) {
        error = "cannot create " + Utf8(to);
        return false;
    }
    std::vector<char> buffer(1 << 20);
    jpt_sha256_t digest;
    jpt_sha256_init(&digest);
    for (;;) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        const size_t n = size_t(in.gcount());
        if (!n) break;
        if (!out.Write(buffer.data(), n)) {
            error = "cannot write " + Utf8(to) + " (disk full?)";
            return false;
        }
        jpt_sha256_update(&digest, buffer.data(), n);
        if (progress && !progress(n)) {
            error = "cancelled";
            return false;
        }
    }
    if (in.bad() || !in.eof() || !out.Commit()) {
        error = "cannot copy " + Utf8(from);
        return false;
    }
    char hex[65];
    jpt_sha256_hex(&digest, hex);
    sha = hex;
    return true;
}

bool IsReparse(const fs::path& path, const fs::file_status& status) {
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

struct IndexedFile {
    fs::path path;
    std::string relative;  // lower case, '/' separated, below its chosen folder
    std::string name;      // lower case
    uint64_t size = 0;
    size_t folder = 0;     // the chosen folder it is in, the first one first
};

// Every plain file below the chosen folders, never through a link or a reparse point.
class FolderIndex {
public:
    bool Build(const std::vector<fs::path>& folders, std::string& why) {
        for (size_t i = 0; i < folders.size(); ++i) {
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(folders[i], ec);
            if (ec || !fs::is_directory(status) || IsReparse(folders[i], status)) {
                why = "not a folder that can be read: " + Utf8(folders[i]);
                return false;
            }
            Walk(folders[i], "", i, 0);
        }
        return true;
    }

    const std::vector<IndexedFile>& files() const { return files_; }
    bool truncated() const { return truncated_; }

    // Empty when it cannot be read.
    const std::string& DigestOf(const IndexedFile& file) {
        const auto [it, added] = digests_.try_emplace(Utf8(file.path));
        if (added) it->second = Sha256File(file.path);
        return it->second;
    }

    // The profile's cabinet among the files, opened once; nullptr when there is none.
    const Cabinet* CabinetOf(const GameProfile& profile) {
        if (!profile.cabinet) return nullptr;
        const auto [it, added] = cabinets_.try_emplace(profile.cabinet);
        if (!added) return it->second.get();
        const std::string wanted = Lowered(profile.cabinet), name = BaseName(wanted);
        for (const IndexedFile& file : files_) {
            if (file.name != name || file.size > kCabinetLimit) continue;
            std::ifstream in(file.path, std::ios::binary);
            std::vector<uint8_t> bytes(size_t(file.size));
            std::string error;
            auto cabinet = std::make_unique<Cabinet>();
            if (in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) && cabinet->Open(std::move(bytes), error)) {
                it->second = std::move(cabinet);
                break;
            }
        }
        return it->second.get();
    }

private:
    static constexpr int kDepthMax = 8;
    static constexpr size_t kFilesMax = 60000;
    static constexpr uint64_t kCabinetLimit = 512ull << 20;

    void Walk(const fs::path& dir, const std::string& relative, size_t folder, int depth) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            const fs::file_status status = it->symlink_status(type_ec);
            if (type_ec || IsReparse(it->path(), status)) continue;
            const std::string name = Lowered(Utf8(it->path().filename()));
            const std::string below = relative.empty() ? name : relative + "/" + name;
            if (fs::is_directory(status)) {
                if (depth < kDepthMax) Walk(it->path(), below, folder, depth + 1);
                else truncated_ = true;
            } else if (fs::is_regular_file(status)) {
                if (files_.size() == kFilesMax) {
                    truncated_ = true;
                    return;
                }
                std::error_code size_ec;
                const uintmax_t size = it->file_size(size_ec);
                if (!size_ec) files_.push_back({it->path(), below, name, size, folder});
            }
        }
    }

    std::vector<IndexedFile> files_;
    std::map<std::string, std::string> digests_;
    std::map<std::string, std::unique_ptr<Cabinet>> cabinets_;
    bool truncated_ = false;
};

// The copy with the pressed bytes. A data file goes by its name and size, the disc's or the installation's own place
// first; an executable by its size and bytes alone, since an installation renames it. seen: one of its name is there.
const IndexedFile* FindCopy(FolderIndex& index, const InstallFile& f, bool& seen) {
    const uint64_t size = SourceSize(f);
    const char* sha = f.role == FileRole::kStringTable ? f.source_sha256 : f.sha256;
    const std::string source = Lowered(f.source), installed = Lowered(f.installed);
    const std::string names[] = {BaseName(source), BaseName(installed)};
    const std::string data_relative = installed.rfind("data/", 0) == 0 ? installed.substr(5) : installed;
    std::vector<const IndexedFile*> candidates;
    for (const IndexedFile& file : index.files()) {
        const bool named = file.name == names[0] || file.name == names[1];
        seen = seen || named;
        if (file.size == size && (named || f.role != FileRole::kData)) candidates.push_back(&file);
    }
    const auto conventional = [&](const IndexedFile* file) {
        return EndsWithPath(file->relative, source) || EndsWithPath(file->relative, installed) || file->relative == data_relative;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const IndexedFile* a, const IndexedFile* b) {
        return std::make_pair(a->folder, !conventional(a)) < std::make_pair(b->folder, !conventional(b));
    });
    for (const IndexedFile* file : candidates)
        if (index.DigestOf(*file) == sha) return file;
    return nullptr;
}

const CabinetFile* FindInCabinet(FolderIndex& index, const GameProfile& profile, const InstallFile& f) {
    const Cabinet* cabinet = f.kind == SourceKind::kCabinet ? index.CabinetOf(profile) : nullptr;
    const CabinetFile* found = cabinet ? cabinet->Find(f.source) : nullptr;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!found || found->size != SourceSize(f) || !cabinet->Read(*found, bytes, error)) return nullptr;
    return Digest(bytes) == (f.role == FileRole::kStringTable ? f.source_sha256 : f.sha256) ? found : nullptr;
}

bool HasIdentity(FolderIndex& index, const GameProfile& profile) {
    const std::string name = Lowered(BaseName(profile.identity_file));
    for (const IndexedFile& file : index.files())
        if (file.name == name && index.DigestOf(file) == profile.identity_sha256) return true;
    return false;
}

FolderMatch PlanFolders(FolderIndex& index, const GameProfile& profile) {
    FolderMatch m;
    m.profile = &profile;
    for (const InstallFile& f : profile.files) {
        PlannedFile planned{&f, nullptr, nullptr, {}};
        bool seen = false;
        if (const IndexedFile* copy = FindCopy(index, f, seen)) planned.folder_file = copy->path;
        else planned.cabinet = FindInCabinet(index, profile, f);
        if (!planned.present()) {
            m.missing.push_back(f.installed);
            if (seen) m.modified.push_back(f.installed);
        }
        m.plan.push_back(std::move(planned));
    }
    return m;
}

struct KnownExecutable {
    const char* sha256;
    const char* what;
};

// Retail executables the port does not run (docs/GAME_VERSIONS.md); named so the log says why they are not used.
constexpr KnownExecutable kOtherExecutables[] = {
    {"9c2f46098307fac2a3689616657c615f67a2113584f09290d5e489e538991816", "the retail Pentium build (tpassp5.exe)"},
    {"701701f7b6bedd263bc36c4ed3e2379aed743af929075c6490b22138ce6acbd3", "the retail AMD K6 build (tpassk6.exe)"},
    {"7b17f534b982e4797c33d8d1047d944bc660e0859cdd4567558e7649ff57bf61", "the official Patch 1.1 executable"},
};

std::vector<std::string> DescribeExecutables(FolderIndex& index, std::span<const GameProfile> profiles) {
    constexpr uint64_t kSmallest = 1ull << 20, kLargest = 16ull << 20;
    constexpr size_t kMost = 16;
    std::vector<std::string> lines;
    for (const IndexedFile& file : index.files()) {
        if (file.name.size() < 4 || file.name.compare(file.name.size() - 4, 4, ".exe") || file.size < kSmallest || file.size > kLargest)
            continue;
        if (lines.size() == kMost) break;
        const std::string& sha = index.DigestOf(file);
        std::string what;
        for (const GameProfile& p : profiles)
            for (const InstallFile& f : p.files) {
                if (what.empty() && f.role == FileRole::kGuestImage && sha == f.sha256)
                    what = std::string("the retail executable of ") + p.label() + ": the source of its guest image";
                if (what.empty() && f.role == FileRole::kStringTable && sha == f.source_sha256)
                    what = std::string("the executable of ") + p.label() + ": the source of its game text";
            }
        for (const KnownExecutable& known : kOtherExecutables)
            if (what.empty() && sha == known.sha256) what = std::string(known.what) + ": not used, the port runs the retail tpassp6.exe";
        if (what.empty()) what = "not a retail executable this port knows (" + std::to_string(file.size) + " bytes, SHA-256 " + sha + "): not used";
        lines.push_back(file.relative + ": " + what);
    }
    return lines;
}

std::string Listed(const std::vector<std::string>& names, size_t most) {
    std::string text;
    for (size_t i = 0; i < names.size() && i < most; ++i) text += (i ? ", " : "") + names[i];
    if (names.size() > most) text += " and " + std::to_string(names.size() - most) + " more";
    return text;
}

std::string IncompleteText(const FolderMatch& m) {
    const bool executable = std::find(m.missing.begin(), m.missing.end(), std::string(JPT_GUEST_IMAGE)) != m.missing.end();
    std::string text = std::string(m.profile->label()) + ": " + std::to_string(m.missing.size()) + " of its files are not in these folders (" +
                       Listed(m.missing, 6) + ")";
    if (!m.modified.empty()) text += "; " + std::to_string(m.modified.size()) + " of them are there with other contents (patched or modified)";
    if (executable) text += ". The retail setup/tpassp6.exe of the disc is needed for the game itself";
    return text + ". Add the folder of the original disc to complete it";
}

FolderMatch Identify(FolderIndex& index, const std::vector<fs::path>& folders, std::span<const GameProfile> profiles, std::string& why) {
    FolderMatch best;
    if (folders.empty() || !index.Build(folders, why)) {
        if (why.empty()) why = "no folder was chosen";
        return best;
    }
    std::vector<std::string> diagnostics = {std::to_string(index.files().size()) + " files in " + std::to_string(folders.size()) + " folder(s)" +
                                            (index.truncated() ? ", more were not read (too many or too deep)" : "")};
    for (std::string& line : DescribeExecutables(index, profiles)) diagnostics.push_back(std::move(line));
    for (const GameProfile& p : profiles) {
        if (!HasIdentity(index, p)) continue;
        FolderMatch m = PlanFolders(index, p);
        if (!best.profile || m.missing.size() < best.missing.size()) best = std::move(m);
    }
    if (!best.profile) {
        bool trespasser = false;
        for (const char* marker : kTrespasserMarkers)
            for (const IndexedFile& file : index.files()) {
                if (file.name != BaseName(marker)) continue;
                trespasser = true;
                diagnostics.push_back(file.relative + ": " + std::to_string(file.size) + " bytes, SHA-256 " + index.DigestOf(file));
            }
        best.status = trespasser ? InstallStatus::kUnsupportedBuild : InstallStatus::kNotAnInstallation;
        why = trespasser ? "these folders hold no known version's data files: an unsupported Trespasser build; its fingerprints are in the log"
                         : "no Trespasser game files were found in these folders";
    } else if (!best.missing.empty()) {
        best.status = InstallStatus::kIncompleteInstallation;
        why = IncompleteText(best);
    }
    best.diagnostics = std::move(diagnostics);
    return best;
}

class FolderSource final : public PlanSource {
public:
    FolderSource(FolderIndex& index, const GameProfile& profile) : index_(index), profile_(profile) {}
    bool Extract(const PlannedFile& f, const fs::path& to, const Progress& progress, std::string& sha,
                 std::string& error) override {
        if (!f.folder_file.empty()) return CopyHashed(f.folder_file, to, progress, sha, error);
        const Cabinet* cabinet = index_.CabinetOf(profile_);
        return cabinet && f.cabinet && ExtractCabinet(*cabinet, *f.cabinet, to, progress, sha, error);
    }
    bool ReadAll(const PlannedFile& f, std::vector<uint8_t>& bytes, std::string& error) override {
        if (f.folder_file.empty()) {
            const Cabinet* cabinet = index_.CabinetOf(profile_);
            return cabinet && f.cabinet && cabinet->Read(*f.cabinet, bytes, error);
        }
        std::error_code ec;
        const uintmax_t size = fs::file_size(f.folder_file, ec);
        std::ifstream in(f.folder_file, std::ios::binary);
        bytes.resize(ec ? 0 : size_t(size));
        if (ec || !in || !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
            error = "cannot read " + Utf8(f.folder_file);
            return false;
        }
        return true;
    }

private:
    FolderIndex& index_;
    const GameProfile& profile_;
};

struct Destination {
    fs::path game_root, target;
    std::span<const GameProfile> profiles;
};

// Copies the plan into the staging folder beside the target, repairs, checks and swaps it in; anything else leaves the
// installed versions as they were.
InstallResult InstallPlan(PlanSource& source, const std::vector<PlannedFile>& plan, const GameProfile& profile,
                          const Destination& to, uint64_t total, const InstallHooks& hooks, InstallStatus damaged_status,
                          const std::function<std::string(const std::string&)>& damaged_text) {
    std::string why;
    std::error_code ec;
    const fs::path staging = StagingFolder(to.target);
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
    const auto advance = [&](uint64_t n) {
        done += n;
        return !hooks.progress || hooks.progress(done, total);
    };
    std::vector<const InstallFile*> damaged;
    for (const PlannedFile& f : plan) {
        const fs::path destination = staging / PathFromUtf8(f.file->installed);
        fs::create_directories(destination.parent_path(), ec);
        std::string sha;
        bool copied;
        if (f.file->role == FileRole::kStringTable) {
            std::vector<uint8_t> bytes, table;
            copied = source.ReadAll(f, bytes, why);
            if (copied) {
                if (!advance(bytes.size())) {
                    why = "cancelled";
                    copied = false;
                } else if (Digest(bytes) != f.file->source_sha256) {
                    damaged.push_back(f.file);
                    continue;
                }
            }
            copied = copied && DeriveStringTable(bytes, staging, table, why) && WriteBytes(destination, table, why);
            if (copied) sha = Digest(table);
        } else {
            copied = source.Extract(f, destination, advance, sha, why);
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
        Say(hooks, std::string(f->installed) + " differs from the pressed file (damaged copy)");
        if (Repair(to.game_root, to.profiles, staging, *f, hooks)) {
            ++repaired;
        } else {
            unrepaired += unrepaired.empty() ? "" : ", ";
            unrepaired += f->installed;
        }
    }
    if (!unrepaired.empty()) {
        Remove(staging);
        return Fail(damaged_status, damaged_text(unrepaired));
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
    if (!ActivateStaging(staging, to.target, why, hooks.log)) {
        Remove(staging);
        return Fail(InstallStatus::kActivateFailed, why);
    }
    InstallResult result;
    result.status = InstallStatus::kInstalled;
    result.profile = &profile;
    result.dir = to.target;
    result.files = plan.size();
    result.repaired = repaired;
    result.bytes = total;
    return result;
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
        PlannedFile p{&f, nullptr, nullptr, {}};
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

    if (std::optional<InstallResult> installed = AlreadyInstalled(game_root, profiles, profile, hooks)) {
        installed->disc = match.disc;
        return *installed;
    }

    const fs::path target = VersionFolder(game_root, profile);
    if (WithinInstallPlaces(iso, target)) return Fail(InstallStatus::kInsideGameFolder, Utf8(iso));
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

    IsoSource source(image, cabinet);
    const Destination to{game_root, target, profiles};
    InstallResult result = InstallPlan(source, plan, profile, to, total, hooks, InstallStatus::kDamagedDisc, [](const std::string& list) {
        return "the image is damaged: " + list +
               " differ from the pressed files. Use another copy of the disc, or install another version first: its "
               "identical files repair them";
    });
    if (!result.ok()) return result;
    result.disc = match.disc;
    result.detail = std::string(match.disc->edition) + ", " + profile.label();
    return result;
}

FolderMatch IdentifyFolders(const std::vector<fs::path>& folders, std::span<const GameProfile> profiles, std::string& why) {
    FolderIndex index;
    return Identify(index, folders, profiles, why);
}

InstallResult InstallFromFolders(const std::vector<fs::path>& folders, const fs::path& game_root, const InstallHooks& hooks,
                                 std::span<const GameProfile> profiles) {
    std::string why;
    if (hooks.phase) hooks.phase("Reading the folders...");
    for (const fs::path& folder : folders) Say(hooks, "game folder " + Utf8(folder));
    if (hooks.phase) hooks.phase("Identifying the game version...");
    FolderIndex index;
    FolderMatch match = Identify(index, folders, profiles, why);
    for (const std::string& line : match.diagnostics) Say(hooks, "game folder: " + line);
    if (!match.profile) {
        InstallResult r = Fail(match.status, why);
        r.diagnostics = std::move(match.diagnostics);
        return r;
    }
    const GameProfile& profile = *match.profile;
    Say(hooks, std::string("identified ") + profile.label() + " (" + profile.id() + ")");
    if (std::optional<InstallResult> installed = AlreadyInstalled(game_root, profiles, profile, hooks)) return *installed;
    if (!match.complete()) {
        InstallResult r = Fail(InstallStatus::kIncompleteInstallation, why);
        r.profile = &profile;
        r.missing = match.missing;
        r.diagnostics = std::move(match.diagnostics);
        for (const std::string& name : r.missing) Say(hooks, "game folder: missing " + name);
        return r;
    }

    const fs::path target = VersionFolder(game_root, profile);
    for (const fs::path& folder : folders)
        if (WithinInstallPlaces(folder, target)) return Fail(InstallStatus::kInsideGameFolder, Utf8(folder));
    uint64_t total = 0;
    for (const PlannedFile& f : match.plan) total += SourceSize(*f.file);
    FolderSource source(index, profile);
    const Destination to{game_root, target, profiles};
    InstallResult result = InstallPlan(source, match.plan, profile, to, total, hooks, InstallStatus::kExtractFailed, [](const std::string& list) {
        return list + " changed while they were copied. Nothing was installed; import the folder again";
    });
    if (result.ok()) result.detail = std::string(profile.label()) + ", from " + std::to_string(folders.size()) + " folder(s)";
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
    case InstallStatus::kNotAnInstallation: return "No Trespasser game files in this folder";
    case InstallStatus::kIncompleteInstallation: return "The installation is incomplete";
    }
    return "";
}

}  // namespace jpt
