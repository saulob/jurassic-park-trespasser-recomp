#include "level_link.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_hash.h"
#include "level_ini.h"
#include "updater/update_paths.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr uint64_t kLinkLimit = 64u << 10;
constexpr size_t kDepthMax = 8, kFoldersMax = 5000, kSceneDepth = 3;

struct Entry {
    fs::path path;
    std::string name;
    bool folder = false;
};

// Plain files and folders, never a link or a reparse point.
std::vector<Entry> Entries(const fs::path& folder) {
    std::vector<Entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        const fs::file_status status = it->symlink_status(type_ec);
        if (type_ec || IsReparsePoint(it->path(), status) || !(fs::is_directory(status) || fs::is_regular_file(status))) continue;
        entries.push_back({it->path(), Utf8(it->path().filename()), fs::is_directory(status)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return LowerAscii(a.name) < LowerAscii(b.name); });
    return entries;
}

bool HoldsScene(const fs::path& folder, size_t depth) {
    for (const Entry& e : Entries(folder))
        if (e.folder ? depth > 0 && HoldsScene(e.path, depth - 1) : IsScenePath(e.name)) return true;
    return false;
}

// Its scenes, its data folder or its mod settings are right here, as the corpus walker tells packages apart.
bool IsPackage(const std::vector<Entry>& entries) {
    for (const Entry& e : entries) {
        const std::string name = LowerAscii(e.name);
        if (e.folder ? name == "data" && HoldsScene(e.path, 0) : IsScenePath(name) || name == "fm.ini" || name == "tp_mod.ini") return true;
    }
    return false;
}

const Entry* FileNamed(const std::vector<Entry>& entries, const char* name) {
    for (const Entry& e : entries)
        if (!e.folder && SameText(e.name, name)) return &e;
    return nullptr;
}

bool ReadText(const fs::path& file, uint64_t limit, std::string& text) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    if (ec || size > limit || !in) return false;
    text.resize(size_t(size));
    return !size || bool(in.read(text.data(), std::streamsize(size)));
}

std::string DisplayName(const fs::path& folder) {
    const std::string name = Utf8(folder.filename()), lower = LowerAscii(name);
    if ((lower == "data" || lower == "game" || lower == "level" || lower == "levels") && !folder.parent_path().filename().empty())
        return Utf8(folder.parent_path().filename());
    return name;
}

bool SafeFolder(const std::string& text, std::string& folder) { return !text.empty() && trg::update::NormalizeRelativePath(text, folder); }

bool PlainFolder(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_directory(status) && !IsReparsePoint(path, status);
}

// Each fan mission of the tp_mod.ini tree at home with the folders it shares with the others (UberModPaths, ModPaths).
std::vector<LinkCandidate> FanMissions(const fs::path& home, const std::string& relative_home, const std::string& ini_name) {
    std::string text, fm_path;
    if (!ReadText(home / PathFromUtf8(ini_name), kLinkLimit, text)) return {};
    const IniFile ini = ParseIni(text);
    const IniEntry* fm_entry = ini.Find("Paths", "FMPath");
    if (fm_entry && !TrimmedPath(fm_entry->value).empty() && !SafeFolder(TrimmedPath(fm_entry->value), fm_path)) return {};
    std::vector<std::string> shared;
    for (const char* key : {"UberModPaths", "ModPaths"})
        if (const IniEntry* entry = ini.Find("Paths", key))
            for (const std::string& part : SplitPlus(entry->value)) {
                std::string folder;
                if (SafeFolder(part, folder) && PlainFolder(home / PathFromUtf8(folder))) shared.push_back(folder);
            }
    const fs::path base = fm_path.empty() ? home : home / PathFromUtf8(fm_path);
    std::vector<LinkCandidate> found;
    for (const Entry& e : Entries(base)) {
        const std::string relative = fm_path.empty() ? e.name : fm_path + "/" + e.name;
        const bool is_shared = std::any_of(shared.begin(), shared.end(), [&relative](const std::string& s) { return SameText(s, relative); });
        if (!e.folder || is_shared || SameText(e.name, "data") || !HoldsScene(e.path, kSceneDepth)) continue;
        LinkCandidate candidate;
        candidate.link.root = home;
        candidate.link.scope = {relative, ini_name};
        candidate.link.scope.insert(candidate.link.scope.end(), shared.begin(), shared.end());
        candidate.link.active_fm = relative;
        candidate.link.name = e.name;
        candidate.path = relative_home.empty() ? relative : relative_home + "/" + relative;
        found.push_back(std::move(candidate));
    }
    return found;
}

class Finder {
public:
    explicit Finder(std::vector<std::string>& notes) : notes_(notes) {}

    void Visit(const fs::path& folder, const std::string& relative, size_t depth) {
        if (++folders_ > kFoldersMax || depth > kDepthMax) {
            Note("folders too deep or too many were not searched");
            return;
        }
        const std::vector<Entry> entries = Entries(folder);
        if (const Entry* ini = FileNamed(entries, "tp_mod.ini")) {
            std::vector<LinkCandidate> missions = FanMissions(folder, relative, ini->name);
            if (!missions.empty()) {
                for (LinkCandidate& c : missions) found_.push_back(std::move(c));
                return;
            }
        }
        if (IsPackage(entries)) {
            LinkCandidate candidate;
            candidate.link.root = folder;
            candidate.link.name = DisplayName(folder);
            candidate.path = relative.empty() ? "." : relative;
            found_.push_back(std::move(candidate));
            return;
        }
        for (const Entry& e : entries) {
            const std::string type = PathType(e.name);
            if (e.folder) Visit(e.path, relative.empty() ? e.name : relative + "/" + e.name, depth + 1);
            else if (type == ".zip" || type == ".7z" || type == ".rar") ++archives_;
        }
    }

    std::vector<LinkCandidate> Take() {
        if (archives_) Note(std::to_string(archives_) + " archive(s) are not linked: import them with Import ZIP / 7z / RAR");
        return std::move(found_);
    }

private:
    void Note(const std::string& text) {
        if (std::find(notes_.begin(), notes_.end(), text) == notes_.end()) notes_.push_back(text);
    }

    std::vector<std::string>& notes_;
    std::vector<LinkCandidate> found_;
    size_t folders_ = 0, archives_ = 0;
};

std::string ComparableFolder(const fs::path& folder) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(folder, ec);
    std::string text = Utf8((ec ? folder : canonical).lexically_normal());
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\')) text.pop_back();
#ifdef _WIN32
    std::replace(text.begin(), text.end(), '/', '\\');
    text = LowerAscii(text);
#endif
    return text;
}

std::set<std::string> LowerSet(const std::vector<std::string>& items) {
    std::set<std::string> out;
    for (const std::string& item : items) out.insert(LowerAscii(item));
    return out;
}

bool OneLine(const std::string& text) { return text.find_first_of("\r\n") == std::string::npos; }

} // namespace

std::string PackageLinkText(const PackageLink& link) {
    std::string text = std::string(JPT_CUSTOM_LEVEL_LINK_HEAD) + "\npath " + Utf8(link.root) + "\n";
    for (const std::string& part : link.scope) text += "scope " + part + "\n";
    if (!link.active_fm.empty()) text += "fm " + link.active_fm + "\n";
    if (!link.name.empty()) text += "name " + link.name + "\n";
    if (!link.identity.empty()) text += "identity " + link.identity + "\n";
    return text;
}

bool ParsePackageLink(const std::string& text, PackageLink& link) {
    std::istringstream in(text);
    std::string line;
    PackageLink out;
    bool head = true, path = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (head) {
            if (line != JPT_CUSTOM_LEVEL_LINK_HEAD) return false;
            head = false;
            continue;
        }
        const size_t space = line.find(' ');
        const std::string kind = line.substr(0, space), value = space == std::string::npos ? std::string() : line.substr(space + 1);
        std::string normalized;
        if (kind == "path" && !value.empty()) {
            out.root = PathFromUtf8(value);
            path = out.root.is_absolute();
        } else if (kind == "scope") {
            if (!trg::update::NormalizeRelativePath(value, normalized) || out.scope.size() == JPT_CUSTOM_LEVEL_LINK_SCOPES_MAX) return false;
            out.scope.push_back(normalized);
        } else if (kind == "fm") {
            if (!trg::update::NormalizeRelativePath(value, normalized)) return false;
            out.active_fm = normalized;
        } else if (kind == "name") {
            out.name = value;
        } else if (kind == "identity") {
            out.identity = value;
        }
    }
    if (head || !path) return false;
    link = std::move(out);
    return true;
}

bool ReadPackageLink(const fs::path& records, PackageLink& link) {
    const fs::path file = records / JPT_CUSTOM_LEVEL_LINK_FILE;
    std::error_code ec;
    std::string text;
    return fs::is_regular_file(fs::symlink_status(file, ec)) && ReadText(file, kLinkLimit, text) && ParsePackageLink(text, link);
}

bool WritePackageLink(const fs::path& records, const PackageLink& link) {
    if (!OneLine(Utf8(link.root)) || !OneLine(link.name) || !OneLine(link.active_fm) || !OneLine(link.identity) ||
        !std::all_of(link.scope.begin(), link.scope.end(), OneLine) || link.scope.size() > JPT_CUSTOM_LEVEL_LINK_SCOPES_MAX)
        return false;
    const fs::path file = records / JPT_CUSTOM_LEVEL_LINK_FILE;
    fs::path temporary = file;
    temporary += ".new";
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(records, ec)) || fs::is_symlink(fs::symlink_status(file, ec))) return false;
    fs::create_directories(records, ec);
    {
        const std::string text = PackageLinkText(link);
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    fs::rename(temporary, file, ec);
    if (ec) fs::remove(temporary, ec);
    return !ec;
}

bool LinkAvailable(const PackageLink& link) { return !link.root.empty() && PlainFolder(link.root); }

bool SameLink(const PackageLink& a, const PackageLink& b) {
    std::error_code ec;
    const bool same_folder = fs::equivalent(a.root, b.root, ec);
    if (!(ec ? ComparableFolder(a.root) == ComparableFolder(b.root) : same_folder)) return false;
    return LowerSet(a.scope) == LowerSet(b.scope) && LowerAscii(a.active_fm) == LowerAscii(b.active_fm);
}

std::string LinkIdentity(const std::vector<PackageFile>& files) {
    std::vector<std::string> scenes;
    for (const PackageFile& file : files)
        if (IsScenePath(file.path)) scenes.push_back(LowerAscii(file.path));
    std::sort(scenes.begin(), scenes.end());
    std::string text;
    for (const std::string& scene : scenes) text += scene + "\n";
    char hex[65];
    jpt_sha256_buffer_hex(text.data(), text.size(), hex);
    return hex;
}

bool GameCanOpen(const fs::path& folder) {
    char native[JPT_PATH_MAX];
    return jpt_fs_from_utf8(Utf8(folder).c_str(), native, sizeof native) != 0;
}

bool IsReparsePoint(const fs::path& path, const fs::file_status& status) {
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

std::vector<LinkCandidate> FindLinkCandidates(const fs::path& folder, std::vector<std::string>& notes) {
    Finder finder(notes);
    if (PlainFolder(folder)) finder.Visit(folder, "", 0);
    return finder.Take();
}

} // namespace jpt
