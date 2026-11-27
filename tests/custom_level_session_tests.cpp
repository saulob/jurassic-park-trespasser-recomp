// An active custom level session as the game reads it, headless: the pictures of ATX map actions below the package's MAPS, and
// Restart Level, which reads the current scene again. Packages are made here; their names decide nothing.
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "jpt_level_scripts.h"
#include "jpt_groff.h"
#include "jpt_level_session.h"
#include "custom_level_guide.h"
#include "jpt_picture.h"
#include "level_fixtures.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;
using namespace fixtures;

using Files = std::vector<std::pair<std::string, std::string>>;

static int g_checks, g_failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static fs::path g_scratch, g_game;

static void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

static fs::path Fresh(const std::string& name) {
    const fs::path dir = g_scratch / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static std::string Scene(const std::string& world, uint32_t version = 22) { return Container({{"Header", HeaderSection(world, version)}}); }

// A level of one crate: its scene, world, texture set, swap file and terrain.
static Files Level(const std::string& folder, const std::string& stem, uint32_t version = 22, WorldShape shape = {}) {
    shape.terrain = stem;
    shape.meshes.push_back({"Crate-00", "Map\\" + stem + "\\crate.bmp"});
    return {{folder + stem + ".scn", Scene(stem + ".grf", version)}, {folder + stem + ".grf", MakeWorld(shape)},
            {folder + stem + "-130.pid", TextureSet({{TextureHash("Map\\" + stem + "\\crate.bmp")}})}, {folder + stem + "-130.swp", "swap"},
            {folder + stem + ".wtd", "terrain of " + stem}};
}

static WorldShape Showing(std::vector<std::string> maps, std::vector<std::string> levels = {}) {
    WorldShape shape;
    shape.maps = std::move(maps);
    shape.levels = std::move(levels);
    return shape;
}

static std::string U16(uint32_t v) { return std::string(1, char(v)) + char(v >> 8); }

// colour(x, y) of the pictures below, top row first.
static uint32_t Colour(int x, int y) { return uint32_t(x * 40) << 16 | uint32_t(y * 60) << 8 | 0xC0u; }

// An uncompressed bitmap of a few layouts GDI draws: 24 bits, 8 bits with a palette, 16 bits 5-6-5, and the 12-byte OS/2 header.
static std::string Bmp(int w, int h, int bits, bool os2 = false) {
    const bool indexed = bits <= 8, fields = bits == 16;
    const uint32_t header = os2 ? 12u : 40u, colours = indexed ? (os2 ? 1u << bits : 4u) : 0u, entry = os2 ? 3u : 4u;
    const uint32_t stride = uint32_t(((w * bits + 31) / 32) * 4), offset = 14u + header + (fields ? 12u : 0u) + colours * entry;
    std::string out = "BM" + U32(offset + stride * uint32_t(h)) + U32(0) + U32(offset);
    if (os2) out += U32(12) + U16(uint32_t(w)) + U16(uint32_t(h)) + U16(1) + U16(uint32_t(bits));
    else out += U32(40) + U32(uint32_t(w)) + U32(uint32_t(h)) + U16(1) + U16(uint32_t(bits)) + U32(fields ? 3u : 0u) + U32(stride * uint32_t(h)) + U32(0) + U32(0) + U32(colours) + U32(0);
    if (fields) out += U32(0xF800u) + U32(0x7E0u) + U32(0x1Fu);
    for (uint32_t i = 0; i < colours; ++i) {
        const uint32_t c = Colour(int(i), int(i));
        out += std::string{char(c), char(c >> 8), char(c >> 16)} + (os2 ? "" : std::string(1, '\0'));
    }
    for (int row = h - 1; row >= 0; --row) {
        std::string line(stride, '\0');
        size_t at = 0;
        for (int x = 0; x < w; ++x) {
            const uint32_t c = Colour(x, row);
            if (indexed) {
                const size_t bit = size_t(x) * size_t(bits);
                line[bit / 8] = char(uint8_t(line[bit / 8]) | uint8_t(((x + row) % 4) << (8 - bits - int(bit % 8))));
                continue;
            }
            std::string pixel;
            if (fields) pixel = U16((((c >> 16) & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
            else pixel = std::string{char(c), char(c >> 8), char(c >> 16)};
            line.replace(at, pixel.size(), pixel);
            at += pixel.size();
        }
        out += line;
    }
    return out;
}

static std::string Fixture(const Files& files, const std::string& path) {
    for (const auto& [name, bytes] : files)
        if (name == path) return bytes;
    return {};
}

static std::string Tpa() { return std::string("\x50\x01\0\0", 4) + std::string(60, '\0'); }

static ImportResult Install(const fs::path& user, const std::string& name, const Files& files) {
    const fs::path source = Fresh("source") / name;
    for (const auto& [path, bytes] : files) Write(source / PathFromUtf8(path), bytes);
    return ImportLevelFolder(source, user, {}, {}, g_game);
}

static int GameFile(const char* relative, char* out, size_t cap) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(g_game, ec))
        if (SameText(Utf8(entry.path().filename()), PathName(relative))) return std::snprintf(out, cap, "%s", Utf8(entry.path()).c_str()) > 0;
    return 0;
}

static bool Activate(const fs::path& user, const std::string& package, const std::string& scene) {
    jpt_custom_level_request_t request{};
    char error[256] = "";
    std::snprintf(request.package, sizeof request.package, "%s", package.c_str());
    std::snprintf(request.scene, sizeof request.scene, "%s", scene.c_str());
    const bool ok = jpt_custom_level_activate(Utf8(user).c_str(), &request, error, sizeof error) != 0;
    if (!ok) std::printf("activate %s %s: %s\n", package.c_str(), scene.c_str(), error);
    jpt_custom_level_set_game(GameFile);
    return ok;
}

// The package's file that answers, as a path below its folder; empty when the game's own answers.
static std::string Answer(int (*find)(const char*, char*, size_t), const fs::path& user, const std::string& package, const char* name) {
    char out[1024];
    if (!find(name, out, sizeof out)) return {};
    std::string relative = Utf8(fs::path(PathFromUtf8(out)).lexically_relative(CustomLevelsDir(user) / package));
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

static std::string Found(const fs::path& user, const std::string& package, const char* name) { return Answer(jpt_custom_level_find, user, package, name); }
static std::string Mapped(const fs::path& user, const std::string& package, const char* name) { return Answer(jpt_custom_level_map, user, package, name); }

static std::string Restarted(const fs::path& user, const std::string& package, const char* scene) {
    return Answer([](const char* s, char* o, size_t c) { return jpt_custom_level_restart(s, o, c); }, user, package, scene);
}

static const LevelScene* SceneNamed(const fs::path& user, const std::string& package, const std::string& file) {
    static std::vector<LevelPackage> listed;
    listed = ListLevelPackages(user, g_game);
    for (const LevelPackage& p : listed)
        if (p.id == package)
            for (const LevelScene& s : p.scenes)
                if (SameText(s.file, file)) return &s;
    return nullptr;
}

static bool HasIssue(const LevelScene* scene, const char* code) {
    return scene && std::any_of(scene->issues.begin(), scene->issues.end(), [code](const LevelIssue& i) { return i.code == code; });
}

static bool Runtime(const ImportResult& result, const std::string& line) {
    return std::any_of(result.log.begin(), result.log.end(), [&line](const std::string& l) { return l.find(line) != std::string::npos; });
}

// A fan mission with a script one of its levels needs: imported whole, the script installed and named in the runtime map, read
// back by the game, and a VM only while that level is played.
static void TestScripts() {
    const fs::path user = Fresh("scripts user");
    const Files files = Level("Sc/data/", "alpha") + Level("Sc/data/", "beta") +
                        Files{{"Sc/fm.ini", "[General]\nStartLevel=alpha.scn\n"},
                              {"Sc/scripts/shared.nut", "class AlphaRules extends LevelScript { static SCN = \"alpha.scn\"; function OnLevelStart() { print(\"alpha\"); } }\n"},
                              {"Sc/doc/sample.nut", "class Sample extends GameScript { function OnPaint() {} }\n"}};
    const ImportResult result = Install(user, "Scripted", files);
    CHECK(result.ok() && result.levels == 2 && Runtime(result, "info: SCRIPT_CLASS AlphaRules"));
    const LevelScene* found = SceneNamed(user, result.package, "alpha.scn");
    const LevelScene alpha = found ? *found : LevelScene{};
    found = SceneNamed(user, result.package, "beta.scn");
    const LevelScene beta = found ? *found : LevelScene{};
    CHECK(alpha.scripted && alpha.script_files == std::vector<std::string>{"Sc/scripts/shared.nut"} && alpha.playable() && HasIssue(&alpha, "SCRIPT_CLASS"));
    CHECK(!beta.scripted && beta.playable() && !HasIssue(&beta, "SCRIPT_CLASS"));
    CHECK(Activate(user, result.package, "Sc/data/alpha.scn"));
    CHECK(jpt_custom_level_script_file(0) && std::string(jpt_custom_level_script_file(0)) == "Sc/scripts/shared.nut" && !jpt_custom_level_script_file(1));
    jpt_level_scripts_start(JPT_SESSION_CUSTOM, nullptr);
    jpt_script_session* s = jpt_level_scripts_session();
    CHECK(s != nullptr);
    jpt_level_scripts_begin(JPT_REASON_INITIAL, "alpha.scn", nullptr);
    jpt_level_scripts_ready(1);
    CHECK(jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 1);
    jpt_level_scripts_begin(JPT_REASON_TRANSITION, "beta.scn", nullptr);
    jpt_level_scripts_ready(1);
    CHECK(!jpt_script_session_has_vm(s));
    jpt_level_scripts_begin(JPT_REASON_RESTART, "beta.scn", nullptr);
    CHECK(!jpt_script_session_has_vm(s));
    jpt_level_scripts_begin(JPT_REASON_TRANSITION, "alpha.scn", nullptr);
    CHECK(jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 2);
    jpt_custom_level_deactivate();
    CHECK(jpt_level_scripts_session() == nullptr);
}

static std::vector<std::string> g_script_lines;
static void ScriptLine(const char* line) { g_script_lines.push_back(line); }

static std::string Text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static LevelScene Listed(const fs::path& user, const fs::path& game, const std::string& package, const char* file) {
    for (const LevelPackage& p : ListLevelPackages(user, game))
        for (const LevelScene& scene : p.scenes)
            if (p.id == package && SameText(scene.file, file)) return scene;
    return {};
}

static bool Has(const std::vector<std::string>& lines, const std::string& part) {
    return std::any_of(lines.begin(), lines.end(), [&part](const std::string& l) { return l.find(part) != std::string::npos; });
}

// One package, four levels, each judged on its own: alpha without scripts, beta with a script the port runs, gamma with one that
// asks for CE's API, delta beside a script no level of it uses. A sibling never changes another's state, Play or report.
static void TestLevelsApart() {
    // A game folder of its own: the listing key covers the folder the game's data is in.
    const fs::path user = Fresh("apart user"), game = Fresh("apart game") / "version" / "data";
    fs::create_directories(game);
    const Files files = Level("Four/data/", "alpha") + Level("Four/data/", "beta") + Level("Four/data/", "gamma") + Level("Four/data/", "delta") +
                        Files{{"Four/fm.ini", "[General]\nStartLevel=alpha.scn\n"},
                              {"Four/scripts/beta.nut", "class BetaRules extends LevelScript { static SCN = \"beta.scn\"; function OnLevelStart() { print(\"beta start\"); } }\n"},
                              {"Four/scripts/gamma.nut", "class GammaHud extends LevelScript { static SCN = \"gamma.scn\"; function OnLevelStart() { TP_Engine.ReplayVoiceover(); } }\n"},
                              {"Four/scripts/spare.nut", "class Spare extends LevelScript { static SCN = \"elsewhere.scn\"; function OnLevelStart() {} }\n"}};
    const fs::path source = Fresh("apart source");
    for (const auto& [path, bytes] : files) Write(source / PathFromUtf8(path), bytes);
    const ImportResult result = ImportLevelFolder(source, user, {}, {}, game);
    CHECK(result.ok() && result.levels == 3 && result.limited == 1);
    const std::string id = result.package;
    ListingStats first;
    ListLevelPackages(user, game, &first);
    const LevelScene alpha = Listed(user, game, id, "alpha.scn"), beta = Listed(user, game, id, "beta.scn"), gamma = Listed(user, game, id, "gamma.scn"), delta = Listed(user, game, id, "delta.scn");
    CHECK(alpha.playable() && beta.playable() && delta.playable() && gamma.state == LevelState::kLimited && gamma.compatibility() == JPT_COMPAT_LIMITED);
    CHECK(gamma.level_class() == LevelClass::kRejectedUnsupported && HasIssue(&gamma, "UNSUPPORTED_FEATURE"));
    for (const LevelScene* s : {&alpha, &beta, &delta}) CHECK(s->level_class() == LevelClass::kReady && !HasIssue(s, "PACKAGE_NOT_SUPPORTED") && !HasIssue(s, "UNSUPPORTED_FEATURE"));
    CHECK(!alpha.scripted && !delta.scripted && alpha.script_files.empty() && delta.script_files.empty());
    CHECK(beta.scripted && beta.script_files == std::vector<std::string>{"Four/scripts/beta.nut"} && beta.script_required.empty());
    CHECK(gamma.scripted && std::find(gamma.script_required.begin(), gamma.script_required.end(), "ReplayVoiceover") != gamma.script_required.end());
    for (const LevelScene* s : {&alpha, &delta})
        for (const LevelIssue& i : s->issues) CHECK(i.subject != "CE_SCRIPT_API" && i.subject != "CE_SCRIPT_FILES" && i.subject != "CE_SCRIPTS" && i.code != "SCRIPT_CLASS");
    CHECK(ActionsOf(&alpha, false).play && ActionsOf(&beta, false).play && ActionsOf(&delta, false).play);
    CHECK(!ActionsOf(&gamma, false).play && ActionsOf(&gamma, false).play_anyway && ActionsOf(&gamma, false).report);

    // The listing record keeps each level as it is; a script that changes is read again.
    ListingStats cached;
    ListLevelPackages(user, game, &cached);
    const LevelScene again = Listed(user, game, id, "beta.scn");
    CHECK(cached.cached >= 1 && again.scripted && again.script_files == beta.script_files && Listed(user, game, id, "gamma.scn").script_required == gamma.script_required);

    // The manifest and the runtime map, per level.
    const fs::path dir = CustomLevelsDir(user) / id;
    const std::string manifest = Text(dir / "manifest.json"), map = Text(dir / JPT_CUSTOM_LEVEL_RUNTIME_MAP);
    const size_t status = manifest.find("\"level_status\"");
    CHECK(status != std::string::npos && manifest.find("REJECTED_UNSUPPORTED", status) != std::string::npos && manifest.find("Four/data/alpha.scn", status) != std::string::npos);
    CHECK(manifest.find("ReplayVoiceover", status) != std::string::npos && manifest.find("Four/scripts/beta.nut", status) != std::string::npos);
    CHECK(map.find("\nuses beta.scn|Four/scripts/beta.nut\n") != std::string::npos && map.find("uses alpha.scn") == std::string::npos && map.find("uses delta.scn") == std::string::npos);
    CHECK(map.find("\ncompat Four/data/gamma.scn|limited\n") != std::string::npos && map.find("\ncompat Four/data/alpha.scn|ready\n") != std::string::npos &&
          manifest.find("\"limited\"", status) != std::string::npos && manifest.find("UNSUPPORTED_FEATURE CE_SCRIPT_API", status) != std::string::npos);

    // Play and its preflight: the level that is asked, never the package.
    for (const char* scene : {"Four/data/alpha.scn", "Four/data/beta.scn", "Four/data/delta.scn"}) CHECK(CheckBeforePlay(user, game, id, scene).ok);
    const PlayCheck refused = CheckBeforePlay(user, game, id, "Four/data/gamma.scn");
    CHECK(!refused.ok && refused.needs_consent && refused.reason == ImportStatus::kNotSupported && Has(refused.log, "CE_SCRIPT_API") && !Has(refused.log, "alpha.scn") && !Has(refused.log, "beta.scn"));
    // Play Anyway: gamma alone, without its scripts, which the port cannot run (no VM, no stub).
    const PlayConsent consent{refused.keys};
    const PlayCheck anyway = CheckBeforePlay(user, game, id, "Four/data/gamma.scn", &consent);
    CHECK(anyway.ok && anyway.experimental && anyway.authorized == std::vector<std::string>{"Four/data/gamma.scn"} && Has(anyway.log, "experimental launch authorized"));
    {
        const jpt_custom_level_request_t request = LaunchRequest(id, "Four/data/gamma.scn", anyway);
        char error[256] = "", line[512] = "";
        CHECK(request.experimental == 1 && std::string(request.authorized) == "Four/data/gamma.scn");
        CHECK(jpt_custom_level_activate(Utf8(user).c_str(), &request, error, sizeof error) && jpt_custom_level_admit_start(line, sizeof line) && jpt_custom_level_experimental());
        g_script_lines.clear();
        jpt_level_scripts_start(JPT_SESSION_CUSTOM, ScriptLine);
        jpt_script_session* s = jpt_level_scripts_session();
        jpt_level_scripts_begin(JPT_REASON_INITIAL, "gamma.scn", nullptr);
        jpt_level_scripts_ready(1);
        CHECK(s && !jpt_script_session_has_vm(s) && Has(g_script_lines, "result=refused"));
        jpt_custom_level_deactivate();
    }

    // Bug Report of gamma: its diagnosis, not a sibling's.
    LevelPackage listed;
    for (const LevelPackage& p : ListLevelPackages(user, game))
        if (p.id == id) listed = p;
    const auto at = std::find_if(listed.scenes.begin(), listed.scenes.end(), [](const LevelScene& s) { return s.file == "gamma.scn"; });
    CHECK(at != listed.scenes.end());
    if (at != listed.scenes.end()) {
        const std::vector<std::string> report = ReportLines(listed, &*at, game);
        CHECK(!report.empty() && Has(report, "level Four/data/gamma.scn: REJECTED_UNSUPPORTED") && Has(report, "CE_SCRIPT_API") && !Has(report, "alpha.scn") && !Has(report, "beta.scn") && !Has(report, "delta.scn"));
        const std::vector<std::string> all = ReportLines(listed, nullptr, game);
        CHECK(Has(all, "gamma.scn") && !Has(all, "level Four/data/alpha.scn"));
    }

    // At run time: a VM only for beta, nothing at all for alpha and delta.
    for (const char* scene : {"alpha.scn", "beta.scn", "delta.scn"}) {
        CHECK(Activate(user, id, std::string("Four/data/") + scene));
        g_script_lines.clear();
        jpt_level_scripts_start(JPT_SESSION_CUSTOM, ScriptLine);
        jpt_script_session* s = jpt_level_scripts_session();
        jpt_level_scripts_begin(JPT_REASON_INITIAL, scene, nullptr);
        jpt_level_scripts_ready(1);
        jpt_level_scripts_step(0.1f);
        const bool vm = s && jpt_script_session_has_vm(s);
        if (!strcmp(scene, "beta.scn")) CHECK(vm && Has(g_script_lines, "[script] print: beta start") && Has(g_script_lines, "vm create level=beta.scn"));
        else CHECK(s && !vm && g_script_lines.empty());
        jpt_custom_level_deactivate();
    }

    // gamma's script changes into one the port runs: the listing reads the package again and gamma alone changes.
    Write(dir / "Four" / "scripts" / "gamma.nut", "class GammaHud extends LevelScript { static SCN = \"gamma.scn\"; function OnLevelStart() { print(\"g\"); } }\n");
    ListingStats changed;
    ListLevelPackages(user, game, &changed);
    CHECK(changed.analysed >= 1 && Listed(user, game, id, "gamma.scn").playable() && Listed(user, game, id, "alpha.scn").playable());
}

// A custom session's scripts end with it: the campaign started after has no console command, player factor or jump of theirs.
static void TestCampaignAfterCustom() {
    const fs::path user = Fresh("campaign after custom");
    const Files files = Level("Cc/data/", "solo") +
                        Files{{"Cc/scripts/console.nut",
                               "class Console extends GameScript { function OnConsoleCommand(c, a) { print(\"console \" + c + \" \" + a); return c == \"ping\"; } }\n"}};
    const ImportResult result = Install(user, "Consoled", files);
    CHECK(result.ok());
    CHECK(Activate(user, result.package, "Cc/data/solo.scn"));
    g_script_lines.clear();
    jpt_level_scripts_start(JPT_SESSION_CUSTOM, ScriptLine);
    jpt_level_scripts_begin(JPT_REASON_INITIAL, "solo.scn", nullptr);
    jpt_level_scripts_ready(1);
    CHECK(jpt_level_scripts_console("ping", nullptr) == 1 && Has(g_script_lines, "[script] print: console ping "));
    CHECK(jpt_level_scripts_console("pong", "x") == 0 && Has(g_script_lines, "[script] print: console pong x"));
    jpt_custom_level_deactivate();
    CHECK(jpt_level_scripts_session() == nullptr);
    jpt_level_scripts_start(JPT_SESSION_CAMPAIGN, ScriptLine);
    g_script_lines.clear();
    CHECK(jpt_level_scripts_session() == nullptr && jpt_level_scripts_console("ping", nullptr) == 0);
    CHECK(jpt_level_scripts_speed_factor(1u, 0.5f) == 0.5f && jpt_level_scripts_jump_factor(1u) == 1.0f && !jpt_level_scripts_drives_player());
    jpt_level_scripts_jump(1u);
    CHECK(g_script_lines.empty());
}

static void TestBitmaps() {
    struct {
        int bits;
        bool os2;
    } layouts[] = {{24, false}, {8, false}, {4, false}, {16, false}, {8, true}, {24, true}};
    for (const auto& layout : layouts) {
        const std::string bytes = Bmp(5, 3, layout.bits, layout.os2);
        jpt_picture_t picture{};
        CHECK(jpt_picture_decode(bytes.data(), bytes.size(), &picture) && picture.width == 5 && picture.height == 3);
        for (int y = 0; picture.bgra && y < 3; ++y)
            for (int x = 0; x < 5; ++x) {
                const uint8_t* p = picture.bgra + (size_t(y) * 5 + size_t(x)) * 4;
                const uint32_t want = layout.bits <= 8 ? Colour((x + y) % 4, (x + y) % 4) : Colour(x, y);
                const int tolerance = layout.bits == 16 ? 8 : 0;
                CHECK(std::abs(int(p[2]) - int(want >> 16 & 0xFF)) <= tolerance && std::abs(int(p[1]) - int(want >> 8 & 0xFF)) <= tolerance &&
                      std::abs(int(p[0]) - int(want & 0xFF)) <= tolerance && p[3] == 255);
            }
        jpt_picture_free(&picture);
    }
    std::string cut = Bmp(5, 3, 24);
    cut.resize(cut.size() - 4);
    jpt_picture_t none{};
    CHECK(!jpt_picture_decode(cut.data(), cut.size(), &none));
}

static void TestMapPaths() {
    char out[JPT_CUSTOM_LEVEL_PATH_MAX];
    for (const char* name : {"chart.bmp", "\\chart.bmp", "/chart.bmp", "\\\\chart.bmp"}) CHECK(jpt_custom_level_map_path(name, out, sizeof out) && !std::strcmp(out, "maps/chart.bmp"));
    CHECK(jpt_custom_level_map_path("Sub\\Chart.BMP", out, sizeof out) && !std::strcmp(out, "maps/Sub/Chart.BMP"));
    CHECK(jpt_custom_level_map_path("sub//chart.bmp", out, sizeof out) && !std::strcmp(out, "maps/sub/chart.bmp"));
    for (const char* name : {"..\\chart.bmp", "sub\\..\\..\\chart.bmp", "C:\\chart.bmp", "./chart.bmp", "", "\\"}) CHECK(!jpt_custom_level_map_path(name, out, sizeof out));
    CHECK(!jpt_custom_level_map_path("chart.bmp", out, 8));
}

// The picture a map action names is the package's own below MAPS, whatever the README says, and only while the session lasts.
static void TestMapPictures() {
    const fs::path user = Fresh("maps");
    const std::string chart = Bmp(6, 4, 24);
    const ImportResult placed = Install(user, "Strata", Level("", "basin", 22, Showing({"Strata_Chart.bmp"})) + Files{{"Strata_Chart.bmp", chart}});
    CHECK(placed.ok() && Runtime(placed, "maps/Strata_Chart.bmp <- Strata_Chart.bmp"));
    const LevelScene* basin = SceneNamed(user, placed.package, "basin.scn");
    CHECK(basin && basin->playable() && !HasIssue(basin, "MAP_PICTURE_ABSENT"));
    CHECK(Activate(user, placed.package, "basin.scn"));
    for (const char* name : {"Strata_Chart.bmp", "strata_chart.BMP", "\\STRATA_CHART.bmp", "/strata_chart.bmp"}) CHECK(Mapped(user, placed.package, name) == "Strata_Chart.bmp");
    CHECK(Mapped(user, placed.package, "..\\Strata_Chart.bmp").empty() && Mapped(user, placed.package, "basin.scn").empty() && Mapped(user, placed.package, "other.bmp").empty());
    char file[1024];
    jpt_picture_t picture{};
    CHECK(jpt_custom_level_map("strata_chart.bmp", file, sizeof file) && jpt_picture_load(file, &picture) && picture.width == 6 && picture.height == 4);
    jpt_picture_free(&picture);
    CHECK(Found(user, placed.package, "Strata_Chart.bmp").empty() || Found(user, placed.package, "Strata_Chart.bmp") == "Strata_Chart.bmp");
    jpt_custom_level_deactivate();
    CHECK(!jpt_custom_level_map("Strata_Chart.bmp", file, sizeof file));

    // Below the package's own MAPS folder, in a subfolder the action names.
    const ImportResult folder = Install(user, "Folders", Level("data/", "ridge", 22, Showing({"Atlas\\Ridge.bmp"})) + Files{{"MAPS/Atlas/Ridge.bmp", Bmp(2, 2, 8)}});
    CHECK(folder.ok());
    CHECK(Activate(user, folder.package, "data/ridge.scn") && Mapped(user, folder.package, "atlas/ridge.BMP") == "MAPS/Atlas/Ridge.bmp");
    jpt_custom_level_deactivate();

    // A picture the action names and the package does not have leaves the map as it was: the level stays playable.
    const ImportResult absent = Install(user, "Bare", Level("", "flat", 22, Showing({"Nowhere.bmp"})));
    const LevelScene* flat = SceneNamed(user, absent.package, "flat.scn");
    CHECK(absent.ok() && flat && flat->playable() && HasIssue(flat, "MAP_PICTURE_ABSENT"));
    const ImportResult plain = Install(user, "Plain", Level("", "meadow"));
    CHECK(plain.ok() && !Runtime(plain, "maps/"));

    // A name that leaves MAPS is never read, and a file that is no picture is not put there.
    const ImportResult escaping = Install(user, "Escape", Level("", "cliff", 22, Showing({"..\\..\\cliff.scn", "C:\\x.bmp"})) + Files{{"x.bmp", chart}});
    const LevelScene* cliff = SceneNamed(user, escaping.package, "cliff.scn");
    CHECK(escaping.ok() && cliff && cliff->playable() && HasIssue(cliff, "UNSAFE_MAP_PICTURE") && !Runtime(escaping, "maps/"));
    const ImportResult broken = Install(user, "Broken", Level("", "dune", 22, Showing({"Dune.bmp"})) + Files{{"Dune.bmp", "BM not a picture"}});
    const LevelScene* dune = SceneNamed(user, broken.package, "dune.scn");
    CHECK(broken.ok() && dune && dune->playable() && HasIssue(dune, "MAP_PICTURE_UNREADABLE") && !Runtime(broken, "maps/"));

    // The pattern of a loose package whose README asks for the picture in MAPS only if ATX is there: optional, and used.
    const ImportResult optional = Install(user, "Loose", Level("", "grove", 22, Showing({"GROVE_CHART.bmp"})) +
                                                             Files{{"GROVE_CHART.bmp", chart},
                                                                   {"readme.txt", "Place the level files into your \"\\data\" directory\n\n(Optional) IF ATX IS INSTALLED: Place \"GROVE_CHART.bmp\" in the \"\\MAPS\" directory\n"}});
    const LevelScene* grove = SceneNamed(user, optional.package, "grove.scn");
    CHECK(optional.ok() && grove && grove->playable() && Runtime(optional, "maps/GROVE_CHART.bmp <- GROVE_CHART.bmp"));
    CHECK(Activate(user, optional.package, "grove.scn") && Mapped(user, optional.package, "grove_chart.bmp") == "GROVE_CHART.bmp");
    jpt_custom_level_deactivate();
}

// Restart Level reads the scene the game names again: the package's own scene and files, never the game's of the same name.
static void TestRestart() {
    const fs::path user = Fresh("restart");
    for (const char* own : {"vale.scn", "vale.grf", "vale-130.pid", "vale-130.swp", "vale.wtd", "keep.scn", "keep-130.spz"}) Write(g_game / own, std::string("the game's ") + own);

    // loose, scene 22, its own texture set, a picture for the map, a sound file added and a hardware texture
    const Files loose = Level("", "vale", 22, Showing({"Vale_Map.bmp"})) +
                        Files{{"Vale_Map.bmp", Bmp(3, 3, 24)}, {"fm.ini", "[General]\nStartLevel=vale.scn\nStreamTPA+=Extra.tpa\n"}, {"Extra.tpa", Tpa()},
                              {"texhw/Map/vale/crate.dds", "DDS "}};
    const ImportResult first = Install(user, "Loose Vale", loose);
    CHECK(first.ok());
    CHECK(Activate(user, first.package, "vale.scn"));
    const unsigned caps = jpt_custom_level_caps();
    for (int round = 0; round < 2; ++round) {
        CHECK(Restarted(user, first.package, "vale.scn") == "vale.scn" && Restarted(user, first.package, "C:\\Game\\data\\VALE.SCN") == "vale.scn");
        for (const char* file : {"vale.grf", "vale-130.pid", "vale-130.swp", "vale.wtd"}) CHECK(Found(user, first.package, file) == file);
        CHECK(Mapped(user, first.package, "vale_map.bmp") == "Vale_Map.bmp");
        char out[1024];
        CHECK(jpt_custom_level_tpa(JPT_LEVEL_TPA_STREAM, 0, out, sizeof out) && Utf8(fs::path(PathFromUtf8(out)).filename()) == "Extra.tpa");
        CHECK(jpt_custom_level_texhw("Map\\vale\\crate.bmp", "", out, sizeof out));
        CHECK(jpt_custom_level_caps() == caps && jpt_custom_level_active());
    }
    jpt_custom_level_deactivate();

    // fan mission, scenes 22 and 26, a transition to the other level of the package, then Restart of that one
    const Files mission = Level("Mission/data/", "keep", 26, Showing({}, {"tower.scn"})) + Level("Mission/data/", "tower") +
                          Files{{"Mission/fm.ini", "[General]\nStartLevel=keep.scn\n"}};
    const ImportResult fan = Install(user, "Fan Mission", mission);
    CHECK(fan.ok());
    CHECK(Activate(user, fan.package, "Mission/data/keep.scn"));
    char host[1024];
    CHECK(Restarted(user, fan.package, "keep.scn") == "Mission/data/keep.scn");
    const std::string keep = Fixture(mission, "Mission/data/keep.scn");
    size_t size = 0;
    jpt_scene_report_t report;
    CHECK(jpt_custom_level_find("keep.scn", host, sizeof host));
    uint8_t* adapted = jpt_custom_level_adapt_scene(host, reinterpret_cast<const uint8_t*>(keep.data()), keep.size(), &size, &report);
    CHECK(jpt_custom_level_scene_version() == 26);
    std::free(adapted);
    jpt_custom_level_scene_loading("tower.scn");
    CHECK(Found(user, fan.package, "tower.grf") == "Mission/data/tower.grf");
    CHECK(Restarted(user, fan.package, "tower.scn") == "Mission/data/tower.scn" && jpt_custom_level_scene_version() == 0);
    CHECK(Found(user, fan.package, "tower-130.pid") == "Mission/data/tower-130.pid");
    CHECK(Restarted(user, fan.package, "keep.scn") == "Mission/data/keep.scn" && Found(user, fan.package, "keep.grf") == "Mission/data/keep.grf");
    jpt_custom_level_deactivate();

    // a texture set of its own over the game's swap file of that name
    std::string set = TextureSet({{TextureHash("Map\\keep\\crate.bmp")}});
    jpt_texture_set_t* opened = jpt_texture_set_open(reinterpret_cast<const uint8_t*>(set.data()), set.size());
    const uint64_t bytes = jpt_texture_set_bytes(opened);
    jpt_texture_set_close(opened);
    Write(g_game / "keep-130.spz", U32(uint32_t(bytes)) + "packed");
    Files borrowed;
    for (const auto& file : Level("", "keep"))
        if (file.first != "keep-130.swp") borrowed.push_back(file);
    const ImportResult retail = Install(user, "Borrowed", borrowed);
    CHECK(retail.ok() && Activate(user, retail.package, "keep.scn"));
    CHECK(Restarted(user, retail.package, "keep.scn") == "keep.scn" && Found(user, retail.package, "keep-130.pid") == "keep-130.pid");
    CHECK(Found(user, retail.package, "keep-130.spz").empty() && Found(user, retail.package, "keep-130.swp").empty());
    // a level of the game the package leads to: its own files, in the same session
    CHECK(Restarted(user, retail.package, "be.scn").empty() && jpt_custom_level_active());
    jpt_custom_level_deactivate();

    // the session is over: Restart reads the game's own level, with nothing of the package
    char out[1024];
    CHECK(!jpt_custom_level_restart("vale.scn", out, sizeof out) && !jpt_custom_level_find("vale.scn", out, sizeof out) && !jpt_custom_level_map("vale_map.bmp", out, sizeof out));
    CHECK(!jpt_custom_level_tpa(JPT_LEVEL_TPA_STREAM, 0, out, sizeof out) && !jpt_custom_level_texhw("Map\\vale\\crate.bmp", "", out, sizeof out));
    CHECK(!jpt_custom_level_caps() && !jpt_custom_level_active() && !jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) && !jpt_custom_level_requires(JPT_LEVEL_CAP_SOUND_ADDED));
}

// The engine's side of a level, over the session's own file lookup. CWorld::bLoadScene purges the world only when no world file
// the scene names is loaded, then opens that world, its texture set, swap file and terrain and makes the player anew; otherwise it
// reads the scene over the world it has. The host's part is what level_actions.c and loading_hd.c do around it.
class Engine {
public:
    Engine(fs::path user, std::string package) : user_(std::move(user)), package_(std::move(package)) {}

    unsigned purges = 0, player = 0, scene_reads = 0;
    bool disturbed = false;
    std::string world, app_scene;
    std::vector<std::string> opened; // of the last world built: "package:<path>" or "game:<name>"

    bool Load(const std::string& scene, bool save = false, bool main_menu = false) {
        jpt_custom_level_scene_loading(scene.c_str());
        jpt_session_level_begin(jpt_session_reason(save, main_menu), scene.c_str(), Source(scene));
        if (!save) app_scene = std::string(jpt_session_scene_name(scene.c_str()));
        if (jpt_session_restart_purges()) Purge(), jpt_session_world_purged();
        const bool ok = Read(scene);
        jpt_session_level_end(ok, ok ? 0 : -1);
        return ok;
    }

    bool Restart() {
        char own[1024];
        bool ok;
        jpt_custom_level_restart(app_scene.c_str(), own, sizeof own);
        if (jpt_session_restart_begin(app_scene.c_str(), Source(app_scene)) == JPT_RESTART_FULL) {
            ok = Load(app_scene);
        } else {
            jpt_session_level_begin(JPT_REASON_RESTART, app_scene.c_str(), Source(app_scene));
            ok = Read(app_scene);
            jpt_session_level_end(ok, 0);
        }
        jpt_session_restart_end(ok);
        return ok;
    }

    jpt_level_source_t Source(const std::string& scene) const {
        char out[1024];
        return jpt_custom_level_find(jpt_session_scene_name(scene.c_str()), out, sizeof out) ? JPT_SOURCE_PACKAGE : JPT_SOURCE_RETAIL;
    }

private:
    void Purge() {
        world.clear();
        player = 0;
        ++purges;
    }

    std::string Where(const std::string& name) const {
        char out[1024];
        if (jpt_custom_level_find(name.c_str(), out, sizeof out)) {
            std::string relative = Utf8(fs::path(PathFromUtf8(out)).lexically_relative(CustomLevelsDir(user_) / package_));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            return "package:" + relative;
        }
        return GameFile(name.c_str(), out, sizeof out) ? "game:" + name : "";
    }

    bool Read(const std::string& scene) {
        char file[1024];
        const std::string name = jpt_session_scene_name(scene.c_str());
        if (!jpt_custom_level_find(name.c_str(), file, sizeof file) && !GameFile(name.c_str(), file, sizeof file) &&
            std::snprintf(file, sizeof file, "%s", scene.c_str()) <= 0)
            return false;
        std::ifstream in(PathFromUtf8(file), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t at = bytes.find("\\data\\");
        if (at == std::string::npos) return false;
        const std::string grf = bytes.substr(at + 6, bytes.find('\0', at) - at - 6), stem = grf.substr(0, grf.find('.'));
        ++scene_reads;
        if (world != grf) {
            if (!world.empty()) Purge();
            world = grf;
            opened = {Where(name), Where(grf), Where(stem + "-130.pid"), Where(stem + "-130.swp"), Where(stem + "-130.spz"), Where(stem + ".wtd")};
            player = ++players_;
            disturbed = false;
        }
        return true;
    }

    fs::path user_;
    std::string package_;
    unsigned players_ = 0;
};

static std::vector<std::string> g_session_lines;
static void SessionLine(const char* tag, const char* text) { g_session_lines.push_back(std::string("[") + tag + "] " + text); }
static bool Logged(const std::string& text) {
    return std::any_of(g_session_lines.begin(), g_session_lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

static void BeginSession(const char* start, const char* layout) {
    std::string caps;
    for (int c = 0; c < JPT_LEVEL_CAPS; ++c)
        if (jpt_custom_level_requires(jpt_level_cap_t(c))) caps += (caps.empty() ? "" : ",") + std::string(jpt_custom_level_cap_name(jpt_level_cap_t(c)));
    jpt_session_info_t info{};
    info.kind = jpt_custom_level_active() ? JPT_SESSION_CUSTOM : JPT_SESSION_CAMPAIGN;
    info.build = "en-116";
    info.renderer = "d3d11";
    info.package = jpt_custom_level_package();
    info.layout = layout;
    info.start_level = start;
    info.capabilities = caps.c_str();
    jpt_session_set_log(SessionLine);
    g_session_lines.clear();
    jpt_session_begin(&info);
}

// One restart, checked as a whole level cycle: the world purged and built again from the same files, the player made anew, the session
// and its capabilities as they were, the level the one the game names.
static void CheckRestart(Engine& engine, const std::string& scene) {
    const unsigned purges = engine.purges, player = engine.player, generation = jpt_session_world_generation(), caps = jpt_custom_level_caps();
    const std::vector<std::string> opened = engine.opened;
    const std::string package = jpt_custom_level_package();
    engine.disturbed = true;
    g_session_lines.clear();
    CHECK(engine.Restart());
    CHECK(engine.purges == purges + 1 && engine.player && engine.player != player && !engine.disturbed);
    CHECK(engine.opened == opened && engine.app_scene == scene && jpt_session_current_scene() == scene);
    CHECK(jpt_session_world_generation() == generation + 1 && jpt_session_kind() == JPT_SESSION_CUSTOM);
    CHECK(jpt_custom_level_active() && jpt_custom_level_caps() == caps && package == jpt_custom_level_package());
    CHECK(Logged("[restart] requested scene=" + scene + " session=custom mode=full") && Logged("[restart] result=ready"));
    CHECK(Logged("[level] begin reason=restart scene=" + scene));
}

// Restart Level of every custom session is a full level cycle, whatever the package's layout, scene format and files.
static void TestRestartLifecycle() {
    const fs::path user = Fresh("lifecycle");
    Write(g_game / "sum.scn", Scene("sum.grf"));
    for (const char* own : {"sum.grf", "sum-130.pid", "sum-130.swp", "sum.wtd", "keep-130.spz"}) Write(g_game / own, std::string("the game's ") + own);

    // loose, scene 22, its own texture set, a map picture, an added sound file and a hardware texture; Restart twice
    const Files loose = Level("", "vale", 22, Showing({"Vale_Map.bmp"})) +
                        Files{{"Vale_Map.bmp", Bmp(3, 3, 24)}, {"fm.ini", "[General]\nStartLevel=vale.scn\nStreamTPA+=Extra.tpa\n"}, {"Extra.tpa", Tpa()},
                              {"texhw/Map/vale/crate.dds", "DDS "}};
    const ImportResult first = Install(user, "Loose Vale", loose);
    CHECK(first.ok() && Activate(user, first.package, "vale.scn"));
    BeginSession("vale.scn", "loose");
    CHECK(Logged("[session] type=custom") && Logged("[session] package=" + first.package + " layout=loose") && Logged("[session] start_level=vale.scn"));
    Engine vale(user, first.package);
    CHECK(vale.Load("vale.scn", false, true) && Logged("[level] begin reason=initial scene=vale.scn source=package session=custom"));
    CHECK(vale.opened.size() == 6 && vale.opened[0] == "package:vale.scn" && vale.opened[1] == "package:vale.grf" && vale.opened[2] == "package:vale-130.pid");
    CHECK(vale.opened[3] == "package:vale-130.swp" && vale.opened[5] == "package:vale.wtd");
    CheckRestart(vale, "vale.scn");
    CheckRestart(vale, "vale.scn");
    char out[1024];
    CHECK(Mapped(user, first.package, "vale_map.bmp") == "Vale_Map.bmp" && jpt_custom_level_tpa(JPT_LEVEL_TPA_STREAM, 0, out, sizeof out));
    CHECK(jpt_custom_level_texhw("Map\\vale\\crate.bmp", "", out, sizeof out));

    // A save made before the restart still loads as a save over its own level.
    const fs::path save = user / "SAVEGAME.001";
    Write(save, Save("vale.grf", "vale.scn"));
    g_session_lines.clear();
    CHECK(vale.Load(Utf8(save), true) && Logged("[level] begin reason=load_save") && vale.app_scene == "vale.scn");
    CheckRestart(vale, "vale.scn");

    // custom -> retail: the game's level the package leads to, restarted whole in the same custom session
    g_session_lines.clear();
    CHECK(vale.Load("sum.scn") && Logged("[level] begin reason=transition scene=sum.scn source=retail session=custom"));
    CHECK(vale.opened[0] == "game:sum.scn" && vale.opened[1] == "game:sum.grf");
    CheckRestart(vale, "sum.scn");
    CHECK(Logged("mode=full source=retail"));
    jpt_session_end();
    jpt_custom_level_deactivate();

    // fan mission, scene 26 then 22: custom -> custom, and Restart after it restarts the second level
    const Files mission = Level("Mission/data/", "keep", 26, Showing({}, {"tower.scn"})) + Level("Mission/data/", "tower") +
                          Files{{"Mission/fm.ini", "[General]\nStartLevel=keep.scn\n"}};
    const ImportResult fan = Install(user, "Fan Mission", mission);
    CHECK(fan.ok() && Activate(user, fan.package, "Mission/data/keep.scn"));
    BeginSession("Mission/data/keep.scn", "fan_mission");
    CHECK(Logged("[session] start_level=keep.scn") && Logged("layout=fan_mission"));
    Engine keep(user, fan.package);
    CHECK(keep.Load("keep.scn", false, true) && keep.opened[1] == "package:Mission/data/keep.grf");
    CheckRestart(keep, "keep.scn");
    CHECK(keep.Load("tower.scn") && keep.opened[1] == "package:Mission/data/tower.grf");
    CheckRestart(keep, "tower.scn");
    CHECK(keep.opened[2] == "package:Mission/data/tower-130.pid");
    jpt_session_end();
    jpt_custom_level_deactivate();

    // scene 25, its own texture set over a swap file the game has (SPZ)
    std::string set = TextureSet({{TextureHash("Map\\keep\\crate.bmp")}});
    jpt_texture_set_t* opened = jpt_texture_set_open(reinterpret_cast<const uint8_t*>(set.data()), set.size());
    const uint64_t bytes = jpt_texture_set_bytes(opened);
    jpt_texture_set_close(opened);
    Write(g_game / "keep-130.spz", U32(uint32_t(bytes)) + "packed");
    Files borrowed;
    for (const auto& file : Level("", "keep", 25))
        if (file.first != "keep-130.swp") borrowed.push_back(file);
    const ImportResult retail = Install(user, "Borrowed", borrowed);
    CHECK(retail.ok() && Activate(user, retail.package, "keep.scn"));
    BeginSession("keep.scn", "loose");
    Engine spz(user, retail.package);
    CHECK(spz.Load("keep.scn", false, true) && spz.opened[2] == "package:keep-130.pid" && spz.opened[4] == "game:keep-130.spz");
    CheckRestart(spz, "keep.scn");
    jpt_session_end();
    jpt_custom_level_deactivate();

    // The campaign afterwards: the game's own Restart Level, the world kept.
    BeginSession("", "");
    CHECK(Logged("[session] type=campaign") && jpt_session_restart_mode() == JPT_RESTART_SCENE_RELOAD);
    Engine campaign(user, "");
    CHECK(campaign.Load("sum.scn", false, true) && Logged("[level] begin reason=new_game scene=sum.scn source=retail session=campaign"));
    const unsigned player = campaign.player, purges = campaign.purges;
    g_session_lines.clear();
    CHECK(campaign.Restart() && campaign.player == player && campaign.purges == purges);
    CHECK(Logged("[restart] requested scene=sum.scn session=campaign mode=scene_reload source=retail") && Logged("[restart] result=ready"));
    jpt_session_end();
    jpt_session_set_log(nullptr);
}

// The host around src/host/custom_level_guide.c: no game text is loaded, so objectives take their fallback titles.
extern "C" void host_log(const char*, const char*, ...) {}
extern "C" int guest_load_string(unsigned, char*, int) { return 0; }
extern "C" const char* l10n_text_utf8(unsigned) { return nullptr; }

// Objects spread over 180 x 100 m, two hint triggers and a level exit.
static WorldShape Guided() {
    WorldShape shape;
    for (int i = 0; i < 12; ++i) shape.placed.push_back({"Tree-" + std::to_string(10 + i), "CInstance", float(i % 4) * 60.0f, float(i / 4) * 50.0f, 0.0f});
    shape.hints = {{"TrigStart-00", 500}, {"TrigNext-00", 501}};
    shape.levels = {"sum.scn"};
    return shape;
}

static std::string GuideOf(const fs::path& user, const std::string& package, const std::string& scene) {
    std::ifstream in(CustomLevelsDir(user) / package / ".runtime" / "guide" / (scene + ".txt"), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Every custom level gets its own minimap and objectives from its world; the campaign's only where the world is the game's own.
static void TestLevelGuides() {
    const fs::path user = Fresh("guides");
    Write(g_game / "sum.scn", Scene("sum.grf"));
    WorldShape game_as = Guided();
    game_as.terrain.clear();
    Write(g_game / "as.grf", MakeWorld(game_as));

    // A level of its own: the guide is written before it starts, then read by the session.
    const ImportResult glen = Install(user, "Glen", Level("", "glen", 22, Guided()));
    CHECK(glen.ok());
    const PlayCheck play = CheckBeforePlay(user, g_game, glen.package, "glen.scn");
    CHECK(play.ok);
    std::string text = GuideOf(user, glen.package, "glen.scn");
    // A guide gone missing is written again before the level starts.
    fs::remove(CustomLevelsDir(user) / glen.package / ".runtime" / "guide" / "glen.scn.txt");
    const PlayCheck again = CheckBeforePlay(user, g_game, glen.package, "glen.scn");
    CHECK(again.ok && GuideOf(user, glen.package, "glen.scn") == text);
    CHECK(Has(text, "level-guide 1\n") && Has(text, "world package glen.grf\n") && Has(text, "\nmap ") && Has(text, "\nrun "));
    CHECK(Has(text, "objective 500 TrigNext-00\n") && Has(text, "objective 501 Leave-00\n"));
    level_guide_t parsed;
    CHECK(level_guide_parse(text.c_str(), &parsed) && parsed.has_map && parsed.map.count > 0 && parsed.objective_count == 2 && !parsed.in_game);
    CHECK(parsed.bounds.min_x < 0.0f && parsed.bounds.max_x > 180.0f && parsed.bounds.max_y > 100.0f && parsed.hints[1] == 501 && !std::strcmp(parsed.targets[1][0], "Leave-00"));
    level_guide_free(&parsed);
    CHECK(!level_guide_parse("level-guide 0\n", &parsed) && !level_guide_parse("map 0 0 1 1 4 4\n", &parsed) && !level_guide_parse("level-guide 1\nmap 0 0 1 1 4 4\nrun 3 9 0 1\n", &parsed));

    CHECK(Activate(user, glen.package, "glen.scn"));
    level_guide_install();
    const minimap_bounds_t* bounds = minimap_bounds("GLEN");
    const minimap_level_map_t* map = minimap_map("GLEN");
    const ob_level_t* goals = nullptr;
    CHECK(bounds && map && map->count > 0 && !minimap_retail_bounds("GLEN"));
    CHECK(level_guide_objectives("GLEN", &goals) && goals && goals->count == 2 && goals->items[0].hint == 500 && !std::strcmp(goals->items[0].target[0], "TrigNext-00"));
    CHECK(!std::strcmp(goals->items[0].title, "Objective 1") && !std::strcmp(goals->items[1].title, "Objective 2") && !goals->items[0].route[0]);
    // The game's own level the package leads to: the campaign's minimap and objectives.
    CHECK(minimap_bounds("SUM") == minimap_retail_bounds("SUM") && !level_guide_objectives("SUM", &goals));
    jpt_custom_level_deactivate();

    // A level named as one of the campaign's, with a world of its own: its guide, never the campaign's.
    const ImportResult own = Install(user, "Own Ascent", Level("", "as2", 22, Guided()));
    CHECK(own.ok() && CheckBeforePlay(user, g_game, own.package, "as2.scn").ok && Activate(user, own.package, "as2.scn"));
    bounds = minimap_bounds("AS2");
    CHECK(bounds && minimap_retail_bounds("AS2") && bounds != minimap_retail_bounds("AS2") && bounds->max_x < 300.0f);
    CHECK(level_guide_objectives("AS2", &goals) && goals && !std::strcmp(goals->items[0].target[0], "TrigNext-00"));
    jpt_custom_level_deactivate();

    // A scene of the package over the game's own world of that level: the campaign's guide holds.
    const ImportResult over = Install(user, "Over Ascent", Files{{"as.scn", Scene("as.grf")}});
    CHECK(over.ok() && CheckBeforePlay(user, g_game, over.package, "as.scn").ok && Has(GuideOf(user, over.package, "as.scn"), "world game as.grf\n"));
    CHECK(Activate(user, over.package, "as.scn") && minimap_bounds("AS") == minimap_retail_bounds("AS") && !level_guide_objectives("AS", &goals));
    jpt_custom_level_deactivate();

    // No guide, or a world that gives no map and no objective: "no information", not the campaign's.
    fs::remove(CustomLevelsDir(user) / glen.package / ".runtime" / "guide" / "glen.scn.txt");
    CHECK(Activate(user, glen.package, "glen.scn") && !minimap_bounds("GLEN") && !minimap_map("GLEN") && level_guide_objectives("GLEN", &goals) && !goals);
    jpt_custom_level_deactivate();
    const ImportResult bare = Install(user, "Bare", Level("", "void"));
    CHECK(bare.ok() && CheckBeforePlay(user, g_game, bare.package, "void.scn").ok);
    const std::string empty = GuideOf(user, bare.package, "void.scn");
    CHECK(Has(empty, "\nnomap ") && !Has(empty, "\nobjective "));
    CHECK(Activate(user, bare.package, "void.scn") && !minimap_bounds("VOID"));
    jpt_custom_level_deactivate();
    CHECK(Activate(user, bare.package, "void.scn") && level_guide_objectives("VOID", &goals) && !goals);
    jpt_custom_level_deactivate();

    // The campaign never reads a guide.
    CHECK(minimap_bounds("AS2") == minimap_retail_bounds("AS2") && !level_guide_objectives("AS2", &goals) && !level_guide_minimap("AS2", &bounds, &map));

    char title[32];
    level_guide_title("  The cliffs look too steep. I need another way.", title, sizeof title);
    CHECK(!std::strcmp(title, "The cliffs look too steep."));
    level_guide_title("Wo bin ich?", title, sizeof title);
    CHECK(!std::strcmp(title, "Wo bin ich?"));
    level_guide_title("A very long thought without any stop at all in it", title, 12);
    CHECK(!std::strcmp(title, "A very long"));
    level_guide_title(nullptr, title, sizeof title);
    CHECK(!title[0]);
}

int main() {
    g_scratch = fs::temp_directory_path() / "jpt_custom_level_session_tests";
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    g_game = Fresh("game");
    TestScripts();
    TestLevelsApart();
    TestCampaignAfterCustom();
    TestBitmaps();
    TestMapPaths();
    TestMapPictures();
    TestRestart();
    TestRestartLifecycle();
    TestLevelGuides();
    fs::remove_all(g_scratch, ec);
    std::printf("custom_level_session_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
