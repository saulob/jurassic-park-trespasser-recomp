// Link External Folder: packages played where they are, never copied, moved or written; their records and saves in user_data;
// several packages in one folder; the fan missions of a tp_mod.ini tree with the folders they share and their precedence;
// Rescan, changes found before Play, a folder that is gone, Relocate Folder, Unlink, and the paths a link may not take.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "jpt_custom_level.h"
#include "level_fixtures.h"
#include "level_link.h"
#include "level_listing.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;
using namespace fixtures;
using Files = std::vector<std::pair<std::string, std::string>>;

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

void Write(const fs::path& file, const std::string& bytes) {
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
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string Scene(const std::string& world, uint32_t version = 22) { return Container({{"Header", HeaderSection(world, version)}}); }

// A level the scanner finds whole: scene, world, texture set, swap file and terrain, below folder.
Files Level(const std::string& folder, const std::string& stem, uint32_t version = 22) {
    return {{folder + stem + ".scn", Scene(stem + ".grf", version)}, {folder + stem + ".grf", World(stem)}, {folder + stem + "-130.pid", Set(stem)},
            {folder + stem + "-130.swp", "swap of " + stem}, {folder + stem + ".wtd", "terrain of " + stem}};
}

Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Files Without(Files files, const std::string& path) {
    files.erase(std::remove_if(files.begin(), files.end(), [&path](const auto& f) { return f.first == path; }), files.end());
    return files;
}

// Every file below a folder with its bytes and time: what linking, listing, playing, relocating or unlinking leaves as it was.
std::map<std::string, std::pair<std::string, fs::file_time_type>> Snapshot(const fs::path& dir) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> files;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) files[fs::relative(e.path(), dir).generic_string()] = {Read(e.path()), e.last_write_time()};
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

bool Activate(const fs::path& user, const std::string& package, const char* scene) {
    jpt_custom_level_request_t request{};
    char error[256] = "";
    std::snprintf(request.package, sizeof request.package, "%s", package.c_str());
    std::snprintf(request.scene, sizeof request.scene, "%s", scene);
    return jpt_custom_level_activate(Utf8(user).c_str(), &request, error, sizeof error) != 0;
}

std::string Found(const char* relative) {
    char host[1024];
    return jpt_custom_level_find(relative, host, sizeof host) ? Read(fs::path(host)) : std::string("-");
}

std::string Setting(const char* wanted) {
    const char *key = nullptr, *value = nullptr;
    for (int i = 0; jpt_custom_level_setting(i, &key, &value); ++i)
        if (!std::strcmp(key, wanted)) return value;
    return "-";
}

bool SamePlace(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

// A folder of several packages: each is registered where it is, its records alone in user_data, nothing copied or written.
void TestLinkFolder() {
    const fs::path user = Fresh("user"), library = Fresh("library");
    WriteTree(library / "First", Level("", "first") + Files{{"tool.exe", "MZ"}, {"readme.txt", "How to play the first level."}});
    WriteTree(library / "Second", Level("data/", "second"));
    WriteTree(library / "Broken", {{"broken.scn", "not a scene"}});
    Write(library / "packed.zip", "PK");
    const auto before = Snapshot(library);

    const LinkResult r = LinkLevelFolder(library, user, {});
    CHECK(r.ok() && r.linked.size() == 2 && r.already.empty() && r.levels == 2);
    CHECK(Logged(r.log, "archive(s) are not linked") && Logged(r.log, "result: linked") && Logged(r.log, "result: rejected"));
    CHECK(Snapshot(library) == before);
    for (const std::string& id : r.linked) {
        const fs::path records = CustomLevelsDir(user) / id;
        CHECK(fs::is_regular_file(records / JPT_CUSTOM_LEVEL_LINK_FILE) && fs::is_regular_file(records / "manifest.json") &&
              fs::is_regular_file(records / JPT_CUSTOM_LEVEL_RUNTIME_MAP));
        for (const char* copy : {"first.scn", "first.grf", "data", "tool.exe", "readme.txt"}) CHECK(!fs::exists(records / copy));
    }

    std::vector<LevelPackage> listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 2);
    for (const LevelPackage& p : listed) {
        CHECK(p.linked && p.available && p.origin == "external" && p.records == CustomLevelsDir(user) / p.id);
        CHECK(p.scenes.size() == 1 && p.scenes[0].playable() && p.levels().size() == 1);
        CHECK(SamePlace(p.dir, library / (p.scenes[0].file == "first.scn" ? "First" : "Second")));
    }
    const LevelPackage* first = Listed(listed, "First");
    CHECK(first && first->readme == "readme.txt");
    fs::path readme;
    CHECK(first && ResolveReadme(user, *first, readme) && SamePlace(readme, library / "First" / "readme.txt"));
    CHECK(Snapshot(library) == before);

    // The same folders again, however spelled: nothing new.
    LinkResult again = LinkLevelFolder(library / "First" / ".." / ".", user, {});
    CHECK(again.status == ImportStatus::kAlreadyLinked && again.linked.empty() && again.already.size() == 2 && !again.ok());
    again = LinkLevelFolder(library / "Second", user, {});
    CHECK(again.status == ImportStatus::kAlreadyLinked && again.already == std::vector<std::string>{"Second"});
#ifdef _WIN32
    std::string upper = Utf8(library);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return char(std::toupper(static_cast<unsigned char>(c))); });
    CHECK(LinkLevelFolder(PathFromUtf8(upper), user, {}).status == ImportStatus::kAlreadyLinked);
#endif
    CHECK(ListLevelPackages(user, {}).size() == 2 && Snapshot(library) == before);

    // An imported copy and a linked one are told apart, and each goes its own way.
    std::string error;
    CHECK(ImportLevelFolder(library / "First", user, {}).ok());
    listed = ListLevelPackages(user, {});
    const LevelPackage* copy = Listed(listed, "First-2");
    CHECK(listed.size() == 3 && copy && !copy->linked && copy->origin == "folder" && fs::exists(copy->dir / "first.scn"));
    CHECK(!UnlinkLevelPackage(user, "First-2", error) && !DeleteLevelPackage(user, "First", error));
    CHECK(DeleteLevelPackage(user, "First-2", error) && Snapshot(library) == before);
}

// What the game reads of a linked package: its own files where they are, the records in user_data, no program, the other
// levels of the package for its transitions and Restart Level, and saves of its own.
void TestLinkedSession() {
    const fs::path user = Fresh("session user"), mission = Fresh("session mission");
    WriteTree(mission, Level("data/", "start") + Level("data/", "next") + Files{{"fm.ini", "[General]\nStartLevel=start.scn\nEnableDualStow=True\n"}, {"data/tool.dll", "MZ"}});
    Write(mission / JPT_CUSTOM_LEVEL_RUNTIME_MAP, "trespasser-runtime 1\nfile data/start.grf|data/next.grf\n");
    Write(mission / JPT_CUSTOM_LEVEL_LINK_FILE, "trespasser-link 1\npath /\n");
    const auto before = Snapshot(mission);
    const LinkResult r = LinkLevelFolder(mission, user, {});
    CHECK(r.ok() && r.linked.size() == 1);
    if (r.linked.empty()) return;
    const std::string id = r.linked[0];
    PackageLink link;
    CHECK(ReadPackageLink(CustomLevelsDir(user) / id, link) && SamePlace(link.root, mission) && link.scope.empty() && link.identity.size() == 64);

    CHECK(Activate(user, id, "data/start.scn") && jpt_custom_level_linked());
    CHECK(SamePlace(jpt_custom_level_dir(), mission) && SamePlace(jpt_custom_level_records_dir(), CustomLevelsDir(user) / id));
    CHECK(Found("start.grf") == World("start") && Found("START-130.PID") == Set("start") && Found("next.scn") == Scene("next.grf"));
    CHECK(Found("tool.dll") == "-" && Setting("dual_stow") == "1");
    char host[1024], saves[1024];
    jpt_custom_level_scene_loading("next.scn");
    CHECK(std::string(jpt_custom_level_current_scene()) == "data/next.scn");
    CHECK(jpt_custom_level_restart("next.scn", host, sizeof host) && SamePlace(host, mission / "data" / "next.scn"));
    CHECK(jpt_custom_level_saves_dir("saves", id.c_str(), saves, sizeof saves) && fs::path(saves) == fs::path("saves") / JPT_CUSTOM_LEVELS_DIR / id);
    jpt_custom_level_deactivate();
    CHECK(!jpt_custom_level_linked() && Found("start.grf") == "-");

    const PlayCheck play = CheckBeforePlay(user, {}, id, "data/start.scn");
    CHECK(play.ok && fs::exists(CustomLevelsDir(user) / id / ".runtime" / "guide"));
    CHECK(!fs::exists(mission / ".runtime" / "guide") && Snapshot(mission) == before);

    // A link the launcher did not write is never followed outside its folder.
    Write(CustomLevelsDir(user) / id / JPT_CUSTOM_LEVEL_LINK_FILE, PackageLinkText(link) + "scope ../outside\n");
    CHECK(!Activate(user, id, "data/start.scn") && !jpt_custom_level_active());
    Write(CustomLevelsDir(user) / id / JPT_CUSTOM_LEVEL_LINK_FILE, "not a link\n");
    CHECK(!Activate(user, id, "data/start.scn"));
    CHECK(WritePackageLink(CustomLevelsDir(user) / id, link) && Activate(user, id, "data/start.scn"));
    jpt_custom_level_deactivate();
}

// A tree of fan missions and the folders tp_mod.ini shares between them: one package for each mission, each seeing its own
// files, then the shared ones in the order Trespasser CE searches them, and never the other missions.
void TestModTree() {
    const fs::path user = Fresh("tree user"), tree = Fresh("tree");
    WriteTree(tree, Files{{"tp_mod.ini", "[Paths]\nFMPath=fms\nModPaths=shared\nUberModPaths=over\n[FM]\nActiveFM=MissionB\n"},
                          {"over/data/anim/door.asa", "door over all"},
                          {"shared/data/anim/door.asa", "shared door"},
                          {"shared/data/anim/gate.asa", "shared gate"},
                          {"shared/data/alpha.wtd", "shared terrain"},
                          {"fms/MissionA/fm.ini", "[General]\nEnableDualStow=True\n"},
                          {"fms/MissionB/data/anim/door.asa", "own door"},
                          {"fms/Notes/notes.txt", "no level here"},
                          {"data/Stream.tpa", "a copy of the game"},
                          {"doc/manual.txt", "documentation"}} +
                        Without(Level("fms/MissionA/data/", "alpha"), "fms/MissionA/data/alpha.wtd") + Level("fms/MissionB/data/", "beta") +
                        Level("fms/MissionC/data/", "gamma", 99));
    const auto before = Snapshot(tree);

    std::vector<std::string> notes;
    const std::vector<LinkCandidate> candidates = FindLinkCandidates(tree, notes);
    CHECK(candidates.size() == 3);
    for (const LinkCandidate& c : candidates) {
        CHECK(SamePlace(c.link.root, tree) && c.link.active_fm == "fms/" + c.link.name);
        CHECK(std::find(c.link.scope.begin(), c.link.scope.end(), "shared") != c.link.scope.end() &&
              std::find(c.link.scope.begin(), c.link.scope.end(), "over") != c.link.scope.end() &&
              std::find(c.link.scope.begin(), c.link.scope.end(), "tp_mod.ini") != c.link.scope.end());
    }

    const LinkResult r = LinkLevelFolder(tree, user, {});
    CHECK(r.ok() && r.linked == std::vector<std::string>({"MissionA", "MissionB"}) && Logged(r.log, "fms/MissionC"));
    CHECK(Snapshot(tree) == before);
    const std::vector<LevelPackage> listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 2);
    for (const LevelPackage& p : listed) {
        CHECK(p.linked && p.layout == "mod_tree" && p.scenes.size() == 1 && p.scenes[0].playable());
        CHECK(p.scenes[0].file == (p.id == "MissionA" ? "alpha.scn" : "beta.scn"));
    }

    CHECK(Activate(user, "MissionA", "fms/MissionA/data/alpha.scn"));
    CHECK(Found("alpha.wtd") == "shared terrain" && Found("anim/door.asa") == "door over all" && Found("anim/gate.asa") == "shared gate");
    CHECK(Found("beta.grf") == "-" && Found("gamma.scn") == "-" && Found("Stream.tpa") == "-" && Setting("dual_stow") == "1");
    CHECK(Activate(user, "MissionB", "fms/MissionB/data/beta.scn"));
    CHECK(Found("beta.wtd") == "terrain of beta" && Found("anim/door.asa") == "door over all" && Found("anim/gate.asa") == "shared gate");
    CHECK(Found("alpha.scn") == "-" && Setting("dual_stow") == "-");
    jpt_custom_level_deactivate();
    CHECK(LinkLevelFolder(tree, user, {}).status == ImportStatus::kAlreadyLinked && Snapshot(tree) == before);
}

// A folder changed, moved, relocated and unlinked: what the launcher keeps follows it, nothing in it is ever written or removed.
void TestRescanRelocateUnlink() {
    const fs::path user = Fresh("moves user"), place = Fresh("moves"), saves = user / "saves" / JPT_CUSTOM_LEVELS_DIR;
    WriteTree(place / "Level Pack", Level("", "solo"));
    const LinkResult r = LinkLevelFolder(place / "Level Pack", user, {});
    CHECK(r.ok() && r.linked == std::vector<std::string>{"Level Pack"});
    const std::string id = "Level Pack";
    const fs::path records = CustomLevelsDir(user) / id;
    Write(saves / id / "SAVEGAME.000", "a save");

    // A file removed in the folder is seen at the next listing and before Play; put back, the level plays again.
    ListLevelPackages(user, {});
    ListingStats cached, changed, rescanned;
    CHECK(ListLevelPackages(user, {}, &cached).size() == 1 && cached.cached == 1 && cached.analysed == 0);
    fs::rename(place / "Level Pack" / "solo.wtd", place / "solo.wtd");
    std::vector<LevelPackage> listed = ListLevelPackages(user, {}, &changed);
    CHECK(changed.analysed == 1 && listed.size() == 1 && !listed[0].scenes[0].playable());
    CHECK(!CheckBeforePlay(user, {}, id, "solo.scn").ok);
    fs::rename(place / "solo.wtd", place / "Level Pack" / "solo.wtd");
    CHECK(CheckBeforePlay(user, {}, id, "solo.scn").ok);

    // Rescan drops what is kept, so the next listing reads the folder again.
    CHECK(RescanLevelPackage(user, id) && !fs::exists(records / ".runtime" / "listing.txt"));
    ListLevelPackages(user, {}, &rescanned);
    CHECK(rescanned.analysed == 1 && fs::exists(records / ".runtime" / "listing.txt"));
    CHECK(!RescanLevelPackage(user, "..") && !RescanLevelPackage(user, "absent"));

    // Gone: kept, shown as it was, never played, saves untouched.
    fs::rename(place / "Level Pack", place / "Moved Pack");
    listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 1 && listed[0].linked && !listed[0].available && listed[0].scenes.size() == 1 && listed[0].scenes[0].file == "solo.scn");
    const PlayCheck gone = CheckBeforePlay(user, {}, id, "solo.scn");
    CHECK(!gone.ok && gone.reason == ImportStatus::kFolderUnavailable && !Activate(user, id, "solo.scn"));
    CHECK(Read(saves / id / "SAVEGAME.000") == "a save");

    // Relocated: only to a folder with the same levels, never to one linked already or to the port's own folders.
    std::string error;
    WriteTree(place / "Other", Level("", "other"));
    CHECK(!RelocateLevelPackage(user, id, place / "Other", error) && error.find("levels") != std::string::npos);
    CHECK(!RelocateLevelPackage(user, id, place / "nowhere", error) && !RelocateLevelPackage(user, "absent", place / "Moved Pack", error));
    CHECK(!RelocateLevelPackage(user, id, user, error));
    const auto moved = Snapshot(place / "Moved Pack");
    CHECK(RelocateLevelPackage(user, id, place / "Moved Pack", error));
    listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 1 && listed[0].available && listed[0].id == id && listed[0].scenes[0].playable() && SamePlace(listed[0].dir, place / "Moved Pack"));
    CHECK(CheckBeforePlay(user, {}, id, "solo.scn").ok && Read(saves / id / "SAVEGAME.000") == "a save");
    CHECK(Snapshot(place / "Moved Pack") == moved);

    // Unlink: the records go, the folder and the saves stay. A records folder with anything else in it is left alone.
    Write(records / "notes.txt", "mine");
    CHECK(!UnlinkLevelPackage(user, id, error) && fs::exists(records / JPT_CUSTOM_LEVEL_LINK_FILE));
    fs::remove(records / "notes.txt");
    for (const char* bad : {"", "..", "absent", "../saves"}) CHECK(!UnlinkLevelPackage(user, bad, error));
    CHECK(UnlinkLevelPackage(user, id, error) && !fs::exists(records));
    CHECK(Snapshot(place / "Moved Pack") == moved && Read(saves / id / "SAVEGAME.000") == "a save" && ListLevelPackages(user, {}).empty());
}

// Folders that are not linked, and paths the game could not open.
void TestLinkRefusals() {
    const fs::path user = Fresh("refusals user"), game = Fresh("refusals game") / "versions" / "en-116" / "data";
    fs::create_directories(game);
    CHECK(LinkLevelFolder(g_scratch / "nowhere", user, {}).status == ImportStatus::kUnreadable);
    WriteTree(Fresh("empty") / "notes", {{"notes.txt", "no level"}});
    CHECK(LinkLevelFolder(g_scratch / "empty", user, {}).status == ImportStatus::kNoLevel);
    WriteTree(CustomLevelsDir(user) / "Inside", Level("", "inside"));
    CHECK(LinkLevelFolder(CustomLevelsDir(user) / "Inside", user, {}).status == ImportStatus::kUnsafe);
    CHECK(LinkLevelFolder(user, user, {}).status == ImportStatus::kUnsafe);
    WriteTree(game / "own", Level("", "own"));
    CHECK(LinkLevelFolder(game / "own", user, {}, {}, game).status == ImportStatus::kUnsafe);
    const fs::path lacking = Fresh("lacking");
    WriteTree(lacking, Without(Level("", "half"), "half.wtd"));
    CHECK(LinkLevelFolder(lacking, user, {}).status == ImportStatus::kMissingFiles);
    const fs::path newer = Fresh("newer");
    WriteTree(newer, Level("", "later", 99));
    CHECK(LinkLevelFolder(newer, user, {}).status == ImportStatus::kUnsupportedFormat);
    std::error_code ec;
    CHECK(!fs::exists(CustomLevelsDir(user) / "half") && !fs::exists(CustomLevelsDir(user) / "later"));

    // Spaces and letters beyond ASCII: linked wherever the game can open the path.
    const fs::path accents = Fresh("A\xC3\xA7\xC3\xA3o levels");
    WriteTree(accents, Level("", "acao"));
    const LinkResult spelled = LinkLevelFolder(accents, user, {});
    CHECK(GameCanOpen(accents) ? spelled.ok() : spelled.status == ImportStatus::kPathNotUsable);
    const fs::path symbols = Fresh("\xE2\x9C\x93 \xE6\x97\xA5\xE6\x9C\xAC");
    WriteTree(symbols, Level("", "marks"));
    const LinkResult marked = LinkLevelFolder(symbols, user, {});
    CHECK(GameCanOpen(symbols) ? marked.ok() : marked.status == ImportStatus::kPathNotUsable && !Logged(marked.log, "result: linked"));
#ifdef _WIN32
    // The game opens a level's files by paths of at most 250 characters.
    if (g_scratch.native().size() < 200) {
        const fs::path deep = Fresh(std::string(246 - (g_scratch.native().size() + 1), 'd'));
        WriteTree(deep, Level("", "f"));
        CHECK(LinkLevelFolder(deep, user, {}).status == ImportStatus::kPathNotUsable);
    }
#endif

    // A link inside the folder is never followed (when this account may make one).
    const fs::path linking = Fresh("linking");
    WriteTree(linking, Level("", "linked"));
    fs::create_directory_symlink(accents, linking / "elsewhere", ec);
    if (!ec) CHECK(LinkLevelFolder(linking, user, {}).status == ImportStatus::kUnsafe);
}

void TestLinkRecord() {
    PackageLink link;
    link.root = g_scratch / "a folder";
    link.scope = {"fms/One", "shared", "tp_mod.ini"};
    link.active_fm = "fms/One";
    link.name = "One";
    link.identity = std::string(64, 'a');
    PackageLink back;
    CHECK(ParsePackageLink(PackageLinkText(link), back) && back.root == link.root && back.scope == link.scope && back.active_fm == link.active_fm &&
          back.name == link.name && back.identity == link.identity);
    CHECK(SameLink(link, back));
    back.scope.pop_back();
    CHECK(!SameLink(link, back));
    for (const char* text : {"", "trespasser-link 2\npath /x\n", "trespasser-link 1\n", "trespasser-link 1\npath relative\n",
                             "trespasser-link 1\npath /x\nscope ../y\n", "trespasser-link 1\npath /x\nfm /abs\n"})
        CHECK(!ParsePackageLink(text, back));
    link.name = "two\nlines";
    CHECK(!WritePackageLink(Fresh("record"), link));
    CHECK(LinkIdentity({{"a/X.scn", 1}, {"b.grf", 2}}) == LinkIdentity({{"A/x.SCN", 9}}) && LinkIdentity({{"a.scn", 1}}) != LinkIdentity({{"b.scn", 1}}));
}

} // namespace

void RunLinkTests(const fs::path& scratch, int& checks, int& failures) {
    g_scratch = scratch / "link";
    std::error_code ec;
    fs::create_directories(g_scratch, ec);
    TestLinkRecord();
    TestLinkFolder();
    TestLinkedSession();
    TestModTree();
    TestRescanRelocateUnlink();
    TestLinkRefusals();
    checks += g_checks;
    failures += g_failures;
}
