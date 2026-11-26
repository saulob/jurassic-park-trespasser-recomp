// The Custom Levels list off the UI thread (loading first, then the job, stale results dropped, shutdown), the listing record
// that spares analysing a package again, and the READY rule: anything missing, unsupported or unsure refuses a package.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "custom_levels.h"
#include "level_fixtures.h"
#include "level_listing.h"
#include "../launcher/loading_layout.h"
#include "localization/ui_text.h"
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

using Clock = std::chrono::steady_clock;

void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

fs::path Fresh(const char* name) {
    const fs::path dir = g_scratch / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string Scene(const std::string& world, uint32_t version = 22) { return Container({{"Header", HeaderSection(world, version)}}); }

Files Level(const std::string& stem, uint32_t version = 22) {
    return {{stem + ".scn", Scene(stem + ".grf", version)}, {stem + ".grf", World(stem)}, {stem + "-130.pid", Set(stem)}, {stem + "-130.swp", "swap"}, {stem + ".wtd", "terrain"}};
}

Files operator+(Files a, const Files& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

ImportResult Import(const fs::path& user, const char* name, const Files& files, const fs::path& game) {
    const fs::path source = Fresh("listing source") / name;
    for (const auto& [path, bytes] : files) Write(source / PathFromUtf8(path), bytes);
    return ImportLevelFolder(source, user, {}, {}, game);
}

bool Logged(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
}

// Nothing of the package, and no staging folder, is left.
bool Nothing(const fs::path& user, const char* name) {
    std::error_code ec;
    if (fs::exists(CustomLevelsDir(user) / name)) return false;
    for (const auto& entry : fs::directory_iterator(CustomLevelsDir(user), ec))
        if (Utf8(entry.path().filename()).rfind(".import-", 0) == 0) return false;
    return true;
}

// A job that waits until it is let go or cancelled, and says which call it was.
struct FakeScan {
    std::atomic<bool> open{false}, fail{false};
    std::atomic<int> calls{0}, running{0}, most{0}, cancelled{0};

    LevelListing::Work Work() {
        return [this](const ListingRequest& request, const std::atomic<bool>& cancel) {
            const int call = ++calls;
            most = std::max(most.load(), ++running);
            while (!open && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --running;
            if (cancel) ++cancelled;
            if (fail) throw std::runtime_error("disk unplugged");
            Listing listing;
            LevelPackage package;
            package.id = "call " + std::to_string(call) + " " + Utf8(request.data_dir.filename());
            listing.packages.push_back(package);
            listing.stats.cancelled = cancel.load();
            return listing;
        };
    }
};

LevelListing::Frame Until(LevelListing& listing, const ListingRequest& request, bool adopted_or_failed) {
    const Clock::time_point end = Clock::now() + std::chrono::seconds(5);
    LevelListing::Frame frame;
    while (Clock::now() < end) {
        frame = listing.Advance(request);
        if (adopted_or_failed ? frame.adopted || frame.failed : !listing.busy()) return frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frame;
}

void TestLoadingFirst() {
    FakeScan scan;
    const ListingRequest request{g_scratch / "user", g_scratch / "en" / "data"};
    LevelListing listing(scan.Work());

    // The first frame shows the loading view alone: no work has started.
    LevelListing::Frame frame = listing.Advance(request);
    CHECK(frame.view == LevelListing::View::kLoading && !frame.adopted && listing.jobs_started() == 0 && !listing.busy() && scan.calls == 0);
    // The next one starts the job and goes on showing the loading view.
    frame = listing.Advance(request);
    CHECK(frame.view == LevelListing::View::kLoading && listing.jobs_started() == 1 && listing.busy());

    // While the job works, every frame comes back at once: the spinner keeps turning.
    double slowest = 0;
    for (int i = 0; i < 200; ++i) {
        const Clock::time_point start = Clock::now();
        frame = listing.Advance(request);
        slowest = std::max(slowest, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        CHECK(frame.view == LevelListing::View::kLoading && !frame.adopted);
    }
    CHECK(slowest < 20.0 && listing.jobs_started() == 1);

    scan.open = true;
    frame = Until(listing, request, true);
    CHECK(frame.adopted && frame.view == LevelListing::View::kList && listing.list().packages.size() == 1 && listing.list().packages[0].id == "call 1 data");
    frame = listing.Advance(request);
    CHECK(!frame.adopted && frame.view == LevelListing::View::kList && listing.jobs_started() == 1);
    CHECK(scan.most == 1);
}

void TestStaleResults() {
    FakeScan scan;
    const ListingRequest request{g_scratch / "user", g_scratch / "en" / "data"};
    LevelListing listing(scan.Work());
    scan.open = true;
    listing.Advance(request);
    Until(listing, request, true);
    CHECK(listing.list().packages[0].id == "call 1 data");

    // Asked again while a list is shown: the list stays, the job runs behind it.
    scan.open = false;
    listing.Invalidate(false);
    LevelListing::Frame frame = listing.Advance(request);
    CHECK(frame.view == LevelListing::View::kList && listing.jobs_started() == 2 && listing.busy());
    // Asked once more before it ends: its result is never adopted, and a third job makes the list.
    listing.Invalidate(false);
    for (int i = 0; i < 20; ++i) CHECK(listing.Advance(request).view == LevelListing::View::kList);
    scan.open = true;
    frame = Until(listing, request, true);
    CHECK(frame.adopted && listing.list().packages[0].id == "call 3 data" && scan.cancelled >= 1 && scan.most == 1);

    // Another game version: the old list says nothing of it, so the loading view comes first, and the work after it.
    scan.open = false;
    const ListingRequest other{request.user_dir, g_scratch / "es" / "data"};
    const uint64_t before = listing.jobs_started();
    frame = listing.Advance(other);
    CHECK(frame.view == LevelListing::View::kLoading && listing.jobs_started() == before);
    frame = listing.Advance(other);
    CHECK(frame.view == LevelListing::View::kLoading && listing.jobs_started() == before + 1);
    scan.open = true;
    frame = Until(listing, other, true);
    CHECK(frame.adopted && listing.list().packages[0].id == "call 4 data" && scan.most == 1);

    // The page left and came back: the list shows at once while it is checked again.
    listing.Hidden();
    listing.Invalidate(false);
    CHECK(listing.Advance(other).view == LevelListing::View::kList);
    Until(listing, other, true);

    listing.Forget("call 5 data");
    CHECK(listing.list().packages.empty());
}

void TestShutdownAndFailure() {
    // The page goes away during a job: the job is told to stop and waited for, never left running.
    FakeScan scan;
    const ListingRequest request{g_scratch / "user", g_scratch / "en" / "data"};
    {
        LevelListing listing(scan.Work());
        listing.Advance(request);
        listing.Advance(request);
        CHECK(listing.busy());
        const Clock::time_point start = Clock::now();
        while (scan.running == 0 && Clock::now() - start < std::chrono::seconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(scan.running == 0 && scan.cancelled == 1);

    FakeScan failing;
    failing.fail = true;
    failing.open = true;
    LevelListing listing(failing.Work());
    listing.Advance(request);
    LevelListing::Frame frame = Until(listing, request, true);
    CHECK(frame.failed && frame.view == LevelListing::View::kFailed && listing.error() == "disk unplugged");
    frame = listing.Advance(request);
    CHECK(!frame.failed && frame.view == LevelListing::View::kFailed && listing.jobs_started() == 1);
    failing.fail = false;
    listing.Invalidate(false);
    listing.Advance(request);
    frame = Until(listing, request, true);
    CHECK(frame.adopted && frame.view == LevelListing::View::kList);

    BackgroundJob<int> job;
    std::atomic<bool> go{false};
    CHECK(job.Start([&go](const std::atomic<bool>& cancel) {
        while (!go && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 7;
    }) == 1);
    CHECK(job.Start([](const std::atomic<bool>&) { return 8; }) == 0 && job.busy() && !job.Collect());
    go = true;
    std::optional<BackgroundJob<int>::Done> done;
    for (int i = 0; i < 5000 && !done; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(1))) done = job.Collect();
    CHECK(done && done->serial == 1 && done->result && *done->result == 7 && !job.busy() && !job.Collect());
}

void TestListingRecord() {
    const fs::path user = Fresh("record user"), game = Fresh("record game") / "data";
    Write(game / "be.grf", "retail");
    CHECK(Import(user, "Jungle", Level("jungle"), game).ok());
    ListingStats first, second, third;
    const std::vector<LevelPackage> analysed = ListLevelPackages(user, game, &first);
    const std::vector<LevelPackage> cached = ListLevelPackages(user, game, &second);
    CHECK(first.analysed == 1 && first.cached == 0 && second.cached == 1 && second.analysed == 0 && cached[0].cached && !analysed[0].cached);
    CHECK(ListingRecordText("k", cached[0]) == ListingRecordText("k", analysed[0]) && cached[0].dir == analysed[0].dir && cached[0].scenes[0].playable());

    // Whatever the analysis read changes, the package is analysed again.
    const fs::path dir = CustomLevelsDir(user) / "Jungle";
    Write(dir / "jungle.wtd", "terrain of another release");
    ListLevelPackages(user, game, &third);
    CHECK(third.analysed == 1);
    ListingStats after;
    const auto list = [&](const fs::path& data) {
        after = {};
        ListLevelPackages(user, data, &after);
    };
    list(game);
    CHECK(after.cached == 1);
    Write(game / "other.grf", "a file the game gained");
    list(game);
    CHECK(after.analysed == 1);
    Write(dir / ".runtime" / "listing.txt", "custom-level-listing 1\nkey x\nend\n");
    list(game);
    CHECK(after.analysed == 1);
    list(game);
    CHECK(after.cached == 1);
    list(game.parent_path().parent_path() / "es" / "data");
    CHECK(after.analysed == 1);

    // A record is read back as it was written, and only under its own key.
    const LevelPackage& p = cached[0];
    LevelPackage read;
    CHECK(ParseListingRecord(ListingRecordText("key", p), "key", read) && ListingRecordText("key", read) == ListingRecordText("key", p));
    CHECK(!ParseListingRecord(ListingRecordText("key", p), "other", read) && !ParseListingRecord(ListingRecordText("key", p), "", read));
    std::string cut = ListingRecordText("key", p);
    cut.resize(cut.size() - 4);
    CHECK(!ParseListingRecord(cut, "key", read));
    LevelPackage odd = p;
    odd.log = {"a | b \\ c\nd"};
    CHECK(ParseListingRecord(ListingRecordText("key", odd), "key", read) && read.log == odd.log);
    CHECK(GameListingKey(game) != GameListingKey(game.parent_path() / "x" / "data") && GameListingKey({}) == "no game");
    CHECK(PackageListingKey(dir, "a") != PackageListingKey(dir, "b") && PackageListingKey(dir, "a") == PackageListingKey(dir, "a") && PackageListingKey(dir / "none", "a").empty());

    // A package an earlier version installed READY_WITH_WARNINGS is analysed with today's rule: no Play.
    for (const auto& [path, bytes] : Level("old") + Files{{"old.sky", "[LAYER1]\nType=STARS\n"}}) Write(CustomLevelsDir(user) / "Old" / path, bytes);
    for (const LevelPackage& package : ListLevelPackages(user, game))
        if (package.id == "Old") CHECK(package.levels().empty() && !ActionsOf(package.rows()[0], true).play && ActionsOf(package.rows()[0], true).report);
}

void TestReadyRule() {
    const fs::path user = Fresh("rule user"), game = Fresh("rule game") / "data";
    const auto refused = [&](const char* name, const Files& files, ImportStatus status, const char* logged) {
        const ImportResult r = Import(user, name, files, game);
        const bool ok = !r.ok() && r.status == status && r.levels == 0 && Nothing(user, name) && Logged(r.log, logged) && Logged(r.log, "result: rejected");
        if (!ok) std::printf("  %s: %s\n", name, ImportStatusTitle(r.status));
        return ok;
    };
    CHECK(Import(user, "Ready", Level("ready"), game).ok());
    CHECK(refused("Missing", {{"lack.scn", Scene("lack.grf")}, {"lack.grf", World("lack")}, {"lack.wtd", "t"}}, ImportStatus::kMissingFiles, "MISSING_REQUIRED_TEXTURE_SET"));
    CHECK(refused("Textures", {{"pic.scn", Scene("pic.grf")}, {"pic.grf", World("pic")}, {"pic-130.pid", TextureSet({{1}})}, {"pic-130.swp", "s"}, {"pic.wtd", "t"}}, ImportStatus::kMissingFiles,
                  "MISSING_TEXTURES pic.grf"));
    // A level that leads to a scene nowhere is ready: the game stays in it when that scene is asked for.
    {
        WorldShape shape;
        shape.terrain = "go";
        shape.meshes.push_back({"Crate-00", "Map\\go\\crate.bmp"});
        shape.levels = {"nowhere.scn"};
        Files files = Level("go");
        files[1].second = MakeWorld(shape);
        const ImportResult r = Import(user, "Transition", files, game);
        CHECK(r.ok() && Logged(r.log, "info: MISSING_TRANSITION_LEVEL nowhere.scn") && Logged(r.log, "class: READY"));
        bool noted = false;
        for (const LevelPackage& package : ListLevelPackages(user, game))
            for (const LevelScene& scene : package.scenes)
                if (package.id == r.package) noted = scene.playable() && LevelNotes(scene) == std::vector<std::string>{"nowhere.scn"};
        CHECK(noted);
    }
    CHECK(refused("Sky", Level("sky") + Files{{"sky.sky", "[LAYER1]\nType=STARS\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE CE_SKY_STARS"));
    CHECK(refused("Script", Level("nut") + Files{{"scripts/x.nut", "class X {}"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE CE_SCRIPTS"));
    CHECK(refused("Action", [] {
        WorldShape shape;
        shape.terrain = "act";
        shape.meshes.push_back({"Crate-00", "Map\\act\\crate.bmp"});
        shape.actions = {99};
        Files files = Level("act");
        files[1].second = MakeWorld(shape);
        return files;
    }(), ImportStatus::kNotSupported, "UNSUPPORTED_ACTION ActionType 99"));
    CHECK(!refused("Optional", Level("opt") + Files{{"MAP.bmp", "b"}, {"readme.txt", "IF ATX IS INSTALLED, place MAP.bmp in the MAPS directory."}}, ImportStatus::kNotSupported, ""));
    CHECK(refused("Settings", Level("set") + Files{{"fm.ini", "[General]\nStartLevel=set.scn\nEndGameDDF=end\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE CE_END_GAME_DDF"));
    CHECK(refused("EngineFile", Level("set") + Files{{"trespasser.ini", "[Game]\nEnableAiActivityEx=-1\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE CE_EXTENDED_AI"));
    const Files atx = {{"readme.txt", "This level requires ATX."}};
    CHECK(!refused("AtxRequired", Level("atx") + atx, ImportStatus::kNotSupported, ""));
    CHECK(refused("AtxJumps", Level("atx") + atx + Files{{"MAPS/JMP/atx.ini", "[General]\nPush=65\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE ATX_JUMPS"));
    CHECK(refused("AtxThrows", Level("atx") + atx + Files{{"MAPS/SPT/atx.ini", "[Spitter]\nProjectileName1=Spit\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE ATX_PROJECTILES"));
    CHECK(refused("AtxRules", Level("atx") + atx + Files{{"ATXconfig.ini", "[Variables]\nGravity=3.0\n"}}, ImportStatus::kNotSupported, "UNSUPPORTED_FEATURE ATX_SETTINGS"));
    CHECK(!refused("AtxOwnRules", Level("atx") + atx + Files{{"ATXconfig.ini", "[Variables]\nGravity=10.000000\nNormalGameSpeed=0\n[RainEffect]\nFeatureEnabled=1\n"}}, ImportStatus::kNotSupported, ""));
    CHECK(!refused("AtxOthers", Level("atx") + atx + Files{{"MAPS/JMP/example.ini", "[General]\nPush=65\n"}, {"MAPS/HUD/0.bmp", "b"}, {"MAPS/RAN/atx.ini", "[Instances]\n"}, {"MAPS/RNE/default.ini", "[CParticles]\n"}}, ImportStatus::kNotSupported, ""));
    CHECK(!refused("AtxForCe", Level("atx") + Files{{"MAPS/JMP/atx.ini", "[General]\nPush=65\n"}}, ImportStatus::kNotSupported, ""));
    // Words alone refuse nothing that the port has, and a setting counts only where Trespasser CE reads it.
    CHECK(!refused("Required", Level("ce") + Files{{"readme.txt", "This level requires Trespasser CE."}}, ImportStatus::kNotSupported, ""));
    CHECK(!refused("Elsewhere", Level("set") + Files{{"fm.ini", "[General]\nStartLevel=set.scn\nDisableSimpleLevelRestart=True\n"}}, ImportStatus::kNotSupported, ""));
    CHECK(!refused("ByHand", Level("set") + Files{{"trespasser.mine.ini", "[Game]\nEnableAiActivityEx=-1\n"}}, ImportStatus::kNotSupported, ""));
    CHECK(refused("Newer", Level("late", 27), ImportStatus::kUnsupportedFormat, "class: REJECTED_UNSUPPORTED"));

    // What only informs never refuses: a readme, its author's own note, a scene read as the game's own, a world without terrain.
    WorldShape bare;
    bare.meshes.push_back({"Crate-00", "Map\\\\crate.bmp"});
    const ImportResult info = Import(user, "Info", Level("v25", 25) + Files{{"readme.txt", "Restart Level does not work in this level."}, {"bare.scn", Scene("bare.grf")}, {"bare.grf", MakeWorld(bare)},
                                                                          {"bare-130.pid", Set("")}, {"bare-130.swp", "s"}}, game);
    CHECK(info.ok() && info.levels == 2 && Logged(info.log, "info: NO_RESTART") && Logged(info.log, "info: NO_TERRAIN bare.grf") && Logged(info.log, "class: READY") && !Logged(info.log, "REJECTED"));
    CHECK(Logged(info.log, "result: installed"));

    for (const ImportStatus status : {ImportStatus::kMissingFiles, ImportStatus::kNotSupported, ImportStatus::kUnsupportedFormat, ImportStatus::kUnsafeToLoad}) {
        const LevelNotice notice = ImportNotice(status);
        CHECK(notice.failed && notice.title == "Could not import this custom level" && notice.reason == "This custom level is not fully supported");
    }
    CHECK(!ImportNotice(ImportStatus::kInstalled).failed && !ImportNotice(ImportStatus::kCancelled).failed && ImportNotice(ImportStatus::kNoSpace).reason == "There is not enough free disk space");
    CHECK(!ActionsOf(nullptr, true).play && ActionsOf(nullptr, true).report);
}

void TestLoadingLayoutAndText() {
    // The same proportions at every scale: text over the spinner, both centered, the block centered in the space left.
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float text_w = 180 * scale, text_h = 16 * scale;
        const LoadingLayout l = LayOutLoading(scale, 10, 20, 800 * scale, 600 * scale, text_w, text_h);
        CHECK(l.radius == 10.0f * scale && l.thickness == 3.0f * scale && l.spinner_x == 10 + 400 * scale && l.text_x + text_w * 0.5f == l.spinner_x);
        CHECK(l.spinner_y - l.radius - (l.text_y + text_h) == 14.0f * scale && l.bottom == l.spinner_y + l.radius);
        CHECK(std::abs((l.text_y - 20) - (20 + 600 * scale - l.bottom)) < 0.01f);
    }
    const LoadingLayout tight = LayOutLoading(1.0f, 0, 0, 100, 5, 50, 16);
    CHECK(tight.text_y == 0);

    SetLauncherLanguage("pt-BR");
    for (const char* english : {"Loading custom levels...", "Reading the package...", "Copying the level files...", "Checking the level files...", "Could not import this custom level", "This custom level is not fully supported", "Not supported",
                                "The custom levels could not be listed", "Bug Report", "Close", "Open Folder", "Delete", "Cancel"})
        CHECK(std::string(Tr(english)) != english);
    CHECK(std::string(Tr("Loading custom levels...")) == "Carregando níveis personalizados...");
    SetLauncherLanguage("en");
}

void TestImportProgress() {
    ImportProgress p;
    CHECK(p.done() == 0 && ImportProgress::kTotal == 1000);
    p.Phase(kImportPhases[0]);
    CHECK(p.done() == 0);
    p.Phase(kImportPhases[1]);
    const long long copying = p.done();
    p.Bytes(10, 100);
    const long long tenth = p.done();
    p.Bytes(100, 100);
    const long long copied = p.done();
    CHECK(copying > 0 && tenth > copying && copied > tenth);
    // Nothing takes it back: an earlier phase, fewer bytes, an unknown phase.
    p.Bytes(5, 100);
    p.Phase(kImportPhases[0]);
    p.Phase("something else");
    CHECK(p.done() == copied);
    p.Phase(kImportPhases[2]);
    p.Bytes(100, 100);
    CHECK(p.done() >= copied && p.done() < ImportProgress::kTotal);
    p.Bytes(1u << 30, 10);
    CHECK(p.done() < ImportProgress::kTotal);
    p.Complete();
    CHECK(p.done() == ImportProgress::kTotal);
}

// The import on a worker thread, as the page runs it: what it publishes is read by this thread alone, under a lock.
struct Published {
    std::mutex lock;
    std::vector<std::pair<std::string, long long>> steps;
    std::vector<std::thread::id> threads;
    long long total = 0;
};

ImportResult ImportOnWorker(const ImportRequest& request, Published& seen, int& frames) {
    std::atomic<bool> finished{false};
    ImportResult result;
    std::thread worker([&] {
        result = RunImport(request, [&seen](const std::string& phase, long long done, long long total) {
            std::lock_guard<std::mutex> hold(seen.lock);
            seen.steps.push_back({phase, done});
            seen.threads.push_back(std::this_thread::get_id());
            seen.total = total;
        }, [] { return false; });
        finished = true;
    });
    for (frames = 0; !finished; ++frames) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    worker.join();
    return result;
}

void TestImportOnWorker() {
    const fs::path user = Fresh("progress user"), game = Fresh("progress game") / "data", source = Fresh("progress source");
    Files files = Level("ready");
    for (int i = 0; i < 8; ++i) files.push_back({"ready_" + std::to_string(i) + ".txt", std::string(4096, char('a' + i))});
    for (const auto& [path, bytes] : files) Write(source / "Ready" / PathFromUtf8(path), bytes);
    Published ok;
    int frames = 0;
    const ImportResult installed = ImportOnWorker({source / "Ready", user, game, true}, ok, frames);
    std::vector<long long> done;
    for (const auto& step : ok.steps) done.push_back(step.second);
    CHECK(installed.ok() && ok.total == ImportProgress::kTotal && !done.empty() && done.front() == 0 && done.back() == ImportProgress::kTotal);
    CHECK(std::is_sorted(done.begin(), done.end()) && std::count(done.begin(), done.end(), ImportProgress::kTotal) == 1);
    CHECK(std::adjacent_find(done.begin(), done.end(), std::less<long long>()) != done.end() && std::set<long long>(done.begin(), done.end()).size() >= 4);
    const auto first = [&ok](const char* phase) {
        return size_t(std::find_if(ok.steps.begin(), ok.steps.end(), [phase](const auto& s) { return s.first == phase; }) - ok.steps.begin());
    };
    CHECK(first(kImportPhases[0]) < first(kImportPhases[1]) && first(kImportPhases[1]) < first(kImportPhases[2]) && first(kImportPhases[2]) < ok.steps.size());
    CHECK(std::all_of(ok.threads.begin(), ok.threads.end(), [](std::thread::id id) { return id != std::this_thread::get_id(); }) && frames >= 0);

    // A refused package keeps what it reached: never 100%.
    Write(source / "Lacking" / "lack.scn", Scene("lack.grf"));
    Write(source / "Lacking" / "lack.grf", World("lack"));
    Published refused;
    const ImportResult lacking = ImportOnWorker({source / "Lacking", user, game, true}, refused, frames);
    long long most = 0;
    for (const auto& step : refused.steps) most = std::max(most, step.second);
    CHECK(!lacking.ok() && !refused.steps.empty() && most < ImportProgress::kTotal && refused.steps.back().second == most);
    Published unreadable;
    CHECK(!ImportOnWorker({source / "nowhere.zip", user, game, false}, unreadable, frames).ok() && unreadable.steps.back().second < ImportProgress::kTotal);
}

} // namespace

void RunListingTests(const fs::path& scratch, int& checks, int& failures) {
    g_scratch = scratch / "listing";
    TestLoadingFirst();
    TestStaleResults();
    TestShutdownAndFailure();
    TestListingRecord();
    TestReadyRule();
    TestLoadingLayoutAndText();
    TestImportProgress();
    TestImportOnWorker();
    checks += g_checks;
    failures += g_failures;
}
