// What one level of a package needs to be loaded, against the files of the package and of the installed game: its world file
// read as the engine will, its terrain data, its texture set and the engine's limits. Reads only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "level_analysis.h"

namespace jpt {

inline constexpr char kInstalledGame[] = "the installed game";

// Info never stops a level; anything else means the port cannot promise the level works as made, and it is refused.
enum class IssuePolicy { kInfo, kWarning, kMissing, kUnsupported, kMalformed };

// A category of problem, never a level's own name.
struct LevelIssue {
    std::string code;    // "MISSING_REQUIRED_TERRAIN"
    std::string subject; // a file, an action type
    std::string detail;
    IssuePolicy policy = IssuePolicy::kWarning;
    bool blocks() const { return policy != IssuePolicy::kInfo; }
};

struct LevelResource {
    std::string type, name;
    std::string place; // a path of the package, kInstalledGame, or empty
    std::string note;
};

enum class LevelClass { kReady, kRejectedOther, kRejectedMissing, kRejectedUnsupported, kRejectedMalformed };
const char* LevelClassName(LevelClass value);
LevelClass ClassOf(const std::vector<LevelIssue>& issues);

// A Trespasser CE script class a world names: an object's Script, a trigger's action 99 by ScriptAT.
struct ScriptUse {
    std::string class_name, object; // the first object that names it
    bool action = false;
    unsigned count = 0;
};

struct WorldFacts {
    std::string problem; // why it cannot be read
    std::string terrain; // "x.wtd"; empty when it names none
    bool optimized = false, has_set = false, borrowed = false;
    std::map<std::string, unsigned long> counts; // "objects", "textures_missing", ...
    std::vector<std::pair<int, unsigned>> actions;
    std::vector<std::string> levels;        // the scenes it leads to
    std::vector<std::string> maps;          // the map pictures it shows, below MAPS
    std::vector<std::string> missing;       // the first textures that are nowhere
    std::vector<ScriptUse> scripts;
    std::string largest_mesh;
    std::vector<LevelIssue> faults; // what of it the engine cannot hold or follow
    unsigned long count(const char* name) const;
};

struct LevelFiles {
    const PackageSource* source = nullptr;
    const PackageAnalysis* analysis = nullptr;
    std::filesystem::path data_dir;
    std::filesystem::path package_dir; // of an installed package: what was read of its files is kept beside them
};

struct WorldRequest {
    std::string name;  // "x.grf"
    std::string place; // of the world
    std::string set_place; // of its .pid; empty when it has none
};

// Kept for a package that is installed, and read again when a file it depends on changes.
class WorldChecker {
public:
    explicit WorldChecker(const LevelFiles& files);
    ~WorldChecker();
    WorldChecker(const WorldChecker&) = delete;
    WorldChecker& operator=(const WorldChecker&) = delete;

    WorldFacts Check(const WorldRequest& request);
    // Whether the pixels of the installed game's swap file are all that the package's texture directory points to.
    bool GameSwapHolds(const std::string& set_place, const std::string& swap) const;

private:
    WorldFacts Read(const WorldRequest& request);
    std::string KeyOf(const WorldRequest& request) const;
    bool ReadPlace(const std::string& place, const std::string& name, uint64_t limit, std::string& bytes) const;
    struct Texture {
        std::string name;
        bool in_set = false, bumped = false, odd_size = false, drawn = true;
        std::string object;
    };
    void CountTextures(const std::vector<Texture>& textures, WorldFacts& facts) const;
    bool GameLacks(const std::string& texture) const;

    LevelFiles files_;
    std::map<std::string, WorldFacts> known_;
    std::vector<std::string> used_;
    bool changed_ = false;
    mutable std::optional<std::set<std::string>> lacking_; // the files of textures the original game draws and does not have
};

std::string WorldFactsText(const WorldFacts& facts);
bool ReadWorldFacts(const std::string& text, WorldFacts& facts);

// The installed game's own file of that name, whatever its case; empty when it has none.
std::filesystem::path GameFile(const std::filesystem::path& data_dir, const std::string& name);

// What the world asks for beyond the 1998 game, by name in jpt_level_features.
std::vector<std::string> RequiredByWorld(const WorldFacts& facts);

void AddWorldIssues(const std::string& world, const WorldFacts& facts, std::vector<LevelIssue>& issues);

} // namespace jpt
