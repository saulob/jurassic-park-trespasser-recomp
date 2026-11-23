"""The compiler command lines of the test targets are sound, whether or not anything was recompiled (headless):
    py -3 -I -B -m unittest discover -s tests -p "test_compile_commands.py"

tools\test.ps1 exports `ninja -t commands` of every test target to the files named by JPT_GATE_COMMANDS; run alone, the
test asks ninja for the x86 build folder itself. Each compile command is checked for flags that contradict each other
(the cause of cl's D9025 "overriding" warnings), for warning suppression in the project's own code and for a warning
level below /W3. Generated lifted code and the pcrecomp, SDL and ImGui sources are the only places warnings may be
muted.
"""
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402

FAMILIES = {
    "optimisation": re.compile(r"^[/-]O(1|2|d|x)$"),
    "inlining": re.compile(r"^[/-]Ob[0-3]$"),
    "warning level": re.compile(r"^[/-](W[0-4]|Wall)$"),
    "runtime library": re.compile(r"^[/-](MD|MT|MDd|MTd)$"),
}
SUPPRESSION = re.compile(r"^[/-](w|wd\d+|W0)$")
OVERRIDE = re.compile(r"^[/-]w$")
GENERATED = re.compile(r"(water_lifted|water_lifted_alt|water_callees|input_lifted)\.c$", re.I)
JUSTIFIED = {("launcher/page_icons.cpp", "/wd4702"): "vendored nanosvg: C4702 is reported at code generation, past its include's warning pragma"}
OUTSIDE = ("src/recomp/", "external/", "third_party/", "launcher/third_party/", "launcher/updater/third_party/")


def tokens(command):
    return [t.strip('"') for t in re.findall(r'"[^"]*"|\S+', command)]


def compile_commands(lines):
    for line in lines:
        parts = tokens(line)
        if not parts or not re.search(r"cl\.exe$", parts[0], re.I):
            continue
        for flag in ("-c", "/c"):
            if flag in parts and parts.index(flag) + 1 < len(parts):
                yield parts, parts[parts.index(flag) + 1]
                break


def relative(source):
    path = Path(source)
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except (ValueError, OSError):
        return path.as_posix().replace("\\", "/")


def analyze(lines):
    problems, project, total = [], 0, 0
    for parts, source in compile_commands(lines):
        total += 1
        name = relative(source)
        for family, pattern in FAMILIES.items():
            values = sorted({t.lower() if family == "warning level" else t[1:] for t in parts[1:] if pattern.match(t)})
            if len(values) > 1:
                problems.append(f"{name}: {family} flags contradict each other: {', '.join('/' + v for v in values)}")
        levels = [t for t in parts[1:] if FAMILIES["warning level"].match(t)]
        if levels and any(OVERRIDE.match(t) for t in parts[1:]) and not GENERATED.search(name):
            problems.append(f"{name}: /w overrides {' '.join(levels)} (cl D9025)")
        outside = name.startswith(OUTSIDE) or "/external/" in name or bool(GENERATED.search(name))
        if outside:
            continue
        project += 1
        muted = [t for t in parts[1:] if SUPPRESSION.match(t) and (name, t) not in JUSTIFIED]
        if muted:
            problems.append(f"{name}: warnings are muted in project code by {' '.join(muted)}")
        if not any(re.match(r"^[/-](W3|W4|Wall)$", t) for t in parts[1:]):
            problems.append(f"{name}: no warning level of /W3 or higher")
    return problems, project, total


def command_files():
    named = os.environ.get("JPT_GATE_COMMANDS")
    if named:
        return [Path(p) for p in named.split(";") if p]
    return []


def load_lines():
    files = command_files()
    if files:
        missing = [str(f) for f in files if not f.exists() or f.stat().st_size == 0]
        if missing:
            raise AssertionError(f"the gate exported no compile commands: {missing}")
        return [line for f in files for line in f.read_text(encoding="utf-8", errors="replace").splitlines()]
    folder = ROOT / "build" / "cmake" / "msvc-x86"
    if not (folder / "build.ninja").exists():
        raise unittest.SkipTest("no configured x86 build folder: compile commands unavailable (DATA UNAVAILABLE)")
    try:
        done = subprocess.run(["ninja", "-C", str(folder), "-t", "commands"] + sorted(
            {m for m in re.findall(r"^build ([A-Za-z0-9_]+_tests[A-Za-z0-9_]*): phony", (folder / "build.ninja").read_text(), re.M)}),
            capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise unittest.SkipTest(f"ninja is not available to list the compile commands: {error}")
    return done.stdout.splitlines()


class CompileCommands(unittest.TestCase):
    def test_the_analyzer_recognises_the_defects_it_exists_for(self):
        base = r"C:\vs\cl.exe /nologo /W3 %s /c " + str(ROOT / "src" / "host" / "x.c")
        self.assertTrue(analyze([base % "/Ob1 /Ob2"])[0])
        self.assertTrue(analyze([base % "/O2 /Od"])[0])
        self.assertTrue(analyze([base % "/wd4244"])[0])
        self.assertTrue(analyze([base % "/w"])[0])
        self.assertTrue(analyze([r"C:\vs\cl.exe /W1 /c " + str(ROOT / "tests" / "x.c")])[0])
        self.assertFalse(analyze([base % "/O2 /Ob1 /O2"])[0])
        lifted = r"C:\vs\cl.exe /w /O2 /c " + str(ROOT / "build" / "water_lifted.c")
        self.assertFalse(analyze([lifted])[0])
        self.assertFalse(analyze([r"C:\vs\cl.exe /W1 /wd4731 /c " + str(ROOT / "external" / "pcrecomp" / "runtime" / "native32" / "native32.c")])[0])
        self.assertFalse(analyze([r"C:\vs\cl.exe /w /c " + str(ROOT / "launcher" / "updater" / "third_party" / "miniz" / "miniz.c")])[0])
        self.assertTrue(analyze([r"C:\vs\cl.exe /W4 /w /c " + str(ROOT / "launcher" / "updater" / "third_party" / "miniz" / "miniz.c")])[0])
        self.assertTrue(analyze([r"C:\vs\cl.exe /W4 /w /c " + str(ROOT / "launcher" / "updater" / "update_job.cpp")])[0])

    def test_no_test_target_has_contradicting_or_muted_flags(self):
        problems, project, total = analyze(load_lines())
        self.assertGreater(total, 20, "too few compile commands to be the test targets'")
        self.assertGreater(project, 10)
        self.assertEqual(problems, [])

    def test_every_test_source_is_compiled_by_some_target(self):
        names = {relative(source) for _, source in compile_commands(load_lines())}
        sources = {f"tests/{p.name}" for p in gs.TESTS.iterdir() if re.search(r"_tests\.(c|cpp)$", p.name) and p.name != "x64_tests.c" and not p.name.startswith("linux_")}
        self.assertEqual(sorted(sources - names), [])


def tearDownModule():
    try:
        problems, project, total = analyze(load_lines())
        gs.report("compile_commands", {"commands": total, "project_commands": project, "problems": problems})
    except Exception:
        pass


if __name__ == "__main__":
    unittest.main()
