"""The Linux artifact flow without running a binary: the source fingerprint, the self-check compared with sources and contract, and the
atomic staging that never replaces the last validated binary with an unvalidated one."""
import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import linux_artifact  # noqa: E402
import source_fingerprint  # noqa: E402


def write(path, text, newline="\n"):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text.replace("\n", newline))


def make_tree(root, contract=None):
    write(root / "CMakeLists.txt", "project(x)\n")
    write(root / "src" / "host" / "a.c", "int a;\n")
    write(root / "src" / "host" / "guest_override_list.h", "GUEST_OVERRIDE(00401000, REQUIRED)\nGUEST_OVERRIDE(00402000, OPTIONAL)\n")
    write(root / "src" / "host" / "lift_hook_list.h", "LIFT_HOOK(00441D1D)\n")
    write(root / "tests" / "feature_contract.json", json.dumps(contract or {"schema": 2, "features": []}))


def selfcheck_for(root, caps=("loading_hd",), fingerprint=None, overrides=2, hooks=1):
    return {
        "self_check": True,
        "build": {"source_fingerprint": fingerprint or source_fingerprint.fingerprint(root), "contract_sha256": linux_artifact.contract_sha256(root),
                  "compiler": "GNU 13", "version": "0", "git_commit": "x", "git_dirty": False, "target": "t", "config": "c", "glibc_baseline": "2.31",
                  "ffmpeg": {"enabled": True, "lgpl_only": True}},
        "registry": {"override_registry": overrides, "overrides_linux_applicable": 1, "overrides_installed_default_settings": 1, "lift_hooks": hooks,
                     "lift_hooks_active": hooks, "selfmod_entries": 1, "main_entries": 1},
        "audio": {"pacer": True, "queue_policy_version": 1},
        "capability_count": len(caps),
        "capabilities": {name: {"ok": True, "detail": ""} for name in caps},
    }


class Fixture(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name) / "repo"
        self.contract = {"schema": 2, "features": [{"id": "f", "linux": {"status": "PASS", "runtime": ["loading_hd"]}}]}
        make_tree(self.root, self.contract)
        self.elf = Path(self.folder.name) / "built" / "trespasser_recomp"
        write(self.elf, "binary-v1")
        self.answer = selfcheck_for(self.root)
        self.original = linux_artifact.run_headless
        linux_artifact.run_headless = lambda elf, argument: (self.answer, 0, [], "")
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()

    def tearDown(self):
        self.quiet.__exit__(None, None, None)
        linux_artifact.run_headless = self.original
        self.folder.cleanup()


class Fingerprint(unittest.TestCase):
    def test_it_is_deterministic_and_ignores_line_endings(self):
        with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as two:
            make_tree(Path(one))
            make_tree(Path(two))
            write(Path(two) / "src" / "host" / "a.c", "int a;\n", "\r\n")
            self.assertEqual(source_fingerprint.fingerprint(Path(one)), source_fingerprint.fingerprint(Path(two)))
            write(Path(two) / "src" / "host" / "a.c", "int b;\n")
            self.assertNotEqual(source_fingerprint.fingerprint(Path(one)), source_fingerprint.fingerprint(Path(two)))

    def test_build_output_logs_user_data_and_game_data_never_take_part(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            make_tree(root)
            before = source_fingerprint.fingerprint(root)
            for ignored in ("src/build/x.c", "src/logs/x.log", "src/user_data/x.ini", "src/game/x.dat", "config/local.json"):
                write(root / ignored, "changed")
            self.assertEqual(before, source_fingerprint.fingerprint(root))

    def test_lifted_code_is_identified_by_name_and_size(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            make_tree(root)
            write(root / "src" / "recomp" / "gen" / "x.c", "aaaa")
            before = source_fingerprint.fingerprint(root)
            write(root / "src" / "recomp" / "gen" / "x.c", "bbbb")
            self.assertEqual(before, source_fingerprint.fingerprint(root))
            write(root / "src" / "recomp" / "gen" / "x.c", "bbbbb")
            self.assertNotEqual(before, source_fingerprint.fingerprint(root))


class Check(Fixture):
    def rows(self):
        return {r.name: r for r in linux_artifact.check_elf(self.elf, self.root)}

    def test_a_binary_of_these_sources_with_its_capabilities_passes(self):
        rows = self.rows()
        self.assertTrue(all(r.ok for r in rows.values()), [(n, r.lines) for n, r in rows.items() if not r.ok])

    def test_a_binary_built_from_other_sources_is_stale(self):
        self.answer = selfcheck_for(self.root, fingerprint="0" * 64)
        self.assertFalse(self.rows()["elf build provenance"].ok)

    def test_a_missing_capability_the_contract_needs_fails(self):
        self.answer = selfcheck_for(self.root, caps=("audio_pacer",))
        row = self.rows()["elf feature contract"]
        self.assertFalse(row.ok)
        self.assertIn("loading_hd", " ".join(row.lines))

    def test_override_count_must_match_the_list(self):
        self.answer = selfcheck_for(self.root, overrides=5)
        self.assertFalse(self.rows()["elf build provenance"].ok)

    def test_a_changed_contract_makes_the_binary_stale(self):
        write(self.root / "tests" / "feature_contract.json", json.dumps({"schema": 2, "features": []}))
        self.assertFalse(self.rows()["elf build provenance"].ok)

    def test_a_self_check_that_left_files_behind_fails(self):
        linux_artifact.run_headless = lambda elf, argument: (self.answer, 0, ["created.log"], "")
        self.assertFalse(self.rows()["elf self-check"].ok)


class Stage(Fixture):
    def test_stage_writes_the_binary_and_its_manifest_atomically(self):
        dest = Path(self.folder.name) / "bin"
        self.assertEqual(linux_artifact.stage(self.elf, dest, None, self.root), 0)
        manifest = json.loads((dest / linux_artifact.MANIFEST_NAME).read_text(encoding="utf-8"))
        self.assertEqual(manifest["elf_sha256"], linux_artifact.sha256_file(dest / linux_artifact.ARTIFACT_NAME))
        self.assertEqual(manifest["test_result"], "PASS")
        self.assertEqual(sorted(p.name for p in dest.iterdir()), [linux_artifact.ARTIFACT_NAME, linux_artifact.MANIFEST_NAME])

    def test_a_failing_check_keeps_the_last_validated_binary(self):
        dest = Path(self.folder.name) / "bin"
        self.assertEqual(linux_artifact.stage(self.elf, dest, None, self.root), 0)
        validated = (dest / linux_artifact.ARTIFACT_NAME).read_bytes()
        write(self.elf, "binary-v2-unvalidated")
        self.answer = selfcheck_for(self.root, fingerprint="1" * 64)
        self.assertEqual(linux_artifact.stage(self.elf, dest, None, self.root), 1)
        self.assertEqual((dest / linux_artifact.ARTIFACT_NAME).read_bytes(), validated)
        self.assertFalse((dest / (linux_artifact.ARTIFACT_NAME + ".new")).exists())

    def test_verify_notices_a_binary_that_is_not_the_validated_one(self):
        dest = Path(self.folder.name) / "bin"
        linux_artifact.stage(self.elf, dest, None, self.root)
        self.assertEqual(linux_artifact.verify(dest / linux_artifact.ARTIFACT_NAME, dest / linux_artifact.MANIFEST_NAME, self.root), 0)
        write(dest / linux_artifact.ARTIFACT_NAME, "a stale binary")
        self.assertEqual(linux_artifact.verify(dest / linux_artifact.ARTIFACT_NAME, dest / linux_artifact.MANIFEST_NAME, self.root), 1)


class Wiring(unittest.TestCase):
    def read(self, relative):
        return (ROOT / relative).read_text(encoding="utf-8", errors="replace")

    def test_the_manual_build_dir_is_not_the_artifact_dir(self):
        for script in ("tools/build-linux.sh", "tools/test-linux.sh"):
            self.assertIn("build_dir=$repo/build/linux-work", self.read(script))

    def test_the_gate_stages_after_everything_passed(self):
        text = self.read("tools/test.ps1")
        self.assertIn("linux_artifact.py", text)
        self.assertIn("NOT STAGED", text)
        self.assertLess(text.index("$parityFailed = ($gFail"), text.index("'stage', '--elf'"))

    def test_the_packager_verifies_and_never_builds(self):
        text = self.read("tools/package-linux.sh")
        self.assertIn("linux_artifact.py", text)
        self.assertNotIn("build-linux.sh", re.sub(r"#.*", "", text))

    def test_every_capability_the_contract_names_is_probed_by_the_binary(self):
        probes = set(re.findall(r'cap\("([a-z_0-9]+)"', self.read("src/linux/linux_selfcheck.c")))
        contract = json.loads(self.read("tests/feature_contract.json"))
        named = {name for f in contract["features"] for name in f.get("linux", {}).get("runtime", [])}
        self.assertTrue(named)
        self.assertEqual(named - probes, set())

    def test_setcursorpos_only_stores_the_position(self):
        text = self.read("src/linux/linux_user32.c")
        body = text[text.index("static uint64_t u_SetCursorPos"):]
        body = body[: body.index("\n}\n")].split("\n", 1)[1]
        self.assertEqual(len(re.findall(r"s_cursor_[xy] = ", body)), 2)
        self.assertNotRegex(body, r"static|count|\+\+")


if __name__ == "__main__":
    unittest.main(verbosity=2)
