// Custom level packages read by their own files: fm.ini, tp_mod.ini, trespasser.ini, tpass.ini and readme files, the folder
// layouts of the community's packages, the manifest and the runtime map, and what the game sees of them. Fixtures are made here,
// shaped like real packages; none of their data is used.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "jpt_config.h"
#include "jpt_custom_level.h"
#include "jpt_level_gate.h"
#include "jpt_level_world.h"
#include "jpt_save.h"
#include "jpt_scene.h"
#include "level_analysis.h"
#include "level_fixtures.h"
#include "level_manifest.h"
#include "level_picture.h"
#include "tpa_merge.h"
#include "updater/json.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace jpt;
using namespace fixtures;

static int g_checks, g_failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

using Files = std::vector<std::pair<std::string, std::string>>;

static fs::path g_scratch;

static std::string Words(uint32_t count, uint32_t from) {
    std::string out;
    for (uint32_t i = 0; i < count; ++i) out += U32(from + i);
    return out;
}

struct SceneShape {
    uint32_t active = 3, listed = 5, meshes = 2, swaps = 0, joints = 2, mask = 0, script = 0;
    bool second = false;
    uint32_t stow = 1;
};

static std::string RenderSection(uint32_t version, const SceneShape& shape) {
    const bool ce = version >= 25;
    std::string out = U32(ce ? 4 : 3) + U32(1) + Words(15, 100) + Words(6, 200) + U32(2) + Words(10, 300) + F32(0.25f) + F32(0.5f);
    if (ce) out += U32(0xD7E6FF) + U32(shape.mask) + std::string((shape.mask & 1 ? 0x80 : 0) + (shape.mask & 2 ? 0x100 : 0), 'c');
    out += Words(5, 400) + U32(ce ? 2 : 1);
    if (ce) out += U32(0);
    out += F32(0.0f) + F32(0.055f) + F32(0.95f) + F32(0.75f) + F32(1000.0f);
    out += F32(0.0f) + F32(ce ? 1.0f : 65536.0f) + F32(ce ? 3.0f : 196608.0f) + F32(ce ? 1.5f : 98304.0f) + F32(7.0f) + F32(8.0f);
    out += Words(5, 500);
    for (uint32_t i = 0; i < shape.meshes; ++i) {
        out += Words(7, 600 + i * 10);
        if (ce) out += U32(shape.swaps) + Words(shape.swaps * 2, 700);
    }
    return out;
}

static std::string AiSection(uint32_t version, const SceneShape& shape) {
    std::string out = Words(5, 800);
    if (version >= 25) out += U32(shape.active) + Words(shape.active, 900);
    else out += Words(std::min(shape.active, 10u), 900) + std::string(4 * (10 - std::min(shape.active, 10u)), '\0');
    return out + U32(shape.listed) + Words(shape.listed, 1000);
}

static std::string Mark(uint32_t version, const SceneShape& shape) {
    if (version < 26) return {};
    return shape.script ? U32(1) + U32(shape.script) + std::string(shape.script, 's') : U32(0);
}

// The second stowed object's placement (CE) lies in the middle of the player's own values.
static std::string PlayerSection(uint32_t version, const SceneShape& shape, bool second) {
    std::string own(236, 'p');
    own[7] = char((second ? 0x80 : 0) | shape.stow);
    if (second) own.insert(120, std::string(28, 'q'));
    return Words(9, 1100) + Mark(version, shape) + Words(9, 1200) + U32(shape.joints) + std::string(48 * shape.joints, 'j') + own;
}

// A scene of `version` as the game or Trespasser CE saves it; `marks`: the one after each object's common part is there.
static std::string SceneOf(const std::string& world, uint32_t version, const SceneShape& shape = {}) {
    Sections sections = {{"Header", HeaderSection(world, version)}, {"4C56C6CC", RenderSection(version, shape)}, {"A9AC56AE", AiSection(version, shape)},
                         {"9FB57F53", PlayerSection(version, shape, shape.second)}, {"00A1B2C3", Words(9, 1300) + Mark(version, shape) + Words(4, 1400)}};
    if (version >= 25) sections.push_back({"4B642136", U32(1) + U32(0xFFFFFFFF) + U32(0)}), sections.push_back({"190DAD34", U32(0)});
    if (version >= 26) sections.push_back({"1C1CA6D4", std::string(16, '\0')});
    return Container(sections);
}

static std::string Scene(const std::string& world, uint32_t version = 22) { return SceneOf(world, version); }

struct Sample {
    uint32_t id;
    std::string payload;
};

// Samples without captions; `collisions` are the impact sound rows of Effects.tpa.
static std::string Tpa(const std::vector<Sample>& samples, const std::vector<std::string>& collisions = {}) {
    const uint32_t count = uint32_t(samples.size());
    std::string directory, records;
    for (const Sample& s : samples) {
        const std::string descriptor = std::string("WBOR") + U32(110) + U32(36) + std::string(24, 'd');
        directory += U32(s.id) + U32(0x18 + count * 0x38 + uint32_t(records.size())) + U32(uint32_t(36 + s.payload.size())) + U32(0) + U32(0) + descriptor;
        records += descriptor + s.payload;
    }
    std::string rows;
    for (const std::string& row : collisions) rows += row + std::string(0x98 - row.size(), '\0');
    const uint32_t end = 0x18 + uint32_t(directory.size() + records.size());
    return U32(0x150) + U32(count) + U32(0x18) + U32(uint32_t(collisions.size())) + U32(end) + U32(count) + directory + records + rows;
}

static std::string Tga(int width, int height, int bits, bool packed) {
    std::string out("\0\0", 2);
    out += char(packed ? 10 : 2);
    out += std::string(9, '\0');
    out += char(width & 255), out += char(width >> 8), out += char(height & 255), out += char(height >> 8), out += char(bits), out += '\0';
    for (int y = 0; y < height; ++y) {
        // Rows are written from the bottom: the lower half is blue, the upper half red.
        const std::string pixel = y < height / 2 ? std::string("\xFF\0\0\0", 4) : std::string("\0\0\xFF\0", 4);
        for (int x = 0; x < width;) {
            const int run = packed ? std::min(128, width - x) : 1;
            if (packed) out += char(0x80 | (run - 1));
            out += bits == 16 ? (y < height / 2 ? std::string("\x1F\0", 2) : std::string("\0\x7C", 2)) : pixel.substr(0, size_t(bits / 8));
            x += run;
        }
    }
    return out;
}

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

static Files Level(const std::string& folder, const std::string& stem) {
    return {{folder + stem + ".scn", Scene(stem + ".grf")}, {folder + stem + ".grf", World(stem)}, {folder + stem + "-130.pid", Set(stem)},
            {folder + stem + "-130.swp", "swap"}, {folder + stem + ".wtd", "terrain of " + stem}};
}

static Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static PackageAnalysis Analyze(const Files& files) {
    PackageSource source;
    for (const auto& [path, bytes] : files) source.files.push_back({path, bytes.size()});
    const auto find = [&files](const std::string& path) -> const std::string* {
        for (const auto& [name, bytes] : files)
            if (name == path) return &bytes;
        return nullptr;
    };
    source.read = [find](const std::string& path, uint64_t limit, std::string& bytes) {
        const std::string* found = find(path);
        if (!found || found->size() > limit) return false;
        bytes = *found;
        return true;
    };
    source.head = [find](const std::string& path, size_t count, std::string& bytes) {
        const std::string* found = find(path);
        if (found) bytes = found->substr(0, count);
        return found != nullptr;
    };
    return AnalyzePackage(source);
}

static std::string RuntimeSource(const PackageAnalysis& a, const std::string& target) {
    for (const RuntimeFile& file : a.runtime)
        if (file.target == target) return file.source;
    return "-";
}

static bool Warned(const PackageAnalysis& a, const char* code) {
    return std::any_of(a.warnings.begin(), a.warnings.end(), [code](const Warning& w) { return w.code == code; });
}

static bool Acted(const PackageAnalysis& a, const char* kind, const std::string& source, Origin origin) {
    return std::any_of(a.actions.begin(), a.actions.end(), [&](const InstallAction& i) { return i.kind == kind && i.source == source && i.origin == origin && !i.reason.empty(); });
}

static std::string Override(const PackageAnalysis& a, const char* key) {
    for (const SessionOverride& o : a.overrides)
        if (o.key == key) return o.value;
    return "-";
}

static std::string UseOf(const PackageAnalysis& a, const char* key) {
    for (const ConfigSetting& s : a.settings)
        if (s.entry.key == key) return SettingUseName(s.meaning.use);
    return "-";
}

static const char kCausewayIni[] =
    "[General]\r\nStartLevel=Causeway~HD.scn\r\nNewGameMovie=0\r\nWinMovie=0\r\nStreamTPA+=CWStream.tpa\r\nEffectsTPA+=CWEffects.tpa\r\n"
    "AmbientTPA+=CWAmbient.tpa\r\n\r\nDisableSimpleLevelRestart=True\r\nEnableDualStow=True\r\nEnableQuickSaveKey=True\r\n\r\n[DisplayDX9]\r\nUseDX9=True\r\n"
    "PixelShaderBump=True\r\nScaleModeMenus=3\r\nEnhancedSky=True\r\n\r\n[Causeway~HD.scn]\r\nLoadingImage=menu/li_causewayHD.tga\r\n\r\n[Render]\r\n"
    "32bitHardwareTextures=True\r\nForceNoBlendTrans=False\r\nForceMaxObjectDetail=False\r\n";

static Files Causeway() {
    const std::string mod = "Causeway~HD/";
    return Level(mod + "data/", "Causeway~HD") +
           Files{{mod + "fm.ini", kCausewayIni}, {mod + "CWStream.tpa", Tpa({{1, "stream"}})}, {mod + "CWEffects.tpa", Tpa({{2, "effects"}})},
                 {mod + "CWAmbient.tpa", Tpa({{3, "ambient"}})}, {mod + "data/anim/anim_hsc.asa", "hsc"}, {mod + "data/Causeway~HD.sky", "sky"},
                 {mod + "data/menu/li_causewayHD.tga", Tga(64, 36, 24, true)}, {mod + "data/texhw/map/a.dds", "dds"},
                 {"Causeway~HD_readme.txt", "2. Installation\n\n    Recommended CE packages to play Causeway~HD in are:\n\t*Trespasser CD installation with CE patch\n\n"
                  "   i. Place the \"Causeway~HD\" folder in the \"mods\" folder of the Tres CE directory\n"}};
}

static std::string ReadyCausewayIni() {
    std::string ini = kCausewayIni;
    const std::string feature = "DisableSimpleLevelRestart=True\r\n";
    ini.erase(ini.find(feature), feature.size());
    return ini;
}

// Causeway without what this port does not have (its sky, its texhw pictures, a CE setting): what an import installs.
static Files ReadyCauseway() {
    Files files;
    for (auto& [path, bytes] : Causeway()) {
        if (path.find("/texhw/") != std::string::npos || PathType(path) == ".sky") continue;
        files.push_back({path, path == "Causeway~HD/fm.ini" ? ReadyCausewayIni() : bytes});
    }
    return files;
}

static void TestIniParser() {
    const IniFile ini = ParseIni("; comment\n[General]\nStartLevel = a.scn\nStreamTPA+=One.tpa\nstreamtpa += Two.tpa\nName=\"quoted value\"\nbroken line\n=none\n"
                                 "Path=a=b\n[other]\nstartlevel=b.scn\n# also a comment\n[unclosed\n");
    CHECK(ini.entries.size() == 6 && ini.malformed == 3);
    CHECK(ini.Find("GENERAL", "startlevel") && ini.Find("general", "STARTLEVEL")->value == "a.scn");
    CHECK(ini.entries[1].additive && ini.entries[1].key == "StreamTPA" && ini.entries[1].value == "One.tpa");
    CHECK(ini.entries[2].additive && ini.entries[2].key == "streamtpa" && ini.entries[2].value == "Two.tpa");
    CHECK(!ini.Find("General", "StreamTPA") && ini.Find("General", "Name")->value == "quoted value" && ini.Find("General", "Path")->value == "a=b");
    CHECK(ini.Find("other", "StartLevel")->value == "b.scn" && ini.entries[0].line == 3);
    bool on = false;
    for (const char* text : {"True", "true", "1", "-1", "yes", "0x10"}) CHECK(ParseIniBool(text, on) && on);
    for (const char* text : {"False", "0", "no"}) CHECK(ParseIniBool(text, on) && !on);
    CHECK(!ParseIniBool("maybe", on) && !ParseIniBool("", on) && ParseIni("").entries.empty());
}

// What ATX and Trespasser CE read of the MAPS folder: a level's loading picture, or the one of all levels, and its sounds.
static void TestAtxFiles() {
    const auto required = [](const PackageAnalysis& a, const char* name) { return std::find(a.required.begin(), a.required.end(), name) != a.required.end(); };
    const PackageAnalysis a = Analyze(Level("Pack/data/", "x") + Files{{"Pack/MAPS/LBG/X.tga", Tga(8, 6, 24, false)}, {"Pack/MAPS/LBG/default.tga", Tga(8, 6, 24, true)},
                                                                       {"Pack/MAPS/LVL/x.ini", "[AdditiveTPAFilenames]\r\nStream=own\\s.tpa\r\nEffects=gone.tpa\r\nAmbient=..\\..\\a.tpa\r\n"},
                                                                       {"Pack/MAPS/LVL/own/s.tpa", Tpa({{7, "x"}})}, {"Pack/MAPS/LVL/example.ini", "[AdditiveTPAFilenames]\nStream=no.tpa\n"}});
    CHECK(a.images.size() == 1 && a.images[0].path == "Pack/MAPS/LBG/X.tga" && a.images[0].scene == "x.scn" && required(a, "ATX_LOADING_PICTURE"));
    CHECK(a.audio.size() == 2 && a.audio[0].state == "ready" && a.audio[0].path == "Pack/MAPS/LVL/own/s.tpa" && a.audio[0].additive && a.audio[0].scene == "x.scn");
    CHECK(a.audio[1].state == "unsafe" && required(a, "ATX_LEVEL_SOUNDS"));
    const PackageAnalysis loose = Analyze(Level("", "y") + Files{{"MAPS/LBG/default.tga", Tga(8, 6, 24, false)}});
    CHECK(loose.images.size() == 1 && loose.images[0].path == "MAPS/LBG/default.tga" && loose.audio.empty());
    const PackageAnalysis set = Analyze(Level("M/data/", "z") + Files{{"M/fm.ini", "[General]\nStartLevel=z.scn\n[z.scn]\nLoadingImage=p.tga\n"}, {"M/p.tga", Tga(8, 6, 24, false)}, {"M/MAPS/LBG/z.tga", Tga(8, 6, 24, false)}});
    CHECK(set.images.size() == 1 && set.images[0].path == "M/p.tga");
    CHECK(Analyze(Level("", "y") + Files{{"MAPS/LBG/other.tga", Tga(8, 6, 24, false)}, {"MAPS/LBG/y.tga", "broken"}}).images.empty());
}

static void TestLooseLevels() {
    const PackageAnalysis complex = Analyze(
        Level("", "complex") + Files{{"Rain_storm.asa", "rain"}, {"complex readme.txt",
                                      "Extract all the files into the data directory of the tres cd folder: C:\\tres cd\\data.\r\n"
                                      "Move \"rain_storm.asa\" into the \"anim\" directory: C:\\tres cd\\data\\anim.\r\n"}});
    CHECK(complex.layout == "loose" && complex.root.empty() && complex.start_level == "complex.scn" && complex.warnings.empty());
    CHECK(RuntimeSource(complex, "data/anim/Rain_storm.asa") == "Rain_storm.asa" && RuntimeSource(complex, "data/Rain_storm.asa") == "-");
    CHECK(Acted(complex, "move", "Rain_storm.asa", Origin::kReadme) && Acted(complex, "keep", "", Origin::kReadme));
    CHECK(complex.need("Trespasser CE") == Need::kNone && complex.need("ATX") == Need::kNone && complex.directories == std::vector<std::string>{"anim"});

    const PackageAnalysis unread = Analyze(Level("", "complex") + Files{{"Rain_storm.asa", "rain"}});
    CHECK(RuntimeSource(unread, "data/anim/Rain_storm.asa") == "Rain_storm.asa" && Acted(unread, "move", "Rain_storm.asa", Origin::kHeuristic));

    const PackageAnalysis jungle = Analyze(
        Level("", "LostJungle") + Files{{"LOSTJUNGLE_MAP.bmp", "bmp"}, {"LJreadme.txt",
                                         "1. Installation\r\n\r\n   i. Place the level files* into your \"\\data\" directory\r\n\r\nLevel files*  LostJungle-130.pid\r\n\t   LJ.wtd\r\n\r\n"
                                         "  ii. (Optional) IF ATX IS INSTALLED: Place \"LOSTJUNGLE_MAP.bmp\" in the \"\\MAPS\" directory\r\n\r\n"
                                         "   i. Lost Jungle (LJ) is optimized for use with Big Red's ATX Patch. However, ATX is not required.\r\n"
                                         "  ii. The LJ level cannot be restarted via the menu; to begin a new game you must close Trespasser.\r\n"}});
    CHECK(RuntimeSource(jungle, "maps/LOSTJUNGLE_MAP.bmp") == "LOSTJUNGLE_MAP.bmp" && RuntimeSource(jungle, "data/LOSTJUNGLE_MAP.bmp") == "-");
    CHECK(jungle.need("ATX") == Need::kOptional && Warned(jungle, "no_restart") && !jungle.config_warnings());
    CHECK(std::any_of(jungle.runtime.begin(), jungle.runtime.end(), [](const RuntimeFile& f) { return f.target == "maps/LOSTJUNGLE_MAP.bmp" && f.active; }));

    const PackageAnalysis misty = Analyze(
        Level("", "MistyJungle") + Files{{"anim/anim_MayaDoor.asa", "door"}, {"texhw/map/camo.dds", "dds"}, {"MistyJungle_readme.txt",
                                          "   Place the level files + the anim and texhw folders into your \"\\data\" directory\r\n\r\n\tMistyJungle-130.pid\r\n\r\n"
                                          "**Due to higher poly models, Misty Jungle can only be run in Trespasser CE**\r\n\r\n   Lee Arbuco for Trespasser CE\r\n"}});
    CHECK(RuntimeSource(misty, "data/anim/anim_MayaDoor.asa") == "anim/anim_MayaDoor.asa" && RuntimeSource(misty, "data/texhw/map/camo.dds") == "texhw/map/camo.dds");
    CHECK(Acted(misty, "keep", "anim", Origin::kReadme) && Acted(misty, "keep", "texhw", Origin::kReadme) && misty.need("Trespasser CE") == Need::kRequired);

    const PackageAnalysis scene = Analyze(Level("", "TstScnRld") + Files{{"TstScnRld_readme.txt", "Place the level files* into your \"\\data\" directory"}});
    CHECK(scene.dependencies.empty() && scene.warnings.empty() && RuntimeSource(scene, "data/TstScnRld.wtd") == "TstScnRld.wtd");
    CHECK(Analyze(Level("Parent/data/", "x") + Files{{"Parent/MAPS/x.bmp", "b"}}).layout == "data_folder");
    CHECK(RuntimeSource(Analyze(Level("Parent/data/", "x") + Files{{"Parent/MAPS/x.bmp", "b"}}), "maps/x.bmp") == "Parent/MAPS/x.bmp");
    CHECK(!Analyze({{"readme.txt", "nothing"}}).playable() && Analyze({{"fake.scn", "xx"}}).layout == "loose" && !Analyze({{"fake.scn", "xx"}}).playable());
}

static void TestReadmeRules() {
    const std::vector<std::string> paths = {"a.scn", "storm.asa", "one/same.asa", "two/same.asa", "map.bmp", "settings/trespasser.mine.ini", "engine.ini"};
    const std::vector<std::string> folders = {"one", "two", "settings"};
    const auto read = [&](const char* text) { return ReadReadme(text, paths, folders); };
    CHECK(read("Move storm.asa into the anim folder.").rules.size() == 1);
    CHECK(read("Copy the file storm.asa to data\\anim").rules.size() == 1 && read("Put storm.asa inside your anim dir!").rules.size() == 1);
    CHECK(read("Move same.asa into the anim directory.").rules.empty() && read("Move same.asa into the anim directory.").ambiguous.size() == 1);
    CHECK(read("Move storm.asa into the system32 folder.").rules.empty() && read("Move storm.asa into the system32 folder.").ambiguous.size() == 1);
    CHECK(read("Move storm.asa into the main Trespasser directory.").ambiguous.size() == 1);
    CHECK(read("Move map.bmp into the anim directory.").rules.empty() && read("Move map.bmp into the anim directory.").ambiguous.size() == 1);
    CHECK(read("Move gone.asa into the anim directory.").absent == std::vector<std::string>{"gone.asa"});
    CHECK(read("Move ..\\..\\evil.asa into the anim directory.").rules.empty());
    CHECK(read("Copy storm.asa into C:\\Windows\\System32").rules.empty());
    for (const char* text : {"Run setup.exe to install.", "Execute 'del /s C:\\*' in the data directory.", "powershell -c Move-Item storm.asa anim", "", "storm.asa anim data maps",
                             "The rain storm moves into the valley, to a place in the data of history."})
        CHECK(read(text).rules.empty());
    CHECK(read("Replace trespasser.ini with trespasser.mine.ini in the game folder").configs == std::vector<std::string>{"settings/trespasser.mine.ini"});
    CHECK(read("Find 'trespasser.mine.ini' in the folder and move it to the main Trespasser directory, then rename that to 'trespasser.ini'").configs.size() == 1);
    CHECK(read("Copy engine.ini into the data directory").rules.empty());
    CHECK(read("This level requires the ATX patch.").atx == Need::kRequired && read("ATX is not required.").atx == Need::kOptional && read("Thanks to ATX").atx == Need::kMentioned);
    CHECK(read("Thanks to Lee for Trespasser CE").ce == Need::kMentioned && read("Use the CE patch").ce == Need::kMentioned && read("nice spaces").ce == Need::kNone);
    CHECK(read("Trespasser CE is recommended.").ce == Need::kRecommended && read("It needs Trespasser CE.").ce == Need::kRequired);
    const std::string flood(400000, 'x');
    CHECK(ReadReadme(flood + " Move storm.asa into the anim folder.", paths, folders).rules.empty());
}

static void TestFanMissions() {
    const PackageAnalysis causeway = Analyze(Causeway());
    CHECK(causeway.layout == "fan_mission" && causeway.root == "Causeway~HD" && causeway.start_level == "Causeway~HD/data/Causeway~HD.scn" && causeway.levels[0].start);
    CHECK(causeway.audio.size() == 3 && std::all_of(causeway.audio.begin(), causeway.audio.end(), [](const AudioPack& p) { return p.additive && p.state == "ready" && p.scene.empty(); }));
    CHECK(causeway.audio[0].path == "Causeway~HD/CWStream.tpa" && causeway.images.size() == 1 && causeway.images[0].path == "Causeway~HD/data/menu/li_causewayHD.tga");
    CHECK(Override(causeway, "dual_stow") == "1" && Override(causeway, "enhanced_sky") == "1" && causeway.overrides.size() == 2 && causeway.skip_win_movie);
    CHECK(UseOf(causeway, "StartLevel") == "applied" && UseOf(causeway, "UseDX9") == "user_choice" && UseOf(causeway, "32bitHardwareTextures") == "obsolete");
    CHECK(UseOf(causeway, "EnableQuickSaveKey") == "built_in" && UseOf(causeway, "DisableSimpleLevelRestart") == "ineffective" && UseOf(causeway, "ForceMaxObjectDetail") == "recommended");
    CHECK(causeway.need("Trespasser CE") == Need::kRecommended && causeway.warnings.empty());

    const PackageAnalysis escape = Analyze(
        Level("EFTLW/data/", "TCC~Caldera") + Files{{"EFTLW/fm.ini", "[General]\nStartLevel=TCC~Caldera.scn\nEffectsTPA+=EffectsJP.tpa\n[TCC~Caldera.scn]\nLoadingImage=menu/li_caldera_0\n"},
                                                    {"EFTLW/EffectsJP.tpa", Tpa({{9, "x"}})}, {"EFTLW/data/menu/li_caldera_0.tga", Tga(8, 6, 24, false)},
                                                    {"tp_mod.ini", "[Paths]\nFMPath=mods\n[FM]\nActiveFM=EFTLW\n;ActiveFM=(mod folder name here)\n"}});
    CHECK(escape.layout == "mod_tree" && escape.root == "EFTLW" && Acted(escape, "root", "EFTLW", Origin::kTpModIni));
    CHECK(escape.images.size() == 1 && escape.images[0].state == "ready" && escape.images[0].path == "EFTLW/data/menu/li_caldera_0.tga");

    const std::string trescom = "TresCom20/FMs/TresCom20/";
    const PackageAnalysis standalone = Analyze(
        Level(trescom + "data/", "TresCom20") +
        Files{{trescom + "fm.ini", "[General]\nStartLevel=TresCom20.scn\nStreamTPA+=TresCom20Stream.tpa\n"}, {trescom + "TresCom20Stream.tpa", Tpa({{5, "x"}})},
              {"TresCom20/tp_mod.ini", "[Paths]\nFMPath=FMs\n[FM]\nActiveFM=TresCom20\n"}, {"TresCom20/data/Stream.tpa", Tpa({{1, "the game's own"}})},
              {"TresCom20/trespasser.ini", "[General]\nEnableDualStow=True\nEnableAiActivityEx=-1\n[DisplayDX9]\nMultiSamplingLevel=16\nWaterRefraction=True\n[SoundOpenAL]\nDevice=Generic Software\n[New]\nMystery=1\n"},
              {"TresCom20/tpass.ini", "[Settings]\nWidth=1440\nUse D3D=1\nKeyMap=0100\nDX9 Adapter=0\n"}});
    CHECK(standalone.layout == "standalone" && standalone.root == "TresCom20/FMs/TresCom20" && Acted(standalone, "ignore", "TresCom20/data", Origin::kTpModIni));
    CHECK(RuntimeSource(standalone, "data/Stream.tpa") == "-" && Override(standalone, "dual_stow") == "1" && Override(standalone, "water_refraction") == "1" && standalone.overrides.size() == 2);
    CHECK(UseOf(standalone, "MultiSamplingLevel") == "user_choice" && UseOf(standalone, "Device") == "obsolete" && UseOf(standalone, "EnableAiActivityEx") == "ineffective");
    CHECK(UseOf(standalone, "Width") == "user_choice" && UseOf(standalone, "DX9 Adapter") == "user_choice" && UseOf(standalone, "Mystery") == "unknown");

    const PackageAnalysis ascent = Analyze(
        Level("AS2R/data/", "as2") + Files{{"AS2R/fm.ini", "[General]\nStartLevel=as2.scn\n"}, {"AS2R/anim/Pterotest00.ASA", "p"}, {"AS2R/animb/Pterotest00.asb", "b"},
                                           {"trespasser.AS2R.ini", "[General]\nEnableDualStow=True\nTryIncompleteInstall=True\n"}, {"Readme_AS2R.txt",
                                            "1.Download the zip and extract the 'AS2R' folder to your mods directory.\n\n2.b. Find 'trespasser.AS2R.ini' in the AS2R folder and move it to the main "
                                            "Trespasser directory, then rename that to 'trespasser.ini'- these steps replace the trespasser ini.\n"}});
    CHECK(ascent.root == "AS2R" && Acted(ascent, "root", "AS2R", Origin::kReadme) && Acted(ascent, "settings", "trespasser.AS2R.ini", Origin::kReadme));
    CHECK(Override(ascent, "dual_stow") == "1" && RuntimeSource(ascent, "data/anim/Pterotest00.ASA") == "AS2R/anim/Pterotest00.ASA");
    CHECK(std::none_of(ascent.runtime.begin(), ascent.runtime.end(), [](const RuntimeFile& f) { return f.source.find(".ini") != std::string::npos || f.source.find(".asb") != std::string::npos; }));

    const PackageAnalysis town = Analyze(Level("TLW/data/", "TLWCITY") + Level("TLW/data/", "TLWSFI") +
                                         Files{{"TLW/fm.ini", "[General]\nStartLevel=TLWCITY.scn\nWinMovie=0\n[TLWCITY.scn]\nAmbientTPA+=AmbientDS.tpa\n"}, {"TLW/AmbientDS.tpa", Tpa({{4, "x"}})}});
    CHECK(town.levels.size() == 2 && town.levels[0].start && !town.levels[1].auxiliary && town.audio.size() == 1 && town.audio[0].scene == "TLWCITY.scn");
    Files castle = Level("D/data/", "Castle01") + Level("D/data/", "Castle02") + Files{{"D/data/Castle01b.scn", Scene("castle01.grf")}, {"D/Spare.scn", Scene("Castle02.grf")}};
    castle.push_back({"D/fm.ini", "[General]\nStartLevel=castle01.SCN\nEffectsTPA+=L.tpa\nStreamTPA+=L.tpa\nAmbientTPA+=L.tpa\n"});
    castle.push_back({"D/L.tpa", Tpa({{4, "x"}})});
    const PackageAnalysis campaign = Analyze(castle);
    CHECK(campaign.audio.size() == 3 && campaign.warnings.empty() && campaign.start_level == "D/data/Castle01.scn");
    size_t auxiliary = 0;
    for (const LevelInfo& level : campaign.levels) auxiliary += level.auxiliary && (level.file == "Castle01b.scn" || level.file == "Spare.scn") && !level.auxiliary_reason.empty();
    CHECK(auxiliary == 2 && campaign.levels.size() == 4);
    const PackageAnalysis retold = Analyze(Level("SNE/data/", "be") + Level("SNE/data/", "it") + Files{{"SNE/Error.scn", Scene("be.grf")}, {"SNE/Stream.tpa", Tpa({{1, "x"}})}});
    CHECK(retold.layout == "data_folder" && RuntimeSource(retold, "data/Stream.tpa") == "SNE/Stream.tpa" && retold.levels.back().file == "Error.scn" && retold.levels.back().auxiliary);
}

static void TestDocumentation() {
    const PackageAnalysis engine = Analyze(Level("Kit/mods/M/data/", "m") + Files{{"Kit/tp_mod.ini", "[FM]\nActiveFM=M\n[Paths]\nFMPath=mods\n"}, {"Kit/doc/tp_mod.ini", "[FM]\nActiveFM=Other\n"},
                                                                         {"Kit/doc/scripting/samples/Spitter.nut", "function f() {}"}, {"Kit/mods/M/fm.ini", "[General]\n"}});
    CHECK(!Warned(engine, "config_several_tp_mod") && std::find(engine.required.begin(), engine.required.end(), "CE_SCRIPTS") == engine.required.end() && engine.root == "Kit/mods/M");
    const PackageAnalysis inside = Analyze(Level("Mission/data/", "m") + Files{{"Mission/tp_mod.ini", "[Paths]\nFMPath=FMs\n[FM]\nActiveFM=Mission\n"}, {"Mission/fm.ini", "[General]\nStartLevel=m.scn\n"}});
    CHECK(!Warned(inside, "config_active_fm_missing") && inside.root == "Mission" && inside.start_level == "Mission/data/m.scn");
    const PackageAnalysis bare = Analyze(Level("data/", "m") + Files{{"tp_mod.ini", "[FM]\nActiveFM=Mission\n"}, {"fm.ini", "[General]\nStartLevel=m.scn\n"}});
    CHECK(!Warned(bare, "config_active_fm_missing") && bare.start_level == "data/m.scn");
    const PackageAnalysis scripted = Analyze(Level("M/data/", "m") + Files{{"M/scripts/Spitter.nut", "function f() {}"}});
    CHECK(std::find(scripted.required.begin(), scripted.required.end(), "CE_SCRIPTS") != scripted.required.end());
}

static void TestConfigProblems() {
    const auto mission = [](const std::string& ini, const Files& more = {}) { return Analyze(Level("M/data/", "m") + Files{{"M/fm.ini", ini}} + more); };
    const PackageAnalysis missing = mission("[General]\nStartLevel=gone.scn\nStreamTPA+=Gone.tpa\nEffectsTPA+=Text.tpa\nAmbientTPA+=..\\..\\x.tpa\n[m.scn]\nLoadingImage=menu/none.tga\n[other.scn]\nLoadingImage=../x.tga\n",
                                            {{"M/Text.tpa", "not a tpa"}});
    CHECK(Warned(missing, "config_start_level_missing") && Warned(missing, "config_tpa_unusable") && Warned(missing, "config_loading_image") && Warned(missing, "config_level_absent"));
    CHECK(missing.audio.size() == 3 && missing.audio[0].state == "missing" && missing.audio[1].state == "not_a_tpa" && missing.audio[2].state == "unsafe" && missing.config_warnings());
    CHECK(std::all_of(missing.audio.begin(), missing.audio.end(), [](const AudioPack& p) { return p.path.empty(); }));
    CHECK(missing.images[0].state == "missing" && missing.images[1].state == "unsafe" && missing.start_level == "M/data/m.scn");
    CHECK(RuntimeMap(missing).find("tpa+") == std::string::npos && RuntimeMap(missing).find("loading") == std::string::npos);
    CHECK(Warned(mission("[General]\nStreamTPA+=a.tpa\nStreamTPA+=A.TPA\n", {{"M/a.tpa", Tpa({{1, "x"}})}}), "config_tpa_twice"));
    CHECK(Warned(mission("[General]\nEnableDualStow=perhaps\n"), "config_invalid_value") && mission("[General]\nEnableDualStow=perhaps\n").overrides.empty());
    CHECK(!Warned(mission("[General]\nAmbientTPA+=\n[Memory]\nMemSizeD3D=1\nMemSizeD3D=2\n"), "config_tpa_unusable"));
    CHECK(!Warned(mission("[Memory]\nExtraTerrainNodes=20\nExtraTerrainNodes=10\nMemSizeD3D=1\nMemSizeD3D=2\n"), "config_twice"));
    CHECK(Warned(mission("[General]\nEnableDualStow=True\nenabledualstow=False\n"), "config_twice") && Override(mission("[General]\nEnableDualStow=True\nenabledualstow=False\n"), "dual_stow") == "1");
    CHECK(mission("[General]\nEffectsTPA=Own.tpa\n", {{"M/Own.tpa", Tpa({{1, "x"}})}}).audio[0].additive == false);
    CHECK(mission("[Game]\nMaxActiveAi=6\n").overrides.size() == 1 && Override(mission("[Game]\nMaxActiveAi=3\n"), "active_ai") == "original");
    CHECK(mission("[m.scn]\nLoadingImage=menu/broken.tga\n", {{"M/data/menu/broken.tga", "short"}}).images[0].state == "unreadable");

    const auto tree = [](const std::string& ini) { return Analyze(Level("pack/mods/M/data/", "m") + Level("pack/mods/Other/data/", "o") + Files{{"pack/tp_mod.ini", ini}, {"pack/mods/M/fm.ini", "[General]\n"}}); };
    CHECK(tree("[Paths]\nFMPath=mods\n[FM]\nActiveFM=M\n").root == "pack/mods/M" && tree("[FM]\nActiveFM=mods\\M\n").root == "pack/mods/M");
    CHECK(Warned(tree("[Paths]\nFMPath=..\\..\\Windows\n[FM]\nActiveFM=M\n"), "config_unsafe_path") && Warned(tree("[FM]\nActiveFM=C:\\x\n"), "config_unsafe_path"));
    CHECK(Warned(tree("[Paths]\nFMPath=mods\n[FM]\nActiveFM=None\n"), "config_active_fm_missing"));
    const Files layers = Level("mods/M/data/", "m") + Files{{"tp_mod.ini", "[Paths]\nFMPath=mods\nModPaths=extra+gone\nUberModPaths=first\n[FM]\nActiveFM=M\n"}, {"first/data/m.wtd", "uber"},
                                                            {"extra/data/m.wtd", "late"}, {"extra/data/more.wtd", "more"}};
    const PackageAnalysis layered = Analyze(layers);
    CHECK(RuntimeSource(layered, "data/m.wtd") == "first/data/m.wtd" && RuntimeSource(layered, "data/more.wtd") == "extra/data/more.wtd" && Warned(layered, "config_mod_path_missing"));
    CHECK(Warned(Analyze(Level("A/data/", "a") + Level("B/data/", "b") + Files{{"A/fm.ini", ""}, {"B/fm.ini", ""}}), "config_several_fan_missions"));
    CHECK(Warned(Analyze(Level("", "x") + Files{{"X.grf", "other case"}}), "case_collision"));
    PackageAnalysis unsafe = Analyze(Level("", "x"));
    std::string problem;
    const std::vector<PackageFile> known = {{"x.scn"}, {"x.grf"}, {"x-130.pid"}, {"x-130.swp"}, {"x.wtd"}};
    CHECK(RuntimeIsSafe(unsafe, known, problem) && problem.empty());
    unsafe.runtime[0].target = "Map/as2/x.bmp";
    CHECK(RuntimeIsSafe(unsafe, known, problem));
    for (const char* target : {"../x.scn", "data/../../x.scn", "C:/x.scn", "Map/../../x.bmp", "/data/x.scn", "data", "x.scn", "data/x.dll", "Map/x.exe"}) {
        PackageAnalysis bad = unsafe;
        bad.runtime[0].target = target;
        CHECK(!RuntimeIsSafe(bad, known, problem) && problem == target);
    }
    unsafe.runtime[0].source = "elsewhere.scn";
    CHECK(!RuntimeIsSafe(unsafe, known, problem));
}

static void TestPictures() {
    for (const bool packed : {false, true})
        for (const int bits : {16, 24, 32}) {
            const std::string made = MakeLoadingPicture(Tga(640, 480, bits, packed));
            CHECK(made.size() == 18 + 640 * 480 * 2 && made[2] == 2 && made[16] == 16 && IsLoadingPicture(made));
            CHECK(made[18] == 0x1F && made[19] == 0 && made[made.size() - 2] == 0 && made[made.size() - 1] == 0x7C);
        }
    const std::string wide = MakeLoadingPicture(Tga(640, 360, 24, true));
    CHECK(wide[18] == 0 && wide[19] == 0 && wide[18 + 640 * 2 * 60] == 0x1F);
    CHECK(MakeLoadingPicture(Tga(1280, 960, 24, false)) == MakeLoadingPicture(Tga(640, 480, 24, false)));
    std::string cut = Tga(64, 48, 24, true);
    CHECK(!IsLoadingPicture(cut.substr(0, cut.size() - 1)) && !IsLoadingPicture("") && MakeLoadingPicture("text of some length here").empty());
    cut[2] = 1;
    CHECK(!IsLoadingPicture(cut));
    CHECK(!IsLoadingPicture(Tga(0, 48, 24, false)));
}

static int ReadMemory(void* ctx, uint64_t offset, void* buf, size_t n) {
    const std::string& bytes = *static_cast<const std::string*>(ctx);
    if (offset + n > bytes.size()) return 0;
    std::memcpy(buf, bytes.data() + offset, n);
    return 1;
}

static std::string MergedSample(const tpa_merge_t* merge, uint32_t id) {
    const auto u32 = [merge](uint64_t at) {
        unsigned char b[4] = {};
        tpa_merge_read(merge, at, b, 4);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    };
    std::string found = "-";
    for (uint32_t i = 0; i < u32(0x14); ++i) {
        const uint64_t entry = 0x18 + uint64_t(i) * 0x38;
        if (u32(entry) != id) continue;
        std::string record(u32(entry + 8), '\0');
        if (tpa_merge_read(merge, u32(entry + 4), record.data(), record.size()) != long(record.size())) return "unreadable";
        found = record.substr(36);
    }
    return found;
}

static void TestTpaMerge() {
    std::string base = Tpa({{1, "one"}, {2, "two"}, {2, "two"}}, {"row a"}), extra = Tpa({{2, "new two"}, {3, "three"}}, {"row b", "row c"}), more = Tpa({{3, "last three"}, {4, "four"}}), text = "not a tpa at all, but long enough";
    const auto source = [](std::string& bytes) { return tpa_source_t{ReadMemory, &bytes, bytes.size()}; };
    tpa_source_t sources[] = {source(base), source(extra), source(text), source(more)};
    tpa_merge_stats_t stats;
    tpa_merge_t* merge = tpa_merge_open(sources, 4, &stats);
    CHECK(merge && stats.added == 2 && stats.replaced == 2 && stats.refused == 1 && stats.collisions == 2);
    if (!merge) return;
    CHECK(MergedSample(merge, 1) == "one" && MergedSample(merge, 2) == "new two" && MergedSample(merge, 3) == "last three" && MergedSample(merge, 4) == "four" && MergedSample(merge, 5) == "-");
    std::string head(0x18, '\0');
    CHECK(tpa_merge_read(merge, 0, head.data(), head.size()) == 0x18 && head.substr(0, 4) == U32(0x150) && head.substr(0x14, 4) == U32(5) && head.substr(0x0C, 4) == U32(3));
    uint32_t at = 0;
    std::memcpy(&at, head.data() + 0x10, 4);
    std::string rows(3 * 0x98, '\0');
    CHECK(tpa_merge_read(merge, at, rows.data(), rows.size()) == long(rows.size()) && !rows.compare(0, 5, "row a") && !rows.compare(0x98, 5, "row b") && !rows.compare(0x130, 5, "row c"));
    CHECK(tpa_merge_size(merge) == at + rows.size() && tpa_merge_read(merge, tpa_merge_size(merge), head.data(), 4) == 0);
    std::string whole(size_t(tpa_merge_size(merge)), '\0'), pieces;
    CHECK(tpa_merge_read(merge, 0, whole.data(), whole.size()) == long(whole.size()));
    for (size_t done = 0; done < whole.size();) {
        char piece[7];
        const long got = tpa_merge_read(merge, done, piece, sizeof piece);
        if (got <= 0) break;
        pieces.append(piece, size_t(got));
        done += size_t(got);
    }
    CHECK(pieces == whole);
    tpa_merge_close(merge);

    merge = tpa_merge_open(sources, 1, &stats);
    CHECK(merge && tpa_merge_size(merge) == base.size() && MergedSample(merge, 2) == "two" && !stats.added);
    if (merge) {
        std::string same(base.size(), '\0');
        tpa_merge_read(merge, 0, same.data(), same.size());
        CHECK(same == base);
    }
    tpa_merge_close(merge);
    std::string torn = Tpa({{7, "seven"}});
    torn.resize(torn.size() - 2);
    tpa_source_t bad[] = {source(base), source(torn)};
    merge = tpa_merge_open(bad, 2, &stats);
    CHECK(merge && stats.refused == 1 && MergedSample(merge, 7) == "-" && MergedSample(merge, 1) == "one");
    tpa_merge_close(merge);
    CHECK(!tpa_merge_open(&sources[2], 1, &stats) && !tpa_merge_open(sources, 0, &stats));
}

static fs::path Install(const fs::path& user, const char* name, const Files& files, const fs::path& data_dir = {}) {
    const fs::path source = Fresh("source") / name;
    for (const auto& [path, bytes] : files) Write(source / PathFromUtf8(path), bytes);
    const ImportResult result = ImportLevelFolder(source, user, {}, {}, data_dir);
    // Refused by the READY rule: placed as an earlier version installed it, for the runtime's own checks.
    if (result.status == ImportStatus::kNotSupported || result.status == ImportStatus::kMissingFiles) {
        for (const auto& [path, bytes] : files)
            if (IsLevelPackageFile(path)) Write(CustomLevelsDir(user) / name / PathFromUtf8(path), bytes);
        ListLevelPackages(user, {});
        return CustomLevelsDir(user) / name;
    }
    CHECK(result.ok());
    if (!result.ok()) std::printf("  %s: %s %s\n", name, ImportStatusTitle(result.status), result.detail.c_str());
    return CustomLevelsDir(user) / result.package;
}

static bool Activate(const fs::path& user, const char* package, const char* scene) {
    jpt_custom_level_request_t request{};
    char error[256] = "";
    std::snprintf(request.package, sizeof request.package, "%s", package);
    std::snprintf(request.scene, sizeof request.scene, "%s", scene);
    return jpt_custom_level_activate(user.string().c_str(), &request, error, sizeof error) != 0;
}

static std::string Found(const char* relative) {
    char host[1024];
    return jpt_custom_level_find(relative, host, sizeof host) ? Read(fs::path(host)) : std::string("-");
}

static std::string Added(jpt_level_tpa_t kind, int index) {
    char host[1024];
    return jpt_custom_level_tpa(kind, index, host, sizeof host) ? Utf8(fs::path(host).filename()) : std::string("-");
}

static std::string Setting(const char* wanted) {
    const char *key = nullptr, *value = nullptr;
    for (int i = 0; jpt_custom_level_setting(i, &key, &value); ++i)
        if (!std::strcmp(key, wanted)) return value;
    return "-";
}

static void TestManifest() {
    const fs::path user = Fresh("manifest"), dir = Install(user, "Causeway", ReadyCauseway() + Files{{"Causeway~HD/tool.exe", "MZ"}});
    const std::string text = Read(dir / kManifestFile), map = Read(dir / JPT_CUSTOM_LEVEL_RUNTIME_MAP);
    const auto json = trg::update::json::Parse(text);
    CHECK(json && json->Find("schema_version") && json->Find("schema_version")->AsNumber() == kManifestSchema && json->Find("package_id")->AsString() == "Causeway");
    CHECK(json && json->Find("layout_type")->AsString() == "fan_mission" && json->Find("source_type")->AsString() == "folder" && json->Find("source_hash")->AsString().size() == 64);
    for (const char* key : {"levels", "readmes", "configs", "audio", "directories", "dependencies", "session_overrides", "unsupported_settings", "install_actions", "warnings", "refused_files", "runtime"})
        CHECK(json && json->Find(key));
    CHECK(json && json->Find("refused_files")->AsArray().size() == 1 && json->Find("refused_files")->At(0)->AsString() == "Causeway~HD/tool.exe - a program or library");
    CHECK(!fs::exists(dir / "Causeway~HD" / "tool.exe") && Read(dir / "Causeway~HD" / "fm.ini") == ReadyCausewayIni());
    CHECK(map.rfind("trespasser-runtime 1\n", 0) == 0 && map.find("tpa+ stream|*|Causeway~HD/CWStream.tpa\n") != std::string::npos);
    CHECK(map.find(".ini") == std::string::npos && map.find("texhw") == std::string::npos && fs::exists(dir / PathFromUtf8(LoadingPictureFile(0))));
    PackageOrigin origin;
    CHECK(ReadManifestOrigin(text, origin) && origin.refused.size() == 1 && !ReadManifestOrigin("{}", origin) && !ReadManifestOrigin("nonsense", origin) && !ManifestIsNewer(text));

    std::vector<LevelPackage> listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 1 && listed[0].recorded && listed[0].ce == Need::kRecommended && listed[0].levels().size() == 1 && listed[0].levels()[0]->start && listed[0].scenes[0].missing.empty());
    CHECK(Read(dir / kManifestFile) == text && Read(dir / JPT_CUSTOM_LEVEL_RUNTIME_MAP) == map);

    // A package of an earlier version has no records; its files and saves stay as they are.
    const fs::path save = user / "saves" / "custom_levels" / "Causeway" / "SAVEGAME.000";
    Write(save, "save");
    for (const char* name : {"manifest.json", "runtime.map"}) fs::remove(dir / name);
    fs::remove_all(dir / JPT_CUSTOM_LEVEL_GENERATED_DIR);
    listed = ListLevelPackages(user, {});
    CHECK(listed.size() == 1 && listed[0].recorded && Read(dir / JPT_CUSTOM_LEVEL_RUNTIME_MAP) == map && fs::exists(dir / PathFromUtf8(LoadingPictureFile(0))));
    const auto again = trg::update::json::Parse(Read(dir / kManifestFile));
    CHECK(again && again->Find("source_type")->AsString() == "migrated" && json && again->Find("source_hash")->AsString() == json->Find("source_hash")->AsString());
    CHECK(Read(save) == "save");
    Write(dir / kManifestFile, "{ damaged");
    CHECK(ListLevelPackages(user, {})[0].recorded && trg::update::json::Parse(Read(dir / kManifestFile)));
    Write(dir / kManifestFile, "{\"schema_version\": 99}");
    listed = ListLevelPackages(user, {});
    CHECK(!listed[0].recorded && Read(dir / kManifestFile) == "{\"schema_version\": 99}" && listed[0].levels().size() == 1);

    const fs::path source = Fresh("again");
    for (const auto& [path, bytes] : ReadyCauseway()) Write(source / "Renamed" / PathFromUtf8(path), bytes);
    CHECK(ImportLevelFolder(source / "Renamed", user, {}).status == ImportStatus::kAlreadyInstalled);
    Write(source / "Renamed" / "Causeway~HD" / "data" / "Causeway~HD.wtd", "terrain of another release");
    CHECK(ImportLevelFolder(source / "Renamed", user, {}).package == "Renamed");
    Write(source / "Renamed" / "Causeway~HD" / "data" / "Causeway~HD.wtd", "a third release of the terrain");
    CHECK(ImportLevelFolder(source / "Renamed", user, {}).package == "Renamed-2");

    const size_t before = size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator()));
    const AnalysisResult result = AnalyzeLevelSource(source / "Renamed");
    CHECK(result.import.status == ImportStatus::kInstalled && result.report.find("CWStream.tpa") != std::string::npos && trg::update::json::Parse(result.json));
    CHECK(size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator())) == before && !fs::exists(source / "Renamed" / kManifestFile));
    CHECK(AnalyzeLevelSource(source / "none").import.status == ImportStatus::kUnreadable);
}

static std::string Bmp8(const std::string& mark = {}) {
    std::string out = "BM" + U32(0) + U32(0) + U32(54 + 1024) + U32(40) + U32(4) + U32(4) + std::string("\1\0\x08\0", 4) + U32(0) + U32(16) + U32(0) + U32(0) + U32(256) + U32(0);
    return out + std::string(1024, 'p') + std::string(16, 'x') + mark;
}

static std::string Texture(const char* path) {
    char host[1024];
    return jpt_custom_level_find_relative(path, host, sizeof host) ? Read(fs::path(host)) : std::string("-");
}

static fs::path g_texture_game;

static int GameFile(const char* relative, char* out, size_t cap) {
    for (const fs::path& root : {g_texture_game, g_texture_game / "data"}) {
        const std::string host = (root / relative).string();
        if (!fs::is_regular_file(host) || host.size() >= cap) continue;
        std::snprintf(out, cap, "%s", host.c_str());
        return 1;
    }
    return 0;
}

static std::string Winner(const char* path) {
    jpt_texture_try_t t;
    for (int i = 0; jpt_custom_level_texture_try(path, i, &t); ++i)
        if (t.result == JPT_TEXTURE_FOUND) return std::string(t.in_game ? "game " : "package ") + t.relative;
    return "-";
}

// Trespasser CE 1.07e: texhw\<path>, else texhw\Map\<file>, else <path>; each through the mod's folder, its data folder, the game.
static void TestTextureRoots() {
    const fs::path user = Fresh("textures"), game = g_texture_game = Fresh("textures game");
    const fs::path dir = Install(user, "AS2R", Level("AS2R/data/", "as2") + Level("AS2R/data/", "other") +
        Files{{"AS2R/fm.ini", "[General]\nStartLevel=as2.scn\n"},
              {"AS2R/data/texhw/map/whitewaterDiff.bmp", Bmp8("shared")}, {"AS2R/data/texhw/map/as2/Rock.bmp", Bmp8("rock of as2")},
              {"AS2R/data/texhw/map/both.bmp", Bmp8("shared both")}, {"AS2R/data/texhw/map/as2/both.bmp", Bmp8("both of as2")},
              {"AS2R/data/texhw/map/other/Private.bmp", Bmp8("private")}, {"AS2R/texhw/map/twice.bmp", Bmp8("mod folder")}, {"AS2R/data/texhw/map/twice.bmp", Bmp8("data folder")},
              {"AS2R/Map/as2/foam.bmp", Bmp8("foam")}, {"AS2R/data/Map/sign.bmp", Bmp8("sign")}, {"AS2R/Map/as2/over.bmp", Bmp8("plain")}, {"AS2R/data/texhw/map/over.bmp", Bmp8("over")},
              {"AS2R/data/texhw/map/deep.bmp", "BM not 8 bits"}, {"AS2R/data/texhw/map/cloud.dds", "dds"}, {"AS2R/Props/doors/Door.asa", "door"}, {"AS2R/saves/SAVEGAME.000", "save"}});
    CHECK(!fs::exists(dir / "AS2R" / "saves" / "SAVEGAME.000") && Activate(user, "AS2R", "AS2R/data/as2.scn"));
    jpt_custom_level_set_game(GameFile);
    const size_t files = size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator()));

    for (const char* path : {"Map\\as2\\whitewaterDiff.bmp", "Map/as2/whitewaterDiff.bmp", "map\\AS2\\WHITEWATERDIFF.BMP", "\\Map\\as2\\whitewaterDiff.bmp", "texhw\\Map\\whitewaterDiff.bmp"})
        CHECK(Texture(path) == Bmp8("shared") && Winner(path) == "package texhw/Map/" + std::string(std::strstr(path, "WHITE") ? "WHITEWATERDIFF.bmp" : "whitewaterDiff.bmp"));
    CHECK(Texture("Map\\as2\\Rock.bmp") == Bmp8("rock of as2") && Texture("texhw\\Map\\as2\\ROCK.bmp") == Bmp8("rock of as2") && Winner("Map\\as2\\rock.bmp") == "package texhw/Map/as2/rock.bmp");
    CHECK(Texture("Map\\as2\\both.bmp") == Bmp8("both of as2") && Texture("Map\\other\\both.bmp") == Bmp8("shared both"));
    CHECK(Texture("Map\\as2\\twice.bmp") == Bmp8("mod folder") && Texture("Map\\as2\\over.bmp") == Bmp8("over"));
    CHECK(Texture("Map\\as2\\foam.bmp") == Bmp8("foam") && Texture("Map\\sign.bmp") == Bmp8("sign") && Winner("Map\\as2\\foam.bmp") == "package Map/as2/foam.bmp");
    CHECK(Texture("Map\\as2\\Private.bmp") == "-" && Texture("Map\\other\\Private.bmp") == Bmp8("private") && Texture("Private.bmp") == "-");
    CHECK(Texture("Map\\as2\\gone.bmp") == "-" && Winner("Map\\as2\\gone.bmp") == "-" && Texture("Map\\as2\\deep.bmp") == "-" && Texture("Map\\as2\\cloud.bmp") == "-");
    CHECK(Texture("Props/Doors/door.asa") == "door" && Texture("as2.wtd") == "-" && Texture("data\\as2.wtd") == "-");
    for (const char* path : {"..\\Map\\as2\\foam.bmp", "Map\\..\\..\\x.bmp", "Map\\as2\\..\\..\\..\\foam.bmp", "C:\\Map\\as2\\foam.bmp", "\\\\host\\Map\\as2\\foam.bmp", "Map\\\\as2\\foam.bmp", ""})
        CHECK(Texture(path) == "-" && Winner(path) == "-");

    Write(game / "data" / "texhw" / "Map" / "gone.bmp", Bmp8("game override"));
    Write(game / "Map" / "as2" / "foam.bmp", Bmp8("game foam"));
    Write(game / "Map" / "as2" / "whitewaterDiff.bmp", Bmp8("game water"));
    CHECK(Texture("Map\\as2\\gone.bmp") == Bmp8("game override") && Winner("Map\\as2\\gone.bmp") == "game texhw/Map/gone.bmp");
    CHECK(Texture("Map\\as2\\foam.bmp") == Bmp8("foam") && Texture("Map\\as2\\whitewaterDiff.bmp") == Bmp8("shared"));
    jpt_texture_try_t t;
    std::string tried;
    for (int i = 0; jpt_custom_level_texture_try("Map\\as2\\deep.bmp", i, &t); ++i) tried += std::string(t.in_game ? "g:" : "p:") + t.relative + (t.result == JPT_TEXTURE_UNUSABLE ? "!" : "") + " ";
    CHECK(tried == "p:texhw/Map/as2/deep.bmp g:texhw/Map/as2/deep.bmp p:texhw/Map/deep.bmp! g:texhw/Map/deep.bmp p:Map/as2/deep.bmp g:Map/as2/deep.bmp ");
    char other[256] = "";
    CHECK(jpt_custom_level_texture_other("Map\\as2\\cloud.bmp", other, sizeof other) && std::string(other) == "AS2R/data/texhw/map/cloud.dds" && !jpt_custom_level_texture_other("Map\\as2\\gone.bmp", other, sizeof other));
    CHECK(size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator())) == files);

    const fs::path home = fs::current_path(), elsewhere = Fresh("textures elsewhere");
    Write(elsewhere / "Map" / "as2" / "gone2.bmp", Bmp8("not a root"));
    fs::current_path(elsewhere);
    CHECK(Texture("Map\\as2\\whitewaterDiff.bmp") == Bmp8("shared") && Texture("Map\\as2\\gone2.bmp") == "-");
    fs::current_path(home);

    Install(user, "Loose", Level("", "cave") + Files{{"texhw/map/whitewaterDiff.bmp", Bmp8("water of cave")}, {"Map/cave/wall.bmp", Bmp8("wall")}});
    CHECK(Activate(user, "Loose", "cave.scn") && Texture("Map\\cave\\whitewaterDiff.bmp") == Bmp8("water of cave") && Texture("Map\\cave\\wall.bmp") == Bmp8("wall"));
    CHECK(Texture("Map\\as2\\Rock.bmp") == "-" && Texture("Map\\as2\\foam.bmp") == "-");
    jpt_custom_level_deactivate();
    CHECK(Texture("Map\\cave\\wall.bmp") == "-" && !jpt_custom_level_is_texture_request("Map\\cave\\wall.bmp"));
}

static long LimitOf(const PackageAnalysis& a, const char* key) {
    for (const SessionLimit& limit : a.limits)
        if (limit.key == key) return limit.value;
    return 0;
}

static void TestSessionLimits() {
    const auto mission = [](const std::string& fm, const std::string& global = {}) {
        Files files = Level("M/data/", "m") + Files{{"M/fm.ini", fm}};
        if (!global.empty()) files.push_back({"trespasser.ini", global});
        return Analyze(files);
    };
    CHECK(mission("[General]\nStartLevel=m.scn\n").limits.empty() || LimitOf(mission("[General]\nStartLevel=m.scn\n"), "terrain_nodes") == 0);
    CHECK(LimitOf(mission("[Render]\nTerrainBackfaceCullBias=0.1\n"), "terrain_cull_mm") == 100 && LimitOf(mission("[Render]\nTerrainBackfaceCullBias=0.0004\n"), "terrain_cull_mm") == 1 && LimitOf(mission("[Render]\nTerrainBackfaceCullBias=5000\n"), "terrain_cull_mm") == 10000);
    for (const char* off : {"0", "-1", "0.0", "", "x"}) CHECK(!LimitOf(mission(std::string("[Render]\nTerrainBackfaceCullBias=") + off + "\n"), "terrain_cull_mm"));
    CHECK(LimitOf(mission("[Memory]\nMeshHeapSize=64\nExtraTerrainNodes=10\n"), "mesh_heap_mb") == 64 && LimitOf(mission("[Memory]\nExtraTerrainNodes=10\n"), "terrain_nodes") == 10);
    CHECK(LimitOf(mission("[Memory]\nMeshHeapSize=48\n", "[Memory]\nMeshHeapSize=96\n"), "mesh_heap_mb") == 96);
    CHECK(LimitOf(mission("[Memory]\nMeshHeapSize=96\n", "[Memory]\nMeshHeapSize=48\n"), "mesh_heap_mb") == 96);
    for (const char* value : {"1", "4096", "99999999999999999999", "-5", "lots"}) {
        const PackageAnalysis a = mission(std::string("[Memory]\nMeshHeapSize=") + value + "\nExtraTerrainNodes=" + value + "\n");
        const long heap = LimitOf(a, "mesh_heap_mb"), nodes = LimitOf(a, "terrain_nodes");
        CHECK((heap == 0 || (heap >= kMeshHeapMegabytes && heap <= kMeshHeapMegabytesMax)) && nodes >= 0 && nodes <= kTerrainNodesMax);
    }
    // Misty Jungle's own shape: no ini at all, a readme that asks for CE.
    const Files misty = Level("", "Misty") + Files{{"Misty_readme.txt", "Due to higher poly models this level can only be run in Trespasser CE."}};
    CHECK(Analyze(misty).need("Trespasser CE") == Need::kRequired && LimitOf(Analyze(misty), "mesh_heap_mb") == 0);

    const fs::path user = Fresh("limits");
    Install(user, "Big", Level("M/data/", "m") + Files{{"M/fm.ini", "[Memory]\nMeshHeapSize=64\nExtraTerrainNodes=100\n"}});
    Install(user, "Small", Level("", "small"));
    CHECK(Activate(user, "Big", "M/data/m.scn") && jpt_custom_level_limit("mesh_heap_mb") == 64 && jpt_custom_level_limit("terrain_nodes") == 100 && !jpt_custom_level_limit("other"));
    CHECK(Activate(user, "Small", "small.scn") && !jpt_custom_level_limit("mesh_heap_mb") && !jpt_custom_level_limit("terrain_nodes"));
    const fs::path map = CustomLevelsDir(user) / "Small" / JPT_CUSTOM_LEVEL_RUNTIME_MAP;
    Write(map, "trespasser-runtime 1\nscene small.scn\nfile data/small.scn|small.scn\nlimit mesh_heap_mb|2000\nlimit terrain_nodes|-1\nlimit stack_mb|64\n");
    CHECK(Activate(user, "Small", "small.scn") && !jpt_custom_level_limit("mesh_heap_mb") && !jpt_custom_level_limit("terrain_nodes") && !jpt_custom_level_limit("stack_mb"));
    jpt_custom_level_deactivate();
    CHECK(!jpt_custom_level_limit("mesh_heap_mb"));
}

static bool Logged(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
}

static ImportResult ImportFiles(const fs::path& user, const char* name, const Files& files, const fs::path& data_dir) {
    const fs::path source = Fresh("states source") / name;
    for (const auto& [path, bytes] : files) Write(source / PathFromUtf8(path), bytes);
    return ImportLevelFolder(source, user, {}, {}, data_dir);
}

// A package as an earlier version installed it, before only READY ones were.
static void PlaceFiles(const fs::path& user, const char* name, const Files& files) {
    for (const auto& [path, bytes] : files) Write(CustomLevelsDir(user) / name / PathFromUtf8(path), bytes);
}

static bool Left(const fs::path& user, const char* name) {
    std::error_code ec;
    if (fs::exists(CustomLevelsDir(user) / name)) return true;
    for (const auto& entry : fs::directory_iterator(CustomLevelsDir(user), ec))
        if (Utf8(entry.path().filename()).rfind(".import-", 0) == 0) return true;
    return false;
}

// The launcher's question before Play; why: what it answers with when it refuses.
static bool Plays(const fs::path& user, const fs::path& game, const std::string& id, const std::string& scene, ImportStatus* why = nullptr, std::vector<std::string>* log = nullptr) {
    const PlayCheck check = CheckBeforePlay(user, game, id, scene);
    if (why) *why = check.reason;
    if (log) *log = check.log;
    return check.ok;
}

static void TestLevelStates() {
    const fs::path user = Fresh("states"), game = Fresh("states game");
    for (const char* name : {"ij-130.spz", "be.GRF", "be-130.spz"}) Write(game / name, "retail");
    for (const char* name : {"IJ-130.pid", "be-130.pid", "PW-130.pid"}) Write(game / name, Set(""));
    const Files e3 = {{"IJ.scn", Scene("ij.grf")}, {"IJ.GRF", World("")}, {"PV.scn", Scene("pv.grf", 27)}, {"PV.GRF", World("")}, {"Reuse.scn", Scene("be.grf")}};
    // One level the port cannot read refuses the whole package; nothing of it is left behind.
    const ImportResult mixed = ImportFiles(user, "E3", e3, game);
    CHECK(!mixed.ok() && mixed.status == ImportStatus::kUnsupportedFormat && mixed.levels == 0 && !Left(user, "E3") && Logged(mixed.log, "result: rejected"));
    CHECK(Logged(mixed.log, "PV.scn - Unsupported, scene version 27 is newer than 26") && Logged(mixed.log, "  scene version 27") && Logged(mixed.log, "missing required: pv-130.pid") && Logged(mixed.log, "class: REJECTED_UNSUPPORTED"));
    CHECK(Logged(mixed.log, "IJ.scn - Not supported") && Logged(mixed.log, "PACKAGE_NOT_SUPPORTED PV.scn"));

    // The same package installed by an earlier version: listed, never started.
    PlaceFiles(user, "E3", e3);
    std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(listed.size() == 1 && listed[0].scenes.size() == 3 && listed[0].levels().empty() && PlayableLevels(listed) == 0);
    for (const LevelScene& scene : listed[0].scenes) CHECK(!scene.playable() && scene.rejected() && (scene.file == "PV.scn") == (scene.state == LevelState::kUnsupportedFormat));
    for (const LevelScene* row : listed[0].rows()) CHECK(!ActionsOf(row, true).play && ActionsOf(row, true).report && !ActionsOf(row, true).readme);
    ImportStatus why = ImportStatus::kInstalled;
    std::vector<std::string> log;
    CHECK(!Plays(user, game, "E3", "IJ.scn", &why, &log) && why == ImportStatus::kNotSupported && Logged(log, "PACKAGE_NOT_SUPPORTED") && !Plays(user, game, "E3", "PV.scn", &why) && why == ImportStatus::kUnsupportedFormat);
    CHECK(!Plays(user, game, "E3", "gone.scn") && !Plays(user, game, "none", "IJ.scn") && !Plays(user, game, "../E3", "IJ.scn"));
    const std::string manifest = Read(CustomLevelsDir(user) / "E3" / kManifestFile);
    CHECK(manifest.find("PV.scn: pv-130.pid") != std::string::npos);
    fs::remove_all(CustomLevelsDir(user) / "E3");
    PlaceFiles(user, "E3", {e3[0], e3[1]});
    CHECK(Plays(user, game, "E3", "IJ.scn"));
    fs::remove(CustomLevelsDir(user) / "E3" / "IJ.GRF");
    CHECK(!Plays(user, game, "E3", "IJ.scn", &why, &log) && why == ImportStatus::kMissingFiles && Logged(log, "MISSING_REQUIRED_WORLD ij.grf") && Logged(log, "result: BLOCKED_PRECHECK, not started, This level is missing required files") && PlayableLevels(ListLevelPackages(user, game)) == 0);

    // A level that lacks files is refused, and starts once the installed game has them.
    const Files pw = {{"PW.scn", Scene("pw.grf")}, {"PW.grf", World("")}};
    const ImportResult waiting = ImportFiles(user, "PW", pw, game);
    CHECK(!waiting.ok() && waiting.status == ImportStatus::kMissingFiles && !Left(user, "PW") && Logged(waiting.log, "missing required: pw-130.swp") && Logged(waiting.log, "class: REJECTED_MISSING"));
    PlaceFiles(user, "PW", pw);
    CHECK(!Plays(user, game, "PW", "PW.scn", &why, &log) && why == ImportStatus::kMissingFiles && Logged(log, "MISSING_REQUIRED_TEXTURE_SET pw-130.swp") && Logged(log, "looked in: ") && Logged(log, "alternative: none"));
    Write(game / "PW-130.spz", "retail");
    CHECK(Plays(user, game, "PW", "PW.scn"));
    fs::remove(game / "PW-130.spz");
    const ImportResult none = ImportFiles(user, "Broken", {{"Broken.scn", "nothing"}, {"Broken.grf", World("")}}, game);
    CHECK(none.status == ImportStatus::kNoLevel && !fs::exists(CustomLevelsDir(user) / "Broken") && Logged(none.log, "Broken.scn - Unsupported") && Logged(none.log, "result: rejected, No playable level was found"));
    const ImportResult text = ImportFiles(user, "Text", {{"readme.txt", "no level"}}, game);
    CHECK(text.status == ImportStatus::kNoLevel && Logged(text.log, "files found: 1, accepted: 1, ignored: 0, rejected: 0"));
    CHECK(ImportFiles(user, "Unknown game", {{"PX.scn", Scene("px.grf")}, {"PX.grf", World("")}}, {}).status == ImportStatus::kMissingFiles && !Left(user, "Unknown game"));

    // Pictures beside the level instead of a texture set.
    CHECK(ImportFiles(user, "Loose", {{"Cave.scn", Scene("cave.grf")}, {"Cave.grf", World("Cave")}, {"Cave.wtd", "t"}, {"Map/Cave/crate.bmp", Bmp8()}}, game).levels == 1);
    // A texture set that cannot be read leaves every texture missing.
    const ImportResult half = ImportFiles(user, "Half", {{"Pit.scn", Scene("pit.grf")}, {"Pit.grf", World("")}, {"Pit-130.pid", "p"}, {"Pit-130.spz", "s"}}, game);
    CHECK(half.status == ImportStatus::kMissingFiles && Logged(half.log, "MISSING_TEXTURES pit.grf") && !Left(user, "Half"));

    const auto level = [](const char* stem, uint32_t version) {
        const std::string s = stem;
        return Files{{s + ".scn", Scene(s + ".grf", version)}, {s + ".grf", World(s)}, {s + "-130.pid", Set(s)}, {s + "-130.swp", "s"}, {s + ".wtd", "t"}};
    };
    const std::string late = std::string("\xBE\xBA\xCE\x0A", 4) + std::string(90000, '\7') + "note.grf" + std::string(20, '\0') + Scene("late.grf", 25).substr(4);
    CHECK(jpt_scene_version(late.data(), late.size()) == 25 && jpt_scene_version("x.grf", 5) == 0 && jpt_scene_version("", 0) == 0);
    CHECK(ImportFiles(user, "Old", level("Old", 16), game).levels == 1 && ImportFiles(user, "Retail", level("Retail", 22), game).levels == 1);
    for (const uint32_t version : {23u, 24u, 25u, 26u}) {
        const std::string name = "V" + std::to_string(version);
        const ImportResult later = ImportFiles(user, name.c_str(), level(name.c_str(), version), game);
        CHECK(later.ok() && later.levels == 1 && Logged(later.log, "scene version " + std::to_string(version) + ", read as the game's own 22") && Logged(later.log, "result: installed"));
        CHECK(Read(CustomLevelsDir(user) / name / (name + ".scn")) == Scene(name + ".grf", version) && Plays(user, game, name, name + ".scn") && Activate(user, name.c_str(), (name + ".scn").c_str()));
        CHECK(!Logged(later.log, "Trespasser CE"));
    }
    Files damaged = level("Newer", 25);
    damaged[0].second.resize(damaged[0].second.size() / 2);
    for (const Files& files : {level("Newer", 27), damaged}) {
        const ImportResult refused = ImportFiles(user, "Newer", files, game);
        CHECK(refused.status == ImportStatus::kUnsupportedFormat && refused.levels == 0 && !fs::exists(CustomLevelsDir(user) / "Newer"));
        CHECK(Logged(refused.log, "result: rejected, This level is not supported by this port yet (Newer.scn)") && ImportFiles(user, "Newer", files, {}).status == ImportStatus::kUnsupportedFormat);
    }
    const ImportResult b55 = ImportFiles(user, "B55", level("ij", 25) + level("jr", 25), game);
    CHECK(b55.ok() && b55.levels == 2 && Logged(b55.log, "ij.scn - Ready") && Logged(b55.log, "jr.scn - Ready"));
    const ImportResult test_scene = ImportFiles(user, "B55 test", level("ij", 25) + Files{{"TestScene.scn", Scene("TestScene.grf", 25)}, {"TestScene.grf", World("")}}, game);
    CHECK(test_scene.status == ImportStatus::kMissingFiles && !Left(user, "B55 test"));
    const ImportResult part = ImportFiles(user, "Part", level("Good", 22) + level("Late", 27), game);
    CHECK(!part.ok() && part.status == ImportStatus::kUnsupportedFormat && !Left(user, "Part"));
    PlaceFiles(user, "Part", level("Good", 22) + level("Late", 27));
    CHECK(!Plays(user, game, "Part", "Late.scn", &why) && why == ImportStatus::kUnsupportedFormat && !Plays(user, game, "Part", "Good.scn", &why) && why == ImportStatus::kNotSupported);
    CHECK(Activate(user, "Part", "Good.scn") && !Activate(user, "Part", "Late.scn"));
    const ImportResult unread = ImportFiles(user, "Unread", Files{{"U.scn", Scene("U.grf", 0)}, {"U.grf", World("")}, {"U-130.pid", "p"}, {"U-130.swp", "s"}}, game);
    CHECK(!unread.ok() && Logged(unread.log, "U.scn - Unknown, its scene version could not be read") && !fs::exists(CustomLevelsDir(user) / "Unread"));

    Files states = Level("T/data/", "one") + Files{{"T/fm.ini", "[General]\nStartLevel=one.scn\n"}, {"T/data/one_saved.scn", Scene("one.grf")}, {"T/data/bad.scn", "x"}};
    const ImportResult several = ImportFiles(user, "States", states, game);
    CHECK(!several.ok() && several.status == ImportStatus::kUnsupportedFormat && Logged(several.log, "bad.scn - Unsupported") && Logged(several.log, "NOT_A_SCENE bad.scn"));
    states.pop_back();
    const ImportResult saved = ImportFiles(user, "States", states, game);
    CHECK(saved.ok() && saved.levels == 1 && Logged(saved.log, "one.scn - Ready") && Logged(saved.log, "scene version 22, StartLevel") && Logged(saved.log, "one_saved.scn - Auxiliary"));
}

static void TestImportLog() {
    const fs::path user = Fresh("log"), game = Fresh("log game");
    const std::string wide("\xFF\xFE" "W\0i\0d\0e\0 \0\xE9\0", 14);
    Files files = Level("M/data/", "m") + Files{{"M/fm.ini", "[General]\nStartLevel=m.scn\nStreamTPA+=More.tpa\n"}, {"M/More.tpa", Tpa({{1, "x"}})}, {"M/Readme.txt", "Put the level in data.\r\n=== fault caf\xE9\r\n"},
                                                {"M/INSTALL.txt", wide}, {"M/utf8.nfo", "\xEF\xBB\xBF" "caf\xC3\xA9"}, {"M/Guide.md", std::string(70000, 'g')}, {"M/data/notes.txt", "hello"}, {"M/manual.txt", std::string("bin\0ary", 7)},
                                                {"M/run.exe", "MZ"}, {"M/go.bat", "x"}, {"M/script.nut", "x"}, {"M/saves/SAVEGAME.000", "s"}, {"M/doc.html", "x"}, {"M/animb/x.asb", "b"}, {"M/data/a.sky", "[LAYER1]\nType=STARS\n"}};
    for (int i = 0; i < 60; ++i) files.push_back({"M/data/texhw/map/t" + std::to_string(i) + ".dds", "dds"});
    const ImportResult result = ImportFiles(user, "Mission", files, game);
    CHECK(result.status == ImportStatus::kNotSupported && !Left(user, "Mission") && std::all_of(result.log.begin(), result.log.end(), [](const std::string& line) { return line.size() < 900 && line.find('\n') == std::string::npos; }));
    for (const char* line : {"custom level import source: folder \"Mission\"", "source path: ", "package Mission, layout fan_mission, mod folder M", "files found: 80, accepted: 75, ignored: 3, rejected: 2",
                             "SCN (1):", "GRF (1):", "PID (1):", "SWP (1):", "WTD (1):", "INI (1):", "TPA (1):", "TXT (4):", "MD (1):", "NFO (1):", "ASA (1):", "DDS (60):", "SKY (1):", "  M/data/texhw/map/t59.dds",
                             "M/run.exe - a program or library", "M/go.bat - a script or shortcut", "M/saves/SAVEGAME.000 - a saved game", "M/script.nut - a Trespasser CE script",
                             "level M/data/m.scn - Not supported", "  scene version 22, StartLevel", "  class: REJECTED_UNSUPPORTED",
                             "  UNSUPPORTED_FEATURE CE_SKY_STARS: ", "  UNSUPPORTED_FEATURE CE_SCRIPTS: ", "  GRF m.grf: M/data/m.grf", "  PID m-130.pid: M/data/m-130.pid", "  SWP m-130.swp: M/data/m-130.swp",
                             "  WTD m.wtd: M/data/m.wtd", "  config: M/fm.ini", "stream += More.tpa (ready)", "README BEGIN M/Readme.txt (40 bytes, SHA-256 ", "| Put the level in data.", "| === fault caf\xC3\xA9", "README END M/Readme.txt",
                             "README BEGIN M/INSTALL.txt", "| Wide \xC3\xA9", "| caf\xC3\xA9", "| ... truncated: the first 65536 of 70000 bytes", "| not a readable text", "README BEGIN M/manual.txt", "TXT BEGIN M/data/notes.txt (5 bytes, SHA-256 ", "| hello", "TXT END M/data/notes.txt", "README (", "Configs (1):", "result: rejected, This custom level is not fully supported (m.scn)"})
    {
        const bool logged = Logged(result.log, line);
        if (!logged) std::printf("  not logged: %s\n", line);
        CHECK(logged);
    }
    CHECK(Logged(result.log, "M/animb/x.asb") && result.log.back().rfind("result: ", 0) == 0);
    const auto times = [&result](const std::string& text) {
        return std::count_if(result.log.begin(), result.log.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    CHECK(times("BEGIN M/Readme.txt") == 1 && times("BEGIN M/data/notes.txt") == 1 && times("| hello") == 1 && times(" BEGIN ") == times(" END "));
    const ImportResult again = ImportFiles(user, "Mission", files, game);
    CHECK(again.status == ImportStatus::kNotSupported && !Left(user, "Mission"));
    files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f) { return f.first.find("texhw") != std::string::npos || PathType(f.first) == ".sky" || PathType(f.first) == ".nut"; }), files.end());
    CHECK(ImportFiles(user, "Mission", files, game).ok());
    const ImportResult twice = ImportFiles(user, "Mission", files, game);
    CHECK(twice.status == ImportStatus::kAlreadyInstalled && Logged(twice.log, "result: rejected, This level is already installed"));

    const fs::path bad = user / "bad.zip";
    Write(bad, "not an archive");
    const ImportResult broken = ImportLevelArchive(bad, user, {});
    CHECK(!broken.ok() && Logged(broken.log, "archive \"bad.zip\", 14 bytes") && Logged(broken.log, "source SHA-256: ") && Logged(broken.log, "result: rejected"));
    const ImportResult gone = ImportLevelArchive(user / "gone.zip", user, {});
    CHECK(gone.status == ImportStatus::kUnreadable && Logged(gone.log, "gone.zip") && Logged(gone.log, "result: rejected"));
    CHECK(Logged(ImportFiles(user, "Text", {{"readme.txt", "only words"}}, game).log, "| only words"));
    Files refused = {{"N.scn", Scene("N.grf", 27)}, {"N.grf", World("N")}, {"N-130.pid", "p"}, {"N-130.swp", "s"}, {"N.wtd", "t"}, {"N.txt", "the author's words"}, {"readme.txt", "how to install"}};
    const ImportResult unsupported = ImportFiles(user, "Refused", refused, game);
    for (const char* line : {"N.scn - Unsupported, scene version 27 is newer than 26", "  scene version 27", "  GRF N.grf: N.grf", "  PID N-130.pid: N-130.pid", "  SWP N-130.swp: N-130.swp", "  WTD N.wtd: N.wtd", "TXT BEGIN N.txt", "| the author's words",
                             "TXT END N.txt", "README BEGIN readme.txt", "| how to install", "layout loose", "SCN (1):", "result: rejected, This level is not supported by this port yet (N.scn)"}) {
        const bool logged = Logged(unsupported.log, line);
        if (!logged) std::printf("  not logged: %s\n", line);
        CHECK(logged);
    }
    CHECK(AnalyzeLevelSource(Fresh("none"), game).import.status != ImportStatus::kInstalled);
}

static std::string Adapt(const std::string& bytes, jpt_scene_report_t* report) {
    size_t size = 0;
    uint8_t* out = jpt_scene_adapt(bytes.data(), bytes.size(), &size, report);
    const std::string text = out ? std::string(reinterpret_cast<char*>(out), size) : std::string();
    std::free(out);
    return text;
}

static bool Noted(const jpt_scene_report_t& report, const char* text) {
    for (int i = 0; i < report.note_count; ++i)
        if (std::strstr(report.notes[i], text)) return true;
    return false;
}

// `version` as the loader is to read it: the game's own sections, and what only CE knows left where it is.
static std::string Expected(const std::string& world, uint32_t version, const SceneShape& shape) {
    SceneShape own = shape;
    own.second = false;
    Sections sections = {{"Header", HeaderSection(world, version >= 26 ? version : 22)}, {"4C56C6CC", RenderSection(22, own)}, {"A9AC56AE", AiSection(22, own)},
                         {"9FB57F53", PlayerSection(version, own, false)}, {"00A1B2C3", Words(9, 1300) + Mark(version, shape) + Words(4, 1400)}};
    if (version >= 25) sections.push_back({"4B642136", U32(1) + U32(0xFFFFFFFF) + U32(0)}), sections.push_back({"190DAD34", U32(0)});
    if (version >= 26) sections.push_back({"1C1CA6D4", std::string(16, '\0')});
    return Container(sections);
}

static void TestSceneFormats() {
    jpt_scene_report_t report;
    for (const uint32_t version : {16u, 21u, 22u})
        CHECK(Adapt(SceneOf("w.grf", version), &report).empty() && report.version == int(version) && !report.problem[0] && !report.note_count && jpt_scene_readable(SceneOf("w.grf", version).data(), SceneOf("w.grf", version).size(), nullptr));
    for (const uint32_t version : {23u, 24u, 25u, 26u}) {
        const std::string source = SceneOf("w.grf", version), copy = source;
        CHECK(Adapt(source, &report) == Expected("w.grf", version, {}) && report.version == int(version) && !report.problem[0] && source == copy);
        if (version < 26) CHECK(Adapt(Adapt(source, &report), &report).empty() && report.version == 22 && !report.problem[0]);
    }
    for (const uint32_t version : {25u, 26u}) {
        SceneShape shape;
        shape.second = true, shape.swaps = 3, shape.mask = 3, shape.script = version >= 26 ? 20 : 0, shape.joints = 0;
        CHECK(Adapt(SceneOf("w.grf", version, shape), &report) == Expected("w.grf", version, shape) && !report.problem[0]);
        CHECK(Noted(report, "second stowed object") && Noted(report, "swapped textures of 2 animated mesh(es)") && Noted(report, "colour tables"));
        SceneShape many;
        many.active = 14;
        CHECK(Adapt(SceneOf("w.grf", version, many), &report) == Expected("w.grf", version, many) && Noted(report, "4 of 14 active animals"));
        SceneShape none;
        none.active = none.listed = none.meshes = 0;
        CHECK(Adapt(SceneOf("w.grf", version, none), &report) == Expected("w.grf", version, none) && !report.problem[0]);
    }
    CHECK(Adapt(SceneOf("w.grf", 27), &report).empty() && report.version == 27 && report.problem[0] && !jpt_scene_readable("", 0, &report) && report.version == 0);

    const Sections whole = {{"Header", HeaderSection("w.grf", 26)}, {"4C56C6CC", RenderSection(26, {})}, {"A9AC56AE", AiSection(26, {})}, {"9FB57F53", PlayerSection(26, {}, true)}};
    const auto with = [&whole](size_t section, size_t at, uint32_t value) {
        Sections changed = whole;
        changed[section].second.replace(at, 4, U32(value));
        return Container(changed);
    };
    const auto cut = [&whole](size_t section, size_t size) {
        Sections changed = whole;
        changed[section].second.resize(size);
        return Container(changed);
    };
    CHECK(!Adapt(Container(whole), &report).empty());
    const size_t render_block = 4, render_second = 4 * 23, render_mask = 4 * 37, render_water = 4 * 43, mesh_count = 4 * (61 + 7);
    for (const std::string& bad : {with(1, 0, 5), with(1, render_mask, 4), with(1, render_water, 3), with(1, mesh_count, 0xFFFFFFFF), with(1, mesh_count, 0x20000001), with(1, render_second, 1),
                                   with(2, 20, 0xFFFFFFFF), with(2, 20, 0x40000003), with(2, 20, 4), with(2, 20 + 4 + 12, 0xFFFFFFF0), with(2, 20 + 4 + 12, 6), with(3, 36, 1), with(3, 40 + 36, 0x10000000), with(3, 40 + 36, 3),
                                   cut(0, 8), cut(0, 0x400), cut(1, 0), cut(1, 100), cut(1, RenderSection(26, {}).size() - 2), cut(2, 0), cut(2, 30), cut(3, 0), cut(3, 40), cut(3, PlayerSection(26, {}, true).size() - 1)})
        CHECK(Adapt(bad, &report).empty() && report.problem[0] && !jpt_scene_readable(bad.data(), bad.size(), nullptr));
    CHECK(Adapt(with(1, render_block, 0), &report).empty() && report.problem[0]);

    const std::string good = SceneOf("w.grf", 25);
    const auto patched = [&good](size_t at, uint32_t value) {
        std::string changed = good;
        return changed.replace(at, 4, U32(value));
    };
    const uint32_t entry = 0x30;
    for (const std::string& bad : {patched(0, 0), patched(8, 0), patched(8, 0xFFFFFFFF), patched(8, 65537), patched(12, 0), patched(12, 0xFFFFFFFF), patched(16, 0xFFFFFFFF), patched(20, 0xFFFFFFFF), patched(entry + 8, 0xFFFFFFFF),
                                   patched(entry + 12, 0xFFFFFFFF), patched(entry + 12, 0x7FFFFFFF), patched(entry + 32, 0x9999), patched(entry + 32, 0x1000)})
        CHECK(Adapt(bad, &report).empty() && report.problem[0]);
    Sections twice = whole;
    twice.push_back(whole[0]);
    CHECK(Adapt(Container(twice), &report).empty() && report.problem[0]);
    for (size_t size = 0; size < good.size(); size += 7)
        CHECK(Adapt(good.substr(0, size), &report).empty() && report.problem[0]);
    uint32_t seed = 1;
    for (int round = 0; round < 4000; ++round) {
        std::string noise = round & 1 ? good : SceneOf("w.grf", 26);
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1664525u + 1013904223u;
            noise[(seed >> 8) % (round & 2 ? 0x30 + 7 * 32 : noise.size())] = char(seed >> 24);
        }
        const std::string out = Adapt(noise, &report);
        CHECK((report.version > 22 && !report.problem[0]) == !out.empty() && out.size() <= noise.size() * 2 + 64);
    }
}

// Trespasser CE's own picture for a texture that is nowhere: 4 x 4 and grey; nothing of it is ever a file.
static void TestMissingTexture() {
    size_t size = 0;
    for (const int height_map : {0, 1}) {
        const uint32_t side = height_map ? 2048 : 4;
        uint8_t* bytes = jpt_custom_level_missing_texture(height_map, &size);
        const std::string picture = bytes ? std::string(reinterpret_cast<char*>(bytes), size) : std::string();
        CHECK(size == 54 + 1024 + side * side && picture.compare(0, 2, "BM") == 0 && picture.compare(10, 4, U32(54 + 1024)) == 0 && picture.compare(18, 8, U32(side) + U32(side)) == 0 && picture[28] == 8);
        CHECK(picture.find_first_not_of('\1', 54 + 1024) == std::string::npos);
        for (size_t i = 0; i < 256; ++i) CHECK(picture.compare(54 + i * 4, 4, U32(0x7F7F7F)) == 0);
        std::free(bytes);
    }

    // The rule both platforms ask, before the picture: the resolver found nothing anywhere.
    const fs::path user = Fresh("missing"), game = g_texture_game = Fresh("missing game");
    Install(user, "Jungle", Level("", "jungle") + Files{{"Map/jungle/here.bmp", Bmp8("here")}, {"Map/shared.bmp", Bmp8("shared")}, {"texhw/map/over.bmp", Bmp8("over")}});
    CHECK(Activate(user, "Jungle", "jungle.scn"));
    jpt_custom_level_set_game(GameFile);
    Write(game / "Map" / "jungle" / "game.bmp", Bmp8("game"));
    const size_t files = size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator()));
    for (const char* name : {"here", "over", "game"}) {
        char host[1024] = "";
        const std::string path = std::string("Map\\jungle\\") + name + ".bmp";
        CHECK(jpt_custom_level_is_texture_request(path.c_str()) && Winner(path.c_str()) != "-" && jpt_custom_level_find_relative(path.c_str(), host, sizeof host) && host[0]);
    }
    CHECK(Winner("Map\\jungle\\LightBlue03.bmp") == "-" && jpt_custom_level_is_texture_request("Map\\jungle\\LightBlue03.bmp") && jpt_custom_level_is_texture_request("LightBlue03.bmp"));
    CHECK(size_t(std::distance(fs::recursive_directory_iterator(user), fs::recursive_directory_iterator())) == files);
    jpt_custom_level_deactivate();
    CHECK(!jpt_custom_level_is_texture_request("LightBlue03.bmp"));

    const auto source = [](const char* name) { return Read(fs::path(JPT_SOURCE_DIR) / PathFromUtf8(name)); };
    const std::string windows = source("src/host/io_trace.c"), posix = source("src/linux/linux_hle.c"), rule = source("src/host/missing_textures.c");
    CHECK(windows.find("if (h == INVALID_HANDLE_VALUE && GUEST_ARG(4) == OPEN_EXISTING && missing_texture_attempt(") != std::string::npos && windows.find("stream_audio_open_memory(picture, picture_size)") != std::string::npos);
    CHECK(posix.find("if (!resolve_read(name, host, sizeof host, &overlaid)) {") < posix.find("missing_texture_attempt(name, &picture, &size);") && posix.find("open_memory(name, picture, size)") != std::string::npos);
    CHECK(rule.find("if (first_time(s_asked)) report(s_asked);") != std::string::npos && rule.find("fopen") == std::string::npos && rule.find("LightBlue") == std::string::npos);
}

static void TestListedLevels() {
    const fs::path user = Fresh("listed"), game = Fresh("listed game");
    const Files town = Level("TLW/data/", "TLWCITY") + Level("TLW/data/", "TLWSFI") + Files{{"TLW/fm.ini", "[General]\nStartLevel=TLWCITY.scn\n"}};
    const ImportResult imported = ImportFiles(user, "CityChaos", town, game);
    std::vector<LevelPackage> listed = ListLevelPackages(user, game);
    CHECK(imported.ok() && listed.size() == 1 && listed[0].scenes.size() == 2 && listed[0].levels().size() == 1 && listed[0].levels()[0]->file == "TLWCITY.scn" && PlayableLevels(listed) == 1);
    CHECK(fs::exists(CustomLevelsDir(user) / "CityChaos" / "TLW" / "data" / "TLWSFI.scn") && Logged(imported.log, "TLWSFI.scn - Ready") && Logged(imported.log, "scene version 22, StartLevel"));
    CHECK(Activate(user, "CityChaos", "TLW/data/TLWCITY.scn") && Found("TLWSFI.scn") == Scene("TLWSFI.grf") && Found("TLWSFI.grf") == World("TLWSFI"));
    jpt_custom_level_deactivate();

    CHECK(ImportFiles(user, "Loose", Level("", "one") + Level("", "two") + Level("", "three"), game).ok());
    listed = ListLevelPackages(user, game);
    const auto loose = std::find_if(listed.begin(), listed.end(), [](const LevelPackage& p) { return p.id == "Loose"; });
    CHECK(loose != listed.end() && loose->levels().size() == 3 && PlayableLevels(listed) == 4);

    // A start level that cannot be played does not hide the others.
    Files broken = town;
    broken[0].second = Scene("TLWCITY.grf", 27);
    CHECK(ImportFiles(user, "Broken", broken, game).status == ImportStatus::kUnsupportedFormat && !fs::exists(CustomLevelsDir(user) / "Broken"));
    for (const auto& [path, bytes] : broken) Write(CustomLevelsDir(user) / "Broken" / PathFromUtf8(path), bytes);
    listed = ListLevelPackages(user, game);
    const auto half = std::find_if(listed.begin(), listed.end(), [](const LevelPackage& p) { return p.id == "Broken"; });
    CHECK(half != listed.end() && half->levels().empty() && half->rows().size() == 2 && PlayableLevels(listed) == 4);
}

static void TestActivePackage() {
    const fs::path user = Fresh("active");
    Install(user, "Causeway", Causeway());
    Install(user, "Town", Level("TLW/data/", "TLWCITY") + Level("TLW/data/", "TLWSFI") + Level("TLW/data/", "be") +
                              Files{{"TLW/fm.ini", "[General]\nStartLevel=TLWCITY.scn\nEffectsTPA=Own.tpa\nStreamTPA+=All.tpa\n[TLWCITY.scn]\nAmbientTPA+=City.tpa\nLoadingImage=menu/li_city.tga\n[TLWSFI.scn]\nStreamTPA+=Sfi.tpa\n"},
                                    {"TLW/Own.tpa", Tpa({{1, "own"}})}, {"TLW/All.tpa", Tpa({{1, "all"}})}, {"TLW/City.tpa", Tpa({{1, "city"}})}, {"TLW/Sfi.tpa", Tpa({{1, "sfi"}})},
                                    {"TLW/anim/Bird.ASA", "bird"}, {"TLW/data/menu/li_city.tga", Tga(640, 480, 24, false)}, {"TLW/data/menu/mainscreen.ddf", "menu"}, {"TLW/data/menu/Scene.smk", "video"}});
    Install(user, "Complex", Level("", "complex") + Files{{"Rain_storm.asa", "rain"}, {"MAP.bmp", "bmp"}, {"readme.txt", "Move rain_storm.asa into the anim directory.\n\nPlace MAP.bmp in the MAPS directory."}});
    CHECK(Activate(user, "Causeway", "Causeway~HD/data/Causeway~HD.scn"));
    CHECK(std::string(jpt_custom_level_scene()) == "LEVEL1.SCN" && Found("LEVEL1.SCN") == Scene("Causeway~HD.grf") && Found("causeway~hd.scn") == Scene("Causeway~HD.grf"));
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "CWStream.tpa" && Added(JPT_LEVEL_TPA_EFFECTS, 0) == "CWEffects.tpa" && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "CWAmbient.tpa" && Added(JPT_LEVEL_TPA_STREAM, 1) == "-");
    CHECK(Setting("dual_stow") == "1" && Setting("enhanced_sky") == "1" && Setting("renderer") == "-" && jpt_custom_level_skips_ending());
    CHECK(Found("menu\\li_other_0.tga").size() == 18 + 640 * 480 * 2 && Found("menu\\mainscreen.ddf") == "-" && Found("menu/li_other_0.ddf") == "-");
    CHECK(Found("anim\\anim_hsc.asa") == "hsc" && Found("Stream.tpa") == "-");
    jpt_custom_level_scene_loading("C:\\data\\sum.scn");
    CHECK(Found("menu\\li_sum_0.tga") == "-" && Added(JPT_LEVEL_TPA_STREAM, 0) == "CWStream.tpa" && jpt_custom_level_caps());
    jpt_custom_level_scene_loading("C:\\data\\LEVEL1.SCN");
    CHECK(Found("menu\\li_other_0.tga") != "-");
    jpt_custom_level_scene_loading("D:\\SAVEGAME.000");
    CHECK(Found("menu\\li_other_0.tga") != "-");

    CHECK(Activate(user, "Town", "TLW/data/TLWCITY.scn"));
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "All.tpa" && Added(JPT_LEVEL_TPA_STREAM, 1) == "-" && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "City.tpa" && Added(JPT_LEVEL_TPA_EFFECTS, 0) == "-");
    CHECK(Found("EFFECTS.TPA") == Tpa({{1, "own"}}) && Found("Ambient.tpa") == "-" && Found("anim/bird.asa") == "bird" && Found("menu/li_other_0.tga") != "-");
    // A video answers only while an action plays it.
    CHECK(Found("menu\\scene.smk") == "-" && jpt_custom_level_play_video("menu\\scene.smk") && Found("menu\\scene.smk") == "video" && Found("menu\\other.smk") == "-");
    CHECK(!jpt_custom_level_play_video(nullptr) && Found("menu\\scene.smk") == "-" && !jpt_custom_level_play_video("menu\\other.smk") && !jpt_custom_level_play_video("..\\scene.smk"));
    CHECK(Setting("dual_stow") == "-" && !jpt_custom_level_skips_ending() && Found("CWStream.tpa") == "-");
    jpt_custom_level_scene_loading("data\\TLWSFI.scn");
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 1) == "Sfi.tpa" && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "-" && Found("menu/li_other_0.tga") == "-");
    jpt_custom_level_scene_loading("be.scn");
    CHECK(Found("be.scn") == Scene("be.grf") && Found("be.wtd") == "terrain of be");

    CHECK(Activate(user, "Complex", "complex.scn") && Found("anim\\rain_storm.ASA") == "rain" && Found("complex.wtd") == "terrain of complex" && Found("maps/MAP.bmp") == "-");
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "-" && Setting("dual_stow") == "-");

    // A map is only ever a pointer to the package's own files.
    const fs::path map = CustomLevelsDir(user) / "Complex" / JPT_CUSTOM_LEVEL_RUNTIME_MAP;
    Write(user / "outside.tpa", "outside");
    Write(map, "trespasser-runtime 1\nscene complex.scn\nfile data/secret.wtd|../../outside.tpa\nfile data/../up.wtd|complex.wtd\nfile data/two.wtd|complex.wtd\ntpa+ stream|*|../../outside.tpa\n"
               "tpa+ stream|*|../Town/TLW/All.tpa\nset renderer|software\nset dual_stow|1\nloading complex.scn|../Town/x.tga\nnonsense\nfile\nfile |\ntpa+ other|*|complex.wtd\n");
    CHECK(Activate(user, "Complex", "complex.scn") && Found("secret.wtd") == "-" && Found("two.wtd") == "terrain of complex" && Found("..\\up.wtd") == "-");
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "-" && Setting("renderer") == "-" && Setting("dual_stow") == "1" && Found("menu/li_other_0.tga") == "-");
    Write(map, "trespasser-runtime 2\nscene complex.scn\nset dual_stow|1\n");
    CHECK(Activate(user, "Complex", "complex.scn") && Setting("dual_stow") == "-" && Found("anim/rain_storm.asa") == "rain");
    fs::remove(map);
    CHECK(Activate(user, "Complex", "complex.scn") && Found("anim/rain_storm.asa") == "rain" && Found("complex.grf") == World("complex"));
#ifndef _WIN32
    Write(CustomLevelsDir(user) / "Complex" / "COMPLEX.wtd", "another");
    CHECK(Activate(user, "Complex", "complex.scn") && Found("Complex.WTD") == "-" && Found("complex.wtd") == "terrain of complex");
#endif
    jpt_custom_level_deactivate();
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "-" && Setting("dual_stow") == "-" && !jpt_custom_level_skips_ending());
}

// ---- Where a session comes from: one that a package started is custom to its end, the campaign's own never is. ----

static unsigned Caps(std::initializer_list<jpt_level_cap_t> list) {
    unsigned bits = 0;
    for (const jpt_level_cap_t cap : list) bits |= 1u << cap;
    return bits;
}

static int g_ended;
static void CountEnd() { ++g_ended; }

static WorldShape Meshes(const char* stem, uint32_t polygons) {
    WorldShape shape;
    shape.terrain = stem;
    shape.meshes.push_back({"Big-00", std::string("Map\\") + stem + "\\crate.bmp"});
    shape.meshes[0].polygons = polygons;
    return shape;
}

static Files Replaced(Files files, const std::string& name, const std::string& bytes) {
    for (auto& [path, body] : files)
        if (path == name) body = bytes;
    return files;
}

// Everything a package can ask for, and the installed game it must never reach.
static Files Everything() {
    return Replaced(Level("All/data/", "all"), "All/data/all.grf", MakeWorld(Meshes("all", 3000))) +
           Files{{"All/fm.ini", "[General]\nStartLevel=all.scn\nWinMovie=0\nEffectsTPA=Own.tpa\nStreamTPA+=More.tpa\nEnableDualStow=1\nEnhancedSky=1\nWaterRefraction=1\nMaxActiveAi=6\n"
                                "[Memory]\nMeshHeapSize=64\nExtraTerrainNodes=10\n[all.scn]\nLoadingImage=menu/li_all.tga\n"},
                 {"All/Own.tpa", Tpa({{1, "own"}})}, {"All/More.tpa", Tpa({{2, "more"}})}, {"All/data/menu/li_all.tga", Tga(640, 480, 24, false)},
                 {"All/data/texhw/map/rock.bmp", Bmp8("rock")}, {"All/Map/all/wall.bmp", Bmp8("wall")}, {"All/data/be.grf", World("be")}};
}

static const unsigned kEverything = Caps({JPT_LEVEL_CAP_TEXHW, JPT_LEVEL_CAP_SOUND_ADDED, JPT_LEVEL_CAP_SOUND_REPLACED, JPT_LEVEL_CAP_LOADING_IMAGE, JPT_LEVEL_CAP_SKIP_ENDING,
                                          JPT_LEVEL_CAP_DUAL_STOW, JPT_LEVEL_CAP_WATER_REFRACTION, JPT_LEVEL_CAP_ENHANCED_SKY, JPT_LEVEL_CAP_ACTIVE_AI, JPT_LEVEL_CAP_MESH_FACES,
                                          JPT_LEVEL_CAP_MESH_HEAP, JPT_LEVEL_CAP_TERRAIN_NODES});

struct Boundary {
    fs::path user, game;
    Boundary(const char* name) : user(Fresh(name)), game(g_texture_game = Fresh((std::string(name) + " game").c_str())) {
        Write(game / "data" / "be.scn", Scene("be.grf"));
        Write(game / "data" / "be.grf", World("be"));
        Write(game / "data" / "texhw" / "Map" / "be.bmp", Bmp8("a texhw of the game's folder"));
        Write(game / "Map" / "be" / "sand.bmp", Bmp8("sand"));
        Install(user, "Everything", Everything());
        Install(user, "Plain", Level("", "plain"));
        Install(user, "Later", Files{{"later.scn", SceneOf("later.grf", 25)}} + Files{{"later.grf", World("later")}, {"later-130.pid", Set("later")}, {"later-130.swp", "swap"}, {"later.wtd", "terrain of later"}});
        Install(user, "Scripts", Files{{"scripts.scn", SceneOf("scripts.grf", 26)}} + Files{{"scripts.grf", World("scripts")}, {"scripts-130.pid", Set("scripts")}, {"scripts-130.swp", "swap"}, {"scripts.wtd", "t"}});
        Install(user, "Sounds", Level("S/data/", "s") + Files{{"S/fm.ini", "[General]\nStartLevel=s.scn\nAmbientTPA+=A.tpa\n"}, {"S/A.tpa", Tpa({{1, "a"}})}});
        Install(user, "Stow", Level("D/data/", "d") + Files{{"D/fm.ini", "[General]\nStartLevel=d.scn\nEnableDualStow=1\n"}});
    }
    bool Play(const char* package, const char* scene) const {
        const bool ok = Activate(user, package, scene);
        if (ok) jpt_custom_level_set_game(GameFile);
        return ok;
    }
};

// What a part of the port sees of CE, ATX and packages while the original game runs.
static void CheckCampaign(const fs::path& user) {
    char host[1024], why[256] = "";
    const char *key, *value;
    size_t size = 0;
    jpt_scene_report_t report;
    const std::string later = SceneOf("later.grf", 26), file = (CustomLevelsDir(user) / "Scripts" / "scripts.scn").string();
    CHECK(!jpt_custom_level_active() && jpt_custom_level_caps() == 0);
    for (int cap = -1; cap <= JPT_LEVEL_CAPS; ++cap) CHECK(!jpt_custom_level_requires(jpt_level_cap_t(cap)));
    for (const char* path : {"be.grf", "all.grf", "all.scn", "Effects.tpa", "Stream.tpa", "Ambient.tpa", "menu\\li_all_0.tga", "texhw\\map\\rock.bmp", "Map\\all\\wall.bmp", "anim\\x.asa"})
        CHECK(!jpt_custom_level_find(path, host, sizeof host) && !jpt_custom_level_find_in_game(path, host, sizeof host) && !jpt_custom_level_find_relative(path, host, sizeof host) && !jpt_custom_level_is_texture_request(path));
    for (const jpt_level_tpa_t kind : {JPT_LEVEL_TPA_STREAM, JPT_LEVEL_TPA_EFFECTS, JPT_LEVEL_TPA_AMBIENT}) CHECK(!jpt_custom_level_tpa(kind, 0, host, sizeof host));
    for (const char* name : {"mesh_faces", "mesh_heap_mb", "terrain_nodes", "terrain_cull_mm", "texture_pool_mb", ""}) CHECK(!jpt_custom_level_limit(name));
    CHECK(!jpt_custom_level_setting(0, &key, &value) && !jpt_custom_level_skips_ending() && !jpt_custom_level_scene_count() && !jpt_custom_level_scene()[0] && !jpt_custom_level_package()[0] && !jpt_custom_level_dir()[0]);
    CHECK(!jpt_custom_level_game_file("be.scn", host, sizeof host) && !jpt_level_world_needs("all.wtd") && !jpt_level_world_take_set("all-130.pid", &size));
    CHECK(!jpt_custom_level_adapt_scene(file.c_str(), reinterpret_cast<const uint8_t*>(later.data()), later.size(), &size, &report) && !size && !report.version && !report.problem[0]);
    CHECK(jpt_gate_load("", "C:\\data\\be.scn", why, sizeof why) == JPT_GATE_OPEN && !why[0]);
}

static std::string SourceFile(const char* name) { return Read(fs::path(JPT_SOURCE_DIR) / PathFromUtf8(name)); }

// 1. The campaign uses no capability.
static void TestCampaignHasNoCapability() {
    const Boundary b("boundary campaign");
    CheckCampaign(b.user);
    jpt_custom_level_request_t none{};
    char error[128];
    CHECK(!jpt_custom_level_activate(b.user.string().c_str(), &none, error, sizeof error));
    CheckCampaign(b.user);
    std::string names;
    for (int cap = 0; cap < JPT_LEVEL_CAPS; ++cap) {
        const std::string name = jpt_custom_level_cap_name(jpt_level_cap_t(cap));
        CHECK(!name.empty() && names.find("|" + name + "|") == std::string::npos);
        names += "|" + name + "|";
    }
    CHECK(!jpt_custom_level_cap_name(JPT_LEVEL_CAPS)[0]);
}

// 2. A package has the capabilities its content asks for and no other.
static void TestOnlyRequiredCapabilities() {
    const Boundary b("boundary required");
    CHECK(b.Play("Plain", "plain.scn") && jpt_custom_level_active() && jpt_custom_level_caps() == 0);
    CHECK(b.Play("Later", "later.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_SCENE_FORMAT}));
    CHECK(b.Play("Scripts", "scripts.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_SCENE_FORMAT, JPT_LEVEL_CAP_SCRIPT_STATE}));
    CHECK(b.Play("Sounds", "S/data/s.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_SOUND_ADDED}));
    CHECK(b.Play("Stow", "D/data/d.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_DUAL_STOW}));
    CHECK(b.Play("Everything", "All/data/all.scn") && jpt_custom_level_caps() == kEverything);
    CHECK(jpt_custom_level_limit("mesh_faces") == 3300 && jpt_custom_level_limit("mesh_heap_mb") == 64 && jpt_custom_level_limit("terrain_nodes") == 10 && !jpt_custom_level_limit("texture_pool_mb"));
    const std::string texture = "Map\\plain\\x.bmp";
    jpt_texture_try_t t;
    CHECK(b.Play("Plain", "plain.scn"));
    for (int i = 0; jpt_custom_level_texture_try(texture.c_str(), i, &t); ++i) CHECK(std::string(t.relative).rfind("texhw", 0) != 0);
    const std::string map = Read(CustomLevelsDir(b.user) / "Plain" / JPT_CUSTOM_LEVEL_RUNTIME_MAP);
    CHECK(!map.empty() && map.find("\nlimit ") == std::string::npos && map.find("\nset ") == std::string::npos && map.find("\ntpa") == std::string::npos);
    jpt_custom_level_deactivate();
}

// 3. Nothing passes from one package to the next.
static void TestNoInheritanceBetweenPackages() {
    const Boundary b("boundary next");
    for (int round = 0; round < 2; ++round) {
        CHECK(b.Play("Everything", "All/data/all.scn") && jpt_custom_level_caps() == kEverything && Texture("Map\\all\\rock.bmp") == Bmp8("rock"));
        CHECK(b.Play("Scripts", "scripts.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_SCENE_FORMAT, JPT_LEVEL_CAP_SCRIPT_STATE}));
        CHECK(b.Play("Plain", "plain.scn") && jpt_custom_level_caps() == 0 && !jpt_custom_level_limit("mesh_faces") && !jpt_custom_level_skips_ending());
        CHECK(Setting("dual_stow") == "-" && Added(JPT_LEVEL_TPA_STREAM, 0) == "-" && Found("Effects.tpa") == "-" && Found("menu/li_all_0.tga") == "-" && Found("all.grf") == "-" && Texture("Map\\all\\rock.bmp") == "-" && Found("be.grf") == "-");
    }
    // A package that cannot start leaves none behind it either.
    CHECK(b.Play("Everything", "All/data/all.scn") && !b.Play("Everything", "All/data/gone.scn"));
    CheckCampaign(b.user);
}

// 4. The campaign after a custom level is the campaign before it.
static void TestCampaignAfterCustomLevel() {
    const Boundary b("boundary after");
    CheckCampaign(b.user);
    jpt_custom_level_on_end(CountEnd);
    jpt_custom_level_on_end(CountEnd);
    g_ended = 0;
    for (const auto& [package, scene] : {std::pair("Everything", "All/data/all.scn"), std::pair("Scripts", "scripts.scn"), std::pair("Sounds", "S/data/s.scn")}) {
        const int before = g_ended;
        CHECK(b.Play(package, scene));
        std::string world = World("all");
        jpt_level_world_t prepared;
        jpt_level_world_prepare("all.grf", reinterpret_cast<uint8_t*>(world.data()), world.size(), &prepared);
        CHECK(jpt_level_world_needs("all.wtd") && g_ended == before);
        jpt_custom_level_deactivate();
        CHECK(g_ended == before + 1);
        CheckCampaign(b.user);
        jpt_custom_level_deactivate();
        CHECK(g_ended == before + 1);
    }
}

// 5. Pictures of texhw are a package's: never of the campaign, of another package or of a level of the original game.
static void TestTexhwStaysInSession() {
    const Boundary b("boundary texhw");
    CHECK(Texture("Map\\be\\be.bmp") == "-" && Texture("Map\\all\\rock.bmp") == "-" && Texture("texhw\\Map\\rock.bmp") == "-");
    CHECK(b.Play("Everything", "All/data/all.scn") && Texture("Map\\all\\rock.bmp") == Bmp8("rock") && Texture("Map\\all\\wall.bmp") == Bmp8("wall"));
    jpt_custom_level_scene_loading("be.scn");
    CHECK(Texture("Map\\be\\rock.bmp") == Bmp8("rock") && Texture("Map\\all\\wall.bmp") == Bmp8("wall") && Texture("Map\\be\\sand.bmp") == Bmp8("sand"));
    CHECK(b.Play("Plain", "plain.scn") && Texture("Map\\be\\be.bmp") == "-" && Texture("Map\\be\\sand.bmp") == Bmp8("sand"));
    jpt_custom_level_deactivate();
    CHECK(Texture("Map\\be\\be.bmp") == "-" && Texture("Map\\all\\rock.bmp") == "-" && !jpt_custom_level_is_texture_request("Map\\be\\sand.bmp"));
    for (const char* file : {"src/host/io_trace.c", "src/host/custom_level_files.c", "src/host/missing_textures.c", "src/host/main.c", "src/linux/linux_hle.c", "src/linux/linux_boot.c", "src/common/jpt_vpath.c"})
        CHECK(!SourceFile(file).empty() && SourceFile(file).find("texhw\\\\") == std::string::npos);
}

// 6. A package's sound files are never read by the campaign.
static void TestSoundsStayInSession() {
    const Boundary b("boundary sounds");
    const auto silent = [] { return Added(JPT_LEVEL_TPA_STREAM, 0) == "-" && Added(JPT_LEVEL_TPA_EFFECTS, 0) == "-" && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "-" && Found("Stream.tpa") == "-" && Found("Effects.tpa") == "-" && Found("Ambient.tpa") == "-"; };
    CHECK(silent());
    CHECK(b.Play("Everything", "All/data/all.scn") && Added(JPT_LEVEL_TPA_STREAM, 0) == "More.tpa" && Found("EFFECTS.TPA") == Tpa({{1, "own"}}));
    jpt_custom_level_scene_loading("C:\\data\\be.scn");
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "More.tpa" && Found("Effects.tpa") == Tpa({{1, "own"}}));
    CHECK(b.Play("Sounds", "S/data/s.scn") && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "A.tpa" && Found("Effects.tpa") == "-");
    jpt_custom_level_deactivate();
    CHECK(silent());
}

// 7. A package's settings are never the player's: not in the file, not after the session.
static void TestSettingsStayInSession() {
    const Boundary b("boundary settings");
    const fs::path ini = b.user / "trespasser.ini";
    Write(ini, "dual_stow = 0\nwater_refraction = 0\nenhanced_sky = 0\nactive_ai = original\n");
    const std::string saved = Read(ini);
    const auto session = [&ini](jpt_config_t& config) {
        const char *key, *value;
        int applied = 0;
        jpt_config_load(ini.string().c_str(), &config);
        for (int i = 0; jpt_custom_level_setting(i, &key, &value); ++i) applied += jpt_config_apply(&config, key, value);
        return applied;
    };
    jpt_config_t config;
    const auto original = [&config] { return !config.dual_stow && !config.water_refraction && !config.enhanced_sky && !config.active_ai_enhanced; };
    CHECK(session(config) == 0 && original());
    CHECK(b.Play("Everything", "All/data/all.scn") && session(config) == 4 && config.dual_stow && config.water_refraction && config.enhanced_sky && config.active_ai_enhanced);
    CHECK(b.Play("Stow", "D/data/d.scn") && session(config) == 1 && config.dual_stow && !config.water_refraction && !config.active_ai_enhanced);
    CHECK(b.Play("Plain", "plain.scn") && session(config) == 0 && original());
    jpt_custom_level_deactivate();
    CHECK(session(config) == 0 && original() && Read(ini) == saved);
    CHECK(SourceFile("src/linux/linux_settings.c").find("static void session_ended(void) { s_current = s_players; }") != std::string::npos);
}

// 8. The limits a package's content sizes are 0 without it, and the hosts drop what they kept of them (engine_heaps_tests runs it).
static void TestLimitsReturn() {
    const Boundary b("boundary limits");
    Install(b.user, "Huge", Replaced(Level("", "huge"), "huge.grf", MakeWorld(Meshes("huge", 7900))));
    CHECK(b.Play("Huge", "huge.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_MESH_FACES}) && jpt_custom_level_limit("mesh_faces") == 8192 && !jpt_custom_level_limit("mesh_heap_mb"));
    CHECK(b.Play("Plain", "plain.scn") && !jpt_custom_level_limit("mesh_faces") && !jpt_custom_level_limit("mesh_heap_mb") && !jpt_custom_level_limit("terrain_nodes") && !jpt_custom_level_limit("texture_pool_mb"));
    CHECK(b.Play("Everything", "All/data/all.scn") && jpt_custom_level_limit("mesh_heap_mb") == 64);
    jpt_custom_level_deactivate();
    for (const char* name : {"mesh_faces", "mesh_heap_mb", "terrain_nodes", "terrain_cull_mm", "texture_pool_mb"}) CHECK(!jpt_custom_level_limit(name));
    const std::string heaps = SourceFile("src/host/engine_heaps.c");
    CHECK(heaps.find("jpt_custom_level_on_end(session_ended);") != std::string::npos && heaps.find("memset(&s_session, 0, sizeof s_session);") != std::string::npos);
}

// 9. A custom level's saves are its own.
static void TestSavesStayInSession() {
    const Boundary b("boundary saves");
    const fs::path saves = Fresh("boundary saves folder");
    const std::string slot = (saves / "SAVEGAME.QCK").string();
    char folder[512], other[512], why[256] = "", temp[512];
    const auto gate = [&] { return jpt_gate_load(slot.c_str(), "SAVEGAME.QCK", why, sizeof why); };
    const auto save_as = [&](const std::string& bytes) {
        CHECK(jpt_gate_save_begin(slot.c_str(), temp, sizeof temp));
        Write(temp, bytes);
        return jpt_gate_save_end(slot.c_str(), temp, why, sizeof why) != 0;
    };
    CHECK(jpt_custom_level_saves_dir("saves", "Plain", folder, sizeof folder) && jpt_custom_level_saves_dir("saves", "Sounds", other, sizeof other) && std::string(folder) != other && std::string(folder) != "saves");
    CHECK(b.Play("Plain", "plain.scn") && save_as(Save("plain.grf", "plain.scn")) && gate() == JPT_GATE_OPEN);
    CHECK(b.Play("Sounds", "S/data/s.scn") && gate() == JPT_GATE_INCOMPATIBLE);
    jpt_custom_level_deactivate();
    CHECK(gate() == JPT_GATE_INCOMPATIBLE);
    CHECK(save_as(Save("be.grf", "be.scn")) && gate() == JPT_GATE_OPEN && Read(slot) == Save("be.grf", "be.scn"));
    CHECK(b.Play("Plain", "plain.scn"));
    // Of a level of the original game that the mod led to: the package's save all the same.
    jpt_custom_level_scene_loading("be.scn");
    CHECK(save_as(Save("be.grf", "be.scn")) && gate() == JPT_GATE_OPEN);
    CHECK(b.Play("Sounds", "S/data/s.scn") && gate() == JPT_GATE_INCOMPATIBLE);
    jpt_custom_level_deactivate();
    CHECK(gate() == JPT_GATE_INCOMPATIBLE);
}

// 10. A custom session reads the original game's files and plays its levels as part of the mod, and stays what it was.
static void TestCustomToRetail() {
    const Boundary b("boundary retail");
    char host[1024], why[256];
    for (const char* name : {"next.scn", "last.scn"}) {
        Write(b.game / "data" / name, Scene("be.grf"));
        CHECK(jpt_gate_load("", name, why, sizeof why) == JPT_GATE_OPEN);
    }
    // A mission that ends in levels of the game, and loose levels that go on into one.
    Files mission = Replaced(Everything(), "All/data/all.grf", [] {
        WorldShape shape = Meshes("all", 3000);
        shape.levels = {"next.scn", "last.scn"};
        return MakeWorld(shape);
    }());
    WorldShape one = Meshes("one", 1);
    one.levels = {"two.scn", "next.scn"};
    Install(b.user, "Mission", mission, b.game / "data");
    Install(b.user, "Chain", Replaced(Level("C/data/", "one") + Level("C/data/", "two"), "C/data/one.grf", MakeWorld(one)) + Files{{"C/fm.ini", "[General]\nStartLevel=one.scn\nStreamTPA+=A.tpa\n[next.scn]\nAmbientTPA+=N.tpa\n"}, {"C/A.tpa", Tpa({{1, "a"}})}, {"C/N.tpa", Tpa({{1, "n"}})}}, b.game / "data");
    for (const LevelPackage& package : ListLevelPackages(b.user, b.game / "data"))
        for (const LevelScene& scene : package.scenes)
            if (package.id == "Mission" || package.id == "Chain") CHECK(scene.playable());
    CHECK(b.Play("Mission", "All/data/all.scn"));
    const unsigned caps = jpt_custom_level_caps();
    for (const char* name : {"next.scn", "C:\\data\\LAST.scn", "all.scn", "next.scn"}) {
        CHECK(jpt_gate_load("", name, why, sizeof why) == JPT_GATE_OPEN);
        jpt_custom_level_scene_loading(name);
        CHECK(jpt_custom_level_active() && jpt_custom_level_caps() == caps && std::string(jpt_custom_level_package()) == "Mission" && Setting("dual_stow") == "1" && jpt_custom_level_limit("mesh_heap_mb") == 64 && jpt_custom_level_skips_ending());
        CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "More.tpa" && Texture("Map\\all\\wall.bmp") == Bmp8("wall") && Found("be.grf") == World("be"));
        CHECK(!jpt_custom_level_find("next.scn", host, sizeof host) && jpt_custom_level_game_file("next.scn", host, sizeof host));
    }
    CHECK(b.Play("Chain", "C/data/one.scn") && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "-" && Found("menu/li_x_0.tga") == "-");
    jpt_custom_level_scene_loading("two.scn");
    jpt_custom_level_scene_loading("next.scn");
    CHECK(Added(JPT_LEVEL_TPA_STREAM, 0) == "A.tpa" && Added(JPT_LEVEL_TPA_AMBIENT, 0) == "N.tpa" && std::string(jpt_custom_level_current_scene()).empty() && Found("two.wtd") == "terrain of two");
    jpt_custom_level_deactivate();
    CheckCampaign(b.user);
}

// 11. Without a session every path is the original game's; nothing in the hosts asks whose a file is.
static void TestCampaignPaths() {
    const Boundary b("boundary paths");
    CHECK(b.Play("Everything", "All/data/all.scn"));
    jpt_custom_level_scene_loading("be.scn");
    jpt_custom_level_deactivate();
    CheckCampaign(b.user);

    const std::string scripts = SourceFile("src/host/scene_scripts.c"), menu = SourceFile("src/host/debug_menu.c"), loading = SourceFile("src/host/loading_hd.c"), boot = SourceFile("src/linux/linux_boot.c");
    CHECK(scripts.find("if (g_alias_entry || !jpt_custom_level_requires(JPT_LEVEL_CAP_SCRIPT_STATE)) return;") < scripts.find("MEM32(SAVE_VERSION)"));
    CHECK(menu.find("jpt_custom_level_on_end(forget_start);") != std::string::npos);
    CHECK(loading.find("if (refused()) return;") < loading.find("if (jpt_custom_level_active()) tell_custom_level();"));
    CHECK(boot.find("if (status == LX_BOOT_SETUP_FAILED || status == LX_BOOT_DATA_UNAVAILABLE) jpt_custom_level_deactivate();") != std::string::npos);
    // The adaptation and the capabilities have one door each.
    size_t doors = 0;
    for (const char* folder : {"src/host", "src/linux", "src/common"})
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(JPT_SOURCE_DIR) / folder)) {
            const std::string name = Utf8(entry.path().filename());
            if (entry.path().extension() != ".c" || name == "jpt_scene.c") continue;
            if (Read(entry.path()).find("jpt_scene_adapt(") == std::string::npos) continue;
            ++doors;
            CHECK(name == "jpt_custom_level.c");
        }
    CHECK(doors == 1);
}

// ---- What is told by features, formats and structure, for packages that were never seen. ----

static const LevelScene* SceneNamed(const std::vector<LevelPackage>& packages, const std::string& id) {
    for (const LevelPackage& package : packages)
        if (package.id == id && !package.scenes.empty()) return &package.scenes[0];
    return nullptr;
}

static bool Refused(const std::vector<LevelPackage>& packages, const std::string& id, const std::string& code, const std::string& subject) {
    const LevelScene* scene = SceneNamed(packages, id);
    return scene && std::any_of(scene->issues.begin(), scene->issues.end(), [&](const LevelIssue& i) { return i.blocks() && i.code == code && i.subject == subject; });
}

static Files Custom(const std::string& stem, uint32_t version, const WorldShape& shape) {
    return {{stem + ".scn", SceneOf(stem + ".grf", version)}, {stem + ".grf", MakeWorld(shape)}, {stem + "-130.pid", TextureSet({{TextureHash(shape.meshes[0].texture)}})}, {stem + "-130.swp", "swap"}, {stem + ".wtd", "t"}};
}

static void TestFeaturesOfUnknownPackages() {
    const fs::path user = Fresh("features"), game = g_texture_game = Fresh("features game");
    Write(game / "data" / "be.wtd", "terrain of the game");
    const auto with = [](const char* stem, std::vector<int> actions) {
        WorldShape shape = Meshes(stem, 1);
        shape.actions = std::move(actions);
        return shape;
    };
    Install(user, "A25", Custom("a25", 25, with("a25", {0, 101, 106, 112, 117})), game / "data");
    Install(user, "A26", Custom("a26", 26, with("a26", {})) + Files{{"texhw/Map/crate.dds", "dds"}, {"texhw/Map/a26/other.tga", "tga"}}, game / "data");
    Install(user, "Unknown", Custom("unknown", 22, with("unknown", {36, 107, 400, 1000})), game / "data");
    Install(user, "Later", Custom("later", 22, with("later", {99})), game / "data");
    Files shared = Custom("shared", 22, [&] { WorldShape shape = with("shared", {114}); shape.terrain = "be"; return shape; }());
    shared.erase(std::remove_if(shared.begin(), shared.end(), [](const auto& f) { return f.first == "shared.wtd"; }), shared.end());
    Files moved;
    for (const auto& [path, bytes] : shared) moved.push_back({"S/data/" + path, bytes});
    Install(user, "Shared", moved + Files{{"S/fm.ini", "[General]\nStartLevel=shared.scn\nStreamTPA+=More.tpa\nEnableDualStow=1\n"}, {"S/More.tpa", Tpa({{1, "a"}})}, {"S/data/shared.sky", "[LAYER1]\nType=DOME\n"}}, game / "data");
    Install(user, "Bumped", Custom("bumped", 22, [&] { WorldShape shape = with("bumped", {}); shape.meshes[0].bump = "Map\\bumped\\bump.bmp"; return shape; }()) + Files{{"texhw/Map/crate.dds", "dds"}}, game / "data");
    Files lit = Custom("lit", 22, with("lit", {})), twice;
    Install(user, "Lit", lit + Files{{"texhw/Map/crate.dds", "dds"}, {"texhw/Map/crate.norm.tga", "tga"}}, game / "data");
    for (const auto& [path, bytes] : Custom("twice", 22, with("twice", {}))) twice.push_back({"T/data/" + path, bytes});
    Install(user, "Twice", twice + Files{{"T/fm.ini", "[General]\nStartLevel=twice.scn\n[Render]\nTerrainBackfaceCullBias=0.2\n"}, {"T/data/twice.sky", "[LAYER1]\nType=BODY\nTexture=sun\nBlendMode=2X_ALPHA\n"}}, game / "data");
    const std::vector<LevelPackage> listed = ListLevelPackages(user, game / "data");
    for (const char* id : {"A25", "A26", "Unknown", "Shared", "Lit", "Twice"}) {
        const LevelScene* scene = SceneNamed(listed, id);
        CHECK(scene && scene->playable());
        for (size_t i = 0; scene && i < scene->issues.size(); ++i)
            if (scene->issues[i].blocks()) std::printf("  %s: %s %s: %s\n", id, scene->issues[i].code.c_str(), scene->issues[i].subject.c_str(), scene->issues[i].detail.c_str());
    }
    CHECK(Refused(listed, "Later", "UNSUPPORTED_ACTION", "ActionType 99") && Refused(listed, "Bumped", "UNSUPPORTED_FEATURE", "CE_TEXHW_BUMPED"));
    CHECK(Activate(user, "Twice", "T/data/twice.scn") && jpt_custom_level_requires(JPT_LEVEL_CAP_SKY) && jpt_custom_level_requires(JPT_LEVEL_CAP_TERRAIN_CULL) && jpt_custom_level_limit("terrain_cull_mm") == 200);
    CHECK(Activate(user, "Lit", "lit.scn") && jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) && !jpt_custom_level_requires(JPT_LEVEL_CAP_TERRAIN_CULL) && !jpt_custom_level_limit("terrain_cull_mm"));
    CHECK(Activate(user, "A25", "a25.scn") && jpt_custom_level_action(101) && jpt_custom_level_action(106) && jpt_custom_level_action(117) && !jpt_custom_level_action(114) && !jpt_custom_level_action(0) && jpt_custom_level_requires(JPT_LEVEL_CAP_SCENE_FORMAT));
    CHECK(Activate(user, "A26", "a26.scn") && jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW) && jpt_custom_level_requires(JPT_LEVEL_CAP_SCRIPT_STATE) && !jpt_custom_level_action(101));
    CHECK(Activate(user, "Unknown", "unknown.scn") && !jpt_custom_level_action(107) && !jpt_custom_level_caps());
    CHECK(Activate(user, "Shared", "S/data/shared.scn") && jpt_custom_level_caps() == Caps({JPT_LEVEL_CAP_SOUND_ADDED, JPT_LEVEL_CAP_DUAL_STOW, JPT_LEVEL_CAP_SKY}) && jpt_custom_level_action(114));
    jpt_custom_level_deactivate();
    for (int type = 99; type <= 121; ++type) CHECK(!jpt_custom_level_action(type));
    CheckCampaign(user);
}

// No package and no level is told by its name.
static void TestNamesDecideNothing() {
    static const char* const kNames[] = {"AS2R", "Revae", "Causeway", "Caldera", "EFTLW", "TresCom2", "Tres20", "MistyJungle", "LostJungle", "lostjungle", "Maharaja", "Derelict", "TLWCITY", "TLWSFI", "CityChaos", "MountTres",
                                         "Mount_Tres", "nemlevl", "contestlev", "JurassicGorge", "Castle01", "Castle02", "TresDracul", "PrinceOfPersia", "TresPrince", "TeroJungle", "TstScnRld", "TrespasserCity", "TresPasserCity", "\"complex"};
    size_t files = 0;
    for (const char* folder : {"src/host", "src/linux", "src/common", "src/vulkan", "launcher", "tools"})
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(JPT_SOURCE_DIR) / folder)) {
            const std::string type = Utf8(entry.path().extension());
            if (!entry.is_regular_file() || (type != ".c" && type != ".cpp" && type != ".h" && type != ".inc")) continue;
            const std::string text = Read(entry.path());
            ++files;
            for (const char* name : kNames) {
                const bool named = text.find(name) != std::string::npos;
                if (named) std::printf("  %s names %s\n", Utf8(entry.path().filename()).c_str(), name);
                CHECK(!named);
            }
        }
    CHECK(files > 200);
}

void RunAnalysisTests(const fs::path& scratch, int& checks, int& failures) {
    g_scratch = scratch;
    TestIniParser();
    TestLooseLevels();
    TestAtxFiles();
    TestReadmeRules();
    TestFanMissions();
    TestDocumentation();
    TestConfigProblems();
    TestPictures();
    TestTpaMerge();
    TestManifest();
    TestActivePackage();
    TestTextureRoots();
    TestSessionLimits();
    TestLevelStates();
    TestImportLog();
    TestSceneFormats();
    TestMissingTexture();
    TestListedLevels();
    TestCampaignHasNoCapability();
    TestOnlyRequiredCapabilities();
    TestNoInheritanceBetweenPackages();
    TestCampaignAfterCustomLevel();
    TestTexhwStaysInSession();
    TestSoundsStayInSession();
    TestSettingsStayInSession();
    TestLimitsReturn();
    TestSavesStayInSession();
    TestCustomToRetail();
    TestCampaignPaths();
    TestFeaturesOfUnknownPackages();
    TestNamesDecideNothing();
    checks += g_checks;
    failures += g_failures;
}
