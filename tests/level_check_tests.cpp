// What loading a level needs, asked before the engine is: its world, terrain data and texture set, the engine's limits, damaged
// saves, running out of memory, the installed levels' order and the headless scan of a folder of packages.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "jpt_alloc.h"
#include "jpt_custom_level.h"
#include "jpt_groff.h"
#include "jpt_level_gate.h"
#include "jpt_level_world.h"
#include "jpt_save.h"
#include "jpt_scene.h"
#include "jpt_szdd.h"
#include "level_check.h"
#include "level_corpus.h"
#include "level_fixtures.h"
#include "level_limits.h"
#include "level_manifest.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;
using namespace fixtures;

using Files = std::vector<std::pair<std::string, std::string>>;
void WriteZip(const fs::path& file, const Files& files, unsigned attributes);
std::string SevenZip(const Files& files, uint32_t attributes);

static int g_checks, g_failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static fs::path g_scratch;

static void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

static std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static fs::path Fresh(const char* name) {
    const fs::path dir = g_scratch / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static std::string Scene(const std::string& world, uint32_t version = 22) { return Container({{"Header", HeaderSection(world, version)}}); }

static Files Level(const std::string& folder, const std::string& stem) {
    return {{folder + stem + ".scn", Scene(stem + ".grf")}, {folder + stem + ".grf", World(stem)}, {folder + stem + "-130.pid", Set(stem)},
            {folder + stem + "-130.swp", "swap"}, {folder + stem + ".wtd", "terrain of " + stem}};
}

static Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static Files Without(Files files, const std::string& name) {
    files.erase(std::remove_if(files.begin(), files.end(), [&name](const auto& f) { return f.first == name; }), files.end());
    return files;
}

static Files With(Files files, const std::string& name, const std::string& bytes) { return Without(std::move(files), name) + Files{{name, bytes}}; }

static void WriteTree(const fs::path& root, const Files& files) {
    for (const auto& [path, bytes] : files) Write(root / PathFromUtf8(path), bytes);
}

static ImportResult Install(const fs::path& user, const char* name, const Files& files, const fs::path& data_dir = {}) {
    const fs::path source = Fresh("check source") / name;
    WriteTree(source, files);
    return ImportLevelFolder(source, user, {}, {}, data_dir);
}

// A package as an earlier version installed it, before only READY ones were.
static void Place(const fs::path& user, const char* name, const Files& files) { WriteTree(CustomLevelsDir(user) / name, files); }

static fs::path g_game;

static int GameFile(const char* relative, char* out, size_t cap) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(g_game, ec))
        if (LowerAscii(Utf8(entry.path().filename())) == LowerAscii(PathName(relative))) return std::snprintf(out, cap, "%s", entry.path().string().c_str()) > 0;
    return 0;
}

static bool Activate(const fs::path& user, const char* package, const char* scene) {
    jpt_custom_level_request_t request{};
    char error[256] = "";
    std::snprintf(request.package, sizeof request.package, "%s", package);
    std::snprintf(request.scene, sizeof request.scene, "%s", scene);
    const bool ok = jpt_custom_level_activate(user.string().c_str(), &request, error, sizeof error) != 0;
    if (ok) jpt_custom_level_set_game(GameFile);
    return ok;
}

static bool Logged(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
}

static const uint8_t* Bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

static bool Describe(const std::string& world, jpt_groff_report_t& report) {
    char problem[128] = "";
    jpt_groff_t* g = jpt_groff_open("w.grf", Bytes(world), world.size(), problem, sizeof problem);
    const bool ok = g && jpt_groff_describe(g, &report);
    jpt_groff_close(g);
    return ok;
}

static std::string FaultsOf(const WorldShape& shape) {
    jpt_groff_report_t report{};
    std::string text;
    if (!Describe(MakeWorld(shape), report)) return "unreadable";
    for (unsigned i = 0; i < report.fault_count; ++i) text += (i ? " " : "") + std::string(report.faults[i].code);
    return text;
}

static WorldShape Curved(uint32_t polygons = 2) {
    WorldShape shape;
    shape.terrain = "w";
    MeshShape rock;
    rock.name = "Rock-00";
    rock.texture = "Map\\w\\rock.bmp";
    rock.bump = "Map\\w\\rock_b.bmp";
    rock.polygons = polygons;
    rock.curved = true;
    shape.meshes.push_back(rock);
    return shape;
}

static uint32_t HashOf(const MeshShape& m) { return TextureHash(m.texture + m.bump); }

static void TestWorlds() {
    WorldShape shape;
    shape.terrain = "Isle-27";
    shape.meshes = {{"Crate-00", "Map\\w\\crate.bmp"}, {"Wall-00", "Map\\w\\wall.bmp"}};
    shape.meshes[1].polygons = 40;
    shape.actions = {0, 16, 112, 112, 1000};
    shape.levels = {"next.scn", "NEXT.SCN", "other.scn"};
    shape.animals = 3;
    const std::string world = MakeWorld(shape);
    jpt_groff_report_t report{};
    CHECK(Describe(world, report) && report.terrains == 1 && !std::strcmp(report.terrain, "Isle-27") && report.animals == 3 && report.meshes == 2);
    CHECK(report.largest_mesh == 40 && !std::strcmp(report.largest_mesh_object, "Wall-00") && report.level_count == 2 && report.fault_count == 0 && report.mesh_bytes > 40 * 40);
    unsigned foreign = 0;
    for (unsigned i = 0; i < report.action_count; ++i) foreign += report.actions[i].type > JPT_GROFF_ACTION_OWN_LAST ? report.actions[i].count : 0;
    CHECK(foreign == 3);

    // Cut anywhere, or with any word of it wrong, a world is refused or read; it is never followed out of its bytes.
    size_t refused = 0;
    for (size_t n = 0; n < world.size(); n += 3) refused += !Describe(world.substr(0, n), report);
    CHECK(refused > world.size() / 6);
    const std::string set = TextureSet({{HashOf(Curved().meshes[0]), true, 0xFFFFFFFF}});
    const std::string curved = MakeWorld(Curved());
    for (const std::string& source : {world, curved}) {
        for (size_t at = 0; at + 4 <= source.size(); at += 2) {
            for (const uint32_t value : {0xFFFFFFFFu, 0x7FFFFFFFu, 0x10000u, 1u}) {
                std::string bytes = source, pid = set;
                bytes.replace(at, 4, U32(value));
                char problem[128];
                jpt_groff_t* g = jpt_groff_open("w.grf", Bytes(bytes), bytes.size(), problem, sizeof problem);
                jpt_texture_set_t* s = jpt_texture_set_open(Bytes(pid), pid.size());
                jpt_groff_textures_t textures{};
                unsigned hidden = 0;
                if (g && s && jpt_groff_describe(g, &report) && jpt_texture_set_borrow(s, g) && jpt_groff_textures(g, s, &textures)) {
                    jpt_groff_textures_free(&textures);
                    jpt_groff_hide_unusable(g, s, reinterpret_cast<uint8_t*>(pid.data()), &hidden);
                    jpt_groff_flatten_curved(g, s, reinterpret_cast<uint8_t*>(bytes.data()));
                }
                jpt_texture_set_close(s);
                jpt_groff_close(g);
            }
        }
    }
    for (size_t at = 0; at + 4 <= set.size(); ++at) {
        std::string pid = set;
        pid.replace(at, 4, U32(0xFFFFFFF0u));
        jpt_texture_set_close(jpt_texture_set_open(Bytes(pid), pid.size()));
        jpt_texture_set_close(jpt_texture_set_open(Bytes(pid), at));
    }
    ++g_checks;
}

static void TestFaults() {
    const auto mesh = [](void (*change)(MeshShape&)) {
        WorldShape shape;
        shape.meshes.push_back({"Crate-00", "Map\\w\\crate.bmp"});
        change(shape.meshes[0]);
        return FaultsOf(shape);
    };
    CHECK(mesh([](MeshShape& m) { m.polygons = 8192; }).empty() && mesh([](MeshShape& m) { m.polygons = 8193; }) == "MESH_EXCEEDS_LIMIT");
    CHECK(mesh([](MeshShape& m) { m.bad_index = true; }) == "INVALID_MESH_INDEX" && mesh([](MeshShape& m) { m.bad_surface = true; }) == "INVALID_SURFACE_INDEX");
    CHECK(mesh([](MeshShape& m) { m.polygons = 20, m.corners = 58; }).empty() && mesh([](MeshShape& m) { m.polygons = 20, m.corners = 59; }).find("POLYGON_EXCEEDS_LIMIT") != std::string::npos);
    CHECK(mesh([](MeshShape& m) { m.name = std::string(95, 'n'); }).empty() && mesh([](MeshShape& m) { m.name = std::string(96, 'n'); }) == "NAME_EXCEEDS_LIMIT");

    const auto world = [](void (*change)(WorldShape&)) {
        WorldShape shape;
        shape.meshes.push_back({"Crate-00", "Map\\w\\crate.bmp"});
        change(shape);
        return FaultsOf(shape);
    };
    CHECK(world([](WorldShape& w) { w.trigger_actions = 16; }).empty() && world([](WorldShape& w) { w.trigger_actions = 17; }) == "TRIGGER_ACTIONS_EXCEED_LIMIT");
    CHECK(world([](WorldShape& w) { w.expression = "(a|b)&!c&d|e&f|g&h"; }).empty() && world([](WorldShape& w) { w.expression = "a&b&c&d&e&f&g&h&i"; }) == "EXPRESSION_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.expression = std::string(127, 's'); }).empty() && world([](WorldShape& w) { w.expression = std::string(128, 's'); }) == "EXPRESSION_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.models.assign(10, "Crate-00"); }).empty() && world([](WorldShape& w) { w.models.assign(11, "Crate-00"); }) == "SUBMODELS_EXCEED_LIMIT");
    CHECK(world([](WorldShape& w) { w.models = {"Gone-00"}; }) == "INVALID_MODEL_REFERENCE" && world([](WorldShape& w) { w.models = {"CRATE-00"}; }) == "INVALID_MODEL_REFERENCE");
    // Only a compound has submodels, one after another, and of one name the engine reads one.
    CHECK(world([](WorldShape& w) { w.models.assign(13, "Gone-00"), w.models_type = "Box"; }).empty() && world([](WorldShape& w) { w.models.assign(13, "Crate-00"), w.models_named = 0; }).empty() &&
          world([](WorldShape& w) { w.models.assign(3, "Gone-00"), w.models_named = 1; }).empty());
    // A mesh or a physics box taken of another object, and a trigger's own mesh, which only a bounding volume reads.
    const auto other = [](void (*change)(MeshShape&)) {
        WorldShape shape;
        shape.meshes.push_back({"Crate-00", "Map\\w\\crate.bmp"});
        shape.meshes.push_back({"Other-00", "Map\\w\\crate.bmp"});
        change(shape.meshes[1]);
        return FaultsOf(shape);
    };
    CHECK(other([](MeshShape& m) { m.mesh = m.physics = "Crate-00"; }).empty() && other([](MeshShape& m) { m.mesh = "crate-00"; }) == "INVALID_MODEL_REFERENCE" && other([](MeshShape& m) { m.physics = "Gone"; }) == "INVALID_MODEL_REFERENCE");
    CHECK(other([](MeshShape& m) { m.mesh = "Crate-00", m.polygons = 9000, m.bad_index = true; }).empty() && other([](MeshShape& m) { m.polygons = 9000; }) == "MESH_EXCEEDS_LIMIT");
    CHECK(other([](MeshShape& m) { m.skeleton = true; }) == "MALFORMED_MESH" && other([](MeshShape& m) { m.skeleton = true, m.mesh = "Crate-00"; }).empty());
    CHECK(other([](MeshShape& m) { m.type = "CLocationTrigger", m.polygons = 9000; }).empty() && other([](MeshShape& m) { m.type = "CLocationTrigger", m.bound = 0, m.polygons = 9000; }).empty() &&
          other([](MeshShape& m) { m.type = "CLocationTrigger", m.bound = 1, m.polygons = 9000; }) == "MESH_EXCEEDS_LIMIT" && other([](MeshShape& m) { m.type = "CInstance", m.bound = 0, m.polygons = 9000; }) == "MESH_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.more_objects = 131071; }).empty() && world([](WorldShape& w) { w.more_objects = 131072; }) == "OBJECTS_EXCEED_LIMIT");
    CHECK(world([](WorldShape& w) { w.links = {0, 2}; }).empty() && world([](WorldShape& w) { w.links = {0, 3}; }) == "INVALID_SKELETON_INDEX" && world([](WorldShape& w) { w.links = {-1}; }) == "INVALID_SKELETON_INDEX");
    CHECK(world([](WorldShape& w) { w.links = {0}, w.joints = 33; }) == "SKELETON_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.substitute = "Nobody"; }).empty() && world([](WorldShape& w) { w.substitute = "Skeleton-00"; }).empty());
    CHECK(world([](WorldShape& w) { w.substitute = "Fan-00", w.meeting = 1; }).empty() && world([](WorldShape& w) { w.substitute = "Fan-00", w.meeting = 2; }) == "SUBSTITUTE_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.skeleton_polygons = 545, w.substitute = "Skeleton-00"; }).empty() && world([](WorldShape& w) { w.skeleton_polygons = 546, w.substitute = "Skeleton-00"; }) == "SUBSTITUTE_EXCEEDS_LIMIT");
    CHECK(world([](WorldShape& w) { w.meeting = 64, w.meeting_skeleton = true; }).empty() && world([](WorldShape& w) { w.meeting = 65, w.meeting_skeleton = true; }) == "NORMALS_EXCEED_LIMIT");
    const auto fan = [](void (*change)(WorldShape&)) {
        WorldShape shape;
        change(shape);
        return FaultsOf(shape);
    };
    CHECK(fan([](WorldShape& w) { w.meeting = 200; }).empty() && fan([](WorldShape& w) { w.meeting = 65, w.optimized = false, w.normals = 2; }) == "NORMALS_EXCEED_LIMIT");
    CHECK(fan([](WorldShape& w) { w.meeting = 16, w.optimized = false, w.normals = 1; }).empty() && fan([](WorldShape& w) { w.meeting = 40, w.optimized = false, w.normals = 1; }) == "NORMALS_EXCEED_LIMIT" &&
          fan([](WorldShape& w) { w.meeting = 40, w.optimized = false, w.normals = 2; }).empty());
    // A face with a corner past the points is reported and left out of the normals of the point it shares; it is never read.
    const auto broken_fan = [](std::vector<uint32_t> face, int normals) {
        WorldShape shape;
        shape.meeting = 40, shape.optimized = false, shape.normals = normals, shape.fan_face = face;
        return FaultsOf(shape);
    };
    const uint32_t points = 42;
    CHECK(broken_fan({0, 1, 2}, 2).empty() && broken_fan({0, 1, 2}, 1) == "NORMALS_EXCEED_LIMIT");
    for (const std::vector<uint32_t>& face : std::vector<std::vector<uint32_t>>{{points, 0, 1},
                                                                             {0, points, 1},
                                                                             {0, 1, points},
                                                                             {0, 1, points + 5},
                                                                             {0xFFFFFFFFu, 0, 1},
                                                                             {0, 0x7FFFFFFFu, 1},
                                                                             {0, 1, 0xFFFFFFFFu}}) {
        CHECK(broken_fan(face, 2) == "INVALID_MESH_INDEX");
        CHECK(broken_fan(face, 1) == "INVALID_MESH_INDEX NORMALS_EXCEED_LIMIT");
    }
    WorldShape far_corner;
    far_corner.meeting = 40, far_corner.optimized = false, far_corner.normals = 2, far_corner.fan_face = {0, 1, 0xFFFFFFFFu};
    jpt_groff_report_t far_report{};
    CHECK(Describe(MakeWorld(far_corner), far_report) && far_report.fault_count == 1 && !std::strcmp(far_report.faults[0].code, "INVALID_MESH_INDEX") &&
          far_report.faults[0].value == 0xFFFFFFFFu && far_report.faults[0].limit == points && far_report.faults[0].count == 1);
    CHECK(fan([](WorldShape& w) { w.meeting = 62, w.optimized = false, w.normals = 2, w.planted = true; }).empty() && fan([](WorldShape& w) { w.meeting = 63, w.optimized = false, w.normals = 2, w.planted = true; }) == "PLANTED_POINTS_EXCEED_LIMIT" &&
          fan([](WorldShape& w) { w.meeting = 63, w.planted = true; }).empty());
    CHECK(world([](WorldShape& w) { w.optimized = false; }) == "MALFORMED_MESH");
    CHECK(world([](WorldShape& w) { w.meshes[0].animated = 1; }).empty() && world([](WorldShape& w) { w.meshes[0].animated = 2; }) == "INVALID_SURFACE_INDEX" && world([](WorldShape& w) { w.meshes[0].animated = 0; }).empty());
    CHECK(world([](WorldShape& w) { w.models.assign(9, "Crate-00"), w.magnets = {{"Compound-00", "Crate-00"}}; }).empty() && world([](WorldShape& w) { w.models.assign(10, "Crate-00"), w.magnets = {{"Compound-00", "Crate-00"}}; }) == "MAGNET_GROUP_EXCEEDS_LIMIT" &&
          world([](WorldShape& w) { w.models.assign(10, "Crate-00"), w.magnets = {{"Compound-00", "Nobody"}, {"Crate-00", "Crate-00"}, {"Compound-00", ""}}; }).empty());

    const auto set_fault = [](const PidEntry& entry, uint32_t first_pages = 0) {
        std::string bytes = TextureSet({{1}, entry});
        jpt_groff_fault_t found;
        bytes.replace(24, 4, U32(first_pages));
        jpt_texture_set_t* set = jpt_texture_set_open(Bytes(bytes), bytes.size());
        const std::string code = set && jpt_texture_set_fault(set, &found) ? found.code : "";
        jpt_texture_set_close(set);
        return code;
    };
    PidEntry entry{2};
    CHECK(set_fault(entry).empty() && set_fault(entry, 224u << 20).empty() && set_fault(entry, (224u << 20) + 1) == "TEXTURE_POOL_EXCEEDS_LIMIT");
    entry.offset = (224u << 20) - 63 * 512 - 256;
    CHECK(set_fault(entry).empty());
    entry.offset += 1;
    CHECK(set_fault(entry) == "TEXTURE_POOL_EXCEEDS_LIMIT");
    entry.offset = 0xFFFFFFFF;
    entry.height = 0xFFFFFFFF;
    CHECK(set_fault(entry) == "TEXTURE_POOL_EXCEEDS_LIMIT");
    entry = {2};
    entry.palette = 0;
    CHECK(set_fault(entry) == "INVALID_PALETTE_INDEX");

    // The 1998 limit is told apart from the port's, and what needs a larger heap gets one.
    const fs::path user = Fresh("limits user"), game = Fresh("limits game");
    const auto sized = [](uint32_t polygons) {
        WorldShape shape;
        shape.terrain = "big";
        shape.meshes.push_back({"Big-00", "Map\\big\\crate.bmp"});
        shape.meshes[0].polygons = polygons;
        return With(Level("", "big"), "big.grf", MakeWorld(shape));
    };
    const ImportResult larger = Install(user, "Larger", sized(3000), game), beyond = Install(user, "Beyond", sized(9000), game);
    CHECK(larger.ok() && larger.levels == 1 && !beyond.ok() && beyond.status == ImportStatus::kNotSupported && !fs::exists(CustomLevelsDir(user) / "Beyond"));
    Place(user, "Beyond", sized(9000));
    PlayCheck check = CheckBeforePlay(user, game, "Beyond", "big.scn");
    CHECK(!check.ok && check.reason == ImportStatus::kNotSupported && Logged(check.log, "MESH_EXCEEDS_LIMIT Big-00: 9000 where the engine takes 8192") && Logged(check.log, "not started, This custom level is not fully supported"));
    CHECK(CheckBeforePlay(user, game, "Larger", "big.scn").ok);
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(listed.size() == 2 && LimitsExceeded(listed[0].scenes[0].worlds[0].second, false).size() == 1 && LimitsExceeded(listed[1].scenes[0].worlds[0].second, true) == std::vector<std::string>{"MESH_EXCEEDS_LIMIT"} &&
          LimitsExceeded(listed[1].scenes[0].worlds[0].second, false).empty() && std::string(LevelStatusText(listed[0].scenes[0])) == "Invalid / Unsafe");

    // A set the 1998 pool does not hold: a larger pool for this session.
    PidEntry far_one{TextureHash("Map\\big\\crate.bmp")};
    far_one.offset = 135u << 20;
    CHECK(Install(user, "Pool", With(Level("", "big"), "big-130.pid", TextureSet({far_one})), game).levels == 1 && CheckBeforePlay(user, game, "Pool", "big.scn").ok);
    CHECK(Activate(user, "Pool", "big.scn") && jpt_custom_level_limit("texture_pool_mb") == 168 && Activate(user, "Larger", "big.scn") && !jpt_custom_level_limit("texture_pool_mb"));
    jpt_custom_level_deactivate();

    WorldFacts facts;
    facts.counts["terrains"] = 1;
    facts.counts["mesh_bytes"] = 200ul << 20;
    std::vector<LevelIssue> issues;
    AddWorldIssues("w.grf", facts, issues);
    CHECK(issues.size() == 1 && issues[0].code == "ALLOCATION_REQUIREMENT_EXCEEDS_CAP" && issues[0].blocks());
    facts.counts["mesh_bytes"] = 44ul << 20;
    issues.clear();
    AddWorldIssues("w.grf", facts, issues);
    CHECK(issues.empty());

    // Facts read from a record are the facts written to it.
    facts.terrain = "x.wtd";
    facts.faults = {{"MESH_EXCEEDS_LIMIT", "A name, with spaces", "9000 where the engine takes 8192", IssuePolicy::kUnsupported}};
    WorldFacts again;
    CHECK(ReadWorldFacts(WorldFactsText(facts), again) && again.terrain == "x.wtd" && again.faults.size() == 1 && again.faults[0].code == "MESH_EXCEEDS_LIMIT" && again.faults[0].subject == "A name, with spaces" &&
          again.faults[0].detail == facts.faults[0].detail && again.count("mesh_bytes") == (44ul << 20) && again.faults[0].blocks());
}

// A world that names terrain data which is nowhere: the engine would go on with a null world.
static void TestMissingTerrain() {
    const fs::path user = Fresh("terrain user"), game = Fresh("terrain game");
    const Files level = With(With(Level("", "isle"), "isle.grf", World("Isle-27")), "isle-130.pid", Set("Isle-27"));
    const ImportResult result = Install(user, "Isle", level + Level("", "other"), game);
    CHECK(result.ok() && result.levels == 1 && Logged(result.log, "MISSING_REQUIRED_TERRAIN") && !Logged(result.log, "PACKAGE_NOT_SUPPORTED"));
    fs::remove_all(CustomLevelsDir(user) / result.package);
    CHECK(!Install(user, "Alone", level, game).ok() && !fs::exists(CustomLevelsDir(user) / "Alone"));
    Place(user, "Isle", level + Level("", "other"));
    PlayCheck check = CheckBeforePlay(user, game, "Isle", "isle.scn");
    CHECK(!check.ok && check.reason == ImportStatus::kMissingFiles && Logged(check.log, "package Isle, level isle.scn: REJECTED_MISSING") && Logged(check.log, "MISSING_REQUIRED_TERRAIN Isle-27.wtd") &&
          Logged(check.log, "looked in:") && Logged(check.log, "alternative: none") && Logged(check.log, "not started, This level is missing required files"));
    check = CheckBeforePlay(user, game, "Isle", "other.scn");
    CHECK(check.ok && !Logged(check.log, "isle.scn"));
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(listed.size() == 1 && listed[0].rows().size() == 2 && listed[0].levels().size() == 1);

    // The world's own name for it counts, not the world's name.
    Write(game / "ISLE-27.WTD", "retail");
    check = CheckBeforePlay(user, game, "Isle", "isle.scn");
    CHECK(check.ok && Logged(check.log, "WTD Isle-27.wtd: the installed game"));
    fs::remove(game / "ISLE-27.WTD");
    CHECK(Install(user, "Whole", With(level, "Isle-27.wtd", "terrain"), game).levels == 1 && CheckBeforePlay(user, game, "Whole", "isle.scn").ok);

    g_game = game;
    char why[256];
    CHECK(Activate(user, "Isle", "isle.scn") && jpt_gate_load("", "C:\\data\\isle.scn", why, sizeof why) == JPT_GATE_MISSING && std::strstr(why, "Isle-27.wtd"));
    // A level neither the package nor the game has is refused before the game changes anything: it stays in the current one.
    CHECK(jpt_gate_load("", "C:\\data\\gone.scn", why, sizeof why) == JPT_GATE_NOT_INCLUDED && std::strstr(why, "gone.scn") && std::strstr(jpt_gate_message(JPT_GATE_NOT_INCLUDED), "not included"));
    CHECK(jpt_gate_load("", "other.scn", why, sizeof why) == JPT_GATE_OPEN);
    Write(game / "gone.scn", "the game's own");
    CHECK(jpt_gate_load("", "gone.scn", why, sizeof why) == JPT_GATE_OPEN);
    fs::remove(game / "gone.scn");
    std::string bytes = World("Isle-27");
    jpt_level_world_t prepared;
    jpt_level_world_prepare("isle.grf", reinterpret_cast<uint8_t*>(bytes.data()), bytes.size(), &prepared);
    CHECK(!prepared.failed && jpt_level_world_needs("C:\\data\\ISLE-27.wtd") && !jpt_level_world_needs("other.wtd"));
    jpt_level_world_forget();
    jpt_custom_level_deactivate();
    CHECK(!jpt_level_world_needs("Isle-27.wtd"));
}

// Polygons with a curved bump map that the texture set was not made with: the engine would build it from a source it has not.
static void TestCurvedMaps() {
    const fs::path user = Fresh("curved user"), game = g_game = Fresh("curved game");
    const WorldShape shape = Curved();
    const MeshShape& rock = shape.meshes[0];
    const uint32_t hash = HashOf(rock);
    const std::string world = MakeWorld(shape);
    const std::string made = TextureSet({{hash, true, 0xFFFFFFFF}, {CurvedKey(rock, 0, hash)}, {CurvedKey(rock, 1, hash)}});
    const std::string foreign = TextureSet({{hash, true, 0xFFFFFFFF}}), pictured = TextureSet({{hash, true, 0x808080}});
    Place(user, "W", {{"w.scn", Scene("w.grf")}, {"w.grf", world}, {"w.wtd", "t"}});
    const auto prepare = [&](const std::string& set, std::string& bytes, std::string& taken) {
        jpt_level_world_t out;
        size_t size = 0;
        Write(game / "W-130.pid", set);
        bytes = world;
        taken.clear();
        CHECK(Activate(user, "W", "w.scn"));
        jpt_level_world_prepare("C:\\data\\w.grf", reinterpret_cast<uint8_t*>(bytes.data()), bytes.size(), &out);
        CHECK(!jpt_level_world_take_set("other-130.pid", &size));
        uint8_t* pid = jpt_level_world_take_set("C:\\data\\W-130.PID", &size);
        if (pid) taken.assign(reinterpret_cast<char*>(pid), size);
        std::free(pid);
        return out;
    };
    std::string bytes, taken, line(400, '\0');
    jpt_level_world_t out = prepare(made, bytes, taken);
    CHECK(out.borrowed && out.textures == 1 && !out.textures_missing && out.curved == 2 && !out.curved_missing && bytes == world && taken.empty() && !out.failed);

    out = prepare(foreign, bytes, taken);
    CHECK(out.curved == 2 && out.curved_missing == 2 && out.hidden == 1 && !out.flattened && taken.size() == foreign.size() && taken != foreign && !out.failed);
    CHECK(jpt_level_world_line(&out, 0, line.data(), line.size()) && jpt_level_world_line(&out, 1, line.data(), line.size()) && std::strstr(line.c_str(), "2 of its 2 polygons") && !jpt_level_world_line(&out, 2, line.data(), line.size()));
    CHECK(prepare(taken, bytes, taken).curved_missing == 0);

    out = prepare(pictured, bytes, taken);
    CHECK(out.curved_missing == 2 && out.flattened == 2 && !out.hidden && taken.empty() && bytes != world && bytes.size() == world.size());
    const std::string flat = bytes;
    char problem[128];
    jpt_groff_t* g = jpt_groff_open("w.grf", Bytes(flat), flat.size(), problem, sizeof problem);
    jpt_texture_set_t* set = jpt_texture_set_open(Bytes(pictured), pictured.size());
    jpt_groff_textures_t textures{};
    CHECK(g && set && jpt_groff_textures(g, set, &textures) && textures.curved == 0 && textures.curved_missing == 0);
    jpt_groff_textures_free(&textures);
    jpt_texture_set_close(set);
    jpt_groff_close(g);

    // A skeleton's mesh has its maps by the shape of the one the set was made with.
    WorldShape animal;
    MeshShape raptor;
    raptor.name = "Raptor-00";
    raptor.texture = "Map\\m\\skin.bmp";
    raptor.bump = "Map\\m\\skin_b.bmp";
    raptor.model = raptor.curved_property = true;
    raptor.polygons = 4;
    animal.meshes.push_back(raptor);
    WorldShape grown = animal;
    grown.meshes[0].size = 2.0f;
    const std::string skin = TextureSet({{HashOf(raptor), true, 0xFFFFFFFF}});
    for (const bool same : {true, false}) {
        Place(user, same ? "Same" : "Grown", {{"m.scn", Scene("m.grf")}, {"m.grf", MakeWorld(same ? animal : grown)}});
        Write(game / "M-130.pid", skin);
        Write(game / "M.grf", MakeWorld(animal));
        bytes = MakeWorld(same ? animal : grown);
        CHECK(Activate(user, same ? "Same" : "Grown", "m.scn"));
        jpt_level_world_prepare("m.grf", reinterpret_cast<uint8_t*>(bytes.data()), bytes.size(), &out);
        CHECK(out.curved == 4 && out.curved_missing == (same ? 0u : 4u) && out.hidden == (same ? 0u : 1u));
    }

    // What the launcher says of such a level: its lighting would not be the author's, so it is not played.
    Write(game / "W-130.pid", foreign);
    Write(game / "W-130.spz", "swap");
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    const auto w = std::find_if(listed.begin(), listed.end(), [](const LevelPackage& p) { return p.id == "W"; });
    CHECK(w != listed.end() && !w->scenes[0].playable() && w->scenes[0].level_class() == LevelClass::kRejectedMissing && w->scenes[0].worlds[0].second.count("curved_missing") == 2 && w->scenes[0].worlds[0].second.borrowed);
    jpt_level_world_forget();
    jpt_custom_level_deactivate();
}

static void TestAnimatedTextures() {
    WorldShape shape;
    shape.frames = {"one.bmp", "two.bmp"};
    const std::string world = MakeWorld(shape), set = TextureSet({{TextureHash("Map\\w\\one.bmp")}});
    char problem[128];
    jpt_groff_t* g = jpt_groff_open("w.grf", Bytes(world), world.size(), problem, sizeof problem);
    jpt_texture_set_t* s = jpt_texture_set_open(Bytes(set), set.size());
    jpt_groff_textures_t textures{};
    unsigned missing = 0;
    CHECK(g && s && jpt_groff_textures(g, s, &textures));
    for (unsigned i = 0; i < textures.count; ++i) missing += !textures.items[i].in_set && !std::strcmp(textures.items[i].name, "Map\\w\\two.bmp");
    CHECK(missing == 1);
    jpt_groff_textures_free(&textures);
    jpt_texture_set_close(s);
    jpt_groff_close(g);
}

static void TestDrawnTextures() {
    WorldShape shape;
    shape.terrain = "pit";
    MeshShape crate{"Crate-00", "Map\\pit\\crate.bmp"}, magnet{"Magnet-01", "Map\\pit\\magnet.bmp"};
    crate.unused = "Map\\pit\\unused.bmp";
    magnet.type = "CMagnet";
    shape.meshes = {crate, magnet};
    const std::string world = MakeWorld(shape), set = TextureSet({{TextureHash("Map\\pit\\crate.bmp")}});
    fs::path user = Fresh("drawn");
    const Files level = {{"Pit/pit.scn", Scene("pit.grf")}, {"Pit/pit.grf", world}, {"Pit/pit.wtd", "terrain"}, {"Pit/pit-130.pid", set}, {"Pit/pit-130.swp", "pictures"}};
    CHECK(Install(user, "Undrawn", level).ok());

    shape.meshes.push_back({"Crate-01", "Map\\pit\\unused.bmp"});
    const ImportResult drawn = Install(user, "Drawn", With(level, "Pit/pit.grf", MakeWorld(shape)));
    CHECK(drawn.ok() && Logged(drawn.log, "MISSING_TEXTURES pit.grf: 1 of 3") && Logged(drawn.log, "grey texture stands for them") &&Logged(drawn.log, "Map\\pit\\unused.bmp (Crate-01)") && !Logged(drawn.log, "magnet.bmp"));

    const fs::path game = Fresh("drawn_game");
    user = Fresh("drawn_with_game");
    WorldShape own;
    own.terrain = "own";
    own.meshes = {{"Gate-00", "Map\\own\\unused.bmp"}};
    Write(game / "own.grf", MakeWorld(own));
    Write(game / "own-130.pid", TextureSet({{TextureHash("Map\\own\\other.bmp")}}));
    CHECK(Install(user, "GameLacks", With(level, "Pit/pit.grf", MakeWorld(shape)), game).ok());
    Write(game / "own-130.pid", TextureSet({{TextureHash("Map\\own\\unused.bmp")}}));
    // The game's own texture set for a name is never mixed in: the texture stays missing, and plays grey.
    const ImportResult has = Install(Fresh("drawn_game_has"), "GameHas", With(level, "Pit/pit.grf", MakeWorld(shape)), game);
    CHECK(has.ok() && Logged(has.log, "MISSING_TEXTURES pit.grf") && Logged(has.log, "Map\\pit\\unused.bmp (Crate-01)"));
}

static void TestGameSwap() {
    const fs::path user = Fresh("swap"), game = Fresh("swap_game");
    const std::string set = TextureSet({{TextureHash("Map\\pit\\crate.bmp")}});
    const Files level = {{"Pit/pit.scn", Scene("pit.grf")}, {"Pit/pit.grf", World("pit")}, {"Pit/pit.wtd", "terrain"}, {"Pit/pit-130.pid", set}};
    CHECK(Install(user, "None", level, game).status == ImportStatus::kMissingFiles);
    jpt_texture_set_t* s = jpt_texture_set_open(Bytes(set), set.size());
    const uint64_t bytes = s ? jpt_texture_set_bytes(s) : 0;
    jpt_texture_set_close(s);
    CHECK(bytes > 0);
    Write(game / "pit-130.spz", U32(uint32_t(bytes - 1)) + "packed");
    CHECK(Install(user, "Short", level, game).status == ImportStatus::kMissingFiles);
    Write(game / "pit-130.spz", U32(uint32_t(bytes)) + "packed");
    const ImportResult whole = Install(user, "Whole", level, game);
    CHECK(whole.ok());
    char out[512];
    CHECK(Activate(user, whole.package.c_str(), "Pit/pit.scn") && jpt_custom_level_find("pit-130.pid", out, sizeof out));
    CHECK(!jpt_custom_level_find("pit-130.spz", out, sizeof out) && !jpt_custom_level_find("pit-130.swp", out, sizeof out));
    jpt_custom_level_deactivate();
}

static uint32_t FortyTwo(const jpt_save_info_t*) { return 42; }

static std::string Problem(const std::string& bytes) {
    jpt_save_info_t info;
    return jpt_save_inspect(bytes.data(), bytes.size(), &info) ? "" : info.problem;
}

static void TestSaves() {
    for (const char* name : {"SAVEGAME.000", "savegame.QCK", "D:\\saves\\SAVEGAME.12345", "/x/SAVEGAME.999"}) CHECK(jpt_save_is_name(name));
    for (const char* name : {"SAVEGAME.12", "SAVEGAME.123456", "SAVEGAME.0x1", "SAVEGAME.TMP", "save.000", "SAVEGAME."}) CHECK(!jpt_save_is_name(name));
    CHECK(jpt_save_is_temporary("C:\\TEMP.SCX") && !jpt_save_is_temporary("temp.scn"));

    const std::string good = Save("w.grf", "w.scn");
    jpt_save_info_t info;
    CHECK(jpt_save_inspect(good.data(), good.size(), &info) && info.version == 22 && !std::strcmp(info.worlds[0], "w.grf") && !std::strcmp(info.scene, "w.scn") && !info.stamped && info.image_size == good.size());
    for (size_t n = 0; n < good.size(); ++n) CHECK(!Problem(good.substr(0, n)).empty());
    const auto patched = [&good](size_t at, uint32_t value) {
        std::string bytes = good;
        return Problem(bytes.replace(at, 4, U32(value)));
    };
    CHECK(patched(0, 1) == "it is not a scene file" && patched(32, 11) == "its container has another version");
    CHECK(patched(8, 0x7FFFFFFF) == "its section table does not fit the file" && patched(8, 0) == "its section table does not fit the file" && patched(12, 0xFFFFFFFF) == "it is cut short");
    CHECK(patched(48 + 12, (4 << 20) + 1) == "a section is larger than the game's buffer" && patched(48 + 12, 4 << 20) == "it is cut short");
    jpt_save_set_buffer(JPT_SAVE_SECTION_BYTES);
    CHECK(patched(48 + 12, (1 << 20) + 1) == "a section is larger than the game's buffer" && patched(48 + 12, 1 << 20) == "it is cut short");
    jpt_save_set_buffer(JPT_SAVE_SECTION_BYTES_MAX);
    CHECK(patched(48 + 12, (4 << 20) + 1) == "a section is larger than the game's buffer" && patched(48 + 12, 1 << 19) == "it is cut short" && !patched(48 + 12, 20).empty());
    CHECK(Problem(Save("w.grf", "w.scn", 23)) == "its version is not one of this game" && Problem(Save("w.grf", "w.scn", 6)) == "its version is not one of this game" && Problem(Save("w.grf", "a name longer than it.scn")) == "its level's name is damaged");
    const auto counted = [](const char* section, size_t at, uint32_t count, size_t size) {
        std::string part(size, '\0');
        part.replace(at, 4, U32(count));
        return Problem(Container({{"Header", HeaderSection("w.grf", 22)}, {"Partitions", "p"}, {"Hierarchy", "h"}, {section, part}}));
    };
    CHECK(counted("A9AC56AE", 60, 3, 76).empty() && counted("A9AC56AE", 60, 4, 76) == "its animals do not fit their section" && counted("A9AC56AE", 60, 0x40000001, 76) == "its animals do not fit their section");
    CHECK(counted("9FB57F53", 72, 2, 72 + 4 + 96).empty() && counted("9FB57F53", 72, 3, 72 + 4 + 96) == "the player's joints do not fit their section" && counted("9FB57F53", 72, 33, 72 + 4 + 48 * 33) == "the player's joints do not fit their section");
    CHECK(Problem(Scene("w.grf")) == "it has no objects" && Problem(Container({{"Partitions", "p"}})) == "it has no header" && Problem("") != "");
    std::string endless = good;
    endless.replace(endless.find("c:\\tres cd"), 10, std::string(10, 'x'));
    for (size_t i = endless.find("xxxxxxxxxx"), n = 0; n < 256; ++n) endless[i + n] = 'x';
    CHECK(Problem(endless) == "a world's name in it is damaged");
    for (size_t at = 0; at + 4 <= good.size(); ++at) Problem(std::string(good).replace(at, 4, U32(0xFFFFFFFE)));

    // Written whole or not at all.
    const fs::path dir = Fresh("saves");
    const std::string path = (dir / "SAVEGAME.001").string();
    char temp[512], why[160];
    CHECK(jpt_save_begin(path.c_str(), temp, sizeof temp) && fs::path(temp).parent_path() == dir && !jpt_save_is_name(temp) && !jpt_save_begin((dir / "other.txt").string().c_str(), temp, sizeof temp));
    CHECK(jpt_save_begin(path.c_str(), temp, sizeof temp));
    Write(temp, good);
    CHECK(jpt_save_finish(path.c_str(), temp, "", FortyTwo, why, sizeof why) && Read(path) == good && !fs::exists(temp));
    for (const std::string& broken : {std::string(), good.substr(0, good.size() / 2), std::string("SZDD")}) {
        Write(temp, broken);
        CHECK(!jpt_save_finish(path.c_str(), temp, "", FortyTwo, why, sizeof why) && why[0] && Read(path) == good && !fs::exists(temp));
    }
    Write(temp, good);
    CHECK(jpt_save_finish(path.c_str(), temp, "Isle", FortyTwo, why, sizeof why) && Read(path).size() > good.size() && Read(path).compare(0, good.size(), good) == 0);
    CHECK(jpt_save_inspect_file(path.c_str(), &info) && info.stamped && !std::strcmp(info.package, "Isle") && info.content == 42 && info.image_size == good.size());
    CHECK(jpt_save_belongs(&info, "Isle", 42, why, sizeof why) && !jpt_save_belongs(&info, "Other", 42, why, sizeof why) && std::strstr(why, "save incompatible with current custom level") && !jpt_save_belongs(&info, "Isle", 43, why, sizeof why) &&
          !jpt_save_belongs(&info, "", 42, why, sizeof why) && !jpt_save_belongs(&info, nullptr, 42, why, sizeof why));
    CHECK(!jpt_save_inspect_file((dir / "SAVEGAME.404").string().c_str(), &info) && info.problem[0]);
    CHECK(jpt_gate_lists(path.c_str(), why, sizeof why) && !jpt_gate_lists((dir / "x.txt").string().c_str(), why, sizeof why));
    Write(dir / "SAVEGAME.002", good.substr(0, 700));
    CHECK(!jpt_gate_lists((dir / "SAVEGAME.002").string().c_str(), why, sizeof why) && why[0]);
}

static void TestGate() {
    const fs::path user = Fresh("gate user"), saves = Fresh("gate saves");
    g_game = Fresh("gate game");
    WriteTree(g_game, {{"be.grf", World("be")}, {"be.scn", Scene("be.grf")}});
    const Files one = Level("", "one");
    CHECK(Install(user, "One", one + Level("", "two")).ok() && Install(user, "Two", With(Level("", "one"), "one.grf", World("one") + "x")).ok());
    const std::string slot = (saves / "SAVEGAME.QCK").string();
    char why[256], temp[512];
    const auto gate = [&](const char* asked) { return jpt_gate_load(slot.c_str(), asked, why, sizeof why); };
    const auto save_as = [&](const std::string& bytes) {
        CHECK(jpt_gate_save_begin(slot.c_str(), temp, sizeof temp));
        Write(temp, bytes);
        return jpt_gate_save_end(slot.c_str(), temp, why, sizeof why) != 0;
    };

    CHECK(Activate(user, "One", "one.scn") && gate("C:\\data\\one.scn") == JPT_GATE_OPEN && gate("two.scn") == JPT_GATE_OPEN && gate("be.scn") == JPT_GATE_OPEN);
    CHECK(gate("a level name that is long.scn") == JPT_GATE_UNSAFE && !jpt_gate_message(JPT_GATE_OPEN));
    CHECK(save_as(Save("one.grf", "one.scn")) && gate("SAVEGAME.QCK") == JPT_GATE_OPEN && !jpt_gate_save_failed());
    CHECK(!save_as(Save("one.grf", "one.scn").substr(0, 900)) && jpt_gate_save_failed() && !jpt_gate_save_failed() && gate("SAVEGAME.QCK") == JPT_GATE_OPEN);

    // Another package's save, the campaign's session, and the package after a change.
    CHECK(Activate(user, "Two", "one.scn") && gate("SAVEGAME.QCK") == JPT_GATE_INCOMPATIBLE && std::strstr(jpt_gate_message(JPT_GATE_INCOMPATIBLE), "not compatible"));
    jpt_custom_level_deactivate();
    CHECK(gate("SAVEGAME.QCK") == JPT_GATE_INCOMPATIBLE);
    Write(slot, Save("be.grf", "be.scn"));
    CHECK(gate("SAVEGAME.QCK") == JPT_GATE_OPEN && gate("be.scn") == JPT_GATE_OPEN);
    Write(slot, Save("be.grf", "be.scn").substr(0, 800));
    CHECK(gate("SAVEGAME.QCK") == JPT_GATE_DAMAGED && jpt_gate_load((saves / "gone").string().c_str(), "SAVEGAME.QCK", why, sizeof why) == JPT_GATE_DAMAGED);
    CHECK(Activate(user, "One", "one.scn") && save_as(Save("one.grf", "one.scn")));
    Write(CustomLevelsDir(user) / "One" / "one.grf", World("one") + "changed");
    CHECK(gate("SAVEGAME.QCK") == JPT_GATE_INCOMPATIBLE && std::strstr(why, "changed since"));
    Write(CustomLevelsDir(user) / "One" / "one.grf", World("one"));
    CHECK(gate("SAVEGAME.QCK") == JPT_GATE_OPEN);

    const std::string far_away = "C:\\" + std::string(240, 'd') + "\\one.scn";
    CHECK(gate(far_away.c_str()) == JPT_GATE_OPEN && gate(("C:\\" + std::string(250, 'd') + "\\one.scn").c_str()) == JPT_GATE_UNSAFE && std::strstr(why, "engine limit exceeded: the paths"));

    // A level that fails leaves the others as they were.
    const fs::path home = CustomLevelsDir(user) / "One";
    Write(home / "two.grf", World("two").substr(0, 200));
    CHECK(gate("two.scn") == JPT_GATE_UNSAFE && std::strstr(why, "two.grf") && gate("one.scn") == JPT_GATE_OPEN);
    WorldShape beyond;
    beyond.trigger_actions = 17;
    Write(home / "two.grf", MakeWorld(beyond));
    CHECK(gate("two.scn") == JPT_GATE_UNSAFE && std::strstr(why, "engine limit exceeded") && std::strstr(why, "TRIGGER_ACTIONS_EXCEED_LIMIT"));
    WorldShape broken;
    broken.meeting = 40, broken.optimized = false, broken.normals = 2, broken.fan_face = {0, 1, 0xFFFFFFFFu};
    Write(home / "two.grf", MakeWorld(broken));
    CHECK(gate("two.scn") == JPT_GATE_UNSAFE && std::strstr(why, "INVALID_MESH_INDEX"));
    Write(home / "two.scn", Scene("two.grf").substr(0, 1000));
    CHECK(gate("two.scn") == JPT_GATE_UNSAFE);
    Write(home / "two.scn", Scene("gone.grf"));
    CHECK(gate("two.scn") == JPT_GATE_MISSING && gate("one.scn") == JPT_GATE_OPEN);
    jpt_custom_level_deactivate();
    CHECK(gate("two.scn") == JPT_GATE_OPEN);
}

// Memory that runs out at any point of a load: an answer that says so, nothing half done, and the next load as if nothing was.
static void TestOutOfMemory() {
    const fs::path user = Fresh("memory user");
    g_game = Fresh("memory game");
    const WorldShape shape = Curved();
    const std::string world = MakeWorld(shape), set = TextureSet({{HashOf(shape.meshes[0]), true, 0xFFFFFFFF}});
    Place(user, "W", {{"w.scn", Scene("w.grf")}, {"w.grf", world}, {"w.wtd", "t"}, {"w-130.pid", set}, {"w-130.swp", "s"}, {"v.scn", Scene("w.grf", 25)}});
    CHECK(!ListLevelPackages(user, {}).empty());
    CHECK(Activate(user, "W", "w.scn"));
    char why[256];
    size_t failures = 0;
    bool whole = false;
    for (long n = 1; n < 200 && !whole; ++n) {
        std::string bytes = world;
        jpt_level_world_t out;
        size_t size = 0;
        jpt_alloc_fail_at(n);
        jpt_level_world_prepare("w.grf", reinterpret_cast<uint8_t*>(bytes.data()), bytes.size(), &out);
        jpt_alloc_fail_at(0);
        uint8_t* pid = jpt_level_world_take_set("w-130.pid", &size);
        whole = !out.failed && pid && out.hidden == 1;
        // Without its set the engine reads the file itself: that must never be one it cannot take.
        CHECK(whole || out.failed || out.set[0] == 0);
        failures += out.failed;
        std::free(pid);
    }
    CHECK(whole && failures > 8);
    // Its curved bump maps are not in its set: INVALID / UNSAFE, refused before its world is read.
    CHECK(jpt_gate_load("", "w.scn", why, sizeof why) == JPT_GATE_UNSAFE && std::string(why).find("INVALID") != std::string::npos);
    {
        const fs::path map = CustomLevelsDir(user) / "W" / JPT_CUSTOM_LEVEL_RUNTIME_MAP;
        std::string kept;
        std::istringstream lines(Read(map));
        for (std::string line; std::getline(lines, line);)
            if (line.rfind("compat ", 0) != 0) kept += line + "\n";
        Write(map, kept);
    }
    CHECK(Activate(user, "W", "w.scn"));
    CHECK(jpt_gate_load("", "w.scn", why, sizeof why) == JPT_GATE_OPEN);
    for (long n = 1; n < 12; ++n) {
        jpt_alloc_fail_at(n);
        const jpt_gate_t gate = jpt_gate_load("", "w.scn", why, sizeof why);
        jpt_alloc_fail_at(0);
        CHECK(gate == JPT_GATE_OPEN || gate == JPT_GATE_UNSAFE);
        if (n < 3) CHECK(gate == JPT_GATE_UNSAFE);
    }
    CHECK(jpt_gate_load("", "w.scn", why, sizeof why) == JPT_GATE_OPEN);

    const std::string packed("SZDD\x88\xF0\x27\x33" "A\0\x07\0\0\0\x07" "abc\xF0\xF1", 20);
    size_t size = 0;
    jpt_alloc_fail_at(1);
    CHECK(!jpt_szdd_expand(Bytes(packed), packed.size(), &size));
    jpt_alloc_fail_at(0);
    std::string bomb = packed;
    bomb.replace(10, 4, U32(0x7FFFFFFF));
    CHECK(!jpt_szdd_expand(Bytes(bomb), bomb.size(), &size));

    const std::string later = Scene("w.grf", 25);
    jpt_scene_report_t report;
    uint8_t* adapted = nullptr;
    jpt_alloc_fail_at(1);
    adapted = jpt_scene_adapt(later.data(), later.size(), &size, &report);
    CHECK(!adapted);
    jpt_alloc_fail_at(0);

    CHECK(!jpt_alloc(SIZE_MAX, 2) && !jpt_alloc(SIZE_MAX / 2, 3) && !jpt_alloc_zero(SIZE_MAX / 16, 32) && !jpt_alloc_again(nullptr, SIZE_MAX / 4, 8) && !jpt_alloc(JPT_ALLOC_MAX + 1, 1));
    void* none = jpt_alloc(0, 8);
    CHECK(none != nullptr);
    std::free(none);
    jpt_custom_level_deactivate();
    jpt_level_world_forget();
}

static LevelPackage Listed(const char* id, int64_t when, std::vector<std::pair<const char*, bool>> files) {
    LevelPackage package;
    package.id = id;
    package.imported_at = when;
    package.layout = "loose";
    for (const auto& [name, start] : files) {
        LevelScene scene;
        scene.file = scene.path = name;
        scene.valid = true;
        scene.version = 22;
        scene.state = LevelState::kReady;
        scene.start = start;
        package.scenes.push_back(scene);
    }
    return package;
}

static std::string Order(const std::vector<LevelPackage>& packages, LevelSort sort) {
    std::string text;
    for (const LevelRow& row : SortedRows(packages, sort)) text += (text.empty() ? "" : " ") + row.package->id + "/" + (row.scene ? row.scene->file : "-");
    return text;
}

static void TestSorting() {
    std::vector<LevelPackage> packages = {Listed("beta", 200, {{"b.scn", false}, {"A.scn", false}}), Listed("Alpha", 300, {{"z.scn", false}}), Listed("gamma", 200, {{"g.scn", true}}), Listed("delta", 100, {})};
    CHECK(Order(packages, LevelSort::kNewest) == "Alpha/z.scn gamma/g.scn beta/A.scn beta/b.scn delta/-");
    CHECK(Order(packages, LevelSort::kOldest) == "delta/- gamma/g.scn beta/A.scn beta/b.scn Alpha/z.scn");
    CHECK(Order(packages, LevelSort::kNameUp) == "Alpha/z.scn beta/A.scn beta/b.scn delta/- gamma/g.scn");
    CHECK(Order(packages, LevelSort::kNameDown) == "gamma/g.scn delta/- beta/b.scn beta/A.scn Alpha/z.scn");
    std::set<std::string> orders;
    std::sort(packages.begin(), packages.end(), [](const LevelPackage& a, const LevelPackage& b) { return a.id < b.id; });
    do orders.insert(Order(packages, LevelSort::kNewest));
    while (std::next_permutation(packages.begin(), packages.end(), [](const LevelPackage& a, const LevelPackage& b) { return a.id < b.id; }));
    CHECK(orders.size() == 1);
    for (const LevelSort sort : {LevelSort::kNewest, LevelSort::kOldest, LevelSort::kNameUp, LevelSort::kNameDown}) CHECK(LevelSortOf(LevelSortKey(sort)) == sort);
    CHECK(LevelSortOf("") == LevelSort::kNewest && LevelSortOf("nonsense") == LevelSort::kNewest);

    // Kept from the import on; a package of an earlier version takes the date of its oldest file.
    const fs::path user = Fresh("dates");
    CHECK(Install(user, "One", Level("", "one")).ok());
    const fs::path manifest = CustomLevelsDir(user) / "One" / kManifestFile;
    const int64_t when = ListLevelPackages(user, {})[0].imported_at;
    const std::string text = Read(manifest);
    CHECK(when > 1700000000 && text.find("imported_at") != std::string::npos && DateOf(DateText(when)) == when && DateOf("") == 0 && DateOf("yesterday") == 0);
    fs::last_write_time(CustomLevelsDir(user) / "One" / "one.wtd", fs::file_time_type::clock::now() + std::chrono::hours(48));
    CHECK(ListLevelPackages(user, {})[0].imported_at == when);
    fs::last_write_time(CustomLevelsDir(user) / "One" / "one.wtd", fs::file_time_type::clock::now() - std::chrono::hours(48));
    fs::remove(manifest);
    const int64_t fallback = ListLevelPackages(user, {})[0].imported_at;
    CHECK(fallback < when - 47 * 3600 && fallback > when - 49 * 3600 && ListLevelPackages(user, {})[0].imported_at == fallback && Read(manifest).find("imported_at") != std::string::npos);
    fs::last_write_time(CustomLevelsDir(user) / "One" / "one.wtd", fs::file_time_type::clock::now());
    CHECK(ListLevelPackages(user, {})[0].imported_at == fallback);
}

static std::vector<std::string> Snapshot(const fs::path& root) {
    std::vector<std::string> lines;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) lines.push_back(Utf8(entry.path().lexically_relative(root)) + " " + std::to_string(std::hash<std::string>()(Read(entry.path()))));
    std::sort(lines.begin(), lines.end());
    return lines;
}

static void TestCorpus() {
    const fs::path root = Fresh("corpus"), game = Fresh("corpus game");
    for (const char* name : {"be.grf", "be-130.pid", "be-130.spz", "be.wtd"}) Write(game / name, "retail");
    const auto mission = [](const std::string& stem) { return Level("M/data/", stem) + Level("M/data/", "next") + Files{{"M/fm.ini", "[General]\nStartLevel=" + stem + ".scn\n"}}; };
    const Files later = With(Level("", "late"), "late.scn", Container({{"Header", HeaderSection("late.grf", 27)}}));
    WorldShape actions;
    actions.terrain = "act";
    actions.actions = {0, 112, 112, 1000};
    actions.levels = {"gone.scn"};
    actions.meshes.push_back({"Crate-00", "Map\\act\\crate.bmp"});

    WriteTree(root / "loose", Level("", "loose"));
    WriteZip(root / "deep" / "er" / "loose copy.zip", Level("", "loose"), 0);
    WriteZip(root / "mission.zip", mission("start"), 0);
    Write(root / "seven.7z", SevenZip(Level("", "seven"), 0));
    WriteTree(root / "folder mod", mission("begin"));
    WriteTree(root / "no terrain", With(Level("", "isle"), "isle.grf", World("Isle-27")) + Level("", "fine"));
    WriteTree(root / "no set", Without(Without(Level("", "bare"), "bare-130.pid"), "bare-130.swp"));
    WriteTree(root / "no world", Without(Level("", "lost"), "lost.grf"));
    WriteTree(root / "reuses", {{"mine.scn", Scene("be.grf")}});
    WriteTree(root / "future", later);
    WriteTree(root / "actions", With(Level("", "act"), "act.grf", MakeWorld(actions)) + Files{{"texhw/map/a.dds", "d"}, {"texhw/map/b.tga", "t"}, {"tp_mod.ini", "[General]\nAuthor=x\n"}});
    WriteZip(root / "programs.zip", Level("", "prog") + Files{{"setup.exe", "MZ"}, {"run.bat", "del"}, {"x.ps1", "x"}, {"a.lnk", "x"}}, 0);
    WriteZip(root / "escape.zip", Level("", "evil") + Files{{"../../out.scn", "x"}}, 0);
    Write(root / "cut.zip", Read(root / "mission.zip").substr(0, 300));
    Write(root / "other.rar", "Rar!");
    Write(root / "noise.txt", "text");

    const std::vector<std::string> before = Snapshot(root), game_before = Snapshot(game);
    const CorpusReport report = ScanCorpus(root, game);
    CHECK(Snapshot(root) == before && Snapshot(game) == game_before && !fs::exists(g_scratch / "out.scn") && !fs::exists(root.parent_path() / "out.scn"));
    const auto find = [&report](const char* path) {
        const auto it = std::find_if(report.packages.begin(), report.packages.end(), [path](const CorpusPackage& p) { return p.path == path; });
        static const CorpusPackage none;
        CHECK(it != report.packages.end());
        return it == report.packages.end() ? none : *it;
    };
    const auto class_of = [](const CorpusPackage& p, const char* file) {
        for (const LevelScene& s : p.result.scenes)
            if (s.file == file) return std::string(LevelClassName(s.level_class()));
        return std::string("none");
    };
    CHECK(report.packages.size() == 15 && std::is_sorted(report.packages.begin(), report.packages.end(), [](const CorpusPackage& a, const CorpusPackage& b) { return LowerAscii(a.path) < LowerAscii(b.path); }));
    CHECK(class_of(find("loose"), "loose.scn") == "none" || class_of(find("loose"), "loose.scn") == "READY");
    CHECK(find("deep/er/loose copy.zip").duplicate_of.empty() != find("loose").duplicate_of.empty() && report.totals.at("duplicates") == 1);
    CHECK(class_of(find("seven.7z"), "seven.scn") == "READY" && find("seven.7z").result.type == "7z" && class_of(find("mission.zip"), "start.scn") == "READY" && find("mission.zip").result.sha256.size() == 64);
    CHECK(find("folder mod/M").result.analysis.start_level.find("begin.scn") != std::string::npos);
    CHECK(class_of(find("no terrain"), "isle.scn") == "REJECTED_MISSING" && class_of(find("no terrain"), "fine.scn") == "READY" && find("no terrain").result.import.ok());
    CHECK(class_of(find("no set"), "bare.scn") == "REJECTED_MISSING" && class_of(find("no world"), "lost.scn") == "REJECTED_MISSING");
    CHECK(class_of(find("reuses"), "mine.scn") == "READY" && report.totals.at("use_game_resources") == 1);
    CHECK(class_of(find("future"), "late.scn") == "REJECTED_UNSUPPORTED" && class_of(find("actions"), "act.scn") == "READY");
    CHECK(report.totals.at("package READY") >= 1 && report.totals.at("package REJECTED_MISSING") == 3 && !report.totals.count("READY_WITH_WARNINGS"));
    CHECK(report.actions.at(112) == 1 && report.actions.at(1000) == 1 && !report.actions.count(0) && report.totals.at("with_texhw") == 1 && report.totals.at("with_dds") == 1 && report.totals.at("with_tga") == 1);
    CHECK(find("programs.zip").result.import.refused.size() == 4 && class_of(find("programs.zip"), "prog.scn") == "READY" && find("escape.zip").result.import.status == ImportStatus::kUnsafe && find("escape.zip").result.scenes.empty());
    CHECK(!find("cut.zip").result.import.ok() && !find("other.rar").unread.empty() && report.totals.at("crash_signature_no_terrain") == 1 && report.totals.at("missing_resources") == 3);
    CHECK(!report.categories.empty() && std::is_sorted(report.categories.begin(), report.categories.end(), [](const CorpusProblem& a, const CorpusProblem& b) { return a.levels > b.levels; }));
    const std::string text = CorpusText(report), json = CorpusJson(report);
    CHECK(text.find("TRULY_MISSING") != std::string::npos && json.find("RESOLVED_FROM_GAME") != std::string::npos && json.find("IN_PACKAGE") != std::string::npos && text.find(Utf8(root)) == std::string::npos && json.find(Utf8(root.filename()) + "/") == std::string::npos);
    const CorpusReport again = ScanCorpus(root, game);
    CHECK(CorpusText(again) == text && CorpusJson(again) == json);

    CorpusLimits limits;
    limits.max_packages = 4;
    CHECK(ScanCorpus(root, game, limits).packages.size() == 4 && !ScanCorpus(root, game, limits).notes.empty());
    limits = {};
    limits.max_depth = 1;
    const CorpusReport shallow = ScanCorpus(root, game, limits);
    CHECK(shallow.packages.size() == 13 && !shallow.notes.empty());
    CHECK(ScanCorpus(root / "nowhere", game).packages.empty());
}

static void TestStates() {
    const auto status = [](const std::vector<IssuePolicy>& policies) {
        LevelScene s;
        for (const IssuePolicy policy : policies) s.issues.push_back({"x", "a", "b", policy});
        return std::string(LevelStatusText(s));
    };
    const auto class_of = [](const std::vector<IssuePolicy>& policies) {
        std::vector<LevelIssue> issues;
        for (const IssuePolicy policy : policies) issues.push_back({"x", "a", "b", policy});
        return std::string(LevelClassName(ClassOf(issues)));
    };
    LevelScene ready;
    ready.state = LevelState::kReady;
    CHECK(std::string(LevelStatusText(ready)) == "Ready" && status({}) == "Invalid / Unsafe" && status({IssuePolicy::kInfo}) == "Invalid / Unsafe");
    LevelScene limited;
    limited.state = LevelState::kLimited;
    CHECK(std::string(LevelStatusText(limited)) == "Limited Compatibility" && limited.launchable() && !limited.playable());
    CHECK(class_of({}) == "READY" && class_of({IssuePolicy::kInfo, IssuePolicy::kInfo}) == "READY" && class_of({IssuePolicy::kWarning}) == "REJECTED_OTHER");
    CHECK(class_of({IssuePolicy::kWarning, IssuePolicy::kMissing}) == "REJECTED_MISSING" && class_of({IssuePolicy::kMissing, IssuePolicy::kUnsupported}) == "REJECTED_UNSUPPORTED" &&
          class_of({IssuePolicy::kUnsupported, IssuePolicy::kMalformed}) == "REJECTED_MALFORMED");
    for (const IssuePolicy policy : {IssuePolicy::kWarning, IssuePolicy::kMissing, IssuePolicy::kUnsupported, IssuePolicy::kMalformed}) CHECK((LevelIssue{"x", "a", "b", policy}.blocks()));
    CHECK(!(LevelIssue{"x", "a", "b", IssuePolicy::kInfo}.blocks()));

    // A world or scene that cannot be read refuses its own level only: the package is installed for the one that is READY.
    const fs::path user = Fresh("states check"), game = Fresh("states check game");
    const Files mixed = With(Level("", "bad"), "bad.grf", World("bad").substr(0, 300)) + Level("", "good") + With(Level("", "cut"), "cut.scn", Scene("cut.grf").substr(0, 1380));
    const ImportResult result = Install(user, "Mixed", mixed, game);
    CHECK(result.ok() && result.levels == 1 && fs::exists(CustomLevelsDir(user) / "Mixed"));
    const PlayCheck bad = CheckBeforePlay(user, game, "Mixed", "bad.scn");
    CHECK(!bad.ok && bad.reason == ImportStatus::kUnsupportedFormat && Logged(bad.log, "bad.grf") && CheckBeforePlay(user, game, "Mixed", "good.scn").ok);
    CHECK(!CheckBeforePlay(user, game, "Mixed", "cut.scn").ok);

    // Some textures missing: the level plays as in Trespasser CE, with its grey texture for them.
    WorldShape some;
    some.terrain = "part";
    some.meshes.push_back({"Crate-00", "Map\\part\\crate.bmp"});
    some.meshes.push_back({"Lost-00", "Map\\elsewhere\\lost.bmp"});
    const ImportResult partly = Install(user, "Partly", With(Level("", "part"), "part.grf", MakeWorld(some)), game);
    CHECK(partly.ok() && partly.levels == 1 && Logged(partly.log, "MISSING_TEXTURES part.grf: 1 of 2 textures") && Logged(partly.log, "grey texture stands for them"));
    CHECK(Logged(partly.log, "class: READY") && !Logged(partly.log, "REJECTED"));

    // A face with a corner past the mesh's points keeps its level from READY, alone or beside one that is.
    WorldShape broken;
    broken.terrain = "fan";
    broken.meeting = 40, broken.optimized = false, broken.normals = 2, broken.fan_face = {0, 1, 0xFFFFFFFFu};
    const Files fan = With(Level("", "fan"), "fan.grf", MakeWorld(broken));
    const ImportResult alone = Install(user, "Fan", fan, game);
    CHECK(!alone.ok() && alone.levels == 0 && Logged(alone.log, "INVALID_MESH_INDEX") && Logged(alone.log, "class: REJECTED_UNSUPPORTED") && !Logged(alone.log, "class: READY"));
    CHECK(!fs::exists(CustomLevelsDir(user) / "Fan"));
    const ImportResult beside = Install(user, "FanAndGood", fan + Level("", "good"), game);
    CHECK(beside.ok() && beside.levels == 1 && !CheckBeforePlay(user, game, "FanAndGood", "fan.scn").ok && CheckBeforePlay(user, game, "FanAndGood", "good.scn").ok);

    // None of the world's textures anywhere: the texture set is not the world's, and the level is refused.
    const ImportResult pictures = Install(user, "Pictures", With(Level("", "pic"), "pic-130.pid", TextureSet({{1}})), game);
    CHECK(!pictures.ok() && pictures.status == ImportStatus::kMissingFiles && pictures.levels == 0 && Logged(pictures.log, "MISSING_TEXTURES") && Logged(pictures.log, "class: REJECTED_MISSING"));
    CHECK(!fs::exists(CustomLevelsDir(user) / "Pictures") && ImportNotice(pictures.status).title == "Could not import this custom level" && ImportNotice(pictures.status).reason == "This custom level is not fully supported" && ImportNotice(pictures.status).failed);
}

// Trespasser CE: the script classes a world names, by Script on objects and ScriptAT on a trigger's action 99, and their way
// through the facts kept for an installed package.
static void TestScriptReferences() {
    WorldShape shape;
    shape.terrain = "w";
    shape.scripted = {{"Door-00", "Door"}, {"Door-01", "Door"}, {"Lamp-00", "Lamp"}};
    shape.scripted_actions = {{"Trig-00", "MyAT"}};
    jpt_groff_report_t report{};
    CHECK(Describe(MakeWorld(shape), report));
    CHECK(report.script_count == 3 && !report.scripts_overflow);
    CHECK(!strcmp(report.scripts[0].class_name, "Door") && !strcmp(report.scripts[0].object, "Door-00") && report.scripts[0].count == 2 && !report.scripts[0].action);
    CHECK(!strcmp(report.scripts[1].class_name, "Lamp") && report.scripts[1].count == 1);
    CHECK(!strcmp(report.scripts[2].class_name, "MyAT") && !strcmp(report.scripts[2].object, "Trig-00") && report.scripts[2].action);
    WorldShape none;
    none.terrain = "w";
    none.actions = {99};
    CHECK(Describe(MakeWorld(none), report) && report.script_count == 0);
    WorldShape many;
    many.terrain = "w";
    for (int i = 0; i < JPT_GROFF_SCRIPTS_MAX + 2; ++i) many.scripted.push_back({"Box-" + std::to_string(i), "Class" + std::to_string(i)});
    CHECK(Describe(MakeWorld(many), report) && report.script_count == JPT_GROFF_SCRIPTS_MAX && report.scripts_overflow);

    WorldFacts facts, read;
    facts.scripts = {{"Door", "Door-00", false, 2}, {"MyAT", "Trig 00", true, 1}};
    CHECK(ReadWorldFacts(WorldFactsText(facts), read) && read.scripts.size() == 2);
    if (read.scripts.size() == 2) {
        CHECK(read.scripts[0].class_name == "Door" && read.scripts[0].object == "Door-00" && read.scripts[0].count == 2 && !read.scripts[0].action);
        CHECK(read.scripts[1].class_name == "MyAT" && read.scripts[1].object == "Trig 00" && read.scripts[1].action);
    }
    CHECK(!ReadWorldFacts("flags 100\nscript 2 1 X y\n", read) && !ReadWorldFacts("flags 100\nscript 0 X\n", read));
    CHECK(ReadWorldFacts("flags 100\nscript 0 1 X\n", read) && read.scripts.size() == 1 && read.scripts[0].class_name == "X" && read.scripts[0].object.empty());
}

void RunLevelCheckTests(const fs::path& scratch, int& checks, int& failures) {
    g_scratch = scratch;
    TestWorlds();
    TestFaults();
    TestMissingTerrain();
    TestCurvedMaps();
    TestAnimatedTextures();
    TestDrawnTextures();
    TestGameSwap();
    TestSaves();
    TestGate();
    TestOutOfMemory();
    TestSorting();
    TestStates();
    TestCorpus();
    TestScriptReferences();
    checks += g_checks;
    failures += g_failures;
}
