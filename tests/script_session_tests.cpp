// The Trespasser CE scripts of a custom session (jpt_script_session.cpp, jpt_level_scripts.cpp), headless, over scripts
// written here: one VM per level that needs it, the CE load steps and callbacks in their order, the frame step, transitions,
// Restart Level, errors and budgets of one script or of the whole VM, and a campaign that never makes any of it.
#include "script_fixtures.h"

// alpha uses a script, beta does not: the decision is the level's, never the package's.
static void test_level_without_scripts() {
    jpt_script_session* s = Session({{"scripts/shared.nut", kShared}});
    CHECK(!jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 0);
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 1);
    CHECK(Has("[script] vm create level=alpha.scn generation=1 scripts=1 files=1 memory_limit=16777216"));
    CHECK(Has("[script] loaded file=scripts/shared.nut") && Has(Print("alpha made")));
    jpt_script_level_ready(s, 1);
    CHECK(InOrder({"[script] print: alpha init false steps=0", "[script] print: alpha start steps=0",
                   "[script] callback level_start level=alpha.scn scripts=1"}));
    int stepping = 0;
    CHECK(jpt_script_session_scripts(s, &stepping) == 1 && stepping == 1);
    Steps(s, 3);

    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(InOrder({"[script] print: alpha end true", "[script] callback level_end level=alpha.scn scripts=1", "[script] print: alpha purge",
                   "[script] print: alpha gone"}));
    CHECK(Contains("[script] vm destroy level=alpha.scn generation=1 peak="));
    CHECK(!Contains("level=beta.scn"));
    CHECK(!jpt_script_session_has_vm(s) && g_bytes == 0 && jpt_script_session_generation(s) == 1);
    jpt_script_level_ready(s, 1);
    Steps(s, 2);
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_RESTART);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 1 && Count("[script] vm create") == 1);

    g_lines.clear();
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(jpt_script_session_has_vm(s) && jpt_script_session_generation(s) == 2 && Has(Print("alpha made")));
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("alpha init false steps=0")) && Has(Print("alpha start steps=0")));
    // A retail level of the same custom session: the same rule.
    Begin(s, "be.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(!jpt_script_session_has_vm(s) && !Contains("level=be.scn"));
    Close(s);
    CHECK(Has("[script] session end vms=2"));
}

static void test_lifecycle_order() {
    jpt_script_session* s = Session({{"scripts/game.nut", kGame}, {"scripts/shared.nut", kShared}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(Has("[script] vm create level=alpha.scn generation=1 scripts=2 files=2 memory_limit=16777216"));
    CHECK(InOrder({"[script] loaded file=scripts/game.nut", "[script] loaded file=scripts/shared.nut", "[script] print: game made", "[script] print: alpha made"}));
    jpt_script_level_ready(s, 1);
    CHECK(InOrder({"[script] print: game init false levels=0 steps=0", "[script] print: alpha init false steps=0", "[script] print: game start",
                   "[script] print: game level start levels=1", "[script] print: alpha start steps=0",
                   "[script] callback level_start level=alpha.scn scripts=2"}));
    Steps(s, 5);

    // A transition: the level ends, its VM goes, and the game script's nvram travels to the next level's.
    g_lines.clear();
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(InOrder({"[script] print: game end true", "[script] print: alpha end true", "[script] callback level_end level=alpha.scn scripts=2",
                   "[script] print: game purge", "[script] print: alpha purge", "[script] print: game gone", "[script] print: alpha gone"}));
    CHECK(Has("[script] vm create level=beta.scn generation=2 scripts=1 files=1 memory_limit=16777216") && !Has(Print("alpha made")));
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("game init true levels=1 steps=5")) && Has(Print("game level start levels=2")) && !Has(Print("game start")));
    Steps(s, 1);
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("game init true levels=2 steps=6")) && Has(Print("game level start levels=3")) && Has(Print("alpha start steps=0")));
    // A failed load: the scripts stop with it.
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 0);
    CHECK(Has("[script] level=beta.scn the game's load failed: its scripts stop") && !jpt_script_session_has_vm(s) && g_bytes == 0);
    Close(s);
}

static void test_restart() {
    jpt_script_session* s = Session({{"scripts/game.nut", kGame}, {"scripts/shared.nut", kShared}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Steps(s, 4);
    const unsigned before = jpt_script_session_generation(s);
    g_lines.clear();
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(!Contains("game end") && !Contains("callback level_end") && Has(Print("game purge")) && Has(Print("alpha gone")));
    CHECK(jpt_script_session_generation(s) == before + 1 && jpt_script_session_has_vm(s));
    jpt_script_level_ready(s, 1);
    // The game script is as it was right before OnLevelStart; the level script is new.
    CHECK(Has(Print("game init false levels=0 steps=0")) && Has(Print("game level start levels=1")) && !Has(Print("game start")));
    CHECK(Has(Print("alpha init false steps=0")) && Has(Print("alpha start steps=0")));
    for (int i = 0; i < 12; ++i) {
        Steps(s, 3);
        const size_t held = Held(s);
        CHECK(held > 0 && held == g_bytes);
        Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_RESTART);
        jpt_script_level_ready(s, 1);
        CHECK(jpt_script_session_generation(s) == before + 2 + static_cast<unsigned>(i) && Held(s) == g_bytes);
    }
    CHECK(Count(Print("game level start levels=1")) == 13 && !Contains("levels=2") && Count(Print("game start")) == 0);
    for (int i = 0; i < 12; ++i) {
        Begin(s, i % 2 ? "alpha.scn" : "beta.scn", JPT_SCRIPT_BEGIN_TRANSITION);
        jpt_script_level_ready(s, 1);
        CHECK(Held(s) == g_bytes);
    }
    CHECK(Has(Print("game level start levels=13")));
    Close(s);
}

static void test_scripts_apart() {
    const Files files = {
        {"scripts/m1.nut", "class A extends LevelScript { n = 0; function OnSimStep(dt) { n++; if (n == 2) { local t = {}; t.missing; } print(\"A \" + n); } }\n"},
        {"scripts/m2.nut", "class B extends LevelScript { n = 0; function OnSimStep(dt) { n++; print(\"B \" + n); } }\n"},
        {"scripts/m3.nut", "class C extends LevelScript { function OnSimStep(dt) { while (true) {} } }\n"},
        {"scripts/m4.nut", "class D extends LevelScript { function OnLevelStart() { print(\"D start\"); } }\n"},
    };
    jpt_script_session* s = Session(files);
    Begin(s, "multi.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    int stepping = 0;
    CHECK(jpt_script_session_scripts(s, &stepping) == 4 && stepping == 3);
    Steps(s, 5);
    CHECK(InOrder({"[script] print: A 1", "[script] print: B 1", "[script] print: B 2", "[script] print: B 5"}));
    CHECK(!Has(Print("A 2")) && !Has(Print("A 3")));
    CHECK(Has("[script] level=multi.scn script=A file=scripts/m1.nut callback=OnSimStep line=1 result=disabled error=the index 'missing' does not exist"));
    CHECK(Count("[script] level=multi.scn script=A ") == 1 && Count("[script] level=multi.scn script=C ") == 1);
    CHECK(Contains("script=C file=scripts/m3.nut callback=OnSimStep line=1 result=disabled error=instruction budget exceeded (200000)"));
    // Stopped until the level ends: a new level's VM runs them all again.
    Begin(s, "multi.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    g_lines.clear();
    Steps(s, 1);
    CHECK(Has(Print("A 1")) && Has(Print("B 1")));
    Close(s);

    // One memory budget for the VM: a script that takes all of it stops every script of the level, and the next level has a VM.
    const Files hog = {
        {"scripts/b.nut", "class B extends LevelScript { n = 0; function OnSimStep(dt) { n++; print(\"B \" + n); } }\n"},
        {"scripts/hog.nut", "class Hog extends LevelScript { n = 0; function OnSimStep(dt) { if (++n >= 3) { local a = []; while (true) a.append(a.len()); } } }\n"},
    };
    s = Session(hog, 2u * 1024 * 1024, 2000000000L);
    Begin(s, "hog.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Steps(s, 6);
    CHECK(Has(Print("B 3")) && !Has(Print("B 4")));
    CHECK(Has("[script] level=hog.scn script=Hog callback=OnSimStep result=vm_disabled error=memory budget exceeded (limit 2097152 bytes)"));
    CHECK(Count("[script] level=hog.scn result=vm_disabled every script of this VM stops") == 1);
    jpt_script_memory m;
    CHECK(jpt_script_session_memory(s, &m) && m.abandoned && m.current == 0 && g_bytes == 0);
    CHECK(SaveOf(s).empty());
    Begin(s, "next.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    g_lines.clear();
    Steps(s, 1);
    CHECK(Has(Print("B 1")) && jpt_script_session_memory(s, &m) && !m.abandoned);
    Close(s);
}

static void test_load_errors() {
    const Files files = {
        {"scripts/a.nut", "class G extends GameScript { function OnLevelStart() { print(\"G \" + gHelper()); } }\n"},
        {"scripts/b.nut", "function gHelper() { return 1; }\n"},
        {"scripts/c.nut", "class Bad extends GameScript { function OnLevelStart() { local = ; } }\n"},
        {"scripts/d.nut", "class T extends GameScript { function OnLevelStart() { print(\"T start\"); } }\nlocal x = null;\nx.y = 1;\n"},
    };
    jpt_script_session* s = Session(files);
    g_fail_after["scripts/b.nut"] = 1;
    Begin(s, "x.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(Has("[script] level=x.scn file=scripts/b.nut callback=load result=missing"));
    CHECK(Contains("[script] level=x.scn file=scripts/c.nut callback=load line=1 column=") && Contains("result=failed"));
    CHECK(Has("[script] level=x.scn script=Bad result=disabled error=it is not a class derived from GameScript once loaded"));
    CHECK(Contains("[script] level=x.scn script=scripts/d.nut file=scripts/d.nut callback=load line=3 function=main result=failed error="));
    jpt_script_level_ready(s, 1);
    CHECK(Contains("[script] level=x.scn script=G file=scripts/a.nut callback=OnLevelStart line=1 result=disabled error="));
    CHECK(Has(Print("T start")));
    Close(s);

    // What the port does not have yet: no VM, and the names in the log.
    s = Session({{"scripts/hud.nut", "class Hud extends GameScript { function OnLevelStart() { TP_Engine.GetPlayer(); } }\n"}});
    Begin(s, "x.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(!jpt_script_session_has_vm(s) && Has("[script] level=x.scn vm=none result=refused error=its scripts need what the port does not have: GetPlayer,TP_Engine"));
    Close(s);
    s = Session({{"scripts/door.nut", "class Door extends CInstance { function OnUse(b) { return true; } }\n"}});
    jpt_script_session_add_reference(s, "levels/X.SCN", "Door", "Door-00", 0);
    jpt_script_session_add_reference(s, "x.scn", "Ghost", "Box-00", 0);
    Begin(s, "x.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(!jpt_script_session_has_vm(s) && Contains("CInstance,OnUse,Box-00 names the script class Ghost"));
    Begin(s, "y.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(!Contains("level=y.scn"));
    Close(s);

    // A VM that cannot open within its memory limit is not there; the level goes on without it.
    s = Session({{"scripts/game.nut", kGame}}, 1024);
    Begin(s, "x.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Steps(s, 2);
    CHECK(!jpt_script_session_has_vm(s) && Has("[script] level=x.scn result=vm_disabled error=the VM could not open within its memory limit"));
    Close(s);
}

// The game's way: a level reads only the files the launcher found for it; one without any reads none, makes no VM, says nothing.
static void test_per_level_files() {
    g_lines.clear();
    g_files = {{"scripts/beta.nut", "class BetaRules extends LevelScript { static SCN = \"beta.scn\"; function OnLevelStart() { print(\"beta\"); } }\n"},
               {"scripts/extra.nut", "class Spare extends GameScript { function OnLevelStart() { print(\"spare\"); } }\n"}};
    g_reads.clear();
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.allocator = &kFake;
    config.per_level_files = 1;
    jpt_script_session* s = jpt_script_session_create(&config);
    jpt_script_session_add_file(s, "scripts/beta.nut");
    jpt_script_session_add_file(s, "scripts/extra.nut");
    jpt_script_session_add_level_file(s, "levels/BETA.SCN", "scripts/beta.nut");
    for (const char* scene : {"alpha.scn", "delta.scn"}) {
        Begin(s, scene, JPT_SCRIPT_BEGIN_NEW_GAME);
        jpt_script_level_ready(s, 1);
        jpt_script_level_step(s, 0.1f);
        CHECK(!jpt_script_session_has_vm(s) && g_reads.empty() && g_lines.empty() && jpt_script_session_generation(s) == 0);
    }
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(jpt_script_session_has_vm(s) && Has(Print("beta")) && !Has(Print("spare")));
    CHECK(g_reads.size() == 1 && g_reads.count("scripts/beta.nut") && g_reads["scripts/beta.nut"] == 2);
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(!jpt_script_session_has_vm(s) && g_reads.size() == 1);
    Close(s);
}

static void test_campaign_and_package() {
    // The campaign: nothing is created, nothing is logged, its saves have no script block.
    g_lines.clear();
    jpt_level_scripts_start(JPT_SESSION_CAMPAIGN, CaptureLine);
    CHECK(jpt_level_scripts_session() == nullptr);
    jpt_level_scripts_begin(JPT_REASON_NEW_GAME, "be.scn", nullptr);
    jpt_level_scripts_ready(1);
    jpt_level_scripts_step(0.1f);
    jpt_level_scripts_begin(JPT_REASON_TRANSITION, "jr.scn", nullptr);
    jpt_level_scripts_ready(1);
    jpt_level_scripts_begin(JPT_REASON_RESTART, "jr.scn", nullptr);
    jpt_level_scripts_ready(1);
    const fs::path dir = Folder();
    CHECK(Saved(dir, "SAVEGAME.001", ""));
    char problem[128];
    size_t size = 7;
    CHECK(jpt_save_read_scripts((dir / "SAVEGAME.001").string().c_str(), &size, problem, sizeof problem) == nullptr && size == 0);
    jpt_level_scripts_begin(JPT_REASON_LOAD_SAVE, "jr.scn", (dir / "SAVEGAME.001").string().c_str());
    jpt_level_scripts_ready(1);
    CHECK(Slurp(dir / "SAVEGAME.001").find("JPTSCRPT") == std::string::npos);
    CHECK(g_lines.empty() && jpt_level_scripts_session() == nullptr);
    // A custom session without an active package is none either.
    jpt_level_scripts_start(JPT_SESSION_CUSTOM, CaptureLine);
    CHECK(jpt_level_scripts_session() == nullptr);

    // A custom package: its runtime map lists its scripts in load order.
    const fs::path pkg = dir / "custom_levels" / "Pkg";
    Write(pkg / "alpha.scn", fixtures::Container({{"Header", fixtures::HeaderSection("alpha.grf", 22)}}));
    Write(pkg / "beta.scn", fixtures::Container({{"Header", fixtures::HeaderSection("beta.grf", 22)}}));
    Write(pkg / "scripts" / "game.nut", kGame);
    Write(pkg / "scripts" / "shared.nut", kShared);
    Write(pkg / "scripts" / "unused.nut", "class Never extends LevelScript { static SCN = \"none.scn\"; function OnLevelStart() {} }\n");
    Write(pkg / JPT_CUSTOM_LEVEL_RUNTIME_MAP, "trespasser-runtime 1\nscene alpha.scn\nscene beta.scn\nnut scripts/game.nut\nnut scripts/shared.nut\nnut scripts/unused.nut\nnut scripts/absent.nut\n"
                                                "uses alpha.scn|scripts/game.nut\nuses alpha.scn|scripts/shared.nut\n");
    jpt_custom_level_request_t request;
    memset(&request, 0, sizeof request);
    snprintf(request.package, sizeof request.package, "Pkg");
    snprintf(request.scene, sizeof request.scene, "alpha.scn");
    char error[256];
    CHECK(jpt_custom_level_activate(dir.string().c_str(), &request, error, sizeof error));
    CHECK(jpt_custom_level_script_file(0) && !strcmp(jpt_custom_level_script_file(0), "scripts/game.nut"));
    CHECK(jpt_custom_level_script_file(2) && !jpt_custom_level_script_file(3));
    g_lines.clear();
    jpt_level_scripts_start(JPT_SESSION_CUSTOM, CaptureLine);
    jpt_script_session* s = jpt_level_scripts_session();
    CHECK(s != nullptr && g_lines.empty());
    jpt_level_scripts_begin(JPT_REASON_INITIAL, "c:\\tres\\data\\alpha.scn", nullptr);
    jpt_level_scripts_ready(1);
    CHECK(Has("[script] vm create level=alpha.scn generation=1 scripts=2 files=2 memory_limit=16777216"));
    CHECK(!Contains("unused.nut") && Has(Print("game start")));
    jpt_level_scripts_step(0.25f);

    g_state_bytes = 5;
    jpt_save_set_state(SomeState);
    CHECK(Saved(dir, "SAVEGAME.002", "Pkg") && Saved(dir, "temp.scx", "Pkg"));
    jpt_save_set_state(nullptr);
    uint8_t* block = jpt_save_read_scripts((dir / "SAVEGAME.002").string().c_str(), &size, problem, sizeof problem);
    std::vector<jpt::script::Entry> entries;
    std::string why;
    CHECK(block && !problem[0] && jpt::script::DecodeState(block, size, entries, why) && entries.size() == 3);
    free(block);
    uint8_t lstate[16];
    CHECK(jpt_save_read_state((dir / "SAVEGAME.002").string().c_str(), lstate, sizeof lstate) == 5);

    g_lines.clear();
    jpt_level_scripts_begin(JPT_REASON_LOAD_SAVE, "alpha.scn", (dir / "SAVEGAME.002").string().c_str());
    jpt_level_scripts_ready(1);
    CHECK(Has(Print("game init true levels=1 steps=1")) && Has(Print("alpha init true steps=1")));
    // The game's temporary save, written while its display is rebuilt, loads as a save, never as a transition.
    g_lines.clear();
    jpt_level_scripts_begin(JPT_REASON_TRANSITION, "alpha.scn", (dir / "temp.scx").string().c_str());
    jpt_level_scripts_ready(1);
    CHECK(!Contains("game end") && Has(Print("game init true levels=1 steps=1")));
    // A damaged block is left out with its reason; the save itself loads.
    std::string bytes = Slurp(dir / "SAVEGAME.002");
    Write(dir / "SAVEGAME.003", bytes.substr(0, bytes.size() - 2));
    g_lines.clear();
    jpt_level_scripts_begin(JPT_REASON_LOAD_SAVE, "alpha.scn", (dir / "SAVEGAME.003").string().c_str());
    CHECK(Has("[script] level=alpha.scn save state ignored: its script block is truncated"));
    jpt_level_scripts_ready(1);
    CHECK(Has(Print("game init true levels=0 steps=0")));

    // The session ends with the package; the campaign afterwards has no VM and its saves no block.
    jpt_custom_level_deactivate();
    CHECK(jpt_level_scripts_session() == nullptr && Contains("[script] session end vms="));
    g_lines.clear();
    jpt_level_scripts_start(JPT_SESSION_CAMPAIGN, CaptureLine);
    jpt_level_scripts_begin(JPT_REASON_NEW_GAME, "be.scn", nullptr);
    jpt_level_scripts_ready(1);
    jpt_level_scripts_step(0.1f);
    CHECK(Saved(dir, "SAVEGAME.004", ""));
    CHECK(Slurp(dir / "SAVEGAME.004").find("JPTSCRPT") == std::string::npos);
    CHECK(g_lines.empty() && jpt_level_scripts_session() == nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    test_level_without_scripts();
    test_lifecycle_order();
    test_restart();
    test_scripts_apart();
    test_load_errors();
    test_per_level_files();
    test_campaign_and_package();
    printf("script_session_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
