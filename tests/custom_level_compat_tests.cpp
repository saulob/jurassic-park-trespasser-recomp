// READY, LIMITED COMPATIBILITY and INVALID / UNSAFE: the one classification, imports of every kind, Play Anyway and its consent,
// what the game admits at its start, at a transition and at a load, the compatibility report, and the records of v0.1.3.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "jpt_level_compat.h"
#include "jpt_level_features.h"
#include "jpt_level_gate.h"
#include "level_fixtures.h"
#include "level_listing.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;
using namespace fixtures;
using Files = std::vector<std::pair<std::string, std::string>>;

void WriteZip(const fs::path& file, const Files& files, unsigned attributes);
std::string SevenZip(const Files& files, uint32_t attributes);
std::string Rar(const Files& files);

namespace {

int g_checks = 0, g_failures = 0;
fs::path g_scratch;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// Everything this suite writes or removes is below the system's temporary folder, in the suite's own scratch folder.
bool InScratch(const fs::path& path) {
    std::error_code ec;
    const fs::path temp = fs::weakly_canonical(fs::temp_directory_path(), ec), full = fs::weakly_canonical(path, ec);
    const std::string t = Utf8(temp), f = Utf8(full), s = Utf8(fs::weakly_canonical(g_scratch, ec));
    return !ec && !t.empty() && !s.empty() && s.size() > t.size() && f.rfind(s, 0) == 0 && s.rfind(t, 0) == 0 && full != temp;
}

void Write(const fs::path& file, const std::string& bytes) {
    if (!InScratch(file)) return;
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void WriteTree(const fs::path& dir, const Files& files) {
    for (const auto& [path, bytes] : files) Write(dir / PathFromUtf8(path), bytes);
}

fs::path Fresh(const std::string& name) {
    const fs::path dir = g_scratch / PathFromUtf8(name);
    std::error_code ec;
    if (!InScratch(dir)) return {};
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::string Scene(const std::string& world) { return Container({{"Header", HeaderSection(world, 22)}}); }

// A whole level whose world is shaped: its crate texture is in its set, its terrain beside it.
Files Shaped(const std::string& folder, const std::string& stem, const std::vector<int>& actions = {}, const std::vector<std::string>& leads = {}) {
    WorldShape shape;
    shape.terrain = stem;
    shape.meshes.push_back({"Crate-00", "Map\\" + stem + "\\crate.bmp"});
    shape.actions = actions;
    shape.levels = leads;
    return {{folder + stem + ".scn", Scene(stem + ".grf")}, {folder + stem + ".grf", MakeWorld(shape)}, {folder + stem + "-130.pid", Set(stem)},
            {folder + stem + "-130.swp", "swap of " + stem}, {folder + stem + ".wtd", "terrain of " + stem}};
}
Files Ready(const std::string& folder, const std::string& stem) { return Shaped(folder, stem); }
// ActionType 99: an action script the port does not run; the action does nothing.
Files Limited(const std::string& folder, const std::string& stem) { return Shaped(folder, stem, {99}); }
Files Damaged(const std::string& folder, const std::string& stem) {
    Files files = Ready(folder, stem);
    files[1].second = files[1].second.substr(0, 300);
    return files;
}
Files Unbuilt(const std::string& folder, const std::string& stem) {
    Files files = Ready(folder, stem);
    files.pop_back();
    return files;
}

bool Logged(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
}

const LevelPackage* Listed(const std::vector<LevelPackage>& packages, const std::string& id) {
    for (const LevelPackage& p : packages)
        if (p.id == id) return &p;
    return nullptr;
}

const LevelScene* SceneOf(const LevelPackage* package, const std::string& file) {
    if (!package) return nullptr;
    for (const LevelScene& s : package->scenes)
        if (s.file == file) return &s;
    return nullptr;
}

// As the game starts a level: activation, then its admission.
bool Start(const fs::path& user, const jpt_custom_level_request_t& request, std::string* line = nullptr) {
    char error[256] = "", said[512] = "";
    const bool started = jpt_custom_level_activate(Utf8(user).c_str(), &request, error, sizeof error) && jpt_custom_level_admit_start(said, sizeof said);
    if (line) *line = said[0] ? said : error;
    return started;
}

jpt_custom_level_request_t Plain(const std::string& id, const std::string& scene) {
    jpt_custom_level_request_t request{};
    std::snprintf(request.package, sizeof request.package, "%s", id.c_str());
    std::snprintf(request.scene, sizeof request.scene, "%s", scene.c_str());
    return request;
}

void TestClassification() {
    const auto kind = [](const char* code, const char* subject, int note = 0) { return jpt_level_issue_kind(code, subject, note); };
    CHECK(kind("MISSING_TEXTURES", "w.grf", 1) == JPT_ISSUE_NOTE && jpt_issue_compat(JPT_ISSUE_NOTE) == JPT_COMPAT_READY);
    CHECK(kind("UNSUPPORTED_FEATURE", "CE_SKY_STARS") == JPT_ISSUE_FEATURE && kind("UNSUPPORTED_ACTION", "ActionType 99") == JPT_ISSUE_FEATURE);
    CHECK(kind("README_FILE_ABSENT", "package") == JPT_ISSUE_UNCONFIRMED && kind("CONFIG_WARNING", "config_invalid_value") == JPT_ISSUE_UNCONFIRMED);
    CHECK(kind("MALFORMED_WORLD", "w.grf") == JPT_ISSUE_DAMAGED && kind("INVALID_MESH_INDEX", "Mesh-00") == JPT_ISSUE_DAMAGED);
    CHECK(kind("MISSING_REQUIRED_TERRAIN", "w.wtd") == JPT_ISSUE_MISSING && kind("MISSING_TEXTURES", "w.grf") == JPT_ISSUE_MISSING);
    CHECK(kind("MESH_EXCEEDS_LIMIT", "Big-00") == JPT_ISSUE_CAPACITY && kind("UNSUPPORTED_SCENE_FORMAT", "x.scn") == JPT_ISSUE_INCOMPATIBLE);
    CHECK(kind("CONFIG_WARNING", "config_unsafe_path") == JPT_ISSUE_SECURITY && kind("CURVED_BUMP_NOT_IN_SET", "w.grf") == JPT_ISSUE_INCOMPATIBLE);
    // What is not classified stays blocked.
    for (const auto& [code, subject] : {std::pair("A_NEW_PROBLEM", "x"), std::pair("UNSUPPORTED_FEATURE", "NOT_A_FEATURE"), std::pair("UNSUPPORTED_ACTION", "ActionType 9x"),
                                        std::pair("UNSUPPORTED_ACTION", "ActionType 150"), std::pair("CONFIG_WARNING", "config_new_kind"), std::pair("", "")})
        CHECK(kind(code, subject) == JPT_ISSUE_UNKNOWN && jpt_issue_compat(kind(code, subject)) == JPT_COMPAT_INVALID);
    for (int k = JPT_ISSUE_MISSING; k < JPT_ISSUE_KINDS; ++k) CHECK(jpt_issue_compat(jpt_issue_kind_t(k)) == JPT_COMPAT_INVALID);
    CHECK(jpt_issue_compat(JPT_ISSUE_FEATURE) == JPT_COMPAT_LIMITED && jpt_issue_compat(JPT_ISSUE_UNCONFIRMED) == JPT_COMPAT_LIMITED);

    // The most restrictive state holds.
    const LevelIssue note{"NO_TERRAIN", "w.grf", "", IssuePolicy::kInfo}, limited{"UNSUPPORTED_ACTION", "ActionType 99", "", IssuePolicy::kUnsupported},
                     damaged{"MALFORMED_WORLD", "w.grf", "", IssuePolicy::kMalformed}, unknown{"SOMETHING", "x", "", IssuePolicy::kWarning};
    CHECK(CompatibilityOf({}) == JPT_COMPAT_READY && CompatibilityOf({note}) == JPT_COMPAT_READY && CompatibilityOf({note, limited}) == JPT_COMPAT_LIMITED);
    CHECK(CompatibilityOf({limited, damaged, note}) == JPT_COMPAT_INVALID && CompatibilityOf({limited, unknown}) == JPT_COMPAT_INVALID);
    CHECK(jpt_compat_worst(JPT_COMPAT_LIMITED, JPT_COMPAT_READY) == JPT_COMPAT_LIMITED && jpt_compat_worst(JPT_COMPAT_LIMITED, JPT_COMPAT_INVALID) == JPT_COMPAT_INVALID);

    // Every feature the port lacks is classified on purpose, and only those the runtime leaves out are LIMITED.
    const std::vector<std::string> left_out = {"ATX_ACTION_99", "ATX_JUMPS", "ATX_PROJECTILES", "ATX_PLUGIN", "ATX_SETTINGS", "CE_END_GAME_DDF", "CE_SCRIPTS", "CE_SCRIPT_API",
                                               "CE_SCRIPT_FILES", "CE_EXTENDED_AI", "CE_AI_ACTIVATE_DISTANCE", "CE_WOOD_SAP", "CE_TEXHW_ODD_SIZE", "CE_TEXHW_BUMPED", "CE_SKY_STARS"};
    for (int i = 0; const jpt_level_feature_t* f = jpt_level_feature_at(i); ++i) {
        const jpt_issue_kind_t k = kind("UNSUPPORTED_FEATURE", f->name);
        const bool listed = std::find(left_out.begin(), left_out.end(), f->name) != left_out.end();
        CHECK(k == (listed ? JPT_ISSUE_FEATURE : JPT_ISSUE_INCOMPATIBLE));
        if (f->status == JPT_FEATURE_NOT_SUPPORTED || f->status == JPT_FEATURE_PARTIALLY_SUPPORTED) CHECK(listed);
    }
    for (const std::string& name : left_out) CHECK(jpt_level_feature(name.c_str()) != nullptr);

    jpt_compat_t parsed = JPT_COMPAT_INVALID;
    for (const jpt_compat_t c : {JPT_COMPAT_READY, JPT_COMPAT_LIMITED, JPT_COMPAT_INVALID}) CHECK(jpt_compat_parse(jpt_compat_name(c), &parsed) && parsed == c);
    CHECK(!jpt_compat_parse("READY", &parsed) && !jpt_compat_parse("", &parsed) && !jpt_compat_parse(nullptr, &parsed));
    CHECK(std::string(jpt_compat_title(JPT_COMPAT_LIMITED)) == "LIMITED COMPATIBILITY" && std::string(jpt_compat_title(JPT_COMPAT_INVALID)) == "INVALID / UNSAFE");
}

// Each kind of source, each mix of states: installed when one level can be started, never for INVALID / UNSAFE alone.
void TestImports() {
    const fs::path game = Fresh("import game") / "versions" / "en" / "data";
    fs::create_directories(game);
    struct Case {
        const char* name;
        Files files;
        bool installed;
        size_t ready, limited;
    };
    const Case cases[] = {
        {"OnlyReady", Ready("", "plain"), true, 1, 0},
        {"OnlyLimited", Limited("", "odd"), true, 0, 1},
        {"Mixed", Ready("", "good") + Limited("", "odd") + Damaged("", "torn"), true, 1, 1},
        {"OnlyInvalid", Damaged("", "torn") + Unbuilt("", "bare"), false, 0, 0},
    };
    for (const char* kind : {"folder", "zip", "7z", "rar"}) {
        const fs::path user = Fresh(std::string("import user ") + kind);
        for (const Case& c : cases) {
            ImportResult r;
            const fs::path in = Fresh(std::string("in ") + kind + " " + c.name);
            if (!std::strcmp(kind, "folder")) {
                WriteTree(in / c.name, c.files);
                r = ImportLevelFolder(in / c.name, user, {}, {}, game);
            } else {
                const fs::path archive = in / (std::string(c.name) + "." + kind);
                if (!std::strcmp(kind, "zip")) WriteZip(archive, c.files, 0);
                else Write(archive, !std::strcmp(kind, "7z") ? SevenZip(c.files, 0) : Rar(c.files));
                r = ImportLevelArchive(archive, user, {}, {}, game);
            }
            const bool ok = r.ok() == c.installed && r.levels == c.ready && r.limited == c.limited && fs::exists(CustomLevelsDir(user) / c.name) == c.installed;
            if (!ok) std::printf("  %s %s: %s ready %zu limited %zu\n", kind, c.name, ImportStatusTitle(r.status), r.levels, r.limited);
            CHECK(ok);
            CHECK(Logged(r.log, std::string("result: ") + (c.installed ? "installed" : "rejected")));
            if (!c.installed) CHECK(Logged(r.log, "compatibility: invalid") && ImportNotice(r.status).failed);
        }
        const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
        const LevelScene *good = SceneOf(Listed(listed, "Mixed"), "good.scn"), *odd = SceneOf(Listed(listed, "Mixed"), "odd.scn"), *torn = SceneOf(Listed(listed, "Mixed"), "torn.scn");
        CHECK(good && odd && torn && good->playable() && odd->limited() && torn->compatibility() == JPT_COMPAT_INVALID && !torn->launchable());
        if (good && odd && torn) {
            CHECK(ActionsOf(good, true).play && !ActionsOf(good, true).play_anyway && ActionsOf(odd, true).play_anyway && !ActionsOf(odd, true).play && ActionsOf(odd, true).report);
            CHECK(!ActionsOf(torn, true).play && !ActionsOf(torn, true).play_anyway && ActionsOf(torn, true).report && std::string(LevelStatusText(*torn)) == "Invalid / Unsafe");
        }
    }

    // A whole package that is unsafe is refused whatever its levels are.
    const fs::path user = Fresh("import unsafe user"), in = Fresh("import unsafe in");
    const Files escaping = Limited("", "odd") + Files{{"../escape.txt", "out"}};
    WriteZip(in / "escape.zip", escaping, 0);
    Write(in / "escape.7z", SevenZip(escaping, 0));
    Write(in / "escape.rar", Rar(escaping));
    for (const char* name : {"escape.zip", "escape.7z", "escape.rar"}) {
        const ImportResult r = ImportLevelArchive(in / name, user, {}, {}, game);
        CHECK(!r.ok() && r.status == ImportStatus::kUnsafe && !fs::exists(in.parent_path() / "escape.txt"));
    }
    CHECK(!fs::exists(CustomLevelsDir(user)) || fs::is_empty(CustomLevelsDir(user)));
    // A path a level's settings name outside the package blocks that level whatever else it is.
    LevelScene scene;
    scene.valid = true;
    scene.state = LevelState::kLimited;
    scene.issues = {{"UNSUPPORTED_ACTION", "ActionType 99", "", IssuePolicy::kUnsupported}, {"CONFIG_WARNING", "config_unsafe_path", "ModPaths=../x", IssuePolicy::kWarning}};
    CHECK(CompatibilityOf(scene.issues) == JPT_COMPAT_INVALID && ReasonsOf(scene)[0].kind == JPT_ISSUE_SECURITY);
}

// Link External Folder and the fan missions of a CE tree: a LIMITED one is linked, an INVALID one is not, nothing is written there.
void TestLinks() {
    const fs::path user = Fresh("link user"), game = Fresh("link game") / "versions" / "en" / "data", library = Fresh("library");
    fs::create_directories(game);
    WriteTree(library / "Odd", Limited("", "odd"));
    WriteTree(library / "Torn", Damaged("", "torn"));
    LinkResult r = LinkLevelFolder(library, user, {}, {}, game);
    CHECK(r.ok() && r.linked.size() == 1 && r.levels == 0 && r.limited == 1 && Logged(r.log, "compatibility: limited") && Logged(r.log, "compatibility: invalid"));

    const fs::path tree = Fresh("tree"), tree_user = Fresh("tree user");
    WriteTree(tree, Files{{"tp_mod.ini", "[Paths]\nFMPath=fms\n"}, {"fms/Alpha/fm.ini", "[General]\nStartLevel=alpha.scn\n"}, {"fms/Beta/fm.ini", "[General]\nStartLevel=beta.scn\nEndGameDDF=end\n"}} +
                        Ready("fms/Alpha/data/", "alpha") + Ready("fms/Beta/data/", "beta"));
    r = LinkLevelFolder(tree, tree_user, {}, {}, game);
    CHECK(r.ok() && r.linked.size() == 2 && r.levels == 1 && r.limited == 1);
    const std::vector<LevelPackage> listed = ListLevelPackages(tree_user, game);
    size_t ready = 0, limited = 0;
    for (const LevelPackage& p : listed)
        for (const LevelScene& s : p.scenes) (s.playable() ? ready : limited) += s.launchable();
    CHECK(ready == 1 && limited == 1);
}

// Play and Play Anyway, and what the game admits whatever the launcher asked.
void TestPlayAnyway() {
    const fs::path user = Fresh("play user"), game = Fresh("play game") / "versions" / "en" / "data", in = Fresh("play in");
    fs::create_directories(game);
    WriteTree(in / "Mix", Ready("", "good") + Limited("", "odd") + Damaged("", "torn"));
    const ImportResult imported = ImportLevelFolder(in / "Mix", user, {}, {}, game);
    CHECK(imported.ok() && imported.levels == 1 && imported.limited == 1);
    const std::string id = imported.package;

    const PlayCheck ready = CheckBeforePlay(user, game, id, "good.scn");
    CHECK(ready.ok && !ready.needs_consent && !ready.experimental && ready.compat == JPT_COMPAT_READY && LaunchRequest(id, "good.scn", ready).experimental == 0);
    CHECK(Start(user, LaunchRequest(id, "good.scn", ready)) && !jpt_custom_level_experimental() && std::string(jpt_custom_level_compat_name()) == "ready");
    jpt_custom_level_deactivate();

    // Without the warning's answer nothing starts: neither the launcher's check nor the game.
    const PlayCheck asked = CheckBeforePlay(user, game, id, "odd.scn");
    CHECK(!asked.ok && asked.needs_consent && asked.compat == JPT_COMPAT_LIMITED && !asked.reasons.empty() && !asked.keys.empty() && Logged(asked.log, "compatibility=limited"));
    CHECK(!asked.reasons.empty() && asked.reasons.front().kind == JPT_ISSUE_FEATURE && asked.reasons.front().key() == "UNSUPPORTED_ACTION ActionType 99");
    std::string line;
    CHECK(!Start(user, Plain(id, "odd.scn"), &line) && line.find("Play Anyway was not confirmed") != std::string::npos && !jpt_custom_level_active());

    // Play Anyway for this start: the level and nothing else.
    const PlayConsent consent{asked.keys};
    const PlayCheck anyway = CheckBeforePlay(user, game, id, "odd.scn", &consent);
    CHECK(anyway.ok && anyway.experimental && anyway.authorized == std::vector<std::string>{"odd.scn"} && Logged(anyway.log, "result: started with Play Anyway"));
    const jpt_custom_level_request_t request = LaunchRequest(id, "odd.scn", anyway);
    CHECK(Start(user, request, &line) && line.find("experimental launch authorized") != std::string::npos && jpt_custom_level_experimental() &&
          std::string(jpt_custom_level_compat_name()) == "limited");
    jpt_custom_level_deactivate();
    // A consent for other reasons covers nothing.
    const PlayConsent stale{{"odd.scn UNSUPPORTED_FEATURE CE_SKY_STARS"}};
    const PlayCheck changed = CheckBeforePlay(user, game, id, "odd.scn", &stale);
    CHECK(!changed.ok && changed.needs_consent && Logged(changed.log, "changed after Play Anyway"));

    // INVALID / UNSAFE: no consent, request or record opens it.
    const PlayConsent everything{asked.keys};
    const PlayCheck torn = CheckBeforePlay(user, game, id, "torn.scn", &everything);
    CHECK(!torn.ok && !torn.needs_consent && torn.compat == JPT_COMPAT_INVALID && Logged(torn.log, "launch blocked: structural validation failed"));
    jpt_custom_level_request_t forced = Plain(id, "torn.scn");
    forced.experimental = 1;
    std::snprintf(forced.authorized, sizeof forced.authorized, "torn.scn\nodd.scn");
    CHECK(!Start(user, forced, &line) && line.find("INVALID / UNSAFE") != std::string::npos);
    // An edited manifest and runtime map: the launcher analyses the package again and writes its own record back.
    const fs::path records = CustomLevelsDir(user) / id;
    std::string map = Read(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP);
    const size_t at = map.find("compat torn.scn|invalid");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) map.replace(at, std::strlen("compat torn.scn|invalid"), "compat torn.scn|ready");
    Write(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP, map);
    std::string manifest = Read(records / "manifest.json");
    for (size_t p = manifest.find("\"invalid\""); p != std::string::npos; p = manifest.find("\"invalid\"")) manifest.replace(p, 9, "\"ready\"");
    Write(records / "manifest.json", manifest);
    CHECK(!CheckBeforePlay(user, game, id, "torn.scn", &everything).ok && Read(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP).find("compat torn.scn|invalid") != std::string::npos);
    // A runtime map without a record of the start level: the game refuses it rather than guess.
    std::string bare;
    std::istringstream lines(Read(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP));
    for (std::string l; std::getline(lines, l);)
        if (l.rfind("compat ", 0) != 0) bare += l + "\n";
    Write(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP, bare);
    CHECK(!Start(user, Plain(id, "good.scn"), &line) && line.find("no compatibility was recorded") != std::string::npos);
    CHECK(CheckBeforePlay(user, game, id, "good.scn").ok);

    // A linked folder that changes after its analysis: checked again before Play; a LIMITED level that became INVALID never starts.
    const fs::path library = Fresh("play library");
    WriteTree(library / "Moving", Limited("", "move"));
    const LinkResult linked = LinkLevelFolder(library / "Moving", user, {}, {}, game);
    CHECK(linked.ok() && linked.limited == 1);
    const std::string moving = linked.linked.empty() ? std::string() : linked.linked[0];
    const PlayCheck before = CheckBeforePlay(user, game, moving, "move.scn");
    const PlayConsent given{before.keys};
    CHECK(before.needs_consent && CheckBeforePlay(user, game, moving, "move.scn", &given).ok);
    Write(library / "Moving" / "move.grf", Read(library / "Moving" / "move.grf").substr(0, 300));
    const PlayCheck after = CheckBeforePlay(user, game, moving, "move.scn", &given);
    CHECK(!after.ok && !after.needs_consent && after.compat == JPT_COMPAT_INVALID);
    WriteTree(library / "Moving", Ready("", "move"));
    CHECK(CheckBeforePlay(user, game, moving, "move.scn").ok);

    // No command-line option of either host names a custom level: only the launcher's own request does.
    for (const char* file : {"src/host/main.c", "src/linux/linux_main.c"}) {
        const std::string source = Read(fs::path(JPT_SOURCE_DIR) / file);
        CHECK(!source.empty() && source.find("--custom-level") == std::string::npos && source.find("\"--level") == std::string::npos && source.find("experimental =") == std::string::npos);
    }
}

// One Play Anyway covers the level and the LIMITED levels it leads to; a level it does not reach, an INVALID one and a save of either
// stay refused, through Restart and Load as well.
void TestSessionsAndSaves() {
    const fs::path user = Fresh("session user"), game = Fresh("session game") / "versions" / "en" / "data", in = Fresh("session in"), saves = Fresh("session saves");
    fs::create_directories(game);
    WriteTree(in / "Chain", Shaped("", "start", {99}, {"next.scn", "torn.scn"}) + Shaped("", "next", {99}) + Limited("", "aside") + Damaged("", "torn") + Ready("", "calm") +
                               Shaped("", "gate", {}, {"next.scn"}));
    const ImportResult imported = ImportLevelFolder(in / "Chain", user, {}, {}, game);
    CHECK(imported.ok());
    const std::string id = imported.package;
    const PlayCheck asked = CheckBeforePlay(user, game, id, "start.scn");
    CHECK(asked.needs_consent && asked.authorized.size() == 2 && Logged(asked.authorized, "next.scn") && !Logged(asked.authorized, "aside.scn") && !Logged(asked.authorized, "torn.scn"));
    CHECK(std::any_of(asked.reasons.begin(), asked.reasons.end(), [](const LevelReason& r) { return r.subject.rfind("next.scn: ", 0) == 0; }));
    const PlayConsent consent{asked.keys};
    const PlayCheck anyway = CheckBeforePlay(user, game, id, "start.scn", &consent);
    CHECK(anyway.ok && anyway.experimental);
    CHECK(Start(user, LaunchRequest(id, "start.scn", anyway)));
    char why[256];
    const auto gate = [&why](const char* scene) {
        char file[1024] = "";
        jpt_custom_level_find(scene, file, sizeof file);
        return jpt_gate_load(file, scene, why, sizeof why);
    };
    CHECK(gate("next.scn") == JPT_GATE_OPEN && gate("calm.scn") == JPT_GATE_OPEN);
    CHECK(gate("aside.scn") == JPT_GATE_NOT_AUTHORIZED && std::string(why).find("does not cover") != std::string::npos);
    CHECK(gate("torn.scn") == JPT_GATE_UNSAFE && std::string(why).find("INVALID") != std::string::npos);
    CHECK(jpt_gate_message(JPT_GATE_NOT_AUTHORIZED) != nullptr);
    // Restart keeps the session what it was.
    char restarted[1024];
    CHECK(jpt_custom_level_restart("start.scn", restarted, sizeof restarted) && jpt_custom_level_experimental() && gate("start.scn") == JPT_GATE_OPEN);

    // Saves: the package's own, judged by the level they hold and this launch's consent.
    const std::string slot = Utf8(saves / "SAVEGAME.000");
    char temp[1024];
    const auto save_as = [&](const std::string& bytes) {
        if (!jpt_gate_save_begin(slot.c_str(), temp, sizeof temp)) return false;
        Write(PathFromUtf8(temp), bytes);
        return jpt_gate_save_end(slot.c_str(), temp, why, sizeof why) != 0;
    };
    const auto load = [&] { return jpt_gate_load(slot.c_str(), "SAVEGAME.000", why, sizeof why); };
    CHECK(save_as(Save("next.grf", "next.scn")) && load() == JPT_GATE_OPEN);
    CHECK(save_as(Save("aside.grf", "aside.scn")) && load() == JPT_GATE_NOT_AUTHORIZED);
    jpt_custom_level_deactivate();
    // A later READY start of the same package: the experimental save is not loaded without Play Anyway.
    CHECK(Start(user, LaunchRequest(id, "calm.scn", CheckBeforePlay(user, game, id, "calm.scn"))) && !jpt_custom_level_experimental());
    CHECK(save_as(Save("next.grf", "next.scn")) && load() == JPT_GATE_NOT_AUTHORIZED);
    CHECK(save_as(Save("calm.grf", "calm.scn")) && load() == JPT_GATE_OPEN);
    // A READY level that leads to a LIMITED one: the transition is refused in a READY start, allowed with Play Anyway.
    CHECK(gate("next.scn") == JPT_GATE_NOT_AUTHORIZED);
    jpt_custom_level_deactivate();
    const PlayCheck through = CheckBeforePlay(user, game, id, "gate.scn");
    CHECK(through.ok && !through.experimental && !through.keys.empty());
    const PlayConsent onward{through.keys};
    const PlayCheck covered = CheckBeforePlay(user, game, id, "gate.scn", &onward);
    CHECK(covered.ok && covered.experimental && covered.authorized == std::vector<std::string>{"next.scn"});
    CHECK(Start(user, LaunchRequest(id, "gate.scn", covered)) && gate("next.scn") == JPT_GATE_OPEN && gate("aside.scn") == JPT_GATE_NOT_AUTHORIZED);
    jpt_custom_level_deactivate();
    char folder[512];
    CHECK(jpt_custom_level_saves_dir("saves", id.c_str(), folder, sizeof folder) && std::string(folder).find(id) != std::string::npos);
}

// The compatibility report: Bug Report's log lines, for a LIMITED level and an INVALID one, without the folder's own path.
void TestReports() {
    const fs::path user = Fresh("report user"), game = Fresh("report game") / "versions" / "en" / "data", library = Fresh("report library");
    fs::create_directories(game);
    WriteTree(library / "Pack", Limited("", "odd") + Damaged("", "torn"));
    const LinkResult linked = LinkLevelFolder(library / "Pack", user, {}, {}, game);
    CHECK(linked.ok());
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    const LevelPackage* package = linked.linked.empty() ? nullptr : Listed(listed, linked.linked[0]);
    const LevelScene *odd = SceneOf(package, "odd.scn"), *torn = SceneOf(package, "torn.scn");
    CHECK(package && odd && torn);
    if (!package || !odd || !torn) return;
    const std::vector<std::string> limited = ReportLines(*package, odd, game), invalid = ReportLines(*package, torn, game), all = ReportLines(*package, nullptr, game);
    CHECK(Logged(limited, "compatibility report, origin external folder, platform ") && Logged(limited, "compatibility=limited (LIMITED COMPATIBILITY)") &&
          Logged(limited, "UNSUPPORTED_ACTION ActionType 99") && Logged(limited, "[missing feature, limited]") && !Logged(limited, "torn.scn"));
    CHECK(Logged(invalid, "compatibility=invalid (INVALID / UNSAFE)") && Logged(invalid, "[damaged file, invalid]") && !Logged(invalid, "odd.scn"));
    CHECK(Logged(all, "odd.scn") && Logged(all, "torn.scn"));
    for (const std::vector<std::string>* lines : {&limited, &invalid, &all}) CHECK(!Logged(*lines, Utf8(library)));
    const std::vector<LevelReason> reasons = ReasonsOf(*torn);
    CHECK(!reasons.empty() && reasons[0].compat == JPT_COMPAT_INVALID && std::string(IssueKindLabel(reasons[0].kind)) == "Damaged file");
    // A report is sent from the Bug Report page alone, after its preview and the player's choice: the custom level code sends nothing.
    for (const char* file : {"launcher/custom_levels.cpp", "launcher/custom_levels_page.cpp", "launcher/level_listing.cpp"}) {
        const std::string source = Read(fs::path(JPT_SOURCE_DIR) / file);
        CHECK(!source.empty() && source.find("http_post") == std::string::npos && source.find("incident_client") == std::string::npos && source.find("Submit") == std::string::npos);
    }
}

// Records a v0.1.3 launcher wrote: kept where they serve, read again where they cannot tell the new states apart.
void TestOldRecords() {
    const fs::path user = Fresh("old user"), game = Fresh("old game") / "versions" / "en" / "data", in = Fresh("old in"), saves = Fresh("old saves");
    fs::create_directories(game);
    WriteTree(in / "Before", Ready("", "plain") + Limited("", "odd"));
    const ImportResult imported = ImportLevelFolder(in / "Before", user, {}, {}, game);
    CHECK(imported.ok());
    const std::string id = imported.package;
    const fs::path records = CustomLevelsDir(user) / id, listing = records / JPT_CUSTOM_LEVEL_GENERATED_DIR / "listing.txt";
    ListLevelPackages(user, game);
    // As v0.1.3 left it: an older listing record that called the level Not supported, no compatibility in the map.
    std::string record = Read(listing);
    const std::string head = "custom-level-listing " + std::to_string(kListingVersion) + "\n";
    CHECK(record.rfind(head, 0) == 0);
    record.replace(0, head.size(), "custom-level-listing " + std::to_string(kListingVersion - 1) + "\n");
    const size_t limited = record.find("|" + std::to_string(int(LevelState::kLimited)) + "|");
    if (limited != std::string::npos) record.replace(limited, 3, "|" + std::to_string(int(LevelState::kNotSupported)) + "|");
    Write(listing, record);
    std::string map;
    std::istringstream lines(Read(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP));
    for (std::string l; std::getline(lines, l);)
        if (l.rfind("compat ", 0) != 0) map += l + "\n";
    Write(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP, map);
    Write(saves / "custom_levels" / id / "SAVEGAME.000", "a save");

    ListingStats stats;
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game, &stats);
    const LevelPackage* package = Listed(listed, id);
    CHECK(stats.analysed == 1 && package && package->id == id && SceneOf(package, "odd.scn") && SceneOf(package, "odd.scn")->limited());
    CHECK(Read(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP).find("compat odd.scn|limited") != std::string::npos && Read(saves / "custom_levels" / id / "SAVEGAME.000") == "a save");
    CHECK(Read(records / "manifest.json").find("\"compatibility\"") != std::string::npos && package->imported_at != 0);

    // A linked folder that is away keeps the row the older record describes, and is never played until it is back.
    const fs::path library = Fresh("old library");
    WriteTree(library / "Away", Limited("", "far"));
    const LinkResult linked = LinkLevelFolder(library / "Away", user, {}, {}, game);
    CHECK(linked.ok() && !linked.linked.empty());
    if (linked.linked.empty()) return;
    ListLevelPackages(user, game);
    const fs::path away = CustomLevelsDir(user) / linked.linked[0] / JPT_CUSTOM_LEVEL_GENERATED_DIR / "listing.txt";
    record = Read(away);
    if (record.rfind(head, 0) == 0) record.replace(0, head.size(), "custom-level-listing " + std::to_string(kListingVersion - 1) + "\n");
    Write(away, record);
    std::error_code ec;
    fs::rename(library / "Away", library / "Gone", ec);
    const std::vector<LevelPackage> after = ListLevelPackages(user, game);
    const LevelPackage* gone = Listed(after, linked.linked[0]);
    CHECK(!ec && gone && !gone->available && !gone->scenes.empty() && !CheckBeforePlay(user, game, linked.linked[0], "far.scn").ok);
}

} // namespace

void RunCompatTests(const fs::path& scratch, int& checks, int& failures) {
    g_scratch = scratch / "compat";
    std::error_code ec;
    fs::create_directories(g_scratch, ec);
    if (!InScratch(g_scratch / "x")) {
        std::printf("FAIL compat tests: the scratch folder %s is not below the temporary folder\n", Utf8(g_scratch).c_str());
        ++failures;
        return;
    }
    TestClassification();
    TestImports();
    TestLinks();
    TestPlayAnyway();
    TestSessionsAndSaves();
    TestReports();
    TestOldRecords();
    jpt_custom_level_deactivate();
    checks += g_checks;
    failures += g_failures;
}
