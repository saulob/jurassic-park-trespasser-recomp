// Which Trespasser CE scripts a level needs (src/common/jpt_script_plan.cpp), headless, over scripts written here: the scan of
// a script's text, the kinds of its classes, the rule that a level needs scripts only when they act in it, the files it loads
// and in which order, and the CE functions, classes and handlers its scripts ask for that the port does not have.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "jpt_script_plan.h"

using namespace jpt::script;

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static bool HasText(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

static const ClassInfo* ClassOf(const FileInfo& f, const char* name) {
    for (const ClassInfo& c : f.classes)
        if (c.name == name) return &c;
    return nullptr;
}

static std::vector<std::string> Names(const LevelPlan& plan) {
    std::vector<std::string> names;
    for (const PlannedClass& c : plan.classes) names.push_back(c.name);
    return names;
}

static void test_scan() {
    const char* text =
        "// class Commented extends GameScript {}\n"
        "/* class Hidden extends LevelScript { function OnSimStep(d) { TP_Engine.Teleport(); } } */\n"
        "# hash comment GetPlayer()\n"
        "gCount <- 0;\n"
        "::gRooted <- [];\n"
        "const kSpeed = 3;\n"
        "enum EThing { A, B }\n"
        "function Helper(fDist, value = 2) { local r3Rot = 1; foreach (k, v3Pos in [1]) {} try {} catch (pinsKiller) {} return fDist; }\n"
        "local text = \"class InString extends GameScript { GetPlayer() }\";\n"
        "local raw = @\"a \"\"quoted\"\" Teleport(\";\n"
        "local ch = 'x';\n"
        "class Watcher extends LevelScript\n"
        "{\n"
        "  static SCN = [ \"Alpha.SCN\", \"levels\\\\beta.scn\" ];\n"
        "  nvram = null;\n"
        "  constructor() { nvram = {}; }\n"
        "  function OnLevelStart() { print(gCount); }\n"
        "  function OnSimStep(fStepDeltaSeconds) { TP_Engine.GetElapsedSimTime(); }\n"
        "}\n"
        "class One extends LevelScript { static SCN = \"one.scn\"; }\n"
        "class Faulty extends LevelScript { static SCN = 3; }\n"
        "class Mixed extends LevelScript { static SCN = [ \"a.scn\", 4 ]; }\n"
        "class Plain { function Draw() {} }\n"
        "function Plain::Later() { GetScreenWidth(); }\n"
        "class Deep.Nested extends GameScript {}\n";
    const FileInfo f = ScanFile("scripts/watch.nut", text);
    CHECK(f.problem.empty());
    CHECK(f.classes.size() == 6);
    CHECK(!ClassOf(f, "Commented") && !ClassOf(f, "Hidden") && !ClassOf(f, "InString"));
    const ClassInfo* w = ClassOf(f, "Watcher");
    CHECK(w && w->base == "LevelScript" && w->line == 12 && w->has_scenes && w->scenes.size() == 2);
    CHECK(w && HasText(w->methods, "constructor") && HasText(w->methods, "OnLevelStart") && HasText(w->methods, "OnSimStep"));
    CHECK(w && w->uses.count("TP_Engine") && w->uses.count("GetElapsedSimTime") && w->uses.count("gCount") && !w->uses.count("Teleport"));
    CHECK(ClassOf(f, "One") && ClassOf(f, "One")->has_scenes && ClassOf(f, "One")->scenes[0] == "one.scn");
    CHECK(ClassOf(f, "Faulty") && !ClassOf(f, "Faulty")->has_scenes);
    CHECK(ClassOf(f, "Mixed") && !ClassOf(f, "Mixed")->has_scenes);
    CHECK(ClassOf(f, "Plain") && HasText(ClassOf(f, "Plain")->methods, "Later") && ClassOf(f, "Plain")->uses.count("GetScreenWidth"));
    CHECK(ClassOf(f, "Deep.Nested") && ClassOf(f, "Deep.Nested")->base == "GameScript");
    for (const char* g : {"gCount", "gRooted", "kSpeed", "EThing", "Helper", "Watcher", "Plain"}) CHECK(f.globals.count(g));
    CHECK(!f.globals.count("Deep.Nested") && !f.globals.count("text"));
    for (const char* l : {"fDist", "value", "r3Rot", "k", "v3Pos", "pinsKiller", "text", "raw", "ch", "fStepDeltaSeconds"}) CHECK(f.locals.count(l));
    CHECK(!f.top_uses.count("GetPlayer") && !f.top_uses.count("Teleport"));

    CHECK(!ScanFile("a.nut", "class A {\n /* never closed").problem.empty());
    CHECK(!ScanFile("b.nut", "local s = \"never closed\n").problem.empty());
    CHECK(ScanFile("c.nut", "\xEF\xBB\xBF" "class Bom extends GameScript {}").classes.size() == 1);
    CHECK(ScanFile("d.nut", "").classes.empty());
}

static void test_names() {
    for (const char* n : {"TP_Engine", "GetPlayer", "Teleport", "MoveWithFixedVelocity", "CAnimal", "Vector3", "uCMD_USER1", "ActionType", "GameScript"})
        CHECK(IsCeName(n));
    for (const char* n : {"print", "array", "len", "nvram", "OnSimStep", "x", "foreach"}) CHECK(!IsCeName(n));
    CHECK(IsDrivenHandler(Kind::kGame, "OnSimStep") && IsDrivenHandler(Kind::kLevel, "OnLevelEnd") && IsDrivenHandler(Kind::kGame, "OnGameStart"));
    CHECK(!IsDrivenHandler(Kind::kLevel, "OnGameStart") && !IsDrivenHandler(Kind::kGame, "OnPaint"));
    CHECK(IsDrivenHandler(Kind::kGame, "OnPaintHW3D") && !IsDrivenHandler(Kind::kLevel, "OnPaintHW3D"));
    CHECK(!IsDrivenHandler(Kind::kGame, "OnLevelTransitionRequested") && !IsDrivenHandler(Kind::kGame, "OnGameCompleted"));
    CHECK(IsDrivenHandler(Kind::kInstance, "OnScriptInit") && IsDrivenHandler(Kind::kInstance, "OnActBite"));
    CHECK(!IsDrivenHandler(Kind::kInstance, "OnUse") && !IsDrivenHandler(Kind::kAction, "Run") && !IsDrivenHandler(Kind::kGame, "OnActBite"));
}

// The package of the brief: alpha uses a script, beta does not, both beside shared.nut.
static std::vector<FileInfo> SharedPackage() {
    return {ScanFile("scripts/shared.nut",
                     "class AlphaRules extends LevelScript {\n"
                     "  static SCN = \"alpha.scn\";\n"
                     "  function OnLevelStart() { print(\"alpha\"); }\n"
                     "}\n"
                     "class Quiet extends LevelScript {\n"
                     "  nvram = null;\n"
                     "  constructor() { nvram = {}; }\n"
                     "  function OnScriptInit(bSaveGame) {}\n"
                     "  function OnPrepareSave() {}\n"
                     "  function OnWorldPurge() {}\n"
                     "}\n")};
}

static void test_level_decides() {
    const std::vector<FileInfo> files = SharedPackage();
    const LevelPlan alpha = PlanLevel(files, "alpha.scn", {});
    // alpha needs a VM for AlphaRules; then Quiet, which only keeps its state, is made as well.
    CHECK(alpha.needs_scripts() && alpha.supported() && Names(alpha) == (std::vector<std::string>{"AlphaRules", "Quiet"}));
    CHECK(alpha.files == std::vector<std::string>{"scripts/shared.nut"});
    CHECK(PlanLevel(files, "ALPHA.SCN", {}).needs_scripts());
    CHECK(PlanLevel(files, "data\\alpha.scn", {}).needs_scripts());
    const LevelPlan beta = PlanLevel(files, "beta.scn", {});
    CHECK(!beta.needs_scripts() && beta.files.empty() && beta.required.empty());
    // A retail level in the same custom session: the same rule.
    CHECK(!PlanLevel(files, "be.scn", {}).needs_scripts());
}

static void test_kinds_and_world() {
    const std::vector<FileInfo> files = {
        ScanFile("scripts/a.nut",
                 "class Hud extends GameScript { function OnSimStep(d) { gTicks++; } }\n"
                 "class Bridge extends LevelScript { static SCN = [\"x.scn\"]; function OnLevelEnd(b) {} }\n"
                 "class Door extends CInstance { function OnUse(b) { return false; } }\n"
                 "class Bite extends Dino { }\n"
                 "class MyAT extends ActionType { function Run() {} }\n"
                 "class Orphan extends Nothing { function OnSimStep(d) {} }\n"),
        ScanFile("scripts/b.nut", "class Dino extends CAnimal { function OnActBite(a, b, c, d) { Teleport(a, b); } }\ngTicks <- 0;\n"),
    };
    const LevelPlan x = PlanLevel(files, "x.scn", {{"Door", "Door-00", false}, {"Door", "Door-01", false}, {"Bite", "Raptor-00", false}, {"MyAT", "Trig-00", true}});
    CHECK(Names(x) == (std::vector<std::string>{"Hud", "Bridge", "Door", "Bite", "MyAT"}));
    CHECK(x.classes[0].kind == Kind::kGame && x.classes[0].sim_step && x.classes[1].kind == Kind::kLevel && !x.classes[1].sim_step);
    CHECK(x.classes[2].kind == Kind::kInstance && x.classes[2].objects == (std::vector<std::string>{"Door-00", "Door-01"}));
    CHECK(x.classes[3].kind == Kind::kInstance && x.classes[4].kind == Kind::kAction && x.classes[4].objects[0] == "Trig-00");
    CHECK(x.files == (std::vector<std::string>{"scripts/a.nut", "scripts/b.nut"}));
    for (const char* r : {"ActionType", "OnUse", "Run"}) CHECK(HasText(x.required, r));
    for (const char* r : {"CInstance", "CAnimal", "Teleport", "OnActBite"}) CHECK(!HasText(x.required, r));
    CHECK(!HasText(x.required, "OnSimStep") && !HasText(x.required, "OnLevelEnd") && !x.supported());
    CHECK(std::is_sorted(x.required.begin(), x.required.end()));

    const LevelPlan y = PlanLevel(files, "y.scn", {{"Ghost", "Box-00", false}, {"Hud", "Box-01", false}, {"Door", "Trig-01", true}});
    CHECK(Names(y) == std::vector<std::string>{"Hud"});
    CHECK(y.problems.size() == 3 && y.needs_scripts() && !y.supported());
    CHECK(y.problems[0].find("Ghost") != std::string::npos && y.problems[0].find("no script of the package declares") != std::string::npos);
    CHECK(y.problems[1].find("a game script, as its Script") != std::string::npos);
    CHECK(y.problems[2].find("an instance script, as its ScriptAT") != std::string::npos);
    // Only what the level uses: game scripts and its own level scripts; b.nut holds the global Hud reads.
    const LevelPlan z = PlanLevel(files, "z.scn", {});
    CHECK(Names(z) == std::vector<std::string>{"Hud"} && z.supported());
    CHECK(z.files == (std::vector<std::string>{"scripts/a.nut", "scripts/b.nut"}));
}

static void test_files_and_order() {
    const std::vector<FileInfo> files = {
        ScanFile("scripts/1_base.nut", "class HudBase extends GameScript { function Draw() { return gColour; } }\n"),
        ScanFile("scripts/2_colour.nut", "gColour <- 3;\nfunction Shade() { return gColour; }\n"),
        ScanFile("scripts/3_unused.nut", "gNobody <- 1;\nclass Lonely extends LevelScript { static SCN = \"other.scn\"; function OnLevelStart() {} }\n"),
        ScanFile("scripts/4_hud.nut", "class Hud extends HudBase { function OnLevelStart() { Shade(); } }\n"),
    };
    // CE makes an instance of each class derived from GameScript, HudBase as well as Hud.
    const LevelPlan p = PlanLevel(files, "main.scn", {});
    CHECK(Names(p) == (std::vector<std::string>{"HudBase", "Hud"}));
    CHECK(p.files == (std::vector<std::string>{"scripts/1_base.nut", "scripts/2_colour.nut", "scripts/4_hud.nut"}));
    CHECK(p.supported());
    // Deterministic: the same package plans the same way.
    const LevelPlan again = PlanLevel(files, "main.scn", {});
    CHECK(Names(again) == Names(p) && again.files == p.files && again.required == p.required);
    // A later file's class of the same name is the one CE keeps (the highest priority path has the final say).
    const std::vector<FileInfo> twice = {ScanFile("mod/scripts/a.nut", "class Same extends GameScript { function OnSimStep(d) {} }\n"),
                                         ScanFile("fm/scripts/a.nut", "class Same extends GameScript { function OnGameStart() {} }\n")};
    const LevelPlan t = PlanLevel(twice, "l.scn", {});
    CHECK(Names(t) == std::vector<std::string>{"Same"} && t.classes[0].file == "fm/scripts/a.nut" && !t.classes[0].sim_step);
    // A script that cannot be read is a problem of the level that needs it.
    std::vector<FileInfo> broken = {ScanFile("scripts/x.nut", "class G extends GameScript { function OnSimStep(d) {} }\n/* open")};
    CHECK(!PlanLevel(broken, "l.scn", {}).problems.empty() || PlanLevel(broken, "l.scn", {}).classes.empty());
}

static void test_required() {
    const std::vector<FileInfo> files = {
        ScanFile("scripts/hud.nut",
                 "class Gauge {\n"
                 "  function Draw() { TP_Engine.DrawBitmap(bmp, 0, 0); }\n"
                 "}\n"
                 "class Hud extends GameScript {\n"
                 "  gauge = null;\n"
                 "  constructor() { gauge = Gauge(); }\n"
                 "  function OnPaint() { gauge.Draw(); }\n"
                 "  function OnModalChar(c) {}\n"
                 "  function OnHandleInput() { if (bKeyHit(uCMD_USER1)) {} }\n"
                 "  function OnLevelStart() { local GetPlayer = 1; Teleport <- 2; }\n"
                 "}\n"),
    };
    const LevelPlan p = PlanLevel(files, "l.scn", {});
    CHECK(Names(p) == std::vector<std::string>{"Hud"});
    for (const char* r : {"DrawBitmap", "OnPaint", "OnModalChar"}) CHECK(HasText(p.required, r));
    for (const char* r : {"TP_Engine", "OnHandleInput", "bKeyHit", "uCMD_USER1"}) CHECK(!HasText(p.required, r));
    // The uCMD_USER commands a level's code names: only those are bound for it.
    CHECK(p.user_commands == 1u);
    CHECK(!HasText(p.required, "GetPlayer") && !HasText(p.required, "GameScript") && !HasText(p.required, "OnLevelStart"));
    CHECK(FileRequires(files, files[0]) == p.required);

    // Lifecycle and nvram only: what the port has now.
    const std::vector<FileInfo> plain = {ScanFile("scripts/plain.nut",
                                                  "class Counter extends GameScript {\n"
                                                  "  nvram = null;\n"
                                                  "  constructor() { nvram = { n = 0, seen = [] }; }\n"
                                                  "  function OnGameStart() { nvram.n = 0; }\n"
                                                  "  function OnLevelStart() { nvram.n++; print(\"levels \" + nvram.n); }\n"
                                                  "  function OnLevelEnd(bNext) {}\n"
                                                  "  function OnSimStep(fStep) { nvram.seen.append(fStep); }\n"
                                                  "  function OnMessage(s, p) {}\n"
                                                  "}\n")};
    const LevelPlan q = PlanLevel(plain, "any.scn", {});
    CHECK(q.supported() && q.needs_scripts() && q.required.empty());
    // Handlers CE has that the port does not call yet.
    for (const char* h : {"OnLevelTransitionRequested", "OnGameCompleted", "OnPaint", "OnModalChar"}) {
        const std::vector<FileInfo> one = {ScanFile("s.nut", std::string("class G extends GameScript { function ") + h + "() {} }\n")};
        CHECK(HasText(PlanLevel(one, "l.scn", {}).required, h));
    }
}

// Shaped as the corpus's scripts are (our own text): a spitting dinosaur and its level helper.
static void test_corpus_shapes() {
    const std::vector<FileInfo> files = {ScanFile("scripts/spit.nut",
                                                  "gProjectiles <- [];\n"
                                                  "class Helper extends LevelScript {\n"
                                                  "  nvram = null;\n"
                                                  "  constructor() { nvram = {}; nvram.i <- 0; }\n"
                                                  "  function OnScriptInit(bSaveGame) {}\n"
                                                  "  function OnPrepareSave() {}\n"
                                                  "  function OnWorldPurge() { gProjectiles = []; }\n"
                                                  "}\n"
                                                  "class Projectile extends CInstance {\n"
                                                  "  constructor() { gProjectiles.append(this); }\n"
                                                  "  function Launch(v3, r3, vel, ms) { Teleport(v3, r3); MoveWithFixedVelocity(vel, ms, false); }\n"
                                                  "}\n"
                                                  "class Spitter extends CAnimal {\n"
                                                  "  function OnActBite(pinsTarget, v3Loc, fDist, bFeint) {\n"
                                                  "    local t = TP_Engine.GetElapsedSimTime();\n"
                                                  "    local head = pbbBoundaryBox(EBoundaryBoxes.ebbHEAD);\n"
                                                  "  }\n"
                                                  "}\n")};
    const LevelPlan none = PlanLevel(files, "prequel.scn", {});
    CHECK(!none.needs_scripts());
    const LevelPlan with = PlanLevel(files, "ops.scn", {{"Spitter", "Dilo-00", false}, {"Projectile", "Spit-00", false}});
    // Once the level has a VM, its state-only helper is made too, as CE makes every level script.
    CHECK(Names(with) == (std::vector<std::string>{"Helper", "Spitter", "Projectile"}) && with.supported() && with.required.empty());
}

// One package, four levels: each plan is its own, whatever its siblings need.
static void test_four_levels() {
    const std::vector<FileInfo> files = {
        ScanFile("scripts/beta.nut", "class BetaRules extends LevelScript { static SCN = \"beta.scn\"; nvram = null; function OnLevelStart() { print(\"b\"); } }\n"),
        ScanFile("scripts/gamma.nut", "class GammaHud extends LevelScript { static SCN = \"gamma.scn\"; function OnLevelStart() { TP_Engine.ReplayVoiceover(); } }\n"),
        ScanFile("scripts/delta_unused.nut", "class Spare extends LevelScript { static SCN = \"elsewhere.scn\"; function OnLevelStart() {} }\nclass Helper {}\n"),
    };
    const LevelPlan alpha = PlanLevel(files, "alpha.scn", {}), beta = PlanLevel(files, "beta.scn", {});
    const LevelPlan gamma = PlanLevel(files, "gamma.scn", {}), delta = PlanLevel(files, "delta.scn", {});
    CHECK(!alpha.needs_scripts() && alpha.files.empty() && alpha.required.empty());
    CHECK(beta.needs_scripts() && beta.supported() && beta.files == std::vector<std::string>{"scripts/beta.nut"});
    CHECK(gamma.needs_scripts() && !gamma.supported() && gamma.files == std::vector<std::string>{"scripts/gamma.nut"} && HasText(gamma.required, "ReplayVoiceover"));
    CHECK(!delta.needs_scripts() && delta.files.empty() && delta.required.empty());
}

int main() {
    test_four_levels();
    test_scan();
    test_names();
    test_level_decides();
    test_kinds_and_world();
    test_files_and_order();
    test_required();
    test_corpus_shapes();
    printf("script_plan_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
