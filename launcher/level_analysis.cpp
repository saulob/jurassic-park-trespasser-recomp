#include "level_analysis.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>

#include "archive_reader.h"
#include "jpt_custom_level.h"
#include "jpt_level_features.h"
#include "jpt_szdd.h"
#include "level_picture.h"
#include "updater/update_paths.h"
#include "utf8.h"

namespace jpt {
namespace {

constexpr uint64_t kIniLimit = 1ull << 20;
constexpr uint64_t kReadmeLimit = 8ull << 20;
constexpr uint64_t kSceneLimit = 64ull << 20;
constexpr uint64_t kSeveral = 64;

template <size_t N>
bool OneOf(const std::string& text, const char* const (&list)[N]) {
    return std::any_of(std::begin(list), std::end(list), [&text](const char* item) { return text == item; });
}

std::string Join(const std::string& folder, const std::string& rest) { return folder.empty() ? rest : rest.empty() ? folder : folder + "/" + rest; }

bool Under(const std::string& path, const std::string& folder) {
    return folder.empty() || (path.size() > folder.size() && path[folder.size()] == '/' && path.compare(0, folder.size(), folder) == 0);
}

std::string Below(const std::string& path, const std::string& folder) { return folder.empty() ? path : path.substr(folder.size() + 1); }

std::string FirstSegment(const std::string& path) { return path.substr(0, path.find('/')); }

size_t Depth(const std::string& path) { return size_t(std::count(path.begin(), path.end(), '/')); }

bool IsTrespasserFile(const std::string& bytes) {
    static constexpr unsigned char kMagic[] = {0xBE, 0xBA, 0xCE, 0x0A};
    return bytes.size() >= 64 && !std::memcmp(bytes.data(), kMagic, sizeof kMagic);
}

// The names of the world files a scene loads: up to five 256-byte fields, each a path of the author's machine.
std::vector<std::string> WorldFiles(const std::string& scene) {
    constexpr size_t kField = 256, kFields = 5;
    std::vector<std::string> names;
    const std::string lower = LowerAscii(scene);
    const size_t end = lower.find(".grf");
    if (end == std::string::npos) return names;
    size_t start = end;
    while (start > 0 && end - start < kField && scene[start - 1] >= ' ' && scene[start - 1] < 127) --start;
    for (size_t i = 0; i < kFields && start + i * kField < scene.size(); ++i) {
        const std::string field = scene.substr(start + i * kField, kField);
        const std::string path = field.substr(0, field.find('\0'));
        const size_t slash = path.find_last_of("\\/");
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (PathType(name) != ".grf") break;
        names.push_back(name);
    }
    return names;
}

bool IsRuntimeType(const std::string& path) {
    static constexpr const char* kTypes[] = {".scn", ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".asa", ".asb", ".tpa", ".sky", ".ddf",
                                             ".smk", ".bmp", ".tga", ".dds", ".png", ".jpg", ".jpeg", ".gif", ".pcx"};
    return OneOf(PathType(path), kTypes);
}

bool IsTpa(const std::string& head) { return head.size() >= 4 && !std::memcmp(head.data(), "\x50\x01\0\0", 4); }

std::string TrimmedPath(const std::string& text) {
    std::string path = TrimText(text);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

std::vector<std::string> SplitPlus(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('+', start), text.size());
        const std::string part = TrimmedPath(text.substr(start, end - start));
        if (!part.empty()) parts.push_back(part);
        start = end + 1;
    }
    return parts;
}

class Analyzer {
public:
    explicit Analyzer(const PackageSource& source) : source_(source) {}

    PackageAnalysis Run() {
        Index();
        ReadTexts();
        FindConfigs();
        ChooseRoot();
        BuildRuntime();
        FollowReadme();
        PlaceAnimations();
        PlaceTextures();
        ReadLevels();
        ReadFmIni();
        ReadAtxFiles();
        FinishLevels();
        ReadSettings();
        FindDependencies();
        FindFeatures();
        std::sort(a_.runtime.begin(), a_.runtime.end(), [](const RuntimeFile& x, const RuntimeFile& y) { return LowerAscii(x.target) < LowerAscii(y.target); });
        return std::move(a_);
    }

private:
    void Warn(const char* code, const std::string& detail) {
        for (const Warning& w : a_.warnings)
            if (w.code == code && w.detail == detail) return;
        if (a_.warnings.size() < kSeveral) a_.warnings.push_back({code, detail});
    }

    void Act(const char* kind, const std::string& from, const std::string& to, Origin origin, const std::string& reason) {
        if (a_.actions.size() < kSeveral * 4) a_.actions.push_back({kind, from, to, origin, reason});
    }

    void Index() {
        std::set<std::string> folders;
        for (const PackageFile& file : source_.files) {
            paths_.push_back(file.path);
            by_lower_[LowerAscii(file.path)].push_back(file.path);
            for (size_t slash = file.path.find('/'); slash != std::string::npos; slash = file.path.find('/', slash + 1))
                folders.insert(file.path.substr(0, slash));
        }
        folders_.assign(folders.begin(), folders.end());
        std::sort(paths_.begin(), paths_.end(), [](const std::string& x, const std::string& y) { return LowerAscii(x) < LowerAscii(y); });
        for (const auto& [lower, same] : by_lower_)
            if (same.size() > 1) Warn("case_collision", same[0] + " / " + same[1]);
    }

    bool ReadText(const std::string& path, uint64_t limit, std::string& text) {
        std::string bytes;
        if (!source_.read(path, limit, bytes)) return false;
        text = DecodeText(bytes);
        return true;
    }

    enum class Lookup { kFound, kMissing, kAmbiguous };

    Lookup Resolve(const std::string& path, std::string& found) const {
        const auto it = by_lower_.find(LowerAscii(path));
        if (it == by_lower_.end()) return Lookup::kMissing;
        for (const std::string& exact : it->second)
            if (exact == path) return found = exact, Lookup::kFound;
        if (it->second.size() > 1) return Lookup::kAmbiguous;
        found = it->second[0];
        return Lookup::kFound;
    }

    // A path an INI file names, as the engine searches it: in the mod's own folder, then in its data folder.
    Lookup ResolveInRoot(const std::string& relative, std::string& found) const {
        const Lookup first = Resolve(Join(a_.root, relative), found);
        return first == Lookup::kMissing ? Resolve(Join(a_.root, "data/" + relative), found) : first;
    }

    std::vector<std::string> FilesNamed(const char* name) const {
        std::vector<std::string> found;
        for (const std::string& path : paths_)
            if (SameText(PathName(path), name) && !IsDocumentation(path)) found.push_back(path);
        std::stable_sort(found.begin(), found.end(), [](const std::string& x, const std::string& y) { return Depth(x) < Depth(y); });
        return found;
    }

    bool HasScene(const std::string& folder) const {
        return std::any_of(paths_.begin(), paths_.end(), [&folder](const std::string& p) { return IsScenePath(p) && Under(p, folder); });
    }

    // The package's folder of that path, whatever the case; empty when there is none or more than one.
    std::string FolderAt(const std::string& path) const {
        std::string found;
        for (const std::string& folder : folders_) {
            if (!SameText(folder, path)) continue;
            if (!found.empty()) return {};
            found = folder;
        }
        return found;
    }

    void ReadTexts() {
        std::vector<std::pair<int, std::string>> readmes;
        for (const std::string& path : paths_)
            if (const int score = LevelReadmeScore(path)) readmes.push_back({score, path});
        std::stable_sort(readmes.begin(), readmes.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        a_.readme = ChooseReadme(paths_);
        for (const auto& [score, path] : readmes) {
            a_.readmes.push_back(path);
            std::string text;
            if (a_.readmes.size() > 16 || (score < 60 && path != a_.readme) || !ReadText(path, kReadmeLimit, text)) continue;
            Merge(ReadReadme(text, paths_, folders_));
        }
        for (const std::string& name : notes_.ambiguous) Warn("readme_not_followed", name);
        for (const std::string& name : notes_.absent) Warn("readme_file_absent", name);
        if (notes_.no_restart) Warn("no_restart", "the readme says Restart Level does not work in the original game; here it restarts the level whole");
    }

    void Merge(const ReadmeNotes& notes) {
        notes_.rules.insert(notes_.rules.end(), notes.rules.begin(), notes.rules.end());
        notes_.configs.insert(notes_.configs.end(), notes.configs.begin(), notes.configs.end());
        notes_.ambiguous.insert(notes_.ambiguous.end(), notes.ambiguous.begin(), notes.ambiguous.end());
        notes_.absent.insert(notes_.absent.end(), notes.absent.begin(), notes.absent.end());
        notes_.no_restart = notes_.no_restart || notes.no_restart;
        if (notes.ce > notes_.ce) notes_.ce = notes.ce, notes_.ce_evidence = notes.ce_evidence;
        if (notes.atx > notes_.atx) notes_.atx = notes.atx, notes_.atx_evidence = notes.atx_evidence;
    }

    void FindConfigs() {
        for (const std::string& path : FilesNamed("fm.ini")) a_.configs.push_back({SettingFile::kFmIni, path, false});
        for (const std::string& path : FilesNamed("tp_mod.ini")) a_.configs.push_back({SettingFile::kTpModIni, path, false});
        std::vector<std::string> engine;
        for (const std::string& path : paths_) {
            const std::string name = LowerAscii(PathName(path));
            if (name.rfind("trespasser.", 0) == 0 && PathType(name) == ".ini") engine.push_back(path);
        }
        std::stable_sort(engine.begin(), engine.end(), [](const std::string& x, const std::string& y) { return Depth(x) < Depth(y); });
        engine_ini_ = ChooseEngineIni(engine);
        for (const std::string& path : engine) a_.configs.push_back({SettingFile::kTrespasserIni, path, path == engine_ini_});
        for (const std::string& path : FilesNamed("tpass.ini")) a_.configs.push_back({SettingFile::kTpassIni, path, false});
    }

    std::string ChooseEngineIni(const std::vector<std::string>& engine) {
        for (const std::string& path : engine) {
            if (std::find(notes_.configs.begin(), notes_.configs.end(), path) == notes_.configs.end()) continue;
            Act("settings", path, "", Origin::kReadme, "the readme installs it as trespasser.ini: read for this package only, never copied");
            return path;
        }
        std::vector<std::string> plain;
        for (const std::string& path : engine)
            if (SameText(PathName(path), "trespasser.ini")) plain.push_back(path);
        const std::vector<std::string>& candidates = plain.empty() ? engine : plain;
        if (candidates.empty()) return {};
        if (candidates.size() > 1 && Depth(candidates[0]) == Depth(candidates[1])) {
            Warn("config_several_engine_files", candidates[0] + ", " + candidates[1]);
            return {};
        }
        Act("settings", candidates[0], "", Origin::kTrespasserIni, "read for this package only, never copied");
        return candidates[0];
    }

    void Use(SettingFile kind, const std::string& path) {
        for (ConfigFile& config : a_.configs)
            if (config.kind == kind && config.path == path) config.used = true;
    }

    bool SafeFolder(const std::string& text, std::string& normalized) const {
        return text.empty() ? (normalized.clear(), true) : trg::update::NormalizeRelativePath(text, normalized);
    }

    bool RootFromTpMod() {
        const std::vector<std::string> files = FilesNamed("tp_mod.ini");
        std::string text;
        if (files.empty()) return false;
        if (files.size() > 1) Warn("config_several_tp_mod", files[0] + ", " + files[1]);
        if (!ReadText(files[0], kIniLimit, text)) return Warn("config_unreadable", files[0]), false;
        const IniFile ini = ParseIni(text);
        const std::string home = PathFolder(files[0]);
        Use(SettingFile::kTpModIni, files[0]);
        const auto value = [&ini](const char* section, const char* key) {
            const IniEntry* entry = ini.Find(section, key);
            return entry ? TrimmedPath(entry->value) : std::string();
        };
        std::string fm_path, active;
        if (!SafeFolder(value("Paths", "FMPath"), fm_path) || !SafeFolder(value("FM", "ActiveFM"), active)) {
            Warn("config_unsafe_path", files[0]);
            return false;
        }
        Layers(ini.Find("Paths", "UberModPaths"), home, uber_, "UberModPaths");
        Layers(ini.Find("Paths", "ModPaths"), home, mods_, "ModPaths");
        if (active.empty()) return false;
        for (const std::string& candidate : {Join(Join(home, fm_path), active), Join(home, active)}) {
            const std::string folder = FolderAt(candidate);
            if (folder.empty() || !HasScene(folder)) continue;
            return SetRoot(folder, Origin::kTpModIni, "tp_mod.ini: ActiveFM=" + active + (fm_path.empty() ? "" : ", FMPath=" + fm_path));
        }
        std::vector<std::string> named;
        for (const std::string& folder : folders_)
            if (SameText(PathName(folder), PathName(active)) && HasScene(folder)) named.push_back(folder);
        // A copy of tp_mod.ini kept inside the fan mission it names.
        if (named.empty() && (home.empty() ? !FilesNamed("fm.ini").empty() && PathFolder(FilesNamed("fm.ini")[0]).empty() : SameText(PathName(home), PathName(active))) && HasScene(home)) named.push_back(home);
        if (named.size() == 1) return SetRoot(named[0], Origin::kTpModIni, "tp_mod.ini: ActiveFM=" + active);
        Warn("config_active_fm_missing", active);
        return false;
    }

    void Layers(const IniEntry* entry, const std::string& home, std::vector<std::string>& layers, const char* key) {
        if (!entry) return;
        for (const std::string& part : SplitPlus(entry->value)) {
            std::string normalized, folder;
            if (!SafeFolder(part, normalized)) {
                Warn("config_unsafe_path", std::string(key) + "=" + part);
                continue;
            }
            folder = FolderAt(Join(home, normalized));
            if (folder.empty()) Warn("config_mod_path_missing", std::string(key) + "=" + part);
            else layers.push_back(folder);
        }
    }

    bool SetRoot(const std::string& folder, Origin origin, const std::string& reason) {
        a_.root = folder;
        Act("root", folder, "", origin, reason);
        return root_set_ = true;
    }

    bool RootFromReadme() {
        std::set<std::string> roots;
        for (const ReadmeRule& rule : notes_.rules)
            if (rule.target == ReadmeTarget::kModRoot && !rule.folder.empty() && HasScene(rule.folder)) roots.insert(rule.folder);
        if (roots.size() != 1) return false;
        return SetRoot(*roots.begin(), Origin::kReadme, "the readme installs this folder as a fan mission");
    }

    bool RootFromFmIni() {
        std::vector<std::string> roots;
        for (const std::string& path : FilesNamed("fm.ini"))
            if (HasScene(PathFolder(path))) roots.push_back(PathFolder(path));
        if (roots.size() > 1) Warn("config_several_fan_missions", Join(roots[0], "fm.ini") + ", " + Join(roots[1], "fm.ini"));
        if (roots.size() != 1) return false;
        return SetRoot(roots[0], Origin::kFmIni, "the folder of fm.ini");
    }

    void RootFromScenes() {
        std::vector<std::string> folders;
        for (const std::string& path : paths_)
            if (IsScenePath(path)) folders.push_back(PathFolder(path));
        if (folders.empty()) return;
        std::stable_sort(folders.begin(), folders.end(), [](const std::string& x, const std::string& y) {
            const bool data_x = SameText(PathName(x), "data"), data_y = SameText(PathName(y), "data");
            return data_x != data_y ? data_x : Depth(x) < Depth(y);
        });
        loose_ = !SameText(PathName(folders[0]), "data");
        a_.layout = loose_ ? "loose" : "data_folder";
        SetRoot(loose_ ? folders[0] : PathFolder(folders[0]), Origin::kLayout, loose_ ? "the folder of the level files" : "the folder that holds data");
    }

    void ChooseRoot() {
        const bool tp_mod = RootFromTpMod();
        if (!tp_mod && !RootFromReadme() && !RootFromFmIni()) return RootFromScenes();
        const std::string ini = Join(a_.root, "fm.ini");
        std::string found;
        if (Resolve(ini, found) == Lookup::kFound) fm_ini_ = found, Use(SettingFile::kFmIni, found);
        const std::string home = tp_mod ? PathFolder(FilesNamed("tp_mod.ini")[0]) : std::string();
        const bool engine = std::any_of(paths_.begin(), paths_.end(), [&](const std::string& p) {
            return tp_mod && Under(p, Join(home, "data")) && !Under(p, a_.root) && PathType(p) == ".tpa";
        });
        a_.layout = engine ? "standalone" : tp_mod ? "mod_tree" : "fan_mission";
        if (engine) Act("ignore", Join(home, "data"), "", Origin::kTpModIni, "the package's own copy of the game: the installed game version is used instead");
    }

    // Below another mod of the package, or among what it carries for other uses.
    bool Foreign(const std::string& relative) const {
        static constexpr const char* kOther[] = {"mods", "fms", "doc", "docs", "source", "src", "tools", "backup"};
        return OneOf(LowerAscii(FirstSegment(relative)), kOther);
    }

    // "" when the file has no place in the game's folders.
    std::string TargetOf(const std::string& relative, bool data_itself, bool& direct) const {
        const std::string first = LowerAscii(FirstSegment(relative));
        const bool nested = relative.find('/') != std::string::npos;
        direct = true;
        if (!IsRuntimeType(relative)) return {};
        if (nested && first == "data" && !data_itself) return "data/" + relative.substr(5);
        direct = data_itself;
        if (nested && first == "maps") return "maps/" + relative.substr(5);
        if (nested && first == "map" && !data_itself) return "data/Map" + relative.substr(first.size());
        if (nested && (first == "anim" || first == "texhw" || first == "menu")) return "data/" + first + relative.substr(first.size());
        if (nested && (first == "animb" || first == "saves" || first == "screenshots")) return {};
        if (data_itself) return "data/" + relative;
        if (nested) return Foreign(relative) ? std::string() : relative;
        static constexpr const char* kLevelTypes[] = {".scn", ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".sky", ".asa", ".tpa"};
        return OneOf(PathType(relative), kLevelTypes) ? "data/" + relative : std::string();
    }

    RuntimeFile* RuntimeAt(const std::string& target) {
        const auto it = targets_.find(LowerAscii(target));
        return it == targets_.end() ? nullptr : &a_.runtime[it->second];
    }

    static bool Active(const std::string& target) {
        const std::string lower = LowerAscii(target), type = PathType(lower);
        return lower.rfind("data/menu/", 0) != 0 && lower != "data/menu.tpa" && (lower.rfind("data/texhw/", 0) == 0 || type != ".dds");
    }

    bool AddRuntime(const std::string& target, const std::string& from, Origin origin) {
        if (RuntimeFile* taken = RuntimeAt(target)) {
            if (taken->source != from && origin != Origin::kLayout) Warn("target_taken", target + ": " + from);
            return taken->source == from;
        }
        targets_[LowerAscii(target)] = a_.runtime.size();
        a_.runtime.push_back({target, from, origin, Active(target)});
        return true;
    }

    static bool IsTextureTarget(const std::string& target) {
        const std::string lower = LowerAscii(target);
        return lower.rfind("data/texhw/", 0) == 0 || lower.rfind("data/map/", 0) == 0;
    }

    // Trespasser CE looks in a mod's folder before its data folder; only the textures may be in both.
    void AddLayer(const std::string& root, bool data_itself) {
        enum { kRootTextures, kDirect, kOthers, kPasses };
        for (int pass = 0; pass < kPasses; ++pass) {
            for (const std::string& path : paths_) {
                bool direct = false;
                if (!Under(path, root)) continue;
                const std::string target = TargetOf(Below(path, root), data_itself, direct);
                const int of = direct ? kDirect : IsTextureTarget(target) ? kRootTextures : kOthers;
                if (!target.empty() && of == pass) AddRuntime(target, path, Origin::kLayout);
            }
        }
    }

    // The engine's own search order; the installed game comes last, through the package's fallback.
    void BuildRuntime() {
        if (!root_set_) return;
        for (const std::string& layer : uber_) {
            AddLayer(layer, false);
            Act("layer", layer, "", Origin::kTpModIni, "UberModPaths: before the active fan mission");
        }
        AddLayer(a_.root, loose_);
        for (const std::string& layer : mods_) {
            AddLayer(layer, false);
            Act("layer", layer, "", Origin::kTpModIni, "ModPaths: after the active fan mission");
        }
    }

    void MoveRuntime(const std::string& target, const ReadmeRule& rule) {
        for (size_t i = 0; i < a_.runtime.size(); ++i) {
            if (a_.runtime[i].source != rule.file || SameText(a_.runtime[i].target, target)) continue;
            targets_.erase(LowerAscii(a_.runtime[i].target));
            a_.runtime.erase(a_.runtime.begin() + ptrdiff_t(i));
            for (auto& [key, index] : targets_)
                if (index > i) --index;
            --i;
        }
        if (AddRuntime(target, rule.file, Origin::kReadme)) Act("move", rule.file, target, Origin::kReadme, rule.sentence);
    }

    void FollowReadme() {
        for (const ReadmeRule& rule : notes_.rules) {
            const std::string folder = ReadmeTargetFolder(rule.target);
            if (rule.target == ReadmeTarget::kModRoot) continue;
            if (!rule.file.empty()) {
                MoveRuntime(Join(folder, PathName(rule.file)), rule);
            } else if (rule.level_files) {
                Act("keep", a_.root, "data", Origin::kReadme, rule.sentence);
            } else {
                static constexpr const char* kKept[] = {"anim", "texhw", "menu"};
                const std::string name = LowerAscii(PathName(rule.folder));
                if (!OneOf(name, kKept)) continue;
                for (const std::string& path : paths_)
                    if (Under(path, rule.folder) && IsRuntimeType(path)) AddRuntime("data/" + name + "/" + Below(path, rule.folder), path, Origin::kReadme);
                Act("keep", rule.folder, "data/" + name, Origin::kReadme, rule.sentence);
            }
        }
    }

    void PlaceAnimations() {
        std::vector<std::string> beside;
        for (const RuntimeFile& file : a_.runtime)
            if (PathType(file.target) == ".asa" && Depth(file.target) == 1) beside.push_back(file.source);
        for (const std::string& path : beside) {
            const std::string target = "data/anim/" + PathName(path);
            if (RuntimeAt(target) || !AddRuntime(target, path, Origin::kHeuristic)) continue;
            Act("move", path, target, Origin::kHeuristic, "the game reads animation scripts from the anim folder only");
        }
        for (const char* name : {"anim", "texhw", "menu"})
            if (std::any_of(a_.runtime.begin(), a_.runtime.end(), [name](const RuntimeFile& f) { return Under(LowerAscii(f.target), std::string("data/") + name); }))
                a_.directories.push_back(name);
        if (std::any_of(a_.runtime.begin(), a_.runtime.end(), [](const RuntimeFile& f) { return Under(LowerAscii(f.target), "maps"); }))
            a_.directories.push_back("maps");
    }

    bool IsTexture(const std::string& path) const {
        const auto it = std::find_if(source_.files.begin(), source_.files.end(), [&path](const PackageFile& f) { return f.path == path; });
        std::string head;
        return it != source_.files.end() && source_.head(path, 54, head) && jpt_custom_level_texture_ok(head.data(), head.size(), it->size);
    }

    void PlaceTextures() {
        size_t unusable = 0;
        std::string first;
        for (RuntimeFile& file : a_.runtime) {
            if (!IsTextureTarget(file.target)) continue;
            const bool bmp = PathType(file.target) == ".bmp", usable = bmp && IsTexture(file.source);
            file.active = usable || LowerAscii(file.target).rfind("data/texhw/", 0) == 0;
            if (bmp && !file.active && !unusable++) first = file.source;
        }
        if (unusable) Warn("texture_unusable", std::to_string(unusable) + " not 8 bit uncompressed BMP, first " + first);
    }

    void ReadLevels() {
        for (const std::string& path : paths_) {
            if (!IsScenePath(path) || a_.levels.size() >= JPT_CUSTOM_LEVEL_SCENES_MAX) continue;
            LevelInfo level;
            level.path = path;
            level.file = PathName(path);
            level.display_name = PathStem(level.file);
            level.configured = root_set_ && Under(path, a_.root);
            std::string bytes;
            const bool read = source_.read(path, kSceneLimit, bytes);
            if (read) {
                const auto* raw = reinterpret_cast<const uint8_t*>(bytes.data());
                if (jpt_szdd_is(raw, bytes.size())) {
                    size_t size = 0;
                    uint8_t* expanded = jpt_szdd_expand(raw, bytes.size(), &size);
                    bytes.assign(reinterpret_cast<const char*>(expanded), expanded ? size : 0);
                    std::free(expanded);
                }
                level.valid = IsTrespasserFile(bytes);
            }
            if (level.valid) {
                jpt_scene_report_t report;
                jpt_scene_readable(bytes.data(), bytes.size(), &report);
                level.worlds = WorldFiles(bytes);
                level.version = report.version;
                if (report.version) level.problem = report.problem;
                level.left_out.assign(report.notes, report.notes + report.note_count);
                if (!level.problem.empty()) Warn("unreadable_scene", path + ": " + level.problem);
            } else if (read) {
                Warn("not_a_scene", path);
            } else {
                level.problem = "it could not be read";
                Warn("unread_scene", path);
            }
            a_.levels.push_back(std::move(level));
        }
    }

    LevelInfo* LevelNamed(const std::string& name) {
        LevelInfo* found = nullptr;
        for (LevelInfo& level : a_.levels) {
            if (!level.configured || !SameText(level.file, name)) continue;
            if (!found || Depth(level.path) < Depth(found->path)) found = &level;
        }
        return found;
    }

    void ReadFmIni() {
        std::string text;
        if (fm_ini_.empty()) return;
        if (!ReadText(fm_ini_, kIniLimit, text)) return Warn("config_unreadable", fm_ini_);
        fm_ = ParseIni(text);
        if (fm_.malformed) Warn("config_malformed_lines", fm_ini_ + ": " + std::to_string(fm_.malformed));
        if (const IniEntry* start = fm_.Find("General", "StartLevel")) ReadStartLevel(start->value);
        if (const IniEntry* movie = fm_.Find("General", "WinMovie")) {
            bool on = true;
            if (ParseIniBool(movie->value, on)) a_.skip_win_movie = !on;
        }
        for (const IniEntry& entry : fm_.entries) {
            const bool general = SameText(entry.section, "General"), scene = IsScenePath(entry.section);
            if (!general && !scene) continue;
            ReadAudio(entry, scene ? entry.section : std::string());
            if (scene && SameText(entry.key, "LoadingImage")) ReadImage(entry);
            if (scene && !LevelNamed(entry.section)) Warn("config_level_absent", "[" + entry.section + "]");
        }
    }

    void ReadStartLevel(const std::string& value) {
        const std::string name = PathName(TrimmedPath(value));
        LevelInfo* level = LevelNamed(name);
        if (!level) return Warn("config_start_level_missing", value);
        level->start = true;
        a_.start_level = level->path;
    }

    void ReadAudio(const IniEntry& entry, const std::string& scene) {
        static constexpr std::pair<const char*, AudioKind> kKeys[] = {
            {"StreamTPA", AudioKind::kStream}, {"EffectsTPA", AudioKind::kEffects}, {"AmbientTPA", AudioKind::kAmbient}};
        for (const auto& [key, kind] : kKeys) {
            if (!SameText(entry.key, key)) continue;
            if (entry.additive && TrimmedPath(entry.value).empty()) return;
            AudioPack pack{kind, scene, entry.additive, entry.value, "", "ready"};
            for (const AudioPack& other : a_.audio)
                if (other.kind == kind && SameText(other.scene, scene) && SameText(other.declared, pack.declared))
                    return Warn("config_tpa_twice", std::string(key) + "+=" + entry.value);
            std::string relative;
            if (!trg::update::NormalizeRelativePath(TrimmedPath(entry.value), relative)) {
                pack.state = "unsafe";
            } else {
                const Lookup lookup = ResolveInRoot(relative, pack.path);
                pack.state = lookup == Lookup::kFound ? "ready" : lookup == Lookup::kMissing ? "missing" : "ambiguous";
            }
            KeepPack(std::move(pack), std::string(key) + (entry.additive ? "+=" : "=") + entry.value);
        }
    }

    void KeepPack(AudioPack pack, const std::string& declared) {
        std::string head;
        if (pack.state == "ready" && (PathType(pack.path) != ".tpa" || !source_.head(pack.path, 4, head) || !IsTpa(head))) pack.state = "not_a_tpa";
        if (pack.state != "ready") {
            pack.path.clear();
            Warn("config_tpa_unusable", declared + " (" + pack.state + ")");
        }
        a_.audio.push_back(std::move(pack));
    }

    // A file ATX reads from its MAPS folder, which lies beside the data folder.
    Lookup AtxFile(const std::string& name, std::string& found) const {
        Lookup lookup = Lookup::kMissing;
        for (const std::string& home : {a_.root, PathFolder(a_.root)})
            if (lookup == Lookup::kMissing) lookup = Resolve(Join(home, "MAPS/" + name), found);
        return lookup;
    }

    // Trespasser CE reads both as ATX does; what an fm.ini sets holds over them.
    void ReadAtxFiles() {
        for (const LevelInfo& level : a_.levels) {
            if (!level.configured || !level.valid) continue;
            const std::string stem = PathStem(level.file);
            std::string path, bytes, text;
            const bool named = std::any_of(a_.images.begin(), a_.images.end(), [&level](const LoadingImage& i) { return SameText(i.scene, level.file) && i.state == "ready"; });
            if (!named && (AtxFile("LBG/" + stem + ".tga", path) == Lookup::kFound || AtxFile("LBG/default.tga", path) == Lookup::kFound)) {
                const bool ready = source_.read(path, kPictureLimit, bytes) && IsLoadingPicture(bytes);
                if (ready) a_.images.push_back({level.file, path, path, "ready"}), Require(true, "ATX_LOADING_PICTURE");
            }
            if (AtxFile("LVL/" + stem + ".ini", path) != Lookup::kFound || !ReadText(path, kIniLimit, text)) continue;
            const IniFile ini = ParseIni(text);
            static constexpr std::pair<const char*, AudioKind> kKeys[] = {{"Stream", AudioKind::kStream}, {"Effects", AudioKind::kEffects}, {"Ambient", AudioKind::kAmbient}};
            for (const auto& [key, kind] : kKeys) {
                const IniEntry* entry = ini.Find("AdditiveTPAFilenames", key);
                std::string relative;
                if (!entry || TrimmedPath(entry->value).empty()) continue;
                AudioPack pack{kind, level.file, true, entry->value, "", "unsafe"};
                if (trg::update::NormalizeRelativePath(TrimmedPath(entry->value), relative)) {
                    const Lookup lookup = AtxFile("LVL/" + relative, pack.path);
                    if (lookup == Lookup::kMissing) continue; // ATX plays the level without it
                    pack.state = lookup == Lookup::kFound ? "ready" : "ambiguous";
                }
                KeepPack(std::move(pack), std::string(key) + "=" + entry->value);
                Require(true, "ATX_LEVEL_SOUNDS");
            }
        }
    }

    void ReadImage(const IniEntry& entry) {
        LoadingImage image{entry.section, entry.value, "", "ready"};
        std::string relative, bytes;
        if (!trg::update::NormalizeRelativePath(TrimmedPath(entry.value), relative)) {
            image.state = "unsafe";
        } else {
            Lookup lookup = ResolveInRoot(relative, image.path);
            if (lookup == Lookup::kMissing && PathType(relative).empty()) lookup = ResolveInRoot(relative + ".tga", image.path);
            image.state = lookup == Lookup::kFound ? "ready" : lookup == Lookup::kMissing ? "missing" : "ambiguous";
        }
        if (image.state == "ready" && (!source_.read(image.path, kPictureLimit, bytes) || !IsLoadingPicture(bytes))) image.state = "unreadable";
        if (image.state != "ready") {
            image.path.clear();
            Warn("config_loading_image", "[" + entry.section + "] LoadingImage=" + entry.value + " (" + image.state + ")");
        }
        a_.images.push_back(std::move(image));
    }

    bool InDataFolder(const LevelInfo& level) const {
        const std::string folder = PathFolder(level.path);
        return loose_ ? folder == a_.root : SameText(folder, Join(a_.root, "data"));
    }

    void FinishLevels() {
        const auto same_worlds = [](const LevelInfo& x, const LevelInfo& y) {
            return !x.worlds.empty() && std::equal(x.worlds.begin(), x.worlds.end(), y.worlds.begin(), y.worlds.end(),
                                                   [](const std::string& p, const std::string& q) { return SameText(p, q); });
        };
        const auto start = std::find_if(a_.levels.begin(), a_.levels.end(), [](const LevelInfo& l) { return l.start; });
        const bool data_scene = std::any_of(a_.levels.begin(), a_.levels.end(), [this](const LevelInfo& l) { return l.valid && InDataFolder(l); });
        for (LevelInfo& level : a_.levels) {
            if (level.start || !level.valid) continue;
            if (!loose_ && data_scene && level.configured && !InDataFolder(level)) level.auxiliary_reason = "outside the mod's data folder";
            else if (start != a_.levels.end() && same_worlds(level, *start)) level.auxiliary_reason = "another state of " + start->file;
            level.auxiliary = !level.auxiliary_reason.empty();
        }
        std::stable_sort(a_.levels.begin(), a_.levels.end(), [](const LevelInfo& x, const LevelInfo& y) {
            return x.start != y.start ? x.start : y.auxiliary && !x.auxiliary;
        });
        if (a_.start_level.empty())
            for (const LevelInfo& level : a_.levels)
                if (level.valid && !level.auxiliary && a_.start_level.empty()) a_.start_level = level.path;
        if (a_.layout == "loose" || a_.layout == "data_folder") a_.display_name = a_.start_level.empty() ? "" : PathStem(PathName(a_.start_level));
        else a_.display_name = a_.root.empty() && !a_.start_level.empty() ? PathStem(PathName(a_.start_level)) : PathName(a_.root);
        if (a_.layout.empty()) a_.layout = "unknown";
    }

    void AddSettings(SettingFile kind, const std::string& path, const IniFile& ini) {
        std::map<std::string, std::string> seen;
        for (const IniEntry& entry : ini.entries) {
            const std::string key = LowerAscii(entry.section + "|" + entry.key);
            if (!entry.additive) {
                const auto [it, first] = seen.emplace(key, entry.value);
                if (!first) {
                    if (it->second != entry.value) Again(kind, path, entry);
                    continue;
                }
            }
            ConfigSetting setting{kind, path, entry, {}};
            if (IsStructuralSetting(kind, entry)) {
                const bool menu = SameText(entry.key, "MenuTPA");
                setting.meaning = {menu ? SettingUse::kRequired : SettingUse::kApplied, menu ? "CE_MENU_TPA" : "", "", menu ? "the menus keep the original sounds" : ""};
            } else {
                setting.meaning = MeaningOf(kind, path, entry);
            }
            if (setting.meaning.use == SettingUse::kInvalid) Warn("config_invalid_value", path + ": " + entry.key + "=" + entry.value);
            if (setting.meaning.use == SettingUse::kMapped) Override(setting);
            if (setting.meaning.use == SettingUse::kLimit) Limit(setting.meaning.key, std::atol(setting.meaning.value.c_str()), SettingFileName(kind) + std::string(": ") + entry.key);
            a_.settings.push_back(std::move(setting));
        }
    }

    // Set twice in one file: of a limit the larger holds, as it does between files.
    void Again(SettingFile kind, const std::string& path, const IniEntry& entry) {
        const SettingMeaning meaning = MeaningOf(kind, path, entry);
        if (meaning.use == SettingUse::kLimit) Limit(meaning.key, std::atol(meaning.value.c_str()), SettingFileName(kind) + std::string(": ") + entry.key);
        else if (meaning.use == SettingUse::kMapped || meaning.use == SettingUse::kRequired || meaning.use == SettingUse::kInvalid || IsStructuralSetting(kind, entry)) Warn("config_twice", path + ": " + entry.key);
    }

    // Of two files that set it, the larger one holds, as in Trespasser CE.
    void Limit(const std::string& key, long value, const std::string& source) {
        for (SessionLimit& other : a_.limits) {
            if (other.key != key) continue;
            if (value > other.value) other = {key, value, source};
            return;
        }
        if (value > 0) a_.limits.push_back({key, value, source});
    }

    void Override(const ConfigSetting& setting) {
        for (const SessionOverride& other : a_.overrides)
            if (other.key == setting.meaning.key) return;
        a_.overrides.push_back({setting.meaning.key, setting.meaning.value, std::string(SettingFileName(setting.kind)) + ": " + setting.entry.key});
    }

    // fm.ini first: what it sets holds over the engine's own file.
    void ReadSettings() {
        if (!fm_ini_.empty()) AddSettings(SettingFile::kFmIni, fm_ini_, fm_);
        const auto add = [this](SettingFile kind, const std::string& path) {
            std::string text;
            if (path.empty()) return;
            if (!ReadText(path, kIniLimit, text)) return Warn("config_unreadable", path);
            AddSettings(kind, path, ParseIni(text));
        };
        add(SettingFile::kTrespasserIni, engine_ini_);
        for (const ConfigFile& config : a_.configs)
            if (config.kind == SettingFile::kTpModIni && config.used) add(config.kind, config.path);
        const std::vector<std::string> registry = FilesNamed("tpass.ini");
        if (!registry.empty()) add(SettingFile::kTpassIni, registry[0]);
    }

    void FindDependencies() {
        Need ce = notes_.ce;
        std::string evidence = notes_.ce_evidence;
        const auto detect = [&](bool found, const std::string& what) {
            if (!found) return;
            if (ce == Need::kNone) ce = Need::kMentioned, evidence = what;
        };
        detect(!fm_ini_.empty(), fm_ini_);
        for (const ConfigFile& config : a_.configs) detect(config.kind != SettingFile::kTpassIni, config.path);
        detect(std::find(a_.directories.begin(), a_.directories.end(), "texhw") != a_.directories.end(), "texhw");
        if (ce != Need::kNone) a_.dependencies.push_back({"Trespasser CE", ce, evidence});
        if (notes_.atx != Need::kNone) a_.dependencies.push_back({"ATX", notes_.atx, notes_.atx_evidence});
    }

    void AddFeature(bool found, const char* name, const char* state) {
        const bool known = std::any_of(a_.features.begin(), a_.features.end(), [name](const Feature& f) { return f.name == name; });
        if (found && !known) a_.features.push_back({name, state});
    }

    void Require(bool found, const char* name) {
        if (found && std::find(a_.required.begin(), a_.required.end(), name) == a_.required.end()) a_.required.push_back(name);
    }

    bool AtxSetting(const char* key) {
        const std::vector<std::string> files = FilesNamed("ATXconfig.ini");
        std::string text;
        if (files.empty() || !ReadText(files[0], kIniLimit, text)) return false;
        const IniFile ini = ParseIni(text);
        return std::any_of(ini.entries.begin(), ini.entries.end(), [key](const IniEntry& e) { return SameText(e.key, key) && std::atoi(e.value.c_str()) != 0; });
    }

    // The rules of the game that a package's own ATXconfig.ini changes; a value is as ATX has it without the file.
    bool ChangesAtxRules() {
        static constexpr struct {
            const char* key;
            double own, same;
        } kRules[] = {{"GameDifficulty", 2, 2}, {"MaxActiveDinos", 0, 0}, {"EnableInstanceRandomizer", 0, 0}, {"EnableNightMode", 0, 0},
                      {"Gravity", 0, 10}, {"NormalGameSpeed", 0, 1}, {"EnableMXTOnActBite", 0, 0}, {"AllowDinosInBuildings", 0, 0}, {"ForceNoStayNearTarget", 0, 0},
                      {"ForceNoStayAwayTarget", 0, 0}, {"AddHitPointsToAllDinos", 0, 0}, {"AddFreezeDistanceToAllDinos", 0, 0}, {"OverrideGameFogValues", 0, 0}};
        const std::vector<std::string> files = FilesNamed("ATXconfig.ini");
        std::string text;
        if (files.empty() || !ReadText(files[0], kIniLimit, text)) return false;
        for (const IniEntry& entry : ParseIni(text).entries)
            for (const auto& rule : kRules) {
                const double value = std::strtod(entry.value.c_str(), nullptr);
                if (SameText(entry.key, rule.key) && value != rule.own && value != rule.same) return true;
            }
        return false;
    }

    // What of ATX the package's files use. What Trespasser CE does not have either is asked for only by a package made for ATX alone.
    void RequireAtx() {
        const bool atx = notes_.atx == Need::kRequired;
        const auto in_maps = [this](const char* folder, const char* type) {
            return std::any_of(paths_.begin(), paths_.end(), [&](const std::string& path) {
                const std::string lower = "/" + LowerAscii(path);
                return lower.find(std::string("/maps/") + folder) != std::string::npos && (!*type || PathType(path) == type);
            });
        };
        const auto own = [this](const char* folder) {
            std::string found;
            return std::any_of(a_.levels.begin(), a_.levels.end(), [&](const LevelInfo& l) { return l.valid && l.configured && AtxFile(std::string(folder) + "/" + PathStem(l.file) + ".ini", found) == Lookup::kFound; });
        };
        Require(in_maps("hud/", ""), "ATX_HUD");
        Require(in_maps("rne/", "") || AtxSetting("FeatureEnabled"), "ATX_RAIN");
        Require(in_maps("", ".ddf"), "ATX_MENU_TEXTS");
        Require(own("RAN"), "ATX_RANDOMIZER");
        Require(own("NMD"), "ATX_NIGHT_MODE");
        const auto alone = [this, atx](bool found, const char* name) {
            if (atx) Require(found, name);
            else AddFeature(found, name, "not_required");
        };
        alone(own("JMP"), "ATX_JUMPS");
        alone(own("SPT"), "ATX_PROJECTILES");
        alone(in_maps("plg/", ""), "ATX_PLUGIN");
        alone(ChangesAtxRules(), "ATX_SETTINGS");
    }

    void RequireSky() {
        for (const RuntimeFile& file : a_.runtime) {
            std::string text;
            if (PathType(file.target) != ".sky" || !ReadText(file.source, kIniLimit, text)) continue;
            Require(true, "CE_SKY");
            for (const IniEntry& entry : ParseIni(text).entries) {
                const std::string value = LowerAscii(entry.value);
                Require(SameText(entry.key, "Type") && (value == "stars" || value == "meteors"), "CE_SKY_STARS");
                Require(SameText(entry.key, "BlendMode") && value.rfind("2x_", 0) == 0, "CE_SKY_2X");
            }
        }
    }

    // Loading pictures are shown by the port; anything else in the menu folder replaces the game's own menus.
    bool ReplacesMenus() const {
        return std::any_of(a_.runtime.begin(), a_.runtime.end(), [this](const RuntimeFile& f) {
            if (!Under(LowerAscii(f.target), "data/menu")) return false;
            return std::none_of(a_.images.begin(), a_.images.end(), [&f](const LoadingImage& i) { return i.state == "ready" && i.path == f.source; });
        });
    }

    void FindFeatures() {
        const auto has = [this](const char* folder) { return std::find(a_.directories.begin(), a_.directories.end(), folder) != a_.directories.end(); };
        const auto any = [this](const char* type) {
            return std::any_of(a_.runtime.begin(), a_.runtime.end(), [type](const RuntimeFile& f) { return PathType(f.target) == type; });
        };
        AddFeature(fm_.Find("General", "StartLevel") != nullptr, "StartLevel", "supported");
        AddFeature(std::any_of(a_.audio.begin(), a_.audio.end(), [](const AudioPack& p) { return p.state == "ready"; }), "extra sound files (TPA)", "supported");
        AddFeature(std::any_of(a_.images.begin(), a_.images.end(), [](const LoadingImage& i) { return i.state == "ready"; }), "LoadingImage", "supported");
        AddFeature(a_.skip_win_movie, "WinMovie", "supported");
        AddFeature(!uber_.empty() || !mods_.empty(), "ModPaths", "supported");
        AddFeature(has("anim"), "anim scripts", "supported");
        for (const ConfigSetting& setting : a_.settings) {
            if (setting.meaning.use == SettingUse::kMapped || setting.meaning.use == SettingUse::kLimit) AddFeature(true, setting.entry.key.c_str(), "mapped");
            if (setting.meaning.use == SettingUse::kBuiltIn) AddFeature(true, setting.entry.key.c_str(), "supported");
            if (setting.meaning.use == SettingUse::kNotActive) AddFeature(true, setting.entry.key.c_str(), "unsupported");
            if (setting.meaning.use == SettingUse::kRequired) Require(true, setting.meaning.key.c_str());
        }
        Require(has("menu") && ReplacesMenus(), "CE_MENU_FILES");
        Require(has("maps"), "ATX_MAP");
        Require(any(".smk"), "CE_VIDEOS");
        Require(std::any_of(paths_.begin(), paths_.end(), [](const std::string& path) { return PathType(path) == ".nut" && !IsDocumentation(path); }), "CE_SCRIPTS");
        RequireAtx();
        RequireSky();
        for (const std::string& name : a_.required) {
            const jpt_level_feature_t* feature = jpt_level_feature(name.c_str());
            AddFeature(true, name.c_str(), !feature ? "unsupported" : feature->status == JPT_FEATURE_FULLY_SUPPORTED ? "supported" : feature->status == JPT_FEATURE_NOT_REQUIRED ? "not_required" : "unsupported");
        }
    }

    const PackageSource& source_;
    PackageAnalysis a_;
    ReadmeNotes notes_;
    std::vector<std::string> paths_, folders_, uber_, mods_;
    std::map<std::string, std::vector<std::string>> by_lower_;
    std::map<std::string, size_t> targets_;
    std::string fm_ini_, engine_ini_;
    IniFile fm_;
    bool root_set_ = false, loose_ = false;
};

} // namespace

const char* OriginName(Origin origin) {
    switch (origin) {
    case Origin::kLayout: return "archive layout";
    case Origin::kFmIni: return "fm.ini";
    case Origin::kTpModIni: return "tp_mod.ini";
    case Origin::kTrespasserIni: return "trespasser.ini";
    case Origin::kReadme: return "README";
    case Origin::kHeuristic: return "heuristic";
    }
    return "";
}

const char* AudioKindName(AudioKind kind) {
    return kind == AudioKind::kStream ? "stream" : kind == AudioKind::kEffects ? "effects" : "ambient";
}

bool PackageAnalysis::playable() const {
    return std::any_of(levels.begin(), levels.end(), [](const LevelInfo& level) { return level.valid; });
}

Need PackageAnalysis::need(const char* dependency) const {
    for (const Dependency& d : dependencies)
        if (d.name == dependency) return d.need;
    return Need::kNone;
}

bool PackageAnalysis::config_warnings() const {
    return std::any_of(warnings.begin(), warnings.end(), [](const Warning& w) { return w.code.rfind("config_", 0) == 0; });
}

PackageAnalysis AnalyzePackage(const PackageSource& source) { return Analyzer(source).Run(); }

std::string PathName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string PathFolder(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string PathType(const std::string& path) {
    const std::string name = PathName(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : LowerAscii(name.substr(dot));
}

std::string PathStem(const std::string& name) { return name.substr(0, name.rfind('.')); }

bool IsScenePath(const std::string& path) { return PathType(path) == ".scn"; }

bool IsDocumentation(const std::string& path) {
    static constexpr const char* kFolders[] = {"doc", "docs", "documentation"};
    for (size_t start = 0, slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        if (OneOf(LowerAscii(path.substr(start, slash - start)), kFolders)) return true;
    return false;
}

int LevelReadmeScore(const std::string& path) {
    static constexpr const char* kTypes[] = {"", ".txt", ".md", ".markdown", ".nfo", ".1st", ".me"};
    static constexpr const char* kReadme[] = {"readme", "read me", "read_me", "read-me", "leiame", "leia-me", "leia_me", "leia me", "lisezmoi", "lisez-moi", "liesmich", "leeme"};
    static constexpr const char* kGuides[] = {"install", "instruc", "instalac", "how to", "howto", "how_to", "how-to", "walkthrough", "manual", "guide"};
    const std::string name = LowerAscii(PathName(path)), type = PathType(path);
    const std::string stem = type.empty() ? name : PathStem(name);
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

std::string ChooseReadme(const std::vector<std::string>& relative_paths) {
    std::string best;
    int best_score = 0;
    for (const std::string& path : relative_paths) {
        const int score = LevelReadmeScore(path);
        if (!score || score < best_score) continue;
        const bool better = score > best_score || Depth(path) < Depth(best) ||
                            (Depth(path) == Depth(best) && (path.size() < best.size() || (path.size() == best.size() && path < best)));
        if (!better) continue;
        best = path;
        best_score = score;
    }
    return best;
}

// UTF-16 of mostly Western text, without a BOM.
static bool IsWide(const std::string& bytes) {
    size_t high = 0, low = 0;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) low += bytes[i] == 0, high += bytes[i + 1] == 0;
    return bytes.size() >= 8 && !low && high * 10 >= bytes.size() / 2 * 8;
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
        text = Utf8(std::filesystem::path(wide));
    } else if (bytes.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
        text = IsUtf8(bytes) ? bytes.substr(3) : FromWindows1252(bytes.substr(3));
    } else if (IsWide(bytes)) {
        return DecodeText("\xFF\xFE" + bytes);
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

bool IsText(const std::string& bytes) {
    const bool wide = IsWide(bytes) || bytes.rfind("\xFF\xFE", 0) == 0 || bytes.rfind("\xFE\xFF", 0) == 0;
    return !bytes.empty() && (wide || bytes.find('\0') == std::string::npos);
}

} // namespace jpt
