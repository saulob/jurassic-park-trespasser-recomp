#!/usr/bin/env python3
"""Source rules of the Linux host, headless: py -3 -m unittest discover -s tests -p "test_linux_native_rules.py" -v

* no native x86 execution: no inline assembly, no executable mapping, no dynamic loading of guest code, in the Linux host,
  the 64-bit guest memory layer and the lifted C;
* no guest pointer squeezed into a host pointer or the reverse: the casts that treat a 32-bit guest address as a host
  address (or truncate a host pointer to 32 bits) are not used in the code the Linux executable is built from.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST_DIRS = ("src/linux", "src/host64")
HOST_FILES = ("src/common/jpt_vpath.c", "src/common/jpt_sync.c", "src/common/jpt_reg_store.c", "src/common/jpt_fs_posix.c")
EXEC_PATTERNS = [r"__asm__", r"\basm\s*(volatile)?\s*\(", r"__asm\b", r"PROT_EXEC", r"PAGE_EXECUTE", r"MAP_JIT",
                 r"FILE_MAP_EXECUTE", r"\bdlopen\s*\(", r"\bLoadLibrary[AW]?\s*\("]
GUEST_AS_HOST = [r"\(\s*uint32_t\s*\)\s*\(\s*uintptr_t\s*\)",
                 r"\(\s*(void|char|uint8_t|uint16_t|uint32_t|const char|int)\s*\*\s*\)\s*\(\s*uintptr_t\s*\)",
                 r"\(\s*uintptr_t\s*\)\s*\(\s*uint32_t\s*\)"]
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//.*")
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')


def sources():
    out = []
    for d in HOST_DIRS:
        out += sorted(p for p in (ROOT / d).glob("*") if p.suffix in (".c", ".h", ".cpp"))
    return out + [ROOT / f for f in HOST_FILES if (ROOT / f).is_file()]


def code_lines(path):
    text = BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), path.read_text(encoding="utf-8", errors="replace"))
    for number, line in enumerate(text.splitlines(), 1):
        yield number, STRING.sub('""', LINE_COMMENT.sub("", line))


def offending(patterns, skip=()):
    problems = []
    for path in sources():
        if path.name in skip:
            continue
        for number, line in code_lines(path):
            if any(re.search(pattern, line) for pattern in patterns):
                problems.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")
    return problems


class NativeRules(unittest.TestCase):
    def test_no_native_x86_execution(self):
        self.assertEqual(offending(EXEC_PATTERNS), [])

    def test_generated_lift_has_no_assembly(self):
        gen = ROOT / "src" / "recomp" / "gen"
        if not gen.is_dir():
            self.skipTest("DATA UNAVAILABLE: the lift is not unpacked")
        for path in sorted(gen.glob("recomp_*.c")):
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertNotRegex(text, r"__asm|\basm\s*\(|PROT_EXEC|VirtualProtect", path.name)

    def test_memory_layer_never_makes_memory_executable(self):
        posix = (ROOT / "src/host64/host_os_posix.c").read_text(encoding="utf-8")
        self.assertIn("PROT_NONE", posix)
        self.assertNotIn("PROT_EXEC", posix)
        win = (ROOT / "src/host64/host_os_win.c").read_text(encoding="utf-8")
        self.assertNotIn("EXECUTE", win)

    def test_no_guest_address_treated_as_a_host_pointer(self):
        # host_os_*.c place the reservation in the host address space: their pointers are host addresses by design
        self.assertEqual(offending(GUEST_AS_HOST, skip=("host_os_posix.c", "host_os_win.c")), [])

    def test_guest_layouts_do_not_use_host_width_types(self):
        body = BLOCK_COMMENT.sub("", (ROOT / "src/linux/linux_abi.h").read_text(encoding="utf-8"))
        self.assertNotRegex(body, r"\b(long|size_t|uintptr_t|intptr_t)\b|void\s*\*")


if __name__ == "__main__":
    unittest.main()
