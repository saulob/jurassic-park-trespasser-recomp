#include "custom_levels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>

#include "jpt_custom_level.h"
#include "jpt_szdd.h"
#include "updater/update_paths.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr char kStagingPrefix[] = ".import-";
constexpr char kRemovingPrefix[] = ".removing-";
constexpr uint64_t kSpareBytes = 64ull << 20;
constexpr uint64_t kTextLimit = 8ull << 20;
constexpr uint64_t kSceneLimit = 64ull << 20;
// The game opens files by ANSI paths of at most 259 characters.
constexpr size_t kHostPathLimit = 250;

std::string Lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return lower;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string FolderOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string Extension(const std::string& path) {
    const std::string name = BaseName(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : Lower(name.substr(dot));
}

std::string Stem(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

template <size_t N>
bool OneOf(const std::string& text, const char* const (&list)[N]) {
    return std::any_of(std::begin(list), std::end(list), [&text](const char* item) { return text == item; });
}

bool IsScene(const std::string& path) { return Extension(path) == ".scn"; }

// 0: not instructions.
int ReadmeScore(const std::string& path) {
    static constexpr const char* kTypes[] = {"", ".txt", ".md", ".markdown", ".nfo", ".1st", ".me"};
    static constexpr const char* kReadme[] = {"readme", "read me", "read_me", "read-me", "leiame", "leia-me", "leia_me", "leia me", "lisezmoi", "lisez-moi", "liesmich", "leeme"};
    static constexpr const char* kGuides[] = {"install", "instruc", "instalac", "how to", "howto", "how_to", "how-to", "walkthrough", "manual", "guide"};
    const std::string name = Lower(BaseName(path)), type = Extension(path);
    const std::string stem = type.empty() ? name : Stem(name);
    int score = 0;
    if (!OneOf(type, kTypes)) return 0;
    for (const char* word : kReadme) {
        if (stem == word) score = std::max(score, 100);
        else if (stem.find(word) != std::string::npos) score = std::max(score, 90);
    }
    if (type == ".me" && stem == "read") score = 100;
    if (type == ".1st") score = std::max(score, 80);
    if (type.empty()) return score;
    for (const char* word : kGuides)
        if (stem.find(word) != std::string::npos) score = std::max(score, stem.find("install") != std::string::npos ? 70 : 60);
    static constexpr const char* kOther[] = {"change", "license", "licence", "copying", "credits", "history", "todo", "version", "log"};
    for (const char* word : kOther)
        if (!score && stem.find(word) != std::string::npos) return 0;
    return score ? score : 20;
}

bool ReadBytes(const fs::path& file, uint64_t limit, std::string& bytes) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    if (ec || size > limit || !in) return false;
    bytes.resize(size_t(size));
    if (size) in.read(bytes.data(), std::streamsize(size));
    return bool(in);
}

bool IsTrespasserFile(const std::string& bytes) {
    static constexpr unsigned char kMagic[] = {0xBE, 0xBA, 0xCE, 0x0A};
    return bytes.size() >= 64 && !std::memcmp(bytes.data(), kMagic, sizeof kMagic);
}

// The names of the world files a scene loads: up to five 256-byte fields, each a path of the author's machine.
std::vector<std::string> WorldFiles(const std::string& scene) {
    constexpr size_t kField = 256, kFields = 5;
    std::vector<std::string> names;
    const std::string lower = Lower(scene);
    const size_t end = lower.find(".grf");
    if (end == std::string::npos) return names;
    size_t start = end;
    while (start > 0 && end - start < kField && scene[start - 1] >= ' ' && scene[start - 1] < 127) --start;
    for (size_t i = 0; i < kFields && start + i * kField < scene.size(); ++i) {
        const std::string field = scene.substr(start + i * kField, kField);
        const std::string path = field.substr(0, field.find('\0'));
        const size_t slash = path.find_last_of("\\/");
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (Extension(name) != ".grf") break;
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> PackageFiles(const fs::path& dir) {
    ArchiveError error;
    std::vector<std::string> files;
    if (const auto reader = OpenFolder(dir, {}, error))
        for (const ArchiveEntry& entry : reader->entries())
            if (!entry.directory && !entry.link) files.push_back(entry.name);
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return files;
}

bool HasFile(const std::vector<std::string>& files, const std::string& folder, const std::string& name) {
    const std::string beside = Lower(folder + name), wanted = Lower(name);
    size_t named = 0;
    for (const std::string& file : files) {
        const std::string lower = Lower(file);
        if (lower == beside) return true;
        named += BaseName(lower) == wanted;
    }
    return named == 1;
}

bool InGame(const fs::path& data_dir, const std::string& name) {
    std::error_code ec;
    if (data_dir.empty()) return false;
    for (fs::directory_iterator it(data_dir, ec), end; !ec && it != end; it.increment(ec))
        if (Lower(Utf8(it->path().filename())) == Lower(name)) return true;
    return false;
}

LevelScene InspectScene(const fs::path& dir, const std::string& path, const std::vector<std::string>& files, const fs::path& data_dir) {
    LevelScene scene;
    scene.path = path;
    scene.file = BaseName(path);
    std::string bytes;
    if (!ReadBytes(dir / PathFromUtf8(path), kSceneLimit, bytes)) return scene;
    const auto* raw = reinterpret_cast<const uint8_t*>(bytes.data());
    if (jpt_szdd_is(raw, bytes.size())) {
        size_t size = 0;
        uint8_t* expanded = jpt_szdd_expand(raw, bytes.size(), &size);
        bytes.assign(reinterpret_cast<const char*>(expanded), expanded ? size : 0);
        std::free(expanded);
    }
    scene.valid = IsTrespasserFile(bytes);
    if (!scene.valid) return scene;
    const std::string folder = FolderOf(path);
    const std::vector<std::string> worlds = WorldFiles(bytes);
    scene.loadable = !worlds.empty();
    if (worlds.empty()) scene.missing.push_back(Stem(scene.file) + ".grf");
    for (const std::string& world : worlds) {
        const bool own = HasFile(files, folder, world), original = !own && InGame(data_dir, world);
        if (!own && !original) {
            scene.missing.push_back(world);
            scene.loadable = false;
            continue;
        }
        if (original) continue;
        const std::string textures = Stem(world) + "-130";
        if (!HasFile(files, folder, textures + ".pid")) scene.missing.push_back(textures + ".pid");
        if (!HasFile(files, folder, textures + ".swp") && !HasFile(files, folder, textures + ".spz") &&
            !HasFile(files, folder, textures + ".spp"))
            scene.missing.push_back(textures + ".swp");
    }
    return scene;
}

bool MentionsWord(const std::string& lower_text, const std::string& word) {
    const auto letter = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (size_t at = lower_text.find(word); at != std::string::npos; at = lower_text.find(word, at + 1)) {
        const size_t after = at + word.size();
        if ((at == 0 || !letter(lower_text[at - 1])) && (after == lower_text.size() || !letter(lower_text[after]))) return true;
    }
    return false;
}

std::string PackageId(const std::string& wanted) {
    std::string id;
    for (const char c : wanted) {
        const bool kept = std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128;
        id += kept || c == '.' || c == '_' || c == '-' || c == ' ' ? c : '_';
    }
    id.erase(0, std::min(id.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"), id.size()));
    id = id.substr(0, JPT_CUSTOM_LEVEL_ID_MAX - 4);
    while (!id.empty() && (id.back() == '.' || id.back() == ' ')) id.pop_back();
    std::string normalized;
    return trg::update::NormalizeRelativePath(id, normalized) ? id : std::string("level");
}

struct PlannedFile {
    size_t index;
    std::string path;
    uint64_t size;
};

ImportStatus FromArchive(ArchiveError error) {
    switch (error) {
    case ArchiveError::kCancelled: return ImportStatus::kCancelled;
    case ArchiveError::kUnreadable: return ImportStatus::kUnreadable;
    case ArchiveError::kUnsupported: return ImportStatus::kUnsupported;
    case ArchiveError::kTooLarge:
    case ArchiveError::kOutOfMemory: return ImportStatus::kTooLarge;
    case ArchiveError::kNone:
    case ArchiveError::kDamaged: break;
    }
    return ImportStatus::kDamaged;
}

bool PlanFiles(const ArchiveReader& reader, const ArchiveLimits& limits, std::vector<PlannedFile>& plan, ImportResult& result) {
    std::set<std::string> files, folders;
    uint64_t total = 0;
    const auto refuse = [&result](ImportStatus status, const std::string& detail) {
        result.status = status;
        result.detail = detail;
        return false;
    };
    for (size_t i = 0; i < reader.entries().size(); ++i) {
        const ArchiveEntry& entry = reader.entries()[i];
        std::string path, reason;
        if (!trg::update::NormalizeRelativePath(entry.name, path, &reason))
            return refuse(ImportStatus::kUnsafe, reason);
        if (entry.link) return refuse(ImportStatus::kUnsafe, path);
        if (std::count(path.begin(), path.end(), '/') > JPT_CUSTOM_LEVEL_DEPTH_MAX) return refuse(ImportStatus::kUnsafe, path);
        const std::string key = Lower(path);
        for (size_t slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
            folders.insert(key.substr(0, slash));
        if (entry.directory) {
            folders.insert(key);
            continue;
        }
        if (!files.insert(key).second) return refuse(ImportStatus::kUnsafe, path);
        if (!IsLevelPackageFile(path)) {
            ++result.skipped;
            continue;
        }
        if (entry.encrypted) return refuse(ImportStatus::kUnsupported, path);
        if (entry.size > limits.max_file_bytes || total > limits.max_total_bytes - entry.size)
            return refuse(ImportStatus::kTooLarge, path);
        total += entry.size;
        plan.push_back({i, path, entry.size});
    }
    for (const std::string& file : files)
        if (folders.count(file)) return refuse(ImportStatus::kUnsafe, file);
    if (std::none_of(plan.begin(), plan.end(), [](const PlannedFile& f) { return IsScene(f.path); }))
        return refuse(ImportStatus::kNoLevel, "");
    return true;
}

// Several levels of their own side by side: one import would merge them.
bool HoldsSeveralPackages(const std::vector<PlannedFile>& plan, const ArchiveReader& reader) {
    std::set<std::string> tops;
    for (const PlannedFile& f : plan) {
        if (!IsScene(f.path)) continue;
        const size_t slash = f.path.find('/');
        if (slash == std::string::npos) return false;
        tops.insert(Lower(f.path.substr(0, slash)));
    }
    const bool archives = std::any_of(reader.entries().begin(), reader.entries().end(), [](const ArchiveEntry& e) {
        const std::string type = Extension(e.name);
        return !e.directory && e.name.find('/') == std::string::npos && (type == ".zip" || type == ".7z" || type == ".rar");
    });
    return tops.size() > 1 || (archives && !tops.empty());
}

struct SizedFile {
    std::string path;
    uint64_t size = 0;
    bool operator==(const SizedFile& other) const { return size == other.size; }
};
using FileSizes = std::map<std::string, SizedFile>; // by lower-case path

FileSizes SizesOf(const fs::path& dir) {
    ArchiveError error;
    FileSizes sizes;
    if (const auto reader = OpenFolder(dir, {}, error))
        for (const ArchiveEntry& entry : reader->entries())
            if (!entry.directory) sizes[Lower(entry.name)] = {entry.name, entry.size};
    return sizes;
}

bool SameFiles(const fs::path& a, const FileSizes& in_a, const fs::path& b, const FileSizes& in_b) {
    if (in_a != in_b) return false;
    for (const auto& [key, file] : in_a) {
        std::ifstream x(a / PathFromUtf8(file.path), std::ios::binary);
        std::ifstream y(b / PathFromUtf8(in_b.at(key).path), std::ios::binary);
        std::vector<char> p(1 << 18), q(1 << 18);
        if (!x || !y) return false;
        while (x && y) {
            x.read(p.data(), std::streamsize(p.size()));
            y.read(q.data(), std::streamsize(q.size()));
            if (x.gcount() != y.gcount() || std::memcmp(p.data(), q.data(), size_t(x.gcount()))) return false;
        }
    }
    return true;
}

std::vector<std::string> InstalledIds(const fs::path& levels) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (fs::directory_iterator it(levels, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        const std::string id = Utf8(it->path().filename());
        if (fs::is_directory(it->symlink_status(type_ec)) && jpt_custom_level_id_valid(id.c_str())) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return ids;
}

bool Rename(const fs::path& from, const fs::path& to) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

class Staging {
public:
    explicit Staging(const fs::path& levels) {
        std::error_code ec;
        fs::create_directories(levels, ec);
        for (int i = 0; i < 100 && dir_.empty(); ++i) {
            const fs::path candidate = levels / (kStagingPrefix + std::to_string(i));
            if (fs::create_directory(candidate, ec)) dir_ = candidate;
        }
    }
    ~Staging() {
        std::error_code ec;
        if (!dir_.empty()) fs::remove_all(dir_, ec);
    }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;

    const fs::path& dir() const { return dir_; }
    bool Publish(const fs::path& target) {
        if (!Rename(dir_, target)) return false;
        dir_.clear();
        return true;
    }

private:
    fs::path dir_;
};

ArchiveError Unpack(ArchiveReader& reader, const PlannedFile& file, const fs::path& target, const ImportHooks& hooks,
                    uint64_t& done, uint64_t total, bool& write_failed) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    uint64_t written = 0;
    bool cancelled = false;
    const ArchiveError error = reader.Extract(file.index, [&](const uint8_t* data, size_t size) {
        if (written + size > file.size) return false;
        out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        written += size;
        done += size;
        cancelled = hooks.progress && !hooks.progress(done, total);
        write_failed = !out;
        return !cancelled && !write_failed;
    });
    out.close();
    if (cancelled) return ArchiveError::kCancelled;
    if (write_failed || !out) {
        write_failed = true;
        return ArchiveError::kUnreadable;
    }
    if (error == ArchiveError::kCancelled || (error == ArchiveError::kNone && written != file.size)) return ArchiveError::kDamaged;
    return error;
}

ImportResult Import(ArchiveReader& reader, const std::string& wanted_id, bool folder, const fs::path& user_dir, const ImportHooks& hooks,
                    const ArchiveLimits& limits) {
    ImportResult result;
    std::vector<PlannedFile> plan;
    const auto fail = [&result](ImportStatus status, const std::string& detail) {
        result.status = status;
        result.detail = detail;
        return result;
    };
    if (!PlanFiles(reader, limits, plan, result)) return result;
    if (folder && HoldsSeveralPackages(plan, reader)) return fail(ImportStatus::kSeveralPackages, "");
    const fs::path levels = CustomLevelsDir(user_dir);
    uint64_t total = 0, done = 0;
    for (const PlannedFile& file : plan) {
        total += file.size;
#ifdef _WIN32
        if (levels.native().size() + JPT_CUSTOM_LEVEL_ID_MAX + PathFromUtf8(file.path).native().size() + 2 > kHostPathLimit)
            return fail(ImportStatus::kUnsafe, file.path);
#endif
    }
    Staging staging(levels);
    if (staging.dir().empty()) return fail(ImportStatus::kWriteFailed, Utf8(levels));
    std::error_code ec;
    const fs::space_info space = fs::space(levels, ec);
    if (!ec && space.available < total + kSpareBytes) return fail(ImportStatus::kNoSpace, "");
    if (hooks.phase) hooks.phase("Copying the level files...");
    for (const PlannedFile& file : plan) {
        const fs::path target = staging.dir() / PathFromUtf8(file.path);
        bool write_failed = false;
        if (!trg::update::PathIsWithin(target, staging.dir())) return fail(ImportStatus::kUnsafe, file.path);
        const ArchiveError error = Unpack(reader, file, target, hooks, done, total, write_failed);
        if (write_failed) return fail(ImportStatus::kWriteFailed, file.path);
        if (error != ArchiveError::kNone) return fail(FromArchive(error), error == ArchiveError::kCancelled ? std::string() : file.path);
    }
    if (hooks.phase) hooks.phase("Checking the level files...");
    const FileSizes sizes = SizesOf(staging.dir());
    if (sizes.size() != plan.size()) return fail(ImportStatus::kWriteFailed, Utf8(levels));
    for (const PlannedFile& file : plan) {
        const auto found = sizes.find(Lower(file.path));
        if (found == sizes.end() || found->second.size != file.size) return fail(ImportStatus::kWriteFailed, file.path);
    }
    bool any_scene = false;
    for (const PlannedFile& file : plan)
        any_scene = any_scene || (IsScene(file.path) && InspectScene(staging.dir(), file.path, {}, {}).valid);
    if (!any_scene) return fail(ImportStatus::kNoLevel, "");
    const std::vector<std::string> installed = InstalledIds(levels);
    for (const std::string& id : installed) {
        if (!SameFiles(staging.dir(), sizes, levels / id, SizesOf(levels / id))) continue;
        result.package = id;
        return fail(ImportStatus::kAlreadyInstalled, id);
    }
    const auto taken = [&levels](const std::string& id) {
        std::error_code exists_ec;
        return fs::exists(fs::symlink_status(levels / id, exists_ec));
    };
    std::string id = PackageId(wanted_id);
    for (int n = 2; taken(id) && n < 100; ++n) id = PackageId(wanted_id) + "-" + std::to_string(n);
    if (!jpt_custom_level_id_valid(id.c_str()) || taken(id) || !staging.Publish(levels / id))
        return fail(ImportStatus::kWriteFailed, id);
    result.status = ImportStatus::kInstalled;
    result.package = id;
    result.files = plan.size();
    return result;
}

std::string FolderName(const fs::path& folder) {
    fs::path path = folder;
    if (path.filename().empty()) path = path.parent_path();
    const std::string name = Utf8(path.filename());
    const std::string lower = Lower(name);
    if ((lower == "data" || lower == "game" || lower == "level" || lower == "levels") && !path.parent_path().filename().empty())
        return Utf8(path.parent_path().filename());
    return name;
}

} // namespace

const char* ImportStatusTitle(ImportStatus status) {
    switch (status) {
    case ImportStatus::kInstalled: return "Custom level installed";
    case ImportStatus::kAlreadyInstalled: return "This level is already installed";
    case ImportStatus::kCancelled: return "Import cancelled. Nothing was changed.";
    case ImportStatus::kUnreadable: return "The files could not be read";
    case ImportStatus::kUnsupported: return "This archive is not supported. Extract it and use Import Folder";
    case ImportStatus::kDamaged: return "The archive is damaged";
    case ImportStatus::kUnsafe: return "The package holds an unsafe path or file";
    case ImportStatus::kTooLarge: return "The package is too large";
    case ImportStatus::kNoLevel: return "No Trespasser level (.scn) was found";
    case ImportStatus::kSeveralPackages: return "This folder holds several levels. Choose the folder of one of them";
    case ImportStatus::kNoSpace: return "There is not enough free disk space";
    case ImportStatus::kWriteFailed: return "The level could not be written";
    }
    return "";
}

fs::path CustomLevelsDir(const fs::path& user_dir) { return user_dir / JPT_CUSTOM_LEVELS_DIR; }

bool IsLevelPackageFile(const std::string& relative_path) {
    static constexpr const char* kTypes[] = {
        ".scn", ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".asa", ".asb", ".tpa", ".sky", ".ddf", ".smk", ".bmp", ".tga", ".dds",
        ".png", ".jpg", ".jpeg", ".gif", ".pcx", ".wav", ".mp3", ".ogg", ".txt", ".md", ".markdown", ".nfo", ".1st", ".me", ".pdf",
        ".rtf", ".ini", ".cfg", ".csv"};
    const std::string type = Extension(relative_path);
    return type.empty() ? ReadmeScore(relative_path) > 0 : OneOf(type, kTypes);
}

ImportResult ImportLevelArchive(const fs::path& archive, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits) {
    ArchiveError error = ArchiveError::kNone;
    if (hooks.phase) hooks.phase("Reading the package...");
    const std::unique_ptr<ArchiveReader> reader = OpenArchive(archive, limits, error);
    if (!reader) {
        ImportResult result;
        result.status = FromArchive(error);
        return result;
    }
    return Import(*reader, Utf8(archive.stem()), false, user_dir, hooks, limits);
}

ImportResult ImportLevelFolder(const fs::path& folder, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits) {
    ImportResult result;
    ArchiveError error = ArchiveError::kNone;
    if (hooks.phase) hooks.phase("Reading the package...");
    std::error_code ec;
    const bool overlaps = trg::update::PathIsWithin(folder, CustomLevelsDir(user_dir)) ||
                          trg::update::PathIsWithin(CustomLevelsDir(user_dir), folder);
    if (!fs::is_directory(folder, ec) || overlaps) {
        result.status = overlaps ? ImportStatus::kUnsafe : ImportStatus::kUnreadable;
        return result;
    }
    const std::unique_ptr<ArchiveReader> reader = OpenFolder(folder, limits, error);
    if (!reader) {
        result.status = FromArchive(error);
        return result;
    }
    return Import(*reader, FolderName(folder), true, user_dir, hooks, limits);
}

void CleanLevelStaging(const fs::path& user_dir) {
    std::error_code ec;
    std::vector<fs::path> leftovers;
    for (fs::directory_iterator it(CustomLevelsDir(user_dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = Utf8(it->path().filename());
        if (name.rfind(kStagingPrefix, 0) == 0 || name.rfind(kRemovingPrefix, 0) == 0) leftovers.push_back(it->path());
    }
    for (const fs::path& path : leftovers) fs::remove_all(path, ec);
}

std::vector<LevelPackage> ListLevelPackages(const fs::path& user_dir, const fs::path& data_dir) {
    std::vector<LevelPackage> packages;
    for (const std::string& id : InstalledIds(CustomLevelsDir(user_dir))) {
        LevelPackage package;
        package.id = id;
        package.dir = CustomLevelsDir(user_dir) / id;
        const std::vector<std::string> files = PackageFiles(package.dir);
        for (const std::string& file : files)
            if (IsScene(file) && package.scenes.size() < JPT_CUSTOM_LEVEL_SCENES_MAX)
                package.scenes.push_back(InspectScene(package.dir, file, files, data_dir));
        package.readme = ChooseReadme(files);
        std::string text;
        if (!package.readme.empty() && ReadLevelText(package.dir / PathFromUtf8(package.readme), text)) {
            text = Lower(text);
            package.mentions_atx = MentionsWord(text, "atx");
            package.mentions_ce = MentionsWord(text, "trespasser ce") || MentionsWord(text, "tresce") || MentionsWord(text, "ce patch");
        }
        packages.push_back(std::move(package));
    }
    return packages;
}

bool DeleteLevelPackage(const fs::path& user_dir, const std::string& id, std::string& error) {
    const fs::path levels = CustomLevelsDir(user_dir);
    std::error_code ec;
    if (!jpt_custom_level_id_valid(id.c_str()) || !fs::is_directory(fs::symlink_status(levels / id, ec)) ||
        !trg::update::PathIsWithin(levels / id, levels)) {
        error = "not an installed package";
        return false;
    }
    const fs::path removing = levels / (kRemovingPrefix + id);
    fs::remove_all(removing, ec);
    if (!Rename(levels / id, removing)) {
        error = "a file of the package is in use";
        return false;
    }
    fs::remove_all(removing, ec);
    return true;
}

std::string ChooseReadme(const std::vector<std::string>& relative_paths) {
    std::string best;
    int best_score = 0;
    const auto depth = [](const std::string& path) { return std::count(path.begin(), path.end(), '/'); };
    for (const std::string& path : relative_paths) {
        const int score = ReadmeScore(path);
        if (!score || score < best_score) continue;
        const bool better = score > best_score || depth(path) < depth(best) ||
                            (depth(path) == depth(best) && (path.size() < best.size() || (path.size() == best.size() && path < best)));
        if (!better) continue;
        best = path;
        best_score = score;
    }
    return best;
}

bool ResolveReadme(const fs::path& user_dir, const LevelPackage& package, fs::path& file) {
    const fs::path home = CustomLevelsDir(user_dir) / package.id;
    const fs::path candidate = package.dir / PathFromUtf8(package.readme);
    std::error_code ec;
    if (package.readme.empty() || !ReadmeScore(package.readme) || !jpt_custom_level_id_valid(package.id.c_str()) ||
        package.dir.lexically_normal() != home.lexically_normal() || !trg::update::PathIsWithin(candidate, home) ||
        !fs::is_regular_file(fs::symlink_status(candidate, ec))) {
        return false;
    }
    file = candidate;
    return true;
}

std::string DecodeText(const std::string& bytes) {
    const auto at = [&bytes](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    std::string text;
    const bool little = bytes.size() >= 2 && at(0) == 0xFF && at(1) == 0xFE, big = bytes.size() >= 2 && at(0) == 0xFE && at(1) == 0xFF;
    if (little || big) {
        std::u16string wide;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) wide += char16_t(little ? at(i) | at(i + 1) << 8 : at(i) << 8 | at(i + 1));
        for (char16_t& c : wide)
            if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        text = Utf8(fs::path(wide));
    } else if (bytes.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
        text = IsUtf8(bytes) ? bytes.substr(3) : FromWindows1252(bytes.substr(3));
    } else {
        text = IsUtf8(bytes) ? bytes : FromWindows1252(bytes);
    }
    std::string clean;
    clean.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            if (i + 1 == text.size() || text[i + 1] != '\n') clean += '\n';
        } else if (c >= 0x20 || c == '\n' || c == '\t') {
            if (c != 0x7F) clean += char(c);
        }
    }
    return clean;
}

bool ReadLevelText(const fs::path& file, std::string& text) {
    std::string bytes;
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec)) || !ReadBytes(file, kTextLimit, bytes)) return false;
    text = DecodeText(bytes);
    return true;
}

} // namespace jpt
