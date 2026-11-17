#!/usr/bin/env python3
"""Checks every game version of src/common/jpt_game_version.c against its lift, its guest map and its features.

    py -3 tools/check_profiles.py [--matrix]

For each version: the lift it needs, its profile file and guest map, and how many of the host's guest addresses
are proven (identical, aligned, data), only candidates, or unresolved. The rule it enforces: a version the table marks
playable (no `unplayable` text) must have a profile, a map with nothing unresolved or unproven, every required override
resolved, and a generated lift (when one is present) made from the executable the table names. --matrix prints the
addresses by feature. Read only; exits 1 when a rule is broken.
"""
import argparse
import collections
import json
import re
import sys

from common import GUEST_MAPS_DIR, PROFILES_DIR, ROOT

TABLE = ROOT / "src" / "common" / "jpt_game_version.c"
OVERRIDES = ROOT / "src" / "host" / "guest_override_list.h"
BASELINE_LIFT = "lift116"
PROVEN = ("identical", "aligned", "data", "decided")

FEATURES = [
    ("Patch 1.1", ("patch117", "lift_hook_list")), ("Cheats and F10", ("cheats",)),
    ("Debug menu F5", ("debug_menu",)), ("Trespasser CE", ("ce_", "active_ai")),
    ("FOV and aspect ratio", ("widescreen", "fov_policy")), ("Physics and water", ("physics", "water_kernels", "bump_spans")),
    ("Gamepad and controls", ("gamepad", "guest_ui", "pad_", "tutorial_prompts")),
    ("Text and fonts", ("game_text", "l10n", "text_layout")), ("Front end and loading", ("frontend", "loading", "ui_canvas")),
    ("Videos and subtitles", ("video",)), ("Renderer", ("ddraw", "d3d", "cpu_polygons", "polygon_submit", "engine_", "terrain_quality",
                                                          "texture_streaming", "occlusion", "target_sync", "page_")),
    ("Window, input and system", ("window", "host_policy", "registry", "io_trace", "diagnostics", "exit_policy", "overlay",
                                    "save_preview", "objective", "compass", "guest_")),
]


def feature_of(where):
    name = where.split("/")[-1].split(":")[0]
    for feature, keys in FEATURES:
        if any(name.startswith(k) or k in name for k in keys):
            return feature
    return "Other host code"


def versions():
    text = TABLE.read_text(encoding="utf-8")
    body = text[text.index("jpt_game_versions[JPT_GAME_VERSION_COUNT] = {"):text.index("};", text.index("jpt_game_versions["))]
    out = []
    for row in re.split(r"\n    \{ ", body)[1:]:
        strings = re.findall(r'"((?:[^"\\]|\\.)*)"', row)
        out.append({"id": strings[0], "lift": next(s for s in strings if s.startswith("lift")),
                    "playable": re.search(r"NULL\s*\},?\s*$", row.strip()) is not None})
    return out


def offsets_unproven(profile_id):
    path = ROOT / "config" / "host_offsets.json"
    if not path.exists():
        return None
    return sum(1 for r in json.loads(path.read_text(encoding="utf-8"))["offsets"]
               if r.get(profile_id, {}).get("status") == "unproven")


def abi_mismatches(profile_id):
    path = ROOT / "config" / "abi_results" / f"{profile_id}.json"
    return json.loads(path.read_text(encoding="utf-8"))["mismatched"] if path.exists() else None


def required_overrides():
    return re.findall(r"^GUEST_OVERRIDE\(([0-9A-F]{8}), REQUIRED\)", OVERRIDES.read_text(encoding="utf-8"), re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--matrix", action="store_true")
    args = ap.parse_args()
    broken, matrices = [], {}
    print(f"{'version':10} {'lift':13} {'playable':8} {'proven':>6} {'candidate':>9} {'unresolved':>10}  required overrides")
    for v in versions():
        if v["lift"] == BASELINE_LIFT:
            print(f"{v['id']:10} {v['lift']:13} {'yes' if v['playable'] else 'no':8} {'all':>6} {0:>9} {0:>10}  all")
            continue
        profile, gmap = PROFILES_DIR / f"{v['id']}.json", GUEST_MAPS_DIR / f"{v['id']}.json"
        if not profile.exists() or not gmap.exists():
            print(f"{v['id']:10} {v['lift']:13} {'yes' if v['playable'] else 'no':8} (no profile or guest map)")
            if v["playable"]:
                broken.append(f"{v['id']} is playable without a profile and a guest map")
            continue
        addresses = json.loads(gmap.read_text(encoding="utf-8"))["addresses"]
        count = collections.Counter(r["class"] for r in addresses.values())
        proven = sum(count[c] for c in PROVEN)
        required = required_overrides()
        required_ok = sum(1 for va in required if addresses.get(va, {}).get("class") in PROVEN + ("absent",))
        unproven = offsets_unproven(v["id"])
        abi = abi_mismatches(v["id"])
        print(f"{v['id']:10} {v['lift']:13} {'yes' if v['playable'] else 'no':8} {proven:>6} "
              f"{count['candidate']:>9} {count['unresolved']:>10}  {required_ok}/{len(required)}  "
              f"offsets unproven {unproven}  abi mismatches {abi}")
        if v["playable"]:
            if count["candidate"] or count["unresolved"]:
                broken.append(f"{v['id']} is playable with {count['candidate']} candidate and {count['unresolved']} unresolved addresses")
            if required_ok != len(required):
                broken.append(f"{v['id']} is playable with {len(required) - required_ok} required overrides unresolved")
            if unproven or unproven is None:
                broken.append(f"{v['id']} is playable with {unproven} host structure offsets unproven")
            if abi != 0:
                broken.append(f"{v['id']} is playable without a clean ABI check ({abi})")
            if f'"{v["lift"]}"' not in (ROOT / "src" / "host" / "profiles" / "guest_profile_data.c").read_text(encoding="utf-8"):
                broken.append(f"{v['id']} is playable without its lift in src/host/profiles/guest_profile_data.c")
        meta = json.loads(profile.read_text(encoding="utf-8"))
        gen = ROOT / "src" / "recomp" / meta["gen_dir"] / "recomp_dispatch.c"
        if gen.exists():
            text = gen.read_text(encoding="utf-8")
            if f'"{meta["sha256"]}"' not in text or f'"{v["lift"]}"' not in text:
                broken.append(f"{meta['gen_dir']} was not generated from {v['id']}'s executable and lift")
        matrix = collections.defaultdict(collections.Counter)
        for va, r in addresses.items():
            matrix[feature_of(r["where"])][r["class"] if r["class"] in ("candidate", "unresolved") else "proven"] += 1
        matrices[v["id"]] = matrix
    if args.matrix:
        for vid, matrix in matrices.items():
            print(f"\n{vid}: guest addresses by feature (proven / candidate / unresolved)")
            for feature, _ in FEATURES + [("Other host code", ())]:
                c = matrix.get(feature)
                if c:
                    print(f"  {feature:28} {c['proven']:>4} / {c['candidate']:>3} / {c['unresolved']:>3}")
    for line in broken:
        print("BROKEN:", line)
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
