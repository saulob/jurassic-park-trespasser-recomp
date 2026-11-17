#!/usr/bin/env python3
"""The layout of the guest structures whose fields are not where 1.0.116 has them, as each lift's executable lays them out.

    py -3 tools/gen_guest_layout.py

config/layout_descriptors.json names each such field (GLAYOUT_<STRUCT>_<FIELD>) with its 1.0.116 value. For every profile
the value is what config/host_offsets.json proves for the host defines that use the descriptor: the 1.0.116 value when the
field did not move, the new value when it did. tools/gen_guest_va.py writes them into the host's tables
(src/host/profiles); this prints each lift's values and exits 1 when the proofs of a descriptor disagree.
"""
import argparse
import json
import sys

from common import CONFIG_DIR

DESCRIPTORS = CONFIG_DIR / "layout_descriptors.json"
MANIFEST = CONFIG_DIR / "host_offsets.json"

PROFILE_LIFT = {"lift116": None, "lift_es117": "es-117", "lift_ptbr117": "ptbr-117"}


def descriptors():
    return json.loads(DESCRIPTORS.read_text(encoding="utf-8"))["descriptors"]


def values(profile):
    """descriptor name -> value in this profile, or an error when its host defines disagree."""
    rows = json.loads(MANIFEST.read_text(encoding="utf-8"))["offsets"]
    out, errors = {}, []
    for d in descriptors():
        seen = set()
        for row in rows:
            if row.get("descriptor") != f"GLAYOUT_{d['name']}":
                continue
            entry = row.get(profile)
            if entry and entry["status"] == "gated":
                continue
            seen.add(entry["value"] if entry and entry["status"] == "moved" else d["value"])
            if entry and entry["status"] not in ("unmoved", "moved"):
                errors.append(f"{row['file']} {row['name']}: {entry['status']} in {profile}")
        if len(seen) > 1:
            errors.append(f"GLAYOUT_{d['name']}: its host defines disagree in {profile}: {sorted(seen)}")
        out[d["name"]] = (seen.pop() if seen else d["value"])
    return out, errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args(argv)
    bad = 0
    for lift, profile in PROFILE_LIFT.items():
        if profile is None:
            found, errors = {d["name"]: d["value"] for d in descriptors()}, []
        else:
            found, errors = values(profile)
        for e in errors:
            print(f"{lift}: {e}")
            bad = 1
        print(lift, ", ".join(f"GLAYOUT_{name} = {value}" for name, value in found.items()))
    return bad


if __name__ == "__main__":
    sys.exit(main())
