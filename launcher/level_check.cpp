#include "level_check.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

#include "archive_reader.h"
#include "jpt_custom_level.h"
#include "jpt_groff.h"
#include "jpt_level_features.h"
#include "jpt_level_world.h"
#include "jpt_szdd.h"
#include "level_limits.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr uint64_t kWorldLimit = 128ull << 20;
constexpr uint64_t kSetLimit = 32ull << 20;
constexpr size_t kNamedTextures = 16;
constexpr char kCacheFile[] = "worlds.txt";
LevelIssue FaultIssue(const jpt_groff_fault_t& f, const std::string& subject) {
    return {f.code, subject, std::to_string(f.value) + " where the engine takes " + std::to_string(f.limit) + ", in " + std::to_string(f.count) + " place(s)", IssuePolicy::kUnsupported};
}

constexpr char kCacheHead[] = "level-check 11";

struct Closer {
    void operator()(jpt_groff_t* g) const { jpt_groff_close(g); }
    void operator()(jpt_texture_set_t* s) const { jpt_texture_set_close(s); }
};
using World = std::unique_ptr<jpt_groff_t, Closer>;
using TextureSet = std::unique_ptr<jpt_texture_set_t, Closer>;

const uint8_t* Bytes(const std::string& text) { return reinterpret_cast<const uint8_t*>(text.data()); }

bool ReadWhole(const fs::path& file, uint64_t limit, std::string& bytes) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    if (ec || size > limit || !in) return false;
    bytes.resize(size_t(size));
    if (size) in.read(bytes.data(), std::streamsize(size));
    return bool(in);
}

void Expand(std::string& bytes) {
    if (!jpt_szdd_is(Bytes(bytes), bytes.size())) return;
    size_t size = 0;
    uint8_t* expanded = jpt_szdd_expand(Bytes(bytes), bytes.size(), &size);
    bytes.assign(reinterpret_cast<const char*>(expanded), expanded ? size : 0);
    std::free(expanded);
}

std::string TextureFile(const std::string& name) { return name.substr(name.find_last_of("\\/") + 1); }

World OpenWorld(const std::string& name, const std::string& bytes, std::string& problem) {
    char text[128];
    World world(jpt_groff_open(name.c_str(), Bytes(bytes), bytes.size(), text, sizeof text));
    problem = text;
    return world;
}

std::string Stamp(const fs::path& file) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) return "none";
    return std::to_string(size) + "@" + std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
}

std::string Clean(std::string text) {
    for (char& c : text)
        if (static_cast<unsigned char>(c) < ' ' || c == '|') c = ' ';
    return text;
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, separator);) parts.push_back(part);
    return parts;
}

} // namespace

unsigned long WorldFacts::count(const char* name) const {
    const auto it = counts.find(name);
    return it == counts.end() ? 0 : it->second;
}

const char* LevelClassName(LevelClass value) {
    switch (value) {
    case LevelClass::kReady: return "READY";
    case LevelClass::kRejectedOther: return "REJECTED_OTHER";
    case LevelClass::kRejectedMissing: return "REJECTED_MISSING";
    case LevelClass::kRejectedUnsupported: return "REJECTED_UNSUPPORTED";
    case LevelClass::kRejectedMalformed: break;
    }
    return "REJECTED_MALFORMED";
}

LevelClass ClassOf(const std::vector<LevelIssue>& issues) {
    IssuePolicy worst = IssuePolicy::kInfo;
    for (const LevelIssue& issue : issues) worst = std::max(worst, issue.policy);
    switch (worst) {
    case IssuePolicy::kInfo: return LevelClass::kReady;
    case IssuePolicy::kWarning: return LevelClass::kRejectedOther;
    case IssuePolicy::kMissing: return LevelClass::kRejectedMissing;
    case IssuePolicy::kUnsupported: return LevelClass::kRejectedUnsupported;
    case IssuePolicy::kMalformed: break;
    }
    return LevelClass::kRejectedMalformed;
}

jpt_compat_t CompatibilityOf(const std::vector<LevelIssue>& issues) {
    jpt_compat_t worst = JPT_COMPAT_READY;
    for (const LevelIssue& issue : issues) worst = jpt_compat_worst(worst, issue.compat());
    return worst;
}

fs::path GameFile(const fs::path& data_dir, const std::string& name) {
    std::error_code ec;
    if (data_dir.empty()) return {};
    for (fs::directory_iterator it(data_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (SameText(Utf8(it->path().filename()), name) && it->is_regular_file(type_ec)) return it->path();
    }
    return {};
}

std::string WorldFactsText(const WorldFacts& facts) {
    std::string text;
    const auto line = [&text](const char* key, const std::string& value) { text += std::string(key) + " " + Clean(value) + "\n"; };
    if (!facts.problem.empty()) line("problem", facts.problem);
    if (!facts.terrain.empty()) line("terrain", facts.terrain);
    if (!facts.largest_mesh.empty()) line("largest", facts.largest_mesh);
    line("flags", std::string(facts.optimized ? "1" : "0") + (facts.has_set ? "1" : "0") + (facts.borrowed ? "1" : "0"));
    for (const auto& [name, count] : facts.counts) line("count", name + " " + std::to_string(count));
    for (const auto& [type, count] : facts.actions) line("action", std::to_string(type) + " " + std::to_string(count));
    for (const std::string& level : facts.levels) line("level", level);
    for (const std::string& map : facts.maps) line("map", map);
    for (const std::string& texture : facts.missing) line("missing", texture);
    for (const ScriptUse& use : facts.scripts) line("script", std::string(use.action ? "1 " : "0 ") + std::to_string(use.count) + " " + use.class_name + " " + use.object);
    for (const LevelIssue& fault : facts.faults) line("fault", fault.code + " " + fault.subject), line("why", fault.detail);
    return text;
}

bool ReadWorldFacts(const std::string& text, WorldFacts& facts) {
    facts = {};
    bool flags = false;
    for (const std::string& line : Split(text, '\n')) {
        const size_t space = line.find(' ');
        if (space == std::string::npos) return false;
        const std::string key = line.substr(0, space), value = line.substr(space + 1);
        const size_t last = value.rfind(' ');
        const std::string name = value.substr(0, last), number = last == std::string::npos ? std::string() : value.substr(last + 1);
        if (key == "problem") facts.problem = value;
        else if (key == "terrain") facts.terrain = value;
        else if (key == "largest") facts.largest_mesh = value;
        else if (key == "level") facts.levels.push_back(value);
        else if (key == "map") facts.maps.push_back(value);
        else if (key == "missing") facts.missing.push_back(value);
        else if (key == "script" && value.size() > 4 && (value[0] == '0' || value[0] == '1') && value[1] == ' ') {
            const size_t count_end = value.find(' ', 2), class_end = count_end == std::string::npos ? count_end : value.find(' ', count_end + 1);
            if (count_end == std::string::npos || count_end + 1 >= value.size()) return false;
            const std::string object = class_end == std::string::npos ? std::string() : value.substr(class_end + 1);
            facts.scripts.push_back({value.substr(count_end + 1, class_end == std::string::npos ? std::string::npos : class_end - count_end - 1), object, value[0] == '1',
                                     unsigned(std::strtoul(value.c_str() + 2, nullptr, 10))});
        }
        else if (key == "fault") facts.faults.push_back({value.substr(0, value.find(' ')), value.substr(std::min(value.find(' '), value.size() - 1) + 1), "", IssuePolicy::kUnsupported});
        else if (key == "why" && !facts.faults.empty()) facts.faults.back().detail = value;
        else if (key == "count" && !number.empty()) facts.counts[name] = std::strtoul(number.c_str(), nullptr, 10);
        else if (key == "action" && !number.empty()) facts.actions.push_back({std::atoi(name.c_str()), unsigned(std::strtoul(number.c_str(), nullptr, 10))});
        else if (key == "flags" && value.size() == 3) facts.optimized = value[0] == '1', facts.has_set = value[1] == '1', facts.borrowed = value[2] == '1', flags = true;
        else return false;
    }
    return flags;
}

WorldChecker::WorldChecker(const LevelFiles& files) : files_(files) {
    std::string text;
    if (files_.package_dir.empty() || !ReadWhole(files_.package_dir / JPT_CUSTOM_LEVEL_GENERATED_DIR / kCacheFile, 4 << 20, text)) return;
    std::stringstream in(text);
    std::string line, key, body;
    if (!std::getline(in, line) || line != kCacheHead) return;
    const auto keep = [&] {
        WorldFacts facts;
        if (!key.empty() && ReadWorldFacts(body, facts)) known_[key] = std::move(facts);
        body.clear();
    };
    while (std::getline(in, line)) {
        if (line.rfind("world ", 0) == 0) keep(), key = line.substr(6);
        else body += line + "\n";
    }
    keep();
}

WorldChecker::~WorldChecker() {
    if (files_.package_dir.empty() || !changed_) return;
    const fs::path folder = files_.package_dir / JPT_CUSTOM_LEVEL_GENERATED_DIR, file = folder / kCacheFile;
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(folder, ec)) || fs::is_symlink(fs::symlink_status(file, ec))) return;
    fs::create_directories(folder, ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << kCacheHead << "\n";
    for (const std::string& key : used_) out << "world " << key << "\n" << WorldFactsText(known_[key]);
}

bool WorldChecker::GameSwapHolds(const std::string& set_place, const std::string& swap) const {
    std::string pid;
    const fs::path file = GameFile(files_.data_dir, swap);
    if (file.empty() || !ReadPlace(set_place, set_place, kSetLimit, pid)) return false;
    const TextureSet set(jpt_texture_set_open(Bytes(pid), pid.size()));
    std::error_code ec;
    uint64_t size = fs::file_size(file, ec);
    if (!SameText(Utf8(file.extension()), ".swp")) {
        unsigned char head[4] = {};
        std::ifstream in(file, std::ios::binary);
        size = in.read(reinterpret_cast<char*>(head), sizeof head) ? uint64_t(head[0]) | uint64_t(head[1]) << 8 | uint64_t(head[2]) << 16 | uint64_t(head[3]) << 24 : 0;
    }
    return !ec && set && jpt_texture_set_bytes(set.get()) <= size;
}

std::string WorldChecker::KeyOf(const WorldRequest& request) const {
    uint64_t total = 0;
    for (const PackageFile& file : files_.source->files) total += file.size;
    const auto stamp = [this](const std::string& place, const std::string& name) {
        if (place.empty()) return std::string("none");
        return place + "@" + Stamp(place == kInstalledGame ? GameFile(files_.data_dir, name) : files_.package_dir / PathFromUtf8(place));
    };
    char set[JPT_CUSTOM_LEVEL_PATH_MAX] = "";
    jpt_level_world_set_name(request.name.c_str(), set, sizeof set);
    const std::string original = request.set_place == kInstalledGame ? Stamp(GameFile(files_.data_dir, request.name)) : "";
    return Clean(request.name) + "|" + Clean(stamp(request.place, request.name)) + "|" + Clean(stamp(request.set_place, set)) + "|" + original + "|" +
           std::to_string(files_.source->files.size()) + "+" + std::to_string(total);
}

WorldFacts WorldChecker::Check(const WorldRequest& request) {
    const std::string key = KeyOf(request);
    const bool kept = !files_.package_dir.empty();
    auto it = kept ? known_.find(key) : known_.end();
    if (it == known_.end()) {
        it = known_.insert_or_assign(key, Read(request)).first;
        changed_ = true;
    }
    if (std::find(used_.begin(), used_.end(), key) == used_.end()) used_.push_back(key);
    return it->second;
}

bool WorldChecker::ReadPlace(const std::string& place, const std::string& name, uint64_t limit, std::string& bytes) const {
    if (place.empty()) return false;
    return place == kInstalledGame ? ReadWhole(GameFile(files_.data_dir, name), limit, bytes) : files_.source->read(place, limit, bytes);
}

// The original game's levels draw some textures that none of its files has.
bool WorldChecker::GameLacks(const std::string& texture) const {
    if (!lacking_) {
        lacking_.emplace();
        std::error_code ec;
        if (!files_.data_dir.empty())
            for (fs::directory_iterator it(files_.data_dir, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = Utf8(it->path().filename());
                std::string bytes, pid, problem;
                char set_name[JPT_CUSTOM_LEVEL_PATH_MAX] = "";
                if (PathType(name) != ".grf" || !jpt_level_world_set_name(name.c_str(), set_name, sizeof set_name)) continue;
                if (!ReadWhole(it->path(), kWorldLimit, bytes) || !ReadWhole(GameFile(files_.data_dir, set_name), kSetLimit, pid)) continue;
                Expand(bytes);
                const World world = OpenWorld(name, bytes, problem);
                const TextureSet set(jpt_texture_set_open(Bytes(pid), pid.size()));
                jpt_groff_textures_t textures;
                if (!world || !set || !jpt_groff_textures(world.get(), set.get(), &textures)) continue;
                for (unsigned i = 0; i < textures.count; ++i)
                    if (textures.items[i].drawn && !textures.items[i].in_set) lacking_->insert(LowerAscii(TextureFile(textures.items[i].name)));
                jpt_groff_textures_free(&textures);
            }
    }
    return lacking_->count(LowerAscii(TextureFile(texture))) != 0;
}

// A texture that is not in the set is read from a picture of its name, in the places the runtime looks in.
void WorldChecker::CountTextures(const std::vector<Texture>& textures, WorldFacts& facts) const {
    std::set<std::string> targets, overrides;
    for (const RuntimeFile& file : files_.analysis->runtime) {
        const std::string lower = LowerAscii(file.target);
        if (file.active && PathType(file.target) == ".bmp") targets.insert(lower);
        if (file.active && lower.rfind("data/texhw/", 0) == 0) overrides.insert(lower);
    }
    const auto overridden = [&](const Texture& texture) {
        std::string path = LowerAscii(texture.name);
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::string stem = path.substr(0, path.size() - PathType(path).size());
        for (const std::string& place : {"data/texhw/" + stem, "data/texhw/map/" + PathName(stem)})
            for (const char* type : {".dds", ".tga", ".bmp"}) {
                if (!overrides.count(place + type)) continue;
                const bool normal = overrides.count(place + ".norm.dds") || overrides.count(place + ".norm.tga") || overrides.count(place + ".norm.bmp");
                ++facts.counts[texture.bumped ? std::string("texhw_bumped") : texture.odd_size ? std::string("texhw_odd") : std::string("texhw") + type];
                if (normal && !texture.bumped) ++facts.counts["texhw_normal"];
                return;
            }
    };
    const auto there = [&](const std::string& name) {
        std::string path = LowerAscii(name);
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::string stem = path.substr(0, path.size() - PathType(path).size()), file = PathName(stem) + ".bmp";
        for (const std::string& place : {"data/texhw/" + stem + ".bmp", "data/texhw/map/" + file, "data/" + path, path})
            if (targets.count(place)) return true;
        std::error_code ec;
        return !files_.data_dir.empty() && (fs::is_regular_file(files_.data_dir / PathFromUtf8(path), ec) || fs::is_regular_file(files_.data_dir.parent_path() / PathFromUtf8(path), ec));
    };
    for (const Texture& texture : textures) {
        const std::string& name = texture.name;
        overridden(texture);
        if (texture.in_set) ++facts.counts["textures_in_set"];
        else if (there(name)) ++facts.counts["textures_in_files"];
        else if (!texture.drawn) ++facts.counts["textures_undrawn"];
        else if (GameLacks(name)) ++facts.counts["textures_game_lacks"];
        else if (++facts.counts["textures_missing"] <= kNamedTextures) facts.missing.push_back(name + " (" + texture.object + ")");
    }
}

WorldFacts WorldChecker::Read(const WorldRequest& request) {
    WorldFacts facts;
    std::string bytes, pid, original;
    if (!ReadPlace(request.place, request.name, kWorldLimit, bytes)) return facts.problem = "it cannot be read", facts;
    Expand(bytes);
    const World world = OpenWorld(request.name, bytes, facts.problem);
    if (!world) return facts;
    jpt_groff_report_t report;
    if (!jpt_groff_describe(world.get(), &report)) return facts.problem = "out of memory", facts;
    facts.optimized = report.optimized != 0;
    if (report.terrain[0]) facts.terrain = std::string(report.terrain) + ".wtd";
    facts.largest_mesh = report.largest_mesh_object;
    for (unsigned i = 0; i < report.action_count; ++i) facts.actions.push_back({report.actions[i].type, report.actions[i].count});
    for (unsigned i = 0; i < report.level_count; ++i) facts.levels.push_back(report.levels[i]);
    for (unsigned i = 0; i < report.map_count; ++i) facts.maps.push_back(report.maps[i]);
    for (unsigned i = 0; i < report.script_count; ++i)
        facts.scripts.push_back({report.scripts[i].class_name, report.scripts[i].object, report.scripts[i].action != 0, report.scripts[i].count});
    facts.counts = {{"objects", report.objects},     {"animals", report.animals},   {"triggers", report.triggers}, {"more_dinosaurs", report.more_dinosaurs}, {"meshes", report.meshes},
                    {"points", (unsigned long)report.points}, {"vertices", (unsigned long)report.vertices}, {"faces", (unsigned long)report.faces},      {"largest_mesh", report.largest_mesh},
                    {"terrains", report.terrains},   {"mesh_build", report.mesh_build}, {"mesh_bytes", (unsigned long)std::min<uint64_t>(report.mesh_bytes, 0xFFFFFFFFu)}};
    if (report.scripts_overflow) facts.counts["script_classes_unread"] = 1;
    for (unsigned i = 0; i < report.fault_count; ++i) {
        const jpt_groff_fault_t& f = report.faults[i];
        facts.faults.push_back(FaultIssue(f, f.subject));
    }
    char set_name[JPT_CUSTOM_LEVEL_PATH_MAX] = "";
    jpt_level_world_set_name(request.name.c_str(), set_name, sizeof set_name);
    TextureSet set;
    if (ReadPlace(request.set_place, set_name, kSetLimit, pid)) set.reset(jpt_texture_set_open(Bytes(pid), pid.size()));
    facts.has_set = set != nullptr;
    facts.borrowed = set && request.set_place == kInstalledGame;
    jpt_groff_fault_t fault;
    if (set && !facts.borrowed && jpt_texture_set_fault(set.get(), &fault)) facts.faults.push_back(FaultIssue(fault, set_name));
    World original_world;
    if (facts.borrowed) {
        std::string problem;
        if (ReadWhole(GameFile(files_.data_dir, request.name), kWorldLimit, original)) Expand(original), original_world = OpenWorld(request.name, original, problem);
        jpt_texture_set_borrow(set.get(), original_world.get());
    }
    jpt_groff_textures_t textures;
    if (!jpt_groff_textures(world.get(), set.get(), &textures)) return facts.problem = "out of memory", facts;
    std::vector<Texture> names;
    for (unsigned i = 0; i < textures.count; ++i) names.push_back({textures.items[i].name, textures.items[i].in_set != 0, textures.items[i].bump[0] != 0 || textures.items[i].bumped != 0, textures.items[i].odd_size != 0, textures.items[i].drawn != 0, textures.items[i].object});
    facts.counts["textures"] = textures.count;
    if (set) facts.counts["set_bytes"] = (unsigned long)std::min<uint64_t>(jpt_texture_set_bytes(set.get()), 0xFFFFFFFFu);
    facts.counts["set_entries"] = jpt_texture_set_count(set.get());
    facts.counts["curved"] = textures.curved;
    facts.counts["curved_missing"] = textures.curved_missing;
    jpt_groff_textures_free(&textures);
    CountTextures(names, facts);
    return facts;
}

void AddWorldIssues(const std::string& world, const WorldFacts& facts, std::vector<LevelIssue>& issues) {
    const auto number = [&facts](const char* name) { return std::to_string(facts.count(name)); };
    if (!facts.problem.empty()) {
        issues.push_back({"MALFORMED_WORLD", world, facts.problem, IssuePolicy::kMalformed});
        return;
    }
    if (!facts.count("terrains"))
        issues.push_back({"NO_TERRAIN", world, "no object of the world is its terrain, as the level was made", IssuePolicy::kInfo});
    if (facts.count("textures_missing")) {
        // Some textures missing play as in Trespasser CE, with its grey texture; none found means the set is not this world's.
        const bool all = facts.count("textures_missing") >= facts.count("textures");
        std::string some;
        for (const std::string& name : facts.missing) some += (some.empty() ? "" : ", ") + name;
        issues.push_back({"MISSING_TEXTURES", world, number("textures_missing") + " of " + number("textures") + " textures are in no texture set and no file: " +
                                                         some + (facts.count("textures_missing") > facts.missing.size() ? ", ..." : "") +
                                                         (all ? "" : "; Trespasser CE's grey texture stands for them"),
                          all ? IssuePolicy::kMissing : IssuePolicy::kInfo});
    }
    if (facts.count("curved_missing"))
        issues.push_back({"CURVED_BUMP_NOT_IN_SET", world, number("curved_missing") + " of " + number("curved") + " polygons with a curved bump map have none in the texture set, which was made for another "
                                                               "build of the world; they are lit flat, or their texture is read as a missing one",
                          IssuePolicy::kMissing});
    issues.insert(issues.end(), facts.faults.begin(), facts.faults.end());
    for (const EngineLimit& limit : EngineLimits())
        if (limit.fatal && facts.count(limit.count) > limit.port)
            issues.push_back({limit.name, world, number(limit.count) + ", the port's limit is " + std::to_string(limit.port), IssuePolicy::kUnsupported});
    for (const auto& [type, count] : facts.actions) {
        if (type >= 0 && type <= JPT_GROFF_ACTION_OWN_LAST) continue;
        const jpt_level_feature_t* feature = jpt_level_action_feature(type);
        const std::string subject = "ActionType " + std::to_string(type), used = std::to_string(count) + " trigger(s) of " + world;
        if (!feature) issues.push_back({"UNDEFINED_ACTION", subject, used + "; neither ATX nor Trespasser CE gives it an effect", IssuePolicy::kInfo});
        else if (!jpt_level_feature_playable(feature->name)) issues.push_back({"UNSUPPORTED_ACTION", subject, used + ", " + feature->title + ": " + feature->note, IssuePolicy::kUnsupported});
    }
    for (const std::string& name : RequiredByWorld(facts)) {
        const jpt_level_feature_t* feature = jpt_level_feature(name.c_str());
        if (name.rfind("ATX_ACTION_", 0) != 0 && !jpt_level_feature_playable(name.c_str()))
            issues.push_back({"UNSUPPORTED_FEATURE", name, feature ? std::string(feature->title) + ", in " + world + (feature->note[0] ? std::string(": ") + feature->note : "") : "not known", IssuePolicy::kUnsupported});
    }
}

std::vector<std::string> RequiredByWorld(const WorldFacts& facts) {
    std::vector<std::string> names;
    const auto add = [&names](bool needed, const char* name) {
        if (needed) names.push_back(name);
    };
    for (const auto& [type, count] : facts.actions)
        if (const jpt_level_feature_t* feature = type > JPT_GROFF_ACTION_OWN_LAST ? jpt_level_action_feature(type) : nullptr) names.push_back(feature->name);
    add(facts.count("more_dinosaurs"), "ATX_DINOSAUR_SOUNDS");
    add(facts.count("texhw.bmp"), "CE_TEXHW_BMP");
    add(facts.count("texhw.tga"), "CE_TEXHW_TGA");
    add(facts.count("texhw.dds"), "CE_TEXHW_DDS");
    add(facts.count("texhw_bumped"), "CE_TEXHW_BUMPED");
    add(facts.count("texhw_normal"), "CE_NORMAL_MAPS");
    add(facts.count("texhw_odd"), "CE_TEXHW_ODD_SIZE");
    for (const EngineLimit& limit : EngineLimits())
        if (facts.count(limit.count) > limit.original) add(true, limit.feature);
    return names;
}

} // namespace jpt
