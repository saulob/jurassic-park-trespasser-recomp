// Game scripts' input and modal mode (headless): OnHandleInput with bKeyHit, bKeyPressed and the movement input, uCMD_USER1 to
// uCMD_USER8 bound only for a level whose scripts name them, modal mode entered, fed keys, aborted by Escape or the player's death,
// released when its script stops, at Restart and at a transition; budgets inside the handlers; the campaign gets none of it.
#include "jpt_level_scripts.h"
#include "script_hud_fixture.h"

static int Additive(const jpt_draw_frame_t* f) {
    int n = 0;
    for (int i = 0; f && i < f->count; i++) n += f->cmds[i].blend == JPT_DRAW_ADD;
    return n;
}

static void test_user_commands() {
    jpt_script_session* s = HudSession({{"scripts/hud.nut", kHudScript}});
    Ready(s, "ti.scn");
    // The level's code names USER1 and USER2: only those reach it.
    CHECK(jpt_script_session_user_commands(s) == 3u);
    Close(s);
    s = HudSession({{"scripts/all.nut",
                     "class Keys extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER8)) print(\"user8 hit\"); if (bKeyPressed(uCMD_USER8)) print(\"user8 held\"); } }\n"}});
    Ready(s, "k.scn");
    CHECK(jpt_script_session_user_commands(s) == 0x80u);
    Input(s, 0xFFu, 0xFFu);
    CHECK(Has(Print("user8 hit")) && Has(Print("user8 held")));
    Close(s);
    s = HudSession({{"scripts/plain.nut", "class P extends GameScript { function OnSimStep(d) {} }\n"}});
    Ready(s, "p.scn");
    CHECK(jpt_script_session_user_commands(s) == 0u);
    Close(s);
    s = HudSession({{"scripts/none.nut", "class Q extends LevelScript { static SCN = \"other.scn\"; function OnLevelStart() {} }\n"}});
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
    jpt_script_session* s = HudSession({{"scripts/hud.nut", kHudScript}});
    Ready(s, "ti.scn");
    // Sprint held and moving forward: the HUD's additive icon; not moving, or the key up: none.
    CHECK(!Input(s, 0x1u, 0x1u, 1.0f).modal);
    CHECK(Additive(jpt_script_level_paint(s)) == 1);
    Input(s, 0x1u, 0u, 0.0f);
    CHECK(Additive(jpt_script_level_paint(s)) == 0);
    Input(s, 0x40u, 0u, 1.0f);
    CHECK(Additive(jpt_script_level_paint(s)) == 0);
    Close(s);
    // The key state is read in the input handlers only.
    s = HudSession({{"scripts/out.nut", "class Late extends GameScript { function OnSimStep(d) { bKeyHit(uCMD_USER1); } }\n"}});
    Ready(s, "o.scn");
    Steps(s, 1);
    CHECK(Contains("bKeyHit: only in OnHandleInput or OnHandleModalInput"));
    Close(s);
    // An endless OnHandleInput: that script stops, the next frame goes on.
    s = HudSession({{"scripts/loop.nut", "class Loop extends GameScript { function OnHandleInput() { while (true) {} } }\n"}}, 20000);
    Ready(s, "l.scn");
    Input(s, 0u, 0u);
    CHECK(Contains("callback=OnHandleInput") && Contains("instruction budget exceeded (20000)"));
    Input(s, 0u, 0u);
    CHECK(Count("[script] level=l.scn script=Loop") == 1);
    Close(s);
}

static void test_modal() {
    jpt_script_session* s = HudSession({{"scripts/hud.nut", kHudScript}});
    Ready(s, "ti.scn");
    // USER2 opens the map in modal mode: the player's controls stop, the mouse still looks once the modal handler says so.
    jpt_script_input_result r = Input(s, 0u, 0x2u);
    CHECK(Has(Print("map true")) && r.modal && jpt_script_level_modal(s));
    r = Input(s, 0u, 0u);
    CHECK(r.modal && r.look);
    // Keys go to the script; F keys stay the port's.
    CHECK(jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 1 && Has(Print("key 39 true page 1")));
    CHECK(jpt_script_level_key(s, 0x22u, 1, 0, 0, 0) == 1 && Has(Print("key 34 true page 2")));
    CHECK(jpt_script_level_key(s, 0x25u, 0, 0, 0, 0) == 1 && Has(Print("key 37 false page 2")));
    CHECK(jpt_script_level_key(s, 0x23u, 1, 1, 0, 0) == 1 && Has(Print("key 35 true page 9")));
    CHECK(jpt_script_level_key(s, 0x24u, 1, 0, 1, 1) == 1 && Has(Print("key 36 true page 0")));
    CHECK(jpt_script_level_key(s, 0x74u, 1, 0, 0, 0) == 0);
    // The map is drawn centred while modal.
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    int map = 0;
    for (int i = 0; f && i < f->count; i++) map += f->cmds[i].w == 256.0f && f->cmds[i].x == 384.0f && f->cmds[i].y == 320.0f;
    CHECK(map == 1);
    // USER2 again inside modal mode closes it.
    r = Input(s, 0u, 0x2u);
    CHECK(Has(Print("map closed")) && !r.modal && !jpt_script_level_modal(s));
    CHECK(jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 0);
    // Escape aborts it, taken from the game.
    Input(s, 0u, 0x2u);
    CHECK(jpt_script_level_key(s, 0x1Bu, 1, 0, 0, 0) == 1 && Has(Print("aborted")) && !jpt_script_level_modal(s));
    // The player's death aborts it.
    g_lines.clear();
    Input(s, 0u, 0x2u);
    g_hud_world.objects[g_hud_world.player].hit_points = 0.0f;
    r = Input(s, 0u, 0u);
    CHECK(Has(Print("aborted")) && !r.modal);
    g_hud_world.objects[g_hud_world.player].hit_points = 50.0f;
    // Restart and a transition end it with the VM.
    Input(s, 0u, 0x2u);
    CHECK(jpt_script_level_modal(s));
    Begin(s, "ti.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(!jpt_script_level_modal(s) && jpt_script_level_key(s, 0x27u, 1, 0, 0, 0) == 0);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_modal(s) && !Input(s, 0u, 0u).modal);
    Input(s, 0u, 0x2u);
    Begin(s, "next.scn", JPT_SCRIPT_BEGIN_TRANSITION);
    jpt_script_level_ready(s, 1);
    CHECK(!jpt_script_level_modal(s));
    Close(s);
}

static void test_modal_owner_dies() {
    const char* two =
        "class First extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER1)) print(\"first \" + bSetModalMode(true)); }\n"
        "  function OnModalKey(vk, p, s, c, a) { if (vk == VK_LEFT) throw \"broken key\"; if (vk == VK_RIGHT) while (true) {} } }\n"
        "class Second extends GameScript { function OnHandleInput() { if (bKeyHit(uCMD_USER1)) print(\"second \" + bSetModalMode(true) + \" \" + bIsModalMode()); } }\n";
    jpt_script_session* s = HudSession({{"scripts/two.nut", two}}, 20000);
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
    s = HudSession({{"scripts/two.nut", two}}, 20000);
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
