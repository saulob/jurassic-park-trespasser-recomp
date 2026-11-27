#!/usr/bin/env python3
"""The lifts as project artifacts and a clean checkout without any original executable (headless):

    py -3 -m unittest tests.test_lifts
"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import lifts  # noqa: E402

ARCHIVE_NAMES = ("lift116", "lift_es117", "lift_ptbr117")


def cmake():
    found = shutil.which("cmake")
    if found:
        return found
    vs = list(Path("C:/Program Files/Microsoft Visual Studio").glob("*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"))
    return str(vs[0]) if vs else None


class LiftArtifacts(unittest.TestCase):
    def test_every_lift_is_an_archive_the_manifest_describes(self):
        manifest = lifts.read_manifest()["lifts"]
        self.assertEqual(sorted(manifest), sorted(ARCHIVE_NAMES))
        self.assertEqual(lifts.problems(check_content=True), [])

    def test_the_manifest_carries_the_source_metadata_but_names_no_executable(self):
        for lift, entry in lifts.read_manifest()["lifts"].items():
            self.assertEqual(len(entry["source_sha256"]), 64, lift)
            self.assertEqual(entry["format"], lifts.lift_format(), lift)
            self.assertTrue(entry["archive"].endswith(".tar.xz"), lift)
            self.assertNotIn(".exe", json.dumps(entry).lower(), lift)

    def test_an_archive_holds_only_generated_sources(self):
        for lift, entry in lifts.read_manifest()["lifts"].items():
            names = [name for name, _ in lifts.archive_files(lifts.LIFTS_DIR / entry["archive"])]
            self.assertEqual(names, sorted(names), lift)
            self.assertTrue(all(n.startswith("recomp_") and n.endswith((".c", ".h", ".json")) for n in names), lift)
            modules = set(entry.get("modules", {}))
            self.assertEqual({n[len("recomp_"):-len(".json")] for n in names if n.endswith(".json")}, modules, lift)
            self.assertIn("recomp_dispatch.c", names)
            self.assertIn("recomp_funcs.h", names)
            self.assertIn("recomp_selfmod.c", names)

    def test_every_listed_override_is_a_renamed_body_in_the_packed_lift(self):
        listed = {"lift116": "src/host/guest_override_list.h", "lift_es117": "src/host/profiles/lift_es117/guest_override_list.h",
                  "lift_ptbr117": "src/host/profiles/lift_ptbr117/guest_override_list.h"}
        for lift, entry in lifts.read_manifest()["lifts"].items():
            text = "".join(data.decode("utf-8", "replace") for name, data in lifts.archive_files(lifts.LIFTS_DIR / entry["archive"])
                           if name.endswith(".c"))
            vas = re.findall(r"^GUEST_OVERRIDE\(([0-9A-F]{8}),", (ROOT / listed[lift]).read_text(encoding="utf-8"), re.M)
            self.assertTrue(vas, lift)
            for va in vas:
                lifted = len(re.findall(rf"^void {lift}_lifted_sub_{va}\(void\) \{{", text, re.M))
                plain = len(re.findall(rf"^void {lift}_sub_{va}\(void\) \{{", text, re.M))
                self.assertEqual((lifted, plain), (1, 0), f"{lift} sub_{va}: {lifted} lifted, {plain} plain definitions")

    def test_a_tampered_archive_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            shutil.copytree(ROOT / "lifts", tree / "lifts")
            shutil.copytree(ROOT / "cmake", tree / "cmake")
            with open(tree / "lifts" / "lift_es117.tar.xz", "r+b") as fh:
                fh.seek(100)
                fh.write(b"\x00\x01")
            program = cmake()
            if not program:
                self.skipTest("cmake not found")
            r = subprocess.run([program, f"-DSOURCE={tree.as_posix()}", "-P", str(ROOT / "tests" / "lift_resolution_check.cmake")],
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("lift_es117.tar.xz does not match lifts/manifest.json", " ".join((r.stdout + r.stderr).split()))

    def test_a_clean_checkout_without_game_files_or_executables_finds_every_lift(self):
        program = cmake()
        if not program:
            self.skipTest("cmake not found")
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            shutil.copytree(ROOT / "lifts", tree / "lifts")
            shutil.copytree(ROOT / "cmake", tree / "cmake")
            r = subprocess.run([program, f"-DSOURCE={tree.as_posix()}", "-P", str(ROOT / "tests" / "lift_resolution_check.cmake")],
                               capture_output=True, text=True)
            out = r.stdout + r.stderr
            self.assertEqual(r.returncode, 0, out)
            for lift in ARCHIVE_NAMES:
                self.assertIn(f"RESOLVED {lift} ", out)
            self.assertFalse((tree / "game").exists())
            executables = [p for p in tree.rglob("*") if p.suffix.lower() == ".exe"]
            self.assertEqual(executables, [])
            self.assertTrue((tree / "src" / "recomp" / "gen_es117" / "recomp_dispatch.c").exists())

    def test_the_project_cmake_unpacks_the_lifts_instead_of_requiring_generated_folders(self):
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("jpt_unpack_lifts(", text)
        self.assertNotIn("tools/run_lift.py", text)


if __name__ == "__main__":
    unittest.main()
