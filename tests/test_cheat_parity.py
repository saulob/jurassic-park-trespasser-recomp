"""Every cheat of jpt_cheats[] is wired, tested and translated, in every build that runs it (headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_cheat_parity.py"

The cheats are read from src/common/jpt_cheats.c, never listed here. For each: its table row (kind, default, key,
persistence), its F10 row and action, its host handler, the test that names it, its translation in every language and
the guest addresses the handlers read in every build.
"""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_profiles  # noqa: E402
import gate_support as gs  # noqa: E402

CHEATS = gs.cheats()
LANGUAGES = gs.languages()
INTERFACE = {}
for language in LANGUAGES:
    path = gs.ROOT / "assets" / "translations" / f"{language['code']}.interface.txt"
    INTERFACE[language["code"]] = {} if not path.exists() else dict(
        line.split("\t", 1) for line in gs.read(path).splitlines() if "\t" in line and not line.startswith("#"))

POLICY = gs.strip_comments(gs.read(gs.HOST / "debug_menu_policy.c"))
HANDLERS = {name: gs.strip_comments(gs.read(gs.HOST / name)) for name in ("cheats.c", "cheats_policy.c", "debug_menu.c")}
TEST_SOURCE = gs.read(gs.TESTS / "cheats_tests.c")
ITEM = dict(re.findall(r"\[(JPT_CHEAT_\w+)\]\s*=\s*(DM_\w+)", POLICY))
ACTION_OF_ITEM = {item: act for act, item in re.findall(r"case (DM_ACT_\w+):\s*return (DM_\w+);", HANDLERS["debug_menu.c"])}
_enum = re.search(r"typedef enum \{(.*?)\} dm_action_kind_t", gs.read(gs.HOST / "debug_menu_policy.h"), re.S).group(1)
_unmapped = [a for a in re.findall(r"DM_ACT_\w+", _enum) if a != "DM_ACT_NONE" and a not in ACTION_OF_ITEM.values()]
_default = re.search(r"default:\s*return (DM_\w+);", HANDLERS["debug_menu.c"])
if len(_unmapped) == 1 and _default:
    ACTION_OF_ITEM[_default.group(1)] = _unmapped[0]
SAVED = [c for c in CHEATS if c["key"] and c["kind"] != "JPT_CHEAT_ACTION"]
RESULT = {"cheats": []}


def cheat_name(c):
    return c["id"].replace("JPT_CHEAT_", "")


class CheatParity(unittest.TestCase):
    def test_a_cheat_is_found(self):
        self.assertGreaterEqual(len(CHEATS), 12)
        self.assertEqual(len({c["id"] for c in CHEATS}), len(CHEATS))
        self.assertEqual(len({c["label"] for c in CHEATS}), len(CHEATS))

    def test_table_rows_are_coherent(self):
        persistent = gs.define(gs.read(gs.COMMON / "jpt_cheats.h"), "JPT_CHEAT_PERSISTENT_COUNT")
        self.assertEqual(int(persistent), len(SAVED), "JPT_CHEAT_PERSISTENT_COUNT")
        for c in CHEATS:
            with self.subTest(cheat=cheat_name(c)):
                if c["kind"] == "JPT_CHEAT_ACTION":
                    self.assertIsNone(c["key"], "an action is never saved")
                    self.assertEqual(c["default"], "0")
                    self.assertEqual(c["choices"], "NULL")
                if c["kind"] == "JPT_CHEAT_TOGGLE":
                    self.assertIn(c["default"], ("0", "1"))
                if c["kind"] == "JPT_CHEAT_CHOICE":
                    self.assertEqual(c["default"], "JPT_CHEAT_MULTIPLIER_DEFAULT", "a multiplier starts at 1.0x")
                    self.assertNotEqual(c["choices"], "NULL")
                if c["key"]:
                    self.assertRegex(c["key"], r"^cheat_[a-z_]+$")
                    self.assertIn(c["key"], gs.setting_keys(), "a saved cheat is a setting of the launcher")

    def test_saved_cheats_default_like_the_setting_and_session_cheats_are_not_settings(self):
        keys = gs.setting_keys()
        for c in CHEATS:
            if c["key"]:
                expected = "1" if c["default"] == "JPT_CHEAT_MULTIPLIER_DEFAULT" else c["default"]
                if c["kind"] == "JPT_CHEAT_CHOICE":
                    self.assertEqual(keys[c["key"]], "1", c["label"])
                else:
                    self.assertEqual(keys[c["key"]], expected, c["label"])
            else:
                self.assertEqual(c["kind"] == "JPT_CHEAT_ACTION" or c["id"] in ("JPT_CHEAT_NOCLIP", "JPT_CHEAT_STEALTH", "JPT_CHEAT_FREE_CAMERA"), True,
                                 f"{c['label']} is session-only but not marked as one")

    def test_every_cheat_has_an_f10_row_and_an_action(self):
        for c in CHEATS:
            with self.subTest(cheat=cheat_name(c)):
                self.assertIn(c["id"], ITEM, "no row in F10's table")
                item = ITEM[c["id"]]
                self.assertIn(item.removesuffix("_PAGE"), ACTION_OF_ITEM, "its row has no action")

    def test_every_cheat_has_a_handler(self):
        for c in CHEATS:
            with self.subTest(cheat=cheat_name(c)):
                item = ITEM.get(c["id"], "").removesuffix("_PAGE")
                action = ACTION_OF_ITEM.get(item)
                in_handlers = any(c["id"] in text for text in HANDLERS.values())
                in_menu = action is not None and action in HANDLERS["debug_menu.c"]
                self.assertTrue(in_handlers or in_menu, f"{c['label']}: neither cheats.c nor an F10 action handles it")

    def test_every_cheat_is_tested_by_name(self):
        for c in CHEATS:
            with self.subTest(cheat=cheat_name(c)):
                self.assertIn(c["id"], TEST_SOURCE, "tests/cheats_tests.c never names it")
                self.assertIn(c["label"], TEST_SOURCE, "tests/cheats_tests.c never names its label")

    def test_every_cheat_label_is_translated_in_every_language(self):
        for language in LANGUAGES:
            if language["code"] == "en":
                continue
            table = INTERFACE[language["code"]]
            for c in CHEATS:
                with self.subTest(language=language["code"], cheat=cheat_name(c)):
                    self.assertTrue(c["label"] in table, f"{language['code']}.interface.txt has no translation of {c['label']!r}")
                    self.assertTrue(table[c["label"]].strip())

    def test_the_handlers_read_only_addresses_every_build_proves(self):
        addresses = sorted({int(a, 16) for text in (gs.read(gs.HOST / "cheats.c"), gs.read(gs.HOST / "cheats_policy.c"))
                            for a in re.findall(r"GVA\((0x[0-9A-Fa-f]{8})\)", text)})
        self.assertTrue(addresses)
        for path in sorted((gs.CONFIG / "guest_maps").glob("*.json")):
            table = {int(k, 16): v for k, v in json.loads(path.read_text(encoding="utf-8"))["addresses"].items()}
            for a in addresses:
                with self.subTest(build=path.stem, address=f"0x{a:08X}"):
                    self.assertIn(a, table)
                    self.assertIn(table[a]["class"], check_profiles.PROVEN)


def tearDownModule():
    builds = [v["id"] for v in gs.game_versions()]
    gs.report("cheat_parity", {"discovered": len(CHEATS), "tested": sum(1 for c in CHEATS if c["id"] in TEST_SOURCE),
                               "saved": len(SAVED), "builds": builds, "cheats": [c["label"] for c in CHEATS]})


if __name__ == "__main__":
    unittest.main()
