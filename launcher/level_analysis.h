// What a level package is and how the game will see it, worked out from its files alone: the folder layout, fm.ini, tp_mod.ini,
// trespasser.ini, tpass.ini and the readme files. Reads only; the importer, the Custom Levels page and
// --analyze-custom-level all use this.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jpt_custom_level.h"
#include "jpt_scene.h"
#include "level_readme.h"
#include "level_settings.h"

namespace jpt {

struct PackageFile {
    std::string path; // relative, '/' separators
    uint64_t size = 0;
};

struct PackageSource {
    std::vector<PackageFile> files;
    // Whole files up to a size, and the first bytes of a file of any size.
    std::function<bool(const std::string& path, uint64_t limit, std::string& bytes)> read;
    std::function<bool(const std::string& path, size_t count, std::string& bytes)> head;
};

enum class Origin { kLayout, kFmIni, kTpModIni, kTrespasserIni, kReadme, kHeuristic };
const char* OriginName(Origin origin);

struct InstallAction {
    std::string kind; // "root", "layer", "move", "keep", "settings", "ignore"
    std::string source, target;
    Origin origin = Origin::kLayout;
    std::string reason;
};

struct Warning {
    std::string code, detail;
};

struct Dependency {
    std::string name;
    Need need = Need::kNone;
    std::string evidence;
};

struct Feature {
    std::string name;
    std::string state; // "supported", "mapped", "not_required", "unsupported"
};

struct ConfigFile {
    SettingFile kind = SettingFile::kFmIni;
    std::string path;
    bool used = false; // read for this package's session; the others are kept for reference
};

struct ConfigSetting {
    SettingFile kind = SettingFile::kFmIni;
    std::string path;
    IniEntry entry;
    SettingMeaning meaning;
};

struct SessionOverride {
    std::string key, value, source;
};

enum class AudioKind { kStream, kEffects, kAmbient };
const char* AudioKindName(AudioKind kind);

// An engine limit of the session: "mesh_faces", "mesh_heap_mb", "terrain_nodes", "texture_pool_mb".
struct SessionLimit {
    std::string key;
    long value = 0;
    std::string source;
};

struct AudioPack {
    AudioKind kind = AudioKind::kStream;
    std::string scene; // empty: every level of the package
    bool additive = true;
    std::string declared;
    std::string path; // empty when it is not used
    std::string state; // "ready", "missing", "ambiguous", "unsafe", "not_a_tpa"
};

struct LoadingImage {
    std::string scene;
    std::string declared;
    std::string path;
    std::string state; // "ready", "missing", "ambiguous", "unsafe", "unreadable", "unsupported_location"
};

struct LevelInfo {
    std::string path;
    std::string file;
    std::string display_name;
    std::vector<std::string> worlds;
    int version = 0; // of the scene's format; 0: not known
    std::string problem; // why its format cannot be read
    std::vector<std::string> left_out; // what of its format the port has no use for
    bool valid = false;
    bool start = false;
    bool configured = false; // in the active mod: fm.ini and the sound files apply to it
    bool auxiliary = false;
    std::string auxiliary_reason;
};

// A file as the game sees it, relative to the game folder: "data/anim/x.asa", "Map/as2/x.bmp".
struct RuntimeFile {
    std::string target, source;
    Origin origin = Origin::kLayout;
    bool active = true; // false: kept for a feature the port does not have yet
};

struct PackageAnalysis {
    std::string layout; // "loose", "data_folder", "fan_mission", "mod_tree", "standalone", "unknown"
    std::string root;   // the active mod's folder, empty for the package itself
    std::string display_name;
    std::string start_level; // a path of the package
    bool skip_win_movie = false;
    std::vector<LevelInfo> levels; // the start level first
    std::vector<std::string> readmes;
    std::string readme;
    std::vector<ConfigFile> configs;
    std::vector<ConfigSetting> settings;
    std::vector<SessionOverride> overrides;
    std::vector<SessionLimit> limits;
    std::vector<AudioPack> audio;
    std::vector<LoadingImage> images;
    std::vector<std::string> directories; // "anim", "texhw", "menu", "maps"
    std::vector<Dependency> dependencies;
    std::vector<Feature> features;
    std::vector<std::string> required; // of the package, by name in jpt_level_features
    std::vector<int> action_types;     // past the game's own, of every level
    std::vector<InstallAction> actions;
    std::vector<RuntimeFile> runtime;
    std::vector<Warning> warnings;

    bool playable() const;
    Need need(const char* dependency) const;
    bool config_warnings() const;
};

PackageAnalysis AnalyzePackage(const PackageSource& source);


// Below a folder of documents, where engine packages keep their samples.
bool IsDocumentation(const std::string& path);
int LevelReadmeScore(const std::string& path); // 0: not instructions
std::string ChooseReadme(const std::vector<std::string>& relative_paths);
// UTF-8 and UTF-16, with or without a BOM, else Windows-1252; line ends as '\n', other control characters dropped.
std::string DecodeText(const std::string& bytes);
bool IsText(const std::string& bytes);

std::string PathName(const std::string& path);
std::string PathFolder(const std::string& path); // without the last '/'
std::string PathType(const std::string& path);   // ".scn"
std::string PathStem(const std::string& name);
bool IsScenePath(const std::string& path);

} // namespace jpt
