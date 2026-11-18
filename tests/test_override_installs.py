"""Every address the host installs an override at by literal must be a listed stable entry.

    py -3 -m unittest tests.test_override_installs

A missing one makes guest_overrides_check refuse to run at start-up (a required refusal), so this fails first.
"""
import re
import unittest
from pathlib import Path

HOST = Path(__file__).resolve().parent.parent / "src" / "host"
LISTED = {int(m, 16) for m in re.findall(r"GUEST_OVERRIDE\(([0-9A-Fa-f]{8}),", (HOST / "guest_override_list.h").read_text())}


def host_sources():
    return [p for p in HOST.rglob("*.[ch]") if "profiles" not in p.parts]


class OverrideInstalls(unittest.TestCase):
    def test_literal_installs_are_listed(self):
        defines = {}
        for p in host_sources():
            for name, value in re.findall(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)u?\b", p.read_text(errors="ignore")):
                defines.setdefault(name, int(value, 16))
        missing = []
        for p in host_sources():
            for arg in re.findall(r"guest_override_install\(\s*(0x[0-9A-Fa-f]+|[A-Z][A-Z0-9_]+)\s*,", p.read_text(errors="ignore")):
                va = int(arg, 16) if arg.startswith("0x") else defines.get(arg)
                if va is not None and va not in LISTED:
                    missing.append(f"{p.name}: {arg} = {va:#010x}")
        self.assertEqual(missing, [])

    def test_the_bump_primitive_is_listed_required(self):
        text = (HOST / "guest_override_list.h").read_text()
        self.assertIn("GUEST_OVERRIDE(0057DB50, REQUIRED)", text)
        for lift in ("lift_es117", "lift_ptbr117"):
            listed = (HOST / "profiles" / lift / "guest_override_list.h").read_text()
            self.assertIn("bump primitive, perspective", listed)


if __name__ == "__main__":
    unittest.main()
