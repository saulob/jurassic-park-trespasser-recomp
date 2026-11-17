#!/usr/bin/env python3
"""Regenerate launcher/game_manifest_<profile>.inc from the player's own disc images (read only).

    py -3 tools/make_game_manifests.py --en-uk uk.iso --en-us us.iso
        --es es.iso --ptbr ptbr.iso --ru ru.iso

Only names, sizes and SHA-256 digests are written; no game content. The disc's original executables are import
inputs: one becomes guest.img (copied), the Russian release's one becomes text/strings.dat (its string resources, in the
format of src/common/jpt_string_table.h), and the manifests record the digests of those installed files and, for
strings.dat, of its source.
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.files = {}
        root = None
        for sector in range(16, 64):
            self.f.seek(sector * SECTOR)
            d = self.f.read(SECTOR)
            if d[0] == 255:
                break
            if d[0] == 1 and root is None:
                self.volume = d[40:72].decode("latin1").strip()
                root = (d[156:190], False)
            if d[0] == 2 and d[88:91] in (b"%/@", b"%/C", b"%/E"):
                root = (d[156:190], True)
        record, joliet = root
        self._walk(struct.unpack_from("<I", record, 2)[0], struct.unpack_from("<I", record, 10)[0], "", joliet, 0)

    def _walk(self, extent, size, parent, joliet, depth):
        self.f.seek(extent * SECTOR)
        data = self.f.read(size)
        o = 0
        while o < len(data):
            n = data[o]
            if n == 0:
                o = (o // SECTOR + 1) * SECTOR
                continue
            rec = data[o:o + n]
            o += n
            name = rec[33:33 + rec[32]]
            if name in (b"\0", b"\1"):
                continue
            name = (name.decode("utf-16-be") if joliet else name.decode("latin1")).split(";")[0]
            path = f"{parent}/{name}" if parent else name
            ext, sz = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            if rec[25] & 2:
                if depth < 16:
                    self._walk(ext, sz, path, joliet, depth + 1)
            else:
                self.files[path.lower()] = (path, ext, sz)

    def read(self, path):
        _, ext, size = self.files[path.lower()]
        self.f.seek(ext * SECTOR)
        return self.f.read(size)


class Cabinet:
    """InstallShield 5 cabinet with its own descriptor (setup/data1.cab of the Russian disc)."""

    def __init__(self, data):
        if data[:4] != b"ISc(":
            sys.exit("not an InstallShield cabinet")
        u32 = lambda o: struct.unpack_from("<I", data, o)[0]
        desc = u32(12)
        base = desc + u32(desc + 0x0C)
        dirs_n, files_n = u32(desc + 0x1C), u32(desc + 0x28)
        table = [u32(base + 4 * i) for i in range(dirs_n + files_n)]
        name = lambda o: data[o:data.index(b"\0", o)].decode("latin1")
        dirs = [name(base + table[i]) for i in range(dirs_n)]
        self.data, self.files = data, {}
        for i in range(files_n):
            p = base + table[dirs_n + i]
            d = dirs[u32(p + 4)]
            n = name(base + u32(p))
            path = f"{d}/{n}" if d else n
            self.files[path.lower()] = (path, struct.unpack_from("<H", data, p + 8)[0], u32(p + 10), u32(p + 14), u32(p + 0x26))

    def read(self, path):
        _, flags, size, packed, offset = self.files[path.lower()]
        raw = self.data[offset:offset + packed]
        if flags & 4:
            z = zlib.decompressobj(-15)
            raw = z.decompress(raw) + z.flush()
        if len(raw) != size:
            sys.exit(f"{path}: {len(raw)} bytes, expected {size}")
        return raw


def sha(data):
    return hashlib.sha256(data).hexdigest()


GUEST_IMAGE = "guest.img"
STRING_TABLE = "text/strings.dat"
STRING_ID_LIMIT = 65536  # JPT_L10N_ID_LIMIT


def pe_string_table(exe):
    """The string resources of a PE file as a jpt_string_table.h table: what launcher/game_install.cpp
    DeriveStringTable makes (block 1..LIMIT/16, 16 entries each, a unit below 0x100 is the byte, else '?')."""
    u16 = lambda o: struct.unpack_from("<H", exe, o)[0]
    u32 = lambda o: struct.unpack_from("<I", exe, o)[0]
    nt = u32(0x3C)
    if exe[nt:nt + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    sections, optional = u16(nt + 6), nt + 24
    section_table = optional + u16(nt + 20)
    rsrc_rva = u32(optional + (96 if u16(optional) == 0x10B else 112) + 16)

    def offset(rva):
        for i in range(sections):
            sec = section_table + 40 * i
            virtual, size, raw = u32(sec + 8), u32(sec + 16), u32(sec + 20)
            if u32(sec + 12) <= rva < u32(sec + 12) + max(virtual, size):
                return raw + rva - u32(sec + 12)
        sys.exit("resource outside the sections")

    base = offset(rsrc_rva)

    def entries(at):
        named, ids = u16(base + at + 12), u16(base + at + 14)
        return [(u32(base + at + 16 + 8 * i), u32(base + at + 20 + 8 * i)) for i in range(named + ids)][named:]

    blocks = {}
    for type_id, type_at in entries(0):
        if type_id != 6 or not type_at & 0x80000000:
            continue
        for block, name_at in entries(type_at & 0x7FFFFFFF):
            language = entries(name_at & 0x7FFFFFFF)[0][1]
            rva, size = u32(base + language), u32(base + language + 4)
            blocks[block] = exe[offset(rva):offset(rva) + size]
    strings = []
    for block in sorted(b for b in blocks if 1 <= b <= STRING_ID_LIMIT // 16):
        data, at = blocks[block], 0
        for i in range(16):
            if at + 2 > len(data):
                break
            units = struct.unpack_from("<H", data, at)[0]
            at += 2
            if units * 2 > len(data) - at:
                break
            if units:
                text = bytes(u if u < 0x100 else 63 for u in struct.unpack_from(f"<{units}H", data, at))
                strings.append(((block - 1) * 16 + i, text))
            at += units * 2
    if not strings:
        sys.exit("no string resources")
    table = struct.pack("<III", 0x5354504A, 1, len(strings))
    for string_id, text in strings:
        table += struct.pack("<II", string_id, len(text)) + text
    return table


RUNTIME = [("setup/tpassp6.exe", GUEST_IMAGE), ("setup/msvcirt.dll", "MSVCIRT.DLL"),
           ("setup/smackw32.dll", "SMACKW32.DLL"), ("setup/processor.dll", "Processor.dll"), ("readme.txt", "Readme.txt")]
# Files mastered onto a disc that the game never reads.
NOT_GAME_DATA = {"data/error.log"}


def disc_tree(iso, runtime):
    rows = []
    for src, dst in runtime:
        rows.append(("iso", iso.files[src.lower()][0], dst, iso.read(src)))
    for key, (path, _, _) in sorted(iso.files.items()):
        if key.startswith("data/") and key not in NOT_GAME_DATA:
            rows.append(("iso", path, path, iso.read(path)))
    return rows


def write(profile, rows, repaired=()):
    out = ROOT / "launcher" / f"game_manifest_{profile}.inc"
    lines = [f"// {profile}: every installed file, where it comes from on the disc, its size and SHA-256 "
             "(tools/make_game_manifests.py)."]
    for kind, src, dst, digest, size, *source in rows:
        kind_name = "kCabinet" if kind == "cab" else "kDisc"
        tail = ""
        if dst == GUEST_IMAGE:
            tail = ", FileRole::kGuestImage"
        elif source:
            tail = f', FileRole::kStringTable, {source[1]}, "{source[0]}"'
        lines.append(f'    {{SourceKind::{kind_name}, "{src}", "{dst}", {size}, "{digest}"{tail}}},')
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{out.name}: {len(rows)} files, {sum(r[4] for r in rows)} bytes" +
          (f", {len(repaired)} recorded with the reference digest: {', '.join(repaired)}" if repaired else ""))


def hashed(rows):
    return [(k, s, d, sha(b), len(b)) for k, s, d, b in rows]


def derived(kind, src, dst, source_bytes):
    """A row whose installed file is made from the source: its own digest and size, then the source's."""
    table = pe_string_table(source_bytes)
    return (kind, src, dst, sha(table), len(table), sha(source_bytes), len(source_bytes))


def check_identity(profiles):
    """Each profile's identity file (launcher/game_files.cpp) is a disc data file of that profile and no other
    profile has a file with that digest."""
    text = (ROOT / "launcher" / "game_files.cpp").read_text(encoding="utf-8")
    found = re.findall(r'\{&jpt_game_versions\[(\d)\],\s*"([^"]+)",\s*"([0-9a-f]{64})"', text)
    names = ["en_116", "ru_116", "es_117", "ptbr_117"]
    if len(found) != len(names):
        sys.exit("game_files.cpp: identity files not found")
    for index, path, digest in found:
        name = names[int(index)]
        mine = [r for r in profiles[name] if r[0] == "iso" and r[1].lower() == path.lower() and r[3] == digest]
        if not mine or path.lower().endswith(".exe"):
            sys.exit(f"{name}: identity file {path} is not a data file of the profile with that digest")
        for other, rows in profiles.items():
            if other != name and any(r[3] == digest for r in rows):
                sys.exit(f"{name}: identity file {path} has the same digest in {other}")
        print(f"{name}: identity {path} ({mine[0][4]} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("en-uk", "en-us", "es", "ptbr", "ru"):
        ap.add_argument(f"--{name}", type=Path, required=True)
    a = ap.parse_args()

    uk, us = Iso(a.en_uk), Iso(a.en_us)
    en = hashed(disc_tree(uk, RUNTIME))
    us_rows = hashed(disc_tree(us, RUNTIME))
    if [r[1:] for r in en] != [r[1:] for r in us_rows] and sorted(r[3] for r in en) != sorted(r[3] for r in us_rows):
        sys.exit("the US and UK discs no longer install the same files")
    profiles = {"en_116": en}
    write("en_116", en)
    en_by_name = {r[2].lower(): r for r in en}

    profiles["es_117"] = hashed(disc_tree(Iso(a.es), RUNTIME))
    write("es_117", profiles["es_117"])

    # Five level files of the Brazilian image differ from every other disc by one flipped bit at byte 470 of a
    # sector: a damaged copy. The manifest keeps the pressed digest, so the launcher detects and repairs them.
    pt = hashed(disc_tree(Iso(a.ptbr), RUNTIME))
    repaired = []
    for i, (k, s, d, digest, size) in enumerate(pt):
        ref = en_by_name.get(d.lower())
        if d.lower().endswith((".spz", ".pid")) and ref and ref[4] == size and ref[3] != digest:
            pt[i] = (k, s, d, ref[3], size)
            repaired.append(d)
    profiles["ptbr_117"] = pt
    write("ptbr_117", pt, repaired)

    ru = Iso(a.ru)
    cab = Cabinet(ru.read("setup/data1.cab"))
    rows = [("cab", cab.files["tpassp6.exe"][0], GUEST_IMAGE, cab.read("tpassp6.exe")),
            ("iso", ru.files["setup/msvcirt.dll"][0], "MSVCIRT.DLL", ru.read("setup/msvcirt.dll")),
            ("iso", ru.files["setup/smackw32.dll"][0], "SMACKW32.DLL", ru.read("setup/smackw32.dll")),
            ("iso", ru.files["setup/proces~1.dll"][0], "Processor.dll", ru.read("setup/proces~1.dll")),
            ("iso", ru.files["readme.txt"][0], "Readme.txt", ru.read("readme.txt")),
            ("iso", ru.files["tresp.ttf"][0], "text/tresp.ttf", ru.read("tresp.ttf"))]
    strings_row = derived("iso", ru.files["setup/trespass.exe"][0], STRING_TABLE, ru.read("setup/trespass.exe"))
    en_by_digest = {r[3]: r for r in en}
    for key, (path, _, _) in sorted(ru.files.items()):
        if not key.startswith("data/"):
            continue
        body = ru.read(path)
        name = path
        if "~" in key:  # 8.3 names on this disc: the English disc's long name for the same bytes or the same video
            same = en_by_digest.get(sha(body))
            name = same[2] if same else {"data/menu/tpassi~1.smk": "data/menu/tpassintro.smk"}[key]
        elif key in en_by_name:
            name = en_by_name[key][2]
        rows.append(("iso", path, name, body))
    for key, (path, *_) in sorted(cab.files.items()):
        if key.startswith("menu/"):
            name = "data/" + path
            name = en_by_name.get(name.lower(), (0, 0, name))[2]
            rows.append(("cab", path, name, cab.read(path)))
    ru_rows = hashed(rows)
    ru_rows.insert(5, strings_row)
    profiles["ru_116"] = ru_rows
    write("ru_116", ru_rows)
    check_identity(profiles)


if __name__ == "__main__":
    main()
