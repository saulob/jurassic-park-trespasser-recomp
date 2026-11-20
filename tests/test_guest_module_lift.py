#!/usr/bin/env python3
"""The lifted guest DLL (MSVCIRT) against its config, the manifest and, when the DLL is present, its bytes. Headless; reads
generated C and the PE with the standard library only: py -3 -m unittest discover -s tests -p "test_guest_module_lift.py" -v

The DLL is never executed; what the lift carries is C.
"""
import hashlib
import json
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "src" / "recomp" / "gen"
CONFIG = json.loads((ROOT / "config" / "guest_modules.json").read_text(encoding="utf-8"))["modules"]["msvcirt"]
DLL = next((p for p in (ROOT / "game" / "versions" / "en-116" / "MSVCIRT.DLL", ROOT / "game" / "MSVCIRT.DLL") if p.is_file()), None)
TABLE = re.compile(r"\{ 0x([0-9A-F]{8})u, (msvcirt_sub_[0-9A-F]{8}) \}")
TARGET = re.compile(r"RECOMP_(?:ICALL|ITAIL)\(0x([0-9A-F]{8})u\)")
DEFINED = re.compile(r"^void (\w+)\(void\) \{", re.M)


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def parse_dll(raw):
    pe = u32(raw, 0x3C)
    nsec, = struct.unpack_from("<H", raw, pe + 6)
    opt = pe + 24
    opt_size, = struct.unpack_from("<H", raw, pe + 20)
    size_of_image, size_of_headers = u32(raw, opt + 56), u32(raw, opt + 60)
    mem = bytearray(size_of_image)
    mem[:size_of_headers] = raw[:size_of_headers]
    sections = {}
    for i in range(nsec):
        off = opt + opt_size + 40 * i
        name = raw[off:off + 8].rstrip(b"\0").decode()
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", raw, off + 8)
        mem[va:va + min(rsize, vsize or rsize)] = raw[rptr:rptr + min(rsize, vsize or rsize)]
        sections[name] = (va, vsize or rsize)
    return mem, sections, {"base": u32(raw, opt + 28), "size": size_of_image, "entry": u32(raw, opt + 16),
                           "export": (u32(raw, opt + 96), u32(raw, opt + 100)), "reloc": (u32(raw, opt + 96 + 40), u32(raw, opt + 96 + 44)),
                           "tls": u32(raw, opt + 96 + 72)}


def exports(mem, directory):
    d = directory[0]
    base_ord, nfuncs, nnames = u32(mem, d + 16), u32(mem, d + 20), u32(mem, d + 24)
    funcs, names, ords = u32(mem, d + 28), u32(mem, d + 32), u32(mem, d + 36)
    out = []
    for i in range(nnames):
        name_rva = u32(mem, names + 4 * i)
        ordinal_index, = struct.unpack_from("<H", mem, ords + 2 * i)
        out.append((base_ord + ordinal_index, bytes(mem[name_rva:mem.index(b"\0", name_rva)]).decode("latin1"), u32(mem, funcs + 4 * ordinal_index)))
    return sorted(out), nfuncs


def relocations(mem, directory):
    out, off, end = [], directory[0], directory[0] + directory[1]
    while off < end:
        page, block = u32(mem, off), u32(mem, off + 4)
        for i in range((block - 8) // 2):
            entry, = struct.unpack_from("<H", mem, off + 8 + 2 * i)
            if entry >> 12 == 3:
                out.append(page + (entry & 0xFFF))
        off += block
    return out


@unittest.skipUnless((GEN / "recomp_msvcirt.c").is_file(), "DATA UNAVAILABLE: the lift is not unpacked (configure the build)")
class GuestModuleLift(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = (GEN / "recomp_msvcirt.c").read_text(encoding="utf-8")
        cls.facts = json.loads((GEN / "recomp_msvcirt.json").read_text(encoding="utf-8"))
        cls.entries = [(int(a, 16), n) for a, n in TABLE.findall(cls.text)]

    def test_facts_are_the_configs(self):
        f = self.facts
        for key in ("sha256", "export_fingerprint"):
            self.assertEqual(f[key], CONFIG[key])
        self.assertEqual((f["image_base"], f["image_size"], f["entry"]), (CONFIG["image_base"], CONFIG["image_size"], "0x780A1000"))
        self.assertEqual((f["exports"], f["code_exports"], f["data_exports"]), (CONFIG["exports"], CONFIG["code_exports"], CONFIG["data_exports"]))
        self.assertFalse(f["tls"])
        self.assertIn(f'"{CONFIG["sha256"]}"', self.text)

    def test_table_is_sorted_unique_defined_and_prefixed(self):
        vas = [va for va, _ in self.entries]
        self.assertEqual(vas, sorted(set(vas)))
        self.assertEqual(len(vas), self.facts["entries"])
        lo, hi = (int(x, 16) for x in self.facts["text"])
        self.assertTrue(all(lo <= va < hi for va in vas))
        defined = DEFINED.findall(self.text)
        self.assertEqual(len(defined), len(set(defined)))
        self.assertEqual(set(defined), {n for _, n in self.entries})
        self.assertTrue(all(n.startswith("msvcirt_sub_") for n in defined))
        for va, name in self.entries:
            self.assertEqual(name, f"msvcirt_sub_{va:08X}")

    def test_no_symbol_collides_with_the_executable_lift(self):
        main = (GEN / "recomp_dispatch.c").read_text(encoding="utf-8")
        self.assertFalse({n for _, n in self.entries} & set(re.findall(r"\b(\w+_sub_[0-9A-F]{8})\b", main)))

    def test_every_constant_jump_and_call_target_is_a_lifted_entry(self):
        have = {va for va, _ in self.entries}
        self.assertEqual(sorted({int(m.group(1), 16) for m in TARGET.finditer(self.text)} - have), [])
        for marker in ("UNIMPLEMENTED", "RECOMP_NOT_LIFTED", "__asm", "asm("):
            self.assertNotIn(marker, self.text)

    def test_manifest_records_the_module(self):
        manifest = json.loads((ROOT / "lifts" / "manifest.json").read_text(encoding="utf-8"))["lifts"]["lift116"]["modules"]["msvcirt"]
        for key in ("sha256", "image_base", "image_size", "entry", "text", "entries", "exports", "code_exports", "data_exports", "export_fingerprint"):
            self.assertEqual(manifest[key], self.facts[key], key)

    @unittest.skipUnless(DLL is not None, "DATA UNAVAILABLE: MSVCIRT.DLL is not installed")
    def test_the_dll_is_the_supported_one_and_fully_covered(self):
        raw = DLL.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), CONFIG["sha256"])
        mem, sections, info = parse_dll(raw)
        self.assertEqual((info["base"], info["size"], info["entry"]), (0x780A0000, 0x12000, 0x1000))
        self.assertEqual(info["tls"], 0, "the DLL has no TLS directory")
        listed, nfuncs = exports(mem, info["export"])
        self.assertEqual((len(listed), nfuncs), (445, 445))
        text_lo, text_size = sections[".text"]
        directory_lo, directory_size = info["export"]
        forwarded = [e for e in listed if directory_lo <= e[2] < directory_lo + directory_size]
        self.assertEqual(forwarded, [], "no export is a forwarder")
        fingerprint = hashlib.sha256("\n".join(f"{o} {n} {r:08X}" for o, n, r in listed).encode()).hexdigest()
        self.assertEqual(fingerprint, CONFIG["export_fingerprint"])
        have = {va - info["base"] for va, _ in self.entries}
        code = [e for e in listed if text_lo <= e[2] < text_lo + text_size]
        self.assertEqual(len(code), 390)
        self.assertEqual(sorted(e[2] for e in code if e[2] not in have), [], "every code export is a lifted entry")
        self.assertIn(info["entry"], have, "DllMain is lifted")
        data_lo = [sections[n] for n in (".rdata", ".data")]
        pointed = {u32(mem, rva) - info["base"] for rva in relocations(mem, info["reloc"]) if any(lo <= rva < lo + sz for lo, sz in data_lo)}
        pointed = {p for p in pointed if text_lo <= p < text_lo + text_size}
        self.assertEqual(sorted(pointed - have), [], "every code address held in the DLL's data (vtable entries, callbacks) is a lifted entry")
        self.assertGreater(len(pointed), 100)
        immediates = {u32(mem, rva) - info["base"] for rva in relocations(mem, info["reloc"]) if text_lo <= rva < text_lo + text_size}
        immediates = {p for p in immediates if text_lo <= p < text_lo + text_size}
        self.assertEqual(sorted(immediates - have), [], "every code address the DLL's code takes as an immediate (the _onexit handlers) is a lifted entry")
        self.assertIn(0x780A1ADC - info["base"], have, "the exit handler pushed at 0x780A165E is lifted")


if __name__ == "__main__":
    unittest.main()
