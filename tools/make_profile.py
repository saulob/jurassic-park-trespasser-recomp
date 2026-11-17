#!/usr/bin/env python3
"""Writes config/profiles/<id>.json, the facts the lift of a build needs, from the build's own executable.

    py -3 tools/make_profile.py es-117 --exe <trespass.exe> --gen-dir gen_es117 --lift lift_es117

The decoder fixups (undocumented x87 encodings capstone cannot decode) are the 1.0.116 ones moved through the
instruction alignment of guest_map.py and checked against the bytes found there. Only facts of the executable are
stored; nothing of the game's content.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from common import CONFIG_DIR, PROFILES_DIR, ROOT, TARGET, sha256_of
import guest_map


def pe_facts(exe):
    data, base, size, sections = guest_map.read_pe(exe)
    o = struct.unpack_from("<I", data, 0x3C)[0]
    timestamp = struct.unpack_from("<I", data, o + 8)[0]
    entry = struct.unpack_from("<I", data, o + 24 + 16)[0]
    selfmod = next(s for s in sections if s[0] == "SelfMod")
    text = next(s for s in sections if s[0] == ".text")
    return {"size": len(data), "image_base": f"0x{base:08X}", "size_of_image": f"0x{size:08X}",
            "entry_point": f"0x{base + entry:08X}", "timestamp": timestamp,
            "selfmod": {"start": f"0x{selfmod[1]:08X}", "size": f"0x{selfmod[2]:08X}"},
            "text": (text[1], text[2])}


def moved_fixups(engine, other, mapper):
    fixups = json.loads((CONFIG_DIR / "decoder_fixups.json").read_text(encoding="utf-8"))["fixups"]
    out = []
    for fix in fixups:
        va = int(fix["va"], 16)
        fa = engine.function_of(va) or max(k for k in engine.starts if k <= va)
        fb = mapper.match.get(fa)
        if fb is None:
            sys.exit(f"decoder fixup {fix['va']}: its function has no counterpart")
        moved = fb + (va - fa)
        o = other.offset(moved)
        if other.data[o:o + len(bytes.fromhex(fix["expect"]))] != bytes.fromhex(fix["expect"]):
            sys.exit(f"decoder fixup {fix['va']}: the bytes at 0x{moved:08X} are not {fix['expect']}")
        out.append(dict(fix, va=f"0x{moved:08X}"))
    return sorted(out, key=lambda f: f["va"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--gen-dir", required=True)
    ap.add_argument("--lift", required=True)
    ap.add_argument("--file-version", default="1.0.117")
    args = ap.parse_args()
    facts = pe_facts(args.exe)
    engine = guest_map.load_image(ROOT / "game" / TARGET["installed_name"])
    other = guest_map.load_image(args.exe)
    mapper = guest_map.Mapper(engine, other).run()
    import time
    profile = {
        "id": args.profile, "lift": args.lift, "gen_dir": args.gen_dir, "sha256": sha256_of(args.exe),
        "size": facts["size"], "file_version": args.file_version,
        "pe_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(facts["timestamp"])),
        "image_base": facts["image_base"], "size_of_image": facts["size_of_image"], "entry_point": facts["entry_point"],
        "selfmod": facts["selfmod"], "decoder_fixups": moved_fixups(engine, other, mapper), "extra_entries": [],
    }
    PROFILES_DIR.mkdir(parents=True, exist_ok=True)
    out = PROFILES_DIR / f"{args.profile}.json"
    out.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{out}: {profile['sha256'][:16]}..., entry {profile['entry_point']}, "
          f"{len(profile['decoder_fixups'])} decoder fixups")


if __name__ == "__main__":
    main()
