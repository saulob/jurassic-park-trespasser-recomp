#!/usr/bin/env python3
"""The lifted SelfMod section (recomp_selfmod.c) against the lift's own dispatch and, when the executable is present, against
its bytes. Headless; reads generated C only: py -3 -m unittest discover -s tests -p "test_selfmod_lift.py" -v

No guest byte is executed anywhere: the section is lifted C, and the 44 fields its routines patch in their prologues are
ordinary guest memory.
"""
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "src" / "recomp" / "gen"
EXE = ROOT / "game" / "trespass.exe"
TABLE = re.compile(r"\{ 0x([0-9A-F]{8})u, (\w+) \}")
CALL_TARGET = re.compile(r"RECOMP_(?:ICALL|ITAIL|CALL)\(0x([0-9A-F]{8})u\)")
DIRECT_CALL = re.compile(r"RECOMP_CALL\((\w+)\)")
META = re.compile(r'"lift116", "[^"]+", "[0-9a-f]{64}",\s*(\d+)u, (\d+)u, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u,\s*'
                  r"0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u")


def selfmod_source():
    path = GEN / "recomp_selfmod.c"
    return path.read_text(encoding="utf-8") if path.is_file() else None


def selfmod_entries(text, lift="lift116"):
    start = text.index(f"{lift}_selfmod_dispatch_table[]")
    return [(int(a, 16), name) for a, name in TABLE.findall(text[start:])]


def lift_meta():
    m = META.search((GEN / "recomp_dispatch.c").read_text(encoding="utf-8"))
    return {"entry": int(m.group(3), 16), "selfmod": (int(m.group(7), 16), int(m.group(8), 16))}


def main_entries():
    text = (GEN / "recomp_dispatch.c").read_text(encoding="utf-8")
    return {int(a, 16) for a, _ in TABLE.findall(text)}


def main_sources():
    return sorted(p for p in GEN.glob("recomp_0*.c"))


def exe_routine_starts():
    """The 16-byte-aligned `push ebp; mov ebp, esp` starts of the SelfMod section, read with the standard library only."""
    data = EXE.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    base, = struct.unpack_from("<I", data, pe + 24 + 28)
    for i in range(nsec):
        off = pe + 24 + opt_size + 40 * i
        if data[off:off + 8].rstrip(b"\0") == b"SelfMod":
            vsize, va, _, raw = struct.unpack_from("<IIII", data, off + 8)
            section = data[raw:raw + vsize]
            lo = base + va
            return lo, lo + vsize, [lo + k for k in range(0, vsize - 3, 16) if section[k:k + 3] == b"\x55\x8b\xec"]
    raise AssertionError("no SelfMod section")


@unittest.skipUnless(selfmod_source() is not None, "DATA UNAVAILABLE: the lift is not unpacked (configure the build)")
class SelfModLift(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = selfmod_source()
        cls.entries = selfmod_entries(cls.text)
        cls.meta = lift_meta()
        cls.lo, cls.hi = cls.meta["selfmod"]

    def test_range(self):
        self.assertEqual((self.lo, self.hi), (0x0063B000, 0x00644B75))
        for va, _ in self.entries:
            self.assertTrue(self.lo <= va < self.hi, f"0x{va:08X}")

    def test_table_is_sorted_unique_and_defined(self):
        vas = [va for va, _ in self.entries]
        self.assertEqual(vas, sorted(set(vas)))
        self.assertEqual(len(vas), 46)
        defined = set(re.findall(r"^void (lift116_sub_[0-9A-F]{8})\(void\) \{", self.text, re.M))
        self.assertEqual(defined, {name for _, name in self.entries})
        for va, name in self.entries:
            self.assertEqual(name, f"lift116_sub_{va:08X}")
        self.assertIn("lift116_selfmod_dispatch_count = 46u;", self.text)

    def test_every_lifted_caller_target_has_a_dispatch_entry(self):
        have = {va for va, _ in self.entries}
        targets = set()
        for path in main_sources():
            for m in CALL_TARGET.finditer(path.read_text(encoding="utf-8", errors="replace")):
                va = int(m.group(1), 16)
                if self.lo <= va < self.hi:
                    targets.add(va)
        self.assertGreater(len(targets), 20)
        self.assertEqual(sorted(targets - have), [], "lifted code enters SelfMod at addresses with no dispatch entry")

    def test_calls_out_of_the_range_reach_lifted_text(self):
        main = main_entries()
        own = {va for va, _ in self.entries}
        called = {int(m.group(1), 16) for m in re.finditer(r"RECOMP_CALL\(lift116_sub_([0-9A-F]{8})\)", self.text)}
        outside = called - own
        self.assertEqual(sorted(outside - main), [])
        self.assertIn(0x00626B57, outside)
        constant = {int(m.group(1), 16) for m in CALL_TARGET.finditer(self.text)}
        self.assertEqual(sorted(a for a in constant if not self.lo <= a < self.hi and a not in main), [])

    def test_nothing_is_left_unlifted(self):
        for marker in ("RECOMP_NOT_LIFTED", "UNIMPLEMENTED", "RECOMP_ITAIL"):
            self.assertNotIn(marker, self.text, marker)

    def test_patch_sites_are_guest_memory_loads(self):
        loads = set(re.findall(r"MEM32\(0x(006[0-9A-F]{5})u\)", self.text))
        sites = {int(a, 16) for a in loads if self.lo <= int(a, 16) < self.hi}
        self.assertEqual(len(sites), 44)
        self.assertNotRegex(self.text, r"0xDEADBEEFu|0xDEADC0DEu")

    def test_the_selfmod_functions_are_not_in_the_main_table(self):
        main = main_entries()
        self.assertFalse(any(self.lo <= va < self.hi for va in main))

    @unittest.skipUnless(EXE.is_file(), "DATA UNAVAILABLE: game/trespass.exe")
    def test_entries_are_the_executables_routine_starts(self):
        lo, hi, starts = exe_routine_starts()
        self.assertEqual((lo, hi), (self.lo, self.hi))
        self.assertEqual(starts, [va for va, _ in self.entries])


class EveryLiftCarriesItsSelfMod(unittest.TestCase):
    LIFTS = (
        ("lift_es117", "gen_es117", 0x0063D000, 0x00646B75),
        ("lift_ptbr117", "gen_ptbr117", 0x0063F000, 0x00648B75),
    )

    def test_each_117_lift_has_its_own_section(self):
        for lift, folder, lo, hi in self.LIFTS:
            with self.subTest(lift=lift):
                path = ROOT / "src" / "recomp" / folder / "recomp_selfmod.c"
                if not path.is_file():
                    self.skipTest(f"{path.relative_to(ROOT)} is not unpacked (configure the build)")
                text = path.read_text(encoding="utf-8")
                entries = selfmod_entries(text, lift)
                self.assertEqual(len(entries), 46)
                self.assertEqual([va for va, _ in entries], sorted({va for va, _ in entries}))
                self.assertTrue(all(lo <= va < hi for va, _ in entries))
                self.assertTrue(all(name == f"{lift}_sub_{va:08X}" for va, name in entries))
                self.assertIn(f"{lift}_selfmod_dispatch_count = 46u;", text)
                self.assertNotIn("RECOMP_NOT_LIFTED", text)
                sites = {int(a, 16) for a in re.findall(r"MEM32\(0x(006[0-9A-F]{5})u\)", text) if lo <= int(a, 16) < hi}
                self.assertEqual(len(sites), 44)
                folder_path = ROOT / "src" / "recomp" / folder
                main = {int(a, 16) for a, _ in TABLE.findall((folder_path / "recomp_dispatch.c").read_text(encoding="utf-8"))}
                called = {int(a, 16) for a in re.findall(r"RECOMP_CALL\(\w+_sub_([0-9A-F]{8})\)", text)}
                self.assertEqual(sorted(a for a in called if not lo <= a < hi and a not in main), [])


if __name__ == "__main__":
    unittest.main()
