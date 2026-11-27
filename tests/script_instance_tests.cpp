// Instance scripts of a custom level (headless), over a world of the test's own: the world's Script properties make them,
// OnScriptInit, OnActBite from the AI, a spitting animal launching its projectiles through Teleport and
// MoveWithFixedVelocity, objects gone from the world, Restart, saves and loads of their nvram, and errors kept to one script.
#include <math.h>

#include "jpt_script_plan.h"
#include "script_fixtures.h"
#include "script_world.h"

// Shaped as CE's Spitter sample (our own text): a level helper, projectiles and spitting animals.
static const char* kSpitter =
    "gSpits <- [];\n"
    "gSpitIndex <- 0;\n"
    "class SpitHelper extends LevelScript {\n"
    "  nvram = null;\n"
    "  constructor() { nvram = { index = 0 }; }\n"
    "  function OnScriptInit(bSaveGame) { gSpitIndex = bSaveGame ? nvram.index : 0; }\n"
    "  function OnPrepareSave() { nvram.index = gSpitIndex; }\n"
    "  function OnWorldPurge() { gSpits = []; }\n"
    "}\n"
    "class Spit extends CInstance {\n"
    "  constructor() { gSpits.append(this); print(\"spit made \" + GetInstanceName()); }\n"
    "  function Launch(v3At, r3At, v3Vel, iMs) { Teleport(v3At, r3At); MoveWithFixedVelocity(v3Vel, iMs, false); }\n"
    "  function destructor() { print(\"spit gone\"); }\n"
    "}\n"
    "class Spitter extends CAnimal {\n"
    "  nvram = null;\n"
    "  fInterval = 4.0; fSpeed = 7.0; fAhead = 0.5; fChance = 1.0;\n"
    "  constructor() { nvram = { fNext = -100.0, iShots = 0 }; }\n"
    "  function OnScriptInit(bSaveGame) { if (!bSaveGame) nvram.fNext = -100.0; print(\"spitter init \" + bSaveGame + \" next=\" + nvram.fNext + \" shots=\" + nvram.iShots); }\n"
    "  function OnPostLoad() { print(\"spitter loaded shots=\" + nvram.iShots); }\n"
    "  function OnActBite(pinsTarget, v3Loc, fDist, bFeint) {\n"
    "    local now = TP_Engine.GetElapsedSimTime();\n"
    "    if (now < nvram.fNext) return;\n"
    "    if (TP_Engine.GetRandFloatRanged(0.0, 0.9999999) >= fChance) return;\n"
    "    local head = pbbBoundaryBox(EBoundaryBoxes.ebbHEAD);\n"
    "    local at = v3GetHeadPos();\n"
    "    local delta = v3Loc - at;\n"
    "    local flat = Vector3(delta.x, delta.y, 0); flat.Normalise();\n"
    "    local dir = Vector3.YAxis() * head.r3Rot();\n"
    "    if (dir.Dot(flat) < 0.5) return;\n"
    "    nvram.fNext = now + fInterval;\n"
    "    nvram.iShots++;\n"
    "    gSpitIndex = (gSpitIndex + 1) % gSpits.len();\n"
    "    print(\"bite \" + (pinsTarget == null ? \"nothing\" : pinsTarget.GetInstanceName()) + \" \" + fDist + \" \" + bFeint);\n"
    "    gSpits[gSpitIndex].Launch(at + dir * fAhead, head.r3Rot(), delta * (fSpeed / delta.Length()), 60);\n"
    "  }\n"
    "}\n";

static World g_world;
static jpt_script_engine g_engine;
static uint32_t g_dino, g_dino2, g_player, g_spit0, g_spit1;

static void MakeWorld() {
    g_world = World{};
    g_dino = g_world.Add("Dilo-00", "CAnimal");
    g_dino2 = g_world.Add("Dilo-01", "CAnimal");
    g_player = g_world.Add("Anne", "CPlayer");
    g_spit0 = g_world.Add("Spit-00", "CInstance");
    g_spit1 = g_world.Add("Spit-01", "CInstance");
    g_world.Add("Rock-00", "CInstance");
    for (uint32_t d : {g_dino, g_dino2}) {
        g_world.objects[d].head = {0.0f, 0.0f, 1.5f};
        g_world.objects[d].boxes[3] = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    g_engine = EngineOf(g_world);
}

static jpt_script_session* Level() {
    jpt_script_session* s = Session({{"scripts/spitter.nut", kSpitter}});
    jpt_script_session_destroy(s);
    MakeWorld();
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = 200000;
    config.allocator = &kFake;
    config.engine = &g_engine;
    s = jpt_script_session_create(&config);
    jpt_script_session_add_file(s, "scripts/spitter.nut");
    for (const char* object : {"Dilo-00", "Dilo-01"}) jpt_script_session_add_reference(s, "ops.scn", "Spitter", object, 0);
    for (const char* object : {"Spit-00", "Spit-01"}) jpt_script_session_add_reference(s, "ops.scn", "Spit", object, 0);
    jpt_script_session_add_reference(s, "ops.scn", "Spitter", "Rock-00", 0);
    jpt_script_session_add_reference(s, "ops.scn", "Spit", "Ghost-00", 0);
    g_lines.clear();
    return s;
}

static void Bite(jpt_script_session* s, uint32_t animal, float x, float y, float z, int feint = 0) {
    const float at[3] = {x, y, z};
    jpt_script_level_act_bite(s, animal, g_player, at, sqrtf(x * x + y * y + (z - 1.5f) * (z - 1.5f)), feint);
}

static std::string Hex(uint32_t h) {
    char text[16];
    snprintf(text, sizeof text, "%08X", h);
    return text;
}

static void test_spitter() {
    jpt_script_session* s = Level();
    Begin(s, "ops.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(jpt_script_session_has_vm(s));
    jpt_script_level_ready(s, 1);
    // The world's objects get their scripts; an object of another engine class and a missing one do not.
    CHECK(Has(Print("spit made " + Hex(g_spit0))) && Has(Print("spit made " + Hex(g_spit1))));
    CHECK(Count(Print("spitter init false next=-100 shots=0")) == 2);
    CHECK(Has("[script] level=ops.scn script=Spitter:Rock-00 result=disabled error=the class extends CAnimal, the object is a CInstance"));
    CHECK(Has("[script] level=ops.scn script=Spit:Ghost-00 result=missing error=the world has no object of that name"));
    int stepping = 0;
    CHECK(jpt_script_session_scripts(s, &stepping) == 5);

    // The AI bites: the head, its direction, a projectile placed ahead of it and sent at the target.
    g_world.time = 10.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    CHECK(Has(Print("bite " + Hex(g_player) + " 6 false")));
    CHECK(g_world.teleported.size() == 1 && g_world.teleported[0] == g_spit1);
    const WorldObject& spit = g_world.objects[g_spit1];
    CHECK(spit.position.x == 0.0f && fabsf(spit.position.y - 0.5f) < 1e-5f && fabsf(spit.position.z - 1.5f) < 1e-5f);
    CHECK(g_world.moves.size() == 1 && g_world.moves[0].handle == g_spit1 && fabsf(g_world.moves[0].velocity.y - 7.0f) < 1e-4f &&
          g_world.moves[0].milliseconds == 60);
    // Within the interval, and behind the head: nothing.
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f, 1);
    g_world.time = 20.0f;
    Bite(s, g_dino, 0.0f, -6.0f, 1.5f);
    CHECK(g_world.moves.size() == 1);
    // A feint spits too; an animal without a script, nothing.
    Bite(s, g_dino, 3.0f, 4.0f, 1.5f, 1);
    CHECK(g_world.moves.size() == 2 && Has(Print("bite " + Hex(g_player) + " 5 true")));
    Bite(s, g_player, 0.0f, 6.0f, 1.5f);
    CHECK(g_world.moves.size() == 2);

    // A projectile gone from the world: that spitter's script stops with the reason, the other goes on, nothing crashes.
    g_world.objects.erase(g_spit1);
    g_world.time = 30.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    CHECK(Contains("script=Spitter:Dilo-00") && Contains("callback=OnActBite") && Contains("Teleport: the object is no longer in the world"));
    g_world.time = 40.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    Bite(s, g_dino2, 0.0f, 6.0f, 1.5f);
    CHECK(g_world.moves.size() == 3 && g_world.moves[2].handle == g_spit0);
    jpt_script_session_destroy(s);
    CHECK(g_bytes == 0 && g_blocks.empty());
}

static void test_lifecycle() {
    jpt_script_session* s = Level();
    Begin(s, "ops.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    g_world.time = 10.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    const unsigned first = jpt_script_session_generation(s);

    // A save keeps each instance script's nvram under its object.
    const std::vector<uint8_t> saved = SaveOf(s);
    std::vector<jpt::script::Entry> entries;
    std::string problem;
    CHECK(jpt::script::DecodeState(saved.data(), saved.size(), entries, problem));
    int instances = 0;
    for (const jpt::script::Entry& e : entries)
        if (e.kind == static_cast<uint8_t>(jpt::script::Kind::kInstance)) {
            instances++;
            CHECK(e.class_name == "Spitter" && (e.object == "Dilo-00" || e.object == "Dilo-01"));
        }
    CHECK(instances == 2);

    // Restart: the old VM and its objects are gone, the new world gets new ones of a new generation.
    g_lines.clear();
    Begin(s, "ops.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(Count(Print("spit gone")) == 2 && jpt_script_session_generation(s) == first + 1);
    jpt_script_level_ready(s, 1);
    CHECK(Count(Print("spitter init false next=-100 shots=0")) == 2);
    g_world.time = 11.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    CHECK(g_world.moves.size() == 2);

    // The save loaded: nvram back, OnPostLoad, then OnScriptInit(true).
    g_lines.clear();
    Begin(s, "ops.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(InOrder({"[script] print: spitter loaded shots=1", "[script] print: spitter init true next=14 shots=1"}));
    CHECK(Has(Print("spitter init true next=-100 shots=0")));
    g_world.time = 12.0f;
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    CHECK(g_world.moves.size() == 2);

    // A save from an older world that lacks an object: that entry is skipped with a line.
    std::vector<jpt::script::Entry> extra = entries;
    extra.push_back({jpt::script::Role::kCurrent, static_cast<uint8_t>(jpt::script::Kind::kInstance), "Spitter", "Dilo-09", {}, 0});
    const std::vector<uint8_t> more = jpt::script::EncodeState(extra);
    Begin(s, "ops.scn", JPT_SCRIPT_BEGIN_SAVE, &more);
    jpt_script_level_ready(s, 1);
    CHECK(Has("[script] level=ops.scn save state of script=Spitter:Dilo-09 skipped: the level has no such instance script now"));

    // A transition into a level without scripts: the instance scripts are destroyed with their VM.
    g_lines.clear();
    Begin(s, "next.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    CHECK(Count(Print("spit gone")) == 2 && !jpt_script_session_has_vm(s));
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    jpt_script_session_destroy(s);
    CHECK(g_bytes == 0 && g_blocks.empty());
}

static void test_budgets() {
    // An endless OnActBite runs out of its instruction budget: that script stops, the next call is refused quickly.
    const char* endless =
        "class Loop extends CAnimal { function OnActBite(a, b, c, d) { while (true) {} } }\n"
        "class Calm extends CAnimal { function OnActBite(a, b, c, d) { print(\"calm\"); } }\n";
    jpt_script_session* s = Session({{"scripts/loop.nut", endless}});
    jpt_script_session_destroy(s);
    MakeWorld();
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = 50000;
    config.allocator = &kFake;
    config.engine = &g_engine;
    s = jpt_script_session_create(&config);
    jpt_script_session_add_file(s, "scripts/loop.nut");
    jpt_script_session_add_reference(s, "l.scn", "Loop", "Dilo-00", 0);
    jpt_script_session_add_reference(s, "l.scn", "Calm", "Dilo-01", 0);
    g_lines.clear();
    Begin(s, "l.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    CHECK(Contains("script=Loop:Dilo-00") && Contains("instruction budget exceeded (50000)"));
    Bite(s, g_dino, 0.0f, 6.0f, 1.5f);
    Bite(s, g_dino2, 0.0f, 6.0f, 1.5f);
    CHECK(Count("[script] level=l.scn script=Loop:Dilo-00") == 1 && Has(Print("calm")));
    jpt_script_session_destroy(s);
    CHECK(g_bytes == 0 && g_blocks.empty());
}

// SendLGMessage to game and level scripts, SendMessage to an object's script with its reply, ActivateSimStep and its save.
static const char* kMessages =
    "class Hud extends GameScript {\n"
    "  function OnMessage(s, p) { print(\"hud got \" + s + \" \" + p); if (s == \"Echo\") SendLGMessage(\"Echo\", p); }\n"
    "  function OnSimStep(dt) {}\n"
    "}\n"
    "class Rules extends LevelScript {\n"
    "  function OnMessage(s, p) { print(\"rules got \" + s); }\n"
    "  function OnLevelStart() { SendLGMessage(\"Start\", 3); print(\"reply \" + SendMessage(TP_Engine.GetPlayer(), \"Ask\", \"map0\")); }\n"
    "}\n"
    "class Runner extends CPlayer {\n"
    "  nvram = null;\n"
    "  constructor() { nvram = { steps = 0 }; }\n"
    "  function OnScriptInit(bSaveGame) { if (!bSaveGame) ActivateSimStep(true); print(\"runner stepping \" + bIsSimStepActive()); }\n"
    "  function OnSimStep(dt) { nvram.steps++; if (nvram.steps == 2) { SendLGMessage(\"Tired\", nvram.steps); ActivateSimStep(false); } }\n"
    "  function OnMessage(from, s, p) { print(\"runner got \" + s + \" \" + p + \" from \" + from); return \"ok \" + p; }\n"
    "}\n";

static void test_messages_and_steps() {
    jpt_script_session* s = Session({{"scripts/m.nut", kMessages}});
    jpt_script_session_destroy(s);
    MakeWorld();
    g_world.player = g_player;
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = 200000;
    config.allocator = &kFake;
    config.engine = &g_engine;
    s = jpt_script_session_create(&config);
    jpt_script_session_add_file(s, "scripts/m.nut");
    jpt_script_session_add_reference(s, "m.scn", "Runner", "Anne", 0);
    g_lines.clear();
    Begin(s, "m.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("runner stepping true")));
    // The level script's message reaches the game script, not itself; the object's script answers its sender.
    CHECK(Has(Print("hud got Start 3")) && !Has(Print("rules got Start")));
    CHECK(Has(Print("runner got Ask map0 from null")) && Has(Print("reply ok map0")));
    // Steps: the instance's OnSimStep while active, its message to every game and level script, then off.
    Steps(s, 4);
    CHECK(Has(Print("hud got Tired 2")) && Has(Print("rules got Tired")));
    std::vector<uint8_t> saved = SaveOf(s);
    std::vector<jpt::script::Entry> entries;
    std::string problem;
    CHECK(jpt::script::DecodeState(saved.data(), saved.size(), entries, problem));
    for (const jpt::script::Entry& e : entries)
        if (e.object == "Anne") CHECK(e.flags == 0 && e.slots.size() == 1 && e.slots[0].value.i == 2);
    // Restart: the script turns it on again; a save made then keeps it on.
    g_lines.clear();
    Begin(s, "m.scn", JPT_SCRIPT_BEGIN_RESTART);
    jpt_script_level_ready(s, 1);
    Steps(s, 1);
    saved = SaveOf(s);
    CHECK(jpt::script::DecodeState(saved.data(), saved.size(), entries, problem));
    for (const jpt::script::Entry& e : entries)
        if (e.object == "Anne") CHECK(e.flags == jpt::script::kEntrySimStep);
    // The save keeps ActivateSimStep: after the load, OnScriptInit(true) does not turn it on and it still steps.
    g_lines.clear();
    Begin(s, "m.scn", JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("runner stepping true")));
    Steps(s, 1);
    CHECK(Has(Print("hud got Tired 2")));
    jpt_script_session_destroy(s);
    CHECK(g_bytes == 0 && g_blocks.empty());
}

static void test_message_depth() {
    jpt_script_session* s = Session({{"scripts/e.nut",
                                      "class A extends GameScript { function OnMessage(s, p) { SendLGMessage(\"x\", p + 1); } function OnLevelStart() { SendLGMessage(\"x\", 0); } }\n"
                                      "class B extends GameScript { function OnMessage(s, p) { print(\"b \" + p); SendLGMessage(\"x\", p + 1); } }\n"}});
    Begin(s, "e.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    jpt_script_level_ready(s, 1);
    CHECK(Contains("messages nested deeper than 16"));
    CHECK(Has(Print("b 0")) && !Contains("print: b 40"));
    Close(s);
}

int main() {
    test_spitter();
    test_messages_and_steps();
    test_message_depth();
    test_lifecycle();
    test_budgets();
    printf("script_instance_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
