// Game scripts' input and modal mode (headless): OnHandleInput with bKeyHit, bKeyPressed and the movement input, uCMD_USER1 to
// uCMD_USER8 bound only for a level whose scripts name them, modal mode entered, fed keys, aborted by Escape or the player's death,
// released when its script stops, at Restart and at a transition; budgets inside the handlers; the campaign gets none of it.
#include "jpt_level_scripts.h"
#include "script_api_fixture.h"

static const char* kReader =
    "class Reader extends GameScript {\n"
    "  function OnHandleInput() {\n"
    "    print(\"input hit=\" + bKeyHit(uCMD_USER1) + \" held=\" + bKeyPressed(uCMD_USER1) + \" move=\" + fGetInputMoveAmountY());\n"
    "  }\n"
    "}\n";

static const char* kDialog =
    "gLook <- true;\n"
    "class Dialog extends GameScript {\n"
    "  tex = null;\n"
    "  function OnScriptInit(b) { tex = TP_Engine.GetTexture(\"textures/white.tga\"); }\n"
    "  function OnHandleInput() { if (bKeyHit(uCMD_USER2)) print(\"open \" + bSetModalMode(true)); }\n"
    "  function OnHandleModalInput() { if (bKeyHit(uCMD_USER2)) { bSetModalMode(false); print(\"closed\"); } return gLook; }\n"
    "  function OnModalKey(vk, pressed, shift, ctrl, alt) { print(\"key \" + Name(vk) + \" \" + pressed + \" \" + shift + \" \" + ctrl + \" \" + alt); }\n"
    "  function OnModalAborted() { print(\"aborted\"); }\n"
    "  function OnPaintHW3D() { if (bIsModalMode()) TP_Engine.DrawTexture(tex, 0, 0, 8, 4, 0, 0, 8, 4, 0xFFFFFFFF, 1); }\n"
    "  function Name(vk) {\n"
    "    if (vk == VK_LEFT) return \"left\"; if (vk == VK_RIGHT) return \"right\"; if (vk == VK_HOME) return \"home\";\n"
    "    if (vk == VK_END) return \"end\"; if (vk == VK_PRIOR) return \"prior\"; if (vk == VK_NEXT) return \"next\";\n"
    "    return vk.tostring();\n"
    "  }\n"
    "}\n";

static void test_user_commands() {
    jpt_script_session* s = ApiSession({{"scripts/keys.nut",
                                         "class Keys extends GameScript { function OnHandleInput() { bKeyHit(uCMD_USER1); bKeyPressed(uCMD_USER3); } }\n"}});
    Ready(s, "keys.scn");
    // Only the commands the level's code names reach it.
    CHECK(jpt_script_session_user_commands(s) == 0x5u);
    Close(s);
    s = ApiSession({{"scripts/all.nut",
                     "class Keys extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER8)) print(\"user8 hit\"); if (bKeyPressed(uCMD_USER8)) print(\"user8 held\"); } }\n"}});
    Ready(s, "k.scn");
    CHECK(jpt_script_session_user_commands(s) == 0x80u);
    Input(s, 0xFFu, 0xFFu);
    CHECK(Has(Print("user8 hit")) && Has(Print("user8 held")));
    Close(s);
    s = ApiSession({{"scripts/plain.nut", "class P extends GameScript { function OnSimStep(d) {} }\n"}});
    Ready(s, "p.scn");
    CHECK(jpt_script_session_user_commands(s) == 0u);
    Close(s);
    s = ApiSession({{"scripts/none.nut", "class Q extends LevelScript { static SCN = \"other.scn\"; function OnLevelStart() {} }\n"}});
    Ready(s, "p.scn");
    CHECK(!jpt_script_session_has_vm(s) && jpt_script_session_user_commands(s) == 0u);
    Close(s);
    // The campaign has no script session: no USER command, no frame, no modal key.
    jpt_level_scripts_end();
    CHECK(jpt_level_scripts_user_commands() == 0u && !jpt_level_scripts_frame() && !jpt_level_scripts_key(0x27u, 1, 0, 0, 0));
    jpt_script_input in{0xFFu, 0xFFu, 0.0f, 1.0f};
    jpt_script_input_result out{1, 1};
    jpt_level_scripts_input(&in, &out);
    CHECK(!out.modal && !out.look);
}

static void test_input() {
    jpt_script_session* s = ApiSession({{"scripts/reader.nut", kReader}});
    Ready(s, "input.scn");
    // Hit is the frame the key went down, held while it stays down; the movement input as the game read it.
    CHECK(!Input(s, 0x1u, 0x1u, 1.0f).modal && Has(Print("input hit=true held=true move=1")));
    Input(s, 0x1u, 0u, 0.5f);
    CHECK(Has(Print("input hit=false held=true move=0.5")));
    // Another command's bits are not USER1.
    Input(s, 0x40u, 0x40u, 0.0f);
    CHECK(Has(Print("input hit=false held=false move=0")));
    Close(s);
    // The key state is read in the input handlers only.
    s = ApiSession({{"scripts/out.nut", "class Late extends GameScript { function OnSimStep(d) { bKeyHit(uCMD_USER1); } }\n"}});
    Ready(s, "o.scn");
    Steps(s, 1);
    CHECK(Contains("bKeyHit: only in OnHandleInput or OnHandleModalInput"));
    Close(s);
    // An endless OnHandleInput: that script stops, the next frame goes on.
    s = ApiSession({{"scripts/loop.nut", "class Loop extends GameScript { function OnHandleInput() { while (true) {} } }\n"}}, 20000);
    Ready(s, "l.scn");
    Input(s, 0u, 0u);
    CHECK(Contains("callback=OnHandleInput") && Contains("instruction budget exceeded (20000)"));
    Input(s, 0u, 0u);
    CHECK(Count("[script] level=l.scn script=Loop") == 1);
    Close(s);
}

static int Drawn(jpt_script_session* s) {
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    return f ? f->count : 0;
}

static void test_modal() {
    jpt_script_session* s = ApiSession({{"scripts/dialog.nut", kDialog}});
    Ready(s, "modal.scn");
    CHECK(Drawn(s) == 0);
    // Entered from OnHandleInput: the player's controls stop; the mouse still looks while OnHandleModalInput returns true.
    jpt_script_input_result r = Input(s, 0u, 0x2u);
    CHECK(Has(Print("open true")) && r.modal && jpt_script_level_modal(s));
    r = Input(s, 0u, 0u);
    CHECK(r.modal && r.look);
    CHECK(Drawn(s) == 1);
    // Keys go to OnModalKey with their state and modifiers; F keys stay the port's.
    CHECK(jpt_script_level_key(s, 0x25u, 1, 0, 0, 0) == 1 && Has(Print("key left true false false false")));
    CHECK(jpt_script_level_key(s, 0x27u, 0, 1, 0, 0) == 1 && Has(Print("key right false true false false")));
    CHECK(jpt_script_level_key(s, 0x24u, 1, 0, 1, 0) == 1 && Has(Print("key home true false true false")));
    CHECK(jpt_script_level_key(s, 0x23u, 1, 0, 0, 1) == 1 && Has(Print("key end true false false true")));
    CHECK(jpt_script_level_key(s, 0x21u, 1, 0, 0, 0) == 1 && Has(Print("key prior true false false false")));
    CHECK(jpt_script_level_key(s, 0x22u, 1, 0, 0, 0) == 1 && Has(Print("key next true false false false")));
    CHECK(jpt_script_level_key(s, 0x41u, 1, 0, 0, 0) == 1 && Has(Print("key 65 true false false false")));
    CHECK(jpt_script_level_key(s, 0x74u, 1, 0, 0, 0) == 0);
    // OnHandleModalInput ends it; then keys are the game's again and nothing is drawn.
    r = Input(s, 0u, 0x2u);
    CHECK(Has(Print("closed")) && !r.modal && !jpt_script_level_modal(s));
    CHECK(jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 0 && Drawn(s) == 0);
    // Escape aborts it, taken from the game.
    Input(s, 0u, 0x2u);
    CHECK(jpt_script_level_key(s, 0x1Bu, 1, 0, 0, 0) == 1 && Has(Print("aborted")) && !jpt_script_level_modal(s));
    // The player's death aborts it.
    g_lines.clear();
    Input(s, 0u, 0x2u);
    g_world.objects[g_world.player].hit_points = 0.0f;
    r = Input(s, 0u, 0u);
    CHECK(Has(Print("aborted")) && !r.modal);
    g_world.objects[g_world.player].hit_points = 100.0f;
    // Restart and a transition end it with the VM.
    Input(s, 0u, 0x2u);
    CHECK(jpt_script_level_modal(s));
    Begin(s, "modal.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(!jpt_script_level_modal(s) && jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 0);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_modal(s) && !Input(s, 0u, 0u).modal);
    Input(s, 0u, 0x2u);
    Begin(s, "next.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_modal(s));
    Close(s);
    // OnHandleModalInput returning false: no mouse look either.
    s = ApiSession({{"scripts/dialog.nut", std::string(kDialog) + "gLook = false;\n"}});
    Ready(s, "modal.scn");
    Input(s, 0u, 0x2u);
    r = Input(s, 0u, 0u);
    CHECK(r.modal && !r.look);
    Close(s);
}

static void test_modal_owner_dies() {
    const char* two =
        "class First extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER1)) print(\"first \" + bSetModalMode(true)); }\n"
        "  function OnModalKey(vk, p, s, c, a) { if (vk == VK_LEFT) throw \"broken key\"; if (vk == VK_RIGHT) while (true) {} } }\n"
        "class Second extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER1)) print(\"second \" + bSetModalMode(true) + \" \" + bIsModalMode()); } }\n";
    jpt_script_session* s = ApiSession({{"scripts/two.nut", two}}, 20000);
    Ready(s, "t.scn");
    // One modal script at a time.
    Input(s, 0u, 0x1u);
    CHECK(Has(Print("first true")) && Has(Print("second false false")));
    // A script error in its key handler stops it and frees the input.
    CHECK(jpt_script_level_key(s, 0x25u, 1, 0, 0, 0) == 1 && Contains("broken key"));
    CHECK(!jpt_script_level_modal(s) && Contains("modal mode released: its script stopped"));
    CHECK(jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 0 && !Input(s, 0u, 0u).modal);
    Close(s);
    // Out of its budget inside the key handler: the same.
    s = ApiSession({{"scripts/two.nut", two}}, 20000);
    Ready(s, "t.scn");
    Input(s, 0u, 0x1u);
    CHECK(jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 1 && Contains("instruction budget exceeded (20000)") && !jpt_script_level_modal(s));
    Close(s);
}

int main() {
    test_user_commands();
    test_input();
    test_modal();
    test_modal_owner_dies();
    printf("script_input_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
