#!/usr/bin/env python3
"""Maintainers only: lay out the original files of an English disc in game/ (development inputs for tools/generate-lift.ps1
and the retail checks of the tests). The build, the launcher and the game never use them: players import from the launcher.

The image is opened read-only. Nothing extracted is tracked by Git.

    py -3 tools/extract_game.py --iso "D:\\images\\trespasser.iso"
    py -3 tools/extract_game.py              # iso from TRESPASSER_ISO or config/local.json
    py -3 tools/extract_game.py --binaries-only
"""
import argparse
import hashlib
import os
import sys
from pathlib import Path

from common import GAME_DIR, TARGET, local_config
from iso9660 import Iso

RUNTIME_FILES = {
    TARGET["disc_path"]: TARGET["installed_name"],
    "setup/msvcirt.dll": "MSVCIRT.DLL",
    "setup/smackw32.dll": "SMACKW32.DLL",
    "setup/processor.dll": "Processor.dll",
    "readme.txt": "Readme.txt",
}
DATA_PREFIX = "data/"


def resolve_iso(arg):
    path = arg or os.environ.get("TRESPASSER_ISO") or local_config().get("iso")
    if not path:
        sys.exit("no disc image given: use --iso, TRESPASSER_ISO or config/local.json")
    path = Path(path)
    if not path.is_file():
        sys.exit(f"disc image not found: {path}")
    return path


def plan(entries, binaries_only):
    for entry in entries:
        key = entry.path.lower()
        if key in RUNTIME_FILES:
            yield entry, GAME_DIR / RUNTIME_FILES[key]
        elif key.startswith(DATA_PREFIX) and not binaries_only:
            yield entry, GAME_DIR / entry.path


def extract(iso, entry, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    partial = dest.with_name(dest.name + ".part")
    with open(partial, "wb") as out:
        iso.copy_to(entry, out, digest)
    os.replace(partial, dest)
    return digest.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso")
    ap.add_argument("--binaries-only", action="store_true", help="skip data/ (enough for analysis and lifting)")
    ap.add_argument("--force", action="store_true", help="re-extract files that already exist")
    args = ap.parse_args()

    iso_path = resolve_iso(args.iso)
    copied = skipped = 0
    target_hash = None
    with Iso(iso_path) as iso:
        todo = sorted(plan(iso.walk(), args.binaries_only), key=lambda item: item[0].path.lower())
        found = {entry.path.lower() for entry, _ in todo}
        missing = sorted(set(RUNTIME_FILES) - found)
        if missing:
            sys.exit(f"{iso_path} is not the expected disc: missing {', '.join(missing)}")
        for entry, dest in todo:
            is_target = entry.path.lower() == TARGET["disc_path"]
            if dest.exists() and dest.stat().st_size == entry.size and not args.force and not is_target:
                skipped += 1
                continue
            digest = extract(iso, entry, dest)
            copied += 1
            if is_target:
                target_hash = digest

    print(f"image    {iso_path} (joliet names)")
    print(f"install  {GAME_DIR}")
    print(f"files    {copied} extracted, {skipped} already present")
    print(f"target   {TARGET['disc_path']} -> {TARGET['installed_name']}")
    print(f"sha256   {target_hash}")
    if target_hash != TARGET["sha256"]:
        sys.exit(f"MISMATCH: expected {TARGET['sha256']} -- this disc carries a different build")
    print("         matches config/target.json")


if __name__ == "__main__":
    main()
