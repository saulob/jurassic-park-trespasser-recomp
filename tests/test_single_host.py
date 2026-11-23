#!/usr/bin/env python3
"""The port is one executable, trespasser_recomp.exe, that carries every lift (headless, no build):

    py -3 -m unittest tests.test_single_host
"""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import lifts  # noqa: E402

CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD = (ROOT / "tools" / "build.ps1").read_text(encoding="utf-8")
OLD_NAMES = ("trespasser_es117", "trespasser_ptbr117", "jpt_add_lift_host", "JPT_EXPECTED_LIFT", "JPT_FORWARDED_HOST",
             "forward_to_host", "jpt_host_dispatch", "AllHosts")


def sources():
    for folder in ("src", "launcher", "tests", "tools"):
        for path in sorted((ROOT / folder).rglob("*")):
            parts = path.relative_to(ROOT).parts
            if (path.suffix in (".c", ".h", ".cpp", ".py", ".ps1", ".cmake") and parts[:2] != ("src", "recomp")
                    and "__pycache__" not in parts and path.name != Path(__file__).name):
                yield path


class SingleHost(unittest.TestCase):
    def test_the_only_default_host_executable_is_trespasser_recomp(self):
        x86 = CMAKE[:CMAKE.index("if(CMAKE_SIZEOF_VOID_P EQUAL 8)")] + CMAKE[CMAKE.index("  return()\nendif()"):]
        defaults = [m.group(1) for m in re.finditer(r"^add_executable\(([A-Za-z0-9_-]+)\b(?![^)]*EXCLUDE_FROM_ALL)", x86, re.M)]
        self.assertEqual(defaults, ["trespasser_recomp", "trg-updater"])
        self.assertIn("add_dependencies(trespasser_recomp trg-updater)", x86)
        updater_block = re.search(r"^add_executable\(trg-updater\b([^)]*)\)", x86, re.M)
        self.assertIsNotNone(updater_block)
        for token in ("HOST_SOURCES", "PCRECOMP_SOURCES", "GEN_SOURCES"):
            self.assertNotIn(token, updater_block.group(1), token)

    def test_no_other_host_target_exists(self):
        for name in OLD_NAMES:
            self.assertNotIn(name, CMAKE, name)
        self.assertNotIn("add_executable(trespasser ", CMAKE)
        self.assertEqual(len(re.findall(r"^add_executable\(trespasser_recomp ", CMAKE, re.M)), 1)

    def test_every_lift_is_linked_into_it(self):
        self.assertIn("jpt_unpack_lifts(", CMAKE)
        self.assertEqual(sorted(lifts.read_manifest()["lifts"]), ["lift116", "lift_es117", "lift_ptbr117"])
        host_block = re.search(r"^add_executable\(trespasser_recomp\b([^)]*)\)", CMAKE, re.M)
        self.assertIsNotNone(host_block)
        for token in ("${HOST_SOURCES}", "${PCRECOMP_SOURCES}", "${GEN_SOURCES}"):
            self.assertIn(token, host_block.group(1), token)

    def test_the_build_script_builds_and_runs_only_it(self):
        for name in OLD_NAMES:
            self.assertNotIn(name, BUILD, name)
        self.assertIn("'trespasser_recomp'", BUILD)
        self.assertIsNone(re.search(r"\btrespasser\.exe\b", BUILD))
        self.assertIsNone(re.search(r"hostTargets.*trespasser_(es|ptbr)", BUILD))

    def test_the_old_one_host_per_lift_code_is_gone(self):
        for path in sources():
            text = path.read_text(encoding="utf-8", errors="replace")
            for name in ("forward_to_host", "JPT_FORWARDED_HOST", "jpt_host_dispatch", "JPT_EXPECTED_LIFT", "trespasser_es117",
                         "trespasser_ptbr117", "jpt_game_version_runs_here"):
                self.assertNotIn(name, text, f"{path.relative_to(ROOT)}: {name}")
        self.assertFalse((ROOT / "src" / "common" / "jpt_host_dispatch.c").exists())

    def test_the_version_table_names_no_host_program(self):
        text = (ROOT / "src" / "common" / "jpt_game_version.c").read_text(encoding="utf-8")
        self.assertNotIn("trespasser", text.replace("trespass.exe", ""))

    def test_the_host_sources_name_no_lift_symbol_directly(self):
        for path in (ROOT / "src" / "host").rglob("*.[ch]"):
            if "profiles" in path.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertIsNone(re.search(r"\b(?:lift116|lift_es117|lift_ptbr117)_(?:lifted_)?sub_[0-9A-F]{8}\b", text), path.name)

    def test_the_host_reads_no_build_time_lift_choice(self):
        for path in (ROOT / "src" / "host").rglob("*.[ch]"):
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn("JPT_GUEST_VA_MAP", text, path.name)
            self.assertNotIn("JPT_PROFILE_HEADER", text, path.name)

    def test_the_start_up_reservation_comes_from_the_lifts_not_from_what_is_installed(self):
        main = (ROOT / "src" / "host" / "main.c").read_text(encoding="utf-8")
        self.assertIn("native32_hold_range(JPT_GUEST_IMAGE_BASE, jpt_guest_reservation_size())", main)
        for name in ("native32_hold_image", "largest_image_dir", "restart_for_game", "pe_image_size"):
            self.assertNotIn(name, main, name)
        data = (ROOT / "src" / "host" / "profiles" / "guest_profile_data.c").read_text(encoding="utf-8")
        for pid, lift in (("en-116", "lift116"), ("es-117", "lift_es117"), ("ptbr-117", "lift_ptbr117")):
            facts = json.loads((ROOT / "config" / ("target.json" if pid == "en-116" else f"profiles/{pid}.json")).read_text(encoding="utf-8"))
            block = data[data.index(f'"{lift}",'):]
            self.assertIn(f'{facts["size_of_image"]}u,', block[:600], lift)

    def test_each_patch_group_is_checked_at_run_time_where_it_installs(self):
        for name, macro in (("patch117.c", "JPT_P117_ADD_CHILD"), ("patch117_player.c", "JPT_P117_HAND_JUMP"),
                            ("patch117_physics.c", "JPT_P117_BOX_BOX")):
            text = (ROOT / "src" / "host" / name).read_text(encoding="utf-8")
            self.assertIn(f"if (!{macro}) return;", text, name)
            self.assertNotRegex(text, r"^#\s*if.*JPT_P117_", name)


if __name__ == "__main__":
    unittest.main()
