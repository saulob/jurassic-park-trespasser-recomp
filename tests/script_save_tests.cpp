// What a save keeps of a custom session's Trespasser CE scripts (jpt_script_session.cpp, jpt_script_state.cpp, the script
// block of jpt_save.c), headless: the nvram of game and level scripts by class, the level's start for Restart Level, a load
// that makes the scripts again and gives them their state, damaged, truncated and unknown blocks, scripts changed since,
// and saves of the campaign, which never have a block.
#include "script_fixtures.h"

static void test_save_and_load() {
    jpt_script_session* s = Session({{"scripts/game.nut", kGame}, {"scripts/shared.nut", kShared}});
    CHECK(SaveOf(s).empty());
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Steps(s, 3);
    g_lines.clear();
    const std::vector<uint8_t> saved = SaveOf(s);
    CHECK(!saved.empty() && InOrder({"[script] print: game prepare save", "[script] print: alpha prepare save"}));
    CHECK(Contains("[script] save level=alpha.scn scripts=3 bytes="));
    std::vector<jpt::script::Entry> entries;
    std::string problem;
    CHECK(jpt::script::DecodeState(saved.data(), saved.size(), entries, problem) && entries.size() == 3);
    CHECK(entries[0].class_name == "Campaign" && entries[0].role == jpt::script::Role::kCurrent && entries[0].kind == 1);
    CHECK(entries[1].class_name == "AlphaRules" && entries[1].role == jpt::script::Role::kCurrent && entries[1].kind == 2);
    CHECK(entries[2].class_name == "Campaign" && entries[2].role == jpt::script::Role::kLevelStart);
    CHECK(jpt::script::EncodeState(entries) == saved);
    Steps(s, 2);

    // Loading it: a new VM, the scripts made by their constructors, then their saved nvram, then OnScriptInit(true).
    g_lines.clear();
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    CHECK(!Contains("game end") && jpt_script_session_generation(s) == 2);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("game init true levels=1 steps=3")) && Has(Print("alpha init true steps=3")));
    CHECK(!Contains("level start") && Has("[script] callback script_init level=alpha.scn save=1 scripts=2"));
    // A restart after the load goes back to the level's start, kept in the save.
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_RESTART);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("game init false levels=0 steps=0")) && Has(Print("alpha start steps=0")));
    // The save of another level of the package, where alpha's level script does not apply.
    g_lines.clear();
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(Has("[script] level=beta.scn save state of script=AlphaRules skipped: the level has no such level script now"));
    CHECK(Has(Print("game init true levels=1 steps=3")));
    Close(s);

    // A package without game scripts: beta has no VM, and alpha's saved level script state is left out.
    s = Session({{"scripts/shared.nut", kShared}});
    Begin(s, "beta.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    CHECK(!jpt_script_session_has_vm(s) && Has("[script] level=beta.scn save state of 1 level scripts skipped: the level needs no script now"));
    Close(s);
}

static void test_save_state_damaged() {
    jpt_script_session* s = Session({{"scripts/game.nut", kGame}, {"scripts/shared.nut", kShared}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Steps(s, 2);
    const std::vector<uint8_t> saved = SaveOf(s);
    std::vector<uint8_t> version = saved, truncated = saved, trailing = saved, garbage = {1, 2, 3};
    version[0] = 99;
    truncated.resize(saved.size() - 3);
    trailing.push_back(0);
    const std::pair<const std::vector<uint8_t>*, const char*> cases[] = {
        {&version, "its version 99 is not known"},
        {&truncated, "truncated"},
        {&trailing, "1 bytes follow its last entry"},
        {&garbage, "it is truncated"},
    };
    for (const auto& [bytes, why] : cases) {
        g_lines.clear();
        Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_SAVE, bytes);
        jpt_script_level_ready(s, 1);
        CHECK(Contains("[script] level=alpha.scn save state ignored: ") && Contains(why));
        CHECK(Has(Print("game init true levels=0 steps=0")) && Has(Print("alpha init true steps=0")));
    }
    std::vector<jpt::script::Entry> entries;
    std::string problem;
    std::vector<uint8_t> huge = saved;
    huge[4] = 0xFF, huge[5] = 0xFF, huge[6] = 0xFF, huge[7] = 0x7F;
    CHECK(!jpt::script::DecodeState(huge.data(), huge.size(), entries, problem) && entries.empty());
    for (size_t cut = 0; cut < saved.size(); ++cut) CHECK(!jpt::script::DecodeState(saved.data(), cut, entries, problem));
    Close(s);

    // The scripts changed since the save: a class is gone, another has a new nvram slot, a third has no table any more.
    const char* changed =
        "class AlphaRules extends LevelScript {\n"
        "  static SCN = \"alpha.scn\";\n"
        "  nvram = null;\n"
        "  constructor() { nvram = { steps = 0, extra = 5 }; }\n"
        "  function OnScriptInit(bSaveGame) { print(\"alpha init \" + bSaveGame + \" steps=\" + nvram.steps + \" extra=\" + nvram.extra); }\n"
        "  function OnLevelStart() {}\n"
        "}\n"
        "class Renamed extends GameScript { nvram = 3; function OnScriptInit(b) { print(\"renamed \" + nvram); } function OnLevelStart() {} }\n";
    s = Session({{"scripts/shared.nut", changed}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("alpha init true steps=2 extra=5")) && Has(Print("renamed 3")));
    CHECK(Has("[script] level=alpha.scn save state of script=Campaign skipped: the level has no such game script now"));
    Close(s);
    const char* no_table = "class Campaign extends GameScript { nvram = 3; function OnLevelStart() {} }\n";
    s = Session({{"scripts/game.nut", no_table}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(Has("[script] level=alpha.scn script=Campaign nvram not restored: the script has no nvram table"));
    Close(s);
}

// The block as jpt_save writes and reads it, behind the stamp and the actions' state of a custom session's save only.
static void test_save_file_block() {
    const fs::path dir = Folder();
    char problem[128];
    size_t size = 9;
    jpt_save_set_scripts(nullptr);
    CHECK(Saved(dir, "SAVEGAME.001", "") && Saved(dir, "SAVEGAME.002", "Pkg"));
    CHECK(!jpt_save_read_scripts((dir / "SAVEGAME.001").string().c_str(), &size, problem, sizeof problem) && size == 0 && !problem[0]);
    CHECK(!jpt_save_read_scripts((dir / "SAVEGAME.002").string().c_str(), &size, problem, sizeof problem) && !problem[0]);

    jpt_script_session* s = Session({{"scripts/game.nut", kGame}});
    Begin(s, "alpha.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    static jpt_script_session* current;
    current = s;
    jpt_save_set_scripts([](size_t* n) { return jpt_script_session_save(current, n); });
    g_state_bytes = 7;
    jpt_save_set_state(SomeState);
    CHECK(Saved(dir, "SAVEGAME.003", "") && Slurp(dir / "SAVEGAME.003").find("JPTSCRPT") == std::string::npos);
    CHECK(Saved(dir, "SAVEGAME.004", "Pkg"));
    jpt_save_set_state(nullptr);
    jpt_save_set_scripts(nullptr);
    uint8_t* block = jpt_save_read_scripts((dir / "SAVEGAME.004").string().c_str(), &size, problem, sizeof problem);
    std::vector<jpt::script::Entry> entries;
    std::string why;
    CHECK(block && jpt::script::DecodeState(block, size, entries, why) && entries.size() == 2 && entries[0].class_name == "Campaign");
    free(block);
    uint8_t state[16];
    CHECK(jpt_save_read_state((dir / "SAVEGAME.004").string().c_str(), state, sizeof state) == 7);
    jpt_save_info_t info;
    CHECK(jpt_save_inspect_file((dir / "SAVEGAME.004").string().c_str(), &info) && info.stamped && !strcmp(info.package, "Pkg") && !info.problem[0]);

    const std::string whole = Slurp(dir / "SAVEGAME.004");
    Write(dir / "SAVEGAME.005", whole.substr(0, whole.size() - 1));
    CHECK(!jpt_save_read_scripts((dir / "SAVEGAME.005").string().c_str(), &size, problem, sizeof problem) && !strcmp(problem, "its script block is truncated"));
    std::string zero = whole;
    const size_t at = zero.find("JPTSCRPT");
    for (int k = 0; k < 4; ++k) zero[at + 8 + static_cast<size_t>(k)] = 0;
    Write(dir / "SAVEGAME.006", zero);
    CHECK(!jpt_save_read_scripts((dir / "SAVEGAME.006").string().c_str(), &size, problem, sizeof problem) && !strcmp(problem, "its script block has a size of 0 bytes"));
    Close(s);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    test_save_and_load();
    test_save_state_damaged();
    test_save_file_block();
    printf("script_save_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
