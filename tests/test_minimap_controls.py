#!/usr/bin/env python3
"""F5 and L3 + R3 are the minimap's controls and the Debug Menu is gone, headless:
py -3 -m unittest discover -s tests -p "test_minimap_controls.py" """
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "src" / "host"
LAUNCHER = ROOT / "launcher"
TRANSLATIONS = ROOT / "assets" / "translations"

REMOVED = ("minimap_zoom", "minimap_toggle_key", "minimap_toggle_active", "MM_VK_TOGGLE", "MM_ZOOM_DEFAULT",
           "MM_ZOOM_STEP", "debug_menu_f5", "dm_f5", "DM_MENU_DEBUG", "KEY_HOST_DEBUG_MENU", "OV_DEBUG",
           "tutorial_prompts_key_bound")


def read(path):
    return path.read_text(encoding="utf-8")


def sources():
    for root in (HOST, LAUNCHER, ROOT / "src" / "common"):
        for path in sorted(root.rglob("*")):
            if path.suffix in (".c", ".h", ".cpp"):
                yield path, read(path)


class MinimapControls(unittest.TestCase):
    def test_nothing_of_the_old_controls_is_left(self):
        for path, text in sources():
            for name in REMOVED:
                self.assertNotIn(name, text, f"{name} in {path.relative_to(ROOT)}")

    def test_f5_and_the_chord_share_one_toggle(self):
        window = read(HOST / "window_host.c")
        pad = read(HOST / "gamepad.c")
        toggle = window[window.index("void window_host_toggle_minimap(") :]
        toggle = toggle[: toggle.index("\n}\n")]
        self.assertIn("minimap_toggle()", toggle)
        self.assertIn("save_minimap(on)", toggle)
        self.assertIn("minimap_toggle_allowed(", toggle)
        self.assertEqual(window.count("minimap_toggle()"), 1)
        self.assertIn('window_host_toggle_minimap("F5")', window)
        self.assertIn('window_host_toggle_minimap("L3 + R3")', pad)
        self.assertIn("pad_chord_update(&G.chord, MM_PAD_CHORD,", pad)
        save = window[window.index("static void save_minimap(") :]
        save = save[: save.index("\n}\n")]
        self.assertIn("JPT_KEY_MINIMAP", save)
        self.assertIn("jpt_ini_set(s.cfg.settings_path", save)

    def test_the_launcher_writes_the_same_setting(self):
        launcher = read(LAUNCHER / "launcher.cpp")
        self.assertRegex(launcher, r'ui\.Toggle\("Enable Minimap", kMinimapHelp, JPT_KEY_MINIMAP, true, "Off", "On"\);')
        page = launcher[launcher.index("void PageGameplay(") :]
        page = page[: page.index("ui.EndRows();")]
        self.assertIn('"Enable Minimap"', page)
        self.assertEqual(len(re.findall(r"JPT_KEY_MINIMAP", launcher)), 1)
        self.assertEqual(read(ROOT / "src" / "host" / "main.c").count("minimap_configure(cfg.minimap)"), 1)

    def test_the_controls_lists_name_the_new_controls(self):
        text = read(ROOT / "src" / "common" / "jpt_prompt_text.c")
        self.assertIn('{ "<KEY_F5>", "Toggle Minimap" },', text)
        self.assertIn('{ "<PAD_L3> + <PAD_R3>", "Toggle Minimap" },', text)
        self.assertNotIn("Debug Menu", text)
        self.assertNotIn("Debug / Test", text)
        self.assertNotRegex(text, r"[Zz]oom")

    def test_no_text_of_the_debug_menu_is_left(self):
        for path in sorted(TRANSLATIONS.glob("*.interface.txt")):
            text = read(path)
            for gone in ("Debug Menu\t", "Debug / Test Menu", "digits edit | Esc/B back | F5 close", "Show Coordinates"):
                self.assertFalse(gone in text, f"{gone!r} in {path.name}")
            for kept in ("Toggle Minimap\t", "Minimap On\t", "Minimap Off\t"):
                self.assertTrue(kept in text, f"{kept!r} missing from {path.name}")
        for path, text in sources():
            self.assertNotIn("Debug / Test", text, str(path.relative_to(ROOT)))
            self.assertNotRegex(text, r"F5'?s? Debug", str(path.relative_to(ROOT)))

    def test_the_minimap_uses_the_fixed_zoom(self):
        header = read(HOST / "minimap.h")
        self.assertRegex(header, r"#define MM_ZOOM 2\.0f")
        panel = read(HOST / "debug_menu_panel.cpp")
        self.assertIn("MM_ZOOM", panel)
        self.assertIn("minimap_creature_range(MM_ZOOM)", read(HOST / "debug_menu.c"))


if __name__ == "__main__":
    unittest.main()
