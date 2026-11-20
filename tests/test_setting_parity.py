"""The launcher and the runtime agree on every setting (headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_setting_parity.py"

jpt_settings[] and jpt_config_apply() (src/common/jpt_config.c) are the runtime's side; the `ui.Toggle/Combo/Choice/
SliderInt` rows of launcher/launcher.cpp are the launcher's. A setting with no parser, no launcher row, no reader in the
host, or a default, range or list that differs between the two fails here instead of in a player's launcher.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402

KEYS = gs.setting_keys()
CONSTANTS = gs.key_constants()
FIELDS = gs.apply_fields()
LAUNCHER = gs.strip_comments("\n".join(gs.read(p) for p in sorted(gs.LAUNCHER.glob("*.cpp"))))
CONFIG_C = gs.strip_comments(gs.read(gs.COMMON / "jpt_config.c"))
CONFIG_H = gs.read(gs.COMMON / "jpt_config.h")
HOST_TEXT = "\n".join(gs.strip_comments(gs.read(p)) for p in gs.host_sources() if p.name != "debug_menu_panel.cpp")
FAMILIES = (("gamepad_", "jpt_pad_actions", "the gamepad table's own rows"), ("cheat_", "jpt_cheats", "the cheats' table rows"))

LAUNCHER_ONLY = {"show_launcher": "decides whether the launcher opens; the host never reads it",
                 "game_version": "the launcher's Play page picks it; the host resolves the installed version itself"}


def ui_rows():
    """The launcher's rows over a setting constant: (call, key, default text or None, option keys)."""
    out = []
    for match in re.finditer(r"\bui\.(Toggle|Combo|Choice|SliderInt)\(", LAUNCHER):
        args = gs.split_top(gs.balanced(LAUNCHER, match.end() - 1))
        if len(args) < 4 or not re.fullmatch(r"JPT_KEY_\w+", args[2]):
            continue
        default = args[3]
        if default in ("true", "false"):
            default = "1" if default == "true" else "0"
        elif default.startswith('"'):
            default = gs.string_value(default)
        options = re.findall(r'\{\s*"([^"]+)"\s*,\s*"[^"]*"\s*\}', ",".join(args[4:])) if match.group(1) in ("Combo", "Choice") else []
        out.append({"call": match.group(1), "key": CONSTANTS[args[2]], "default": default, "options": options})
    return out


ROWS = ui_rows()


def referenced_keys():
    names = set()
    for const, value in CONSTANTS.items():
        if re.search(rf"\b{const}\b", LAUNCHER):
            names.add(value)
    for prefix, table, _ in FAMILIES:
        if table in LAUNCHER:
            names.update(k for k in KEYS if k.startswith(prefix))
    if "jpt_hotkeys" in LAUNCHER:
        names.update(re.findall(rf'"([a-z_]+)"', ",".join(m for m in re.findall(r"\{\s*(JPT_KEY_QUICK_\w+)", CONFIG_C))) or [])
        names.update(CONSTANTS[c] for c in re.findall(r"\{\s*(JPT_KEY_QUICK_\w+)", CONFIG_C))
    if "LanguageKey" in LAUNCHER:
        names.add(CONSTANTS["JPT_KEY_LANGUAGE"])
    return names


def runtime_covered():
    names = set(FIELDS)
    names.update(CONSTANTS[c] for c in re.findall(r"\{\s*(JPT_KEY_QUICK_\w+)", CONFIG_C))
    names.update(k for k in KEYS if k.startswith("gamepad_") or k.startswith("cheat_"))
    return names


class SettingParity(unittest.TestCase):
    def test_every_setting_has_a_parser(self):
        missing = sorted(set(KEYS) - runtime_covered())
        self.assertEqual(missing, [], "settings in jpt_settings[] that jpt_config_apply() cannot read")

    def test_every_setting_has_a_launcher_row_or_a_reason(self):
        shown = referenced_keys()
        missing = sorted(k for k in KEYS if k not in shown and k not in LAUNCHER_ONLY)
        self.assertEqual(missing, [], "settings the launcher never shows: give them a row")

    def test_every_launcher_row_is_a_runtime_setting(self):
        for row in ROWS:
            self.assertIn(row["key"], KEYS, f"launcher row for {row['key']} has no setting in jpt_settings[]")

    def test_the_host_reads_every_setting_it_is_given(self):
        for key, field in FIELDS.items():
            if key in LAUNCHER_ONLY:
                continue
            with self.subTest(setting=key):
                self.assertRegex(HOST_TEXT, rf"(?:->|\.){field}\b", f"nothing in src/host reads jpt_config_t.{field}")

    def test_launcher_defaults_equal_runtime_defaults(self):
        checked = 0
        for row in ROWS:
            if row["default"] is None or row["default"].startswith("JPT_") or row["key"].startswith("language"):
                continue
            checked += 1
            with self.subTest(setting=row["key"]):
                self.assertEqual(row["default"], KEYS[row["key"]], "launcher default differs from jpt_settings[]")
        self.assertGreater(checked, 20)

    def test_choice_lists_equal_the_runtime_lists(self):
        def strings(body):
            return [gs.string_value(m) for m in re.findall(gs.STRING, body)]

        ordered = {"texture_filter": strings(gs.array_body(CONFIG_C, "jpt_filter_keys")),
                   "active_ai": strings(gs.array_body(CONFIG_C, "jpt_active_ai_keys")),
                   "ambient_occlusion": strings(gs.array_body(CONFIG_C, "jpt_ao_keys"))}
        local = {m.group(1): strings(gs.balanced(CONFIG_C, m.end() - 1))
                 for m in re.finditer(r"static const char\* const (modes|presentations|renderers)\[\] = \{", CONFIG_C)}
        unordered = {"display_mode": local["modes"], "presentation": local["presentations"], "renderer": local["renderers"]}
        by_key = {row["key"]: row for row in ROWS}
        for key, runtime in ordered.items():
            with self.subTest(setting=key):
                self.assertEqual(by_key[key]["options"], runtime)
        for key, runtime in unordered.items():
            with self.subTest(setting=key):
                self.assertEqual(sorted(by_key[key]["options"]), sorted(runtime))
        names = strings(gs.array_body(CONFIG_C, "jpt_ao_names"))
        self.assertEqual(len(names), len(ordered["ambient_occlusion"]))
        for key, label in zip(ordered["ambient_occlusion"], names):
            self.assertIn(f'{{"{key}", "{label}"}}', LAUNCHER, "ambient occlusion label differs")

    def test_launcher_defaults_are_values_the_runtime_accepts(self):
        for row in ROWS:
            if row["options"] and row["default"] is not None and not row["default"].startswith("JPT_"):
                with self.subTest(setting=row["key"]):
                    self.assertIn(row["default"], row["options"] if row["key"] != "msaa" else row["options"] + [row["default"]])

    def test_ranges_come_from_one_definition(self):
        for name in ("JPT_FOV_MIN", "JPT_FOV_MAX", "JPT_FOV_DEFAULT"):
            self.assertRegex(LAUNCHER, rf"\b{name}\b", f"the launcher does not use {name}")
        for name in ("JPT_MOUSE_SENSITIVITY_MIN", "JPT_MOUSE_SENSITIVITY_MAX", "JPT_MOUSE_SENSITIVITY_DEFAULT"):
            self.assertRegex(LAUNCHER, rf"\b{name}\b", f"the launcher does not use {name}")
        fov = [int(gs.define(CONFIG_H, f"JPT_FOV_{n}")) for n in ("MIN", "DEFAULT", "MAX")]
        self.assertEqual(fov, [60, 75, 110])
        self.assertEqual(int(KEYS["fov"]), 75)
        low, high, step, default = (float(gs.define(CONFIG_H, f"JPT_MOUSE_SENSITIVITY_{n}").rstrip("f")) for n in ("MIN", "MAX", "STEP", "DEFAULT"))
        self.assertEqual((low, high, step, default), (0.25, 4.0, 0.05, 1.0))
        self.assertEqual(float(KEYS["mouse_sensitivity"]), default)
        self.assertEqual(round((high - low) / step), 75, "the step must land on both ends of the range")
        self.assertNotRegex(LAUNCHER, r"fov[^\n]{0,80}\b(60|110)\b", "the launcher repeats the FOV range as numbers")

    def test_gamepad_sliders_use_the_runtime_range_and_default(self):
        header = gs.read(gs.COMMON / "jpt_gamepad.h")
        sliders = {row["key"]: row["default"] for row in ROWS if row["call"] == "SliderInt"}
        self.assertEqual(len(sliders), 4)
        range_of = {"gamepad_look_sensitivity_x": "LOOK", "gamepad_look_sensitivity_y": "LOOK",
                    "gamepad_stick_deadzone": "STICK_DEADZONE", "gamepad_trigger_deadzone": "TRIGGER_DEADZONE"}
        for key, default in sliders.items():
            self.assertEqual(default, KEYS[key], key)
            low, high = (int(gs.define(header, f"JPT_PAD_{range_of[key]}_{end}")) for end in ("MIN", "MAX"))
            self.assertTrue(low <= int(default) <= high, f"{key}: default {default} outside {low}-{high}")
        for name in ("JPT_PAD_LOOK_MIN", "JPT_PAD_LOOK_MAX", "JPT_PAD_STICK_DEADZONE_MIN", "JPT_PAD_STICK_DEADZONE_MAX",
                     "JPT_PAD_TRIGGER_DEADZONE_MIN", "JPT_PAD_TRIGGER_DEADZONE_MAX"):
            self.assertIsNotNone(gs.define(header, name), name)
            self.assertRegex(LAUNCHER, rf"\b{name}\b")

    def test_there_is_no_in_game_mouse_sensitivity_row(self):
        for name in ("debug_menu_policy.c", "debug_menu_panel.cpp", "overlay.c", "overlay_policy.c", "debug_menu.c"):
            text = gs.read(gs.HOST / name).lower()
            self.assertNotIn("mouse sensitivity", text, name)
            self.assertNotIn("mouse_sens", text, name)


def tearDownModule():
    gs.report("setting_parity", {"settings": len(KEYS), "launcher_rows": len(ROWS), "shown_in_launcher": len(referenced_keys()),
                                 "launcher_only": sorted(LAUNCHER_ONLY)})


if __name__ == "__main__":
    unittest.main()
