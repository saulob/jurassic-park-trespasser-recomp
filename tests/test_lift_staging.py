#!/usr/bin/env python3
"""tools/lift_staging.py and tools/run_lift.py's publishing, headless:
    py -3 -m unittest discover -s tests -p "test_lift_staging.py"
TRESPASSER_TEST_LIFT=1 also lifts a small closure of the game twice into temporary folders."""
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import lift_staging  # noqa: E402

WINDOWS = os.name == "nt"
NO_WAIT = (0, 0, 0, 0, 0)


def write_tree(folder, files):
    folder.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (folder / name).write_text(text, encoding="utf-8")


def digest(folder):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(folder).glob("recomp_*"))}


class Temp(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.handles = []

    def tearDown(self):
        for handle in self.handles:
            handle.close()
        self.dir.cleanup()

    def lock(self, path):
        handle = open(path, "rb")
        self.handles.append(handle)
        return handle


class Retrying(Temp):
    def test_other_errors_are_not_retried(self):
        slept = []
        with self.assertRaises(FileNotFoundError):
            lift_staging.retrying(lambda: open(self.root / "missing"), sleep=slept.append)
        with self.assertRaises(ValueError):
            lift_staging.retrying(lambda: int("x"), sleep=slept.append)
        self.assertEqual(slept, [])

    def test_busy_errors_use_the_backoff_then_give_up(self):
        slept = []

        def busy():
            raise PermissionError(13, "busy", None, lift_staging.WIN_SHARING_VIOLATION)

        with self.assertRaises(PermissionError):
            lift_staging.retrying(busy, sleep=slept.append)
        self.assertEqual(tuple(slept), lift_staging.RETRY_DELAYS)
        self.assertEqual(sum(lift_staging.RETRY_DELAYS) < 2.0, True)

    def test_busy_then_free(self):
        calls = []

        def action():
            calls.append(1)
            if len(calls) < 3:
                raise PermissionError(13, "denied", None, lift_staging.WIN_ACCESS_DENIED)
            return "done"

        self.assertEqual(lift_staging.retrying(action, sleep=lambda _: None), "done")
        self.assertEqual(len(calls), 3)


@unittest.skipUnless(WINDOWS, "a file open elsewhere blocks deletion on Windows")
class Cleanup(Temp):
    def staging(self):
        staged = lift_staging.create_staging(self.root / "staging")
        write_tree(staged, {"recomp_0000.c": "a", "recomp_0001.c": "b"})
        return staged

    def test_lock_released_during_the_retries(self):
        staged = self.staging()
        handle = self.lock(staged / "recomp_0001.c")
        slept = []

        def sleep(delay):
            slept.append(delay)
            if len(slept) == 2:
                handle.close()

        self.assertIs(lift_staging.remove_tree(staged, sleep=sleep), True)
        self.assertFalse(staged.exists())
        self.assertEqual(slept, list(lift_staging.RETRY_DELAYS[:2]))

    def test_lock_beyond_the_retries_is_a_warning_and_the_folder_stays(self):
        staged = self.staging()
        self.lock(staged / "recomp_0001.c")
        out = io.StringIO()
        self.assertFalse(lift_staging.finish_staging(staged, delays=NO_WAIT, sleep=lambda _: None, out=out))
        self.assertIn("could not be removed", out.getvalue())
        self.assertTrue((staged / "recomp_0001.c").exists())
        self.assertTrue(lift_staging.marker_for(staged).exists())
        self.handles[0].close()
        self.handles.clear()
        removed = lift_staging.sweep_staging(self.root / "staging", out=io.StringIO())
        self.assertEqual(removed, 1)
        self.assertFalse(staged.exists())
        self.assertFalse(lift_staging.marker_for(staged).exists())

    def test_publish_with_a_locked_staging_cleanup_still_counts(self):
        staged = self.staging()
        gen = self.root / "gen"
        self.lock(staged / "recomp_0001.c")
        written = lift_staging.publish(staged, gen, self.root / "stats.json", "{}", delays=NO_WAIT, sleep=lambda _: None)
        expected = digest(staged)
        self.assertEqual(written, 2)
        lift_staging.finish_staging(staged, delays=NO_WAIT, sleep=lambda _: None, out=io.StringIO())
        self.assertEqual(digest(gen), expected)
        self.assertTrue((self.root / "stats.json").exists())


@unittest.skipUnless(WINDOWS, "replacing a file open elsewhere fails on Windows")
class LockedOutputs(Temp):
    def setUp(self):
        super().setUp()
        self.staged = self.root / "staged"
        self.gen = self.root / "gen"
        write_tree(self.staged, {"recomp_0000.c": "new", "recomp_0001.c": "same"})
        write_tree(self.gen, {"recomp_0000.c": "old", "recomp_0001.c": "same"})
        self.stats = self.root / "stats.json"
        self.stats.write_text("{\"old\": 1}", encoding="utf-8")

    def test_locked_output_is_retried(self):
        handle = self.lock(self.gen / "recomp_0000.c")
        slept = []

        def sleep(delay):
            slept.append(delay)
            if len(slept) == 1:
                handle.close()

        self.assertEqual(lift_staging.publish(self.staged, self.gen, self.stats, "{}", sleep=sleep), 1)
        self.assertEqual((self.gen / "recomp_0000.c").read_text(encoding="utf-8"), "new")
        self.assertEqual(self.stats.read_text(encoding="utf-8"), "{}")
        self.assertEqual(sorted(p.name for p in self.gen.iterdir()), ["recomp_0000.c", "recomp_0001.c"])

    def test_persistent_lock_is_fatal_and_leaves_no_stats(self):
        self.lock(self.gen / "recomp_0000.c")
        with self.assertRaises(lift_staging.LiftPublishError):
            lift_staging.publish(self.staged, self.gen, self.stats, "{}", delays=NO_WAIT, sleep=lambda _: None)
        self.assertFalse(self.stats.exists())
        self.assertEqual(sorted(p.name for p in self.gen.iterdir()), ["recomp_0000.c", "recomp_0001.c"])
        self.assertEqual((self.gen / "recomp_0000.c").read_text(encoding="utf-8"), "old")


class Publishing(Temp):
    def setUp(self):
        super().setUp()
        self.staged = self.root / "staged"
        self.gen = self.root / "gen"
        self.stats = self.root / "analysis" / "stats.json"
        write_tree(self.staged, {"recomp_0000.c": "a", "recomp_0001.c": "b", "recomp_funcs.h": "h",
                                 "recomp_dispatch.c": "d"})

    def publish(self):
        return lift_staging.publish(self.staged, self.gen, self.stats, json.dumps({"lifted": 1}))

    def test_second_publish_rewrites_nothing(self):
        self.assertEqual(self.publish(), 4)
        first = digest(self.gen)
        times = {p.name: p.stat().st_mtime_ns for p in self.gen.glob("recomp_*")}
        time.sleep(0.02)
        self.assertEqual(self.publish(), 0)
        self.assertEqual(digest(self.gen), first)
        self.assertEqual({p.name: p.stat().st_mtime_ns for p in self.gen.glob("recomp_*")}, times)
        self.assertEqual(first, digest(self.staged))

    def test_only_changed_files_are_written_and_stale_ones_removed(self):
        self.publish()
        (self.staged / "recomp_0001.c").write_text("changed", encoding="utf-8")
        (self.staged / "recomp_0000.c").unlink()
        (self.gen / "notes.txt").write_text("not generated", encoding="utf-8")
        self.assertEqual(self.publish(), 1)
        self.assertFalse((self.gen / "recomp_0000.c").exists())
        self.assertEqual((self.gen / "recomp_0001.c").read_text(encoding="utf-8"), "changed")
        self.assertTrue((self.gen / "notes.txt").exists())

    def test_stats_come_last_and_an_old_one_never_survives_a_failure(self):
        self.publish()
        before = self.stats.read_text(encoding="utf-8")
        self.assertIn("lifted", before)
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaises(lift_staging.LiftPublishError):
            lift_staging.publish(empty, self.gen, self.stats, "{}")
        self.assertTrue(self.stats.exists())
        with self.assertRaises(lift_staging.LiftPublishError):
            lift_staging.publish(self.staged, self.gen, self.stats, "[1]")
        self.assertFalse(self.stats.exists())
        newest = max(p.stat().st_mtime_ns for p in self.gen.glob("recomp_*"))
        self.publish()
        self.assertGreaterEqual(self.stats.stat().st_mtime_ns, newest)

    def test_no_temporary_files_are_left(self):
        self.publish()
        names = [p.name for p in self.gen.iterdir()] + [p.name for p in self.stats.parent.iterdir()]
        self.assertFalse([n for n in names if n.endswith(".new")])


class Sweeping(Temp):
    def test_only_lift_staging_folders_are_removed(self):
        root, gen = self.root / "staging", self.root / "gen"
        finished, fresh, abandoned = root / "lift-done", root / "lift-fresh", root / "lift-old"
        other, current = root / "keep-me", root / "lift-current"
        for folder in (finished, fresh, abandoned, other, current):
            write_tree(folder, {"recomp_0000.c": "x"})
        lift_staging.marker_for(finished).write_text("", encoding="ascii")
        lift_staging.marker_for(root / "lift-gone").write_text("", encoding="ascii")
        legacy = gen / ".lift-gi9jkvk9"
        write_tree(legacy, {"recomp_0035.c": "x"})
        write_tree(gen, {"recomp_0000.c": "published", "recomp_dispatch.c": "d"})
        (gen / "other-dir").mkdir()
        later = time.time() + lift_staging.ABANDONED_AFTER_SECONDS + 10
        os.utime(abandoned, (0, 0))
        removed = lift_staging.sweep_staging(root, keep=current, legacy_dir=gen, now=time.time(), out=io.StringIO())
        self.assertEqual(removed, 3)
        for folder in (finished, abandoned, legacy):
            self.assertFalse(folder.exists(), folder)
        for folder in (fresh, other, current, gen / "other-dir"):
            self.assertTrue(folder.exists(), folder)
        self.assertEqual(sorted(p.name for p in gen.glob("recomp_*")), ["recomp_0000.c", "recomp_dispatch.c"])
        self.assertFalse(lift_staging.marker_for(finished).exists())
        self.assertFalse(lift_staging.marker_for(root / "lift-gone").exists())
        self.assertEqual(lift_staging.sweep_staging(root, keep=current, legacy_dir=gen, now=later, out=io.StringIO()), 1)

    def test_staging_lives_outside_src(self):
        self.assertEqual(lift_staging.STAGING_ROOT, ROOT / "build" / "lift-staging")
        self.assertNotIn("src", lift_staging.STAGING_ROOT.relative_to(ROOT).parts)


@unittest.skipUnless(os.environ.get("TRESPASSER_TEST_LIFT") == "1" and (ROOT / "game" / "trespass.exe").exists()
                     and (ROOT / "analysis" / "functions.json").exists(), "set TRESPASSER_TEST_LIFT=1 (needs the game)")
class SmallLift(Temp):
    def lift(self, gen, stats, staging):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "run_lift.py"), "--closure", "300",
                               "--gen-dir", str(gen), "--stats-file", str(stats), "--staging-root", str(staging)],
                              cwd=ROOT, capture_output=True, text=True, timeout=1800)

    def test_two_lifts_agree_and_the_second_rewrites_nothing(self):
        gen, stats, staging = self.root / "gen", self.root / "stats.json", self.root / "staging"
        first = self.lift(gen, stats, staging)
        self.assertEqual(first.returncode, 0, first.stdout[-2000:] + first.stderr[-2000:])
        digests, stats_text = digest(gen), stats.read_text(encoding="utf-8")
        self.assertIn("stable entries 38", first.stdout)
        self.assertEqual(list(staging.iterdir()), [])
        second = self.lift(gen, stats, staging)
        self.assertEqual(second.returncode, 0, second.stdout[-2000:] + second.stderr[-2000:])
        self.assertIn(": 0 of ", second.stdout)
        self.assertEqual(digest(gen), digests)
        self.assertEqual(stats.read_text(encoding="utf-8"), stats_text)
        self.assertEqual(json.loads(stats_text)["lift_errors"], [])


if __name__ == "__main__":
    unittest.main()
