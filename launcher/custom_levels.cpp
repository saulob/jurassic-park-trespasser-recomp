#include "custom_levels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>

#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_groff.h"
#include "jpt_hash.h"
#include "jpt_picture.h"
#include "jpt_script_plan.h"
#include "jpt_level_features.h"
#include "jpt_level_world.h"
#include "level_guide.h"
#include "level_import_log.h"
#include "level_listing.h"
#include "level_manifest.h"
#include "level_picture.h"
#include "updater/update_paths.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr char kStagingPrefix[] = ".import-";
constexpr char kRemovingPrefix[] = ".removing-";
constexpr uint64_t kSpareBytes = 64ull << 20;
constexpr uint64_t kTextLimit = 8ull << 20;
constexpr uint64_t kRecordLimit = 16ull << 20;
constexpr uint64_t kMapPictureLimit = 64ull << 20;
// The game opens files by ANSI paths of at most 259 characters.
constexpr size_t kHostPathLimit = 250;
constexpr size_t kRefusedMax = 512;
// "-99" after a package's name that is taken.
[[maybe_unused]] constexpr size_t kSuffixRoom = 3;

std::string Lower(std::string_view text) { return LowerAscii(text); }

template <size_t N>
bool OneOf(const std::string& text, const char* const (&list)[N]) {
    return std::any_of(std::begin(list), std::end(list), [&text](const char* item) { return text == item; });
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

std::vector<PackageFile> PackageFiles(const fs::path& dir) {
    ArchiveError error;
    std::vector<PackageFile> files;
    if (const auto reader = OpenFolder(dir, {}, error))
        for (const ArchiveEntry& entry : reader->entries())
            if (!entry.directory && !entry.link && !IsGeneratedPath(entry.name)) files.push_back({entry.name, entry.size});
    std::sort(files.begin(), files.end(), [](const PackageFile& a, const PackageFile& b) { return Lower(a.path) < Lower(b.path); });
    return files;
}

bool ReadHead(const fs::path& file, size_t count, std::string& bytes) {
    std::ifstream in(file, std::ios::binary);
    bytes.resize(count);
    in.read(bytes.data(), std::streamsize(count));
    bytes.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    return !bytes.empty();
}

PackageSource FolderSource(const fs::path& dir, const std::vector<PackageFile>& files) {
    PackageSource source;
    source.files = files;
    source.read = [dir](const std::string& path, uint64_t limit, std::string& bytes) { return ReadBytes(dir / PathFromUtf8(path), limit, bytes); };
    source.head = [dir](const std::string& path, size_t count, std::string& bytes) { return ReadHead(dir / PathFromUtf8(path), count, bytes); };
    return source;
}

// Beside the scene, else the only file of that name in the package.
std::string FindFile(const std::vector<PackageFile>& files, const std::string& folder, const std::string& name) {
    const std::string beside = Lower(folder + name), wanted = Lower(name);
    std::string found;
    size_t named = 0;
    for (const PackageFile& file : files) {
        const std::string lower = Lower(file.path);
        if (lower == beside) return file.path;
        if (PathName(lower) != wanted) continue;
        found = file.path;
        ++named;
    }
    return named == 1 ? found : std::string();
}

bool InGame(const fs::path& data_dir, const std::string& name) { return !GameFile(data_dir, name).empty(); }

// Loose pictures of a level, which the engine takes when it has no texture set.
bool HasLooseTextures(const PackageAnalysis& a, const std::string& world) {
    const std::string folder = "map/" + Lower(PathStem(world)) + "/";
    return std::any_of(a.runtime.begin(), a.runtime.end(), [&folder](const RuntimeFile& file) {
        const std::string lower = Lower(file.target);
        return file.active && (lower.rfind(folder, 0) == 0 || lower.rfind("data/" + folder, 0) == 0);
    });
}

// Its place in the package, or the installed game; "" when it is nowhere.
std::string PlaceOf(const std::vector<PackageFile>& files, const std::string& folder, const fs::path& data_dir, const std::string& name) {
    const std::string own = FindFile(files, folder, name);
    if (!own.empty()) return own;
    return InGame(data_dir, name) ? kInstalledGame : "";
}

void Lacks(LevelScene& scene, const char* code, const std::string& name, const std::string& why) {
    scene.missing.push_back(name);
    scene.issues.push_back({code, name, why, IssuePolicy::kMissing});
}

void Uses(LevelScene& scene, const char* type, const std::string& name, const std::string& place, const std::string& note = {}) {
    scene.resources.push_back({type, name, place, note});
    scene.uses.push_back(std::string(type) + " " + name + ": " + (!place.empty() ? place : !note.empty() ? note : "missing"));
}

// Where the engine finds the world's textures: the place of its .pid.
std::string FindTextures(const PackageAnalysis& a, const std::vector<PackageFile>& files, const std::string& folder, const fs::path& data_dir,
                         const std::string& world, const WorldChecker& checker, LevelScene& scene) {
    static constexpr const char* kSwaps[] = {".spz", ".spp", ".swp"};
    const std::string set = PathStem(world) + "-130";
    std::string pid = FindFile(files, folder, set + ".pid"), swap, swap_name = set + ".swp";
    const auto find_swap = [&](const auto& place) {
        for (const char* type : kSwaps)
            if (swap.empty() && !(swap = place(set + type)).empty()) swap_name = set + type;
    };
    find_swap([&](const std::string& name) { return FindFile(files, folder, name); });
    if (pid.empty() && swap.empty()) {
        if (InGame(data_dir, set + ".pid")) pid = kInstalledGame;
        find_swap([&](const std::string& name) { return std::string(InGame(data_dir, name) ? kInstalledGame : ""); });
        if (!pid.empty() && !swap.empty()) scene.note = "textures of the installed game (" + set + ")";
    }
    if (!pid.empty() && swap.empty()) {
        std::string own;
        for (const char* type : kSwaps)
            if (own.empty() && InGame(data_dir, set + type)) own = set + type;
        if (!own.empty() && checker.GameSwapHolds(pid, own)) swap = kInstalledGame, swap_name = own;
    }
    const bool loose = (pid.empty() || swap.empty()) && HasLooseTextures(a, world);
    const std::string none = loose ? "none, loose pictures" : "";
    Uses(scene, "PID", set + ".pid", pid, none);
    Uses(scene, PathType(swap_name) == ".swp" ? "SWP" : "SPZ", swap_name, swap, none);
    const char* why = "without it the level has no texture set, and it has no loose pictures for its textures either";
    if (pid.empty() && !loose) Lacks(scene, "MISSING_REQUIRED_TEXTURE_SET", set + ".pid", why);
    if (swap.empty() && !loose) Lacks(scene, "MISSING_REQUIRED_TEXTURE_SET", set + ".swp", why);
    return pid.empty() || swap.empty() ? std::string() : pid;
}

// The engine builds the world from this file and goes on with none when it is not there.
void FindTerrain(const std::vector<PackageFile>& files, const std::string& folder, const fs::path& data_dir, const WorldFacts& facts, LevelScene& scene) {
    if (facts.terrain.empty()) return;
    const std::string place = PlaceOf(files, folder, data_dir, facts.terrain);
    Uses(scene, "WTD", facts.terrain, place);
    if (place.empty()) Lacks(scene, "MISSING_REQUIRED_TERRAIN", facts.terrain, "the world names it as its terrain data and cannot be built without it");
}

void FindWorld(const std::string& world, const PackageAnalysis& a, const std::vector<PackageFile>& files, const std::string& folder, const fs::path& data_dir,
               WorldChecker& checker, LevelScene& scene) {
    const std::string place = PlaceOf(files, folder, data_dir, world);
    Uses(scene, "GRF", world, place);
    if (place.empty()) return Lacks(scene, "MISSING_REQUIRED_WORLD", world, "the scene is a state of this world");
    if (place == kInstalledGame) return;
    const std::string set = FindTextures(a, files, folder, data_dir, world, checker, scene);
    const WorldFacts facts = checker.Check({world, place, set});
    FindTerrain(files, folder, data_dir, facts, scene);
    AddWorldIssues(world, facts, scene.issues);
    scene.worlds.push_back({world, facts});
}

void AddSceneIssues(const LevelInfo& level, LevelScene& scene) {
    const bool newer = level.version > JPT_SCENE_VERSION_MAX;
    if (!level.problem.empty()) scene.issues.push_back({newer ? "UNSUPPORTED_SCENE_FORMAT" : "MALFORMED_SCENE", scene.file, level.problem, newer ? IssuePolicy::kUnsupported : IssuePolicy::kMalformed});
    else if (!level.version) scene.issues.push_back({"MALFORMED_SCENE", scene.file, "its scene version could not be read", IssuePolicy::kMalformed});
    for (const std::string& part : level.left_out) scene.issues.push_back({"SCENE_PART_LEFT_OUT", scene.file, part, IssuePolicy::kInfo});
    if (!level.problem.empty()) scene.note = level.problem;
    else if (!level.version) scene.note = "its scene version could not be read";
}

void JudgeScene(const LevelInfo& level, LevelScene& scene) {
    if (!scene.valid) return;
    const LevelClass value = scene.level_class();
    if (!level.problem.empty()) scene.state = LevelState::kUnsupportedFormat;
    else if (!level.version) scene.state = LevelState::kUnknown;
    else if (value == LevelClass::kRejectedMalformed) scene.state = LevelState::kDamaged;
    else if (value == LevelClass::kRejectedMissing) scene.state = LevelState::kIncomplete;
    else if (value != LevelClass::kReady) scene.state = LevelState::kNotSupported;
    else scene.state = scene.auxiliary ? LevelState::kAuxiliary : LevelState::kReady;
}

LevelScene SceneOf(const LevelInfo& level, const PackageAnalysis& a, const std::vector<PackageFile>& files, const fs::path& data_dir, WorldChecker& checker) {
    LevelScene scene;
    scene.path = level.path;
    scene.file = level.file;
    scene.valid = level.valid;
    scene.start = level.start;
    scene.auxiliary = level.auxiliary;
    scene.version = level.version;
    scene.note = level.valid ? level.auxiliary_reason : level.problem.empty() ? "not a Trespasser scene" : level.problem;
    scene.left_out = level.left_out;
    if (!scene.valid) {
        scene.issues.push_back({level.problem.empty() ? "NOT_A_SCENE" : "UNREAD_SCENE", scene.file, scene.note, IssuePolicy::kMalformed});
        return scene;
    }
    const std::string parent = PathFolder(level.path), folder = parent.empty() ? parent : parent + "/";
    if (level.worlds.empty()) Lacks(scene, "MISSING_REQUIRED_WORLD", PathStem(scene.file) + ".grf", "the scene names no world");
    for (const std::string& world : level.worlds) FindWorld(world, a, files, folder, data_dir, checker, scene);
    AddSceneIssues(level, scene);
    return scene;
}

// The scenes a level loads: the package's, the installed game's, or nowhere.
void FollowTransitions(const fs::path& data_dir, std::vector<LevelScene>& scenes) {
    for (LevelScene& scene : scenes) {
        std::set<std::string> seen;
        for (const auto& [world, facts] : scene.worlds) {
            for (const std::string& name : facts.levels) {
                if (!seen.insert(Lower(name)).second || SameText(name, scene.file)) continue;
                const auto target = std::find_if(scenes.begin(), scenes.end(), [&name](const LevelScene& s) { return SameText(s.file, name); });
                const std::string place = target != scenes.end() ? target->path : InGame(data_dir, name) ? kInstalledGame : "";
                scene.resources.push_back({"SCN", name, place, "loaded by a trigger"});
                if (target != scenes.end()) target->led_to_by.push_back(scene.file);
                else if (place.empty() && !data_dir.empty())
                    scene.issues.push_back({"MISSING_TRANSITION_LEVEL", name, "a trigger of " + world + " loads it; the package does not include it, so the game stays in this level",
                                            IssuePolicy::kInfo});
            }
        }
    }
}

IssuePolicy WarningPolicy(const std::string& code) {
    if (code == "no_restart") return IssuePolicy::kInfo; // the author's own note on the original game
    if (code == "config_level_absent") return IssuePolicy::kInfo; // a section for another level, which that level reads
    if (code == "config_malformed_lines") return IssuePolicy::kInfo; // lines that set nothing
    if (code == "config_start_level_missing") return IssuePolicy::kInfo; // each level the package does have is started by itself
    if (code == "readme_file_absent") return IssuePolicy::kMissing;
    if (code == "texture_unusable") return IssuePolicy::kUnsupported;
    return IssuePolicy::kWarning;
}

std::string UpperAscii(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// The original game's own file; of a texture's pixels its packed form.
bool GameHas(const fs::path& data_dir, const std::string& name) {
    return InGame(data_dir, name) || (PathType(name) == ".swp" && InGame(data_dir, PathStem(name) + ".spz"));
}

void AddPackageIssues(const PackageAnalysis& a, const fs::path& data_dir, std::vector<LevelScene>& scenes) {
    std::vector<LevelIssue> issues;
    for (const Dependency& d : a.dependencies) {
        const std::string code = d.name == "ATX" ? "DEPENDENCY_ATX" : "DEPENDENCY_CE";
        issues.push_back({code, d.name, std::string(NeedName(d.need)) + (d.need == Need::kRequired ? " (" + d.evidence + ")" : "") + "; what of it the package uses is checked on its own", IssuePolicy::kInfo});
    }
    for (const Feature& feature : a.features) {
        const jpt_level_feature_t* known = jpt_level_feature(feature.name.c_str());
        const std::string detail = known ? std::string(known->title) + (known->note[0] ? std::string(": ") + known->note : "") : "the package uses it and this port does not have it";
        if (feature.state == "unsupported") issues.push_back({"UNSUPPORTED_FEATURE", feature.name, detail, IssuePolicy::kUnsupported});
        if (feature.state == "not_required") issues.push_back({"NOT_REQUIRED", feature.name, detail, IssuePolicy::kInfo});
    }
    for (const Warning& warning : a.warnings) {
        if (warning.code == "not_a_scene" || warning.code == "unread_scene" || warning.code == "unreadable_scene") continue;
        if (warning.code.rfind("config_", 0) == 0) issues.push_back({"CONFIG_WARNING", warning.code, warning.detail, WarningPolicy(warning.code)});
        else issues.push_back({UpperAscii(warning.code), "package", warning.detail, warning.code == "readme_file_absent" && GameHas(data_dir, warning.detail) ? IssuePolicy::kInfo : WarningPolicy(warning.code)});
    }
    for (LevelScene& scene : scenes)
        if (scene.valid) scene.issues.insert(scene.issues.end(), issues.begin(), issues.end());
}


uint64_t MostOf(const std::vector<LevelScene>& scenes, const char* count) {
    uint64_t most = 0;
    for (const LevelScene& scene : scenes)
        for (const auto& [world, facts] : scene.worlds) most = std::max<uint64_t>(most, facts.count(count));
    return most;
}

void AtLeast(PackageAnalysis& a, const char* key, long value, const char* source) {
    for (SessionLimit& limit : a.limits) {
        if (limit.key != key) continue;
        if (limit.value < value) limit = {key, value, source};
        return;
    }
    a.limits.push_back({key, value, source});
}

// Earlier versions left a package's scripts out when they installed it; it is not whole without them.
void NoteRefused(PackageAnalysis& a, const std::vector<std::string>& refused) {
    const bool scripts = std::any_of(refused.begin(), refused.end(), [](const std::string& name) {
        const std::string path = name.substr(0, name.find(" - "));
        return PathType(path) == ".nut" && !IsDocumentation(path);
    });
    a.scripts_left_out = a.scripts_left_out || scripts;
}

// Which of the package's scripts each level needs (docs/CE_SCRIPTS.md, phase 0): never the package as a whole.
void AddScriptIssues(const PackageAnalysis& a, const PackageSource& source, std::vector<LevelScene>& scenes) {
    std::vector<script::FileInfo> files;
    for (const std::string& path : a.scripts) {
        std::string bytes;
        if (source.read(path, script::kSourceMax, bytes)) {
            files.push_back(script::ScanFile(path, bytes));
        } else {
            files.push_back({});
            files.back().path = path;
            files.back().problem = "it cannot be read";
        }
    }
    for (LevelScene& scene : scenes) {
        if (!scene.valid) continue;
        std::vector<script::WorldScript> world;
        bool unread = false;
        for (const auto& [name, facts] : scene.worlds) {
            for (const ScriptUse& use : facts.scripts) world.push_back({use.class_name, use.object, use.action});
            unread = unread || facts.count("script_classes_unread");
        }
        const script::LevelPlan plan = script::PlanLevel(files, scene.file, world);
        if (!plan.needs_scripts() && !unread) continue;
        scene.scripted = true;
        scene.script_files = plan.files;
        scene.script_required = plan.required;
        for (const std::string& file : plan.files) scene.uses.push_back("script " + file);
        for (const script::PlannedClass& c : plan.classes)
            scene.issues.push_back({"SCRIPT_CLASS", c.name, std::string(script::KindName(c.kind)) + " script of " + c.file + (c.objects.empty() ? "" : ", named by " + c.objects.front() + (c.objects.size() > 1 ? " and " + std::to_string(c.objects.size() - 1) + " more" : "")), IssuePolicy::kInfo});
        for (const std::string& file : plan.files) {
            const auto info = std::find_if(files.begin(), files.end(), [&file](const script::FileInfo& f) { return f.path == file; });
            const std::vector<std::string> asks = script::FileRequires(files, *info);
            std::string list;
            for (const std::string& name : asks) list += (list.empty() ? "" : ", ") + name;
            if (!list.empty()) scene.issues.push_back({"SCRIPT_REQUIRES", file, "required script functions: " + list, IssuePolicy::kInfo});
        }
        if (!plan.required.empty()) {
            std::string list;
            for (const std::string& name : plan.required) list += (list.empty() ? "" : ", ") + name;
            scene.issues.push_back({"UNSUPPORTED_FEATURE", "CE_SCRIPT_API", "Trespasser CE script functions the port does not have yet: " + list, IssuePolicy::kUnsupported});
        }
        for (const std::string& problem : plan.problems)
            scene.issues.push_back({"UNSUPPORTED_FEATURE", a.scripts_left_out ? "CE_SCRIPT_FILES" : "CE_SCRIPTS", problem, IssuePolicy::kUnsupported});
        if (unread) scene.issues.push_back({"UNSUPPORTED_FEATURE", "CE_SCRIPTS", "a world names more script classes than are read", IssuePolicy::kUnsupported});
    }
}

void NoteActions(PackageAnalysis& a, const std::vector<LevelScene>& scenes) {
    std::set<int> types;
    for (const LevelScene& scene : scenes)
        for (const auto& [world, facts] : scene.worlds)
            for (const auto& [type, count] : facts.actions)
                if (const jpt_level_feature_t* f = type > JPT_GROFF_ACTION_OWN_LAST ? jpt_level_action_feature(type) : nullptr)
                    if (f->status != JPT_FEATURE_NOT_REQUIRED) types.insert(type);
    a.action_types.assign(types.begin(), types.end());
}

// What a level's meshes need at most, with a tenth to spare, when that is more than the engine's own or its settings ask for;
// and its textures, when the engine's pool does not hold them and some of other files. A level that fits asks for nothing.
void SizeHeaps(PackageAnalysis& a, const std::vector<LevelScene>& scenes) {
    constexpr long kOwnFaces = 2048, kOwnMegabytes = 32;
    NoteActions(a, scenes);
    const uint64_t bytes = MostOf(scenes, "mesh_bytes"), faces = MostOf(scenes, "mesh_build"), textures = MostOf(scenes, "set_bytes") + JPT_GROFF_POOL_ROOM;
    const long wanted = std::min<long>(long((bytes + bytes / 10 + (1u << 20) - 1) >> 20), kMeshHeapMegabytesMax);
    if (wanted > kOwnMegabytes) AtLeast(a, "mesh_heap_mb", wanted, "the size of its meshes");
    if (faces > uint64_t(kOwnFaces)) AtLeast(a, "mesh_faces", std::min<long>(long(faces + faces / 10), JPT_GROFF_MESH_FACES_MAX), "the largest of its meshes");
    if (textures > JPT_GROFF_POOL_BYTES + JPT_GROFF_POOL_ROOM)
        a.limits.push_back({"texture_pool_mb", std::min<long>(long((textures + (1u << 20) - 1) >> 20), long(JPT_GROFF_POOL_BYTES_MAX >> 20)), "the size of its textures"});
}

// ATX reads the picture a map action names as MAPS\<name>. The package's own file of that path below MAPS, or else its only file of
// that path anywhere, is put there; a picture that is nowhere leaves the map as it was, as in ATX.
void PlaceMapPictures(PackageAnalysis& a, const PackageSource& source, std::vector<LevelScene>& scenes) {
    const auto placed = [&a](const std::string& target) {
        return std::any_of(a.runtime.begin(), a.runtime.end(), [&target](const RuntimeFile& f) { return f.active && SameText(f.target, target); });
    };
    const auto only_file = [&source](const std::string& relative) {
        std::string found;
        const std::string tail = "/" + Lower(relative);
        for (const PackageFile& file : source.files) {
            const std::string lower = "/" + Lower(file.path);
            if (lower.size() < tail.size() || lower.compare(lower.size() - tail.size(), tail.size(), tail)) continue;
            if (!found.empty()) return std::string();
            found = file.path;
        }
        return found;
    };
    for (LevelScene& scene : scenes)
        for (const auto& [world, facts] : scene.worlds)
            for (const std::string& name : facts.maps) {
                char target[JPT_CUSTOM_LEVEL_PATH_MAX];
                if (!jpt_custom_level_map_path(name.c_str(), target, sizeof target) || !IsGamePath(target)) {
                    scene.issues.push_back({"UNSAFE_MAP_PICTURE", name, "a map action of " + world + " names a picture outside the MAPS folder; it is not read", IssuePolicy::kInfo});
                    continue;
                }
                if (placed(target)) continue;
                const std::string file = only_file(std::string(target).substr(5));
                std::string bytes;
                jpt_picture_t picture{};
                if (file.empty()) {
                    scene.issues.push_back({"MAP_PICTURE_ABSENT", name, "a map action of " + world + " names it; the map stays as it was", IssuePolicy::kInfo});
                } else if (!source.read(file, kMapPictureLimit, bytes) || !jpt_picture_decode(bytes.data(), bytes.size(), &picture)) {
                    scene.issues.push_back({"MAP_PICTURE_UNREADABLE", file, "a map action of " + world + " names it and it is no picture the map can show", IssuePolicy::kInfo});
                } else {
                    a.runtime.push_back({target, file, Origin::kHeuristic, true});
                    a.actions.push_back({"move", file, target, Origin::kHeuristic, "a map action of " + world + " shows it from MAPS"});
                }
                jpt_picture_free(&picture);
            }
    const bool maps = std::any_of(a.runtime.begin(), a.runtime.end(), [](const RuntimeFile& f) { return f.active && Lower(f.target).rfind("maps/", 0) == 0; });
    if (maps && std::find(a.directories.begin(), a.directories.end(), "maps") == a.directories.end()) a.directories.push_back("maps");
    if (maps && std::find(a.required.begin(), a.required.end(), "ATX_MAP") == a.required.end()) a.required.push_back("ATX_MAP");
}

void FinishAnalysis(PackageAnalysis& a, const PackageSource& source, std::vector<LevelScene>& scenes) {
    a.script_references.clear();
    a.script_uses.clear();
    for (const LevelScene& scene : scenes)
        for (const std::string& file : scene.script_files) a.script_uses.push_back({scene.file, file});
    for (const LevelScene& scene : scenes)
        for (const auto& [world, facts] : scene.worlds)
            for (const ScriptUse& use : facts.scripts) a.script_references.push_back({scene.file, use.class_name, use.object, use.action});
    PlaceMapPictures(a, source, scenes);
    SizeHeaps(a, scenes);
}

// package_dir: of an installed package, so that its worlds are not read again each time.
std::vector<LevelScene> ScenesOf(const PackageAnalysis& a, const PackageSource& source, const fs::path& data_dir, const fs::path& package_dir = {}) {
    WorldChecker checker({&source, &a, data_dir, package_dir});
    std::vector<LevelScene> scenes;
    for (const LevelInfo& level : a.levels) scenes.push_back(SceneOf(level, a, source.files, data_dir, checker));
    FollowTransitions(data_dir, scenes);
    AddPackageIssues(a, data_dir, scenes);
    AddScriptIssues(a, source, scenes);
    for (size_t i = 0; i < scenes.size(); ++i) JudgeScene(a.levels[i], scenes[i]);
    return scenes;
}

std::vector<LevelVerdict> VerdictsOf(const std::vector<LevelScene>& scenes) {
    std::vector<LevelVerdict> verdicts;
    for (const LevelScene& scene : scenes) {
        if (!scene.valid) continue;
        LevelVerdict v{scene.path, LevelClassName(scene.level_class()), scene.playable(), scene.scripted, scene.script_files, scene.script_required, {}};
        for (const LevelIssue& issue : scene.issues)
            if (issue.blocks()) v.blockers.push_back(issue.code + " " + issue.subject);
        verdicts.push_back(std::move(v));
    }
    return verdicts;
}

std::vector<std::string> MissingOf(const std::vector<LevelScene>& scenes) {
    std::vector<std::string> missing;
    for (const LevelScene& scene : scenes)
        for (const std::string& name : scene.missing) missing.push_back(scene.file + ": " + name);
    return missing;
}

size_t Playable(const std::vector<LevelScene>& scenes) {
    return size_t(std::count_if(scenes.begin(), scenes.end(), [](const LevelScene& scene) { return scene.playable(); }));
}

RefusedFile Refused(const std::string& path) {
    static constexpr const char* kPrograms[] = {".exe", ".dll", ".com", ".scr", ".msi", ".asi", ".so", ".sys", ".ocx", ".cpl", ".pif"};
    static constexpr const char* kScripts[] = {".bat", ".cmd", ".ps1", ".vbs", ".js", ".wsf", ".hta", ".sh", ".lnk", ".reg", ".url", ".jar", ".py"};
    const std::string type = PathType(path), name = Lower(PathName(path));
    if (IsGeneratedPath(path)) return {path, "a name this launcher generates", false};
    if (OneOf(type, kPrograms)) return {path, "a program or library", true};
    if (OneOf(type, kScripts)) return {path, "a script or shortcut", true};
    if (name.rfind("savegame.", 0) == 0) return {path, "a saved game", false};
    return {path, "not level data, a text or a picture", false};
}

std::vector<std::string> RefusedNames(const std::vector<RefusedFile>& refused) {
    std::vector<std::string> names;
    for (const RefusedFile& file : refused) names.push_back(file.path + " - " + file.reason);
    return names;
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
        if (!IsLevelPackageFile(path) || IsGeneratedPath(path)) {
            ++result.skipped;
            if (result.refused.size() < kRefusedMax) result.refused.push_back(Refused(path));
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
    if (std::none_of(plan.begin(), plan.end(), [](const PlannedFile& f) { return IsScenePath(f.path); }))
        return refuse(ImportStatus::kNoLevel, "");
    return true;
}

// Several levels of their own side by side: one import would merge them.
bool HoldsSeveralPackages(const std::vector<PlannedFile>& plan, const ArchiveReader& reader) {
    std::set<std::string> tops;
    for (const PlannedFile& f : plan) {
        if (!IsScenePath(f.path)) continue;
        const size_t slash = f.path.find('/');
        if (slash == std::string::npos) return false;
        tops.insert(Lower(f.path.substr(0, slash)));
    }
    const bool archives = std::any_of(reader.entries().begin(), reader.entries().end(), [](const ArchiveEntry& e) {
        const std::string type = PathType(e.name);
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
            if (!entry.directory && !IsGeneratedPath(entry.name)) sizes[Lower(entry.name)] = {entry.name, entry.size};
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

using FileHashes = std::map<std::string, std::string>; // SHA-256 by path

// Names and contents of the files, whatever order they came in.
std::string HashOf(const FileHashes& hashes) {
    std::map<std::string, std::string> sorted;
    for (const auto& [path, hash] : hashes) sorted[Lower(path) + '\n' + path] = hash;
    std::string list;
    for (const auto& [path, hash] : sorted) list += path + '\n' + hash + '\n';
    char hex[65];
    jpt_sha256_buffer_hex(list.data(), list.size(), hex);
    return hex;
}

std::string HashFolder(const fs::path& dir, const std::vector<PackageFile>& files) {
    FileHashes hashes;
    std::vector<char> buffer(1 << 18);
    for (const PackageFile& file : files) {
        jpt_sha256_t sha;
        char hex[65];
        jpt_sha256_init(&sha);
        std::ifstream in(dir / PathFromUtf8(file.path), std::ios::binary);
        while (in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            jpt_sha256_update(&sha, buffer.data(), size_t(in.gcount()));
        }
        if (!in.eof()) return {};
        jpt_sha256_hex(&sha, hex);
        hashes[file.path] = hex;
    }
    return HashOf(hashes);
}

// Whole or not at all; a file that already says so is left alone.
bool WriteRecord(const fs::path& file, const std::string& text) {
    std::string current;
    std::error_code ec;
    if (ReadBytes(file, kRecordLimit, current) && current == text) return true;
    fs::create_directories(file.parent_path(), ec);
    fs::path temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    fs::rename(temporary, file, ec);
    if (ec) fs::remove(temporary, ec);
    return !ec;
}

// files: the package's own files; records: where the launcher's go (the same folder unless the package is linked).
bool WriteRecords(const fs::path& files, const fs::path& records, const PackageManifest& manifest) {
    const PackageAnalysis& a = manifest.analysis;
    if (trg::update::HasReparseAncestor(records / PathFromUtf8(LoadingPictureFile(0)), records)) return false;
    for (size_t i = 0; i < a.images.size(); ++i) {
        std::string bytes;
        if (a.images[i].state != "ready") continue;
        if (!ReadBytes(files / PathFromUtf8(a.images[i].path), kPictureLimit, bytes) ||
            !WriteRecord(records / PathFromUtf8(LoadingPictureFile(i)), MakeLoadingPicture(bytes)))
            return false;
    }
    return WriteRecord(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP, RuntimeMap(a)) && WriteRecord(records / kManifestFile, ManifestJson(manifest));
}

ArchiveError Unpack(ArchiveReader& reader, const PlannedFile& file, const fs::path& target, const ImportHooks& hooks,
                    uint64_t& done, uint64_t total, bool& write_failed, std::string& hash) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    uint64_t written = 0;
    bool cancelled = false;
    jpt_sha256_t sha;
    char hex[65];
    jpt_sha256_init(&sha);
    const ArchiveError error = reader.Extract(file.index, [&](const uint8_t* data, size_t size) {
        if (written + size > file.size) return false;
        jpt_sha256_update(&sha, data, size);
        out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        written += size;
        done += size;
        cancelled = hooks.progress && !hooks.progress(done, total);
        write_failed = !out;
        return !cancelled && !write_failed;
    });
    out.close();
    jpt_sha256_hex(&sha, hex);
    hash = hex;
    if (cancelled) return ArchiveError::kCancelled;
    if (write_failed || !out) {
        write_failed = true;
        return ArchiveError::kUnreadable;
    }
    if (error == ArchiveError::kCancelled || (error == ArchiveError::kNone && written != file.size)) return ArchiveError::kDamaged;
    return error;
}

std::string RecordedHash(const fs::path& dir) {
    std::string text;
    PackageOrigin origin;
    return ReadBytes(dir / kManifestFile, kRecordLimit, text) && ReadManifestOrigin(text, origin) ? origin.source_hash : std::string();
}

struct ImportSource {
    ArchiveReader& reader;
    std::string wanted_id;
    std::string type; // "zip", "7z", "rar", "folder"
    std::string name;
    fs::path path;
    bool folder() const { return type == "folder"; }
};

bool FormatProblem(const LevelScene& scene) {
    return scene.state == LevelState::kUnsupportedFormat || scene.state == LevelState::kDamaged || scene.state == LevelState::kUnknown || scene.state == LevelState::kUnsupported;
}

ImportStatus RejectionOf(const LevelScene& scene) {
    if (FormatProblem(scene) || scene.level_class() == LevelClass::kRejectedMalformed) return ImportStatus::kUnsupportedFormat;
    return scene.level_class() == LevelClass::kRejectedMissing ? ImportStatus::kMissingFiles : ImportStatus::kNotSupported;
}

// A package with a level that can be played is installed; each of its levels keeps its own state. One with none is refused.
ImportStatus Judge(const PackageAnalysis& a, const std::vector<LevelScene>& scenes, ImportResult& result) {
    result.levels = Playable(scenes);
    if (!a.playable()) return ImportStatus::kNoLevel;
    if (result.levels) return ImportStatus::kInstalled;
    const LevelScene* worst = nullptr;
    for (const LevelScene& scene : scenes) {
        if (!scene.rejected()) continue;
        const auto rank = [](const LevelScene& s) { return std::make_pair(FormatProblem(s), s.level_class()); };
        if (!worst || rank(scene) > rank(*worst)) worst = &scene;
    }
    if (worst) {
        result.detail = worst->file;
        return RejectionOf(*worst);
    }
    return result.levels ? ImportStatus::kInstalled : ImportStatus::kNoLevel;
}

int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// Of a package an earlier version installed: when its oldest file was written.
int64_t OldestFile(const fs::path& dir, const std::vector<PackageFile>& files) {
    int64_t oldest = 0;
    for (const PackageFile& file : files) {
        std::error_code ec;
        const auto written = fs::last_write_time(dir / PathFromUtf8(file.path), ec);
        if (ec) continue;
        const auto system = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now());
        const int64_t seconds = std::chrono::round<std::chrono::seconds>(system.time_since_epoch()).count();
        if (!oldest || seconds < oldest) oldest = seconds;
    }
    return oldest;
}

ImportResult Install(const ImportSource& source, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits, const fs::path& data_dir,
                     ImportRecord& record) {
    ArchiveReader& reader = source.reader;
    ImportResult result;
    std::vector<PlannedFile> plan;
    const auto fail = [&result](ImportStatus status, const std::string& detail) {
        result.status = status;
        result.detail = detail;
        return result;
    };
    const bool planned = PlanFiles(reader, limits, plan, result);
    for (const PlannedFile& file : plan) record.accepted.push_back(file.path);
    if (!planned) return result;
    if (source.folder() && HoldsSeveralPackages(plan, reader)) return fail(ImportStatus::kSeveralPackages, "");
    const fs::path levels = CustomLevelsDir(user_dir);
    uint64_t total = 0, done = 0;
    for (const PlannedFile& file : plan) {
        total += file.size;
#ifdef _WIN32
        if (levels.native().size() + std::max(PackageId(source.wanted_id).size() + kSuffixRoom, sizeof kStagingPrefix + 1) +
                PathFromUtf8(file.path).native().size() + 2 > kHostPathLimit)
            return fail(ImportStatus::kUnsafe, file.path);
#endif
    }
    Staging staging(levels);
    if (staging.dir().empty()) return fail(ImportStatus::kWriteFailed, Utf8(levels));
    std::error_code ec;
    const fs::space_info space = fs::space(levels, ec);
    if (!ec && space.available < total + kSpareBytes) return fail(ImportStatus::kNoSpace, "");
    if (hooks.phase) hooks.phase(kImportPhases[1]);
    FileHashes hashes;
    for (const PlannedFile& file : plan) {
        const fs::path target = staging.dir() / PathFromUtf8(file.path);
        bool write_failed = false;
        if (!trg::update::PathIsWithin(target, staging.dir())) return fail(ImportStatus::kUnsafe, file.path);
        const ArchiveError error = Unpack(reader, file, target, hooks, done, total, write_failed, hashes[file.path]);
        if (write_failed) return fail(ImportStatus::kWriteFailed, file.path);
        if (error != ArchiveError::kNone) return fail(FromArchive(error), error == ArchiveError::kCancelled ? std::string() : file.path);
    }
    if (hooks.phase) hooks.phase(kImportPhases[2]);
    const FileSizes sizes = SizesOf(staging.dir());
    if (sizes.size() != plan.size()) return fail(ImportStatus::kWriteFailed, Utf8(levels));
    for (const PlannedFile& file : plan) {
        const auto found = sizes.find(Lower(file.path));
        if (found == sizes.end() || found->second.size != file.size) return fail(ImportStatus::kWriteFailed, file.path);
    }
    PackageManifest manifest;
    const std::vector<PackageFile> files = PackageFiles(staging.dir());
    const PackageSource package = FolderSource(staging.dir(), files);
    manifest.analysis = AnalyzePackage(package);
    manifest.origin = {source.type, source.name, HashOf(hashes), RefusedNames(result.refused), Now()};
    NoteRefused(manifest.analysis, manifest.origin.refused);
    record.package = PackageId(source.wanted_id);
    record.analysis = manifest.analysis;
    record.scenes = ScenesOf(manifest.analysis, package, data_dir, staging.dir());
    FinishAnalysis(manifest.analysis, package, record.scenes);
    record.analysis = manifest.analysis;
    if (!data_dir.empty()) manifest.missing = MissingOf(record.scenes);
    manifest.verdicts = VerdictsOf(record.scenes);
    result.status = Judge(manifest.analysis, record.scenes, result);
    if (result.status != ImportStatus::kInstalled) return result;
    std::string problem;
    if (!RuntimeIsSafe(manifest.analysis, files, problem)) return fail(ImportStatus::kUnsafe, problem);
    const std::vector<std::string> installed = InstalledIds(levels);
    for (const std::string& id : installed) {
        const std::string recorded = RecordedHash(levels / id);
        const bool may_match = recorded.empty() || recorded == manifest.origin.source_hash;
        if (!may_match || !SameFiles(staging.dir(), sizes, levels / id, SizesOf(levels / id))) continue;
        result.package = id;
        return fail(ImportStatus::kAlreadyInstalled, id);
    }
    const auto taken = [&levels](const std::string& id) {
        std::error_code exists_ec;
        return fs::exists(fs::symlink_status(levels / id, exists_ec));
    };
    std::string id = PackageId(source.wanted_id);
    for (int n = 2; taken(id) && n < 100; ++n) id = PackageId(source.wanted_id) + "-" + std::to_string(n);
    manifest.package_id = id;
    if (!jpt_custom_level_id_valid(id.c_str()) || taken(id) || !WriteRecords(staging.dir(), staging.dir(), manifest) || !staging.Publish(levels / id))
        return fail(ImportStatus::kWriteFailed, id);
    result.status = ImportStatus::kInstalled;
    result.package = id;
    result.files = plan.size();
    return result;
}

std::string HashFile(const fs::path& file) {
    std::vector<char> buffer(1 << 18);
    jpt_sha256_t sha;
    char hex[65];
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    jpt_sha256_init(&sha);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        jpt_sha256_update(&sha, buffer.data(), size_t(in.gcount()));
    }
    jpt_sha256_hex(&sha, hex);
    return hex;
}

LogDocument DocumentOf(ArchiveReader& reader, size_t index) {
    const ArchiveEntry& entry = reader.entries()[index];
    LogDocument document;
    std::string bytes;
    char hex[65];
    document.path = entry.name;
    document.bytes = entry.size;
    if (entry.size > kTextLimit) return document;
    const ArchiveError error = reader.Extract(index, [&bytes](const uint8_t* data, size_t size) {
        bytes.append(reinterpret_cast<const char*>(data), size);
        return bytes.size() <= kTextLimit;
    });
    if (error != ArchiveError::kNone) return document;
    jpt_sha256_buffer_hex(bytes.data(), bytes.size(), hex);
    document.sha256 = hex;
    document.text = DecodeText(bytes.substr(0, kLogDocumentBytes));
    document.truncated = bytes.size() > kLogDocumentBytes;
    document.readable = IsText(bytes);
    document.readme = LevelReadmeScore(entry.name) >= kInstructionsScore;
    return document;
}

bool IsTextDocument(const std::string& path) {
    static constexpr const char* kTypes[] = {".txt", ".md", ".markdown", ".nfo", ".1st", ".me"};
    const std::string type = PathType(path);
    return LevelReadmeScore(path) > 0 || OneOf(type, kTypes);
}

void Describe(ArchiveReader& reader, ImportRecord& record) {
    size_t total = 0;
    for (size_t i = 0; i < reader.entries().size(); ++i) {
        const ArchiveEntry& entry = reader.entries()[i];
        if (entry.directory) continue;
        ++record.found;
        if (entry.link || entry.encrypted || !IsTextDocument(entry.name)) continue;
        if (record.documents.size() >= kLogDocuments || total >= kLogDocumentsBytes) {
            ++record.documents_skipped;
            continue;
        }
        record.documents.push_back(DocumentOf(reader, i));
        total += record.documents.back().text.size();
    }
}

void DescribeArchive(const fs::path& path, bool folder, ImportRecord& record) {
    std::error_code ec;
    record.path = Utf8(path);
    if (folder || !fs::is_regular_file(path, ec)) return;
    record.bytes = fs::file_size(path, ec);
    record.sha256 = HashFile(path);
}

ImportResult Import(const ImportSource& source, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits, const fs::path& data_dir) {
    ImportRecord record;
    record.type = source.type;
    record.name = source.name;
    DescribeArchive(source.path, source.folder(), record);
    Describe(source.reader, record);
    ImportResult result = Install(source, user_dir, hooks, limits, data_dir, record);
    if (!result.package.empty()) record.package = result.package;
    record.refused = result.refused;
    record.status = result.status;
    record.detail = result.detail;
    result.log = ImportLog(record);
    return result;
}

ImportResult Unopened(ImportStatus status, const std::string& type, const std::string& name, const fs::path& path) {
    ImportRecord record;
    ImportResult result;
    record.type = type;
    record.name = name;
    DescribeArchive(path, type == "folder", record);
    record.status = result.status = status;
    result.log = ImportLog(record);
    return result;
}

std::string ArchiveType(const fs::path& archive) {
    std::string head;
    if (!ReadHead(archive, 2, head)) return "zip";
    return head == "7z" ? "7z" : head == "Ra" ? "rar" : "zip";
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

// A linked package's files where they are: its folder, or the parts of it its link names, listed as the folder import lists one.
class LinkedReader final : public ArchiveReader {
public:
    ArchiveError Open(const PackageLink& link, const ArchiveLimits& limits) {
        root_ = link.root;
        if (link.scope.empty()) return Add(root_, "", limits);
        for (const std::string& part : link.scope) {
            const fs::path path = root_ / PathFromUtf8(part);
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(path, ec);
            if (ec || !fs::exists(status)) continue;
            if (entries_.size() == limits.max_entries) return ArchiveError::kTooLarge;
            if (IsReparsePoint(path, status) || trg::update::HasReparseAncestor(path, root_)) {
                entries_.push_back({part, 0, false, true, false});
            } else if (fs::is_directory(status)) {
                const ArchiveError error = Add(path, part + "/", limits);
                if (error != ArchiveError::kNone) return error;
            } else if (fs::is_regular_file(status)) {
                entries_.push_back({part, uint64_t(fs::file_size(path, ec)), false, false, false});
            }
        }
        return ArchiveError::kNone;
    }

    ArchiveError Extract(size_t index, const Sink& sink) override {
        std::ifstream in(root_ / PathFromUtf8(entries_[index].name), std::ios::binary);
        std::vector<char> buffer(1 << 20);
        while (in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            const size_t got = size_t(in.gcount());
            if (got && !sink(reinterpret_cast<const uint8_t*>(buffer.data()), got)) return ArchiveError::kCancelled;
        }
        return in.eof() ? ArchiveError::kNone : ArchiveError::kUnreadable;
    }

private:
    ArchiveError Add(const fs::path& folder, const std::string& prefix, const ArchiveLimits& limits) {
        ArchiveError error = ArchiveError::kNone;
        const std::unique_ptr<ArchiveReader> reader = OpenFolder(folder, limits, error);
        if (!reader) return error;
        for (const ArchiveEntry& entry : reader->entries()) {
            if (entries_.size() == limits.max_entries) return ArchiveError::kTooLarge;
            ArchiveEntry named = entry;
            named.name = prefix + entry.name;
            entries_.push_back(std::move(named));
        }
        return ArchiveError::kNone;
    }

    fs::path root_;
};

// What an imported copy of the package would hold: its level files, never a link or a generated name.
std::vector<PackageFile> LinkedFiles(const PackageLink& link) {
    LinkedReader reader;
    std::vector<PackageFile> files;
    if (reader.Open(link, {}) != ArchiveError::kNone) return files;
    for (const ArchiveEntry& entry : reader.entries())
        if (!entry.directory && !entry.link && IsLevelPackageFile(entry.name) && !IsGeneratedPath(entry.name)) files.push_back({entry.name, entry.size});
    std::sort(files.begin(), files.end(), [](const PackageFile& a, const PackageFile& b) { return Lower(a.path) < Lower(b.path); });
    return files;
}

struct KnownLink {
    std::string id;
    PackageLink link;
};

std::vector<KnownLink> KnownLinks(const fs::path& levels) {
    std::vector<KnownLink> known;
    for (const std::string& id : InstalledIds(levels)) {
        PackageLink link;
        if (ReadPackageLink(levels / id, link)) known.push_back({id, std::move(link)});
    }
    return known;
}

// One package of a linked folder, checked as the import checks one; nothing is written to its folder.
ImportStatus LinkOne(PackageLink link, const fs::path& levels, const fs::path& data_dir, const ArchiveLimits& limits, std::vector<KnownLink>& known,
                     LinkResult& result) {
    ImportRecord record;
    record.type = "external";
    record.name = link.name;
    record.path = Utf8(link.root) + (link.active_fm.empty() ? "" : " (fan mission " + link.active_fm + ")");
    const auto finish = [&](ImportStatus status, const std::string& detail) {
        record.status = status;
        record.detail = detail;
        for (std::string& line : ImportLog(record)) result.log.push_back(std::move(line));
        return status;
    };
    for (const KnownLink& other : known)
        if (SameLink(other.link, link)) {
            result.already.push_back(other.id);
            record.package = other.id;
            return finish(ImportStatus::kAlreadyLinked, other.id);
        }
    if (!GameCanOpen(link.root)) return finish(ImportStatus::kPathNotUsable, record.path);
    LinkedReader reader;
    const ArchiveError opened = reader.Open(link, limits);
    if (opened != ArchiveError::kNone) return finish(FromArchive(opened), "");
    Describe(reader, record);
    ImportResult import;
    std::vector<PlannedFile> plan;
    const bool planned = PlanFiles(reader, limits, plan, import);
    for (const PlannedFile& file : plan) record.accepted.push_back(file.path);
    record.refused = import.refused;
    if (!planned) return finish(import.status, import.detail);
#ifdef _WIN32
    for (const PlannedFile& file : plan)
        if (link.root.native().size() + 1 + PathFromUtf8(file.path).native().size() > kHostPathLimit) return finish(ImportStatus::kPathNotUsable, file.path);
#endif
    std::vector<PackageFile> files;
    for (const PlannedFile& file : plan) files.push_back({file.path, file.size});
    PackageSource source = FolderSource(link.root, files);
    source.active_fm = link.active_fm;
    link.identity = LinkIdentity(files);
    PackageManifest manifest;
    manifest.origin = {"external", link.name, "scenes " + link.identity, RefusedNames(import.refused), Now()};
    manifest.analysis = AnalyzePackage(source);
    NoteRefused(manifest.analysis, manifest.origin.refused);
    record.scenes = ScenesOf(manifest.analysis, source, data_dir);
    FinishAnalysis(manifest.analysis, source, record.scenes);
    record.analysis = manifest.analysis;
    if (!data_dir.empty()) manifest.missing = MissingOf(record.scenes);
    manifest.verdicts = VerdictsOf(record.scenes);
    const ImportStatus judged = Judge(manifest.analysis, record.scenes, import);
    if (judged != ImportStatus::kInstalled) return finish(judged, import.detail);
    std::string problem;
    if (!RuntimeIsSafe(manifest.analysis, files, problem)) return finish(ImportStatus::kUnsafe, problem);
    const auto taken = [&levels](const std::string& id) {
        std::error_code ec;
        return fs::exists(fs::symlink_status(levels / id, ec));
    };
    std::string id = PackageId(link.name);
    for (int n = 2; taken(id) && n < 100; ++n) id = PackageId(link.name) + "-" + std::to_string(n);
    manifest.package_id = id;
    Staging staging(levels);
    if (staging.dir().empty() || !jpt_custom_level_id_valid(id.c_str()) || taken(id) || !WritePackageLink(staging.dir(), link) ||
        !WriteRecords(link.root, staging.dir(), manifest) || !staging.Publish(levels / id))
        return finish(ImportStatus::kWriteFailed, id);
    record.package = id;
    result.linked.push_back(id);
    result.levels += import.levels;
    known.push_back({id, link});
    return finish(ImportStatus::kLinked, id);
}

// Only the files the launcher writes about a linked package; anything else in its records folder keeps it from going.
bool OnlyRecords(const fs::path& records) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(records, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        const fs::file_status status = it->symlink_status(type_ec);
        std::string relative = ForwardSlashes(Utf8(it->path().lexically_relative(records)));
        if (type_ec || IsReparsePoint(it->path(), status) || !(fs::is_regular_file(status) || fs::is_directory(status))) return false;
        if (fs::is_regular_file(status) && PathType(relative) == ".new") relative.resize(relative.size() - 4);
        if (!IsGeneratedPath(relative)) return false;
    }
    return !ec;
}

} // namespace

const char* ImportStatusTitle(ImportStatus status) {
    switch (status) {
    case ImportStatus::kInstalled: return "Custom level installed";
    case ImportStatus::kAlreadyInstalled: return "This level is already installed";
    case ImportStatus::kCancelled: return "Import cancelled. Nothing was changed.";
    case ImportStatus::kUnreadable: return "The files could not be read";
    case ImportStatus::kUnsupported: return "Unsupported archive. Extract it and use Import Folder";
    case ImportStatus::kDamaged: return "The archive is damaged";
    case ImportStatus::kUnsafe: return "Unsafe path in archive";
    case ImportStatus::kTooLarge: return "The package is too large";
    case ImportStatus::kNoLevel: return "No playable level was found";
    case ImportStatus::kUnsupportedFormat: return "This level is not supported by this port yet";
    case ImportStatus::kMissingFiles: return "This level is missing required files";
    case ImportStatus::kNotSupported: return "This custom level is not fully supported";
    case ImportStatus::kUnsafeToLoad: return "This level cannot be loaded safely";
    case ImportStatus::kSeveralPackages: return "This folder holds several levels. Choose the folder of one of them";
    case ImportStatus::kNoSpace: return "There is not enough free disk space";
    case ImportStatus::kWriteFailed: return "The level could not be written";
    case ImportStatus::kLinked: return "External folder linked";
    case ImportStatus::kAlreadyLinked: return "This folder is already linked";
    case ImportStatus::kFolderUnavailable: return "Folder unavailable";
    case ImportStatus::kPathNotUsable: return "The game cannot open this folder's path. Move it to a shorter, plainer path, or use Import Folder";
    }
    return "";
}

void ImportProgress::Raise(long long value) { done_ = std::max(done_, std::min(value, kTotal - 1)); }

void ImportProgress::Phase(const std::string& label) {
    static constexpr long long kStart[] = {0, 50, 900};
    for (size_t i = 0; i < std::size(kImportPhases); ++i)
        if (label == kImportPhases[i]) Raise(kStart[i]);
}

void ImportProgress::Bytes(uint64_t done, uint64_t total) {
    if (total) Raise(50 + (long long)(double(std::min(done, total)) / double(total) * 850.0));
}

ImportResult RunImport(const ImportRequest& request, const std::function<void(const std::string&, long long, long long)>& publish,
                       const std::function<bool()>& cancelled) {
    ImportProgress progress;
    std::string phase = kImportPhases[0];
    const auto tell = [&] { publish(phase, progress.done(), ImportProgress::kTotal); };
    tell();
    ImportHooks hooks;
    hooks.phase = [&](const char* text) {
        phase = text;
        progress.Phase(text);
        tell();
    };
    hooks.progress = [&](uint64_t done, uint64_t total) {
        progress.Bytes(done, total);
        tell();
        return !cancelled();
    };
    ImportResult result = request.folder ? ImportLevelFolder(request.source, request.user_dir, hooks, {}, request.data_dir)
                                         : ImportLevelArchive(request.source, request.user_dir, hooks, {}, request.data_dir);
    if (result.ok()) {
        progress.Complete();
        tell();
    }
    return result;
}

LevelNotice ImportNotice(ImportStatus status) {
    constexpr char kCouldNotImport[] = "Could not import this custom level";
    switch (status) {
    case ImportStatus::kInstalled:
    case ImportStatus::kLinked:
    case ImportStatus::kAlreadyLinked:
    case ImportStatus::kCancelled: return {ImportStatusTitle(status), "", false};
    case ImportStatus::kMissingFiles:
    case ImportStatus::kNotSupported:
    case ImportStatus::kUnsupportedFormat:
    case ImportStatus::kUnsafeToLoad: return {kCouldNotImport, ImportStatusTitle(ImportStatus::kNotSupported), true};
    default: break;
    }
    return {kCouldNotImport, ImportStatusTitle(status), true};
}

LevelNotice LinkNotice(ImportStatus status) {
    LevelNotice notice = ImportNotice(status);
    if (notice.failed) notice.title = "Could not link this folder";
    return notice;
}

fs::path CustomLevelsDir(const fs::path& user_dir) { return user_dir / JPT_CUSTOM_LEVELS_DIR; }

bool IsLevelPackageFile(const std::string& relative_path) {
    static constexpr const char* kTypes[] = {
        ".scn", ".grf", ".wtd", ".pid", ".swp", ".spz", ".spp", ".asa", ".asb", ".tpa", ".sky", ".ddf", ".smk", ".bmp", ".tga", ".dds",
        ".png", ".jpg", ".jpeg", ".gif", ".pcx", ".wav", ".mp3", ".ogg", ".txt", ".md", ".markdown", ".nfo", ".1st", ".me", ".pdf",
        ".rtf", ".ini", ".cfg", ".csv", ".nut"};
    const std::string type = PathType(relative_path);
    return type.empty() ? LevelReadmeScore(relative_path) > 0 : OneOf(type, kTypes);
}

ImportResult ImportLevelArchive(const fs::path& archive, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits,
                                const fs::path& data_dir) {
    ArchiveError error = ArchiveError::kNone;
    if (hooks.phase) hooks.phase(kImportPhases[0]);
    const std::unique_ptr<ArchiveReader> reader = OpenArchive(archive, limits, error);
    if (!reader) return Unopened(FromArchive(error), "archive", Utf8(archive.filename()), archive);
    return Import({*reader, Utf8(archive.stem()), ArchiveType(archive), Utf8(archive.filename()), archive}, user_dir, hooks, limits, data_dir);
}

ImportResult ImportLevelFolder(const fs::path& folder, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits,
                               const fs::path& data_dir) {
    ArchiveError error = ArchiveError::kNone;
    if (hooks.phase) hooks.phase(kImportPhases[0]);
    std::error_code ec;
    const bool overlaps = trg::update::PathIsWithin(folder, CustomLevelsDir(user_dir)) ||
                          trg::update::PathIsWithin(CustomLevelsDir(user_dir), folder);
    if (!fs::is_directory(folder, ec) || overlaps) return Unopened(overlaps ? ImportStatus::kUnsafe : ImportStatus::kUnreadable, "folder", FolderName(folder), folder);
    const std::unique_ptr<ArchiveReader> reader = OpenFolder(folder, limits, error);
    if (!reader) return Unopened(FromArchive(error), "folder", FolderName(folder), folder);
    return Import({*reader, FolderName(folder), "folder", FolderName(folder), folder}, user_dir, hooks, limits, data_dir);
}

LinkResult LinkLevelFolder(const fs::path& folder, const fs::path& user_dir, const ImportHooks& hooks, const ArchiveLimits& limits, const fs::path& data_dir) {
    LinkResult result;
    if (hooks.phase) hooks.phase(kLinkPhases[0]);
    std::error_code ec;
    const fs::path chosen = fs::weakly_canonical(fs::absolute(folder, ec), ec);
    const fs::path levels = CustomLevelsDir(user_dir);
    const auto fail = [&result](ImportStatus status, const std::string& detail) {
        result.status = status;
        result.detail = detail;
        result.log.push_back("link: " + std::string(ImportStatusTitle(status)) + (detail.empty() ? "" : " (" + detail + ")"));
        return result;
    };
    if (ec || !fs::is_directory(fs::symlink_status(chosen, ec))) return fail(ImportStatus::kUnreadable, Utf8(folder));
    // The port's own folders are never a package's.
    const fs::path versions = data_dir.empty() ? fs::path() : data_dir.parent_path().parent_path();
    for (const fs::path& own : {user_dir, versions})
        if (!own.empty() && (trg::update::PathIsWithin(chosen, own) || trg::update::PathIsWithin(own, chosen))) return fail(ImportStatus::kUnsafe, Utf8(chosen));
    std::vector<std::string> notes;
    const std::vector<LinkCandidate> candidates = FindLinkCandidates(chosen, notes);
    result.log.push_back("link " + Utf8(chosen) + ": " + std::to_string(candidates.size()) + " package(s)");
    for (const LinkCandidate& c : candidates) result.log.push_back("  " + c.path + (c.link.active_fm.empty() ? "" : ", fan mission of tp_mod.ini"));
    for (const std::string& note : notes) result.log.push_back("  " + note);
    if (candidates.empty()) return fail(ImportStatus::kNoLevel, "");
    if (hooks.phase) hooks.phase(kLinkPhases[1]);
    std::vector<KnownLink> known = KnownLinks(levels);
    std::optional<ImportStatus> refused;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (hooks.progress && !hooks.progress(i, candidates.size())) return fail(ImportStatus::kCancelled, "");
        const ImportStatus status = LinkOne(candidates[i].link, levels, data_dir, limits, known, result);
        if (status != ImportStatus::kLinked && status != ImportStatus::kAlreadyLinked && !refused) refused = status;
    }
    if (hooks.progress) hooks.progress(candidates.size(), candidates.size());
    result.status = !result.linked.empty() ? ImportStatus::kLinked : !result.already.empty() ? ImportStatus::kAlreadyLinked : refused.value_or(ImportStatus::kNoLevel);
    result.detail = std::to_string(result.linked.size()) + " linked, " + std::to_string(result.already.size()) + " linked before, " +
                    std::to_string(candidates.size() - result.linked.size() - result.already.size()) + " not supported";
    return result;
}

LinkResult RunLink(const ImportRequest& request, const std::function<void(const std::string&, long long, long long)>& publish, const std::function<bool()>& cancelled) {
    std::string phase = kLinkPhases[0];
    long long done = 0, total = 0;
    ImportHooks hooks;
    hooks.phase = [&](const char* text) {
        phase = text;
        publish(phase, done, total);
    };
    hooks.progress = [&](uint64_t now, uint64_t all) {
        done = (long long)now;
        total = (long long)all;
        publish(phase, done, total);
        return !cancelled();
    };
    return LinkLevelFolder(request.source, request.user_dir, hooks, {}, request.data_dir);
}

bool RescanLevelPackage(const fs::path& user_dir, const std::string& id) {
    const fs::path generated = CustomLevelsDir(user_dir) / id / JPT_CUSTOM_LEVEL_GENERATED_DIR;
    std::error_code ec;
    if (!jpt_custom_level_id_valid(id.c_str()) || !fs::is_directory(fs::symlink_status(generated, ec))) return false;
    for (const char* name : {"listing.txt", "worlds.txt"}) fs::remove(generated / name, ec);
    return true;
}

bool RelocateLevelPackage(const fs::path& user_dir, const std::string& id, const fs::path& folder, std::string& error) {
    const fs::path levels = CustomLevelsDir(user_dir), records = levels / id;
    PackageLink link;
    if (!jpt_custom_level_id_valid(id.c_str()) || !ReadPackageLink(records, link)) {
        error = "not a linked package";
        return false;
    }
    std::error_code ec;
    PackageLink moved = link;
    moved.root = fs::weakly_canonical(fs::absolute(folder, ec), ec);
    if (ec || !LinkAvailable(moved)) {
        error = "not a folder that can be read";
        return false;
    }
    if (!GameCanOpen(moved.root)) {
        error = "the game cannot open files by this folder's path";
        return false;
    }
    if (trg::update::PathIsWithin(moved.root, user_dir) || trg::update::PathIsWithin(user_dir, moved.root)) {
        error = "a folder of the port";
        return false;
    }
    const std::vector<PackageFile> files = LinkedFiles(moved);
    if (files.empty() || (!link.identity.empty() && LinkIdentity(files) != link.identity)) {
        error = "the folder does not hold this package's levels";
        return false;
    }
    for (const KnownLink& other : KnownLinks(levels))
        if (other.id != id && SameLink(other.link, moved)) {
            error = "the folder is already linked as " + other.id;
            return false;
        }
    if (!WritePackageLink(records, moved)) {
        error = "the link could not be written";
        return false;
    }
    RescanLevelPackage(user_dir, id);
    return true;
}

bool UnlinkLevelPackage(const fs::path& user_dir, const std::string& id, std::string& error) {
    const fs::path levels = CustomLevelsDir(user_dir), records = levels / id;
    PackageLink link;
    std::error_code ec;
    if (!jpt_custom_level_id_valid(id.c_str()) || !trg::update::PathIsWithin(records, levels) || !fs::is_directory(fs::symlink_status(records, ec)) ||
        !ReadPackageLink(records, link)) {
        error = "not a linked package";
        return false;
    }
    if (!OnlyRecords(records)) {
        error = "its records folder holds other files: " + Utf8(records);
        return false;
    }
    const fs::path removing = levels / (kRemovingPrefix + id);
    fs::remove_all(removing, ec);
    if (!Rename(records, removing)) {
        error = "a file of the package is in use";
        return false;
    }
    fs::remove_all(removing, ec);
    return true;
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

std::vector<const LevelScene*> LevelPackage::levels() const {
    std::vector<const LevelScene*> list;
    for (const LevelScene& scene : scenes) {
        if (!scene.playable()) continue;
        if (scene.start) return {&scene};
        list.push_back(&scene);
    }
    return list;
}

std::vector<const LevelScene*> LevelPackage::rows() const {
    const std::vector<const LevelScene*> playable = levels();
    if (!playable.empty() && playable[0]->start) return playable;
    const bool mission = layout != "loose" && layout != "data_folder";
    std::vector<const LevelScene*> list;
    for (const LevelScene& scene : scenes) {
        if (scene.auxiliary || scene.state == LevelState::kAuxiliary) continue;
        list.push_back(&scene);
    }
    // Without a StartLevel, the scenes of a mission that another one leads to are parts of it.
    const auto root = [](const LevelScene* s) { return s->playable() && s->led_to_by.empty(); };
    if (mission && std::count_if(list.begin(), list.end(), root) == 1)
        list.erase(std::remove_if(list.begin(), list.end(), [](const LevelScene* s) { return s->playable() && !s->led_to_by.empty(); }), list.end());
    return list;
}

const char* LevelStatusText(const LevelScene& scene) { return scene.playable() ? "Ready" : "Not supported"; }

std::vector<std::string> LevelNotes(const LevelScene& scene) {
    std::vector<std::string> notes;
    if (!scene.playable()) return notes;
    for (const LevelIssue& i : scene.issues)
        if (i.code == "MISSING_TRANSITION_LEVEL") notes.push_back(i.subject);
    return notes;
}

RowActions ActionsOf(const LevelScene* scene, bool has_readme) {
    RowActions actions;
    actions.play = scene && scene->playable();
    actions.readme = actions.play && has_readme;
    actions.report = !actions.play;
    return actions;
}

const char* LevelSortKey(LevelSort sort) {
    switch (sort) {
    case LevelSort::kOldest: return "oldest";
    case LevelSort::kNameUp: return "name";
    case LevelSort::kNameDown: return "name_down";
    case LevelSort::kNewest: break;
    }
    return "newest";
}

LevelSort LevelSortOf(const std::string& key) {
    for (const LevelSort sort : {LevelSort::kOldest, LevelSort::kNameUp, LevelSort::kNameDown})
        if (key == LevelSortKey(sort)) return sort;
    return LevelSort::kNewest;
}

std::vector<LevelRow> SortedRows(const std::vector<LevelPackage>& packages, LevelSort sort) {
    std::vector<LevelRow> rows;
    for (const LevelPackage& package : packages) {
        const std::vector<const LevelScene*> scenes = package.rows();
        if (scenes.empty()) rows.push_back({&package, nullptr});
        for (const LevelScene* scene : scenes) rows.push_back({&package, scene});
    }
    const auto name = [](const LevelRow& r) { return std::make_pair(Lower(r.package->id), r.scene ? Lower(r.scene->file) : std::string()); };
    const auto start = [](const LevelRow& r) { return r.scene && r.scene->start ? 0 : 1; };
    std::stable_sort(rows.begin(), rows.end(), [&](const LevelRow& a, const LevelRow& b) {
        if (sort == LevelSort::kNameUp) return name(a) < name(b);
        if (sort == LevelSort::kNameDown) return name(b) < name(a);
        if (a.package->imported_at != b.package->imported_at)
            return sort == LevelSort::kNewest ? a.package->imported_at > b.package->imported_at : a.package->imported_at < b.package->imported_at;
        return std::make_pair(start(a), name(a)) < std::make_pair(start(b), name(b));
    });
    return rows;
}

size_t PlayableLevels(const std::vector<LevelPackage>& packages) {
    size_t count = 0;
    for (const LevelPackage& package : packages) count += package.levels().size();
    return count;
}

namespace {

// The guide of each level the game can start (launcher/level_guide.h): rewritten after an analysis, else written when it is missing.
std::vector<std::string> WriteGuides(const fs::path& package_dir, const fs::path& records, const fs::path& data_dir, const std::vector<LevelScene>& scenes,
                                     bool missing_only) {
    std::vector<std::string> log;
    for (const LevelScene& scene : scenes) {
        if (!scene.playable()) continue;
        std::error_code ec;
        const fs::path out = records / PathFromUtf8(kLevelGuideFolder) / PathFromUtf8(LevelGuideName(scene.file));
        if (missing_only && fs::is_regular_file(out, ec)) continue;
        const auto grf = std::find_if(scene.resources.begin(), scene.resources.end(), [](const LevelResource& r) { return r.type == "GRF" && !r.place.empty(); });
        if (grf == scene.resources.end()) continue;
        const bool in_game = grf->place == kInstalledGame;
        const fs::path world = in_game ? GameFile(data_dir, grf->name) : package_dir / PathFromUtf8(grf->place);
        size_t size = 0;
        char problem[256] = "";
        uint8_t* bytes = world.empty() ? nullptr : jpt_level_world_load(Utf8(world).c_str(), &size);
        jpt_groff_t* g = bytes ? jpt_groff_open(grf->name.c_str(), bytes, size, problem, sizeof problem) : nullptr;
        if (g) {
            const LevelGuide guide = BuildLevelGuide(g);
            const fs::path temp = fs::path(out).concat(".tmp");
            fs::create_directories(out.parent_path(), ec);
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file << LevelGuideText(guide, grf->name, in_game);
            }
            fs::rename(temp, out, ec);
            log.push_back("guide of " + scene.file + (ec ? ": could not be written" : ": " + LevelGuideSummary(guide)));
            jpt_groff_close(g);
        } else {
            log.push_back("guide of " + scene.file + ": " + grf->name + " could not be read" + (problem[0] ? std::string(", ") + problem : std::string()));
        }
        std::free(bytes);
    }
    return log;
}

// A linked package's scenes changed in its folder: what Relocate Folder compares with follows them, and the log says that saves
// of a level that is gone cannot be loaded.
void FollowLinkedScenes(LevelPackage& package, const std::vector<PackageFile>& files) {
    const std::string identity = LinkIdentity(files);
    if (!package.linked || package.link.identity == identity) return;
    if (!package.link.identity.empty()) package.log.push_back("its folder's levels changed since they were last read: a save of a level that is gone cannot be loaded");
    package.link.identity = identity;
    if (!WritePackageLink(package.records, package.link)) package.log.push_back("its link could not be updated");
}

LevelPackage AnalysePackage(const fs::path& user_dir, const fs::path& data_dir, const std::string& id) {
    LevelPackage package;
    PackageManifest manifest;
    package.id = manifest.package_id = id;
    package.dir = package.records = CustomLevelsDir(user_dir) / id;
    package.linked = ReadPackageLink(package.records, package.link);
    if (package.linked) package.dir = package.link.root;
    if (package.linked && !LinkAvailable(package.link)) {
        package.available = false;
        return package;
    }
    const std::vector<PackageFile> files = package.linked ? LinkedFiles(package.link) : PackageFiles(package.dir);
    PackageSource source = FolderSource(package.dir, files);
    source.active_fm = package.link.active_fm;
    manifest.analysis = AnalyzePackage(source);
    const PackageAnalysis& a = manifest.analysis;
    std::string record, problem;
    const bool found = ReadBytes(package.records / kManifestFile, kRecordLimit, record), newer = found && ManifestIsNewer(record);
    if (!newer && (!found || !ReadManifestOrigin(record, manifest.origin)))
        manifest.origin = package.linked ? PackageOrigin{"external", package.link.name, "scenes " + LinkIdentity(files), {}, 0}
                                         : PackageOrigin{"migrated", id, HashFolder(package.dir, files), {}, 0};
    if (!manifest.origin.imported_at)
        manifest.origin.imported_at = package.linked ? OldestFile(package.records, {{JPT_CUSTOM_LEVEL_LINK_FILE, 0}}) : OldestFile(package.dir, files);
    NoteRefused(manifest.analysis, manifest.origin.refused);
    FollowLinkedScenes(package, files);
    package.origin = manifest.origin.source_type;
    package.imported_at = manifest.origin.imported_at;
    package.layout = a.layout;
    package.scenes = ScenesOf(a, source, data_dir, package.records);
    FinishAnalysis(manifest.analysis, source, package.scenes);
    if (!data_dir.empty()) manifest.missing = MissingOf(package.scenes);
    manifest.verdicts = VerdictsOf(package.scenes);
    package.recorded = !newer && RuntimeIsSafe(a, files, problem) && WriteRecords(package.dir, package.records, manifest);
    if (package.recorded && !data_dir.empty())
        for (std::string& line : WriteGuides(package.dir, package.records, data_dir, package.scenes, false)) package.log.push_back(std::move(line));
    package.readme = a.readme;
    package.ce = a.need("Trespasser CE");
    package.atx = a.need("ATX");
    package.config_warnings = a.config_warnings();
    package.log.push_back("layout " + a.layout + (a.root.empty() ? "" : ", mod folder " + a.root) + ", start level " + (a.start_level.empty() ? "none" : a.start_level));
    for (const Warning& w : a.warnings) package.log.push_back(w.code + ": " + w.detail);
    for (const std::string& name : manifest.missing) package.log.push_back("missing required: " + name);
    if (!package.recorded) package.log.push_back("its manifest could not be written: the package is read by its folders alone");
    return package;
}

LevelPackage ReadPackage(const fs::path& user_dir, const fs::path& data_dir, const std::string& id, const std::string& game_key) {
    const fs::path dir = CustomLevelsDir(user_dir) / id;
    LevelPackage package;
    PackageLink link;
    const bool linked = ReadPackageLink(dir, link);
    const auto describe = [&](LevelPackage& p) {
        p.id = id;
        p.records = dir;
        p.linked = linked;
        p.link = link;
        p.dir = linked ? link.root : dir;
        p.cached = true;
    };
    // Its folder is gone: shown as it was last read, never analysed or played until it is back or relocated.
    if (linked && !LinkAvailable(link)) {
        ReadLastListingRecord(dir, package);
        describe(package);
        package.available = false;
        package.origin = "external";
        return package;
    }
    if (ReadListingRecord(dir, PackageListingKey(dir, game_key, linked ? &link : nullptr), package)) {
        describe(package);
        return package;
    }
    package = AnalysePackage(user_dir, data_dir, id);
    if (package.recorded) WriteListingRecord(dir, PackageListingKey(dir, game_key, package.linked ? &package.link : nullptr), package);
    return package;
}

LevelPackage ReadPackage(const fs::path& user_dir, const fs::path& data_dir, const std::string& id) {
    return ReadPackage(user_dir, data_dir, id, GameListingKey(data_dir));
}

} // namespace

namespace {

fs::path s_game_data;

int FindGameFile(const char* relative, char* out, size_t cap) {
    const std::string name = PathName(relative), folder = PathFolder(relative);
    for (const fs::path& root : {s_game_data.parent_path(), s_game_data}) {
        const fs::path file = GameFile(folder.empty() ? root : root / PathFromUtf8(folder), name);
        if (file.empty()) continue;
        const int n = std::snprintf(out, cap, "%s", file.string().c_str());
        return n > 0 && size_t(n) < cap;
    }
    return 0;
}

// The runtime's own lookup, with the package active as it is while the level plays.
class RuntimeLookup {
public:
    RuntimeLookup(const fs::path& user_dir, const fs::path& data_dir, const std::string& id, const std::string& scene) {
        jpt_custom_level_request_t request{};
        char error[256] = "";
        std::snprintf(request.package, sizeof request.package, "%s", id.c_str());
        std::snprintf(request.scene, sizeof request.scene, "%s", scene.c_str());
        s_game_data = data_dir;
        active_ = jpt_custom_level_activate(user_dir.string().c_str(), &request, error, sizeof error) != 0;
        if (active_) jpt_custom_level_set_game(FindGameFile);
        error_ = error;
    }
    ~RuntimeLookup() { jpt_custom_level_deactivate(); }
    RuntimeLookup(const RuntimeLookup&) = delete;
    RuntimeLookup& operator=(const RuntimeLookup&) = delete;

    bool active() const { return active_; }
    const std::string& error() const { return error_; }
    bool Finds(const std::string& name) const {
        char file[JPT_PATH_MAX];
        std::error_code ec;
        if (jpt_custom_level_find(name.c_str(), file, sizeof file)) return fs::is_regular_file(file, ec);
        return jpt_custom_level_game_file(name.c_str(), file, sizeof file) != 0;
    }

private:
    bool active_ = false;
    std::string error_;
};

// Other installed versions' copies: named in the log, never used.
std::string ElsewhereInGame(const fs::path& data_dir, const std::string& name) {
    std::string found;
    std::error_code ec;
    if (data_dir.empty()) return found;
    const fs::path versions = data_dir.parent_path().parent_path();
    for (fs::directory_iterator it(versions, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path other = it->path() / data_dir.filename();
        if (other != data_dir && InGame(other, name)) found += (found.empty() ? "" : ", ") + Utf8(it->path().filename());
    }
    return found;
}

} // namespace

std::vector<std::string> ReportLines(const LevelPackage& package, const LevelScene* scene, const fs::path& data_dir) {
    std::vector<std::string> lines;
    if (!scene) lines.push_back("custom level " + package.id + ": no level of the package can be played");
    for (const LevelScene& each : package.scenes)
        if (scene ? &each == scene : each.rejected())
            for (const std::string& line : LevelProblems(package, each, data_dir)) lines.push_back("custom level: " + line);
    return lines;
}

std::vector<std::string> LevelProblems(const LevelPackage& package, const LevelScene& scene, const fs::path& data_dir) {
    std::vector<std::string> lines = {"package " + package.id + ", level " + scene.path + ": " + LevelClassName(scene.level_class())};
    for (const LevelIssue& issue : scene.issues) {
        lines.push_back("  " + issue.code + " " + issue.subject + ": " + issue.detail);
        if (issue.policy != IssuePolicy::kMissing) continue;
        const std::string folder = PathFolder(scene.path), other = ElsewhereInGame(data_dir, issue.subject);
        lines.push_back("    looked in: the package's " + (folder.empty() ? std::string("own folder") : folder) + ", its other folders, " + (data_dir.empty() ? std::string("no installed game") : Utf8(data_dir)));
        lines.push_back("    alternative: " + (other.empty() ? std::string("none") : "in another installed version, which a level of this one cannot use: " + other));
    }
    for (const LevelResource& r : scene.resources) lines.push_back("  " + r.type + " " + r.name + ": " + (!r.place.empty() ? r.place : !r.note.empty() ? r.note : "missing"));
    return lines;
}

PlayCheck CheckBeforePlay(const fs::path& user_dir, const fs::path& data_dir, const std::string& id, const std::string& scene) {
    PlayCheck check;
    const LevelPackage package = jpt_custom_level_id_valid(id.c_str()) ? ReadPackage(user_dir, data_dir, id) : LevelPackage();
    if (!package.available) {
        check.reason = ImportStatus::kFolderUnavailable;
        check.log.push_back("package " + id + ": its linked folder " + Utf8(package.link.root) + " is not available");
        check.log.push_back(std::string("  result: BLOCKED_PRECHECK, not started, ") + ImportStatusTitle(check.reason));
        return check;
    }
    const auto level = std::find_if(package.scenes.begin(), package.scenes.end(), [&scene](const LevelScene& s) { return s.path == scene; });
    if (level == package.scenes.end()) {
        check.log.push_back("package " + id + " has no level " + scene);
        return check;
    }
    check.layout = package.layout;
    check.log = LevelProblems(package, *level, data_dir);
    check.reason = RejectionOf(*level);
    check.ok = level->playable();
    if (check.ok) {
        const RuntimeLookup runtime(user_dir, data_dir, id, scene);
        if (!runtime.active()) check.ok = false, check.reason = ImportStatus::kUnsafeToLoad, check.log.push_back("  " + runtime.error());
        for (const LevelResource& r : level->resources) {
            if (!check.ok || r.place.empty() || r.type == "SCN" || runtime.Finds(r.name)) continue;
            check.ok = false;
            check.reason = ImportStatus::kMissingFiles;
            check.log.push_back("  " + r.type + " " + r.name + " was found in " + r.place + ", but the runtime does not find it");
        }
    }
    if (check.ok)
        for (const std::string& line : WriteGuides(package.dir, package.records, data_dir, package.scenes, true)) check.log.push_back("  " + line);
    check.log.push_back(std::string("  result: ") + (check.ok ? "started" : std::string("BLOCKED_PRECHECK, not started, ") + ImportStatusTitle(check.reason)));
    return check;
}

std::vector<LevelPackage> ListLevelPackages(const fs::path& user_dir, const fs::path& data_dir, ListingStats* stats, const std::atomic<bool>* cancel) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); };
    ListingStats local;
    ListingStats& s = stats ? *stats : local;
    Clock::time_point start = Clock::now();
    const std::string game_key = GameListingKey(data_dir);
    s.game_ms = ms(start);
    std::vector<LevelPackage> packages;
    for (const std::string& id : InstalledIds(CustomLevelsDir(user_dir))) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            s.cancelled = true;
            break;
        }
        start = Clock::now();
        packages.push_back(ReadPackage(user_dir, data_dir, id, game_key));
        const bool cached = packages.back().cached;
        (cached ? s.cached_ms : s.analysed_ms) += ms(start);
        ++(cached ? s.cached : s.analysed);
    }
    return packages;
}

std::string ArchiveSha256(const fs::path& archive) { return HashFile(archive); }

std::vector<std::string> RequiredFeatures(const PackageAnalysis& a, const LevelScene& scene) {
    std::set<std::string> names(a.required.begin(), a.required.end());
    for (const auto& [world, facts] : scene.worlds)
        for (const std::string& name : RequiredByWorld(facts)) names.insert(name);
    if (scene.version > JPT_SCENE_VERSION_OWN) names.insert(scene.version >= JPT_SCENE_VERSION_SCRIPTS ? "CE_SCN_26" : "CE_SCN_23_25");
    for (const AudioPack& pack : a.audio)
        if (pack.state == "ready") names.insert(pack.additive ? "CE_TPA_ADDED" : "CE_TPA_REPLACED");
    for (const LoadingImage& image : a.images)
        if (image.state == "ready") names.insert("CE_LOADING_IMAGE");
    if (a.skip_win_movie) names.insert("CE_WIN_MOVIE");
    if (scene.scripted) names.insert("CE_SCRIPTS");
    if (!scene.script_required.empty()) names.insert("CE_SCRIPT_API");
    static constexpr std::pair<const char*, const char*> kSettings[] = {
        {"dual_stow", "CE_DUAL_STOW"}, {"enhanced_sky", "CE_ENHANCED_SKY"}, {"water_refraction", "CE_WATER_REFRACTION"}, {"active_ai", "CE_MAX_ACTIVE_AI"}};
    for (const SessionOverride& o : a.overrides)
        for (const auto& [key, name] : kSettings)
            if (o.key == key) names.insert(name);
    for (const SessionLimit& l : a.limits)
        if (l.key == "terrain_nodes") names.insert("CE_TERRAIN_NODES");
        else if (l.key == "terrain_cull_mm") names.insert("CE_TERRAIN_CULL_BIAS");
    return {names.begin(), names.end()};
}

AnalysisResult AnalyzeLevelSource(const fs::path& path, const fs::path& data_dir, const ArchiveLimits& limits) {
    AnalysisResult result;
    ArchiveError error = ArchiveError::kNone;
    std::error_code ec;
    const bool folder = fs::is_directory(path, ec);
    const std::unique_ptr<ArchiveReader> reader = folder ? OpenFolder(path, limits, error) : OpenArchive(path, limits, error);
    if (!reader) {
        result.import.status = FromArchive(error);
        return result;
    }
    ImportRecord record;
    record.type = folder ? "folder" : ArchiveType(path);
    record.name = Utf8(path.filename());
    DescribeArchive(path, folder, record);
    result.type = record.type;
    result.sha256 = record.sha256;
    result.bytes = record.bytes;
    Describe(*reader, record);
    std::vector<PlannedFile> plan;
    const bool planned = PlanFiles(*reader, limits, plan, result.import), several = planned && folder && HoldsSeveralPackages(plan, *reader);
    if (several) result.import.status = ImportStatus::kSeveralPackages;
    if (!planned || several) {
        for (const PlannedFile& file : plan) record.accepted.push_back(file.path);
        record.refused = result.import.refused;
        record.status = result.import.status;
        record.detail = result.import.detail;
        result.import.log = ImportLog(record);
        return result;
    }
    PackageSource source;
    std::map<std::string, PlannedFile> by_path;
    for (const PlannedFile& file : plan) {
        source.files.push_back({file.path, file.size});
        by_path.emplace(file.path, file);
    }
    ArchiveError unread = ArchiveError::kNone; // the first file that could not be read
    std::string unread_file;
    const auto extract = [&](const std::string& name, uint64_t most, bool whole, std::string& bytes) {
        const auto it = by_path.find(name);
        bytes.clear();
        if (it == by_path.end() || (whole && it->second.size > most)) return false;
        const ArchiveError done = reader->Extract(it->second.index, [&](const uint8_t* data, size_t size) {
            bytes.append(reinterpret_cast<const char*>(data), size_t(std::min<uint64_t>(size, most - bytes.size())));
            return bytes.size() < most;
        });
        const bool got = done == ArchiveError::kNone || done == ArchiveError::kCancelled, ok = got && (!whole || bytes.size() == it->second.size);
        if (!ok && unread == ArchiveError::kNone) {
            unread = got ? ArchiveError::kDamaged : done;
            unread_file = name;
        }
        return ok;
    };
    source.read = [&](const std::string& name, uint64_t limit, std::string& bytes) { return extract(name, limit, true, bytes); };
    source.head = [&](const std::string& name, size_t count, std::string& bytes) { return extract(name, count, false, bytes); };
    PackageManifest manifest;
    manifest.package_id = PackageId(folder ? FolderName(path) : Utf8(path.stem()));
    manifest.origin = {record.type, record.name, "", RefusedNames(result.import.refused)};
    manifest.analysis = AnalyzePackage(source);
    NoteRefused(manifest.analysis, manifest.origin.refused);
    std::string problem;
    result.import.files = plan.size();
    result.import.package = manifest.package_id;
    record.package = manifest.package_id;
    for (const PackageFile& file : source.files) record.accepted.push_back(file.path);
    record.refused = result.import.refused;
    record.analysis = manifest.analysis;
    record.scenes = ScenesOf(manifest.analysis, source, data_dir);
    if (unread != ArchiveError::kNone) {
        record.analysis = {};
        record.scenes.clear();
        record.status = result.import.status = FromArchive(unread);
        record.detail = result.import.detail = unread_file;
        result.import.log = ImportLog(record);
        return result;
    }
    FinishAnalysis(manifest.analysis, source, record.scenes);
    record.analysis = manifest.analysis;
    if (!data_dir.empty()) manifest.missing = MissingOf(record.scenes);
    manifest.verdicts = VerdictsOf(record.scenes);
    result.import.status = Judge(manifest.analysis, record.scenes, result.import);
    if (result.import.ok() && !RuntimeIsSafe(manifest.analysis, source.files, problem)) result.import.status = ImportStatus::kUnsafe, result.import.detail = problem;
    record.status = result.import.status;
    record.detail = result.import.detail;
    result.import.log = ImportLog(record);
    result.report = AnalysisReport(manifest);
    result.json = ManifestJson(manifest);
    result.analysis = std::move(manifest.analysis);
    result.scenes = std::move(record.scenes);
    result.files = std::move(source.files);
    return result;
}

bool DeleteLevelPackage(const fs::path& user_dir, const std::string& id, std::string& error) {
    const fs::path levels = CustomLevelsDir(user_dir);
    std::error_code ec;
    PackageLink link;
    if (!jpt_custom_level_id_valid(id.c_str()) || !fs::is_directory(fs::symlink_status(levels / id, ec)) ||
        !trg::update::PathIsWithin(levels / id, levels) || ReadPackageLink(levels / id, link)) {
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

bool ResolveReadme(const fs::path& user_dir, const LevelPackage& package, fs::path& file) {
    const fs::path records = CustomLevelsDir(user_dir) / package.id;
    PackageLink link;
    const fs::path home = jpt_custom_level_id_valid(package.id.c_str()) && ReadPackageLink(records, link) ? link.root : records;
    const fs::path candidate = package.dir / PathFromUtf8(package.readme);
    std::error_code ec;
    if (package.readme.empty() || !LevelReadmeScore(package.readme) || !jpt_custom_level_id_valid(package.id.c_str()) ||
        package.dir.lexically_normal() != home.lexically_normal() || !trg::update::PathIsWithin(candidate, home) ||
        trg::update::HasReparseAncestor(candidate, home) || !fs::is_regular_file(fs::symlink_status(candidate, ec))) {
        return false;
    }
    file = candidate;
    return true;
}

bool ReadLevelText(const fs::path& file, std::string& text) {
    std::string bytes;
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec)) || !ReadBytes(file, kTextLimit, bytes)) return false;
    text = DecodeText(bytes);
    return true;
}

} // namespace jpt
