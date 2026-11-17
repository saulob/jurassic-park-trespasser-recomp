#!/usr/bin/env python3
"""No original executable is a requirement of the build, the launcher, the runtime or a release (headless):

    py -3 -m unittest tests.test_no_original_executable

Every mention of an original executable's name falls in one of three classes: development (tools of the maintainers, the
retail checks of the tests, the x64 groundwork), documentation, or import input (the disc's file the importer derives
guest.img from). The runtime class must stay empty.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

NAMES = re.compile(r"trespass\.exe|tpassp6|tpassp5|tpassk6|JPT_GAME_EXE|JPT_ENGINE_SHA256", re.I)
RUNTIME = ("src/host", "src/common", "cmake", "CMakeLists.txt", "tools/build.ps1", "tools/run.ps1", "tools/lifts.py",
           "tools/verify-release.ps1", "tools/_common.ps1", "assets", "packaging", "config/launcher_deps.txt")
IMPORT_INPUT = ("launcher/game_manifest_en_116.inc", "launcher/game_manifest_ru_116.inc", "launcher/game_manifest_es_117.inc",
                "launcher/game_manifest_ptbr_117.inc", "launcher/game_files.cpp", "launcher/game_install.cpp")
SUFFIXES = (".c", ".h", ".cpp", ".inc", ".cmake", ".ps1", ".py", ".txt", ".json", ".md")


def files(prefixes):
    for prefix in prefixes:
        path = ROOT / prefix
        found = [path] if path.is_file() else sorted(p for p in path.rglob("*") if p.is_file()) if path.exists() else []
        for p in found:
            if p.suffix in SUFFIXES and "__pycache__" not in p.parts:
                yield p


DEVELOPMENT_LINES = ("_GAME_EXE=", "'trespass.exe', 'build')")  # retail checks of two maintainer tests; the release's forbidden names


def mentions(prefixes):
    out = []
    for p in files(prefixes):
        for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if NAMES.search(line) and not any(marker in line for marker in DEVELOPMENT_LINES):
                out.append(f"{p.relative_to(ROOT).as_posix()}:{n}: {line.strip()[:100]}")
    return out


class NoOriginalExecutable(unittest.TestCase):
    def test_the_runtime_build_and_release_classes_mention_none(self):
        self.assertEqual(mentions(RUNTIME), [])

    def test_the_launcher_mentions_one_only_as_an_import_input(self):
        launcher = [m for m in mentions(("launcher",)) if not m.startswith(("launcher/patches", "launcher/localization"))]
        for line in launcher:
            self.assertTrue(line.startswith(IMPORT_INPUT), line)

    def test_no_manifest_row_installs_an_executable_or_names_one_as_an_installed_file(self):
        for inc in sorted((ROOT / "launcher").glob("game_manifest_*.inc")):
            for line in inc.read_text(encoding="utf-8").splitlines():
                m = re.match(r'\s*\{[^"]*"([^"]*)",\s*"([^"]*)"', line)
                if m:
                    self.assertFalse(m.group(2).lower().endswith(".exe"), f"{inc.name}: installs {m.group(2)}")

    def test_the_version_table_and_the_lift_expectations_hold_no_executable_hash(self):
        table = (ROOT / "src" / "common" / "jpt_game_version.c").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r'"[0-9a-f]{64}"', table))
        header = (ROOT / "src" / "common" / "jpt_game_version.h").read_text(encoding="utf-8")
        self.assertNotIn("exe_sha256", header)
        self.assertIn('JPT_GUEST_IMAGE "guest.img"', header)

    def test_the_host_never_hashes_or_opens_a_game_executable(self):
        for p in files(("src/host", "src/common")):
            text = p.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn("host_sha256_file", text, p.name)
            self.assertNotIn("LoadLibraryExA(path", text, p.name)

    def test_the_build_script_has_no_game_or_analysis_stage(self):
        text = (ROOT / "tools" / "build.ps1").read_text(encoding="utf-8")
        for word in ("Analysis", "Lifted C", "extract_game", "game\\versions", "-Iso", "analyze.py", "run_lift.py", "Relift", "Reanalyze"):
            self.assertNotIn(word, text, word)
        self.assertIn("lifts.py", text)


if __name__ == "__main__":
    unittest.main()
