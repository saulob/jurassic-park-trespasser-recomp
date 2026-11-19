"""Every supported build has a complete row in the contract, and nothing differs between builds without being decided
(headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_build_parity.py"

The builds are the rows of jpt_game_versions[] in src/common/jpt_game_version.c; nothing here names one. For each build:
its profile and fingerprint, the lift that runs it, its guest addresses, host structure offsets, required overrides,
patch sites, features, cheats, language, saves folder and the launcher's way of identifying it. The installed executable
of a build is compared with its fingerprint only when game\\versions\\<id> is present; otherwise the row says so.
"""
import hashlib
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_profiles  # noqa: E402
import gate_support as gs  # noqa: E402

PROVEN = check_profiles.PROVEN
OFFSET_PROVEN = {"unmoved", "moved", "not_guest", "gated"}
MANIFEST_RE = re.compile(r'"guest\.img",\s*\d+,\s*"([0-9a-f]{64})"')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def baseline_profile():
    target = json.loads((gs.CONFIG / "target.json").read_text(encoding="utf-8"))
    return {"sha256": target["sha256"], "size": target["size"], "file_version": target["file_version"]}


def evaluate():
    versions = gs.game_versions()
    langs = {l["code"] for l in gs.languages()}
    lifts = json.loads((ROOT / "lifts" / "manifest.json").read_text(encoding="utf-8"))["lifts"]
    groups = json.loads((gs.CONFIG / "patch117_groups.json").read_text(encoding="utf-8"))
    offsets = json.loads((gs.CONFIG / "host_offsets.json").read_text(encoding="utf-8"))["offsets"]
    profile_data = (gs.HOST / "profiles" / "guest_profile_data.c").read_text(encoding="utf-8")
    launcher_tests = (gs.TESTS / "launcher_tests.cpp").read_text(encoding="utf-8")
    required = check_profiles.required_overrides()
    cheat_files = gs.read(gs.HOST / "cheats.c") + gs.read(gs.HOST / "cheats_policy.c")
    cheat_addresses = {int(a, 16) for a in re.findall(r"GVA\((0x[0-9A-Fa-f]{8})\)", cheat_files)}
    feature_names = [name for name, _ in check_profiles.FEATURES] + ["Other host code"]
    saves = {}
    out = []
    for v in versions:
        baseline = v["lift"] == check_profiles.BASELINE_LIFT
        row = {"id": v["id"], "lift": v["lift"], "language": v["language_code"], "build": v["build"], "problems": [],
               "playable": v["unplayable"] is None, "facts": {}}
        problems = row["problems"]
        if not baseline:
            path = gs.CONFIG / "profiles" / f"{v['id']}.json"
            if not path.exists():
                problems.append("no profile file")
                out.append(row)
                continue
            profile = json.loads(path.read_text(encoding="utf-8"))
        else:
            profile = baseline_profile()
        fingerprint = profile["sha256"]
        row["facts"]["fingerprint"] = fingerprint[:12]
        manifest_file = gs.LAUNCHER / f"game_manifest_{v['id'].replace('-', '_')}.inc"
        if not manifest_file.exists():
            problems.append(f"{manifest_file.name} is missing: the launcher cannot identify the disc")
            manifest_hash = None
        else:
            found = MANIFEST_RE.search(manifest_file.read_text(encoding="utf-8"))
            manifest_hash = found.group(1) if found else None
            if manifest_hash != fingerprint:
                problems.append(f"the launcher manifest's guest.img digest is not the profile fingerprint ({manifest_hash})")
        lift = lifts.get(v["lift"])
        if not lift:
            problems.append(f"lifts/manifest.json has no {v['lift']}")
        elif lift["source_sha256"] != fingerprint:
            problems.append(f"{v['lift']} was lifted from another executable than this build's")
        if not baseline and f'"{v["lift"]}"' not in profile_data:
            problems.append(f"src/host/profiles/guest_profile_data.c has no table for {v['lift']}")
        if v["language_code"] not in langs:
            problems.append(f"language {v['language_code']} is not a language of the port")
        if v["saves"] in saves:
            problems.append(f"saves folder {v['saves']} is shared with {saves[v['saves']]}")
        saves[v["saves"]] = v["id"]
        if v["saves"] is None and not baseline:
            problems.append("no saves folder of its own")
        if f'"{v["id"]}"' not in launcher_tests and v["id"] not in launcher_tests:
            problems.append("launcher_tests.cpp never names this build")

        if baseline:
            row["facts"]["addresses"] = "identity (the lift's own addresses)"
            row["facts"]["offsets"] = "baseline"
            row["facts"]["required_overrides"] = f"{len(required)}/{len(required)}"
            row["facts"]["features"] = {name: "baseline" for name in feature_names}
            row["facts"]["patch117"] = "applies (retail 1.0 build 116 code)"
            row["facts"]["cheat_addresses"] = f"{len(cheat_addresses)}/{len(cheat_addresses)} (baseline)"
        else:
            gmap_path = gs.CONFIG / "guest_maps" / f"{v['id']}.json"
            if not gmap_path.exists():
                problems.append("no guest map")
                out.append(row)
                continue
            gmap = json.loads(gmap_path.read_text(encoding="utf-8"))
            if gmap["sha256"] != fingerprint:
                problems.append("the guest map was made from another executable than the profile's")
            addresses = gmap["addresses"]
            off = {int(va, 16) for g, d in groups.items() if not profile["patch117"][g] for k in ("hooks", "overrides") for va in d[k]}
            def decided(va, r):
                if r["class"] in PROVEN:
                    return True
                return r["class"] == "absent" and (int(va, 16) in off or "patch117" in r["where"] or "lift_hook_list" in r["where"])

            bad = [f"{va} {r['class']}" for va, r in addresses.items() if not decided(va, r)]
            if bad:
                problems.append(f"{len(bad)} guest addresses unproven: {', '.join(bad[:4])}")
            row["facts"]["addresses"] = f"{sum(1 for r in addresses.values() if r['class'] in PROVEN)} proven of {len(addresses)}"
            statuses = {r.get(v["id"], {}).get("status", "unproven") for r in offsets}
            row["facts"]["offsets"] = f"{len(offsets)} rows: " + ", ".join(
                f"{sum(1 for r in offsets if r.get(v['id'], {}).get('status') == name)} {name}" for name in sorted(statuses))
            if not statuses <= OFFSET_PROVEN:
                problems.append(f"host structure offsets neither proven nor decided: {sorted(statuses - OFFSET_PROVEN)}")
            ok = sum(1 for va in required if addresses.get(va, {}).get("class") in PROVEN + ("absent",))
            row["facts"]["required_overrides"] = f"{ok}/{len(required)}"
            if ok != len(required):
                problems.append(f"{len(required) - ok} required overrides unresolved")
            by_feature = {}
            for va, r in addresses.items():
                name = check_profiles.feature_of(r["where"])
                by_feature.setdefault(name, [0, 0])
                by_feature[name][0 if decided(va, r) else 1] += 1
            row["facts"]["features"] = {name: ("n/a" if name not in by_feature else f"{by_feature[name][0]} proven" if not by_feature[name][1] else f"{by_feature[name][1]} UNPROVEN") for name in feature_names}
            for name, (good, unproven_count) in by_feature.items():
                if unproven_count:
                    problems.append(f"feature {name}: {unproven_count} unproven addresses")
            applied = [g for g in groups if profile["patch117"][g]]
            row["facts"]["patch117"] = (f"the port applies {','.join(applied)}; the other groups are native in this build" if applied
                                         else "native in this build (all groups off by design)")
            by_va = {int(k, 16): r for k, r in addresses.items()}
            missing = [f"0x{a:08X}" for a in sorted(cheat_addresses) if a not in by_va]
            unproven_cheat = [f"0x{a:08X}" for a in sorted(cheat_addresses)
                              if a in by_va and by_va[a]["class"] not in PROVEN]
            listed = len(cheat_addresses) - len(missing)
            row["facts"]["cheat_addresses"] = f"{listed - len(unproven_cheat)}/{len(cheat_addresses)} proven"
            if unproven_cheat:
                problems.append(f"cheat addresses unproven: {', '.join(unproven_cheat)}")
            if missing:
                problems.append(f"cheat addresses the guest map does not list: {', '.join(missing[:4])}")
        if baseline:
            row["facts"]["offsets"] = "baseline (1.0.116 values define the host)"
        installed = gs.ROOT / "game" / "versions" / v["id"] / "guest.img"
        legacy = gs.ROOT / "game" / "trespass.exe" if v["id"] == "en-116" else None
        real = installed if installed.exists() else legacy if legacy and legacy.exists() else None
        if real is None:
            row["evidence"] = "STATIC"
            row["real"] = "DATA UNAVAILABLE"
        else:
            actual = sha256(real)
            row["evidence"] = "STATIC+REAL"
            row["real"] = f"executable verified ({real.relative_to(ROOT)})" if actual == fingerprint else "MISMATCH"
            if actual != fingerprint:
                problems.append(f"{real.relative_to(ROOT)} is not the build the profile names")
        out.append(row)
    return out


ROWS = evaluate()


def tearDownModule():
    gs.report("build_parity", {"builds": ROWS})


class BuildParity(unittest.TestCase):
    def test_the_contract_lists_the_builds_of_the_table(self):
        self.assertGreaterEqual(len(ROWS), 4)
        self.assertEqual([r["id"] for r in ROWS], [v["id"] for v in gs.game_versions()])

    def test_every_supported_build_is_whole(self):
        for row in ROWS:
            with self.subTest(build=row["id"]):
                self.assertEqual(row["problems"], [])

    def test_features_do_not_differ_between_builds_without_a_decision(self):
        names = [set(r["facts"]["features"]) for r in ROWS]
        for found in names:
            self.assertEqual(found, names[0])

    def test_every_playable_build_names_its_real_data_state(self):
        for row in ROWS:
            self.assertIn(row["real"].split(" ")[0], ("executable", "DATA"), row["id"])


if __name__ == "__main__":
    unittest.main()
