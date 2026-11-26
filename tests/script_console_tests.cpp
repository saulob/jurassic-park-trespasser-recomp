// OnConsoleCommand as CE dispatches a console command the game does not know (headless; the port has no console): the game
// scripts in their order until one returns true, the command and its arguments as strings, level and instance scripts never
// asked, an error or a budget stopping only that script, nothing while a level loads or without a VM, the campaign without it.
#include "jpt_level_scripts.h"
#include "script_fixtures.h"

static const char* kConsoles =
    "class First extends GameScript {\n"
    "  function OnConsoleCommand(cmd, args) { print(\"first \" + cmd + \" [\" + args + \"]\"); return cmd == \"one\"; }\n"
    "}\n"
    "class Second extends GameScript {\n"
    "  function OnConsoleCommand(cmd, args) {\n"
    "    print(\"second \" + cmd);\n"
    "    if (cmd == \"two\") return true;\n"
    "    if (cmd == \"number\") return 1;\n"
    "    if (cmd == \"text\") return \"true\";\n"
    "    if (cmd == \"boom\") throw \"console broke\";\n"
    "    if (cmd == \"spin\") while (true) {}\n"
    "    return false;\n"
    "  }\n"
    "}\n"
    "class Third extends GameScript {\n"
    "  function OnConsoleCommand(cmd, args) { print(\"third \" + cmd); return cmd != \"none\"; }\n"
    "}\n"
    "class Quiet extends GameScript { function OnSimStep(d) {} }\n"
    "class Rules extends LevelScript {\n"
    "  static SCN = \"c.scn\";\n"
    "  function OnConsoleCommand(cmd, args) { print(\"level \" + cmd); return true; }\n"
    "}\n";

static int Run(jpt_script_session* s, const char* cmd, const char* args = nullptr) {
    g_lines.clear();
    return jpt_script_level_console(s, cmd, args);
}

static void test_order() {
    jpt_script_session* s = Session({{"scripts/console.nut", kConsoles}}, 0, 20000);
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    // While the level loads, no script is asked.
    CHECK(Run(s, "one") == 0 && !Contains("first"));
    jpt_script_level_ready(s, 1);
    // The first script that handles it ends the dispatch.
    CHECK(Run(s, "one", "a b") == 1 && Has("[script] print: first one [a b]") && !Contains("second"));
    CHECK(Has("[script] level=c.scn console command=one handled by script=First"));
    CHECK(Run(s, "two") == 1 && InOrder({"[script] print: first two []", "[script] print: second two"}) && !Contains("third"));
    // Only a true bool handles it; a number or a string does not.
    CHECK(Run(s, "number") == 1 && Has("[script] print: third number"));
    CHECK(Run(s, "text") == 1 && Has("[script] print: third text"));
    // Nobody: not handled. Level scripts are never asked.
    CHECK(Run(s, "none") == 0 && Has("[script] print: third none") && !Contains("level "));
    // An empty command reaches no script.
    CHECK(Run(s, "") == 0 && Run(s, nullptr) == 0 && g_lines.empty());
    Close(s);
}

static void test_failures() {
    jpt_script_session* s = Session({{"scripts/console.nut", kConsoles}}, 0, 20000);
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    // An error in one script: that script stops, the next is asked.
    CHECK(Run(s, "boom") == 1 && Contains("console broke") && Has("[script] print: third boom"));
    CHECK(Run(s, "two") == 1 && !Contains("second") && Has("[script] print: third two"));
    Close(s);
    s = Session({{"scripts/console.nut", kConsoles}}, 0, 20000);
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    // Out of its budget: the same.
    CHECK(Run(s, "spin") == 1 && Contains("instruction budget exceeded (20000)") && Has("[script] print: third spin"));
    Close(s);
}

static void test_lifecycle() {
    jpt_script_session* s = Session({{"scripts/console.nut", kConsoles}}, 0, 20000);
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    CHECK(Run(s, "one") == 1);
    // Restart: the scripts of the new VM answer.
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(Run(s, "one") == 0);
    jpt_script_level_ready(s, 1);
    CHECK(Run(s, "one") == 1 && jpt_script_session_generation(s) == 2);
    Close(s);
    // A level without scripts has no VM to ask.
    s = Session({{"scripts/rules.nut", "class Only extends LevelScript { static SCN = \"o.scn\"; function OnLevelStart() {} }\n"}});
    Begin(s, "c.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_session_has_vm(s) && Run(s, "one") == 0);
    Close(s);
    // The campaign: no session, no dispatch.
    jpt_level_scripts_end();
    jpt_level_scripts_start(JPT_SESSION_CAMPAIGN, CaptureLine);
    g_lines.clear();
    CHECK(!jpt_level_scripts_session() && jpt_level_scripts_console("one", "x") == 0 && g_lines.empty());
}

int main() {
    test_order();
    test_failures();
    test_lifecycle();
    printf("script_console_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
