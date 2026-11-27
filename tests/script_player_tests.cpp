// The player's CPlayer script as CE drives it (headless): fGetSpeedFactor on every movement request, fGetJumpFactor on one
// that asks to jump, OnJump when the player's jump leaves the ground, Say through the game's voice, r3HeadRotRel as the
// view relative to the body; defaults, clamping and failures; the handle refused after Restart, a load, a transition and the
// session's end; Restart clearing the script's state; the campaign without any of it.
#include <cmath>
#include <cstring>

#include "jpt_hash.h"
#include "jpt_level_scripts.h"
#include "script_hud_fixture.h"

static const char* kProbe =
    "class Probe extends GameScript {\n"
    "  kept = null;\n"
    "  function OnConsoleCommand(cmd, args) {\n"
    "    local p = TP_Engine.GetPlayer();\n"
    "    if (cmd == \"keep\") kept = p;\n"
    "    else if (cmd == \"say\") p.Say(args);\n"
    "    else if (cmd == \"say-number\") p.Say(5);\n"
    "    else if (cmd == \"say-none\") p.Say();\n"
    "    else if (cmd == \"stale\") kept.Say(\"ANNETIRE5\");\n"
    "    else if (cmd == \"stale-head\") kept.r3HeadRotRel();\n"
    "    else if (cmd == \"look\") { local f = p.Forward(); print(\"look \" + (f.x * 1000).tointeger() + \" \" + (f.y * 1000).tointeger() + \" \" + (f.z * 1000).tointeger()); }\n"
    "    else if (cmd == \"stamina\") print(\"stamina \" + p.GetStamina());\n"
    "    else return false;\n"
    "    return true;\n"
    "  }\n"
    "}\n";

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static jpt_script_session* PlayerLevel(const char* scene = "ti.scn", long instructions = 200000, const char* player = kPlayerScript) {
    jpt_script_session* s = HudSession({{"scripts/hud.nut", kHudScript}, {"scripts/player.nut", player}, {"scripts/probe.nut", kProbe}}, instructions);
    jpt_script_session_add_reference(s, scene, "PlayerTI", "Anne", 0);
    g_hud_world.samples = {jpt_sample_id("annetire3"), jpt_sample_id("annetire5"), jpt_sample_id("vo/anne_hello")};
    return s;
}

static float Speed(jpt_script_session* s, float speed) { return jpt_script_level_speed_factor(s, g_hud_world.player, speed); }
static float Jump(jpt_script_session* s) { return jpt_script_level_jump_factor(s, g_hud_world.player); }
static int Console(jpt_script_session* s, const char* cmd, const char* args = nullptr) { return jpt_script_level_console(s, cmd, args); }

static void test_speed() {
    jpt_script_session* s = PlayerLevel();
    // Before the level is there, the request keeps its speed.
    Begin(s, "ti.scn", JPT_SCRIPT_BEGIN_NEW_GAME);
    CHECK(Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f && !jpt_script_level_drives_player(s));
    jpt_script_level_ready(s, 1);
    CHECK(Has(Print("player init false stamina=100")) && jpt_script_level_drives_player(s));
    // Walk, walk-strafe and run: the script's factor of the unmodified one.
    CHECK(Near(Speed(s, 0.5f), 0.55f) && Near(Speed(s, 0.78f), 0.858f) && Near(Speed(s, 1.0f), 1.1f));
    // Sprinting (USER1 held, moving forward): the sprint speed whatever the key.
    Input(s, 0x1u, 0x1u, 1.0f);
    CHECK(Near(Speed(s, 0.5f), 1.5f) && Near(Speed(s, 0.0f), 1.5f));
    // Sprinting spends it all: then slower than walking.
    Steps(s, 60);
    Console(s, "stamina");
    CHECK(Has(Print("stamina 0")) && Near(Speed(s, 1.0f), 0.85f));
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
        const std::string script = std::string("class PlayerTI extends CPlayer { ") + c.body + " }\n";
        jpt_script_session* s = PlayerLevel("ti.scn", 20000, script.c_str());
        Ready(s, "ti.scn");
        CHECK(jpt_script_level_drives_player(s));
        CHECK(Speed(s, 0.5f) == c.speed);
        CHECK(Jump(s) == c.jump);
        CHECK(!c.log || Contains(c.log));
        Close(s);
    }
    // A CPlayer script without the factors: the game's own.
    jpt_script_session* s = PlayerLevel("ti.scn", 200000, "class PlayerTI extends CPlayer { function OnJump() { print(\"only jump\"); } }\n");
    Ready(s, "ti.scn");
    CHECK(!jpt_script_level_drives_player(s) && Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f);
    jpt_script_level_jump(s, g_hud_world.player);
    CHECK(Has(Print("only jump")));
    Close(s);
}

static void test_jump() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, "ti.scn");
    CHECK(Jump(s) == 1.0f && Has(Print("jump factor stamina=100")));
    // Each jump that leaves the ground spends stamina; another object's jump is not the player's.
    jpt_script_level_jump(s, g_hud_world.player);
    CHECK(Count(Print("jumped")) == 1);
    jpt_script_level_jump(s, 0u);
    jpt_script_level_jump(s, 777u);
    CHECK(Count(Print("jumped")) == 1);
    for (int i = 0; i < 3; ++i) jpt_script_level_jump(s, g_hud_world.player);
    Console(s, "stamina");
    CHECK(Count(Print("jumped")) == 4 && Has(Print("stamina 10")));
    // Too tired: the factor that keeps the player on the ground.
    CHECK(Jump(s) == 0.0f);
    // Rested: it jumps again.
    Steps(s, 70);
    CHECK(Jump(s) == 1.0f);
    // While the level loads (Restart), no script is asked.
    Begin(s, "ti.scn", JPT_SCRIPT_BEGIN_RESTART);
    g_lines.clear();
    jpt_script_level_jump(s, g_hud_world.player);
    CHECK(g_lines.empty() && Jump(s) == 1.0f);
    Close(s);
}

static void test_say() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, "ti.scn");
    // The sample by its name, case and path separator aside, through the player's own voice.
    CHECK(Console(s, "say", "ANNETIRE5") && Console(s, "say", "annetire5") && Console(s, "say", "VO\\Anne_Hello"));
    CHECK(g_hud_world.spoken.size() == 3u);
    if (g_hud_world.spoken.size() == 3u) {
        CHECK(g_hud_world.spoken[0].player == g_hud_world.player && g_hud_world.spoken[0].sample == jpt_sample_id("annetire5"));
        CHECK(g_hud_world.spoken[1].sample == g_hud_world.spoken[0].sample && g_hud_world.spoken[2].sample == jpt_sample_id("vo/anne_hello"));
    }
    // A sample the game does not have: the game ignores it, the script goes on.
    g_lines.clear();
    CHECK(Console(s, "say", "NO_SUCH_SAMPLE") && g_hud_world.said == 4 && g_hud_world.spoken.size() == 3u && !Contains("error"));
    // An empty name asks for nothing.
    CHECK(Console(s, "say", "") && g_hud_world.said == 4);
    // Not a name: a script error.
    Console(s, "say-number");
    CHECK(Contains("Say(strSample): expects a string"));
    Close(s);
    // The script's own Say: the stamina's end of its delay.
    s = PlayerLevel();
    Ready(s, "ti.scn");
    jpt_script_level_jump(s, g_hud_world.player);
    Steps(s, 16);
    CHECK(!g_hud_world.spoken.empty() && g_hud_world.spoken.back().sample == jpt_sample_id("annetire5"));
    Close(s);
    s = PlayerLevel();
    Ready(s, "ti.scn");
    Console(s, "say-none");
    CHECK(Contains("Say(strSample): expects a string"));
    Close(s);
}

static void test_head() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, "ti.scn");
    // Head pitched 45 degrees up, body turned 90 degrees: Y rotated by the head, then by the body, as the game's view.
    const float h = std::sqrt(0.5f);
    g_hud_world.objects[g_hud_world.player].head_rotation = {std::cos(0.3926991f), std::sin(0.3926991f), 0.0f, 0.0f};
    g_hud_world.objects[g_hud_world.player].rotation = {h, 0.0f, 0.0f, h};
    Console(s, "look");
    CHECK(Has(Print("look -707 0 707")));
    // Level head and body: straight ahead.
    g_hud_world.objects[g_hud_world.player].head_rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    g_hud_world.objects[g_hud_world.player].rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    Console(s, "look");
    CHECK(Has(Print("look 0 1000 0")));
    Close(s);
}

// Restart, a load and a transition end the VM: nothing a script kept of the player survives, the new level's player answers.
static void test_handles() {
    for (jpt_script_begin why : {JPT_SCRIPT_BEGIN_RESTART, JPT_SCRIPT_BEGIN_SAVE, JPT_SCRIPT_BEGIN_TRANSITION}) {
        jpt_script_session* s = PlayerLevel();
        jpt_script_session_add_reference(s, "next.scn", "PlayerTI", "Anne", 0);
        Ready(s, "ti.scn");
        CHECK(Console(s, "keep") && Console(s, "stale") && g_hud_world.said == 1);
        const unsigned generation = jpt_script_session_generation(s);
        const std::vector<uint8_t> state = SaveOf(s);
        Begin(s, why == JPT_SCRIPT_BEGIN_TRANSITION ? "next.scn" : "ti.scn", why, why == JPT_SCRIPT_BEGIN_SAVE ? &state : nullptr);
        jpt_script_level_ready(s, 1);
        CHECK(jpt_script_session_generation(s) == generation + 1);
        CHECK(Console(s, "say", "ANNETIRE5") && g_hud_world.said == 2);
        g_lines.clear();
        Console(s, "stale");
        CHECK(Contains("callback=OnConsoleCommand") && g_hud_world.said == 2);
        Close(s);
    }
}

// The player kept by a script leaves the world: the reference is refused, nothing reaches the game.
static void test_stale_world() {
    for (const char* use : {"stale", "stale-head"}) {
        jpt_script_session* s = PlayerLevel();
        Ready(s, "ti.scn");
        Console(s, "keep");
        g_hud_world.objects.erase(g_hud_world.player);
        g_lines.clear();
        Console(s, use);
        CHECK(Contains(!strcmp(use, "stale") ? "Say: the object is no longer in the world" : "r3HeadRotRel: the object is no longer in the world"));
        CHECK(g_hud_world.said == 0);
        Close(s);
    }
}

static void test_restart_clears() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, "ti.scn");
    Input(s, 0x1u, 0x1u, 1.0f);
    for (int i = 0; i < 4; ++i) jpt_script_level_jump(s, g_hud_world.player);
    CHECK(Jump(s) == 0.0f && Near(Speed(s, 1.0f), 1.5f));
    // Restart: the level from its start, the player's script with it.
    Begin(s, "ti.scn", JPT_SCRIPT_BEGIN_RESTART);
    jpt_script_level_ready(s, 1);
    CHECK(Count(Print("player init false stamina=100")) == 2 && Jump(s) == 1.0f && Near(Speed(s, 1.0f), 1.1f));
    // A save keeps what CE keeps: the script's nvram (stamina), not its sprint or any factor.
    for (int i = 0; i < 3; ++i) jpt_script_level_jump(s, g_hud_world.player);
    Input(s, 0x1u, 0x1u, 1.0f);
    const std::vector<uint8_t> state = SaveOf(s);
    Begin(s, "ti.scn", JPT_SCRIPT_BEGIN_SAVE, &state);
    jpt_script_level_ready(s, 1);
    Console(s, "stamina");
    // The script makes its nvram only when not loading: the save's becomes it, as in CE.
    CHECK(Has(Print("player init true stamina=32.5")) && Has(Print("stamina 32.5")) && !Contains("nvram not restored"));
    CHECK(Jump(s) == 1.0f && Near(Speed(s, 0.5f), 0.55f));
    // A level without the player's script: the game's own movement.
    Begin(s, "plain.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_drives_player(s) && Speed(s, 0.5f) == 0.5f && Jump(s) == 1.0f);
    Close(s);
}

// The HUD, the modal map and the sprint of the fixture still work with the player's script there.
static void test_level_together() {
    jpt_script_session* s = PlayerLevel();
    Ready(s, "ti.scn");
    CHECK(jpt_script_session_user_commands(s) == 3u);
    Input(s, 0x1u, 0x1u, 1.0f);
    int additive = 0;
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    for (int i = 0; f && i < f->count; i++) additive += f->cmds[i].blend == JPT_DRAW_ADD;
    CHECK(additive == 1 && Near(Speed(s, 0.5f), 1.5f));
    CHECK(Input(s, 0u, 0x2u).modal && Has(Print("map true")));
    CHECK(Console(s, "MAP", "off") && Has(Print("console map off true")) && !jpt_script_level_modal(s));
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
    CHECK(!jpt_level_scripts_console("MAP", nullptr) && g_lines.empty());
}

int main() {
    test_speed();
    test_factor_failures();
    test_jump();
    test_say();
    test_head();
    test_handles();
    test_stale_world();
    test_restart_clears();
    test_level_together();
    test_campaign();
    printf("script_player_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
