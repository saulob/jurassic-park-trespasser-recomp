// Which Trespasser CE scripts a level needs (docs/CE_SCRIPTS.md), read from the scripts' text and the world's Script and
// ScriptAT properties without running anything. The launcher decides with it whether a level can be played, the game which
// files of the package a level's VM loads.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jpt::script {

// The largest script file read, as the VM's JPT_SCRIPT_MAX_SOURCE.
inline constexpr size_t kSourceMax = 1024 * 1024;

enum class Kind { kUnknown, kGame, kLevel, kAction, kInstance };
const char* KindName(Kind kind);

struct ClassInfo {
    std::string name, base; // base: as written after extends; empty without one
    int line = 0;
    std::vector<std::string> methods; // declared in its body, constructor included
    std::vector<std::string> scenes;  // its static SCN
    bool has_scenes = false;          // a static SCN that is a string or an array of strings
    std::set<std::string> uses;       // identifiers of its body and base
};

struct FileInfo {
    std::string path; // in the package, '/' separators
    std::vector<ClassInfo> classes;
    std::set<std::string> globals;  // what its top level declares for every file
    std::set<std::string> locals;   // locals, parameters and loop variables anywhere in it
    std::set<std::string> top_uses; // identifiers outside its classes
    std::string problem;            // why it cannot be read; empty when it can
};

FileInfo ScanFile(const std::string& path, std::string_view source);

// A world object's Script property, or an action 99's ScriptAT of a trigger.
struct WorldScript {
    std::string class_name, object;
    bool action = false;
};

struct PlannedClass {
    std::string name, file;
    Kind kind = Kind::kUnknown;
    std::vector<std::string> objects; // that name it, for instance and action scripts
    bool sim_step = false;            // declares OnSimStep
};

struct LevelPlan {
    std::vector<std::string> files;     // to load, in the package's load order
    std::vector<PlannedClass> classes;  // game scripts, then level scripts, then those the world names
    std::vector<std::string> required;  // CE functions, classes and handlers the port does not have yet, sorted
    std::vector<std::string> problems;  // a class the world names that no script declares, ...
    unsigned user_commands = 0;         // uCMD_USER1..8 its code names, as bits 0..7
    bool needs_scripts() const { return !classes.empty() || !problems.empty(); }
    bool supported() const { return required.empty() && problems.empty(); }
};

// files: every script of the package, in load order (lowest priority root first, then by path). scene: the file name.
LevelPlan PlanLevel(const std::vector<FileInfo>& files, const std::string& scene, const std::vector<WorldScript>& world);

// What a file asks of the engine anywhere in it, for the analysis report: CE names and handlers the port does not have.
std::vector<std::string> FileRequires(const std::vector<FileInfo>& files, const FileInfo& file);

// The handlers the port calls (CE names), and whether a name is one of CE's API.
bool IsDrivenHandler(Kind kind, const std::string& name);
bool IsCeName(const std::string& name);

// The CE API the port implements (jpt_script_registry.inc).
enum class ApiKind { kClass, kNamespace, kEnum, kMethod, kField, kFunction, kConstant, kHandler };
struct ApiEntry {
    const char* owner; // "" for the root table; for a handler, empty
    const char* name;
    ApiKind kind;
    unsigned scopes;   // handlers: kScope* of the script kinds the port calls it on
    const char* capability;
};
inline constexpr unsigned kScopeGame = 1, kScopeLevel = 2, kScopeInstance = 4;
const std::vector<ApiEntry>& Api();
// A CE name some entry of the registry implements (handlers excluded).
bool IsImplemented(const std::string& name);

} // namespace jpt::script
