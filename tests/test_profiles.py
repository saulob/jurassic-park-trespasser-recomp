"""Headless checks of the game version table against the profile files, guest maps and lifts (no game files needed).

    py -3 -m unittest tests.test_profiles

A version may be marked playable only when its guest map has nothing unresolved or unproven; this fails the moment a
version is enabled before that is true, or a map regresses.
"""
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_profiles  # noqa: E402
import lifts  # noqa: E402
from common import GUEST_MAPS_DIR, PROFILES_DIR  # noqa: E402

BASELINE = "lift116"


def patch_group_addresses(profile_id):
    """The hooks and overrides of the patch groups the profile does not apply: absent from its executable by design."""
    groups = json.loads((ROOT / "config" / "patch117_groups.json").read_text(encoding="utf-8"))
    profile = json.loads((PROFILES_DIR / f"{profile_id}.json").read_text(encoding="utf-8"))
    return {va for g, d in groups.items() if not profile["patch117"][g] for k in ("hooks", "overrides") for va in d[k]}


class ProfileTests(unittest.TestCase):
    def test_every_other_lift_has_a_profile_and_a_guest_map(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            self.assertTrue((PROFILES_DIR / f"{row['id']}.json").exists(), row["id"])
            self.assertTrue((GUEST_MAPS_DIR / f"{row['id']}.json").exists(), row["id"])

    def test_profile_and_map_name_one_source_build(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            profile = json.loads((PROFILES_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))
            gmap = json.loads((GUEST_MAPS_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))
            self.assertEqual(profile["sha256"], gmap["sha256"], row["id"])
            self.assertEqual(profile["lift"], row["lift"], row["id"])
            self.assertEqual(profile["id"], row["id"])

    def test_a_lift_is_made_from_one_executable(self):
        lifts = {}
        for row in check_profiles.versions():
            profile = PROFILES_DIR / f"{row['id']}.json"
            sha = json.loads(profile.read_text(encoding="utf-8"))["sha256"] if profile.exists() else "baseline"
            lifts.setdefault(row["lift"], set()).add(sha)
        for lift, shas in lifts.items():
            self.assertEqual(len(shas), 1, lift)

    def test_a_playable_version_has_every_address_proven(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE or not row["playable"]:
                continue
            gmap = json.loads((GUEST_MAPS_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))["addresses"]
            off = patch_group_addresses(row["id"])
            bad = {va: r["class"] for va, r in gmap.items() if r["class"] not in check_profiles.PROVEN and not
                   (r["class"] == "absent" and (va in off or "patch117" in r["where"] or "lift_hook_list" in r["where"]))}
            self.assertEqual(bad, {}, f"{row['id']} is marked playable")
            for va in check_profiles.required_overrides():
                ok = gmap[va]["class"] in check_profiles.PROVEN or (gmap[va]["class"] == "absent" and va in off)
                self.assertTrue(ok, f"{row['id']} required override {va} is {gmap[va]['class']}")

    def test_generated_lifts_come_from_their_executable(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            profile = json.loads((PROFILES_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))
            dispatch = ROOT / "src" / "recomp" / profile["gen_dir"] / "recomp_dispatch.c"
            if dispatch.exists():
                text = dispatch.read_text(encoding="utf-8")
                self.assertIn(f'"{profile["sha256"]}"', text, row["id"])
                self.assertIn(f'"{row["lift"]}", "{row["id"]}", "{profile["sha256"]}"', text, row["id"])

    def test_the_maps_keep_what_was_proven(self):
        known = {"es-117": 321, "ptbr-117": 312}
        for pid, proven in known.items():
            gmap = json.loads((GUEST_MAPS_DIR / f"{pid}.json").read_text(encoding="utf-8"))["addresses"]
            counts = {"proven": 0, "candidate": 0, "unresolved": 0, "absent": 0}
            for r in gmap.values():
                counts["proven" if r["class"] in check_profiles.PROVEN else r["class"]] += 1
            self.assertGreaterEqual(counts["proven"], proven, pid)
            self.assertEqual(counts["candidate"] + counts["unresolved"], 0, pid)


def tool(*args):
    return subprocess.run([sys.executable, str(ROOT / "tools" / args[0]), *args[1:]], capture_output=True, text=True,
                          stdin=subprocess.DEVNULL, cwd=ROOT)


class HostTests(unittest.TestCase):
    def test_no_raw_guest_address_is_left_in_the_host(self):
        r = tool("gva_convert.py", "--check")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_the_profile_tables_are_current(self):
        r = tool("gen_guest_va.py", "--check")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_the_host_offset_manifest_is_current(self):
        r = tool("host_offsets.py")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_abi_results_match_the_executable_and_are_clean(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            result = json.loads((ROOT / "config" / "abi_results" / f"{row['id']}.json").read_text(encoding="utf-8"))
            profile = json.loads((PROFILES_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))
            self.assertEqual(result["sha256"], profile["sha256"], row["id"])
            self.assertEqual(result["mismatched"], 0, row["id"])

    def test_a_playable_version_has_clean_layouts_abi_and_a_host_program(self):
        r = tool("check_profiles.py")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_every_lift_is_linked_into_the_one_program_with_its_profile_tables(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        for row in check_profiles.versions():
            gen = "gen" if row["lift"] == BASELINE else json.loads((PROFILES_DIR / f"{row['id']}.json").read_text(
                encoding="utf-8"))["gen_dir"]
            self.assertEqual(lifts.read_manifest()["lifts"][row["lift"]]["gen_dir"], gen, row["id"])
            if row["lift"] != BASELINE:
                self.assertTrue((ROOT / "src" / "host" / "profiles" / row["lift"] / "guest_override_list.h").exists(), row["lift"])
        for name in ("guest_va_ids.h", "guest_layout.h", "guest_profile_data.c", "guest_gsub_tables.c"):
            self.assertTrue((ROOT / "src" / "host" / "profiles" / name).exists(), name)

    def test_every_patch_group_is_decided_for_every_other_profile(self):
        groups = json.loads((ROOT / "config" / "patch117_groups.json").read_text(encoding="utf-8"))
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            profile = json.loads((PROFILES_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))
            self.assertEqual(set(profile["patch117"]), set(groups), row["id"])

    def test_an_absent_address_belongs_to_a_patch_group_the_profile_does_not_apply(self):
        for row in check_profiles.versions():
            if row["lift"] == BASELINE:
                continue
            off = patch_group_addresses(row["id"])
            gmap = json.loads((GUEST_MAPS_DIR / f"{row['id']}.json").read_text(encoding="utf-8"))["addresses"]
            for va, r in gmap.items():
                if r["class"] == "absent" and "patch117" not in r["where"] and "lift_hook_list" not in r["where"]:
                    self.assertIn(va, off, f"{row['id']} {va} {r['where']}")


if __name__ == "__main__":
    unittest.main()
