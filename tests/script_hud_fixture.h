// A level shaped as the Cloning Facility's scripts (our own text) for script_draw_tests, script_input_tests and
// script_player_tests: a HUD of bars and an additive icon, a map shown in modal mode and paged with keys or the MAP console
// command, a sprint on uCMD_USER1 and the map on uCMD_USER2; and the player's CPlayer script (kPlayerScript): stamina spent by
// sprinting and jumping and given back over time, in the speed and jump factors, spoken about. Over the model world of
// script_world.h.
#pragma once

#include "script_fixtures.h"
#include "script_world.h"

static const char* kHudScript =
    "uCMD_SPRINT <- uCMD_USER1;\n"
    "uCMD_MAP <- uCMD_USER2;\n"
    "class Hud {\n"
    "  bars = null; missing = null;\n"
    "  constructor() { bars = TP_Engine.GetTexture(\"scripts/bars.tga\"); missing = TP_Engine.GetTexture(\"scripts/none.tga\"); }\n"
    "  function Draw(owner) {\n"
    "    local plr = TP_Engine.GetPlayer(); if (!plr) return;\n"
    "    local sh = TP_Engine.GetScreenHeight();\n"
    "    TP_Engine.DrawTexture(bars, 0, 0, 64, 16, 30, sh - 46, 64, 16, 0x66FFFFFF, 1);\n"
    "    local w = (32 * (plr.GetHealth() / 100.0)).tointeger();\n"
    "    TP_Engine.DrawTexture(bars, 0, 16, w, 4, 40, sh - 40, w, 4, 0xCCD42020, 1);\n"
    "    if (owner.sprinting) TP_Engine.DrawTexture(bars, 0, 20, 8, 8, 34, sh - 44, 8, 8, TP_Engine.rgba(0, 255, 0, 128), 2);\n"
    "  }\n"
    "}\n"
    "class MapView {\n"
    "  map = null; page = 0;\n"
    "  constructor() { map = TP_Engine.GetTexture(\"scripts/maps/map\"); }\n"
    "  function OnKey(vk, pressed) {\n"
    "    if (!pressed) return;\n"
    "    if (vk == VK_RIGHT || vk == VK_NEXT) page++;\n"
    "    if ((vk == VK_LEFT || vk == VK_PRIOR) && page > 0) page--;\n"
    "    if (vk == VK_HOME) page = 0;\n"
    "    if (vk == VK_END) page = 9;\n"
    "  }\n"
    "  function Draw() {\n"
    "    local sw = TP_Engine.GetScreenWidth(), sh = TP_Engine.GetScreenHeight();\n"
    "    local s = sw.tofloat() / 1024.0; if (sh.tofloat() / 768.0 < s) s = sh.tofloat() / 768.0;\n"
    "    local mw = map.GetWidth() * s, mh = map.GetHeight() * s;\n"
    "    TP_Engine.DrawTexture(map, 0, 0, map.GetWidth(), map.GetHeight(), (sw - mw) / 2, (sh - mh) / 2, mw, mh, 0xFFFFFFFF, 1);\n"
    "  }\n"
    "}\n"
    "class GameHud extends GameScript {\n"
    "  hud = null; map = null; sprinting = false; moving = 0.0;\n"
    "  function OnScriptInit(b) { if (!TP_Engine.IsSoftwareMode()) { hud = Hud(); map = MapView(); } }\n"
    "  function OnPaintHW3D() { hud.Draw(this); if (bIsModalMode()) map.Draw(); }\n"
    "  function OnHandleInput() {\n"
    "    moving = fGetInputMoveAmountY();\n"
    "    sprinting = bKeyPressed(uCMD_SPRINT) && moving > 0;\n"
    "    if (bKeyHit(uCMD_MAP)) print(\"map \" + bSetModalMode(true));\n"
    "  }\n"
    "  function OnHandleModalInput() { if (bKeyHit(uCMD_MAP)) { bSetModalMode(false); print(\"map closed\"); } return true; }\n"
    "  function OnModalKey(vk, pressed, shift, ctrl, alt) { map.OnKey(vk, pressed); print(\"key \" + vk + \" \" + pressed + \" page \" + map.page); }\n"
    "  function OnModalAborted() { print(\"aborted\"); }\n"
    "  function OnConsoleCommand(cmd, args) {\n"
    "    if (cmd.toupper() != \"MAP\") return false;\n"
    "    print(\"console map \" + args + \" \" + bSetModalMode(!bIsModalMode()));\n"
    "    return true;\n"
    "  }\n"
    "}\n";

[[maybe_unused]] static const char* kPlayerScript =
    "class PlayerTI extends CPlayer {\n"
    "  nvram = null; sprinting = false;\n"
    "  function OnScriptInit(b) { if (!b) { nvram = { stamina = 100.0, delay = 0.0 }; ActivateSimStep(true); } sprinting = false;\n"
    "    print(\"player init \" + b + \" stamina=\" + nvram.stamina); }\n"
    "  function OnSimStep(d) {\n"
    "    if (sprinting) Spend(20.0 * d);\n"
    "    if (nvram.delay > 0.0) { nvram.delay -= d; if (nvram.delay <= 0.0) { nvram.delay = 0.0; Say(\"ANNETIRE5\"); } }\n"
    "    else if (nvram.stamina < 100.0) { nvram.stamina += 15.0 * d; if (nvram.stamina > 100.0) nvram.stamina = 100.0; }\n"
    "  }\n"
    "  function fGetJumpFactor() { print(\"jump factor stamina=\" + nvram.stamina); return nvram.stamina >= 22.5 ? 1.0 : 0.0; }\n"
    "  function fGetSpeedFactor(f) { if (sprinting && nvram.stamina > 0.0) return 1.5; if (nvram.stamina == 0.0) return f * 0.85; return f * 1.1; }\n"
    "  function OnJump() { print(\"jumped\"); Spend(22.5); }\n"
    "  function Spend(n) {\n"
    "    if (nvram.stamina > 0.0) { local before = nvram.stamina; nvram.stamina -= n;\n"
    "      if (nvram.stamina <= 0.0) nvram.stamina = 0.0; else if (nvram.stamina < 55.0 && before >= 80.0) Say(\"ANNETIRE3\"); }\n"
    "    nvram.delay = 1.5;\n"
    "  }\n"
    "  function SetSprint(b) { sprinting = b; }\n"
    "  function GetStamina() { return nvram.stamina; }\n"
    "  function Forward() { return Vector3.YAxis() * (r3HeadRotRel() * r3Rot()); }\n"
    "}\n"
    "class GameTI extends GameScript {\n"
    "  function OnHandleInput() {\n"
    "    local plr = TP_Engine.GetPlayer();\n"
    "    if (plr && plr instanceof PlayerTI) plr.SetSprint(!plr.bDead() && bKeyPressed(uCMD_USER1) && fGetInputMoveAmountY() > 0);\n"
    "  }\n"
    "}\n";

static World g_hud_world;
static jpt_script_engine g_hud_engine;

static inline jpt_script_session* HudSession(const Files& files, long instructions = 200000, size_t memory = 0) {
    jpt_script_session* s = Session(files);
    jpt_script_session_destroy(s);
    g_hud_world = World{};
    g_hud_world.player = g_hud_world.Add("Anne", "CPlayer");
    g_hud_world.objects[g_hud_world.player].hit_points = 50.0f;
    g_hud_world.pictures["scripts/bars.tga"] = {64, 32, 0xFFFFFFFFu};
    g_hud_world.pictures["scripts/maps/map"] = {256, 128, 0xFF808080u};
    g_hud_engine = EngineOf(g_hud_world);
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = instructions;
    config.memory_limit = memory;
    config.allocator = &kFake;
    config.engine = &g_hud_engine;
    s = jpt_script_session_create(&config);
    for (const auto& [path, text] : files) jpt_script_session_add_file(s, path.c_str());
    g_lines.clear();
    return s;
}

static inline void Ready(jpt_script_session* s, const char* scene, jpt_script_begin why = JPT_SCRIPT_BEGIN_NEW_GAME) {
    Begin(s, scene, why);
    jpt_script_level_ready(s, 1);
}

static inline jpt_script_input_result Input(jpt_script_session* s, uint32_t held, uint32_t hit, float move_y = 0.0f) {
    jpt_script_input in{held, hit, 0.0f, move_y};
    jpt_script_input_result out{};
    jpt_script_level_input(s, &in, &out);
    return out;
}
