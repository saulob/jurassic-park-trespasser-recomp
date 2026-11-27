// CE's script API bound into a VM (headless): every registered name exists, Vector3 and Rotate3 as the engine computes them,
// TP_Engine's time and random numbers, and the world's objects as handles checked at every call.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "jpt_script_math.h"
#include "jpt_script_plan.h"
#include "jpt_script_vm.h"
#include "script_world.h"

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static std::vector<std::string> g_lines;
static void Capture(void*, const char* line) { g_lines.push_back(line); }

static bool Has(const std::string& line) {
    for (const std::string& l : g_lines)
        if (l == line) return true;
    return false;
}

static bool Contains(const std::string& part) {
    for (const std::string& l : g_lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

static std::string Print(const std::string& text) { return "[script] print: " + text; }

static World g_world;
static jpt_script_engine g_engine;

static jpt_script_vm* Vm(unsigned seed = 7) {
    g_lines.clear();
    g_engine = EngineOf(g_world);
    jpt_script_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.instructions = 200000;
    config.engine = &g_engine;
    config.random_seed = seed;
    jpt_script_vm* vm = jpt_script_vm_create(&config);
    jpt_script_vm_set_level(vm, "t.scn");
    jpt_script_vm_set_world(vm, 1, 1);
    return vm;
}

static jpt_script_result Run(jpt_script_vm* vm, const std::string& source) {
    return jpt_script_vm_load(vm, "t.nut", source.data(), source.size());
}

// The registry is what the launcher accepts: each name must be there in a VM, on its owner.
static void test_registry_in_runtime() {
    jpt_script_vm* vm = Vm();
    std::string source;
    int names = 0;
    for (const jpt::script::ApiEntry& e : jpt::script::Api()) {
        if (e.kind == jpt::script::ApiKind::kHandler) continue;
        if (e.kind == jpt::script::ApiKind::kClass && (!strcmp(e.name, "GameScript") || !strcmp(e.name, "LevelScript"))) continue;
        const std::string owner = *e.owner ? e.owner : "getroottable()";
        const std::string missing = "print(\"missing " + std::string(e.owner) + "." + e.name + "\");";
        // A field is read through the class's _get: on an object of no world it refuses, it does not say the index is unknown.
        if (e.kind == jpt::script::ApiKind::kField)
            source += "try { " + owner + ".instance()." + e.name + "; " + missing + " } catch (x) { if (typeof x != \"string\" || x.find(\"does not exist\") != null) " +
                      missing + " }\n";
        else
            source += "if (!(\"" + std::string(e.name) + "\" in " + owner + ")) " + missing + "\n";
        CHECK(jpt::script::IsCeName(e.name));
        CHECK(jpt::script::IsImplemented(e.name));
        names++;
    }
    CHECK(names > 40);
    CHECK(Run(vm, source) == JPT_SCRIPT_OK);
    CHECK(!Contains("missing"));
    for (const std::string& l : g_lines)
        if (l.find("missing") != std::string::npos) printf("  %s\n", l.c_str());
    // Inherited along CE's class tree.
    CHECK(Run(vm, "print((\"r3Rot\" in CAnimal) + \" \" + (\"Teleport\" in CBoundaryBox) + \" \" + (\"v3GetHeadPos\" in CInstance));") == JPT_SCRIPT_OK);
    CHECK(Has(Print("true true false")));
    CHECK(!jpt::script::IsImplemented("DrawBitmap") && !jpt::script::IsImplemented("ActionType") && !jpt::script::IsImplemented("OnActBite"));
    jpt_script_vm_destroy(vm);
}

static void test_vector3() {
    jpt_script_vm* vm = Vm();
    CHECK(Run(vm,
              "local a = Vector3(1, 2, 3), b = Vector3(a), c = Vector3();\n"
              "print(a + \" \" + b + \" \" + c);\n"
              "b.x = 4; b.y = 5.5; b.z = -1;\n"
              "print(b.x + \" \" + b.y + \" \" + b.z);\n"
              "print((a + b) + \" \" + (a - b) + \" \" + (a * b) + \" \" + (a / Vector3(2, 4, 3)));\n"
              "print((a + 1) + \" \" + (a - 1) + \" \" + (a * 2) + \" \" + (a / 2));\n"
              "print(a.Dot(Vector3(4, 5, 6)) + \" \" + Vector3(1, 0, 0).Cross(Vector3(0, 1, 0)) + \" \" + Vector3(3, 4, 0).Length() + \" \" + Vector3(3, 4, 0).LengthSqr());\n"
              "local n = Vector3(0, 3, 4); n.Normalise(); print(n);\n"
              "local z = Vector3(); z.Normalise(); print(z);\n"
              "local s = Vector3(1, 2, 3); s.Scale(2); print(s);\n"
              "print(Vector3.XAxis() + \" \" + Vector3.YAxis() + \" \" + Vector3.ZAxis());\n"
              "local k = clone a; k.x = 9; print(a.x + \" \" + k.x);\n") == JPT_SCRIPT_OK);
    CHECK(Has(Print("Vector3(1, 2, 3) Vector3(1, 2, 3) Vector3(0, 0, 0)")));
    CHECK(Has(Print("4 5.5 -1")));
    CHECK(Has(Print("Vector3(5, 7.5, 2) Vector3(-3, -3.5, 4) Vector3(4, 11, -3) Vector3(0.5, 0.5, 1)")));
    CHECK(Has(Print("Vector3(2, 3, 4) Vector3(0, 1, 2) Vector3(2, 4, 6) Vector3(0.5, 1, 1.5)")));
    CHECK(Has(Print("32 Vector3(0, 0, 1) 5 25")));
    CHECK(Has(Print("Vector3(0, 0.6, 0.8)")));
    CHECK(Has(Print("Vector3(0, 0, 0)")));
    CHECK(Has(Print("Vector3(2, 4, 6)")));
    CHECK(Has(Print("Vector3(1, 0, 0) Vector3(0, 1, 0) Vector3(0, 0, 1)")));
    CHECK(Has(Print("1 9")));
    // Safe division and checked arguments: script errors, never a value that is not a number.
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "print(Vector3(1, 1, 1) / 0);") == JPT_SCRIPT_FAILED && Contains("division by zero"));
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "print(Vector3(1, 1, 1) / Vector3(1, 0, 1));") == JPT_SCRIPT_FAILED);
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "Vector3(1, \"a\", 2);") == JPT_SCRIPT_FAILED && Contains("Vector3: expects"));
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "print(Vector3(1, 2, 3).w);") == JPT_SCRIPT_FAILED && Contains("the index 'w' does not exist"));
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "Vector3(1, 2, 3).Dot(5);") == JPT_SCRIPT_FAILED);
    jpt_script_vm_destroy(vm);
}

static bool Near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_rotate3() {
    jpt_script_vm* vm = Vm();
    // A quarter turn about Z takes the Y axis (the engine's forward) to -X; the engine's own forward rule says so too.
    CHECK(Run(vm,
              "local h = sqrt(0.5);\n"
              "local q = Rotate3(h, 0, 0, h);\n"
              "local f = Vector3.YAxis() * q;\n"
              "print(format(\"%.4f %.4f %.4f\", f.x, f.y, f.z));\n"
              "local p = Rotate3(h, h, 0, 0);\n"
              "local v = Vector3(0.3, -1, 2);\n"
              "local one = v * (q * p), two = (v * q) * p;\n"
              "print(\"same \" + ((one - two).Length() < 1e-5));\n"
              "local back = v * q * (-q);\n"
              "print(format(\"%.4f %.4f %.4f\", back.x, back.y, back.z));\n"
              "local d = v * (q / q);\n"
              "print(format(\"%.4f %.4f %.4f\", d.x, d.y, d.z));\n"
              "local half = q * 0.5, twice = v * half * half, whole = v * q;\n"
              "print(format(\"%.4f %.4f %.4f\", twice.x - whole.x, twice.y - whole.y, twice.z - whole.z));\n"
              "local id = v * (q * 0.0); print(format(\"%.4f %.4f %.4f\", id.x, id.y, id.z));\n"
              "local r = Rotate3(2, 0, 0, 0); print(r.Length() + \" \" + r.LengthSqr()); r.Normalise(); print(r);\n"
              "local s = Rotate3(q); s.Scale(2); local g = Vector3.YAxis() * s; print(format(\"%.4f %.4f\", g.x, g.y));\n"
              "local w = Rotate3(0.5, Vector3(0.5, 0.5, 0.5)); print(w.w + \" \" + w.x + \" \" + w.y + \" \" + w.z);\n"
              "local z = Rotate3(0, 0, 0, 0); z.Normalise(); print(z);\n") == JPT_SCRIPT_OK);
    CHECK(Has(Print("-1.0000 0.0000 0.0000")));
    CHECK(Has(Print("same true")));
    CHECK(Has(Print("0.3000 -1.0000 2.0000")));
    CHECK(Has(Print("2 4")) && Has(Print("Rotate3(1, 0, 0, 0)")));
    CHECK(Has(Print("0.0000 -1.0000")));
    CHECK(Has(Print("0.5 0.5 0.5 0.5")));
    jpt::script::Q q{0.7071068f, 0.0f, 0.0f, 0.7071068f};
    const jpt::script::V3 f = jpt::script::Rotate({0.0f, 1.0f, 0.0f}, q);
    CHECK(Near(f.x, -1.0f) && Near(f.y, 0.0f) && Near(f.z, 0.0f));
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "Rotate3(1, 2);") == JPT_SCRIPT_FAILED && Contains("Rotate3(fW, v3): expects"));
    jpt_script_vm_destroy(vm);
}

static void test_engine_functions() {
    g_world.time = 12.5f;
    jpt_script_vm* vm = Vm(11);
    CHECK(Run(vm,
              "print(TP_Engine.GetElapsedSimTime());\n"
              "local ok = true;\n"
              "for (local i = 0; i < 2000; i++) {\n"
              "  local f = TP_Engine.GetRandFloatRanged(0.0, 0.9999999); if (f < 0.0 || f > 0.9999999) ok = false;\n"
              "  local n = TP_Engine.GetRandIntRanged(3, 7); if (n < 3 || n >= 7) ok = false;\n"
              "}\n"
              "print(ok + \" \" + TP_Engine.GetRandIntRanged(5, 5) + \" \" + typeof TP_Engine.GetRandInt());\n"
              "print(TP_Engine.GetRandFloatRanged(-2, 2));\n") == JPT_SCRIPT_OK);
    CHECK(Has(Print("12.5")) && Has(Print("true 5 integer")));
    const std::string first = g_lines.back();
    jpt_script_vm_destroy(vm);
    vm = Vm(11);
    CHECK(Run(vm, "for (local i = 0; i < 2000; i++) { TP_Engine.GetRandFloatRanged(0.0, 1.0); TP_Engine.GetRandIntRanged(0, 4); } TP_Engine.GetRandInt(); print(TP_Engine.GetRandFloatRanged(-2, 2));") == JPT_SCRIPT_OK);
    CHECK(!g_lines.empty() && g_lines.back() == first);
    jpt_script_vm_destroy(vm);
    g_world.time = 0.0f;
}

static void test_entity_handles() {
    g_world = World{};
    const uint32_t dino = g_world.Add("Dino-00", "CAnimal");
    const uint32_t rock = g_world.Add("Rock-00", "CInstance");
    g_world.objects[dino].position = {1, 2, 3};
    g_world.objects[dino].head = {1, 3, 4};
    g_world.objects[dino].boxes[3] = {0.7071068f, 0, 0, 0.7071068f};
    jpt_script_vm* vm = Vm();
    CHECK(Run(vm,
              "gDino <- null; gRock <- null;\n"
              "class Dino extends CAnimal { constructor() { gDino = this; } function Hi() { print(\"pos \" + v3Pos() + \" head \" + v3GetHeadPos()); } }\n"
              "class Rock extends CInstance { constructor() { gRock = this; } }\n"
              "class NotEntity { }\n"
              "function Show() { gDino.Hi(); local b = gDino.pbbBoundaryBox(EBoundaryBoxes.ebbHEAD); local d = Vector3.YAxis() * b.r3Rot();\n"
              "  print(format(\"box %.3f %.3f\", d.x, d.y) + \" \" + (gDino.pbbBoundaryBox(EBoundaryBoxes.ebbTAIL) == null) + \" \" + (b instanceof CBoundaryBox));\n"
              "  print(gDino.GetInstanceName() + \" \" + (gDino.u4GetUniqueHandle() != 0)); }\n"
              "function Move() { gRock.Teleport(Vector3(5, 6, 7), Rotate3(2, 0, 0, 0)); print(gRock.MoveWithFixedVelocity(Vector3(0, 7, 0), 60, false) + \" \" + gRock.MoveWithFixedVelocity(Vector3(0, 7, 0), 60, true)); }\n"
              "function Bad() { gRock.Teleport(Vector3(1e38, 0, 0) * 1e38, Rotate3()); }\n"
              "function ShowRock() { print(\"rock \" + gRock.v3Pos()); }\n") == JPT_SCRIPT_OK);
    CHECK(std::string(jpt_script_vm_native_base(vm, "Dino")) == "CAnimal" && std::string(jpt_script_vm_native_base(vm, "Rock")) == "CInstance");
    CHECK(std::string(jpt_script_vm_native_base(vm, "NotEntity")).empty());
    CHECK(jpt_script_vm_attach(vm, "Dino:Dino-00", "Dino", dino) > 0 && jpt_script_vm_attach(vm, "Rock:Rock-00", "Rock", rock) > 0);
    CHECK(jpt_script_vm_call(vm, "Show", "Show") == JPT_SCRIPT_OK);
    CHECK(Has(Print("pos Vector3(1, 2, 3) head Vector3(1, 3, 4)")));
    CHECK(Has(Print("box -1.000 0.000 true true")));
    char name[16];
    snprintf(name, sizeof name, "%08X", dino);
    CHECK(Has(Print(std::string(name) + " true")));
    CHECK(jpt_script_vm_call(vm, "Move", "Move") == JPT_SCRIPT_OK && Has(Print("true false")));
    CHECK(g_world.teleported.size() == 1 && g_world.objects[rock].position.x == 5.0f && g_world.objects[rock].rotation.w == 1.0f);
    CHECK(g_world.moves.size() == 1 && g_world.moves[0].velocity.y == 7.0f && g_world.moves[0].milliseconds == 60);
    CHECK(jpt_script_vm_call(vm, "Bad", "Bad") == JPT_SCRIPT_FAILED && Contains("Teleport: a position or rotation that is not finite"));
    CHECK(g_world.teleported.size() == 1);

    // The object leaves the world: a script error that names it, never a stale object.
    g_world.objects.erase(rock);
    CHECK(jpt_script_vm_call(vm, "ShowRock", "ShowRock") == JPT_SCRIPT_FAILED && Contains("v3Pos: the object is no longer in the world"));
    // The world is being replaced: nothing of it is reached.
    jpt_script_vm_set_world(vm, 1, 0);
    CHECK(jpt_script_vm_call(vm, "Show2", "Show") == JPT_SCRIPT_FAILED && Contains("the world is not loaded"));
    // A handle of an older world generation.
    jpt_script_vm_set_world(vm, 2, 1);
    CHECK(jpt_script_vm_call(vm, "Show3", "Show") == JPT_SCRIPT_FAILED && Contains("its world was unloaded"));
    // An entity a script made itself is no object of the world.
    CHECK(Run(vm, "CInstance().v3Pos();") == JPT_SCRIPT_FAILED && Contains("v3Pos: it was made by a script"));
    jpt_script_vm_destroy(vm);
}

// nvram keeps Vector3, Rotate3 and objects of the world, as values: an object by its unique handle, found again in the next world.
static void test_nvram_values() {
    g_world = World{};
    const uint32_t rock = g_world.Add("Rock-00", "CInstance");
    jpt_script_vm* vm = Vm();
    CHECK(Run(vm,
              "gRock <- null;\n"
              "class Rock extends CInstance { constructor() { gRock = this; } }\n"
              "class K { nvram = null; constructor() { nvram = { v = Vector3(1, 2, 3), q = Rotate3(0, 1, 0, 0), list = [Vector3(4, 5, 6)] }; }\n"
              "  function Keep() { nvram.o <- gRock; nvram.list.append(gRock); }\n"
              "  function Show() { print(nvram.v + \" \" + nvram.q + \" \" + nvram.o.GetInstanceName() + \" \" + nvram.list[0] + \" \" + (nvram.list[1] == nvram.o)); } }\n") ==
          JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_attach(vm, "Rock", "Rock", rock) > 0);
    const int k = jpt_script_vm_instantiate(vm, "K", "K");
    CHECK(jpt_script_vm_invoke(vm, k, "K", "Keep", nullptr, 0) == JPT_SCRIPT_OK);
    std::vector<jpt::script::Slot> slots;
    std::vector<std::string> notes;
    CHECK(jpt_script_vm_get_nvram(vm, k, slots, notes) && slots.size() == 4 && notes.empty());
    jpt_script_vm_destroy(vm);

    std::vector<jpt::script::Entry> entries(1), back;
    entries[0].kind = 4;
    entries[0].class_name = "K";
    entries[0].slots = slots;
    std::string problem;
    const std::vector<uint8_t> bytes = jpt::script::EncodeState(entries);
    CHECK(jpt::script::DecodeState(bytes.data(), bytes.size(), back, problem) && back.size() == 1 && back[0].slots.size() == 4);
    for (const jpt::script::Slot& slot : back[0].slots) {
        if (slot.name == "v") CHECK(slot.value.type == jpt::script::Value::kVector3 && slot.value.v[2] == 3.0f);
        if (slot.name == "q") CHECK(slot.value.type == jpt::script::Value::kRotate3 && slot.value.v[1] == 1.0f);
        if (slot.name == "o") CHECK(slot.value.type == jpt::script::Value::kInstance && static_cast<uint32_t>(slot.value.i) == rock);
    }

    vm = Vm();
    CHECK(Run(vm,
              "class K { nvram = null; constructor() { nvram = {}; }\n"
              "  function Show() { print(nvram.v + \" \" + nvram.q + \" \" + nvram.o.GetInstanceName() + \" \" + nvram.list[0] + \" \" + (nvram.list[1] == nvram.o)); } }\n") ==
          JPT_SCRIPT_OK);
    const int k2 = jpt_script_vm_instantiate(vm, "K", "K");
    CHECK(jpt_script_vm_set_nvram(vm, k2, back[0].slots));
    CHECK(jpt_script_vm_invoke(vm, k2, "K", "Show", nullptr, 0) == JPT_SCRIPT_OK);
    char name[16];
    snprintf(name, sizeof name, "%08X", rock);
    CHECK(Has(Print(std::string("Vector3(1, 2, 3) Rotate3(0, 1, 0, 0) ") + name + " Vector3(4, 5, 6) true")));
    jpt_script_vm_destroy(vm);
}

// The player, the object in hand, guns, health; colours.
static void test_player_and_guns() {
    g_world = World{};
    g_world.player = g_world.Add("Anne", "CPlayer");
    const uint32_t gun = g_world.Add("Gun-00", "CGun");
    g_world.objects[gun].ammo = 7;
    g_world.objects[gun].max_ammo = 12;
    g_world.objects[g_world.player].hit_points = 33.0f;
    jpt_script_vm* vm = Vm();
    CHECK(Run(vm,
              "function Show() {\n"
              "  local p = TP_Engine.GetPlayer();\n"
              "  print((p instanceof CPlayer) + \" \" + p.GetHealth() + \" \" + p.bDead() + \" \" + (p.pinsHeldObject() == null));\n"
              "}\n"
              "function Gun() {\n"
              "  local g = TP_Engine.GetPlayer().pinsHeldObject();\n"
              "  print((g instanceof CGun) + \" \" + g.iAmmo + \"/\" + g.iMaxAmmo);\n"
              "  g.iAmmo = 20; print(g.iAmmo); g.iMaxAmmo = 5; print(g.iAmmo + \"/\" + g.iMaxAmmo);\n"
              "}\n"
              "function BadGun() { TP_Engine.GetPlayer().pinsHeldObject().iAmmo = -1; }\n"
              "function Colours() {\n"
              "  print(format(\"%08x %08x %08x %08x %08x\", TP_Engine.rgba(255, 0, 0, 205), TP_Engine.rgba(1, 2, 3), TP_Engine.rgb(300, -5, 16),\n"
              "               TP_Engine.frgba(1.0, 0.5, 0.0, 0.25), TP_Engine.frgb(2.0, 0.0, 1.0)));\n"
              "}\n") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "Show", "Show") == JPT_SCRIPT_OK && Has(Print("true 33 false true")));
    g_world.objects[g_world.player].held = gun;
    g_world.objects[g_world.player].hit_points = 0.0f;
    CHECK(jpt_script_vm_call(vm, "Show2", "Show") == JPT_SCRIPT_OK && Has(Print("true 0 true false")));
    CHECK(jpt_script_vm_call(vm, "Gun", "Gun") == JPT_SCRIPT_OK && Has(Print("true 7/12")) && Has(Print("12")) && Has(Print("5/5")));
    CHECK(g_world.objects[gun].ammo == 5 && g_world.objects[gun].max_ammo == 5);
    CHECK(jpt_script_vm_call(vm, "BadGun", "BadGun") == JPT_SCRIPT_FAILED && Contains("iAmmo: takes an int from 0") && g_world.objects[gun].ammo == 5);
    CHECK(jpt_script_vm_call(vm, "Colours", "Colours") == JPT_SCRIPT_OK);
    CHECK(Has(Print("cdff0000 ff010203 ffff0010 40ff8000 ffff00ff")));
    // No player in the world: null, as the game has none yet.
    g_world.objects.erase(g_world.player);
    CHECK(Run(vm, "print(TP_Engine.GetPlayer() == null);") == JPT_SCRIPT_OK && Has(Print("true")));
    jpt_script_vm_destroy(vm);
}

// The game's text overlay: the formats and types as the game uses them, a pick-up message after the last of every type.
static void test_text() {
    g_world = World{};
    jpt_script_vm* vm = Vm();
    CHECK(Run(vm,
              "print(TEXT_FORMAT_CENTER + \" \" + TEXT_FORMAT_TOP + \" \" + TEXT_FORMAT_BOTTOM + \" \" + TEXT_FORMAT_SOLID + \" \" + ETextType.ettALL + \" \" + ETextType.ettSUBTITLE);\n"
              "local first = TP_Engine.u4DisplayFormattedString(\"Picked up Isla Nubar map\", 4.0, TEXT_FORMAT_TOP | TEXT_FORMAT_CENTER, 0x0000FF, 0, ETextType.ettALL);\n"
              "local prev = TP_Engine.u4FindSequenceEnd(ETextType.ettALL);\n"
              "local second = TP_Engine.u4DisplayFormattedString(\"\", 1.5, 0x7FFFFFFF, 0x12345678, prev, ETextType.ettALL);\n"
              "print((first != 0) + \" \" + (prev == first) + \" \" + (second != 0) + \" \" + TP_Engine.u4FindSequenceEnd(ETextType.ettTUTORIAL));\n"
              "print(TP_Engine.u4DisplayFormattedString(\"stale\", 1.0, 0, 0, 12345, 0));\n") == JPT_SCRIPT_OK);
    CHECK(Has(Print("1 8 16 512 0 2")) && Has(Print("true true true 0")) && Has(Print("0")));
    CHECK(g_world.texts.size() == 2 && g_world.texts[0].text == "Picked up Isla Nubar map" && g_world.texts[0].format == 9 && g_world.texts[0].rgb == 0xFF);
    CHECK(g_world.texts[1].text.empty() && g_world.texts[1].format == JPT_TEXT_FORMATS && g_world.texts[1].rgb == 0x345678 && g_world.texts[1].previous == g_world.texts[0].handle);
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "TP_Engine.u4DisplayFormattedString(\"x\", -1.0, 0, 0);") == JPT_SCRIPT_FAILED && Contains("a time that is not a number of seconds"));
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "local s = \"\"; for (local i = 0; i < 1100; i++) s += \"a\"; TP_Engine.u4DisplayFormattedString(s, 1.0, 0, 0);") == JPT_SCRIPT_FAILED &&
          Contains("bytes or more"));
    jpt_script_vm_set_world(vm, 1, 0);
    jpt_script_vm_end_level(vm);
    CHECK(Run(vm, "TP_Engine.u4FindSequenceEnd(0);") == JPT_SCRIPT_FAILED && Contains("u4FindSequenceEnd: the world is not loaded"));
    jpt_script_vm_destroy(vm);
}

int main() {
    test_registry_in_runtime();
    test_text();
    test_player_and_guns();
    test_vector3();
    test_rotate3();
    test_engine_functions();
    test_entity_handles();
    test_nvram_values();
    printf("script_bindings_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
