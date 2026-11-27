// The player's CPlayer script as CE drives it (headless): fGetSpeedFactor on every movement request, fGetJumpFactor on one
// that asks to jump, OnJump when the player's jump leaves the ground, Say through the game's voice, r3HeadRotRel as the
// view relative to the body; defaults, clamping and failures; the handle refused after Restart, a load, a transition and the
// session's end; Restart and saves as CE keeps the script's state; the campaign without any of it.
#include <cmath>
#include <cstring>

#include "jpt_hash.h"
#include "jpt_level_scripts.h"
#include "script_api_fixture.h"

// Its speed factor scales the request by a member a console command sets; its jump factor and jump count live in nvram.
static const char* kPlayer =
    "class TestPlayer extends CPlayer {\n"
    "  nvram = null; scale = 2.0;\n"
    "  constructor() { nvram = { jumps = 0, jump = 1.0 }; }\n"
    "  function OnScriptInit(b) { print(\"player init \" + b + \" jumps=\" + nvram.jumps); }\n"
    "  function fGetSpeedFactor(f) { return f * scale; }\n"
    "  function fGetJumpFactor() { print(\"jump factor \" + nvram.jump); return nvram.jump; }\n"
    "  function OnJump() { nvram.jumps++; print(\"jumped \" + nvram.jumps); }\n"
    "  function Forward() { return Vector3.YAxis() * (r3HeadRotRel() * r3Rot()); }\n"
    "}\n";

static const char* kProbe =
    "class Probe extends GameScript {\n"
    "  kept = null;\n"
    "  function OnConsoleCommand(cmd, args) {\n"
    "    local p = TP_Engine.GetPlayer();\n"
    "    if (cmd == \"keep\") kept = p;\n"
    "    else if (cmd == \"say\") p.Say(args);\n"
    "    else if (cmd == \"say-number\") p.Say(5);\n"
    "    else if (cmd == \"say-none\") p.Say();\n"
    "    else if (cmd == \"stale\") kept.Say(\"TEST_SAMPLE_A\");\n"
    "    else if (cmd == \"stale-head\") kept.r3HeadRotRel();\n"
    "    else if (cmd == \"look\") { local f = p.Forward(); print(\"look \" + (f.x * 1000).tointeger() + \" \" + (f.y * 1000).tointeger() + \" \" + (f.z * 1000).tointeger()); }\n"
    "    else if (cmd == \"scale\") p.scale = args.tofloat();\n"
    "    else if (cmd == \"jump-factor\") p.nvram.jump = args.tofloat();\n"
    "    else if (cmd == \"jumps\") print(\"jumps \" + p.nvram.jumps + \" scale \" + p.scale + \" factor \" + p.nvram.jump);\n"
    "    else return false;\n"
    "    return true;\n"
    "  }\n"
    "}\n";

static const char* kScene = "player.scn";

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static jpt_script_session* PlayerLevel(long instructions = 200000, const char* player = kPlayer) {
    jpt_script_session* s = ApiSession({{"scripts/player.nut", player}, {"scripts/probe.nut", kProbe}}, instructions);
    jpt_script_session_add_reference(s, kScene, "TestPlayer", "Player", 0);
    g_world.samples = {jpt_sample_id("test_sample_a"), jpt_sample_id("voice/test_line")};
    return s;
}

static float Speed(jpt_script_session* s, float speed) { return jpt_script_level_speed_factor(s, g_world.player, speed); }
static float Jump(jpt_script_session* s) { return jpt_script_level_jump_factor(s, g_world.player); }
static int Console(jpt_script_session* s, const char* cmd, const char* args = nullptr) { return jpt_script_level_console(s, cmd, args); }

static void test_speed() {
    jpt_script_session* s = PlayerLevel();
    // Before the level is there, the request keeps its speed.
    Begin(s, kScene, JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f && !jpt_script_level_drives_player(s));
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("player init false jumps=0")) && jpt_script_level_drives_player(s));
    // Every request gets the script's answer for the speed the game asked.
    CHECK(Near(Speed(s, 0.5f), 1.0f) && Near(Speed(s, 0.78f), 1.56f) && Near(Speed(s, 1.0f), 2.0f));
    // Asked again each time: the script's state now.
    CHECK(Console(s, "scale", "0.25") && Near(Speed(s, 1.0f), 0.25f));
    // Not the player's handle: nothing asked.
    g_lines.clear();
    CHECK(jpt_script_level_speed_factor(s, 0u, 0.5f) == 0.5f && jpt_script_level_speed_factor(s, 12345u, 0.5f) == 0.5f && g_lines.empty());
    Close(s);
}

static void test_factor_failures() {
    struct {
        const char* body;
        float speed, jump;
        const char* log;
    } cases[] = {
        {"function fGetSpeedFactor(f) { return -2.0; } function fGetJumpFactor() { return -1; }", 0.0f, 0.0f, nullptr},
        {"function fGetSpeedFactor(f) { return 2; } function fGetJumpFactor() { return 3; }", 2.0f, 3.0f, nullptr},
        {"function fGetSpeedFactor(f) { throw \"bad speed\"; } function fGetJumpFactor() { throw \"bad jump\"; }", 0.5f, 1.0f, "bad speed"},
        {"function fGetSpeedFactor(f) { return \"fast\"; } function fGetJumpFactor() { return true; }", 0.5f, 1.0f, nullptr},
        {"function fGetSpeedFactor(f) { return 0.0 / 0.0; } function fGetJumpFactor() { return null; }", 0.5f, 1.0f, nullptr},
        {"function fGetSpeedFactor() { return 9.0; } function fGetJumpFactor(a, b) { return 9.0; }", 0.5f, 1.0f, "fGetSpeedFactor"},
        {"function fGetSpeedFactor(f) { while (true) {} } function fGetJumpFactor() { while (true) {} }", 0.5f, 1.0f, "instruction budget exceeded (20000)"},
    };
    for (const auto& c : cases) {
        const std::string script = std::string("class TestPlayer extends CPlayer { ") + c.body + " }\n";
        jpt_script_session* s = PlayerLevel(20000, script.c_str());
        Ready(s, kScene);
        CHECK(jpt_script_level_drives_player(s));
        CHECK(Speed(s, 0.5f) == c.speed);
        CHECK(Jump(s) == c.jump);
        CHECK(!c.log || Contains(c.log));
        Close(s);
    }
    // A CPlayer script without the factors: the game's own, and OnJump still called.
    jpt_script_session* s = PlayerLevel(200000, "class TestPlayer extends CPlayer { function OnJump() { print(\"only jump\"); } }\n");
    Ready(s, kScene);
    CHECK(!jpt_script_level_drives_player(s) && Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f);
    jpt_script_level_jump(s, g_world.player);
    CHECK(Has(Print("only jump")));
    Close(s);
}

static void test_jump() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, kScene);
    CHECK(Jump(s) == 1.0f && Has(Print("jump factor 1")));
    // OnJump for the player's own jump only.
    jpt_script_level_jump(s, g_world.player);
    CHECK(Has(Print("jumped 1")));
    jpt_script_level_jump(s, 0u);
    jpt_script_level_jump(s, 777u);
    CHECK(Count(Print("jumped")) == 1);
    jpt_script_level_jump(s, g_world.player);
    CHECK(Has(Print("jumped 2")));
    // The factor is the script's answer of the moment: 0 keeps the player on the ground.
    CHECK(Console(s, "jump-factor", "0") && Jump(s) == 0.0f);
    CHECK(Console(s, "jump-factor", "1.5") && Jump(s) == 1.5f);
    // While the level loads (Restart), no script is asked.
    Begin(s, kScene, JPT_SCRIPT_BEGIN_RESTART);
    g_lines.clear();
    jpt_script_level_jump(s, g_world.player);
    CHECK(g_lines.empty() && Jump(s) == 1.0f);
    Close(s);
}

static void test_say() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, kScene);
    // The sample by its name, case and path separator aside, through the player's own voice.
    CHECK(Console(s, "say", "TEST_SAMPLE_A") && Console(s, "say", "test_sample_a") && Console(s, "say", "VOICE\\Test_Line"));
    CHECK(g_world.spoken.size() == 3u);
    if (g_world.spoken.size() == 3u) {
        CHECK(g_world.spoken[0].player == g_world.player && g_world.spoken[0].sample == jpt_sample_id("test_sample_a"));
        CHECK(g_world.spoken[1].sample == g_world.spoken[0].sample && g_world.spoken[2].sample == jpt_sample_id("voice/test_line"));
    }
    // A sample the game does not have: the game ignores it, the script goes on.
    g_lines.clear();
    CHECK(Console(s, "say", "NO_SUCH_SAMPLE") && g_world.said == 4 && g_world.spoken.size() == 3u && !Contains("error"));
    // An empty name asks for nothing.
    CHECK(Console(s, "say", "") && g_world.said == 4);
    // Not a name: a script error.
    Console(s, "say-number");
    CHECK(Contains("Say(strSample): expects a string"));
    Close(s);
    // The player's own script speaks from its handlers.
    s = PlayerLevel(200000, "class TestPlayer extends CPlayer { function OnJump() { Say(\"VOICE/TEST_LINE\"); } }\n");
    Ready(s, kScene);
    jpt_script_level_jump(s, g_world.player);
    CHECK(g_world.spoken.size() == 1u && g_world.spoken.back().sample == jpt_sample_id("voice/test_line"));
    Close(s);
    s = PlayerLevel();
    Ready(s, kScene);
    Console(s, "say-none");
    CHECK(Contains("Say(strSample): expects a string"));
    Close(s);
}

static void test_head() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, kScene);
    // Head pitched 45 degrees up, body turned 90 degrees: Y rotated by the head, then by the body, as the game's view.
    const float h = std::sqrt(0.5f);
    g_world.objects[g_world.player].head_rotation = {std::cos(0.3926991f), std::sin(0.3926991f), 0.0f, 0.0f};
    g_world.objects[g_world.player].rotation = {h, 0.0f, 0.0f, h};
    Console(s, "look");
    CHECK(Has(Print("look -707 0 707")));
    // Level head and body: straight ahead.
    g_world.objects[g_world.player].head_rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    g_world.objects[g_world.player].rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    Console(s, "look");
    CHECK(Has(Print("look 0 1000 0")));
    Close(s);
}

// Restart, a load and a transition end the VM: nothing a script kept of the player survives, the new level's player answers.
static void test_handles() {
    for (jpt_script_begin why : {JPT_SCRIPT_BEGIN_RESTART, JPT_SCRIPT_BEGIN_SAVE, JPT_SCRIPT_BEGIN_TRANSITION}) {
        jpt_script_session* s = PlayerLevel();
        jpt_script_session_add_reference(s, "next.scn", "TestPlayer", "Player", 0);
        Ready(s, kScene);
        CHECK(Console(s, "keep") && Console(s, "stale") && g_world.said == 1);
        const unsigned generation = jpt_script_session_generation(s);
        const std::vector<uint8_t> state = SaveOf(s);
        Begin(s, why == JPT_SCRIPT_BEGIN_TRANSITION ? "next.scn" : kScene, why, why == JPT_SCRIPT_BEGIN_SAVE ? &state : nullptr);
        jpt_script_level_ready(s, 1);
        CHECK(jpt_script_session_generation(s) == generation + 1);
        CHECK(Console(s, "say", "TEST_SAMPLE_A") && g_world.said == 2);
        g_lines.clear();
        Console(s, "stale");
        CHECK(Contains("callback=OnConsoleCommand") && g_world.said == 2);
        Close(s);
    }
}

// The player kept by a script leaves the world: the reference is refused, nothing reaches the game.
static void test_stale_world() {
    for (const char* use : {"stale", "stale-head"}) {
        jpt_script_session* s = PlayerLevel();
        Ready(s, kScene);
        Console(s, "keep");
        g_world.objects.erase(g_world.player);
        g_lines.clear();
        Console(s, use);
        CHECK(Contains(!strcmp(use, "stale") ? "Say: the object is no longer in the world" : "r3HeadRotRel: the object is no longer in the world"));
        CHECK(g_world.said == 0);
        Close(s);
    }
}

static void test_restart_and_save() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, kScene);
    for (int i = 0; i < 3; ++i) jpt_script_level_jump(s, g_world.player);
    Console(s, "scale", "3");
    Console(s, "jump-factor", "0.5");
    CHECK(Jump(s) == 0.5f && Near(Speed(s, 1.0f), 3.0f));
    // Restart: the level from its start, the player's script with it.
    Begin(s, kScene, JPT_SCRIPT_BEGIN_RESTART);
    jpt_script_level_ready(s, 1);
    CHECK(Count(Print("player init false jumps=0")) == 2 && Jump(s) == 1.0f && Near(Speed(s, 1.0f), 2.0f));
    // A save keeps what CE keeps: the script's nvram, not its other members.
    for (int i = 0; i < 2; ++i) jpt_script_level_jump(s, g_world.player);
    Console(s, "scale", "3");
    Console(s, "jump-factor", "0.5");
    const std::vector<uint8_t> state = SaveOf(s);
    Begin(s, kScene, JPT_SCRIPT_BEGIN_SAVE, &state);
    jpt_script_level_ready(s, 1);
    Console(s, "jumps");
    CHECK(Has(Print("player init true jumps=2")) && Has(Print("jumps 2 scale 2 factor 0.5")));
    CHECK(Jump(s) == 0.5f && Near(Speed(s, 0.5f), 1.0f));
    // A level without the player's script: the game's own movement.
    Begin(s, "plain.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_drives_player(s) && Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f);
    Close(s);
    // nvram declared null and made only for a new game: the save's slots become its table, as in CE.
    const char* lazy =
        "class TestPlayer extends CPlayer {\n"
        "  nvram = null;\n"
        "  function OnScriptInit(b) { if (!b) nvram = { count = 0 }; nvram.count++; print(\"lazy \" + b + \" count=\" + nvram.count); }\n"
        "}\n";
    s = PlayerLevel(200000, lazy);
    Ready(s, kScene);
    CHECK(Has(Print("lazy false count=1")));
    const std::vector<uint8_t> saved = SaveOf(s);
    Begin(s, kScene, JPT_SCRIPT_BEGIN_SAVE, &saved);
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("lazy true count=2")) && !Contains("nvram not restored"));
    Close(s);
}

// The player's script beside game scripts that read input and draw: each does its own part.
static void test_with_game_scripts() {
    const char* game =
        "class Watcher extends GameScript {\n"
        "  tex = null;\n"
        "  function OnScriptInit(b) { tex = TP_Engine.GetTexture(\"textures/white.tga\"); }\n"
        "  function OnHandleInput() { if (bKeyHit(uCMD_USER1)) print(\"user1\"); }\n"
        "  function OnPaintHW3D() { TP_Engine.DrawTexture(tex, 0, 0, 8, 4, 0, 0, 8, 4, 0xFFFFFFFF, 2); }\n"
        "}\n";
    jpt_script_session* s = ApiSession({{"scripts/player.nut", kPlayer}, {"scripts/probe.nut", kProbe}, {"scripts/watcher.nut", game}});
    jpt_script_session_add_reference(s, kScene, "TestPlayer", "Player", 0);
    Ready(s, kScene);
    CHECK(jpt_script_session_user_commands(s) == 0x1u && jpt_script_level_drives_player(s));
    Input(s, 0x1u, 0x1u, 1.0f);
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    CHECK(Has(Print("user1")) && f && f->count == 1 && f->cmds[0].blend == JPT_DRAW_ADD);
    CHECK(Near(Speed(s, 0.5f), 1.0f) && Jump(s) == 1.0f);
    Close(s);
}

// The campaign has no script session: the player moves, jumps and speaks as the game does.
static void test_campaign() {
    jpt_level_scripts_end();
    jpt_level_scripts_start(JPT_SESSION_CAMPAIGN, CaptureLine);
    g_lines.clear();
    CHECK(!jpt_level_scripts_session() && !jpt_level_scripts_drives_player());
    CHECK(jpt_level_scripts_speed_factor(1u, 0.5f) == 0.5f && jpt_level_scripts_jump_factor(1u) == 1.0f);
    jpt_level_scripts_jump(1u);
    CHECK(!jpt_level_scripts_console("anything", nullptr) && g_lines.empty());
}

int main() {
    test_speed();
    test_factor_failures();
    test_jump();
    test_say();
    test_head();
    test_handles();
    test_stale_world();
    test_restart_and_save();
    test_with_game_scripts();
    test_campaign();
    printf("script_player_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
