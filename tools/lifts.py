#!/usr/bin/env python3
"""The lifted builds the port carries, as project artifacts: lifts/<lift>.tar.xz and lifts/manifest.json.

    py -3 tools/lifts.py verify [--content]   the archives match the manifest and the tool/profile facts; --content also
                                              checks each archive's recomp_dispatch.c metadata (no executable is opened)
    py -3 tools/lifts.py extract [lift ...]   unpack into src/recomp/<gen dir> (what CMake does at configure time)
    py -3 tools/lifts.py pack <lift>          archive src/recomp/<gen dir> (made by tools/run_lift.py, a maintainer tool)

An archive holds the files of one lift's generated folder, sorted, with fixed owners and times, so packing the same
sources gives the same bytes. The manifest records the archive's SHA-256 and a digest of its content, the profile and
the SHA-256 of the build the lift was generated from (development metadata only), the generator format and counts.
"""
import argparse
import hashlib
import io
import json
import lzma
import re
import shutil
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIFTS_DIR = ROOT / "lifts"
MANIFEST = LIFTS_DIR / "manifest.json"
META_HEADER = ROOT / "src" / "common" / "jpt_lift_meta.h"
STAMP = ".archive_sha256"
LIFTS = (  # lift id, generated folder, profile the lift is generated from
    ("lift116", "gen", "en-116"),
    ("lift_es117", "gen_es117", "es-117"),
    ("lift_ptbr117", "gen_ptbr117", "ptbr-117"),
)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8")) if MANIFEST.exists() else {"lifts": {}}


def lift_format():
    match = re.search(r"^#define JPT_LIFT_FORMAT (\d+)u", META_HEADER.read_text(encoding="utf-8"), re.M)
    if not match:
        sys.exit(f"{META_HEADER}: JPT_LIFT_FORMAT is not defined")
    return int(match.group(1))


def profile_sha(profile):
    path = ROOT / "config" / ("target.json" if profile == "en-116" else f"profiles/{profile}.json")
    return json.loads(path.read_text(encoding="utf-8"))["sha256"]


def content_digest(files):
    """files: sorted (name, bytes) pairs."""
    digest = hashlib.sha256()
    for name, data in files:
        digest.update(f"{name}\0{len(data)}\0".encode())
        digest.update(data)
    return digest.hexdigest()


def archive_files(archive):
    with tarfile.open(archive, "r:xz") as tar:
        return [(m.name, tar.extractfile(m).read()) for m in tar.getmembers() if m.isfile()]


def pack(lift):
    entry = next((e for e in LIFTS if e[0] == lift), None)
    if not entry:
        sys.exit(f"unknown lift {lift}: {[e[0] for e in LIFTS]}")
    _, gen_dir, profile = entry
    folder = ROOT / "src" / "recomp" / gen_dir
    names = sorted(p.name for p in folder.glob("recomp_*") if p.is_file())
    if "recomp_dispatch.c" not in names or "recomp_funcs.h" not in names:
        sys.exit(f"{folder} is not a complete lift: run tools/run_lift.py --profile {profile}")
    files = [(name, (folder / name).read_bytes()) for name in names]
    dispatch = dict(files)["recomp_dispatch.c"].decode("utf-8")
    meta = re.search(r'jpt_lift_meta_t \w+ = \{\s*"([^"]+)", "([^"]+)", "([0-9a-f]{64})",\s*(\d+)u, (\d+)u,', dispatch)
    if not meta or meta.group(1) != lift or int(meta.group(4)) != lift_format():
        sys.exit(f"{folder}: its metadata is not that of {lift} in format {lift_format()}: regenerate it")
    if meta.group(3) != profile_sha(meta.group(2)):
        sys.exit(f"{folder}: generated from a build other than the one config says for {meta.group(2)}")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:xz", format=tarfile.PAX_FORMAT, preset=9 | lzma.PRESET_EXTREME) as tar:
        for name, data in files:
            info = tarfile.TarInfo(name)
            info.size, info.mtime, info.mode, info.uid, info.gid, info.uname, info.gname = len(data), 0, 0o644, 0, 0, "", ""
            tar.addfile(info, io.BytesIO(data))
    LIFTS_DIR.mkdir(exist_ok=True)
    archive = LIFTS_DIR / f"{lift}.tar.xz"
    archive.write_bytes(buf.getvalue())
    manifest = read_manifest()
    manifest["lifts"][lift] = {
        "profile": meta.group(2), "gen_dir": gen_dir, "archive": archive.name, "archive_sha256": sha256_file(archive),
        "content_sha256": content_digest(files), "files": len(files), "functions": int(meta.group(5)),
        "format": int(meta.group(4)), "source_profile": meta.group(2), "source_sha256": meta.group(3)}
    manifest["lifts"] = dict(sorted(manifest["lifts"].items()))
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{archive.relative_to(ROOT)}: {len(files)} files, {archive.stat().st_size >> 10} KiB, "
          f"{manifest['lifts'][lift]['functions']} functions")


def problems(check_content):
    manifest = read_manifest()["lifts"]
    out = []
    for lift, gen_dir, profile in LIFTS:
        entry = manifest.get(lift)
        if not entry:
            out.append(f"{lift}: not in {MANIFEST.relative_to(ROOT)}")
            continue
        archive = LIFTS_DIR / entry["archive"]
        if not archive.exists():
            out.append(f"{lift}: {archive.relative_to(ROOT)} is missing")
            continue
        if sha256_file(archive) != entry["archive_sha256"]:
            out.append(f"{lift}: {archive.name} does not match the manifest")
        if entry["format"] != lift_format():
            out.append(f"{lift}: made by generator format {entry['format']}, this tree expects {lift_format()}")
        if entry["gen_dir"] != gen_dir or entry["profile"] != profile or entry["source_sha256"] != profile_sha(profile):
            out.append(f"{lift}: the manifest's profile or source build is not the one config/ names")
        if check_content:
            files = archive_files(archive)
            if content_digest(files) != entry["content_sha256"] or len(files) != entry["files"]:
                out.append(f"{lift}: the archive's content does not match the manifest")
            dispatch = dict(files).get("recomp_dispatch.c", b"").decode("utf-8", "replace")
            if f'"{lift}", "{profile}", "{entry["source_sha256"]}"' not in dispatch:
                out.append(f"{lift}: recomp_dispatch.c carries other metadata than the manifest")
    return out


def extract(lifts):
    manifest = read_manifest()["lifts"]
    for lift, gen_dir, _ in LIFTS:
        if lifts and lift not in lifts:
            continue
        entry = manifest[lift]
        dest = ROOT / "src" / "recomp" / gen_dir
        if (dest / STAMP).exists() and (dest / STAMP).read_text().strip() == entry["archive_sha256"]:
            print(f"{lift}: current")
            continue
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        for name, data in archive_files(LIFTS_DIR / entry["archive"]):
            (dest / name).write_bytes(data)
        (dest / STAMP).write_text(entry["archive_sha256"], encoding="ascii")
        print(f"{lift}: extracted {entry['files']} files into {dest.relative_to(ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("verify", "extract", "pack"))
    ap.add_argument("lifts", nargs="*")
    ap.add_argument("--content", action="store_true")
    args = ap.parse_args()
    if args.command == "pack":
        if len(args.lifts) != 1:
            sys.exit("pack takes one lift")
        pack(args.lifts[0])
        return 0
    if args.command == "extract":
        extract(args.lifts)
        return 0
    found = problems(args.content)
    for line in found:
        print("PROBLEM:", line)
    print(f"{len(LIFTS)} lifts, {len(found)} problem(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
